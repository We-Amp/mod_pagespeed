// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/base/json_message_handler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "lib/base/string_util.h"

namespace pagespeed {

const char* JsonMessageHandler::LevelString(MessageType type) {
  switch (type) {
    case MessageType::kInfo:
      return "INFO";
    case MessageType::kWarning:
      return "WARNING";
    case MessageType::kError:
      return "ERROR";
    case MessageType::kFatal:
      return "FATAL";
  }
  return "UNKNOWN";
}

std::string JsonMessageHandler::JsonEscape(std::string_view input) {
  return net_instaweb::JsonEscapeMinimal(input);
}

std::string JsonMessageHandler::GetTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  struct tm tm_buf;
#ifdef _WIN32
  gmtime_s(&tm_buf, &time_t_now);
#else
  gmtime_r(&time_t_now, &tm_buf);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
  return buf;
}

void JsonMessageHandler::Message(MessageType type, const char* format, ...) {
  va_list args;
  va_start(args, format);
  MessageV(type, format, args);
  va_end(args);
}

void JsonMessageHandler::MessageV(MessageType type, const char* format,
                                  va_list args) {
  // Check minimum log level (Fatal always passes)
  if (type != MessageType::kFatal) {
    LogLevel level = MessageTypeToLogLevel(type);
    if (level < min_level_) {
      return;
    }
  }

  std::string message = FormatMessage(format, args);
  std::string escaped = JsonEscape(message);
  std::string timestamp = GetTimestamp();

  std::fprintf(stderr,
               "{\"timestamp\":\"%s\",\"level\":\"%s\",\"message\":\"%s\"}\n",
               timestamp.c_str(), LevelString(type), escaped.c_str());

  if (type == MessageType::kFatal) {
    std::abort();
  }
}

}  // namespace pagespeed
