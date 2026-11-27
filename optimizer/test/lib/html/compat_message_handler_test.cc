// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Behavioral coverage for lib/html/compat/message_handler.h (#1130): the
// compat shim must reproduce the canonical 1.15 MessageHandler semantics the
// vendored HTML kernel relies on — single-format routing, Check(false) at
// kFatal WITHOUT aborting, and correct severity/file/line plumbing.

#include <cstdarg>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/writer.h"
#include "lib/html/compat/message_handler.h"

namespace net_instaweb {
namespace {

struct RecordedMessage {
  MessageType type;
  bool has_file;
  std::string file;
  int line;
  std::string formatted;
};

class RecordingMessageHandler : public MessageHandler {
 public:
  const std::vector<RecordedMessage>& messages() const { return messages_; }

 protected:
  void EmitMessage(MessageType type, const char* file, int line,
                   const std::string& formatted) override {
    messages_.push_back(
        {type, file != nullptr, file != nullptr ? file : "", line, formatted});
  }

 private:
  std::vector<RecordedMessage> messages_;
};

TEST(CompatMessageHandlerTest, CheckTrueEmitsNothing) {
  RecordingMessageHandler handler;
  handler.Check(true, "must not be emitted %d", 42);
  EXPECT_TRUE(handler.messages().empty());
}

TEST(CompatMessageHandlerTest, CheckFalseEmitsExactlyOneFatalAndDoesNotAbort) {
  RecordingMessageHandler handler;
  // Canonical semantics: Check(false, ...) logs at kFatal but never aborts.
  // If it aborted, this test would crash before the assertions below.
  handler.Check(false, "check failed %s=%d", "answer", 42);
  ASSERT_EQ(handler.messages().size(), 1U);
  const RecordedMessage& msg = handler.messages()[0];
  EXPECT_EQ(msg.type, kFatal);
  EXPECT_FALSE(msg.has_file);
  EXPECT_EQ(msg.formatted, "check failed answer=42");
}

TEST(CompatMessageHandlerTest, MessageFormatsAndRoutesWithNullFile) {
  RecordingMessageHandler handler;
  handler.Message(kWarning, "%s %d", "warn", 7);
  ASSERT_EQ(handler.messages().size(), 1U);
  const RecordedMessage& msg = handler.messages()[0];
  EXPECT_EQ(msg.type, kWarning);
  EXPECT_FALSE(msg.has_file);
  EXPECT_EQ(msg.formatted, "warn 7");
}

TEST(CompatMessageHandlerTest, FileMessageRoutesSeverityFileAndLine) {
  RecordingMessageHandler handler;
  handler.FileMessage(kError, "foo.html", 123, "bad %s", "tag");
  ASSERT_EQ(handler.messages().size(), 1U);
  const RecordedMessage& msg = handler.messages()[0];
  EXPECT_EQ(msg.type, kError);
  EXPECT_TRUE(msg.has_file);
  EXPECT_EQ(msg.file, "foo.html");
  EXPECT_EQ(msg.line, 123);
  EXPECT_EQ(msg.formatted, "bad tag");
}

TEST(CompatMessageHandlerTest, ConvenienceWrappersRouteCorrectSeverity) {
  RecordingMessageHandler handler;
  handler.Info("f.html", 1, "i");
  handler.Warning("f.html", 2, "w");
  handler.Error("f.html", 3, "e");
  handler.FatalError("f.html", 4, "f");
  ASSERT_EQ(handler.messages().size(), 4U);
  EXPECT_EQ(handler.messages()[0].type, kInfo);
  EXPECT_EQ(handler.messages()[1].type, kWarning);
  EXPECT_EQ(handler.messages()[2].type, kError);
  EXPECT_EQ(handler.messages()[3].type, kFatal);
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(handler.messages()[i].has_file);
    EXPECT_EQ(handler.messages()[i].file, "f.html");
    EXPECT_EQ(handler.messages()[i].line, static_cast<int>(i + 1));
  }
}

// va_list forwarding helpers so the *V forms can be exercised.
void CallMessageV(MessageHandler* handler, MessageType type, const char* fmt,
                  ...) {
  va_list args;
  va_start(args, fmt);
  handler->MessageV(type, fmt, args);
  va_end(args);
}

void CallFileMessageV(MessageHandler* handler, MessageType type,
                      const char* file, int line, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  handler->FileMessageV(type, file, line, fmt, args);
  va_end(args);
}

void CallInfoV(MessageHandler* handler, const char* file, int line,
               const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  handler->InfoV(file, line, fmt, args);
  va_end(args);
}

void CallWarningV(MessageHandler* handler, const char* file, int line,
                  const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  handler->WarningV(file, line, fmt, args);
  va_end(args);
}

void CallErrorV(MessageHandler* handler, const char* file, int line,
                const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  handler->ErrorV(file, line, fmt, args);
  va_end(args);
}

void CallFatalErrorV(MessageHandler* handler, const char* file, int line,
                     const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  handler->FatalErrorV(file, line, fmt, args);
  va_end(args);
}

void CallCheckV(MessageHandler* handler, bool condition, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  handler->CheckV(condition, fmt, args);
  va_end(args);
}

TEST(CompatMessageHandlerTest, VFormsRouteSeverityFileAndLine) {
  RecordingMessageHandler handler;
  CallMessageV(&handler, kInfo, "plain %d", 1);
  CallFileMessageV(&handler, kWarning, "v.html", 10, "file %d", 2);
  CallInfoV(&handler, "v.html", 11, "info %d", 3);
  CallWarningV(&handler, "v.html", 12, "warn %d", 4);
  CallErrorV(&handler, "v.html", 13, "err %d", 5);
  CallFatalErrorV(&handler, "v.html", 14, "fatal %d", 6);
  CallCheckV(&handler, false, "check %d", 7);

  ASSERT_EQ(handler.messages().size(), 7U);
  EXPECT_EQ(handler.messages()[0].type, kInfo);
  EXPECT_FALSE(handler.messages()[0].has_file);
  EXPECT_EQ(handler.messages()[0].formatted, "plain 1");
  EXPECT_EQ(handler.messages()[1].type, kWarning);
  EXPECT_EQ(handler.messages()[1].file, "v.html");
  EXPECT_EQ(handler.messages()[1].line, 10);
  EXPECT_EQ(handler.messages()[1].formatted, "file 2");
  EXPECT_EQ(handler.messages()[2].type, kInfo);
  EXPECT_EQ(handler.messages()[2].line, 11);
  EXPECT_EQ(handler.messages()[2].formatted, "info 3");
  EXPECT_EQ(handler.messages()[3].type, kWarning);
  EXPECT_EQ(handler.messages()[3].line, 12);
  EXPECT_EQ(handler.messages()[3].formatted, "warn 4");
  EXPECT_EQ(handler.messages()[4].type, kError);
  EXPECT_EQ(handler.messages()[4].line, 13);
  EXPECT_EQ(handler.messages()[4].formatted, "err 5");
  EXPECT_EQ(handler.messages()[5].type, kFatal);
  EXPECT_EQ(handler.messages()[5].formatted, "fatal 6");
  // CheckV(false) routes a kFatal with no file, like Check.
  EXPECT_EQ(handler.messages()[6].type, kFatal);
  EXPECT_FALSE(handler.messages()[6].has_file);
  EXPECT_EQ(handler.messages()[6].formatted, "check 7");
}

TEST(CompatMessageHandlerTest, NullMessageHandlerDiscardsEverything) {
  NullMessageHandler handler;
  handler.Message(kFatal, "discarded %d", 1);
  handler.FileMessage(kInfo, "f.html", 1, "discarded");
  handler.Check(false, "discarded");
  handler.Info("f.html", 1, "discarded");
  // Nothing to observe — the contract is that this neither crashes nor
  // stores anything. Reaching this point is the assertion.
  SUCCEED();
}

TEST(CompatMessageHandlerTest, PrintMessageHandlerSmoke) {
  PrintMessageHandler handler;
  handler.Message(kInfo, "smoke %d", 1);
  handler.FileMessage(kWarning, "f.html", 2, "smoke");
  handler.Check(false, "smoke fatal (does not abort)");
  SUCCEED();
}

// Compile-level proof that the compat net_instaweb::MessageHandler is the
// same type lib/base/writer.h forward-declares: Writer::Write/Flush take
// MessageHandler*, and this subclass's overrides only compile if the two
// declarations name one type.
class CompatWriterProof : public Writer {
 public:
  bool Write(std::string_view str, MessageHandler* handler) override {
    handler->Message(kInfo, "write: %s", std::string(str).c_str());
    return true;
  }
  bool Flush(MessageHandler* handler) override {
    handler->Info("compat_writer_proof", 0, "flushed");
    return true;
  }
};

TEST(CompatMessageHandlerTest, WriterSignaturesAcceptCompatHandler) {
  RecordingMessageHandler handler;
  CompatWriterProof writer;
  EXPECT_TRUE(writer.Write("payload", &handler));
  EXPECT_TRUE(writer.Flush(&handler));
  ASSERT_EQ(handler.messages().size(), 2U);
  EXPECT_EQ(handler.messages()[0].formatted, "write: payload");
}

}  // namespace
}  // namespace net_instaweb
