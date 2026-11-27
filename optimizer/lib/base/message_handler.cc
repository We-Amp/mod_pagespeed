// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/base/message_handler.h"

#include <cstdio>
#include <cstdlib>

namespace pagespeed {

std::string MessageHandler::FormatMessage(const char* format, va_list args) {
  // Get required size
  va_list args_copy;
  va_copy(args_copy, args);
  int size = std::vsnprintf(nullptr, 0, format, args_copy);
  va_end(args_copy);

  if (size <= 0) {
    return {};
  }

  std::string result(static_cast<size_t>(size), '\0');
  std::vsnprintf(result.data(), result.size() + 1, format, args);
  return result;
}

std::string MessageHandler::FormatMessage(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::string result = FormatMessage(format, args);
  va_end(args);
  return result;
}

void MessageHandler::Info(const char* format, ...) {
  va_list args;
  va_start(args, format);
  MessageV(MessageType::kInfo, format, args);
  va_end(args);
}

void MessageHandler::Warning(const char* format, ...) {
  va_list args;
  va_start(args, format);
  MessageV(MessageType::kWarning, format, args);
  va_end(args);
}

void MessageHandler::Error(const char* format, ...) {
  va_list args;
  va_start(args, format);
  MessageV(MessageType::kError, format, args);
  va_end(args);
}

void MessageHandler::Fatal(const char* format, ...) {
  va_list args;
  va_start(args, format);
  MessageV(MessageType::kFatal, format, args);
  va_end(args);
}

// MessageHandler helpers

LogLevel MessageHandler::MessageTypeToLogLevel(MessageType type) {
  switch (type) {
    case MessageType::kInfo:
      return LogLevel::kInfo;
    case MessageType::kWarning:
      return LogLevel::kWarning;
    case MessageType::kError:
      return LogLevel::kError;
    case MessageType::kFatal:
      return LogLevel::kFatal;
  }
  return LogLevel::kInfo;
}

// ConsoleMessageHandler implementation

const char* ConsoleMessageHandler::TypePrefix(MessageType type) {
  switch (type) {
    case MessageType::kInfo:
      return "[INFO]";
    case MessageType::kWarning:
      return "[WARNING]";
    case MessageType::kError:
      return "[ERROR]";
    case MessageType::kFatal:
      return "[FATAL]";
  }
  return "[UNKNOWN]";
}

void ConsoleMessageHandler::Message(MessageType type, const char* format, ...) {
  va_list args;
  va_start(args, format);
  MessageV(type, format, args);
  va_end(args);
}

void ConsoleMessageHandler::MessageV(MessageType type, const char* format,
                                     va_list args) {
  // Check minimum log level (Fatal always passes)
  if (type != MessageType::kFatal) {
    LogLevel level = MessageTypeToLogLevel(type);
    if (level < min_level_) {
      return;
    }
  }
  std::string message = FormatMessage(format, args);
  std::fprintf(stderr, "%s %s\n", TypePrefix(type), message.c_str());

  if (type == MessageType::kFatal) {
    std::abort();
  }
}

}  // namespace pagespeed
