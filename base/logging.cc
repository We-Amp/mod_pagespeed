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

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>

#include "fmt/core.h"
#include "spdlog/spdlog.h"

namespace pagespeed_logging {

namespace {

std::mutex g_sinks_mutex;
std::vector<LogSink*>* g_sinks = nullptr;

std::vector<LogSink*>* GetSinks() {
  if (g_sinks == nullptr) {
    g_sinks = new std::vector<LogSink*>();
  }
  return g_sinks;
}

}  // namespace

void AddLogSink(LogSink* sink) {
  std::lock_guard<std::mutex> lock(g_sinks_mutex);
  GetSinks()->push_back(sink);
}

void RemoveLogSink(LogSink* sink) {
  std::lock_guard<std::mutex> lock(g_sinks_mutex);
  auto* sinks = GetSinks();
  for (auto it = sinks->begin(); it != sinks->end(); ++it) {
    if (*it == sink) {
      sinks->erase(it);
      break;
    }
  }
}

void SendToSinks(int severity, const char* full_filename,
                 const char* base_filename, int line, const char* message,
                 size_t message_len) {
  std::lock_guard<std::mutex> lock(g_sinks_mutex);
  auto* sinks = GetSinks();
  for (auto* sink : *sinks) {
    sink->send(severity, full_filename, base_filename, line, message,
               message_len);
  }
}

LogMessage::~LogMessage() {
  std::string msg = stream_.str();
  const char* base_filename = GetBasename(file_);

  // Send to spdlog
  constexpr char fmtstring[] = "[pagespeed] [{}:{}] {}";
  switch (severity_) {
    case logging::LOG_INFO:
      spdlog::info(fmt::runtime(fmtstring), base_filename, line_, msg);
      break;
    case logging::LOG_WARNING:
      spdlog::warn(fmt::runtime(fmtstring), base_filename, line_, msg);
      break;
    case logging::LOG_ERROR:
      spdlog::error(fmt::runtime(fmtstring), base_filename, line_, msg);
      break;
    case logging::LOG_FATAL:
      spdlog::critical(fmt::runtime(fmtstring), base_filename, line_, msg);
      // Also write to stderr for death tests (EXPECT_DEBUG_DEATH captures stderr)
      std::cerr << "[FATAL] [" << base_filename << ":" << line_ << "] " << msg
                << std::endl;
      spdlog::dump_backtrace();
      break;
    default:
      spdlog::info(fmt::runtime(fmtstring), base_filename, line_, msg);
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
