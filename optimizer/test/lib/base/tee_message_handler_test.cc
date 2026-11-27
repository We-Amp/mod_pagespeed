// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// TeeMessageHandler unit tests.

#include "lib/base/tee_message_handler.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"

namespace pagespeed {
namespace {

// Captures messages for verification.
class CapturingHandler : public MessageHandler {
 public:
  struct Entry {
    MessageType type;
    std::string message;
  };

  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }

  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override {
    entries_.push_back({type, FormatMessage(format, args)});
  }

 private:
  std::vector<Entry> entries_;
};

TEST(TeeMessageHandlerTest, DelegatReceivesMessages) {
  CapturingHandler delegate;
  std::vector<std::string> callback_msgs;

  TeeMessageHandler tee(&delegate, [&](MessageType, const std::string& msg) {
    callback_msgs.push_back(msg);
  });

  tee.Info("hello %s", "world");
  tee.Warning("warn %d", 42);
  tee.Error("err");

  ASSERT_EQ(delegate.entries().size(), 3u);
  EXPECT_EQ(delegate.entries()[0].type, MessageType::kInfo);
  EXPECT_EQ(delegate.entries()[0].message, "hello world");
  EXPECT_EQ(delegate.entries()[1].type, MessageType::kWarning);
  EXPECT_EQ(delegate.entries()[1].message, "warn 42");
  EXPECT_EQ(delegate.entries()[2].type, MessageType::kError);
  EXPECT_EQ(delegate.entries()[2].message, "err");
}

TEST(TeeMessageHandlerTest, CallbackReceivesMessages) {
  CapturingHandler delegate;

  struct CallbackEntry {
    MessageType type;
    std::string message;
  };
  std::vector<CallbackEntry> cb_entries;

  TeeMessageHandler tee(&delegate, [&](MessageType type, std::string msg) {
    cb_entries.push_back({type, std::move(msg)});
  });

  tee.Info("test %d", 1);
  tee.Error("fail");

  ASSERT_EQ(cb_entries.size(), 2u);
  EXPECT_EQ(cb_entries[0].type, MessageType::kInfo);
  EXPECT_EQ(cb_entries[0].message, "test 1");
  EXPECT_EQ(cb_entries[1].type, MessageType::kError);
  EXPECT_EQ(cb_entries[1].message, "fail");
}

TEST(TeeMessageHandlerTest, BothCalledForEachMessage) {
  CapturingHandler delegate;
  int callback_count = 0;

  TeeMessageHandler tee(
      &delegate, [&](MessageType, const std::string&) { ++callback_count; });

  tee.Info("a");
  tee.Warning("b");
  tee.Error("c");

  EXPECT_EQ(delegate.entries().size(), 3u);
  EXPECT_EQ(callback_count, 3);
}

// Test TeeMessageHandler::Message variadic overload (lines 15-20 of
// tee_message_handler.cc). Calling Message() directly exercises the variadic
// path that forwards to MessageV.
TEST(TeeMessageHandlerTest, MessageVariadicOverload) {
  CapturingHandler delegate;
  std::vector<std::string> callback_msgs;

  TeeMessageHandler tee(&delegate, [&](MessageType, const std::string& msg) {
    callback_msgs.push_back(msg);
  });

  // Call Message() directly (variadic overload), not Info()/Warning()/Error().
  tee.Message(MessageType::kInfo, "variadic %s %d", "test", 42);
  tee.Message(MessageType::kWarning, "warn %d", 7);
  tee.Message(MessageType::kError, "err %s", "msg");

  ASSERT_EQ(delegate.entries().size(), 3u);
  EXPECT_EQ(delegate.entries()[0].type, MessageType::kInfo);
  EXPECT_EQ(delegate.entries()[0].message, "variadic test 42");
  EXPECT_EQ(delegate.entries()[1].type, MessageType::kWarning);
  EXPECT_EQ(delegate.entries()[1].message, "warn 7");
  EXPECT_EQ(delegate.entries()[2].type, MessageType::kError);
  EXPECT_EQ(delegate.entries()[2].message, "err msg");

  ASSERT_EQ(callback_msgs.size(), 3u);
  EXPECT_EQ(callback_msgs[0], "variadic test 42");
  EXPECT_EQ(callback_msgs[1], "warn 7");
  EXPECT_EQ(callback_msgs[2], "err msg");
}

}  // namespace
}  // namespace pagespeed
