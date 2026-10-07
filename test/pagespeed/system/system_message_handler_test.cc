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

//
// Unit tests for SystemMessageHandler

#include "pagespeed/system/system_message_handler.h"

#include <memory>

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/query_params.h"
#include "pagespeed/kernel/sharedmem/inprocess_shared_mem.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/system/admin_site.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"

namespace net_instaweb {

class SystemMessageHandlerTest : public testing::Test {
 protected:
  SystemMessageHandlerTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(thread_system_->NewMutex(), MockTimer::kApr_5_2010_ms),
        system_message_handler_(&timer_, thread_system_->NewMutex()),
        writer_(&buffer_) {
    system_message_handler_.set_buffer(&writer_);
    system_message_handler_.SetPidString(1234);
  }

  void AddMessage(MessageType type, StringPiece msg) {
    system_message_handler_.AddMessageToBuffer(type, msg);
  }

  void AddMessage(MessageType type, const char* file, int line,
                  StringPiece msg) {
    system_message_handler_.AddMessageToBuffer(type, file, line, msg);
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  SystemMessageHandler system_message_handler_;
  GoogleString buffer_;
  StringWriter writer_;
};

// Tests that multi-line messages are annotated with the type-code for
// each line.
TEST_F(SystemMessageHandlerTest, WrapLongLinesError) {
  AddMessage(kError, "Now is the time\nfor all good men\nto come to the aid");
  EXPECT_STREQ(
      "E[Mon, 05 Apr 2010 18:51:26 GMT] [Error] [1234] Now is the time\n"
      "Efor all good men\n"
      "Eto come to the aid\n",
      buffer_);
}

TEST_F(SystemMessageHandlerTest, WrapLongLinesInfo) {
  AddMessage(kInfo, "Now is the time\nfor all good men\nto come to the aid");
  EXPECT_STREQ(
      "I[Mon, 05 Apr 2010 18:51:26 GMT] [Info] [1234] Now is the time\n"
      "Ifor all good men\n"
      "Ito come to the aid\n",
      buffer_);
}

TEST_F(SystemMessageHandlerTest, WrapLongLinesWarning) {
  AddMessage(kWarning, "Now is the time\nfor all good men\nto come to the aid");
  EXPECT_STREQ(
      "W[Mon, 05 Apr 2010 18:51:26 GMT] [Warning] [1234] Now is the time\n"
      "Wfor all good men\n"
      "Wto come to the aid\n",
      buffer_);
}

TEST_F(SystemMessageHandlerTest, AddsFileLineInfo) {
  AddMessage(kInfo, "test_file.cc", 4321, "Test message");
  EXPECT_STREQ(
      "I[Mon, 05 Apr 2010 18:51:26 GMT] [Info] [1234] "
      "[test_file.cc:4321] Test message\n",
      buffer_);
}

TEST_F(SystemMessageHandlerTest, AddsFileLineInfoWrapLongLines) {
  AddMessage(kInfo, "test_file.cc", 4321, "Test message with\nnew line.");
  EXPECT_STREQ(
      "I[Mon, 05 Apr 2010 18:51:26 GMT] [Info] [1234] "
      "[test_file.cc:4321] Test message with\n"
      "Inew line.\n",
      buffer_);
}

// The message-history endpoint over a real, wrapped SharedCircularBuffer:
// a `since` older than the retained window returns everything retained, and
// the partial line the wrap leaves at the start of the dump is never
// returned as a message.
TEST_F(SystemMessageHandlerTest, WrappedBufferSinceOlderThanRetained) {
  InProcessSharedMem shm(thread_system_.get());
  SharedCircularBuffer ring(&shm, 400, "/prefix", "wrap");
  ASSERT_TRUE(ring.InitSegment(true, &system_message_handler_));
  system_message_handler_.set_buffer(&ring);
  for (int i = 0; i < 20; ++i) {
    AddMessage(kInfo, StrCat("msg-", (i < 10 ? "0" : ""), IntegerToString(i),
                             "-abcdefghij"));
  }
  SystemRewriteOptions::Initialize();
  // Unused by the handler, but it takes a reference.
  auto options = std::make_unique<SystemRewriteOptions>(thread_system_.get());
  AdminSite site(&timer_, &system_message_handler_, nullptr);
  auto history = [&](const QueryParams& params) {
    GoogleString body;
    StringAsyncFetch fetch(
        RequestContext::NewTestRequestContext(thread_system_.get()), &body);
    site.MessageHistoryHandler(*options, AdminSite::kPageSpeedAdmin, params,
                               &fetch);
    return body;
  };
  QueryParams since;
  since.AddEscaped("since", "0");
  GoogleString body = history(since);
  EXPECT_EQ(history(QueryParams()), body);  // since=0 is "everything".
  EXPECT_NE(GoogleString::npos, body.find("msg-19-abcdefghij"));
  EXPECT_EQ(GoogleString::npos, body.find("msg-00-"));
  // Every returned message is a whole line: each "message" entry starts with
  // the line header and ends with the full message tail.
  int entries = CountSubstring(body, "\"message\":\"");
  EXPECT_GT(entries, 1);
  EXPECT_EQ(entries, CountSubstring(body, "\"message\":\"["));
  EXPECT_EQ(entries, CountSubstring(body, "-abcdefghij\"}"));
  system_message_handler_.set_buffer(&writer_);
  options.reset();
  SystemRewriteOptions::Terminate();
}

}  // namespace net_instaweb
