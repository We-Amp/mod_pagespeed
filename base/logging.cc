/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "base/logging.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#include "fmt/core.h"
#include "spdlog/spdlog.h"

namespace pagespeed_logging {

// Atomic flag set by ShutDownLogging(). Once true, LogMessage::~LogMessage()
// writes to stderr instead of spdlog, avoiding crashes from the static
// destruction order fiasco (spdlog's global logger may be destroyed before
// our static ApacheProcessContext destructor runs).
static std::atomic<bool> g_logging_shutdown{false};

// Returns a process-immortal spdlog logger. The strong reference is
// captured once on first use and intentionally LEAKED, so the logger object
// outlives ALL static destruction — including spdlog's own registry singleton —
// and therefore stays valid for any worker thread that is still draining work
// and may LOG() during process shutdown. Access is lock-free after the first
// call (a function-local-static acquire load), unlike the spdlog::default_logger()
// free function, which locks spdlog's registry mutex on every call, and unlike
// spdlog::info()/warn()/... which re-fetch the registry's raw default logger
// (the object torn down first in the static-destruction-order fiasco).
static spdlog::logger* HeldLogger() {
  // The strong reference is captured once and never freed, so the logger object
  // (and its sinks) stay immortal for the process lifetime. The heap shared_ptr
  // is kept REACHABLE through this static pointer, so LeakSanitizer treats it as
  // a live root — intentional immortality, not a reported leak. (Returning
  // ->get() into the static instead would orphan the shared_ptr and LSan flags
  // it: a 16-byte "direct leak".)
  static std::shared_ptr<spdlog::logger>* const held =
      new std::shared_ptr<spdlog::logger>(spdlog::default_logger());
  return held->get();
}

namespace {

std::mutex g_sinks_mutex;
std::vector<LogSink*>* g_sinks = nullptr;

// Allocates on first use. Only AddLogSink may call this: the read and remove
// paths must not resurrect the container, or a LOG() arriving after the last
// RemoveLogSink would re-leak it past DLL unload.
std::vector<LogSink*>* GetOrCreateSinks() {
  if (g_sinks == nullptr) {
    g_sinks = new std::vector<LogSink*>();
  }
  return g_sinks;
}

}  // namespace

void AddLogSink(LogSink* sink) {
  std::lock_guard<std::mutex> lock(g_sinks_mutex);
  GetOrCreateSinks()->push_back(sink);
}

void RemoveLogSink(LogSink* sink) {
  std::lock_guard<std::mutex> lock(g_sinks_mutex);
  if (g_sinks == nullptr) {
    return;
  }
  for (auto it = g_sinks->begin(); it != g_sinks->end(); ++it) {
    if (*it == sink) {
      g_sinks->erase(it);
      break;
    }
  }
  // Free the container once the last sink unregisters. erase() alone keeps the
  // vector's heap buffer alive, and that buffer is owned by the module that
  // registered the sink -- for a dynamically unloaded module (the IIS DLL) it
  // outlives FreeLibrary and trips AppVerifier's Leak provider (stop 0x900).
  if (g_sinks->empty()) {
    delete g_sinks;
    g_sinks = nullptr;
  }
}

void SendToSinks(int severity, const char* full_filename,
                 const char* base_filename, int line, const char* message,
                 size_t message_len) {
  std::lock_guard<std::mutex> lock(g_sinks_mutex);
  if (g_sinks == nullptr) {
    return;
  }
  for (auto* sink : *g_sinks) {
    sink->send(severity, full_filename, base_filename, line, message,
               message_len);
  }
}

void ShutDownLogging() {
  // Route subsequent LOG()s to stderr. This guards the registered-sink path
  // (SendToSinks -> ApacheGLogSink -> ap_log_perror on the Apache pool), which
  // is NOT covered by the immortal spdlog logger above. The spdlog path is safe
  // regardless of this flag because HeldLogger() never dies.
  g_logging_shutdown.store(true, std::memory_order_release);
}

LogMessage::~LogMessage() {
  std::string msg = stream_.str();
  const char* base_filename = GetBasename(file_);

  if (g_logging_shutdown.load(std::memory_order_acquire)) {
    // Process is shutting down — spdlog may already be destroyed.
    // Write to stderr instead, which is always safe.
    static const char* const kSeverityNames[] = {"INFO", "WARN", "ERROR",
                                                  "FATAL"};
    int idx = (severity_ >= 0 && severity_ <= 3) ? severity_ : 0;
    fprintf(stderr, "[%s] [pagespeed] [%s:%d] %s\n", kSeverityNames[idx],
            base_filename, line_, msg.c_str());
    if (severity_ == logging::LOG_FATAL) {
      std::abort();
    }
    return;
  }

  // Emit through the immortal held logger via a raw pointer: lock-free, and
  // valid even if spdlog's registry singleton has already been destroyed during
  // shutdown.
  spdlog::logger* logger = HeldLogger();

  // Send to spdlog (via the held logger, never the registry free functions).
  constexpr char fmtstring[] = "[pagespeed] [{}:{}] {}";
  switch (severity_) {
    case logging::LOG_INFO:
      logger->info(fmt::runtime(fmtstring), base_filename, line_, msg);
      break;
    case logging::LOG_WARNING:
      logger->warn(fmt::runtime(fmtstring), base_filename, line_, msg);
      break;
    case logging::LOG_ERROR:
      logger->error(fmt::runtime(fmtstring), base_filename, line_, msg);
      break;
    case logging::LOG_FATAL:
      logger->critical(fmt::runtime(fmtstring), base_filename, line_, msg);
      // Also write to stderr for death tests (EXPECT_DEBUG_DEATH captures stderr)
      std::cerr << "[FATAL] [" << base_filename << ":" << line_ << "] " << msg
                << '\n';
      logger->dump_backtrace();
      break;
    default:
      logger->info(fmt::runtime(fmtstring), base_filename, line_, msg);
      break;
  }

  // Send to registered sinks
  SendToSinks(severity_, file_, base_filename, line_, msg.c_str(), msg.size());

  // Abort on fatal errors
  if (severity_ == logging::LOG_FATAL) {
    std::abort();
  }
}

}  // namespace pagespeed_logging

namespace net_instaweb {

PageSpeedLogSink::PageSpeedLogSink() { ::pagespeed_logging::AddLogSink(this); }

PageSpeedLogSink::~PageSpeedLogSink() {
  ::pagespeed_logging::RemoveLogSink(this);
}

void PageSpeedLogSink::send(int severity, const char* /*full_filename*/,
                            const char* base_filename, int line,
                            const char* message, size_t message_len) {
  if (severity < min_log_level_) {
    return;
  }
  // Default implementation just logs to spdlog (already done by LogMessage)
  // Subclasses can override to redirect to Apache error log, etc.
}

}  // namespace net_instaweb
