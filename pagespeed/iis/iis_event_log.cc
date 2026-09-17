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

// Path for early debug log file (before Event Viewer is available)
const char kEarlyDebugLogPath[] = "C:\\Windows\\Temp\\pagespeed_early.log";

}  // namespace

void InitEventLog() {
#ifdef _WIN32
  if (g_event_log_handle != nullptr) {
    return;  // Already initialized
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

void LogEvent(EventLogLevel level, const char* message) {
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
  ReportEventA(g_event_log_handle,
               event_type,
               0,           // Category
               1000,        // Event ID
               nullptr,     // User SID
               1,           // Number of strings
               0,           // Data size
               &msg_ptr,    // Strings
               nullptr);    // Data
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
    fprintf(file, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            message);
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
