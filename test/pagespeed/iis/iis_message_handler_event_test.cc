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

// The wiring test: the port's message handler routes its messages to the
// event-log policy.  That route was dead in every 1.1 release -- the
// handler's writer returned before doing anything -- so this file states
// the behaviour that was missing.

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pagespeed/iis/iis_event_log.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/system/system_thread_system.h"

namespace net_instaweb {

namespace {

struct HandlerEntry {
  EventLogLevel level;
  std::string text;
};

class IisMessageHandlerEventTest : public testing::Test {
 protected:
  void SetUp() override {
    SetEventLogSinkForTesting(&Sink);
    entries.clear();
    SetEventLogMaxEntriesForTesting(kEventLogMaxEntries);
    thread_system_.reset(new SystemThreadSystem());
    handler_.reset(
        new IisMessageHandler(thread_system_->NewTimer(), new NullMutex));
    // Each case says what the directive is; the fixture starts from the
    // state before any site's configuration has been parsed.
    logToEventLog = false;
    logToEventLogSet = false;
    logToEventLogParsed = false;
  }

  void TearDown() override {
    SetEventLogSinkForTesting(nullptr);
    SetEventLogMaxEntriesForTesting(kEventLogMaxEntries);
    logToEventLog = false;
    logToEventLogSet = true;
    logToEventLogParsed = true;
  }

  static void Sink(EventLogLevel level, const char* message) {
    entries.push_back(HandlerEntry{level, message});
  }

  static std::vector<HandlerEntry> entries;
  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<IisMessageHandler> handler_;
};

std::vector<HandlerEntry> IisMessageHandlerEventTest::entries;

TEST_F(IisMessageHandlerEventTest, WarningWithTheDirectiveOnReachesTheLog) {
  // The behaviour that was missing: with UseEventLog on, a warning through
  // the port's message handler appears in the event log.
  logToEventLog = true;
  logToEventLogSet = true;
  logToEventLogParsed = true;
  handler_->Message(kWarning, "handler warning %d", 42);
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(EventLogLevel::kWarning, entries[0].level);
  EXPECT_NE(std::string::npos, entries[0].text.find("handler warning 42"));
}

TEST_F(IisMessageHandlerEventTest, ErrorWithTheDirectiveOnReachesTheLog) {
  logToEventLog = true;
  logToEventLogSet = true;
  logToEventLogParsed = true;
  handler_->Message(kError, "handler error");
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(EventLogLevel::kError, entries[0].level);
}

TEST_F(IisMessageHandlerEventTest, InfoNeverReachesTheLog) {
  logToEventLog = true;
  logToEventLogSet = true;
  logToEventLogParsed = true;
  handler_->Message(kInfo, "handler info");
  EXPECT_EQ(0u, entries.size());
}

TEST_F(IisMessageHandlerEventTest, FatalWithTheDirectiveOffStillReaches) {
  logToEventLog = false;
  logToEventLogSet = true;
  logToEventLogParsed = true;
  handler_->Message(kFatal, "handler fatal");
  // The fatal itself (not an off-notice).
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(EventLogLevel::kError, entries[0].level);
  EXPECT_NE(std::string::npos, entries[0].text.find("handler fatal"));
}

TEST_F(IisMessageHandlerEventTest,
       ASiteThatNeverMentionsTheDirectiveGetsTheNotice) {
  // The common case: no site in this process ever wrote UseEventLog, so the
  // directive has its default -- off -- as soon as a configuration has been
  // read. That is not the same as "not read yet": the notice belongs here.
  logToEventLogSet = false;
  logToEventLogParsed = true;
  handler_->Message(kWarning, "a warning on a site that never said");
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(std::string::npos,
            entries[0].text.find("a warning on a site that never said"));
  EXPECT_NE(std::string::npos,
            entries[0].text.find("Event-log writing is off"));
}

TEST_F(IisMessageHandlerEventTest, OffWritesTheNoticeNotTheMessage) {
  logToEventLog = false;
  logToEventLogSet = true;
  logToEventLogParsed = true;
  handler_->Message(kWarning, "off warning");
  ASSERT_EQ(1u, entries.size());
  // The notice, not the message itself.
  EXPECT_EQ(std::string::npos, entries[0].text.find("off warning"));
  EXPECT_NE(std::string::npos,
            entries[0].text.find("Event-log writing is off"));
  EXPECT_NE(std::string::npos,
            entries[0].text.find("pagespeed UseEventLog on"));
}

TEST_F(IisMessageHandlerEventTest,
       BeforeAnyConfigurationIsParsedNothingIsWritten) {
  // The handler is reached during the factory's own start-up, before a
  // site's configuration has said anything about the directive: a warning
  // there writes nothing at all -- not the message, and not the notice that
  // would tell an operator who has the directive ON to turn it on.
  handler_->Message(kWarning, "a start-up warning");
  handler_->Message(kError, "a start-up error");
  EXPECT_EQ(0u, entries.size());

  // A fatal in that same window is still written.
  handler_->Message(kFatal, "a start-up fatal");
  ASSERT_EQ(1u, entries.size());
  EXPECT_NE(std::string::npos, entries[0].text.find("a start-up fatal"));
}

}  // namespace

}  // namespace net_instaweb
