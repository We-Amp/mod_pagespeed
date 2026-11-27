// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_BASE_MESSAGE_HANDLER_H_
#define PAGESPEED_LIB_BASE_MESSAGE_HANDLER_H_

#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {

// Message severity levels
enum class MessageType : std::uint8_t { kInfo, kWarning, kError, kFatal };

// Log level for filtering (separate from MessageType to allow Debug level)
enum class LogLevel : std::uint8_t {
  kDebug = 0,
  kInfo,
  kWarning,
  kError,
  kFatal
};

// Abstract interface for message/logging output.
// Allows different backends (console, file, null) without coupling.
class MessageHandler {
 public:
  virtual ~MessageHandler() = default;

  // Set minimum log level for filtering
  void SetMinLogLevel(LogLevel level) { min_level_ = level; }
  LogLevel min_log_level() const { return min_level_; }

  // Log a message with the given type
  virtual void Message(MessageType type, const char* format, ...) = 0;

  // Convenience methods
  void Info(const char* format, ...);
  void Warning(const char* format, ...);
  void Error(const char* format, ...);
  void Fatal(const char* format, ...);

 protected:
  LogLevel min_level_ = LogLevel::kInfo;

  // Convert MessageType to LogLevel for comparison
  static LogLevel MessageTypeToLogLevel(MessageType type);

  // Helper for formatting messages
  static std::string FormatMessage(const char* format, va_list args);
  static std::string FormatMessage(const char* format, ...);

  // Internal method for subclasses to implement
  virtual void MessageV(MessageType type, const char* format, va_list args) = 0;
};

// No-op message handler - discards all messages
class NullMessageHandler : public MessageHandler {
 public:
  void Message(MessageType /*type*/, const char* /*format*/, ...) override {}

 protected:
  void MessageV(MessageType /*type*/, const char* /*format*/,
                va_list /*args*/) override {}
};

// Console message handler - outputs to stderr
class ConsoleMessageHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override;

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override;

 private:
  static const char* TypePrefix(MessageType type);
};

}  // namespace pagespeed

// Logging macros for use with pagespeed::MessageHandler.
// These macros are used by the scanline status reporting system
// (PS_LOGGED_STATUS) and by image codec implementations.
//
// A null handler means "no logging" and is permitted wherever a
// MessageHandler* is an optional parameter (e.g. ReadImage). The macros
// must therefore tolerate null the way NullMessageHandler does -- by
// discarding the message -- instead of dereferencing it. Each macro stays
// a single expression (not a statement) because PS_LOGGED_STATUS embeds
// it in a comma expression.
#define PS_LOG_INFO(handler, ...) \
  ((handler) != nullptr ? (handler)->Info(__VA_ARGS__) : void(0))
#define PS_LOG_WARN(handler, ...) \
  ((handler) != nullptr ? (handler)->Warning(__VA_ARGS__) : void(0))
#define PS_LOG_ERROR(handler, ...) \
  ((handler) != nullptr ? (handler)->Error(__VA_ARGS__) : void(0))
#define PS_LOG_FATAL(handler, ...) \
  ((handler) != nullptr ? (handler)->Fatal(__VA_ARGS__) : void(0))

#ifndef NDEBUG
#define PS_LOG_DFATAL(handler, ...) PS_LOG_FATAL(handler, __VA_ARGS__)
#else
#define PS_LOG_DFATAL(handler, ...) PS_LOG_ERROR(handler, __VA_ARGS__)
#endif  // NDEBUG

// Debug-only logging macros. They expand to no-ops in release builds.
#ifndef NDEBUG
#define PS_DLOG_INFO(handler, ...) PS_LOG_INFO(handler, __VA_ARGS__)
#define PS_DLOG_WARN(handler, ...) PS_LOG_WARN(handler, __VA_ARGS__)
#define PS_DLOG_ERROR(handler, ...) PS_LOG_ERROR(handler, __VA_ARGS__)
#else
// No-op that references handler (suppressing -Wunused-parameter) as a function
// call (suppressing -Wunused-value when PS_DLOG_* appears in comma expressions
// like PS_LOGGED_STATUS).  __VA_ARGS__ deliberately not referenced: they may
// contain identifiers that only exist in debug builds (e.g. stats_ guarded
// by #ifndef NDEBUG).  Parameters used only as PS_DLOG format args should be
// marked [[maybe_unused]].
inline void PsDlogNoOp(const void*) noexcept {}
#define PS_DLOG_INFO(handler, ...) ::PsDlogNoOp(handler)
#define PS_DLOG_WARN(handler, ...) ::PsDlogNoOp(handler)
#define PS_DLOG_ERROR(handler, ...) ::PsDlogNoOp(handler)
#endif  // NDEBUG

#endif  // PAGESPEED_LIB_BASE_MESSAGE_HANDLER_H_
