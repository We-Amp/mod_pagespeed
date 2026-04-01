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

#include "pagespeed/envoy/envoy_message_handler.h"

#include <csignal>
#include <ctime>

#include "base/logging.h"
#include "net/instaweb/public/version.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/debug.h"
#include "pagespeed/kernel/base/posix_timer.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/time_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"

namespace net_instaweb {

EnvoyMessageHandler::EnvoyMessageHandler(Timer* timer, AbstractMutex* mutex)
    : SystemMessageHandler(timer, mutex), timer_(timer) {}

void EnvoyMessageHandler::SetStructuredLoggingEnabled(bool enabled) {
  structured_logging_enabled_ = enabled;
}

void EnvoyMessageHandler::MessageSImpl(MessageType type,
                                       const GoogleString& message) {
  if (structured_logging_enabled_) {
    LogStructured(type, message);
  } else {
    GoogleMessageHandler::MessageSImpl(type, message);
  }
  AddMessageToBuffer(type, message);
}

void EnvoyMessageHandler::FileMessageSImpl(MessageType type, const char* file,
                                           int line,
                                           const GoogleString& message) {
  if (structured_logging_enabled_) {
    LogStructured(type, file, line, message);
  } else {
    GoogleMessageHandler::FileMessageSImpl(type, file, line, message);
  }
  AddMessageToBuffer(type, file, line, message);
}

void EnvoyMessageHandler::LogStructured(MessageType type,
                                        const GoogleString& message) {
  GoogleString timestamp = FormatTimestampISO8601();
  const char* level = MessageTypeToJsonLevel(type);
  GoogleString escaped_message = EscapeJsonString(message);

  GoogleString json_output = StrCat(
      "{\"timestamp\":\"", timestamp, "\",\"level\":\"", level,
      "\",\"component\":\"pagespeed\",\"message\":\"", escaped_message, "\"}");

  // Use the appropriate log level based on message type
  switch (type) {
    case kInfo:
      LOG(INFO) << json_output;
      break;
    case kWarning:
      LOG(WARNING) << json_output;
      break;
    case kError:
      LOG(ERROR) << json_output;
      break;
    case kFatal:
      LOG(FATAL) << json_output;
      break;
  }
}

void EnvoyMessageHandler::LogStructured(MessageType type, const char* file,
                                        int line, const GoogleString& message) {
  GoogleString timestamp = FormatTimestampISO8601();
  const char* level = MessageTypeToJsonLevel(type);
  GoogleString escaped_message = EscapeJsonString(message);
  GoogleString escaped_file = EscapeJsonString(file);

  GoogleString json_output =
      StrCat("{\"timestamp\":\"", timestamp, "\",\"level\":\"", level,
             "\",\"component\":\"pagespeed\",\"file\":\"", escaped_file,
             "\",\"line\":", IntegerToString(line), ",\"message\":\"",
             escaped_message, "\"}");

  // Use the appropriate log level based on message type
  switch (type) {
    case kInfo:
      LOG(INFO) << json_output;
      break;
    case kWarning:
      LOG(WARNING) << json_output;
      break;
    case kError:
      LOG(ERROR) << json_output;
      break;
    case kFatal:
      LOG(FATAL) << json_output;
      break;
  }
}

GoogleString EnvoyMessageHandler::FormatTimestampISO8601() {
  int64 time_ms = timer_->NowMs();
  time_t time_sec = time_ms / 1000;
  struct tm time_buf;

#ifdef WIN32
  struct tm* time_info = nullptr;
  if (gmtime_s(&time_buf, &time_sec) == 0) {
    time_info = &time_buf;
  }
#else
  struct tm* time_info = gmtime_r(&time_sec, &time_buf);
#endif

  if (time_info == nullptr) {
    return "1970-01-01T00:00:00Z";
  }

  // Format as ISO 8601: YYYY-MM-DDTHH:MM:SSZ
  return absl::StrFormat("%04d-%02d-%02dT%02d:%02d:%02dZ",
                         1900 + time_buf.tm_year, 1 + time_buf.tm_mon,
                         time_buf.tm_mday, time_buf.tm_hour, time_buf.tm_min,
                         time_buf.tm_sec);
}

// static
GoogleString EnvoyMessageHandler::EscapeJsonString(StringPiece str) {
  GoogleString result;
  result.reserve(str.size() + 16);  // Reserve some extra space for escapes

  for (char c : str) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\b':
        result += "\\b";
        break;
      case '\f':
        result += "\\f";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // Control characters must be escaped as \uXXXX
          result += absl::StrFormat("\\u%04x", static_cast<unsigned int>(c));
        } else {
          result += c;
        }
        break;
    }
  }
  return result;
}

// static
const char* EnvoyMessageHandler::MessageTypeToJsonLevel(MessageType type) {
  switch (type) {
    case kInfo:
      return "info";
    case kWarning:
      return "warning";
    case kError:
      return "error";
    case kFatal:
      return "fatal";
    default:
      return "unknown";
  }
}

}  // namespace net_instaweb
