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

#include "pagespeed/kernel/base/message_handler.h"

namespace net_instaweb {

// Log levels matching Windows Event types
enum class EventLogLevel { kInfo, kWarning, kError };

// Initialize the event log source (call once at module load)
void InitEventLog();

// Shutdown the event log (call at module unload)
void ShutdownEventLog();

// Log a message to Windows Event Viewer
void LogEvent(EventLogLevel level, const char* message);

// For early initialization before Event Viewer is available, use file-based logging
void EarlyDebugLog(const char* message);

// What the site's UseEventLog directive says -- including the state before
// a site's configuration has been parsed, which is neither on nor off.
enum class EventLogDirective { kNotReadYet, kOff, kOn };

// The message handler's event-log policy, on the module's ONE facility.
//
// WARNING and ERROR messages reach the event log only with the directive
// on; a FATAL always does (it names a condition the operator has to see --
// the one severity that must not depend on a directive they may not have
// read); INFO never does, because on this port it is per-request volume.
// With the directive off, the FIRST suppressed warning and the FIRST
// suppressed error each produce one entry saying event-log writing is off,
// how to turn it on, and where the messages can be read instead.
//
// BEFORE THE DIRECTIVE IS KNOWN nothing is written but a FATAL, and nothing
// is remembered: the window belongs to the factory's own start-up messages
// and the parse diagnostics, and a notice latched there would tell an
// operator who has the directive ON to turn it on.
//
// VOLUME: at most one entry per DISTINCT message text per process (the
// shared adapter's announcement latch is the model), and a hard cap of
// kEventLogMaxEntries per process; past the cap one final entry says so and
// nothing further is written OR retained, a FATAL included.
void WriteModuleEvent(EventLogDirective directive, MessageType type,
                      const char* message);

// The hard cap on event-log entries from one module process.  Named so the
// docs and the cap notice can state the same number.
extern const int kEventLogMaxEntries;

// Test seam: the cap WriteModuleEvent enforces (the production value is
// kEventLogMaxEntries; tests lower it to exercise the cap).
void SetEventLogMaxEntriesForTesting(int max_entries);

// Test seam: when set, LogEvent calls this INSTEAD of writing to the real
// event log, so unit tests can count and inspect entries without touching
// the machine's Event Viewer.  Pass nullptr to restore real logging.
using EventLogSink = void (*)(EventLogLevel level, const char* message);
void SetEventLogSinkForTesting(EventLogSink sink);

// Test seam: the number of entries LogEvent has written (real or sink)
// since the sink was last changed.  Lets tests assert "exactly one".
int EventLogEntryCountForTesting();

// How many distinct message texts the policy is holding on to. The policy
// must stop growing this once the cap is reached, or a message that carries
// a URL costs one retained string per request.
int DistinctTextCountForTesting();

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_EVENT_LOG_H_
