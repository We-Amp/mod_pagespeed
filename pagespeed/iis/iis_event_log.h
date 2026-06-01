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

#ifndef PAGESPEED_IIS_IIS_EVENT_LOG_H_
#define PAGESPEED_IIS_IIS_EVENT_LOG_H_

namespace net_instaweb {

// Log levels matching Windows Event types
enum class EventLogLevel {
  kInfo,
  kWarning,
  kError
};

// Initialize the event log source (call once at module load)
void InitEventLog();

// Shutdown the event log (call at module unload)
void ShutdownEventLog();

// Log a message to Windows Event Viewer
void LogEvent(EventLogLevel level, const char* message);

// For early initialization before Event Viewer is available, use file-based logging
void EarlyDebugLog(const char* message);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_EVENT_LOG_H_
