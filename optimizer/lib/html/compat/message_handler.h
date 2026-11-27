// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical pagespeed/kernel/base/message_handler.h and
// print_message_handler.h. The sync rewrites the vendored files' includes to
// land here. Only the surface the vendored HTML kernel consumes is provided —
// Message/FileMessage/Check, the Info/Warning/Error/FatalError convenience
// wrappers, and their va_list forms — nothing else (no Dump, no min-message
// filtering, no PS_LOG_* macros).
//
// Semantics mirror the canonical pagespeed/kernel/base/message_handler.cc:
//   * Message/FileMessage format the printf-style message exactly once and
//     route the formatted string to the backend.
//   * Check(false, ...) logs at kFatal but NEVER aborts — the canonical
//     MessageHandler is a logging sink, not glog CHECK; fatal-ness is purely
//     a severity label here.
//
// Unlike canonical (pure-virtual MessageSImpl/FileMessageSImpl pair), all
// wrappers format once via vsnprintf and route to a single pure-virtual
// EmitMessage(type, file, line, formatted); |file| is nullptr for plain
// Message/Check. This keeps one formatting path for every entry point.
//
// The unscoped net_instaweb::MessageType enum is canonical-compatible (the
// vendored kernel names kInfo/kWarning/... unqualified) and distinct from
// the 2.0-native pagespeed::MessageType enum class in lib/base — the two
// namespaces never meet in one TU today.

#ifndef PAGESPEED_LIB_HTML_COMPAT_MESSAGE_HANDLER_H_
#define PAGESPEED_LIB_HTML_COMPAT_MESSAGE_HANDLER_H_

#include <cstdarg>
#include <cstdio>
#include <string>

#include "lib/base/printf_format.h"

namespace net_instaweb {

// Canonical-shaped unscoped enum: no fixed underlying type, exactly as
// pagespeed/kernel/base/message_handler.h declares it — the vendored kernel
// is written against that declaration, so the size optimization does not
// apply here.
enum MessageType {  // NOLINT(performance-enum-size) — canonical-compatible
  kInfo,
  kWarning,
  kError,
  kFatal
};

namespace detail {

// Formats |msg|/|args| with vsnprintf into a fresh std::string. Falls back
// to the raw format string if vsnprintf reports an encoding error.
inline std::string HtmlCompatVFormat(const char* msg, va_list args) {
  va_list args_copy;
  va_copy(args_copy, args);
  const int size = std::vsnprintf(nullptr, 0, msg, args_copy);
  va_end(args_copy);
  if (size < 0) {
    return std::string(msg);
  }
  std::string result(size + 1, '\0');
  std::vsnprintf(result.data(), result.size(), msg, args);
  result.resize(size);
  return result;
}

}  // namespace detail

// Message sink interface matching the canonical 1.15 MessageHandler surface.
// This also completes the net_instaweb::MessageHandler that
// lib/base/writer.h forward-declares.
class MessageHandler {
 public:
  MessageHandler() = default;
  virtual ~MessageHandler() = default;

  // Log an info, warning, error or fatal error message.
  void Message(MessageType type, const char* msg, ...)
      INSTAWEB_PRINTF_FORMAT(3, 4) {
    va_list args;
    va_start(args, msg);
    MessageV(type, msg, args);
    va_end(args);
  }
  void MessageV(MessageType type, const char* msg, va_list args) {
    EmitMessage(type, nullptr, 0, detail::HtmlCompatVFormat(msg, args));
  }

  // Log a message with a filename and line number attached.
  void FileMessage(MessageType type, const char* file, int line,
                   const char* msg, ...) INSTAWEB_PRINTF_FORMAT(5, 6) {
    va_list args;
    va_start(args, msg);
    FileMessageV(type, file, line, msg, args);
    va_end(args);
  }
  void FileMessageV(MessageType type, const char* file, int line,
                    const char* msg, va_list args) {
    EmitMessage(type, file, line, detail::HtmlCompatVFormat(msg, args));
  }

  // Conditional error: !condition logs at kFatal. Canonical semantics — this
  // NEVER aborts; kFatal is a severity label, not a process exit.
  void Check(bool condition, const char* msg, ...)
      INSTAWEB_PRINTF_FORMAT(3, 4) {
    va_list args;
    va_start(args, msg);
    CheckV(condition, msg, args);
    va_end(args);
  }
  void CheckV(bool condition, const char* msg, va_list args) {
    if (!condition) {
      MessageV(kFatal, msg, args);
    }
  }

  // Convenience wrappers for FileMessage (canonical naming).
  void Info(const char* file, int line, const char* msg, ...)
      INSTAWEB_PRINTF_FORMAT(4, 5) {
    va_list args;
    va_start(args, msg);
    InfoV(file, line, msg, args);
    va_end(args);
  }
  void Warning(const char* file, int line, const char* msg, ...)
      INSTAWEB_PRINTF_FORMAT(4, 5) {
    va_list args;
    va_start(args, msg);
    WarningV(file, line, msg, args);
    va_end(args);
  }
  void Error(const char* file, int line, const char* msg, ...)
      INSTAWEB_PRINTF_FORMAT(4, 5) {
    va_list args;
    va_start(args, msg);
    ErrorV(file, line, msg, args);
    va_end(args);
  }
  void FatalError(const char* file, int line, const char* msg, ...)
      INSTAWEB_PRINTF_FORMAT(4, 5) {
    va_list args;
    va_start(args, msg);
    FatalErrorV(file, line, msg, args);
    va_end(args);
  }

  void InfoV(const char* file, int line, const char* msg, va_list args) {
    FileMessageV(kInfo, file, line, msg, args);
  }
  void WarningV(const char* file, int line, const char* msg, va_list args) {
    FileMessageV(kWarning, file, line, msg, args);
  }
  void ErrorV(const char* file, int line, const char* msg, va_list args) {
    FileMessageV(kError, file, line, msg, args);
  }
  void FatalErrorV(const char* file, int line, const char* msg, va_list args) {
    FileMessageV(kFatal, file, line, msg, args);
  }

 protected:
  // Single backend entry point: every public wrapper formats once and lands
  // here. |file| is nullptr for plain Message/Check messages.
  virtual void EmitMessage(MessageType type, const char* file, int line,
                           const std::string& formatted) = 0;

 private:
  MessageHandler(const MessageHandler&) = delete;
  MessageHandler& operator=(const MessageHandler&) = delete;
};

// Implementation of a message handler that discards everything.
class NullMessageHandler : public MessageHandler {
 protected:
  void EmitMessage(MessageType type, const char* file, int line,
                   const std::string& formatted) override {
    (void)type;
    (void)file;
    (void)line;
    (void)formatted;
  }
};

// Canonical print_message_handler.h stand-in: writes the pre-formatted
// message verbatim to stdout, exactly like canonical PrintMessageHandler
// (fputs(message.c_str(), stdout); fflush(stdout);) — no type/file/line
// annotation and no added newline, so callers that embed their own (e.g.
// DebugPrintQueue's EmitQueue output) are not double-terminated.
class PrintMessageHandler : public MessageHandler {
 protected:
  void EmitMessage(MessageType type, const char* file, int line,
                   const std::string& formatted) override {
    (void)type;
    (void)file;
    (void)line;
    std::fputs(formatted.c_str(), stdout);
    std::fflush(stdout);
  }
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_COMPAT_MESSAGE_HANDLER_H_
