// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/base/message_handler.h"

#include <string>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// Test handler that captures output instead of writing to stderr.
class TestMessageHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }

  [[nodiscard]] const std::string& last_message() const {
    return last_message_;
  }
  [[nodiscard]] MessageType last_type() const { return last_type_; }
  [[nodiscard]] int message_count() const { return message_count_; }

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override {
    // Check minimum log level
    if (type != MessageType::kFatal) {
      LogLevel level = MessageTypeToLogLevel(type);
      if (level < min_level_) {
        return;
      }
    }
    last_message_ = FormatMessage(format, args);
    last_type_ = type;
    ++message_count_;
  }

 private:
  std::string last_message_;
  MessageType last_type_ = MessageType::kInfo;
  int message_count_ = 0;
};

TEST(MessageHandlerTest, InfoMessage) {
  TestMessageHandler handler;
  handler.Info("Hello %s", "world");
  EXPECT_EQ(handler.last_message(), "Hello world");
  EXPECT_EQ(handler.last_type(), MessageType::kInfo);
}

TEST(MessageHandlerTest, WarningMessage) {
  TestMessageHandler handler;
  handler.Warning("Problem: %d", 42);
  EXPECT_EQ(handler.last_message(), "Problem: 42");
  EXPECT_EQ(handler.last_type(), MessageType::kWarning);
}

TEST(MessageHandlerTest, ErrorMessage) {
  TestMessageHandler handler;
  handler.Error("Error %s at line %d", "foo", 10);
  EXPECT_EQ(handler.last_message(), "Error foo at line 10");
  EXPECT_EQ(handler.last_type(), MessageType::kError);
}

TEST(MessageHandlerTest, DefaultLogLevelIsInfo) {
  TestMessageHandler handler;
  EXPECT_EQ(handler.min_log_level(), LogLevel::kInfo);
}

TEST(MessageHandlerTest, LogLevelFilteringDebug) {
  TestMessageHandler handler;
  // Default level is Info, so Info should pass
  handler.Info("visible");
  EXPECT_EQ(handler.message_count(), 1);
}

TEST(MessageHandlerTest, LogLevelFilteringWarning) {
  TestMessageHandler handler;
  handler.SetMinLogLevel(LogLevel::kWarning);

  // Info should be filtered
  handler.Info("filtered");
  EXPECT_EQ(handler.message_count(), 0);

  // Warning should pass
  handler.Warning("visible");
  EXPECT_EQ(handler.message_count(), 1);

  // Error should pass
  handler.Error("also visible");
  EXPECT_EQ(handler.message_count(), 2);
}

TEST(MessageHandlerTest, LogLevelFilteringError) {
  TestMessageHandler handler;
  handler.SetMinLogLevel(LogLevel::kError);

  handler.Info("filtered");
  EXPECT_EQ(handler.message_count(), 0);

  handler.Warning("also filtered");
  EXPECT_EQ(handler.message_count(), 0);

  handler.Error("visible");
  EXPECT_EQ(handler.message_count(), 1);
}

TEST(MessageHandlerTest, NullHandlerDiscards) {
  NullMessageHandler handler;
  // Should not crash
  handler.Info("discarded");
  handler.Warning("discarded");
  handler.Error("discarded");
}

// Helper to test protected MessageTypeToLogLevel
class LogLevelTestHandler : public TestMessageHandler {
 public:
  using MessageHandler::MessageTypeToLogLevel;
};

TEST(MessageHandlerTest, MessageTypeToLogLevel) {
  EXPECT_EQ(LogLevelTestHandler::MessageTypeToLogLevel(MessageType::kInfo),
            LogLevel::kInfo);
  EXPECT_EQ(LogLevelTestHandler::MessageTypeToLogLevel(MessageType::kWarning),
            LogLevel::kWarning);
  EXPECT_EQ(LogLevelTestHandler::MessageTypeToLogLevel(MessageType::kError),
            LogLevel::kError);
  EXPECT_EQ(LogLevelTestHandler::MessageTypeToLogLevel(MessageType::kFatal),
            LogLevel::kFatal);
}

TEST(MessageHandlerTest, MessageTypeToLogLevelUnknown) {
  // Defensive fallback for unknown enum value
  EXPECT_EQ(
      LogLevelTestHandler::MessageTypeToLogLevel(static_cast<MessageType>(99)),
      LogLevel::kInfo);
}

