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

#pragma once

#include <cstdarg>

#include "external/envoy/source/common/common/logger.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/system_message_handler.h"

namespace net_instaweb {

class AbstractMutex;
class Timer;

// Implementation of a message handler that uses envoy_log_error()
// logging to emit messages, with a fallback to GoogleMessageHandler.
// Supports optional structured JSON logging for observability integration.
class EnvoyMessageHandler : public SystemMessageHandler {
 public:
  explicit EnvoyMessageHandler(Timer* timer, AbstractMutex* mutex);

  // Enable or disable structured JSON logging output.
  // When enabled, messages are formatted as JSON with timestamp, level,
  // component, and message fields.
  void SetStructuredLoggingEnabled(bool enabled);

  // Returns whether structured logging is currently enabled.
  bool structured_logging_enabled() const {
    return structured_logging_enabled_;
  }

 protected:
  void MessageSImpl(MessageType type, const GoogleString& message) override;

  void FileMessageSImpl(MessageType type, const char* file, int line,
                        const GoogleString& message) override;

 private:
  // Outputs a structured JSON log entry.
  // Format: {"timestamp":"...","level":"...","component":"pagespeed","message":"..."}
  void LogStructured(MessageType type, const GoogleString& message);

  // Outputs a structured JSON log entry with file location.
  // Format: {"timestamp":"...","level":"...","component":"pagespeed","file":"...","line":N,"message":"..."}
  void LogStructured(MessageType type, const char* file, int line,
                     const GoogleString& message);

  // Formats the current time as an ISO 8601 timestamp (e.g., "2026-02-01T12:00:00Z").
  GoogleString FormatTimestampISO8601();

  // Escapes a string for safe inclusion in JSON output.
  static GoogleString EscapeJsonString(StringPiece str);

  // Converts MessageType to lowercase string for JSON output.
  static const char* MessageTypeToJsonLevel(MessageType type);

  Timer* timer_;  // Not owned, used for timestamps.
  bool structured_logging_enabled_{false};

  EnvoyMessageHandler(const EnvoyMessageHandler&) = delete;
  EnvoyMessageHandler& operator=(const EnvoyMessageHandler&) = delete;
};

}  // namespace net_instaweb
