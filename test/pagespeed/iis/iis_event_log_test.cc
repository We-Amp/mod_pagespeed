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

// Unit tests for the message handler's event-log policy: what reaches the
// Windows event log, how often, and what the operator is told when the
// directive is off.  Driven through the facility's sink seam, so nothing
// here touches the machine's Event Viewer.

#include "pagespeed/iis/iis_event_log.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

struct Entry {
  EventLogLevel level;
  std::string text;
};

class IisEventLogTest : public testing::Test {
 protected:
  void SetUp() override {
    SetEventLogSinkForTesting(&Sink);
    entries.clear();
    SetEventLogMaxEntriesForTesting(kEventLogMaxEntries);
  }

  void TearDown() override {
    SetEventLogSinkForTesting(nullptr);
    SetEventLogMaxEntriesForTesting(kEventLogMaxEntries);
  }

  static void Sink(EventLogLevel level, const char* message) {
    entries.push_back(Entry{level, message});
  }

  int CountText(const std::string& needle) {
    int n = 0;
    for (const Entry& e : entries) {
      if (e.text.find(needle) != std::string::npos) {
        ++n;
      }
    }
    return n;
  }

  static std::vector<Entry> entries;
};

std::vector<Entry> IisEventLogTest::entries;

TEST_F(IisEventLogTest, OnWritesWarningAndErrorNotInfo) {
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "a warning text");
  WriteModuleEvent(EventLogDirective::kOn, kError, "an error text");
  WriteModuleEvent(EventLogDirective::kOn, kInfo, "an info text");
  ASSERT_EQ(2u, entries.size());
  EXPECT_EQ(EventLogLevel::kWarning, entries[0].level);
  EXPECT_NE(std::string::npos, entries[0].text.find("a warning text"));
  EXPECT_EQ(EventLogLevel::kError, entries[1].level);
  EXPECT_NE(std::string::npos, entries[1].text.find("an error text"));
}

TEST_F(IisEventLogTest, FatalAlwaysWritesEvenWithTheDirectiveOff) {
  WriteModuleEvent(EventLogDirective::kOff, kFatal, "a fatal text");
  WriteModuleEvent(EventLogDirective::kOn, kFatal, "another fatal text");
  ASSERT_EQ(2u, entries.size());
  EXPECT_EQ(EventLogLevel::kError, entries[0].level);
  EXPECT_EQ(EventLogLevel::kError, entries[1].level);
  EXPECT_EQ(0, CountText("Event-log writing is off"));
}

TEST_F(IisEventLogTest, OffWritesOneNoticePerSeverityAndNoMessage) {
  WriteModuleEvent(EventLogDirective::kOff, kWarning, "suppressed warning one");
  WriteModuleEvent(EventLogDirective::kOff, kWarning, "suppressed warning two");
  WriteModuleEvent(EventLogDirective::kOff, kError, "suppressed error one");
  WriteModuleEvent(EventLogDirective::kOff, kError, "suppressed error two");
  // The messages themselves never appear...
  EXPECT_EQ(0, CountText("suppressed warning one"));
  EXPECT_EQ(0, CountText("suppressed error one"));
  // ...exactly one notice per severity does...
  EXPECT_EQ(1, CountText("so warnings from the pagespeed module"));
  EXPECT_EQ(1, CountText("so errors from the pagespeed module"));
  // ...each naming the directive and the alternative reading place.
  EXPECT_EQ(2, CountText("pagespeed UseEventLog on"));
  EXPECT_EQ(2u, entries.size());
}

TEST_F(IisEventLogTest, OneEntryPerDistinctMessageText) {
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "the same warning");
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "the same warning");
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "a different warning");
  ASSERT_EQ(2u, entries.size());
  EXPECT_EQ(1, CountText("the same warning"));
  EXPECT_EQ(1, CountText("a different warning"));
}

TEST_F(IisEventLogTest, BeforeTheDirectiveIsReadNothingIsWrittenOrRemembered) {
  // The window before a site's configuration is parsed: the factory's own
  // start-up messages arrive here, and the directive's value is not known.
  WriteModuleEvent(EventLogDirective::kNotReadYet, kWarning, "early warning");
  WriteModuleEvent(EventLogDirective::kNotReadYet, kError, "early error");
  EXPECT_EQ(0u, entries.size());
  EXPECT_EQ(0, DistinctTextCountForTesting());

  // Nothing was latched either: the same texts write once the directive is
  // known to be on, and a site that has it OFF still gets its notices.
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "early warning");
  WriteModuleEvent(EventLogDirective::kOff, kError, "early error");
  EXPECT_EQ(1, CountText("early warning"));
  EXPECT_EQ(0, CountText("early error"));
  EXPECT_EQ(1, CountText("so errors from the pagespeed module"));
}

TEST_F(IisEventLogTest, AFatalIsWrittenOncePerDistinctText) {
  WriteModuleEvent(EventLogDirective::kOff, kFatal, "the same fatal");
  WriteModuleEvent(EventLogDirective::kOff, kFatal, "the same fatal");
  WriteModuleEvent(EventLogDirective::kOn, kFatal, "another fatal");
  EXPECT_EQ(1, CountText("the same fatal"));
  EXPECT_EQ(1, CountText("another fatal"));
  EXPECT_EQ(2u, entries.size());
}

TEST_F(IisEventLogTest, PastTheCapNoFurtherTextIsRemembered) {
  SetEventLogMaxEntriesForTesting(2);
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "one");
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "two");
  const int held = DistinctTextCountForTesting();
  for (int i = 0; i < 50; ++i) {
    WriteModuleEvent(
        EventLogDirective::kOn, kWarning,
        StrCat("a message carrying /url/", IntegerToString(i)).c_str());
  }
  // The entries stop at the cap, and so does the memory behind them.
  EXPECT_EQ(held, DistinctTextCountForTesting());
}

TEST_F(IisEventLogTest, TheCapStopsEverythingAfterOneFinalEntry) {
  SetEventLogMaxEntriesForTesting(2);
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "first distinct message");
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "second distinct message");
  WriteModuleEvent(EventLogDirective::kOn, kWarning, "third distinct message");
  WriteModuleEvent(EventLogDirective::kOn, kFatal, "a fatal past the cap");
  ASSERT_EQ(3u, entries.size());
  EXPECT_EQ(1, CountText("first distinct message"));
  EXPECT_EQ(1, CountText("second distinct message"));
  EXPECT_EQ(0, CountText("third distinct message"));
  EXPECT_EQ(0, CountText("a fatal past the cap"));
  EXPECT_EQ(1, CountText("has reached its cap of 2 event-log entries"));
}

}  // namespace

}  // namespace net_instaweb
