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

#include "pagespeed/iis/iis_event_log.h"

#include <cstdio>
#include <mutex>
#include <set>

#include "pagespeed/kernel/base/string_util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace net_instaweb {

namespace {

#ifdef _WIN32
// Event source name for Windows Event Viewer
const char kEventSourceName[] = "PageSpeed";

// Global event log handle (initialized by InitEventLog)
HANDLE g_event_log_handle = nullptr;
#endif

namespace {
// Test seam state (see the header).
EventLogSink g_event_log_sink = nullptr;
int g_event_log_entries = 0;
}  // namespace

// Path for early debug log file (before Event Viewer is available)
const char kEarlyDebugLogPath[] = "C:\\Windows\\Temp\\pagespeed_early.log";

}  // namespace

void InitEventLog() {
#ifdef _WIN32
  // Two writers reach this -- the message handler's policy and the daemon
  // startup check -- from different locks, so the registration takes its
  // own and happens once for the process.  A test sink replaces the event
  // log entirely: registering a real source under a test would put the
  // suite's messages in the machine's Application log.
  static std::mutex init_mutex;
  std::lock_guard<std::mutex> lock(init_mutex);
  if (g_event_log_sink != nullptr || g_event_log_handle != nullptr) {
    return;
  }
  g_event_log_handle = RegisterEventSourceA(nullptr, kEventSourceName);
  if (g_event_log_handle == nullptr) {
    // Can't use Event Viewer, fall back to early debug log
    EarlyDebugLog("Failed to register event source for PageSpeed");
  }
#endif
}

void ShutdownEventLog() {
#ifdef _WIN32
  if (g_event_log_handle != nullptr) {
    DeregisterEventSource(g_event_log_handle);
    g_event_log_handle = nullptr;
  }
#endif
}

void SetEventLogSinkForTesting(EventLogSink sink) {
  g_event_log_sink = sink;
  g_event_log_entries = 0;
}

// The message handler's event-log policy: what is written, and how often.
// All state is process-wide (the handler instances are per site, the event
// log is per machine -- a rebuild of a site's context must not reset the
// latches or the cap).
namespace {

std::mutex g_module_event_mutex;
std::set<GoogleString> g_module_event_seen;
bool g_notice_written[4] = {false, false, false, false};
int g_module_event_entries = 0;
int g_module_event_cap = kEventLogMaxEntries;
bool g_cap_notice_written = false;

EventLogLevel SeverityFor(MessageType type) {
  return type == kWarning ? EventLogLevel::kWarning : EventLogLevel::kError;
}

// One entry, through the one facility, counted against the cap.  Returns
// false when the cap leaves nothing to write with.
// True once the cap is reached, writing the final entry that says so once.
// Consulted BEFORE anything is retained: past the cap the distinct-text set
// must stop growing too, or a message that carries a URL costs one string
// per request for the life of the process.
bool CapReached(EventLogLevel level) {
  if (g_module_event_entries < g_module_event_cap) {
    return false;
  }
  if (!g_cap_notice_written) {
    g_cap_notice_written = true;
    InitEventLog();
    LogEvent(level, StrCat("The pagespeed module has reached its cap of ",
                           IntegerToString(g_module_event_cap),
                           " event-log entries for this process; further "
                           "entries are suppressed until the worker "
                           "process is recycled.")
                        .c_str());
  }
  return true;
}

// Writes one entry and counts it. Every caller has already asked
// CapReached, which is where the cap and its final entry live.
void WriteOneBounded(EventLogLevel level, const char* text) {
  ++g_module_event_entries;
  InitEventLog();
  LogEvent(level, text);
}

}  // namespace

const int kEventLogMaxEntries = 1000;

void SetEventLogMaxEntriesForTesting(int max_entries) {
  std::lock_guard<std::mutex> lock(g_module_event_mutex);
  g_module_event_cap = max_entries;
  g_module_event_entries = 0;
  g_module_event_seen.clear();
  g_notice_written[kInfo] = false;
  g_notice_written[kWarning] = false;
  g_notice_written[kError] = false;
  g_cap_notice_written = false;
}