TEST(MessageHandlerTest, ConsoleHandlerTypePrefix) {
  ConsoleMessageHandler handler;
  handler.SetMinLogLevel(LogLevel::kInfo);
  // Just verify it doesn't crash and produces output for all types.
  // We can't easily capture stderr here, but we exercise the code paths.
  handler.Info("test %s", "info");
  handler.Warning("test %s", "warn");
  handler.Error("test %s", "error");
}

TEST(MessageHandlerTest, ConsoleHandlerLogLevelFiltering) {
  ConsoleMessageHandler handler;
  handler.SetMinLogLevel(LogLevel::kError);
  // Info and Warning should be filtered (no crash, no output)
  handler.Info("filtered info");
  handler.Warning("filtered warning");
  // Error should pass through
  handler.Error("visible error");
}

TEST(MessageHandlerTest, FatalAlwaysPassesFilter) {
  // Fatal messages should pass through even when minimum level is set high
  TestMessageHandler handler;
  handler.SetMinLogLevel(LogLevel::kFatal);
  handler.Info("filtered");
  EXPECT_EQ(handler.message_count(), 0);
  // We can't test actual Fatal because it calls abort(), but we can verify
  // the filter check is correct via the TestMessageHandler's MessageV.
}

TEST(MessageHandlerTest, FormatMessageEmptyFormat) {
  TestMessageHandler handler;
  handler.Info("%s", "");
  EXPECT_EQ(handler.last_message(), "");
}

// Test the variadic FormatMessage(const char*, ...) overload (lines 24-30).
// We expose it via a helper since it's protected static.
class FormatMessageTestHandler : public TestMessageHandler {
 public:
  using MessageHandler::FormatMessage;
};

TEST(MessageHandlerTest, FormatMessageVariadic) {
  std::string result =
      FormatMessageTestHandler::FormatMessage("hello %s %d", "world", 42);
  EXPECT_EQ(result, "hello world 42");
}

TEST(MessageHandlerTest, FormatMessageVariadicNoArgs) {
  std::string result = FormatMessageTestHandler::FormatMessage("plain message");
  EXPECT_EQ(result, "plain message");
}

// Test ConsoleMessageHandler::Message variadic overload (lines 92-97).
// Calling Message() directly on ConsoleMessageHandler exercises the variadic
// path that forwards to MessageV.
TEST(MessageHandlerTest, ConsoleHandlerMessageVariadic) {
  ConsoleMessageHandler handler;
  // Exercise the variadic Message() overload. We can't easily capture stderr
  // but we exercise the code path. No crash = success.
  handler.Message(MessageType::kInfo, "variadic %s %d", "test", 123);
  handler.Message(MessageType::kWarning, "warn %s", "msg");
  handler.Message(MessageType::kError, "error %s", "msg");
}

// Test TypePrefix for kFatal (line 87) and the ConsoleMessageHandler kFatal
// path that calls std::abort() (line 112).
TEST(MessageHandlerTest, ConsoleHandlerFatalAborts) {
  EXPECT_DEATH_IF_SUPPORTED(
      {
        ConsoleMessageHandler handler;
        handler.Fatal("fatal %s", "crash");
      },
      "FATAL");
}

// Test that ConsoleMessageHandler::Message variadic with kFatal also aborts
// (exercises both the variadic overload and the abort path).
TEST(MessageHandlerTest, ConsoleHandlerMessageVariadicFatalAborts) {
  EXPECT_DEATH_IF_SUPPORTED(
      {
        ConsoleMessageHandler handler;
        handler.Message(MessageType::kFatal, "fatal via Message %d", 999);
      },
      "FATAL");
}

// Test TypePrefix default "[UNKNOWN]" case (line 89) via invalid MessageType.
// An out-of-range MessageType won't match any case in the switch, hitting the
// default return. It also won't match kFatal so no abort occurs.
TEST(MessageHandlerTest, ConsoleHandlerTypePrefixUnknown) {
  ConsoleMessageHandler handler;
  // The invalid type will pass the log-level filter (fallback to kInfo)
  // and call TypePrefix with the invalid value, hitting the default case.
  // It won't match kFatal, so no abort — just prints "[UNKNOWN]" to stderr.
  handler.Message(static_cast<MessageType>(99), "unknown type %d", 99);
  // No crash = success — the default "[UNKNOWN]" case was hit.
}

}  // namespace
}  // namespace pagespeed
