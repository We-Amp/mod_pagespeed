// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/base/json_message_handler.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <string>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// Helper: Capture stderr output.
class StderrCapture {
 public:
  StderrCapture() {
    // Redirect stderr to a temp file
    tmpfile_ = std::tmpfile();
#ifdef _WIN32
    original_fd_ = _dup(_fileno(stderr));
    (void)_dup2(_fileno(tmpfile_), _fileno(stderr));
#else
    original_fd_ = dup(STDERR_FILENO);
    (void)dup2(fileno(tmpfile_), STDERR_FILENO);
#endif
  }

  ~StderrCapture() {
    if (original_fd_ >= 0) {
      Stop();
    }
  }

  std::string Stop() {
    fflush(stderr);
#ifdef _WIN32
    (void)_dup2(original_fd_, _fileno(stderr));
    _close(original_fd_);
#else
    (void)dup2(original_fd_, STDERR_FILENO);
    close(original_fd_);
#endif
    original_fd_ = -1;

    fseek(tmpfile_, 0, SEEK_END);
    long size = ftell(tmpfile_);
    fseek(tmpfile_, 0, SEEK_SET);

    std::string output(size, '\0');
    (void)fread(output.data(), 1, size, tmpfile_);
    fclose(tmpfile_);
    return output;
  }

 private:
  FILE* tmpfile_;
  int original_fd_;
};

TEST(JsonMessageHandlerTest, InfoMessageIsJson) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Info("test message");
  std::string output = capture.Stop();

  // Should be valid JSON-like structure
  EXPECT_NE(output.find(R"("level":"INFO")"), std::string::npos);
  EXPECT_NE(output.find(R"("message":"test message")"), std::string::npos);
  EXPECT_NE(output.find(R"("timestamp":")"), std::string::npos);
  // Should end with newline
  EXPECT_EQ(output.back(), '\n');
}

TEST(JsonMessageHandlerTest, WarningLevel) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Warning("something %s", "bad");
  std::string output = capture.Stop();

  EXPECT_NE(output.find(R"("level":"WARNING")"), std::string::npos);
  EXPECT_NE(output.find(R"("message":"something bad")"), std::string::npos);
}

TEST(JsonMessageHandlerTest, ErrorLevel) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Error("failure: %d", 500);
  std::string output = capture.Stop();

  EXPECT_NE(output.find(R"("level":"ERROR")"), std::string::npos);
  EXPECT_NE(output.find(R"("message":"failure: 500")"), std::string::npos);
}

TEST(JsonMessageHandlerTest, JsonEscapesSpecialChars) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Info("line1\nline2\ttab \"quoted\" back\\slash");
  std::string output = capture.Stop();

  // Newlines, tabs, quotes, backslashes should be escaped
  EXPECT_NE(output.find(R"(line1\nline2\ttab \"quoted\" back\\slash)"),
            std::string::npos)
      << "Output: " << output;
}

TEST(JsonMessageHandlerTest, LogLevelFiltering) {
  JsonMessageHandler handler;
  handler.SetMinLogLevel(LogLevel::kError);

  StderrCapture capture;
  handler.Info("filtered");
  handler.Warning("also filtered");
  handler.Error("visible");
  std::string output = capture.Stop();

  EXPECT_EQ(output.find("filtered"), std::string::npos);
  EXPECT_NE(output.find("visible"), std::string::npos);
}

TEST(JsonMessageHandlerTest, TimestampFormat) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Info("timestamp test");
  std::string output = capture.Stop();

  // Timestamp should match ISO 8601 pattern: YYYY-MM-DDTHH:MM:SSZ
  auto pos = output.find(R"("timestamp":")");
  ASSERT_NE(pos, std::string::npos);
  pos += 13;  // Skip "timestamp":"
  auto end = output.find('"', pos);
  ASSERT_NE(end, std::string::npos);
  std::string ts = output.substr(pos, end - pos);

  // Basic format validation: 20 chars like 2024-01-01T00:00:00Z
  EXPECT_EQ(ts.size(), 20u) << "Timestamp: " << ts;
  EXPECT_EQ(ts[4], '-');
  EXPECT_EQ(ts[7], '-');
  EXPECT_EQ(ts[10], 'T');
  EXPECT_EQ(ts[13], ':');
  EXPECT_EQ(ts[16], ':');
  EXPECT_EQ(ts[19], 'Z');
}

TEST(JsonMessageHandlerTest, JsonEscapesControlChars) {
  // Control characters < 0x20 (not \n, \r, \t) → \u00XX format
  // Test indirectly via Info() which calls JsonEscape internally
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Info("bell\x07here");
  std::string output = capture.Stop();

  // The bell character should be escaped as \u0007
  EXPECT_NE(output.find("\\u0007"), std::string::npos) << "Output: " << output;
}

TEST(JsonMessageHandlerTest, JsonEscapesFormFeed) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Info(
      "ff\x0c"
      "here");
  std::string output = capture.Stop();

  // RFC 8259 canonical escape: \f (not \u000c).
  EXPECT_NE(output.find("\\f"), std::string::npos) << "Output: " << output;
}

// Test \r escape in JsonEscape (lines 38-39 of json_message_handler.cc).
TEST(JsonMessageHandlerTest, JsonEscapesCarriageReturn) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Info("before\rafter");
  std::string output = capture.Stop();

  EXPECT_NE(output.find("before\\rafter"), std::string::npos)
      << "Output: " << output;
}

// Test JsonMessageHandler::Message variadic overload (lines 68-73).
// Calling Message() directly exercises the variadic path.
TEST(JsonMessageHandlerTest, MessageVariadicOverload) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Message(MessageType::kInfo, "variadic %s %d", "test", 42);
  std::string output = capture.Stop();

  EXPECT_NE(output.find(R"("level":"INFO")"), std::string::npos);
  EXPECT_NE(output.find(R"("message":"variadic test 42")"), std::string::npos);
}

TEST(JsonMessageHandlerTest, MessageVariadicWarning) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Message(MessageType::kWarning, "warn %d", 7);
  std::string output = capture.Stop();

  EXPECT_NE(output.find(R"("level":"WARNING")"), std::string::npos);
  EXPECT_NE(output.find(R"("message":"warn 7")"), std::string::npos);
}

// Test FATAL LevelString (line 18) and abort path (line 94).
TEST(JsonMessageHandlerTest, FatalAborts) {
  EXPECT_DEATH_IF_SUPPORTED(
      {
        JsonMessageHandler handler;
        handler.Fatal("fatal %s", "crash");
      },
      "FATAL");
}

// Test FATAL via Message variadic (exercises both lines 68-73 and 93-94).
TEST(JsonMessageHandlerTest, FatalViaMessageVariadicAborts) {
  EXPECT_DEATH_IF_SUPPORTED(
      {
        JsonMessageHandler handler;
        handler.Message(MessageType::kFatal, "fatal via Message %d", 999);
      },
      "FATAL");
}

// Test LevelString default "UNKNOWN" case (line 20) via invalid MessageType.
// An out-of-range value won't match any case, hitting the default return.
// It won't match kFatal so no abort occurs.
TEST(JsonMessageHandlerTest, LevelStringUnknown) {
  JsonMessageHandler handler;
  StderrCapture capture;
  handler.Message(static_cast<MessageType>(99), "unknown type");
  std::string output = capture.Stop();

  EXPECT_NE(output.find(R"("level":"UNKNOWN")"), std::string::npos)
      << "Output: " << output;
  EXPECT_NE(output.find(R"("message":"unknown type")"), std::string::npos)
      << "Output: " << output;
}

}  // namespace
}  // namespace pagespeed