void WriteModuleEvent(EventLogDirective directive, MessageType type,
                      const char* message) {
  std::lock_guard<std::mutex> lock(g_module_event_mutex);
  // INFO is per-request volume on this port; it never reaches the event
  // log, and suppressing it is not worth an entry either.
  if (type == kInfo) {
    return;
  }
  // Past the cap nothing is written and nothing is remembered.
  if (CapReached(SeverityFor(type))) {
    return;
  }
  // A FATAL always writes: it names a condition the operator has to see,
  // and it must not depend on a directive they may not have read. It is
  // bounded like everything else -- the same text twice in one process is
  // the same condition, and the cap holds.
  if (type == kFatal) {
    if (g_module_event_seen.insert(message).second) {
      WriteOneBounded(EventLogLevel::kError, message);
    }
    return;
  }
  if (directive == EventLogDirective::kNotReadYet) {
    // Before a site's configuration is parsed the directive's value is not
    // known: the factory's own start-up messages and the parse diagnostics
    // arrive here first. Writing them would defy an operator who turned
    // the directive off, and the "it is off" notice below would latch on a
    // site that has it ON -- so this window writes nothing and remembers
    // nothing. The messages remain in the module's message history.
    return;
  }
  if (directive == EventLogDirective::kOff) {
    // The directive is off: the message itself is not written, and ONE
    // entry per severity per worker process says so, names the directive,
    // and points at the place the messages can still be read.
    if (!g_notice_written[type]) {
      g_notice_written[type] = true;
      const char* severity = type == kWarning ? "warnings" : "errors";
      WriteOneBounded(
          EventLogLevel::kWarning,
          StrCat("Event-log writing is off in this worker process, so ",
                 severity,
                 " from the pagespeed module are not written to the "
                 "Windows event log. Turn it on with `pagespeed UseEventLog "
                 "on` in pagespeed.config; the messages can also be read at "
                 "the admin message history path (/pagespeed_message by "
                 "default) from the local machine. The setting is one per "
                 "worker process, so a site that sets nothing follows "
                 "whatever a site in the same process set.")
              .c_str());
    }
    return;
  }
  // At most one entry per distinct message text per process: a busy site
  // can produce the same warning on every request, and the event log is
  // an operator's channel, not a per-request log.
  if (!g_module_event_seen.insert(message).second) {
    return;
  }
  WriteOneBounded(SeverityFor(type), message);
}

int EventLogEntryCountForTesting() { return g_event_log_entries; }

int DistinctTextCountForTesting() {
  std::lock_guard<std::mutex> lock(g_module_event_mutex);
  return static_cast<int>(g_module_event_seen.size());
}

void LogEvent(EventLogLevel level, const char* message) {
  ++g_event_log_entries;
  if (g_event_log_sink != nullptr) {
    g_event_log_sink(level, message);
    return;
  }
#ifdef _WIN32
  if (g_event_log_handle == nullptr) {
    // Event log not initialized, fall back to early debug log
    EarlyDebugLog(message);
    return;
  }

  // Map EventLogLevel to Windows event type
  WORD event_type;
  switch (level) {
    case EventLogLevel::kInfo:
      event_type = EVENTLOG_INFORMATION_TYPE;
      break;
    case EventLogLevel::kWarning:
      event_type = EVENTLOG_WARNING_TYPE;
      break;
    case EventLogLevel::kError:
      event_type = EVENTLOG_ERROR_TYPE;
      break;
    default:
      event_type = EVENTLOG_INFORMATION_TYPE;
      break;
  }

  // Write to Windows Event Log
  const char* msg_ptr = message;
  ReportEventA(g_event_log_handle, event_type,
               0,         // Category
               1000,      // Event ID
               nullptr,   // User SID
               1,         // Number of strings
               0,         // Data size
               &msg_ptr,  // Strings
               nullptr);  // Data
#else
  // Non-Windows: write to stderr
  const char* level_str;
  switch (level) {
    case EventLogLevel::kInfo:
      level_str = "INFO";
      break;
    case EventLogLevel::kWarning:
      level_str = "WARNING";
      break;
    case EventLogLevel::kError:
      level_str = "ERROR";
      break;
    default:
      level_str = "INFO";
      break;
  }
  fprintf(stderr, "[PageSpeed %s] %s\n", level_str, message);
#endif
}

void EarlyDebugLog(const char* message) {
  // File-based logging for early initialization before Event Viewer is
  // available. This is useful for debugging DLL load issues.
  FILE* file = fopen(kEarlyDebugLogPath, "a");
  if (file != nullptr) {
#ifdef _WIN32
    // Get current time for timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(file, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s\n", st.wYear,
            st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, message);
#else
    fprintf(file, "%s\n", message);
#endif
    fclose(file);
  }

#ifdef _WIN32
  // Also output to debug output (visible in debugger or DebugView)
  OutputDebugStringA("PageSpeed: ");
  OutputDebugStringA(message);
  OutputDebugStringA("\n");
#endif
}

}  // namespace net_instaweb
