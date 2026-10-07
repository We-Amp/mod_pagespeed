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

// Unit tests for AdminSite.

#include "pagespeed/system/admin_site.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/query_params.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/admin_daemon_handler.h"
#include "pagespeed/system/daemon_reader.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"
#include "test/net/instaweb/rewriter/custom_rewrite_test_base.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace net_instaweb {

namespace {

class SystemServerContextNoProxyHtml : public SystemServerContext {
 public:
  explicit SystemServerContextNoProxyHtml(RewriteDriverFactory* factory)
      : SystemServerContext(factory, "fake_hostname", 80 /* fake port */) {}

  virtual bool ProxiesHtml() const { return false; }

 private:
  SystemServerContextNoProxyHtml(const SystemServerContextNoProxyHtml&) =
      delete;
  SystemServerContextNoProxyHtml& operator=(
      const SystemServerContextNoProxyHtml&) = delete;
};

// A SimpleStats that records whether anyone asked it for its console logger.
// SimpleStats has none (Statistics::console_logger() returns NULL), so the
// spy keeps that answer and only notes the question.
class SpyStatistics : public SimpleStats {
 public:
  explicit SpyStatistics(ThreadSystem* thread_system)
      : SimpleStats(thread_system) {}

  StatisticsLogger* console_logger() override {
    asked_console_logger_ = true;
    return nullptr;
  }

  bool asked_console_logger() const { return asked_console_logger_; }

 private:
  bool asked_console_logger_ = false;
};

// CountHistogram -- what SimpleStats::NewHistogram creates -- is a trivial
// stand-in whose NumBuckets() is hardcoded to 0 (see statistics.h): it never
// carries real per-bucket data, in test or in production (real deployments
// get their bucket data from the shared-memory Histogram implementation).
// To exercise PrintHistograms' per-bucket JSON against real, non-empty
// buckets below, this small histogram tracks fixed-width buckets over
// [0, 100) in addition to CountHistogram's count-only bookkeeping.
class FixedBucketHistogram : public CountHistogram {
 public:
  explicit FixedBucketHistogram(AbstractMutex* mutex) : CountHistogram(mutex) {
    std::fill(std::begin(bucket_counts_), std::end(bucket_counts_), 0.0);
  }

  void Add(double value) override {
    CountHistogram::Add(value);
    ScopedMutex hold(lock());
    int index = static_cast<int>(value / kBucketWidth);
    index = std::max(0, std::min(kNumBuckets - 1, index));
    ++bucket_counts_[index];
  }

  int NumBuckets() override { return kNumBuckets; }
  double BucketStart(int index) override { return index * kBucketWidth; }
  double BucketCount(int index) override {
    ScopedMutex hold(lock());
    return bucket_counts_[index];
  }

 private:
  static constexpr int kNumBuckets = 10;
  static constexpr double kBucketWidth = 10;
  double bucket_counts_[kNumBuckets];
};

// A SimpleStats whose histograms are FixedBucketHistogram instead of the
// bucket-less CountHistogram, so a test can assert on a real, exact
// per-bucket count instead of just the array's opening bracket.
class BucketedStatistics : public SimpleStats {
 public:
  explicit BucketedStatistics(ThreadSystem* thread_system)
      : SimpleStats(thread_system), thread_system_(thread_system) {}

  CountHistogram* NewHistogram(StringPiece /*name*/) override {
    return new FixedBucketHistogram(thread_system_->NewMutex());
  }

 private:
  ThreadSystem* thread_system_;
};

class AdminSiteTest : public CustomRewriteTestBase<SystemRewriteOptions> {
 protected:
  AdminSiteTest()
      : thread_system_(Platform::CreateThreadSystem()),
        options_(new SystemRewriteOptions(thread_system_.get())) {}

  virtual void SetUp() {
    CustomRewriteTestBase<SystemRewriteOptions>::SetUp();
    server_context_.reset(SetupServerContext(options_.release()));
    admin_site_ = std::make_unique<AdminSite>(timer(), message_handler(),
                                              nullptr /* daemon_reader */);
  }

  virtual void TearDown() { RewriteTestBase::TearDown(); }

  ServerContext* SetupServerContext(SystemRewriteOptions* config) {
    std::unique_ptr<SystemServerContext> server_context(
        new SystemServerContextNoProxyHtml(factory()));
    server_context->reset_global_options(config);
    server_context->set_statistics(factory()->statistics());
    return server_context.release();
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<ServerContext> server_context_;
  std::unique_ptr<SystemRewriteOptions> options_;
  std::unique_ptr<AdminSite> admin_site_;
};

TEST_F(AdminSiteTest, MessageHistoryReturnsJson) {
  EXPECT_EQ(message_handler(), admin_site_->MessageHandlerForTesting());
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kError, "Test for %s", "Errors");
  message_handler()->Message(kWarning, "Test for %s", "Warnings");
  message_handler()->Message(kInfo, "Test for %s", "Infos");
  GoogleString buffer;
  StringAsyncFetch fetch(rewrite_driver()->request_context(), &buffer);
  QueryParams none;
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, none, &fetch);
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"severity\":\"error\""));
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"severity\":\"warning\""));
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"severity\":\"info\""));
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"messages\":["));
  EXPECT_THAT(buffer, ::testing::HasSubstr("Test for Errors"));
  EXPECT_THAT(buffer, ::testing::HasSubstr("Test for Warnings"));
  EXPECT_THAT(buffer, ::testing::HasSubstr("Test for Infos"));
}

// Parses the "next":N cursor out of a MessageHistoryHandler JSON response.
bool ExtractNext(const GoogleString& json, int64* next) {
  size_t pos = json.find("\"next\":");
  if (pos == GoogleString::npos) return false;
  return StringToInt64(
      json.substr(pos + 7,
                  json.find_first_not_of("0123456789", pos + 7) - (pos + 7)),
      next);
}

TEST_F(AdminSiteTest, MessageHistoryCarriesScopeAndCursor) {
  message_handler()->Message(kInfo, "one");
  message_handler()->Message(kInfo, "two");
  message_handler()->Message(kInfo, "three");
  GoogleString body;
  StringAsyncFetch fetch(rewrite_driver()->request_context(), &body);
  QueryParams none;
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, none, &fetch);
  EXPECT_THAT(body, ::testing::HasSubstr("\"scope\":\"process\""));
  // Three single-line messages: exactly 3 lines written, not a mere digit.
  EXPECT_THAT(body, ::testing::HasSubstr("\"next\":3"));
}

TEST_F(AdminSiteTest, MessageHistorySinceReturnsOnlyNewerMessages) {
  message_handler()->Message(kInfo, "old-1");
  message_handler()->Message(kInfo, "old-2");
  GoogleString first;
  StringAsyncFetch f1(rewrite_driver()->request_context(), &first);
  QueryParams none;
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, none, &f1);
  int64 next = 0;
  ASSERT_TRUE(ExtractNext(first, &next));  // helper: parses "next":N
  message_handler()->Message(kInfo, "new-1");
  GoogleString second;
  StringAsyncFetch f2(rewrite_driver()->request_context(), &second);
  QueryParams since;
  since.AddEscaped("since", Integer64ToString(next));
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, since, &f2);
  EXPECT_THAT(second, ::testing::HasSubstr("new-1"));
  EXPECT_THAT(second, ::testing::Not(::testing::HasSubstr("old-2")));
  // since == next: nothing new.
  GoogleString third;
  StringAsyncFetch f3(rewrite_driver()->request_context(), &third);
  int64 next2 = 0;
  ASSERT_TRUE(ExtractNext(second, &next2));
  QueryParams since2;
  since2.AddEscaped("since", Integer64ToString(next2));
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, since2, &f3);
  EXPECT_THAT(third, ::testing::HasSubstr("\"messages\":[]"));
}

TEST_F(AdminSiteTest, SinceOlderThanRetainedReturnsAll) {
  // MessageHandler::ParseMessageDumpIntoMessages always discards the first
  // (possibly partial) line of the dump, so the very first message ever
  // logged is never visible on its own -- a leading throwaway message (as
  // MessageHistoryReturnsJson also does above) is required to see "kept".
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kInfo, "kept");
  GoogleString body;
  StringAsyncFetch fetch(rewrite_driver()->request_context(), &body);
  QueryParams since;
  since.AddEscaped("since", "0");
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, since, &fetch);
  EXPECT_THAT(body, ::testing::HasSubstr("kept"));
}

TEST_F(AdminSiteTest, SinceAcrossMultiLineMessageReturnsAllItsLines) {
  // One Write() can carry a multi-line message (SystemMessageHandler's
  // AddMessageToBuffer joins one); the cursor counts lines, not Write()
  // calls/messages, so none of a multi-line message's lines are dropped.
  message_handler()->Message(kInfo, "Ignore the first line.");
  GoogleString before;
  StringAsyncFetch f0(rewrite_driver()->request_context(), &before);
  QueryParams none;
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, none, &f0);
  int64 since = 0;
  ASSERT_TRUE(
      ExtractNext(before, &since));  // the count before the 3-line message

  message_handler()->Message(kInfo, "multi-line-1\nmulti-line-2\nmulti-line-3");
  message_handler()->Message(kInfo, "single-after");

  GoogleString body;
  StringAsyncFetch fetch(rewrite_driver()->request_context(), &body);
  QueryParams since_params;
  since_params.AddEscaped("since", Integer64ToString(since));
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, since_params,
                                     &fetch);
  EXPECT_THAT(body, ::testing::HasSubstr("multi-line-1"));
  EXPECT_THAT(body, ::testing::HasSubstr("multi-line-2"));
  EXPECT_THAT(body, ::testing::HasSubstr("multi-line-3"));
  EXPECT_THAT(body, ::testing::HasSubstr("single-after"));
}

// The console's logger-consuming leaves (console, graphs,
// statistics?json) follow the console's scope. The global console must ask
// the global statistics object for its logger, never the vhost-local one;
// the per-vhost console asks the local one.
TEST_F(AdminSiteTest, GlobalConsoleDoesNotConsultLocalStatistics) {
  SpyStatistics local_spy(thread_system_.get());
  SystemServerContext* ssc =
      static_cast<SystemServerContext*>(server_context_.get());
  ssc->set_statistics(&local_spy);
  ssc->SetAdminSiteForTesting(std::make_unique<AdminSite>(
      timer(), message_handler(), nullptr /* daemon_reader */));
  GoogleString body;
  StringAsyncFetch fetch(rewrite_driver()->request_context(), &body);
  fetch.request_headers()->set_method(RequestHeaders::kGet);
  GoogleUrl gurl("http://example.com/pagespeed_global_admin/console?json");
  QueryParams query_params;
  query_params.ParseFromUrl(gurl);
  ssc->AdminPage(true /* global */, gurl, query_params,
                 rewrite_driver()->options(), &fetch, "");
  EXPECT_TRUE(fetch.done());
  EXPECT_FALSE(local_spy.asked_console_logger())
      << "the global console consulted the vhost-local statistics object for "
         "its logger";
}

TEST_F(AdminSiteTest, VhostConsoleConsultsLocalStatistics) {
  SpyStatistics local_spy(thread_system_.get());
  SystemServerContext* ssc =
      static_cast<SystemServerContext*>(server_context_.get());
  ssc->set_statistics(&local_spy);
  ssc->SetAdminSiteForTesting(std::make_unique<AdminSite>(
      timer(), message_handler(), nullptr /* daemon_reader */));
  GoogleString body;
  StringAsyncFetch fetch(rewrite_driver()->request_context(), &body);
  fetch.request_headers()->set_method(RequestHeaders::kGet);
  GoogleUrl gurl("http://example.com/pagespeed_admin/console?json");
  QueryParams query_params;
  query_params.ParseFromUrl(gurl);
  ssc->AdminPage(false /* global */, gurl, query_params,
                 rewrite_driver()->options(), &fetch, "");
  EXPECT_TRUE(fetch.done());
  EXPECT_TRUE(local_spy.asked_console_logger())
      << "the per-vhost console did not consult its own statistics object";
}

// =============================================================================
// Message templates.  The admin console implements the same rules in
// TypeScript; both test suites read testdata/message_templates.tsv.
// =============================================================================

// "Wed Jan 01 00:00:00 2014" -- the time MockMessageHandler writes -- in ms.
constexpr int64 kMockMessageTimeMs = 1388534400000LL;

struct TemplateCase {
  GoogleString line;
  GoogleString expected;
};

// Rows of the shared fixture: '#' lines and blank lines are skipped; a row is
// split at its first TAB; a trailing CR (a CRLF checkout) is dropped.
std::vector<TemplateCase> ReadTemplateFixture() {
  std::vector<TemplateCase> cases;
  std::ifstream in(StrCat(GTestSrcDir(),
                          "/test/pagespeed/system/testdata/"
                          "message_templates.tsv"),
                   std::ios::binary);
  std::string row;
  while (std::getline(in, row)) {
    if (!row.empty() && row.back() == '\r') row.pop_back();
    if (row.empty() || row[0] == '#') continue;
    const size_t tab = row.find('\t');
    if (tab == std::string::npos) {
      ADD_FAILURE() << "fixture row without a TAB: " << row;
      continue;
    }
    cases.push_back({row.substr(0, tab), row.substr(tab + 1)});
  }
  return cases;
}

TEST(MessageTemplateTest, SharedFixture) {
  const std::vector<TemplateCase> cases = ReadTemplateFixture();
  ASSERT_EQ(17u, cases.size()) << "fixture missing or changed size";
  for (const TemplateCase& c : cases) {
    EXPECT_EQ(c.expected, MessageTemplate(MessageLineBody(c.line)))
        << "line: " << c.line;
  }
}

TEST(MessageTemplateTest, NumbersUrlsAndIds) {
  EXPECT_EQ("took N ms", MessageTemplate("took 250 ms"));
  EXPECT_EQ("vN.N.N", MessageTemplate("v1.16.0"));
  EXPECT_EQ("Nms", MessageTemplate("250ms"));
  EXPECT_EQ("see URL", MessageTemplate("see https://a.test/x?y=1"));
  EXPECT_EQ("see URL", MessageTemplate("see HTTP://A.TEST/"));
  EXPECT_EQ("'URL'", MessageTemplate("'http://a.test/b'"));
  EXPECT_EQ("<URL>", MessageTemplate("<http://a.test/b>"));
  EXPECT_EQ("id ID", MessageTemplate("id ecb2343a75c6a0b7"));
  EXPECT_EQ("id ID", MessageTemplate("id 0x1F"));
  // Digits only: a number, not an identifier.
  EXPECT_EQ("id N", MessageTemplate("id 12345678"));
  // Hex letters only: an ordinary word.
  EXPECT_EQ("deadbeef", MessageTemplate("deadbeef"));
  // Seven characters: too short for an identifier.
  EXPECT_EQ("abcNdef", MessageTemplate("abc1def"));
  // "0x" without hex digits after it is not an identifier.
  EXPECT_EQ("Nx", MessageTemplate("0x"));
  EXPECT_EQ("NXNG", MessageTemplate("0X1G"));
  // A scheme inside a word is not a URL; other schemes are not URLs.
  EXPECT_EQ("xhttp://a", MessageTemplate("xhttp://a"));
  EXPECT_EQ("ftp://host/N", MessageTemplate("ftp://host/7"));
}

TEST(MessageTemplateTest, WhitespaceAndNonAsciiBytes) {
  EXPECT_EQ("done", MessageTemplate("done  \t\r"));
  EXPECT_EQ("", MessageTemplate(""));
  EXPECT_EQ("  lead", MessageTemplate("  lead"));
  // Non-ASCII bytes are copied; only ASCII digits count as digits.
  EXPECT_EQ("Gr\xC3\xB6\xC3\x9F" "e N", MessageTemplate("Gr\xC3\xB6\xC3\x9F" "e 42"));
}

TEST(MessageTemplateTest, MessageLineBodyStripsTheHeader) {
  EXPECT_EQ("hello",
            MessageLineBody("[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] hello"));
  EXPECT_EQ("hello",
            MessageLineBody("[Wed Jan 01 00:00:00 2014] [Info] [00000] hello"));
  EXPECT_EQ("parse", MessageLineBody("[Fri, 02 Oct 2026 10:25:51 GMT] [Error] "
                                     "[7] [a/b.cc:12] parse"));
  // Without a pid group only the time and the level go.
  EXPECT_EQ("x", MessageLineBody("[Fri, 02 Oct 2026 10:25:51 GMT] [Info] x"));
  // A pid group that is not digits stays, and so does what follows it.
  EXPECT_EQ("[abc] [a.cc:1] x", MessageLineBody("[t] [Info] [abc] [a.cc:1] x"));
  // A continuation line, or an unknown level, is not a header.
  EXPECT_EQ("  at frame 2", MessageLineBody("  at frame 2"));
  EXPECT_EQ("[t] [Notice] [1] x", MessageLineBody("[t] [Notice] [1] x"));
  // A group must be followed by a space to count.
  EXPECT_EQ("[t][Info] x", MessageLineBody("[t][Info] x"));
}

TEST(MessageTemplateTest, MessageLineTimeMsParsesTheHeaderOnly) {
  EXPECT_EQ(1790936751000LL,
            MessageLineTimeMs(
                "[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] hello"));
  EXPECT_EQ(kMockMessageTimeMs,
            MessageLineTimeMs("[Wed Jan 01 00:00:00 2014] [Info] [00000] hi"));
  EXPECT_EQ(-1, MessageLineTimeMs("  at frame 2"));
  EXPECT_EQ(-1, MessageLineTimeMs("[not a time] [Info] [1] x"));
  EXPECT_EQ(-1, MessageLineTimeMs(""));
}

// =============================================================================
// message_history?grouped=1
// =============================================================================

class AdminSiteGroupedMessagesTest : public AdminSiteTest {
 protected:
  struct Answer {
    int status = 0;
    GoogleString body;
    GoogleString allow;
    GoogleString cache_control;
    GoogleString content_type;
  };

  // Calls the message-history handler with `query` (e.g. "grouped=1").
  Answer Ask(StringPiece query,
             RequestHeaders::Method method = RequestHeaders::kGet) {
    Answer answer;
    StringAsyncFetch fetch(rewrite_driver()->request_context(), &answer.body);
    fetch.request_headers()->set_method(method);
    GoogleUrl gurl(
        StrCat("http://example.com/pagespeed_admin/message_history?", query));
    QueryParams params;
    params.ParseFromUrl(gurl);
    admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                       AdminSite::kPageSpeedAdmin, params,
                                       &fetch);
    const ResponseHeaders* headers = fetch.response_headers();
    answer.status = headers->status_code();
    const char* v = headers->Lookup1(HttpAttributes::kAllow);
    if (v != nullptr) answer.allow = v;
    // Cache-Control is one of Headers' comma-separated fields, so a
    // multi-directive value is stored as separate directives and Lookup1
    // cannot return it; LookupJoined re-joins it with ", " (the precedent in
    // AdminSiteDaemonTest.JsonLeavesAreNoStoreAndNosniff).
    answer.cache_control = headers->LookupJoined(HttpAttributes::kCacheControl);
    v = headers->Lookup1(HttpAttributes::kContentType);
    if (v != nullptr) answer.content_type = v;
    return answer;
  }

  // Four letters from g..z (never hex digits) naming `i`, so generated
  // messages have distinct templates.
  static GoogleString LettersFor(int i) {
    GoogleString s;
    for (int k = 0; k < 4; ++k) {
      s.push_back(static_cast<char>('g' + i % 20));
      i /= 20;
    }
    return s;
  }
};

TEST_F(AdminSiteGroupedMessagesTest, GroupsByLevelAndTemplate) {
  SetTimeMs(kMockMessageTimeMs + 60 * 1000);
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kWarning,
                             "Fetch of https://a.test/x.css failed after 12 ms");
  message_handler()->Message(
      kWarning, "Fetch of https://b.test/y.css failed after 900 ms");
  message_handler()->Message(kError, "boom 1");
  message_handler()->Message(kInfo, "hello");
  const Answer a = Ask("grouped=1");
  EXPECT_EQ(HttpStatus::kOK, a.status);
  EXPECT_EQ("no-store, private", a.cache_control);
  EXPECT_EQ("application/json", a.content_type);
  EXPECT_EQ(
      "{\"scope\":\"process\",\"next\":5,\"now_ms\":1388534460000,"
      "\"truncated\":false,\"groups\":["
      "{\"level\":\"warning\",\"template\":\"Fetch of URL failed after N ms\","
      "\"count\":2,\"last_ms\":1388534400000},"
      "{\"level\":\"error\",\"template\":\"boom N\",\"count\":1,"
      "\"last_ms\":1388534400000},"
      "{\"level\":\"info\",\"template\":\"hello\",\"count\":1,"
      "\"last_ms\":1388534400000}]}",
      a.body);
}

TEST_F(AdminSiteGroupedMessagesTest, GroupedWindowUsesTheServerClock) {
  SetTimeMs(kMockMessageTimeMs + 60 * 1000);
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kWarning, "slow 1");
  message_handler()->Message(kWarning, "slow 2");
  const Answer within = Ask("grouped=1&window_s=900");
  EXPECT_THAT(within.body, ::testing::HasSubstr("\"window_s\":900,"));
  EXPECT_THAT(within.body,
              ::testing::HasSubstr("{\"level\":\"warning\",\"template\":\"slow "
                                   "N\",\"count\":2,\"last_ms\":1388534400000,"
                                   "\"recent\":2}"));
  // An hour later the same lines are outside a 15-minute window.
  SetTimeMs(kMockMessageTimeMs + 3600 * 1000);
  const Answer later = Ask("grouped=1&window_s=900");
  EXPECT_THAT(later.body,
              ::testing::HasSubstr("\"count\":2,\"last_ms\":1388534400000,"
                                   "\"recent\":0}"));
}

TEST_F(AdminSiteGroupedMessagesTest, GroupedIgnoresAnInvalidWindow) {
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kWarning, "slow 1");
  for (const char* w : {"0", "86401", "-5", "abc", "1.5", ""}) {
    const Answer a = Ask(StrCat("grouped=1&window_s=", w));
    EXPECT_EQ(HttpStatus::kOK, a.status) << w;
    EXPECT_THAT(a.body, ::testing::Not(::testing::HasSubstr("window_s"))) << w;
    EXPECT_THAT(a.body, ::testing::Not(::testing::HasSubstr("recent"))) << w;
    EXPECT_THAT(a.body, ::testing::HasSubstr("\"template\":\"slow N\"")) << w;
  }
  EXPECT_THAT(Ask("grouped=1&window_s=86400").body,
              ::testing::HasSubstr("\"window_s\":86400,"));
}

TEST_F(AdminSiteGroupedMessagesTest, GroupedHonorsTheSinceCursor) {
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kWarning, "old 1");
  int64 next = 0;
  ASSERT_TRUE(ExtractNext(Ask("").body, &next));
  message_handler()->Message(kWarning, "new 1");
  message_handler()->Message(kWarning, "new 2");
  const Answer a = Ask(StrCat("grouped=1&since=", Integer64ToString(next)));
  EXPECT_THAT(a.body, ::testing::HasSubstr("\"template\":\"new N\",\"count\":2"));
  EXPECT_THAT(a.body, ::testing::Not(::testing::HasSubstr("old N")));
  EXPECT_THAT(a.body, ::testing::HasSubstr("\"next\":4,"));
}

TEST_F(AdminSiteGroupedMessagesTest, GroupedContinuationLinesInheritTheStamp) {
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kWarning, "head 1\n  at frame 2");
  const Answer a = Ask("grouped=1");
  EXPECT_THAT(a.body, ::testing::HasSubstr(
                          "{\"level\":\"warning\",\"template\":\"  at frame "
                          "N\",\"count\":1,\"last_ms\":1388534400000}"));
  EXPECT_THAT(a.body, ::testing::HasSubstr(
                          "{\"level\":\"warning\",\"template\":\"head N\","
                          "\"count\":1,\"last_ms\":1388534400000}"));
}

TEST_F(AdminSiteGroupedMessagesTest, PlainModeIsUnchanged) {
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kWarning, "slow 1");
  for (const char* query : {"", "grouped=0", "grouped=yes", "window_s=900"}) {
    const Answer a = Ask(query);
    EXPECT_EQ(HttpStatus::kOK, a.status) << query;
    EXPECT_THAT(a.body, ::testing::HasSubstr("\"messages\":[")) << query;
    EXPECT_THAT(a.body, ::testing::Not(::testing::HasSubstr("\"groups\"")))
        << query;
  }
}

TEST_F(AdminSiteGroupedMessagesTest, GroupedIsGetOrHeadOnly) {
  message_handler()->Message(kInfo, "Ignore the first line.");
  const Answer post = Ask("grouped=1", RequestHeaders::kPost);
  EXPECT_EQ(HttpStatus::kMethodNotAllowed, post.status);
  EXPECT_EQ("GET, HEAD", post.allow);
  EXPECT_THAT(post.body, ::testing::HasSubstr("method not allowed"));
  EXPECT_EQ(HttpStatus::kOK, Ask("grouped=1", RequestHeaders::kHead).status);
  // The plain mode keeps answering every method, as before.
  EXPECT_EQ(HttpStatus::kOK, Ask("", RequestHeaders::kPost).status);
}

TEST_F(AdminSiteGroupedMessagesTest, GroupedAdmitsOneReadAtATime) {
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kWarning, "slow 1");
  admin_site_->SetGroupedSlotBusyForTesting(true);
  const Answer busy = Ask("grouped=1");
  EXPECT_EQ(429, busy.status);
  EXPECT_EQ("{\"success\":false,\"error\":\"busy\"}", busy.body);
  EXPECT_EQ("no-store, private", busy.cache_control);
  // The plain mode is never refused.
  EXPECT_EQ(HttpStatus::kOK, Ask("").status);
  admin_site_->SetGroupedSlotBusyForTesting(false);
  // A finished grouped read frees the slot for the next one.
  EXPECT_EQ(HttpStatus::kOK, Ask("grouped=1").status);
  EXPECT_EQ(HttpStatus::kOK, Ask("grouped=1").status);
}

TEST_F(AdminSiteGroupedMessagesTest, GroupedStaysUnderTheResponseCap) {
  message_handler()->Message(kInfo, "Ignore the first line.");
  const GoogleString filler(480, 'q');
  for (int i = 0; i < 2600; ++i) {
    message_handler()->Message(kWarning, "%s %s", LettersFor(i).c_str(),
                               filler.c_str());
  }
  const Answer a = Ask("grouped=1&window_s=900");
  EXPECT_EQ(HttpStatus::kOK, a.status);
  EXPECT_LE(a.body.size(), kMaxGroupedMessageBytes);
  EXPECT_GT(a.body.size(), kMaxGroupedMessageBytes / 2);
  EXPECT_THAT(a.body, ::testing::HasSubstr("\"truncated\":true,"));
  ASSERT_GE(a.body.size(), 3u);
  EXPECT_EQ("}]}", a.body.substr(a.body.size() - 3));
}

// =============================================================================
// Defense-in-depth admin-exposure warning tests.
// =============================================================================

// IsLoopbackClientIp predicate — no MessageHandler, no atomic state.
class IsLoopbackClientIpTest : public ::testing::Test {};

TEST_F(IsLoopbackClientIpTest, AcceptsIpv4Loopback) {
  EXPECT_TRUE(IsLoopbackClientIp("127.0.0.1"));
  EXPECT_TRUE(IsLoopbackClientIp("127.0.0.255"));
  EXPECT_TRUE(IsLoopbackClientIp("127.1.2.3"));
  EXPECT_TRUE(IsLoopbackClientIp("127.255.255.255"));
}

TEST_F(IsLoopbackClientIpTest, AcceptsIpv6Loopback) {
  EXPECT_TRUE(IsLoopbackClientIp("::1"));
}

TEST_F(IsLoopbackClientIpTest, AcceptsIpv4MappedIpv6Loopback) {
  // ::ffff:127.0.0.1 form (IPv4-mapped).
  EXPECT_TRUE(IsLoopbackClientIp("::ffff:127.0.0.1"));
  EXPECT_TRUE(IsLoopbackClientIp("::FFFF:127.0.0.1"));
  // ::127.0.0.1 form (IPv4-compatible, deprecated but seen in some stacks).
  EXPECT_TRUE(IsLoopbackClientIp("::127.0.0.1"));
}

TEST_F(IsLoopbackClientIpTest, AcceptsLocalhostLiteral) {
  EXPECT_TRUE(IsLoopbackClientIp("localhost"));
}

TEST_F(IsLoopbackClientIpTest, RejectsNonLoopbackIpv4) {
  EXPECT_FALSE(IsLoopbackClientIp("10.0.0.1"));
  EXPECT_FALSE(IsLoopbackClientIp("192.168.1.1"));
  EXPECT_FALSE(IsLoopbackClientIp("8.8.8.8"));
  // Spoofy near-loopback addresses must NOT be accepted.
  EXPECT_FALSE(IsLoopbackClientIp("128.0.0.1"));
  EXPECT_FALSE(IsLoopbackClientIp("1.2.7.0.0.1"));
}

TEST_F(IsLoopbackClientIpTest, RejectsNonLoopbackIpv6) {
  EXPECT_FALSE(IsLoopbackClientIp("::2"));
  EXPECT_FALSE(IsLoopbackClientIp("2001:db8::1"));
  EXPECT_FALSE(IsLoopbackClientIp("fe80::1"));
  // ::ffff:128.x.y.z is NOT loopback.
  EXPECT_FALSE(IsLoopbackClientIp("::ffff:8.8.8.8"));
}

TEST_F(IsLoopbackClientIpTest, RejectsEmpty) {
  // Caller couldn't determine the IP — treat as non-loopback so the warning
  // fires (loud failure mode is the safer default for a security signal).
  EXPECT_FALSE(IsLoopbackClientIp(""));
}

TEST_F(IsLoopbackClientIpTest, RejectsGarbage) {
  EXPECT_FALSE(IsLoopbackClientIp("not-an-ip"));
  EXPECT_FALSE(IsLoopbackClientIp("127.0.0.1 ; rm -rf /"));
}

// WarnIfNonLoopbackAdminAccess — uses MessageHandler + per-family atomic.
class AdminExposureWarningTest : public ::testing::Test {
 protected:
  void SetUp() override {
    thread_system_.reset(Platform::CreateThreadSystem());
    handler_ = std::make_unique<MockMessageHandler>(new NullMutex);
    ResetAdminExposureWarningsForTesting();
  }

  void TearDown() override { ResetAdminExposureWarningsForTesting(); }

  int WarningCount() const { return handler_->MessagesOfType(kWarning); }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<MockMessageHandler> handler_;
};

TEST_F(AdminExposureWarningTest, FiresForNonLoopbackClient) {
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(1, WarningCount());
}

TEST_F(AdminExposureWarningTest, SilentForLoopback) {
  WarnIfNonLoopbackAdminAccess("127.0.0.1", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("::1", AdminHandlerFamily::kStatistics,
                               "mod_pagespeed_statistics", handler_.get());
  WarnIfNonLoopbackAdminAccess("localhost", AdminHandlerFamily::kConsole,
                               "pagespeed_console", handler_.get());
  WarnIfNonLoopbackAdminAccess("::ffff:127.0.0.1",
                               AdminHandlerFamily::kMessages,
                               "mod_pagespeed_message", handler_.get());
  EXPECT_EQ(0, WarningCount());
}

TEST_F(AdminExposureWarningTest, IdempotentPerFamily) {
  // Three calls to the same family from non-loopback IPs → exactly one
  // warning.
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("10.0.0.7", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("8.8.8.8", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(1, WarningCount());
}

TEST_F(AdminExposureWarningTest, IndependentAcrossFamilies) {
  // Distinct families get their own one-shot.
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kGlobalAdmin,
                               "pagespeed_global_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kStatistics,
                               "mod_pagespeed_statistics", handler_.get());
  WarnIfNonLoopbackAdminAccess(
      "192.168.1.5", AdminHandlerFamily::kGlobalStatistics,
      "mod_pagespeed_global_statistics", handler_.get());
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kConsole,
                               "pagespeed_console", handler_.get());
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kMessages,
                               "mod_pagespeed_message", handler_.get());
  EXPECT_EQ(6, WarningCount());
}

TEST_F(AdminExposureWarningTest, ResetReArmsAllFamilies) {
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(1, WarningCount());
  ResetAdminExposureWarningsForTesting();
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(2, WarningCount());
}

TEST_F(AdminExposureWarningTest, EmptyIpStillFires) {
  // Empty client IP = caller couldn't determine. Treat as non-loopback so the
  // operator at least sees that the handler ran.
  WarnIfNonLoopbackAdminAccess("", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(1, WarningCount());
}

TEST_F(AdminExposureWarningTest, WarningMentionsHandlerAndIp) {
  WarnIfNonLoopbackAdminAccess("10.20.30.40", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  GoogleString dump;
  StringWriter writer(&dump);
  handler_->Dump(&writer);
  EXPECT_THAT(dump, ::testing::HasSubstr("pagespeed_admin"));
  EXPECT_THAT(dump, ::testing::HasSubstr("10.20.30.40"));
  EXPECT_THAT(dump, ::testing::HasSubstr("loopback"));
}

TEST_F(AdminExposureWarningTest, NullHandlerIsSafe) {
  // Don't crash if the caller can't supply a handler (unusual but harmless).
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", nullptr);
  // But: the atomic still flips, so a subsequent call with a real handler
  // is suppressed. This is fine — the warning is best-effort.
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(0, WarningCount());
}

// =============================================================================
// PostInitHook: AdminSite construction and the absence of license state,
// and drop-in compatibility with releases that had it.
// =============================================================================

// Builds a serving (non-stub) SystemServerContext whose FileCachePath is
// |file_cache_path| and runs its PostInitHook, as every port does at startup.
std::unique_ptr<SystemServerContext> ServingContextOn(
    RewriteDriverFactory* factory, ThreadSystem* thread_system,
    MessageHandler* handler, const GoogleString& file_cache_path) {
  std::unique_ptr<SystemServerContext> sc(
      new SystemServerContextNoProxyHtml(factory));
  SystemRewriteOptions* opts = new SystemRewriteOptions(thread_system);
  opts->set_file_cache_path(file_cache_path);
  sc->reset_global_options(opts);
  sc->set_statistics(factory->statistics());
  sc->set_message_handler(handler);
  sc->PostInitHook();
  return sc;
}

// A decoding stub context must skip AdminSite setup in PostInitHook(): it
// runs on default options (no FileCachePath) and never serves requests.
TEST_F(AdminSiteTest, DecodingStubSkipsAdminSiteInit) {
  std::unique_ptr<SystemServerContext> stub(
      new SystemServerContextNoProxyHtml(factory()));
  stub->reset_global_options(new SystemRewriteOptions(thread_system_.get()));
  stub->set_statistics(factory()->statistics());
  stub->set_message_handler(message_handler());
  stub->set_is_decoding_stub(true);
  stub->PostInitHook();
  EXPECT_EQ(nullptr, stub->admin_site());
}

// There is no license state. A serving context with no license
// file anywhere builds its AdminSite and logs nothing about licensing -- the
// old startup UNLICENSED warning is gone.
TEST_F(AdminSiteTest, ServingContextLogsNoLicenseMessage) {
  ResetStaleLicenseFileNoticeForTesting();
  std::unique_ptr<SystemServerContext> sc =
      ServingContextOn(factory(), thread_system_.get(), message_handler(),
                       StrCat(GTestTempDir(), "/no-such-dir/cache"));
  EXPECT_NE(nullptr, sc->admin_site());
  GoogleString messages;
  StringWriter writer(&messages);
  message_handler()->Dump(&writer);
  EXPECT_THAT(messages, ::testing::Not(::testing::HasSubstr("UNLICENSED")));
  EXPECT_THAT(messages, ::testing::Not(::testing::HasSubstr("icense")));
}

// Drop-in compatibility: a pagespeed.license left behind by an
// earlier release -- next to FileCachePath, where those releases kept it -- is
// ignored: never read, never deleted, and mentioned exactly once per process
// at INFO so the operator knows it can go. Two serving contexts (Apache: one
// per vhost) share the one notice; a trailing separator on the cache path is
// stripped, as the old reader did, so the sibling file is still found.
TEST_F(AdminSiteTest, StaleLicenseFileNoticedOnceAtInfoAndLeftAlone) {
  ResetStaleLicenseFileNoticeForTesting();
  namespace fs = std::filesystem;
  const fs::path root = fs::path(GTestTempDir()) / "stale-license-notice";
  fs::remove_all(root);
  const fs::path cache_dir = root / "cache";
  ASSERT_TRUE(fs::create_directories(cache_dir));
  const fs::path license_file = root / "pagespeed.license";
  const char kStaleToken[] = "stale-token-from-a-previous-release";
  {
    std::ofstream out(license_file);
    out << kStaleToken;
  }
  ASSERT_TRUE(fs::exists(license_file));

  const GoogleString cache_path = StrCat(cache_dir.string(), "/");
  std::unique_ptr<SystemServerContext> first = ServingContextOn(
      factory(), thread_system_.get(), message_handler(), cache_path);
  std::unique_ptr<SystemServerContext> second = ServingContextOn(
      factory(), thread_system_.get(), message_handler(), cache_path);
  EXPECT_NE(nullptr, first->admin_site());
  EXPECT_NE(nullptr, second->admin_site());

  GoogleString messages;
  StringWriter writer(&messages);
  message_handler()->Dump(&writer);
  const GoogleString notice =
      StrCat(license_file.string(),
             ": this file is no longer read since 2.1 and can be removed");
  EXPECT_EQ(1, CountSubstring(messages, notice)) << messages;
  // INFO, not a warning: MockMessageHandler prefixes each line with its type.
  EXPECT_THAT(messages,
              ::testing::HasSubstr(StrCat("[Info] [00000] ", notice)));
  EXPECT_THAT(messages, ::testing::Not(::testing::HasSubstr("UNLICENSED")));

  // Left alone: still there, byte-identical.
  ASSERT_TRUE(fs::exists(license_file));
  std::ifstream in(license_file);
  const std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(kStaleToken, contents);
  fs::remove_all(root);
}

// With no stale file next to the cache directory nothing is logged, and the
// process-wide latch stays unset for a later context that does have one.
TEST_F(AdminSiteTest, NoStaleLicenseFileMeansNoNotice) {
  ResetStaleLicenseFileNoticeForTesting();
  namespace fs = std::filesystem;
  const fs::path root = fs::path(GTestTempDir()) / "no-stale-license";
  fs::remove_all(root);
  ASSERT_TRUE(fs::create_directories(root / "cache"));
  std::unique_ptr<SystemServerContext> sc =
      ServingContextOn(factory(), thread_system_.get(), message_handler(),
                       (root / "cache").string());
  EXPECT_NE(nullptr, sc->admin_site());
  GoogleString messages;
  StringWriter writer(&messages);
  message_handler()->Dump(&writer);
  EXPECT_THAT(
      messages,
      ::testing::Not(::testing::HasSubstr("is no longer read since 2.1")));
  fs::remove_all(root);
}

// The factory marks the context it builds through
// InitStubDecodingServerContext() as a decoding stub.
TEST_F(AdminSiteTest, FactoryDecodingContextIsMarkedAsStub) {
  std::unique_ptr<ServerContext> stub(factory()->NewDecodingServerContext());
  EXPECT_TRUE(stub->is_decoding_stub());
}

// =============================================================================
// /v1/daemon/* read-only proxy tests.
// =============================================================================

// Test double for the DaemonReader seam: completes synchronously with a
// canned response, records the requested daemon path, and can hold one
// request in flight (to exercise the per-endpoint slot).
class FakeDaemonReader : public DaemonReader {
 public:
  explicit FakeDaemonReader(MessageHandler* handler) : handler_(handler) {}

  void Get(StringPiece daemon_path, size_t max_response_bytes,
          AsyncFetch* fetch, DaemonReadFailure* failure) override {
    ++call_count_;
    daemon_path.CopyToString(&last_daemon_path_);
    last_max_bytes_ = max_response_bytes;
    if (daemon_path == hold_path_) {
      held_fetch_ = fetch;
      held_failure_ = failure;
      held_all_.emplace_back(fetch, failure);
      return;
    }
    Complete(fetch, failure);
  }

  void Complete(AsyncFetch* fetch, DaemonReadFailure* failure) {
    if (fail_) {
      *failure = fail_reason_;
      fetch->Done(false);
      return;
    }
    *failure = DaemonReadFailure::kNone;
    fetch->response_headers()->set_status_code(status_);
    if (!omit_content_type_) {
      fetch->response_headers()->Add(HttpAttributes::kContentType,
                                     content_type_);
    }
    if (!second_content_type_.empty()) {
      fetch->response_headers()->Add(HttpAttributes::kContentType,
                                     second_content_type_);
    }
    // An upstream-only header that must never leak to the client response.
    fetch->response_headers()->Add("X-Upstream-Only", "secret");
    fetch->Write(body_, handler_);
    fetch->Done(true);
  }

  // Completes the captured `held_fetch_` with the canned response.
  void CompleteHeld() { Complete(held_fetch_, held_failure_); }

  // Completes every held request, in arrival order, with the canned
  // response.
  void CompleteAllHeld() {
    std::vector<std::pair<AsyncFetch*, DaemonReadFailure*>> held;
    held.swap(held_all_);
    for (const auto& h : held) {
      Complete(h.first, h.second);
    }
    held_fetch_ = nullptr;
    held_failure_ = nullptr;
  }

  // Completes the captured `held_fetch_` as a transport failure with the
  // given reason (the fetch was pending because its path matched
  // `hold_path_`).
  void Fail(DaemonReadFailure reason = DaemonReadFailure::kDisconnected) {
    *held_failure_ = reason;
    held_fetch_->Done(false);
  }

  // Completes the captured `held_fetch_` with an explicit upstream
  // status/body, as if the daemon had actually answered.
  void CompleteWithStatus(int status, StringPiece body) {
    *held_failure_ = DaemonReadFailure::kNone;
    held_fetch_->response_headers()->set_status_code(status);
    held_fetch_->response_headers()->Add(HttpAttributes::kContentType,
                                         content_type_);
    held_fetch_->Write(body, handler_);
    held_fetch_->Done(true);
  }

  MessageHandler* handler_;
  int call_count_ = 0;
  GoogleString last_daemon_path_;
  size_t last_max_bytes_ = 0;
  int status_ = HttpStatus::kOK;
  GoogleString content_type_ = "application/json";
  // When non-empty, Complete() adds a second Content-Type header.
  GoogleString second_content_type_;
  // When true, Complete() sends no Content-Type header at all.
  bool omit_content_type_ = false;
  GoogleString body_ = "{\"status\":\"ok\"}";
  bool fail_ = false;
  DaemonReadFailure fail_reason_ = DaemonReadFailure::kDisconnected;
  // Requests for this daemon path are held in flight until completed
  // through CompleteHeld()/Fail()/CompleteWithStatus().
  GoogleString hold_path_;
  AsyncFetch* held_fetch_ = nullptr;
  DaemonReadFailure* held_failure_ = nullptr;
  // Every held request, in arrival order (held_fetch_ is the latest).
  std::vector<std::pair<AsyncFetch*, DaemonReadFailure*>> held_all_;
};

class AdminSiteDaemonTest : public AdminSiteTest {
 protected:
  void SetUp() override {
    AdminSiteTest::SetUp();
    // daemon_site_ takes ownership of the reader.
    fake_reader_ = new FakeDaemonReader(message_handler());
    daemon_site_ =
        std::make_unique<AdminSite>(timer(), message_handler(), fake_reader_);
  }

  void TearDown() override {
    // Content reads are admitted process-wide: a read a test leaves held
    // would leak its slot into every later test in this binary.  Every
    // test must complete what it holds.
    EXPECT_EQ(0, AdminDaemonHandler::ContentReadsInFlightForTesting());
    AdminSiteTest::TearDown();
  }

  // Issues a request for `url` against `site`'s AdminPage and returns the
  // client fetch.  `url` includes the admin path prefix and may carry a
  // query string.
  std::unique_ptr<StringAsyncFetch> FetchAdminPage(
      AdminSite* site, const GoogleString& url, RequestHeaders::Method method,
      GoogleString* buffer, bool is_global = true,
      const char* x_requested_with = nullptr,
      const char* sec_fetch_site = nullptr,
      StringPiece own_serve_host = StringPiece()) {
    std::unique_ptr<StringAsyncFetch> fetch(
        new StringAsyncFetch(rewrite_driver()->request_context(), buffer));
    fetch->request_headers()->set_method(method);
    if (x_requested_with != nullptr) {
      fetch->request_headers()->Add("X-Requested-With", x_requested_with);
    }
    if (sec_fetch_site != nullptr) {
      fetch->request_headers()->Add("Sec-Fetch-Site", sec_fetch_site);
    }
    GoogleUrl gurl(url);
    QueryParams query_params;
    query_params.ParseFromUrl(gurl);
    SystemServerContext* system_server_context =
        static_cast<SystemServerContext*>(server_context_.get());
    site->AdminPage(
        is_global, gurl, query_params, rewrite_driver()->options(),
        nullptr /* cache_path */, fetch.get(), nullptr /* system_caches */,
        nullptr /* filesystem_metadata_cache */, nullptr /* http_cache */,
        nullptr /* metadata_cache */, nullptr /* page_property_cache */,
        system_server_context, factory()->statistics(), factory()->statistics(),
        system_server_context->global_system_rewrite_options(),
        "" /* request_body */, own_serve_host);
    return fetch;
  }

  FakeDaemonReader* fake_reader_ = nullptr;  // owned by daemon_site_
  std::unique_ptr<AdminSite> daemon_site_;
};

TEST_F(AdminSiteDaemonTest, HappyPathForwardsStatusContentTypeAndBody) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(1, fake_reader_->call_count_);
  EXPECT_EQ("/v1/health", fake_reader_->last_daemon_path_);
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"status\":\"ok\"}", buffer);
  const char* content_type =
      fetch->response_headers()->Lookup1(HttpAttributes::kContentType);
  ASSERT_TRUE(content_type != nullptr);
  EXPECT_STREQ("application/json", content_type);
  // Cache-Control: no-store, private + nosniff on every daemon response.
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
  EXPECT_STREQ("nosniff",
               fetch->response_headers()->Lookup1("X-Content-Type-Options"));
  // No upstream response headers are forwarded.
  EXPECT_FALSE(fetch->response_headers()->Has("X-Upstream-Only"));
}

TEST_F(AdminSiteDaemonTest, EndpointTableMapping) {
  struct {
    const char* url_suffix;
    const char* upstream_path;
  } cases[] = {
      {"health", "/v1/health"},
      {"stats", "/v1/stats"},
      {"cooldowns", "/v1/cache/cooldowns"},
      {"cache/urls", "/v1/cache/urls"},
      {"logs", "/v1/logs"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_admin/v1/daemon/", c.url_suffix),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.url_suffix;
    EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
    EXPECT_EQ(c.upstream_path, fake_reader_->last_daemon_path_);
  }
}

TEST_F(AdminSiteDaemonTest, UnknownLeafIs404AndNotProxied) {
  // "config" is a real module admin leaf; routing through the daemon branch
  // must not fall through to it.
  for (const char* suffix : {"config", "HEALTH", "Stats"}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_admin/v1/daemon/", suffix),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << suffix;
    EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code())
        << suffix;
    EXPECT_THAT(buffer, ::testing::HasSubstr("Unknown daemon endpoint"));
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

// The shared dispatch cannot know how deep the admin path is mounted (Apache
// does not expose the <Location> prefix), so it is anchored on the tail of
// the path: a trailing slash is the SPA root, "v1/daemon/<leaf>" as the last
// three segments is the daemon proxy, and otherwise the last segment is the
// admin leaf.
TEST_F(AdminSiteDaemonTest, NestedMountServesLeaf) {
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/alt/admin/path/config",
                              RequestHeaders::kGet, &body);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("\"config\":\""));
}
TEST_F(AdminSiteDaemonTest, NestedMountServesSpaRoot) {
  GoogleString body;
  auto fetch =
      FetchAdminPage(daemon_site_.get(), "http://example.com/alt/admin/path/",
                     RequestHeaders::kGet, &body);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_STREQ("text/html; charset=utf-8", fetch->response_headers()->Lookup1(
                                               HttpAttributes::kContentType));
  EXPECT_THAT(body, ::testing::HasSubstr("<title>"));
}
TEST_F(AdminSiteDaemonTest, NestedMountReachesDaemonProxy) {
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://example.com/alt/admin/path/v1/daemon/health",
      RequestHeaders::kGet, &body);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_EQ(1, fake_reader_->call_count_);
  EXPECT_EQ("/v1/health", fake_reader_->last_daemon_path_);
}
TEST_F(AdminSiteDaemonTest, SegmentBeforeDaemonPrefixMayBePartOfTheMount) {
  // "pagespeed_admin/x" could be the mount, so this is proxied like any other
  // nested mount.
  GoogleString body;
  auto fetch =
      FetchAdminPage(daemon_site_.get(),
                     "http://example.com/pagespeed_admin/x/v1/daemon/health",
                     RequestHeaders::kGet, &body);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_EQ(1, fake_reader_->call_count_);
  EXPECT_EQ("/v1/health", fake_reader_->last_daemon_path_);
}
TEST_F(AdminSiteDaemonTest, DaemonConfigNeverServesModuleConfig) {
  // "config" is a module admin leaf, but under v1/daemon/ it is a daemon leaf:
  // unknown to the daemon table, so 404 -- never the module's config dump.
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://example.com/pagespeed_admin/v1/daemon/config",
      RequestHeaders::kGet, &body);
  EXPECT_EQ(404, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("Unknown daemon endpoint"));
  EXPECT_THAT(body, ::testing::Not(::testing::HasSubstr("\"config\":\"")));
  EXPECT_EQ(0, fake_reader_->call_count_);
}
TEST_F(AdminSiteDaemonTest, DaemonLeafWithExtraSegmentIs404) {
  for (const char* url :
       {"http://example.com/pagespeed_admin/v1/daemon/health/extra",
        "http://example.com/pagespeed_admin/v1/daemon/stats/extra"}) {
    GoogleString body;
    auto fetch =
        FetchAdminPage(daemon_site_.get(), url, RequestHeaders::kGet, &body);
    EXPECT_EQ(404, fetch->response_headers()->status_code()) << url;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, NonGetMethodsAre405) {
  for (RequestHeaders::Method method :
       {RequestHeaders::kPost, RequestHeaders::kPut, RequestHeaders::kDelete,
        RequestHeaders::kPatch}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/health",
        method, &buffer);
    ASSERT_TRUE(fetch->done());
    EXPECT_EQ(HttpStatus::kMethodNotAllowed,
              fetch->response_headers()->status_code());
    // WriteError's response carries the same hygiene headers as every other
    // admin JSON path.
    EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                       HttpAttributes::kCacheControl));
    EXPECT_STREQ("nosniff",
                 fetch->response_headers()->Lookup1("X-Content-Type-Options"));
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, HeadRequestSendsNoBody) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kHead, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(1, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, QueryParamOnLeafWithoutAllowListIsRejected) {
  // "cooldowns" allow-lists no parameters: a query on it is not ignored
  // (which would pretend a filter ran) but rejected -- and so is a
  // monitor's cache-buster on "health".
  struct {
    const char* url;
    const char* code;
    const char* param;
  } cases[] = {
      {"http://localhost/pagespeed_admin/v1/daemon/cooldowns?hostname=evil",
       "parameter_not_allowed", "hostname"},
      {"http://localhost/pagespeed_admin/v1/daemon/health?_=1790000000",
       "unknown_parameter", "_"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch =
        FetchAdminPage(daemon_site_.get(), c.url, RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.url;
    EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code())
        << c.url;
    EXPECT_THAT(buffer,
                ::testing::HasSubstr(StrCat("\"error\":\"", c.code, "\"")))
        << c.url;
    EXPECT_THAT(buffer, ::testing::HasSubstr(
                            StrCat("\"parameter\":\"", c.param, "\"")))
        << c.url;
    EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                       HttpAttributes::kCacheControl));
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, UnknownQueryParamIsRejected) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_admin/v1/daemon/health?evil=1",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer,
              ::testing::HasSubstr("\"error\":\"unknown_parameter\""));
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"parameter\":\"evil\""));
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, ParameterNamesAreExactAndCaseSensitive) {
  // Names match on their exact, escaped, case-sensitive form: a case
  // variant or an escaped spelling of a known name is an UNKNOWN name,
  // never a decoded alias of it.
  struct {
    const char* query;
    const char* code;
    const char* echoed;
  } cases[] = {
      {"url=x", "parameter_not_allowed", "url"},
      {"URL=x", "unknown_parameter", "URL"},
      {"%75rl=x", "unknown_parameter", "%75rl"},
      {"Offset=1", "unknown_parameter", "Offset"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_admin/v1/daemon/health?", c.query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.query;
    EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code())
        << c.query;
    EXPECT_THAT(buffer,
                ::testing::HasSubstr(StrCat("\"error\":\"", c.code, "\"")))
        << c.query;
    EXPECT_THAT(buffer, ::testing::HasSubstr(
                            StrCat("\"parameter\":\"", c.echoed, "\"")))
        << c.query;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, ParamErrorEchoIsSanitisedAndBounded) {
  // The offending name is client input: it is echoed at most 64 bytes
  // long, with every byte outside [A-Za-z0-9_%.-] replaced by '_'.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      StrCat("http://localhost/pagespeed_admin/v1/daemon/health?",
             GoogleString(100, 'a'), "=1"),
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_THAT(buffer, ::testing::HasSubstr(StrCat(
                          "\"parameter\":\"", GoogleString(64, 'a'), "\"")));
  EXPECT_THAT(buffer,
              ::testing::Not(::testing::HasSubstr(GoogleString(65, 'a'))));

  GoogleString buffer2;
  auto fetch2 = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_admin/v1/daemon/health?a!b*c=1",
      RequestHeaders::kGet, &buffer2);
  ASSERT_TRUE(fetch2->done());
  EXPECT_THAT(buffer2, ::testing::HasSubstr("\"parameter\":\"a_b_c\""));
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, MultiSegmentDaemonLeafReachesTheTable) {
  // "cache/select" is a real daemon route this proxy deliberately never
  // exposes (D17): a two-segment leaf that is never in the table, so it
  // must 404 as an unknown DAEMON endpoint and never fall through to the
  // module's leaf dispatch.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_admin/v1/daemon/cache/select",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer,
              ::testing::HasSubstr("Unknown daemon endpoint: cache/select"));
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, NestedMountReachesMultiSegmentDaemonLeaf) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://example.com/alt/admin/path/v1/daemon/cache/select",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer,
              ::testing::HasSubstr("Unknown daemon endpoint: cache/select"));
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, DaemonMultiSegmentLeafConfusionIs404NeverProxied) {
  // Only a table leaf is ever proxied.  The daemon's write routes, an
  // upstream path used as a leaf, a bare "cache" (a module leaf name!),
  // case and escape variants, an empty segment and remainders deeper than
  // two segments are all 404, on every method.  A HEAD response never
  // carries a body (AsyncFetch::Write drops it on a HEAD request, pinned
  // by HeadRequestSendsNoBody above), so a HEAD row asserts an empty body
  // instead of the reason-code substring the GET/POST rows carry.
  struct {
    const char* suffix;
    const char* body;  // expected body substring on GET/POST
  } cases[] = {
      {"cache/purge", "Unknown daemon endpoint: cache/purge"},
      {"cache/reprocess", "Unknown daemon endpoint: cache/reprocess"},
      {"cache/cooldowns", "Unknown daemon endpoint: cache/cooldowns"},
      // A real daemon route this proxy deliberately does not expose.
      {"cache/select", "Unknown daemon endpoint: cache/select"},
      {"cache", "Unknown daemon endpoint: cache"},
      {"Cache/urls", "Unknown daemon endpoint: Cache/urls"},
      {"cache%2Furls", "Unknown daemon endpoint: cache%2Furls"},
      {"cache//urls", "Unknown admin page: urls"},
      {"cache/urls/extra", "Unknown admin page: extra"},
      {"cache/urls/extra/more", "Unknown admin page: more"},
  };
  for (const auto& c : cases) {
    for (RequestHeaders::Method method :
         {RequestHeaders::kGet, RequestHeaders::kHead, RequestHeaders::kPost}) {
      GoogleString buffer;
      auto fetch = FetchAdminPage(
          daemon_site_.get(),
          StrCat("http://localhost/pagespeed_global_admin/v1/daemon/",
                 c.suffix),
          method, &buffer);
      ASSERT_TRUE(fetch->done()) << c.suffix;
      EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code())
          << c.suffix << " method=" << static_cast<int>(method);
      if (method == RequestHeaders::kHead) {
        EXPECT_TRUE(buffer.empty()) << c.suffix;
      } else {
        EXPECT_THAT(buffer, ::testing::HasSubstr(c.body)) << c.suffix;
        EXPECT_THAT(buffer, ::testing::Not(::testing::HasSubstr("\"config\":\"")))
            << c.suffix;
      }
    }
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, DaemonTrailingSlashServesTheShellNotTheProxy) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_STREQ("text/html; charset=utf-8", fetch->response_headers()->Lookup1(
                                               HttpAttributes::kContentType));
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, DaemonResponsesAreSameOriginOnly) {
  // A cross-origin page must not load daemon data, nor learn from
  // onload/onerror whether it exists: every daemon-proxy response, success
  // or error, carries Cross-Origin-Resource-Policy: same-origin.
  struct {
    const char* url;
    RequestHeaders::Method method;
    int status;
  } cases[] = {
      {"http://localhost/pagespeed_admin/v1/daemon/health",
       RequestHeaders::kGet, HttpStatus::kOK},
      {"http://localhost/pagespeed_admin/v1/daemon/health",
       RequestHeaders::kPost, HttpStatus::kMethodNotAllowed},
      {"http://localhost/pagespeed_admin/v1/daemon/cooldowns?hostname=x",
       RequestHeaders::kGet, HttpStatus::kBadRequest},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(daemon_site_.get(), c.url, c.method, &buffer);
    ASSERT_TRUE(fetch->done()) << c.url;
    EXPECT_EQ(c.status, fetch->response_headers()->status_code()) << c.url;
    EXPECT_STREQ("same-origin", fetch->response_headers()->Lookup1(
                                    "Cross-Origin-Resource-Policy"))
        << c.url;
  }
  AdminSite no_reader(timer(), message_handler(), nullptr);
  GoogleString body;
  auto fetch = FetchAdminPage(
      &no_reader, "http://localhost/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &body);
  EXPECT_EQ(HttpStatus::kUnavailable, fetch->response_headers()->status_code());
  EXPECT_STREQ("same-origin", fetch->response_headers()->Lookup1(
                                  "Cross-Origin-Resource-Policy"));
}

TEST_F(AdminSiteDaemonTest, UnknownDaemonLeafCarriesProxyResponseHeaders) {
  // The 404 admin_site.cc itself writes when a leaf never reaches
  // HandleRequest() (it is not in the endpoint table at all) must carry
  // the same three hygiene headers as every response AdminDaemonHandler
  // composes itself -- otherwise a cross-origin page could still probe
  // which leaf names exist via this one path.
  const char* leaves[] = {
      "config",        // single-segment, a real module admin leaf
      "cache/select",  // multi-segment, a real daemon route never exposed
  };
  for (const char* leaf : leaves) {
    for (RequestHeaders::Method method :
         {RequestHeaders::kGet, RequestHeaders::kHead}) {
      GoogleString buffer;
      auto fetch = FetchAdminPage(
          daemon_site_.get(),
          StrCat("http://localhost/pagespeed_admin/v1/daemon/", leaf), method,
          &buffer);
      ASSERT_TRUE(fetch->done()) << leaf;
      EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code())
          << leaf;
      EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                         HttpAttributes::kCacheControl))
          << leaf;
      EXPECT_STREQ("nosniff",
                   fetch->response_headers()->Lookup1("X-Content-Type-Options"))
          << leaf;
      EXPECT_STREQ("same-origin", fetch->response_headers()->Lookup1(
                                      "Cross-Origin-Resource-Policy"))
          << leaf;
      if (method == RequestHeaders::kHead) {
        // AsyncFetch::Write drops the body on a HEAD request by design
        // (pinned by HeadRequestSendsNoBody above); there is no reason-code
        // substring to assert here.
        EXPECT_TRUE(buffer.empty()) << leaf;
      } else {
        EXPECT_THAT(buffer, ::testing::HasSubstr(
                                StrCat("Unknown daemon endpoint: ", leaf)))
            << leaf;
      }
    }
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, UrlsRebuildsTheQueryInCanonicalOrder) {
  // The client sends limit first; the module rebuilds the query in its own
  // fixed order from the validated values.  The exact upstream path string
  // is the proof that no raw client bytes pass through.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/urls"
      "?limit=50&offset=100",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("/v1/cache/urls?offset=100&limit=50",
            fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, UrlsWithoutParamsForwardsNoQuery) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/urls",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("/v1/cache/urls", fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, UrlsHostnameIsValidatedAndReEncoded) {
  struct {
    const char* query;
    const char* upstream;
  } cases[] = {
      // ':' is valid input; the module's own escaping writes it as %3a.
      {"hostname=example.com:8080",
       "/v1/cache/urls?hostname=example.com%3a8080"},
      // An IPv6 literal, as GoogleUrl::Host() records it.
      {"hostname=%5B%3A%3A1%5D", "/v1/cache/urls?hostname=%5b%3a%3a1%5d"},
      // A decoded-once value: %2D is '-', a plain host character.
      {"hostname=a%2Db.test", "/v1/cache/urls?hostname=a-b.test"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/urls?",
               c.query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.query;
    EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code())
        << c.query;
    EXPECT_EQ(c.upstream, fake_reader_->last_daemon_path_) << c.query;
  }
}

TEST_F(AdminSiteDaemonTest, UrlsRejectsBadHostname) {
  for (const GoogleString& query : std::vector<GoogleString>{
           "hostname=ex%20ample", "hostname=ex+ample", "hostname=ex%0aample",
           "hostname=ex%00ample", "hostname=ex%7Fample",
           "hostname=ex%C0%AEample", "hostname=ex%26a%3Db",
           "hostname=example.com*", "hostname=",
           StrCat("hostname=", GoogleString(254, 'a'))}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/urls?",
               query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << query;
    EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code())
        << query;
    EXPECT_THAT(buffer, ::testing::HasSubstr("\"error\":\"invalid_parameter\""))
        << query;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, UrlsOffsetAndLimitBounds) {
  struct {
    const char* query;
    int expected_status;
  } cases[] = {
      {"offset=0", 200},
      {"offset=1000000000", 200},
      {"offset=%31", 200},  // decoded once: "1"
      {"offset=1000000001", 400},
      {"offset=-1", 400},
      {"offset=+1", 400},
      {"offset=1x", 400},
      {"offset=1%00", 400},
      {"offset=12345678901", 400},
      {"offset=", 400},
      {"offset", 400},  // no '=': a name without a value
      {"limit=1", 200},
      {"limit=500", 200},
      {"limit=501", 400},
      {"limit=0", 400},
      {"limit=-1", 400},
      {"limit=5.5", 400},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/urls?",
               c.query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.query;
    EXPECT_EQ(c.expected_status, fetch->response_headers()->status_code())
        << c.query;
  }
}

TEST_F(AdminSiteDaemonTest, UrlsRejectsDuplicateAndUnlistedParams) {
  struct {
    const char* query;
    const char* code;
  } cases[] = {
      {"offset=1&offset=2", "invalid_parameter"},
      {"url=%2Fx", "parameter_not_allowed"},
      {"scheme=https", "parameter_not_allowed"},
      {"since=1", "parameter_not_allowed"},
      // Not a proxy parameter at all.
      {"mask=1", "unknown_parameter"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/urls?",
               c.query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.query;
    EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code())
        << c.query;
    EXPECT_THAT(buffer,
                ::testing::HasSubstr(StrCat("\"error\":\"", c.code, "\"")))
        << c.query;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, EmptyParameterNameIsRejected) {
  // "?=x" is a single query segment with an empty name and a value "x" --
  // distinct from "?&url=", whose leading empty segment the parser drops
  // entirely before a name/value pair is ever formed, so "?=x" is the only
  // way to land an empty name in the allow-list check below.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/urls?=x",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"unknown_parameter\",\"parameter\":\"\"}", buffer);
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, UrlsIsWholeServerConsoleOnly) {
  for (RequestHeaders::Method method :
       {RequestHeaders::kGet, RequestHeaders::kHead}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        "http://example.com/pagespeed_admin/v1/daemon/cache/urls", method,
        &buffer, false /* vhost */);
    ASSERT_TRUE(fetch->done());
    EXPECT_EQ(HttpStatus::kForbidden, fetch->response_headers()->status_code())
        << static_cast<int>(method);
    if (method == RequestHeaders::kGet) {
      EXPECT_THAT(buffer, ::testing::HasSubstr(
                              "\"error\":\"whole_server_console_only\""));
    } else {
      // A HEAD request never carries a body, even for an error response,
      // and (like the GET case above) triggers no upstream call.
      EXPECT_TRUE(buffer.empty());
    }
    EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                       HttpAttributes::kCacheControl));
    EXPECT_STREQ("same-origin", fetch->response_headers()->Lookup1(
                                    "Cross-Origin-Resource-Policy"));
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, UrlsScopeGatePrecedesParamValidation) {
  // A per-vhost request gets the 403 regardless of its parameters: the gate
  // runs before validation, so nothing about the validation rules is
  // exercised cross-scope.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://example.com/pagespeed_admin/v1/daemon/cache/urls?offset=abc",
      RequestHeaders::kGet, &buffer, false /* vhost */);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kForbidden, fetch->response_headers()->status_code());
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, UrlsHeadRequestSendsNoBody) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/urls?limit=5",
      RequestHeaders::kHead, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ("/v1/cache/urls?limit=5", fake_reader_->last_daemon_path_);
}

// A real /v1/logs page: the optimizer's committed golden capture
// (pagespeed-optimizer, test/src/worker/testdata/logs_page.golden.json,
// without its trailing newline), byte for byte.
constexpr char kLogsGoldenPage[] =
    R"JSON({"entries":[{"level":"info","message":"cache flush complete","module":"worker","seq":0,"source":"worker","timestamp":1759230000000,"type":"log"},{"level":"warning","message":"origin fetch slow","module":"cache","seq":1,"source":"cache","timestamp":1759230000500,"type":"log"}],"gap":false,"more":false,"newest_seq":1,"next_since":1,"oldest_seq":0,"shed_total":0,"stream_id":"9f2c4e1a7b3d5c80"})JSON";

TEST_F(AdminSiteDaemonTest, LogsHappyPathPassthrough) {
  fake_reader_->body_ = kLogsGoldenPage;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  // The body passes through byte-for-byte: log lines are data for the
  // console to render (as plain text there), never transformed here.
  EXPECT_EQ(kLogsGoldenPage, buffer);
  EXPECT_EQ("/v1/logs", fake_reader_->last_daemon_path_);
  EXPECT_EQ(kMaxJsonResponseBytes, fake_reader_->last_max_bytes_);
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
  EXPECT_STREQ("same-origin", fetch->response_headers()->Lookup1(
                                  "Cross-Origin-Resource-Policy"));
  EXPECT_FALSE(fetch->response_headers()->Has("X-Upstream-Only"));
}

TEST_F(AdminSiteDaemonTest, LogsWithoutParamsForwardsNoQuery) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("/v1/logs", fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, LogsRebuildsTheQueryInCanonicalOrder) {
  // The client sends limit first; the module rebuilds the query in its own
  // fixed order from the validated values.  The exact upstream path string
  // is the proof that no raw client bytes pass through.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs"
      "?limit=50&since=123",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("/v1/logs?since=123&limit=50", fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, LogsSinceAndLimitBounds) {
  struct {
    const char* query;
    int expected_status;
  } cases[] = {
      {"since=0", 200},
      {"since=9223372036854775807", 200},   // INT64_MAX
      {"since=%31", 200},                    // decoded once: "1"
      {"since=9223372036854775808", 400},   // INT64_MAX + 1
      {"since=9999999999999999999", 400},   // 19 digits, above int64
      {"since=12345678901234567890", 400},  // 20 digits
      {"since=-1", 400},
      {"since=+1", 400},
      {"since=1x", 400},
      {"since=1%00", 400},
      {"since=", 400},
      {"since", 400},  // no '=': a name without a value
      {"limit=1", 200},
      {"limit=500", 200},
      {"limit=501", 400},
      {"limit=0", 400},
      {"limit=-1", 400},
      {"limit=5.5", 400},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/logs?",
               c.query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.query;
    EXPECT_EQ(c.expected_status, fetch->response_headers()->status_code())
        << c.query;
  }
  // Exactly the five 200 cases reached the daemon; every rejection
  // happened before any upstream read.
  EXPECT_EQ(5, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, LogsRejectsUnknownDuplicateAndUnlistedParams) {
  struct {
    const char* query;
    const char* code;
  } cases[] = {
      {"since=1&since=2", "invalid_parameter"},
      {"url=%2Fx", "parameter_not_allowed"},
      {"offset=1", "parameter_not_allowed"},
      // The design's draft name is not an alias for the shipped one.
      {"cursor=1", "unknown_parameter"},
      {"mask=1", "unknown_parameter"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/logs?",
               c.query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.query;
    EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code())
        << c.query;
    EXPECT_THAT(buffer,
                ::testing::HasSubstr(StrCat("\"error\":\"", c.code, "\"")))
        << c.query;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, LogsPostIs405) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kPost, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kMethodNotAllowed,
            fetch->response_headers()->status_code());
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, LogsIsWholeServerConsoleOnly) {
  for (RequestHeaders::Method method :
       {RequestHeaders::kGet, RequestHeaders::kHead}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        "http://example.com/pagespeed_admin/v1/daemon/logs", method, &buffer,
        false /* vhost */);
    ASSERT_TRUE(fetch->done());
    EXPECT_EQ(HttpStatus::kForbidden, fetch->response_headers()->status_code())
        << static_cast<int>(method);
    if (method == RequestHeaders::kGet) {
      EXPECT_THAT(buffer, ::testing::HasSubstr(
                              "\"error\":\"whole_server_console_only\""));
    } else {
      // A HEAD request never carries a body, even for an error response,
      // and (like the GET case above) triggers no upstream call.
      EXPECT_TRUE(buffer.empty());
    }
    EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                       HttpAttributes::kCacheControl));
    EXPECT_STREQ("same-origin", fetch->response_headers()->Lookup1(
                                    "Cross-Origin-Resource-Policy"));
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, LogsScopeGatePrecedesParamValidation) {
  // A per-vhost request gets the 403 regardless of its parameters: the gate
  // runs before validation, so nothing about the validation rules is
  // exercised cross-scope.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://example.com/pagespeed_admin/v1/daemon/logs?since=abc",
      RequestHeaders::kGet, &buffer, false /* vhost */);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kForbidden, fetch->response_headers()->status_code());
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, LogsCrossSiteRequestIsRefused) {
  // Whole-server leaves refuse a cross-site browser request outright; a
  // request without the header (monitoring) or a same-origin one is fine.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &buffer, true /* global */,
      nullptr /* x_requested_with */, "cross-site");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kForbidden, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer,
              ::testing::HasSubstr("\"error\":\"cross_site_request\""));
  EXPECT_EQ(0, fake_reader_->call_count_);

  GoogleString ok_buffer;
  auto ok = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &ok_buffer, true /* global */,
      nullptr /* x_requested_with */, "same-origin");
  ASSERT_TRUE(ok->done());
  EXPECT_EQ(HttpStatus::kOK, ok->response_headers()->status_code());
  EXPECT_EQ(1, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, LogsUpstream404BecomesEndpointUnsupported) {
  // The endpoint table only names paths this module knows about; a 404
  // upstream means the running optimizer predates the route.
  fake_reader_->hold_path_ = "/v1/logs";
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &buffer);
  ASSERT_FALSE(fetch->done());  // held upstream
  ASSERT_TRUE(fake_reader_->held_fetch_ != nullptr);
  fake_reader_->CompleteWithStatus(404, "{\"error\":\"not found\"}");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kNotImplemented,
            fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"endpoint_unsupported_by_daemon\"}", buffer);
}

TEST_F(AdminSiteDaemonTest, LogsOverCapReadIsResponseTooLarge) {
  // An answer over the JSON cap is not an unreachable optimizer: this leaf
  // names it, so the console does not suggest restarting a healthy daemon.
  fake_reader_->fail_ = true;
  fake_reader_->fail_reason_ = DaemonReadFailure::kTooLarge;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"response_too_large\"}", buffer);
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));

  // Every other failure is still "unreachable" on this leaf.
  for (DaemonReadFailure reason :
       {DaemonReadFailure::kDisabled, DaemonReadFailure::kTimeout,
        DaemonReadFailure::kDisconnected}) {
    fake_reader_->fail_reason_ = reason;
    GoogleString other;
    auto f = FetchAdminPage(
        daemon_site_.get(),
        "http://localhost/pagespeed_global_admin/v1/daemon/logs",
        RequestHeaders::kGet, &other);
    ASSERT_TRUE(f->done());
    EXPECT_EQ(HttpStatus::kBadGateway, f->response_headers()->status_code())
        << static_cast<int>(reason);
    EXPECT_EQ("{\"error\":\"daemon_unreachable\"}", other)
        << static_cast<int>(reason);
  }
}

TEST_F(AdminSiteDaemonTest, LogsHeadRequestSendsNoBody) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs?limit=5",
      RequestHeaders::kHead, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ("/v1/logs?limit=5", fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, LogsAdmitsOneConcurrentReadSecondIs429) {
  fake_reader_->hold_path_ = "/v1/logs";
  GoogleString b1, b2;
  auto first = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &b1);
  ASSERT_FALSE(first->done());  // held upstream
  auto second = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &b2);
  ASSERT_TRUE(second->done());
  EXPECT_EQ(429, second->response_headers()->status_code());
  EXPECT_EQ(1, fake_reader_->call_count_);

  // Completing the held read frees the slot for the next one.
  fake_reader_->CompleteHeld();
  ASSERT_TRUE(first->done());
  EXPECT_EQ(HttpStatus::kOK, first->response_headers()->status_code());
  fake_reader_->hold_path_.clear();
  GoogleString b3;
  auto third = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &b3);
  ASSERT_TRUE(third->done());
  EXPECT_EQ(HttpStatus::kOK, third->response_headers()->status_code());
}

TEST_F(AdminSiteDaemonTest, LogsTransportFailureReleasesTheSlot) {
  // A read that fails in transport must free its slot exactly once, or
  // every later poll would be a 429.
  fake_reader_->hold_path_ = "/v1/logs";
  GoogleString b1;
  auto first = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &b1);
  ASSERT_FALSE(first->done());  // held upstream
  fake_reader_->Fail(DaemonReadFailure::kDisconnected);
  ASSERT_TRUE(first->done());
  EXPECT_EQ(HttpStatus::kBadGateway, first->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"daemon_unreachable\"}", b1);

  fake_reader_->hold_path_.clear();
  GoogleString b2;
  auto second = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/logs",
      RequestHeaders::kGet, &b2);
  ASSERT_TRUE(second->done());
  EXPECT_EQ(HttpStatus::kOK, second->response_headers()->status_code());
  EXPECT_EQ(2, fake_reader_->call_count_);
}

// The query suffix naming the realistic entry every test below uses:
// https://www.example.test/hero.png, as the module records it.
constexpr char kHeroEntry[] =
    "&hostname=www.example.test&scheme=https";

TEST_F(AdminSiteDaemonTest, AlternatesUrlIsOriginFormPathReEncoded) {
  // The url is the recorded path + query.  Characters that are structure
  // in a query string ('?', '&', '=') must reach the daemon ESCAPED: the
  // module's own escaping writes lowercase %xx, whatever the client sent.
  struct {
    const char* url_param;  // as the client sends it
    const char* upstream;   // exact upstream request path
  } cases[] = {
      {"%2Fhero.png",
       "/v1/cache/alternates?url=%2fhero.png"
       "&hostname=www.example.test&scheme=https"},
      {"%2Fa%2Fb.png%3Fx%3D1%26y%3D2",
       "/v1/cache/alternates?url=%2fa%2fb.png%3fx%3d1%26y%3d2"
       "&hostname=www.example.test&scheme=https"},
      {"/a.png",  // an unescaped '/' in a query value is legal
       "/v1/cache/alternates?url=%2fa.png"
       "&hostname=www.example.test&scheme=https"},
      {"%2Fa%25zz",  // a literal '%' stays a literal '%'
       "/v1/cache/alternates?url=%2fa%25zz"
       "&hostname=www.example.test&scheme=https"},
      {"%2Fa%2Bb",  // a literal '+' is not a space
       "/v1/cache/alternates?url=%2fa%2bb"
       "&hostname=www.example.test&scheme=https"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
               "alternates?url=",
               c.url_param, kHeroEntry),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.url_param;
    EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code())
        << c.url_param;
    EXPECT_EQ(c.upstream, fake_reader_->last_daemon_path_) << c.url_param;
  }
}

TEST_F(AdminSiteDaemonTest, AlternatesCanonicalOrderWithAllParams) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/alternates"
      "?scheme=http&hostname=cdn.example.test&url=%2Fapp.js%3Fv%3D2",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("/v1/cache/alternates?url=%2fapp.js%3fv%3d2"
            "&hostname=cdn.example.test&scheme=http",
            fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, AlternatesRequiresUrlHostnameAndScheme) {
  // The daemon keys an entry on scheme + hostname + url; upstream, a
  // missing hostname would look up a host-less key and a missing scheme
  // would silently default to https.  The module requires all three.
  struct {
    const char* query;
    const char* missing;
  } cases[] = {
      {"", "url"},
      {"?hostname=www.example.test&scheme=https", "url"},
      {"?url=%2Fhero.png&scheme=https", "hostname"},
      {"?url=%2Fhero.png&hostname=www.example.test", "scheme"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
               "alternates",
               c.query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.query;
    EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code())
        << c.query;
    EXPECT_THAT(buffer, ::testing::HasSubstr("\"error\":\"missing_parameter\""))
        << c.query;
    EXPECT_THAT(buffer, ::testing::HasSubstr(
                            StrCat("\"parameter\":\"", c.missing, "\"")))
        << c.query;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, AlternatesRejectsSmuggledUrlValues) {
  const GoogleString cases[] = {
      "https%3A%2F%2Fwww.example.test%2Fhero.png",  // absolute, not a path
      "hero.png",                                    // no leading '/'
      "%2Fa%20b",                                     // space
      "%2Fa+b",                                       // '+' decodes to space
      "%2Fa%00",                                       // NUL
      "%2Fa%0Ab",                                      // control byte
      "%2Fa%7F",                                       // DEL
      "%2Fa%C0%AE",                                    // overlong UTF-8
      "%2Fa%E2%80%8B",                                 // non-ASCII
      "%2Fa%23f",                                      // '#'
      "",                                              // empty
      StrCat("%2F", GoogleString(2048, 'a')),          // 2049 bytes decoded
  };
  for (const GoogleString& url_param : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
               "alternates?url=",
               url_param, kHeroEntry),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << url_param;
    EXPECT_EQ(HttpStatus::kBadRequest,
              fetch->response_headers()->status_code())
        << url_param;
    EXPECT_THAT(buffer, ::testing::HasSubstr("\"error\":\"invalid_parameter\""))
        << url_param;
    EXPECT_THAT(buffer, ::testing::HasSubstr("\"parameter\":\"url\""))
        << url_param;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);

  // The boundary: exactly 2048 decoded bytes pass.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
             "alternates?url=%2F",
             GoogleString(2047, 'a'), kHeroEntry),
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
}

TEST_F(AdminSiteDaemonTest, AlternatesRejectsValuelessAndDuplicateUrl) {
  for (const char* query :
       {"?url&hostname=www.example.test&scheme=https",
        "?url=%2Fa.png&url=%2Fb.png&hostname=www.example.test&scheme=https",
        "?url=%2Fa.png&hostname=www.example.test&hostname=evil.test"
        "&scheme=https"}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
               "alternates",
               query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << query;
    EXPECT_EQ(HttpStatus::kBadRequest, fetch->response_headers()->status_code())
        << query;
    EXPECT_THAT(buffer, ::testing::HasSubstr("\"error\":\"invalid_parameter\""))
        << query;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, AlternatesSchemeMustBeHttpOrHttps) {
  for (const char* scheme : {"ftp", "HTTP", "https%00", ""}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
               "alternates?url=%2Fhero.png&hostname=www.example.test"
               "&scheme=",
               scheme),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << scheme;
    EXPECT_EQ(HttpStatus::kBadRequest,
              fetch->response_headers()->status_code())
        << scheme;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, AlternatesIsWholeServerConsoleOnly) {
  const GoogleString url =
      StrCat("http://example.com/pagespeed_admin/v1/daemon/cache/"
             "alternates?url=%2Fhero.png",
             kHeroEntry);
  for (RequestHeaders::Method method :
       {RequestHeaders::kGet, RequestHeaders::kHead}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(daemon_site_.get(), url, method, &buffer,
                                false /* vhost */);
    ASSERT_TRUE(fetch->done()) << url;
    EXPECT_EQ(HttpStatus::kForbidden, fetch->response_headers()->status_code())
        << url << " method=" << static_cast<int>(method);
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, AlternatesUpstream404BecomesNotInIndex) {
  // "No alternates found for URL" is the per-URL answer (every supported
  // optimizer serves the route).  The module answers with its OWN reason
  // code and drops the daemon's body: the console never decides a state
  // from text an untrusted socket peer controls.
  fake_reader_->hold_path_ =
      "/v1/cache/alternates?url=%2fnone.png"
      "&hostname=www.example.test&scheme=https";
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
             "alternates?url=%2Fnone.png",
             kHeroEntry),
      RequestHeaders::kGet, &buffer);
  ASSERT_FALSE(fetch->done());  // held upstream
  ASSERT_TRUE(fake_reader_->held_fetch_ != nullptr);
  fake_reader_->CompleteWithStatus(
      404, "{\"error\":\"No alternates found for URL Unknown\"}");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"not_in_index\"}", buffer);
  EXPECT_THAT(buffer, ::testing::Not(::testing::HasSubstr("No alternates")));
  EXPECT_STREQ("application/json", fetch->response_headers()->Lookup1(
                                       HttpAttributes::kContentType));
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
  EXPECT_FALSE(fetch->response_headers()->Has("X-Upstream-Only"));
}

TEST_F(AdminSiteDaemonTest, JsonLeavesStateTheJsonCap) {
  for (const char* url :
       {"http://localhost/pagespeed_global_admin/v1/daemon/health",
        "http://localhost/pagespeed_global_admin/v1/daemon/cache/urls"}) {
    GoogleString buffer;
    auto fetch =
        FetchAdminPage(daemon_site_.get(), url, RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << url;
    EXPECT_EQ(kMaxJsonResponseBytes, fake_reader_->last_max_bytes_) << url;
  }
}

TEST_F(AdminSiteDaemonTest, EveryReadFailureIsDaemonUnreachableOnJsonLeaves) {
  // A JSON answer over its cap is as unusable as no answer: the JSON
  // leaves keep one failure state, whatever the reason.
  fake_reader_->fail_ = true;
  for (DaemonReadFailure reason :
       {DaemonReadFailure::kDisabled, DaemonReadFailure::kTimeout,
        DaemonReadFailure::kDisconnected, DaemonReadFailure::kTooLarge}) {
    fake_reader_->fail_reason_ = reason;
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        "http://localhost/pagespeed_admin/v1/daemon/stats",
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done());
    EXPECT_EQ(HttpStatus::kBadGateway,
              fetch->response_headers()->status_code())
        << static_cast<int>(reason);
    EXPECT_EQ("{\"error\":\"daemon_unreachable\"}", buffer)
        << static_cast<int>(reason);
  }
}

// The content leaf's request for alternate 3 of the realistic entry.
constexpr char kHeroContent[] =
    "http://localhost/pagespeed_global_admin/v1/daemon/cache/content"
    "?url=%2Fhero.png&hostname=www.example.test&scheme=https&alternate_id=3";
constexpr char kHeroContentUpstream[] =
    "/v1/cache/content?url=%2fhero.png&hostname=www.example.test"
    "&scheme=https&alternate_id=3";
constexpr char kContentCsp[] =
    "sandbox; default-src 'none'; frame-ancestors 'none'";

// Every cache/content response, success or error, is inert and
// same-origin only.
void ExpectContentHardening(StringAsyncFetch& fetch,
                            const GoogleString& label) {
  ResponseHeaders* h = fetch.response_headers();
  EXPECT_STREQ(kContentCsp, h->Lookup1("Content-Security-Policy")) << label;
  EXPECT_STREQ("nosniff", h->Lookup1("X-Content-Type-Options")) << label;
  EXPECT_STREQ("same-origin", h->Lookup1("Cross-Origin-Resource-Policy"))
      << label;
  EXPECT_EQ("no-store, private",
            h->LookupJoined(HttpAttributes::kCacheControl))
      << label;
}

TEST_F(AdminSiteDaemonTest, ContentRebuildsTheQuery) {
  fake_reader_->content_type_ = "image/png";
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/content"
      "?alternate_id=3&scheme=https&hostname=www.example.test"
      "&url=%2Fhero.png%3Fv%3D2",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("/v1/cache/content?url=%2fhero.png%3fv%3d2"
            "&hostname=www.example.test&scheme=https&alternate_id=3",
            fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, ContentRequiresUrlHostnameSchemeAndAlternateId) {
  struct {
    const char* query;
    const char* missing;
  } cases[] = {
      {"", "url"},
      {"?url=%2Fhero.png&hostname=www.example.test&scheme=https",
       "alternate_id"},
      {"?alternate_id=3&url=%2Fhero.png&hostname=www.example.test",
       "scheme"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
               "content",
               c.query),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.query;
    EXPECT_EQ(HttpStatus::kBadRequest,
              fetch->response_headers()->status_code())
        << c.query;
    EXPECT_THAT(buffer, ::testing::HasSubstr(
                            StrCat("\"parameter\":\"", c.missing, "\"")))
        << c.query;
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, ContentAlternateIdBounds) {
  fake_reader_->content_type_ = "image/png";
  struct {
    const char* id;
    int expected_status;
  } cases[] = {
      {"0", 200},  {"255", 200}, {"256", 400},
      {"-1", 400}, {"abc", 400}, {"", 400},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_global_admin/v1/daemon/cache/"
               "content?url=%2Fhero.png&hostname=www.example.test"
               "&scheme=https&alternate_id=",
               c.id),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.id;
    EXPECT_EQ(c.expected_status, fetch->response_headers()->status_code())
        << c.id;
  }
}

TEST_F(AdminSiteDaemonTest, ContentIsWholeServerConsoleOnly) {
  for (RequestHeaders::Method method :
       {RequestHeaders::kGet, RequestHeaders::kHead}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        "http://example.com/pagespeed_admin/v1/daemon/cache/content"
        "?url=%2Fhero.png&hostname=www.example.test&scheme=https"
        "&alternate_id=3",
        method, &buffer, false /* vhost */);
    ASSERT_TRUE(fetch->done());
    EXPECT_EQ(HttpStatus::kForbidden, fetch->response_headers()->status_code())
        << static_cast<int>(method);
    ExpectContentHardening(*fetch, "per-vhost");
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, ContentServesAllowedImageTypes) {
  struct {
    const char* media_type;
    const char* ext;
  } cases[] = {
      {"image/png", "png"},   {"image/jpeg", "jpg"}, {"image/gif", "gif"},
      {"image/webp", "webp"}, {"image/avif", "avif"},
  };
  for (const auto& c : cases) {
    fake_reader_->content_type_ = c.media_type;
    fake_reader_->body_ = "IMAGE-BYTES";
    GoogleString buffer;
    auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                                RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.media_type;
    EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code())
        << c.media_type;
    EXPECT_EQ("IMAGE-BYTES", buffer) << c.media_type;
    EXPECT_STREQ(c.media_type, fetch->response_headers()->Lookup1(
                                   HttpAttributes::kContentType))
        << c.media_type;
    const char* disposition =
        fetch->response_headers()->Lookup1("Content-Disposition");
    ASSERT_TRUE(disposition != nullptr) << c.media_type;
    EXPECT_EQ(StrCat("inline; filename=\"variant-3.", c.ext, "\""),
              GoogleString(disposition));
    ExpectContentHardening(*fetch, c.media_type);
    EXPECT_FALSE(fetch->response_headers()->Has("X-Upstream-Only"));
  }
  EXPECT_EQ(kHeroContentUpstream, fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, ContentDropsMediaTypeParameters) {
  for (const char* upstream_type :
       {"image/png; charset=binary", " IMAGE/PNG ;x=y", "Image/Png"}) {
    fake_reader_->content_type_ = upstream_type;
    fake_reader_->body_ = "PNG-BYTES";
    GoogleString buffer;
    auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                                RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << upstream_type;
    EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code())
        << upstream_type;
    EXPECT_STREQ("image/png", fetch->response_headers()->Lookup1(
                                  HttpAttributes::kContentType))
        << upstream_type;
    EXPECT_EQ("PNG-BYTES", buffer) << upstream_type;
  }
}

TEST_F(AdminSiteDaemonTest, ContentRejectsActiveContentTypes) {
  // text/html and SVG are active content; octet-stream is the daemon's own
  // fallback; a missing, empty or duplicated Content-Type is no media type
  // at all -- none may render in the admin origin.
  struct {
    const char* first;
    const char* second;
  } cases[] = {
      {"text/html", ""},
      {"image/svg+xml", ""},
      {"application/octet-stream", ""},
      {"", ""},
      {"image/pngx", ""},
      {"image/png", "text/html"},
  };
  for (const auto& c : cases) {
    fake_reader_->content_type_ = c.first;
    fake_reader_->second_content_type_ = c.second;
    fake_reader_->body_ = "ACTIVE-BYTES";
    const GoogleString label = StrCat(c.first, " / ", c.second);
    GoogleString buffer;
    auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                                RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << label;
    EXPECT_EQ(HttpStatus::kUnsupportedMediaType,
              fetch->response_headers()->status_code())
        << label;
    EXPECT_EQ("{\"error\":\"unsupported_content_type\"}", buffer) << label;
    EXPECT_STREQ("application/json", fetch->response_headers()->Lookup1(
                                         HttpAttributes::kContentType))
        << label;
    ExpectContentHardening(*fetch, label);
  }

  // No Content-Type header at all: the daemon named no media type.
  fake_reader_->content_type_ = "image/png";
  fake_reader_->second_content_type_.clear();
  fake_reader_->omit_content_type_ = true;
  GoogleString buffer;
  auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                              RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kUnsupportedMediaType,
            fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"unsupported_content_type\"}", buffer);
  ExpectContentHardening(*fetch, "absent");
}

TEST_F(AdminSiteDaemonTest, ContentNon200NeverPassesUpstreamBytes) {
  // Only a gated 200 serves upstream bytes.  Any other upstream status is
  // answered by the module itself -- the daemon's body never reaches the
  // client, whatever its type.
  fake_reader_->content_type_ = "image/png";
  fake_reader_->body_ = "<script>alert(1)</script>";
  for (int status : {500, 206, 302, 503}) {
    fake_reader_->status_ = status;
    GoogleString buffer;
    auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                                RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << status;
    EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code())
        << status;
    EXPECT_EQ("{\"error\":\"daemon_error\"}", buffer) << status;
    EXPECT_THAT(buffer, ::testing::Not(::testing::HasSubstr("script")))
        << status;
    ExpectContentHardening(*fetch, IntegerToString(status));
  }
}

TEST_F(AdminSiteDaemonTest, ContentErrorResponsesCarryHardeningHeaders) {
  struct {
    const char* label;
    const char* url;
    RequestHeaders::Method method;
    int status;
  } cases[] = {
      {"bad id",
       "http://localhost/pagespeed_global_admin/v1/daemon/cache/content"
       "?url=%2Fhero.png&hostname=www.example.test&scheme=https"
       "&alternate_id=999",
       RequestHeaders::kGet, HttpStatus::kBadRequest},
      {"missing param",
       "http://localhost/pagespeed_global_admin/v1/daemon/cache/content"
       "?url=%2Fhero.png",
       RequestHeaders::kGet, HttpStatus::kBadRequest},
      {"unknown param",
       "http://localhost/pagespeed_global_admin/v1/daemon/cache/content"
       "?evil=1",
       RequestHeaders::kGet, HttpStatus::kBadRequest},
      {"post", kHeroContent, RequestHeaders::kPost,
       HttpStatus::kMethodNotAllowed},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(daemon_site_.get(), c.url, c.method, &buffer);
    ASSERT_TRUE(fetch->done()) << c.label;
    EXPECT_EQ(c.status, fetch->response_headers()->status_code()) << c.label;
    ExpectContentHardening(*fetch, c.label);
  }
  EXPECT_EQ(0, fake_reader_->call_count_);

  // The daemon's per-URL 404 and a transport failure, synchronously.
  fake_reader_->status_ = 404;
  GoogleString buffer404;
  auto not_found = FetchAdminPage(daemon_site_.get(), kHeroContent,
                                  RequestHeaders::kGet, &buffer404);
  EXPECT_EQ(HttpStatus::kNotFound, not_found->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"not_in_index\"}", buffer404);
  ExpectContentHardening(*not_found, "404");

  fake_reader_->fail_ = true;
  GoogleString buffer502;
  auto failed = FetchAdminPage(daemon_site_.get(), kHeroContent,
                               RequestHeaders::kGet, &buffer502);
  EXPECT_EQ(HttpStatus::kBadGateway, failed->response_headers()->status_code());
  ExpectContentHardening(*failed, "502");

  // No transport at all.
  AdminSite no_reader(timer(), message_handler(), nullptr);
  GoogleString buffer503;
  auto unconfigured = FetchAdminPage(&no_reader, kHeroContent,
                                     RequestHeaders::kGet, &buffer503);
  EXPECT_EQ(HttpStatus::kUnavailable,
            unconfigured->response_headers()->status_code());
  ExpectContentHardening(*unconfigured, "503");
}

TEST_F(AdminSiteDaemonTest, ContentOverTheCapIsContentTooLarge) {
  // The reader observed a body over the content cap: a JSON error, never
  // the bytes that arrived before it.
  fake_reader_->hold_path_ = kHeroContentUpstream;
  GoogleString buffer;
  auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                              RequestHeaders::kGet, &buffer);
  ASSERT_FALSE(fetch->done());  // held upstream
  ASSERT_TRUE(fake_reader_->held_fetch_ != nullptr);
  fake_reader_->held_fetch_->response_headers()->set_status_code(
      HttpStatus::kOK);
  fake_reader_->held_fetch_->response_headers()->Add(
      HttpAttributes::kContentType, "image/png");
  fake_reader_->held_fetch_->Write("partial-bytes", message_handler());
  fake_reader_->Fail(DaemonReadFailure::kTooLarge);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"content_too_large\"}", buffer);
  ExpectContentHardening(*fetch, "too large");
}

TEST_F(AdminSiteDaemonTest, ContentOrdinaryFailuresStayDaemonUnreachable) {
  // A status (and even some bytes) can precede an ordinary failure: only
  // the reader's observed kTooLarge means "too large".
  struct {
    DaemonReadFailure reason;
    int status;          // set before the failure (0: none)
    const char* bytes;   // written before the failure
  } cases[] = {
      {DaemonReadFailure::kTimeout, HttpStatus::kOK, "partial-bytes"},
      {DaemonReadFailure::kDisconnected, HttpStatus::kOK, "partial-bytes"},
      {DaemonReadFailure::kDisconnected, 0, ""},
      {DaemonReadFailure::kDisabled, HttpStatus::kBadGateway, ""},
  };
  fake_reader_->hold_path_ = kHeroContentUpstream;
  for (const auto& c : cases) {
    const GoogleString label = IntegerToString(static_cast<int>(c.reason));
    GoogleString buffer;
    auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                                RequestHeaders::kGet, &buffer);
    ASSERT_FALSE(fetch->done()) << label;
    if (c.status != 0) {
      fake_reader_->held_fetch_->response_headers()->set_status_code(c.status);
    }
    if (*c.bytes != '\0') {
      fake_reader_->held_fetch_->Write(c.bytes, message_handler());
    }
    fake_reader_->Fail(c.reason);
    ASSERT_TRUE(fetch->done()) << label;
    EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code())
        << label;
    EXPECT_EQ("{\"error\":\"daemon_unreachable\"}", buffer) << label;
  }
}

TEST_F(AdminSiteDaemonTest, ContentHeadForwardsHeadersNoBody) {
  fake_reader_->content_type_ = "image/webp";
  fake_reader_->body_ = "WEBP-BYTES";
  GoogleString buffer;
  auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                              RequestHeaders::kHead, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_TRUE(buffer.empty());
  EXPECT_STREQ("image/webp", fetch->response_headers()->Lookup1(
                                 HttpAttributes::kContentType));
  ExpectContentHardening(*fetch, "head");
}

TEST_F(AdminSiteDaemonTest, ContentUsesTheContentCapJsonLeavesTheJsonCap) {
  fake_reader_->content_type_ = "image/png";
  GoogleString buffer;
  auto fetch = FetchAdminPage(daemon_site_.get(), kHeroContent,
                              RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(kMaxContentResponseBytes, fake_reader_->last_max_bytes_);
  GoogleString buffer2;
  auto fetch2 = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/alternates"
      "?url=%2Fhero.png&hostname=www.example.test&scheme=https",
      RequestHeaders::kGet, &buffer2);
  ASSERT_TRUE(fetch2->done());
  EXPECT_EQ(kMaxJsonResponseBytes, fake_reader_->last_max_bytes_);
}

TEST_F(AdminSiteDaemonTest, ContentAdmitsTwoConcurrentReadsThirdIs429) {
  // The console loads variant previews in parallel: the content leaf
  // admits two concurrent reads (bounding the buffered bytes to 2 x the
  // content cap per process); a JSON leaf keeps its single slot.
  fake_reader_->hold_path_ = kHeroContentUpstream;
  fake_reader_->content_type_ = "image/png";
  fake_reader_->body_ = "PNG-BYTES";
  GoogleString b1, b2, b3;
  auto first = FetchAdminPage(daemon_site_.get(), kHeroContent,
                              RequestHeaders::kGet, &b1);
  auto second = FetchAdminPage(daemon_site_.get(), kHeroContent,
                               RequestHeaders::kGet, &b2);
  ASSERT_FALSE(first->done());
  ASSERT_FALSE(second->done());
  auto third = FetchAdminPage(daemon_site_.get(), kHeroContent,
                              RequestHeaders::kGet, &b3);
  ASSERT_TRUE(third->done());
  EXPECT_EQ(429, third->response_headers()->status_code());
  ExpectContentHardening(*third, "429");
  EXPECT_EQ(2, fake_reader_->call_count_);

  const char kAlternates[] =
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/alternates"
      "?url=%2Fhero.png&hostname=www.example.test&scheme=https";
  fake_reader_->hold_path_ =
      "/v1/cache/alternates?url=%2fhero.png&hostname=www.example.test"
      "&scheme=https";
  GoogleString a1, a2;
  auto json_first = FetchAdminPage(daemon_site_.get(), kAlternates,
                                   RequestHeaders::kGet, &a1);
  ASSERT_FALSE(json_first->done());
  auto json_second = FetchAdminPage(daemon_site_.get(), kAlternates,
                                    RequestHeaders::kGet, &a2);
  ASSERT_TRUE(json_second->done());
  EXPECT_EQ(429, json_second->response_headers()->status_code());

  // Every held read completes and frees its slot.
  fake_reader_->CompleteAllHeld();
  ASSERT_TRUE(first->done());
  ASSERT_TRUE(second->done());
  ASSERT_TRUE(json_first->done());
  EXPECT_EQ(HttpStatus::kOK, first->response_headers()->status_code());
  EXPECT_EQ("PNG-BYTES", b1);
  EXPECT_EQ("PNG-BYTES", b2);
  fake_reader_->hold_path_.clear();
  GoogleString b4;
  auto fourth = FetchAdminPage(daemon_site_.get(), kHeroContent,
                               RequestHeaders::kGet, &b4);
  ASSERT_TRUE(fourth->done());
  EXPECT_EQ(HttpStatus::kOK, fourth->response_headers()->status_code());
}

TEST_F(AdminSiteDaemonTest, ContentSlotsAreSharedAcrossAdminSites) {
  // There is one AdminSite (and daemon handler) per server context, i.e.
  // per virtual host.  The two content slots bound the buffered bytes per
  // PROCESS, so a second site cannot open two more while the first holds
  // both.
  auto* other_reader = new FakeDaemonReader(message_handler());
  AdminSite other_site(timer(), message_handler(), other_reader);  // owns it
  other_reader->content_type_ = "image/png";
  other_reader->body_ = "OTHER-BYTES";

  fake_reader_->hold_path_ = kHeroContentUpstream;
  fake_reader_->content_type_ = "image/png";
  fake_reader_->body_ = "PNG-BYTES";
  GoogleString b1, b2;
  auto first = FetchAdminPage(daemon_site_.get(), kHeroContent,
                              RequestHeaders::kGet, &b1);
  auto second = FetchAdminPage(daemon_site_.get(), kHeroContent,
                               RequestHeaders::kGet, &b2);
  ASSERT_FALSE(first->done());
  ASSERT_FALSE(second->done());
  EXPECT_EQ(2, AdminDaemonHandler::ContentReadsInFlightForTesting());

  // The other site's own handler has no read in flight, yet the process
  // has no content slot left.
  GoogleString b3;
  auto other = FetchAdminPage(&other_site, kHeroContent,
                              RequestHeaders::kGet, &b3);
  ASSERT_TRUE(other->done());
  EXPECT_EQ(429, other->response_headers()->status_code());
  ExpectContentHardening(*other, "429 other site");
  EXPECT_EQ(0, other_reader->call_count_);

  // The JSON leaves keep per-handler slots: unaffected.
  GoogleString a1;
  auto json = FetchAdminPage(
      &other_site,
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/alternates"
      "?url=%2Fhero.png&hostname=www.example.test&scheme=https",
      RequestHeaders::kGet, &a1);
  ASSERT_TRUE(json->done());
  EXPECT_EQ(HttpStatus::kOK, json->response_headers()->status_code());

  // Completing the held reads frees the process-wide slots.
  fake_reader_->CompleteAllHeld();
  other_reader->CompleteAllHeld();
  ASSERT_TRUE(first->done());
  ASSERT_TRUE(second->done());
  EXPECT_EQ(0, AdminDaemonHandler::ContentReadsInFlightForTesting());

  // The earlier 429 on other_site must have rolled back the per-handler
  // slot it took before the process-wide slot refused it -- not leaked
  // it.  Prove that by having other_site take BOTH of its own slots at
  // once: if the per-vhost rollback were lost, the earlier 429 would
  // have left other_site's per-handler counter at 1 (of 2), and only
  // one of these two reads would be admitted.
  other_reader->hold_path_ = kHeroContentUpstream;
  GoogleString b4, b5;
  auto again = FetchAdminPage(&other_site, kHeroContent,
                              RequestHeaders::kGet, &b4);
  auto again2 = FetchAdminPage(&other_site, kHeroContent,
                               RequestHeaders::kGet, &b5);
  // EXPECT, not ASSERT: even if a slot leaked and one of these came back
  // 429 immediately, CompleteAllHeld() below must still run so the held
  // one (if any) does not linger into later tests' leak checks.
  EXPECT_FALSE(again->done());
  EXPECT_FALSE(again2->done());
  other_reader->CompleteAllHeld();
  ASSERT_TRUE(again->done());
  ASSERT_TRUE(again2->done());
  EXPECT_EQ(HttpStatus::kOK, again->response_headers()->status_code());
  EXPECT_EQ(HttpStatus::kOK, again2->response_headers()->status_code());
  EXPECT_EQ("OTHER-BYTES", b4);
  EXPECT_EQ("OTHER-BYTES", b5);
  EXPECT_EQ(0, AdminDaemonHandler::ContentReadsInFlightForTesting());
}

TEST_F(AdminSiteDaemonTest, CrossSiteSecFetchSiteIsRefusedOnWholeServerLeaves) {
  // The three leaves whose data spans every virtual host also refuse a
  // cross-site browser request outright, before any parameter is looked
  // at or the daemon is contacted.
  const GoogleString alternates_url = StrCat(
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/"
      "alternates?url=%2Fhero.png",
      kHeroEntry);
  struct {
    const char* label;
    const char* url;
    bool content_leaf;
  } leaves[] = {
      {"urls", "http://localhost/pagespeed_global_admin/v1/daemon/cache/urls",
       false},
      {"alternates", alternates_url.c_str(), false},
      {"content", kHeroContent, true},
  };
  for (const auto& leaf : leaves) {
    for (const char* site : {"cross-site", "same-site"}) {
      for (RequestHeaders::Method method :
           {RequestHeaders::kGet, RequestHeaders::kHead}) {
        const GoogleString label =
            StrCat(leaf.label, " ", site, " ",
                   IntegerToString(static_cast<int>(method)));
        GoogleString buffer;
        auto fetch = FetchAdminPage(daemon_site_.get(), leaf.url, method,
                                    &buffer, true /* is_global */,
                                    nullptr /* x_requested_with */, site);
        ASSERT_TRUE(fetch->done()) << label;
        EXPECT_EQ(HttpStatus::kForbidden,
                  fetch->response_headers()->status_code())
            << label;
        if (method == RequestHeaders::kGet) {
          EXPECT_EQ("{\"error\":\"cross_site_request\"}", buffer) << label;
        } else {
          // A HEAD request never carries a body, even for an error
          // response.
          EXPECT_TRUE(buffer.empty()) << label;
        }
        if (leaf.content_leaf) {
          ExpectContentHardening(*fetch, label);
        } else {
          EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                             HttpAttributes::kCacheControl))
              << label;
          EXPECT_STREQ("nosniff", fetch->response_headers()->Lookup1(
                                      "X-Content-Type-Options"))
              << label;
          EXPECT_STREQ("same-origin", fetch->response_headers()->Lookup1(
                                          "Cross-Origin-Resource-Policy"))
              << label;
        }
      }
    }
  }
  // No upstream call on refusal, for any of the leaves/values/methods above.
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest,
       SameOriginNoneOrAbsentSecFetchSiteIsAllowedOnWholeServerLeaves) {
  // Same-origin and top-level navigations (Sec-Fetch-Site: same-origin or
  // none) are allowed, and so is a request with no Sec-Fetch-Site header
  // at all (a non-browser client, e.g. monitoring).
  fake_reader_->content_type_ = "image/png";
  fake_reader_->body_ = "OK-BYTES";
  const GoogleString alternates_url = StrCat(
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/"
      "alternates?url=%2Fhero.png",
      kHeroEntry);
  const char* urls[] = {
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/urls",
      alternates_url.c_str(),
      kHeroContent,
  };
  for (const char* url : urls) {
    for (const char* site :
         {"same-origin", "none", static_cast<const char*>(nullptr)}) {
      for (RequestHeaders::Method method :
           {RequestHeaders::kGet, RequestHeaders::kHead}) {
        const GoogleString label =
            StrCat(url, " ", site == nullptr ? "absent" : site, " ",
                   IntegerToString(static_cast<int>(method)));
        GoogleString buffer;
        auto fetch = FetchAdminPage(daemon_site_.get(), url, method, &buffer,
                                    true /* is_global */,
                                    nullptr /* x_requested_with */, site);
        ASSERT_TRUE(fetch->done()) << label;
        EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code())
            << label;
      }
    }
  }
}

TEST_F(AdminSiteDaemonTest, SecFetchSiteGateDoesNotApplyToPerVhostLeaves) {
  // health/stats/cooldowns are unaffected: the cross-site gate only
  // applies to the three leaves whose data spans every virtual host.
  for (const char* suffix : {"health", "stats", "cooldowns"}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_admin/v1/daemon/", suffix),
        RequestHeaders::kGet, &buffer, true /* is_global */,
        nullptr /* x_requested_with */, "cross-site");
    ASSERT_TRUE(fetch->done()) << suffix;
    EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code())
        << suffix;
  }
}

TEST_F(AdminSiteDaemonTest, DotSegmentsAreResolvedBeforeDispatch) {
  // GoogleUrl resolves "." and ".." before the dispatch sees the path, so
  // a dot segment can only ever land on a path a client could name
  // directly: "cache/./urls" IS the table leaf (proxied to its own
  // upstream path), and "cache/../../config" climbs out of v1/daemon to
  // the module's own config leaf -- never to the daemon.
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/./urls",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("/v1/cache/urls", fake_reader_->last_daemon_path_);
  EXPECT_EQ(1, fake_reader_->call_count_);

  GoogleString buffer2;
  auto fetch2 = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cache/../../config",
      RequestHeaders::kGet, &buffer2);
  ASSERT_TRUE(fetch2->done());
  EXPECT_THAT(buffer2, ::testing::HasSubstr("\"config\":\""));
  EXPECT_EQ(1, fake_reader_->call_count_);  // unchanged: not proxied
}

TEST_F(AdminSiteDaemonTest, NoReaderMeansDaemonUnreachable) {
  // The fixture's admin_site_ has a null DaemonReader (this port/context has
  // no daemon transport, or the socket path is empty): no transport to fail,
  // so this is "not configured" (503), not "unreachable" (502).
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      admin_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kUnavailable, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer, ::testing::HasSubstr("daemon_not_configured"));
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
  EXPECT_STREQ("nosniff",
               fetch->response_headers()->Lookup1("X-Content-Type-Options"));
}

TEST_F(AdminSiteDaemonTest, UpstreamFailureIs502) {
  fake_reader_->fail_ = true;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/stats",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer, ::testing::HasSubstr("daemon_unreachable"));
}

TEST_F(AdminSiteDaemonTest, NoTransportIs503NotConfigured) {
  AdminSite no_reader(timer(), message_handler(), nullptr);
  GoogleString body;
  auto fetch = FetchAdminPage(
      &no_reader, "http://example.com/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &body);
  EXPECT_EQ(503, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("daemon_not_configured"));
}

TEST_F(AdminSiteDaemonTest, TransportFailureIs502Unreachable) {
  // Held (not auto-completed) so fake_reader_->Fail() below completes it as
  // a genuine transport failure, distinct from UpstreamFailureIs502 above
  // (which fails synchronously inside Get()).
  fake_reader_->hold_path_ = "/v1/health";
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://example.com/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &body);
  fake_reader_->Fail();  // completes the pending fetch with success=false
  EXPECT_EQ(502, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("daemon_unreachable"));
}

TEST_F(AdminSiteDaemonTest, Upstream404BecomesEndpointUnsupported) {
  fake_reader_->hold_path_ = "/v1/cache/cooldowns";
  GoogleString body;
  auto fetch =
      FetchAdminPage(daemon_site_.get(),
                     "http://example.com/pagespeed_admin/v1/daemon/cooldowns",
                     RequestHeaders::kGet, &body);
  fake_reader_->CompleteWithStatus(404, "{\"error\":\"not found\"}");
  EXPECT_EQ(501, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("endpoint_unsupported_by_daemon"));
}

TEST_F(AdminSiteDaemonTest, UpstreamStatusAndBodyPassThroughContentTypePinned) {
  // Status and body pass through, but the Content-Type is pinned to
  // application/json: the socket peer is untrusted, and every daemon endpoint
  // serves JSON.  A spoofed text/html upstream must not render in the admin
  // origin.
  fake_reader_->status_ = 503;
  fake_reader_->content_type_ = "text/html";
  fake_reader_->body_ = "<script>alert(1)</script>";
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/stats",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(503, fetch->response_headers()->status_code());
  EXPECT_EQ("<script>alert(1)</script>", buffer);
  const char* content_type =
      fetch->response_headers()->Lookup1(HttpAttributes::kContentType);
  ASSERT_TRUE(content_type != nullptr);
  EXPECT_STREQ("application/json", content_type);
}

TEST_F(AdminSiteDaemonTest, ConcurrentRequestOnSameEndpointGets429) {
  fake_reader_->hold_path_ = "/v1/health";
  GoogleString buffer1, buffer2;
  auto first = FetchAdminPage(
      daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &buffer1);
  ASSERT_FALSE(first->done());  // held upstream
  auto second = FetchAdminPage(
      daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &buffer2);
  ASSERT_TRUE(second->done());
  EXPECT_EQ(429, second->response_headers()->status_code());
  // Release the held upstream read; the first request completes.
  ASSERT_TRUE(fake_reader_->held_fetch_ != nullptr);
  fake_reader_->CompleteHeld();
  ASSERT_TRUE(first->done());
  EXPECT_EQ(HttpStatus::kOK, first->response_headers()->status_code());
  EXPECT_EQ("{\"status\":\"ok\"}", buffer1);
}

TEST_F(AdminSiteDaemonTest, PerEndpointSlotsDoNotBlockEachOther) {
  fake_reader_->hold_path_ = "/v1/health";
  GoogleString buffer1, buffer2;
  auto first = FetchAdminPage(
      daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &buffer1);
  ASSERT_FALSE(first->done());  // held upstream
  // A different endpoint has its own slot and answers immediately.
  auto second = FetchAdminPage(
      daemon_site_.get(), "http://localhost/pagespeed_admin/v1/daemon/stats",
      RequestHeaders::kGet, &buffer2);
  ASSERT_TRUE(second->done());
  EXPECT_EQ(HttpStatus::kOK, second->response_headers()->status_code());
  ASSERT_TRUE(fake_reader_->held_fetch_ != nullptr);
  fake_reader_->CompleteHeld();
  ASSERT_TRUE(first->done());
}

// The /v1/license/* admin API is gone. Its former routes dispatch on their
// last segment, none of which is an admin leaf, so they answer 404 -- never a
// config dump, never a 5xx -- and a POST no longer meets a CSRF gate (403).
// The daemon proxy next to it is untouched.
TEST_F(AdminSiteDaemonTest, RetiredLicenseRoutesAnswer404) {
  for (const char* leaf : {"status", "apply", "activate", "consent"}) {
    for (RequestHeaders::Method method :
         {RequestHeaders::kGet, RequestHeaders::kPost}) {
      GoogleString buffer;
      auto fetch = FetchAdminPage(
          daemon_site_.get(),
          StrCat("http://localhost/pagespeed_global_admin/v1/license/", leaf),
          method, &buffer);
      ASSERT_TRUE(fetch->done()) << leaf;
      EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code())
          << leaf << " method=" << static_cast<int>(method);
      EXPECT_THAT(buffer, ::testing::HasSubstr("\"success\":false")) << leaf;
      EXPECT_THAT(buffer, ::testing::Not(::testing::HasSubstr("CSRF"))) << leaf;
      EXPECT_EQ(0, fake_reader_->call_count_) << leaf;
    }
  }
}

TEST_F(AdminSiteDaemonTest, PurgeWithGetIs405) {
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://example.com/pagespeed_admin/cache?purge=*",
      RequestHeaders::kGet, &body);
  EXPECT_EQ(405, fetch->response_headers()->status_code());
  EXPECT_STREQ("POST",
               fetch->response_headers()->Lookup1(HttpAttributes::kAllow));
  EXPECT_THAT(body, ::testing::HasSubstr("\"success\":false"));
}

TEST_F(AdminSiteDaemonTest, PurgeRefusesCrossOriginPost) {
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://example.com/pagespeed_admin/cache?purge=*",
      RequestHeaders::kPost, &body);
  EXPECT_EQ(403, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("cross-origin"));
}

TEST_F(AdminSiteDaemonTest, DeleteWithGetIs405) {
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/"
                              "cache?url=http://example.com/x.css&Delete",
                              RequestHeaders::kGet, &body);
  EXPECT_EQ(405, fetch->response_headers()->status_code());
  EXPECT_STREQ("POST",
               fetch->response_headers()->Lookup1(HttpAttributes::kAllow));
  EXPECT_THAT(body, ::testing::HasSubstr("\"success\":false"));
}

TEST_F(AdminSiteDaemonTest, DeleteRefusesCrossOriginPost) {
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/"
                              "cache?url=http://example.com/x.css&Delete",
                              RequestHeaders::kPost, &body);
  EXPECT_EQ(403, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("cross-origin"));
}

TEST_F(AdminSiteDaemonTest, GlobalPurgeOnlyFromGlobalAdmin) {
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(), "http://example.com/pagespeed_admin/cache?purge=*",
      RequestHeaders::kPost, &body, false /* vhost */, "XMLHttpRequest");
  EXPECT_EQ(403, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("global admin"));
}

TEST_F(AdminSiteDaemonTest, GlobalPurgeAllAcceptedFromGlobalAdmin) {
  // A same-origin POST of purge=* on the global admin passes the method,
  // origin and scope gates.  Purge is disabled in the test options, so it
  // lands on the "not enabled" answer (200) -- never a 403 or 405.
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://example.com/pagespeed_global_admin/cache?purge=*",
      RequestHeaders::kPost, &body, true /* global */, "XMLHttpRequest");
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("Purging not enabled"));
}

TEST_F(AdminSiteDaemonTest, VhostPurgeWholeCacheSpellingsAreGlobalOnly) {
  // A per-vhost console refuses any target that would act as a whole-cache
  // purge, however spelled: a full URL on its own host, a path under it, a
  // relative reference, or a percent-encoded spelling.  The scope gates run
  // ahead of the purge-enabled check, so each one is a 403 (not the 200
  // "not enabled" answer) even though purging is off in the test options.
  for (const char* target :
       {"http://example.com/*", "http://example.com/x/*", "/x/*",
        "http%3A%2F%2Fexample.com%2Fy%2F%2A", "%2A"}) {
    GoogleString body;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://example.com/pagespeed_admin/cache?purge=", target),
        RequestHeaders::kPost, &body, false /* vhost */, "XMLHttpRequest");
    EXPECT_EQ(403, fetch->response_headers()->status_code()) << target;
    EXPECT_THAT(body, ::testing::HasSubstr("global admin")) << target;
  }
}

TEST_F(AdminSiteDaemonTest, VhostPurgeOwnHostSingleUrlStillReachesPurgeGate) {
  // An ordinary purge of a same-host URL still passes the scope gates, in
  // both spellings; with purging disabled it lands on the "not enabled"
  // answer (200), never a 403.
  for (const char* target : {"http://example.com/x.css", "/x.css"}) {
    GoogleString body;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://example.com/pagespeed_admin/cache?purge=", target),
        RequestHeaders::kPost, &body, false /* vhost */, "XMLHttpRequest");
    EXPECT_EQ(200, fetch->response_headers()->status_code()) << target;
    EXPECT_THAT(body, ::testing::HasSubstr("Purging not enabled")) << target;
  }
}

TEST_F(AdminSiteDaemonTest, GlobalPurgeWildcardUrlAcceptedFromGlobalAdmin) {
  // The whole-server console's behaviour is unchanged: a URL spelling of the
  // whole-cache purge passes its gates and, with purging disabled in the
  // test options, lands on the "not enabled" answer (200).
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://example.com/pagespeed_global_admin/cache?purge="
      "http://example.com/x/*",
      RequestHeaders::kPost, &body, true /* global */, "XMLHttpRequest");
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("Purging not enabled"));
}

TEST_F(AdminSiteDaemonTest, VhostPurgeRejectsForeignHost) {
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/"
                              "cache?purge=http://other.example.org/x.css",
                              RequestHeaders::kPost, &body, false /* vhost */,
                              "XMLHttpRequest");
  EXPECT_EQ(403, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("another host"));
}

TEST_F(AdminSiteDaemonTest, VhostLookupRejectsForeignHost) {
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/"
                              "cache?url=http://other.example.org/x.css",
                              RequestHeaders::kGet, &body, false /* vhost */);
  EXPECT_EQ(403, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("another host"));
}

TEST_F(AdminSiteDaemonTest, SameOriginPostReachesThePurgeGate) {
  // Purge is disabled in the test options, so a request that passes the
  // method, origin and scope gates lands on the "not enabled" answer (200).
  GoogleString body;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://example.com/pagespeed_admin/cache?purge=http://example.com/x.css",
      RequestHeaders::kPost, &body, false /* vhost */, "XMLHttpRequest");
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("Purging not enabled"));
}

TEST_F(AdminSiteDaemonTest, ConfigLeafCarriesScopeHostAndEffectiveOptions) {
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/config",
                              RequestHeaders::kGet, &body, false /* vhost */);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("\"scope\":\"vhost\""));
  EXPECT_THAT(body, ::testing::HasSubstr("\"effective_config\":\""));
  EXPECT_THAT(body, ::testing::HasSubstr("\"config\":\""));
  auto global = FetchAdminPage(
      daemon_site_.get(), "http://example.com/pagespeed_global_admin/config",
      RequestHeaders::kGet, &body, true /* global */);
  EXPECT_THAT(body, ::testing::HasSubstr("\"scope\":\"global\""));
}

TEST_F(AdminSiteDaemonTest, StatsJsonCarriesScopeHostTimestampAndGauges) {
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/stats_json",
                              RequestHeaders::kGet, &body, false /* vhost */);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("\"scope\": \"vhost\""));
  EXPECT_THAT(body, ::testing::HasSubstr("\"host\": \""));
  EXPECT_THAT(body, ::testing::HasSubstr("\"timestamp_ms\": "));
  // Kept for compatibility: scripts outside the console read this endpoint.
  EXPECT_THAT(body, ::testing::HasSubstr("\"maxlength\": "));
  EXPECT_THAT(body, ::testing::HasSubstr("\"gauges\": ["));

  body.clear();
  auto global = FetchAdminPage(
      daemon_site_.get(),
      "http://example.com/pagespeed_global_admin/stats_json",
      RequestHeaders::kGet, &body, true /* global */);
  EXPECT_THAT(body, ::testing::HasSubstr("\"scope\": \"global\""));
}

TEST_F(AdminSiteDaemonTest, StatsJsonGaugesListsUpDownCounters) {
  factory()->statistics()->AddUpDownCounter("test_gauge_counter")->Add(1);
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/stats_json",
                              RequestHeaders::kGet, &body, false /* vhost */);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  const size_t gauges_at = body.find("\"gauges\": [");
  ASSERT_NE(gauges_at, GoogleString::npos);
  EXPECT_NE(body.find("\"test_gauge_counter\"", gauges_at), GoogleString::npos);
}

TEST_F(AdminSiteDaemonTest, DumpJsonWithOnlyUpDownCountersStaysValidJson) {
  // Regression: with no plain variables, the first up/down entry used to be
  // written with a leading comma ("variables": {,"x": ...}).
  SimpleStats stats(factory()->thread_system());
  stats.AddUpDownCounter("inflight")->Add(3);
  GoogleString out;
  StringWriter writer(&out);
  stats.DumpJson("vhost", "h:1", 1234, &writer, message_handler());
  EXPECT_THAT(out, ::testing::HasSubstr("\"variables\": {\"inflight\": 3"));
  EXPECT_THAT(out, ::testing::HasSubstr("\"gauges\": [\"inflight\"]"));
  EXPECT_THAT(out, ::testing::HasSubstr("\"maxlength\": "));
}

TEST_F(AdminSiteDaemonTest, DumpJsonWithNoCountersAtAllStaysValidJson) {
  SimpleStats stats(factory()->thread_system());
  GoogleString out;
  StringWriter writer(&out);
  stats.DumpJson("global", "h:1", 1234, &writer, message_handler());
  EXPECT_THAT(out, ::testing::HasSubstr("\"variables\": {}"));
  EXPECT_THAT(out, ::testing::HasSubstr("\"maxlength\": 0"));
  EXPECT_THAT(out, ::testing::HasSubstr("\"gauges\": []"));
}

TEST_F(AdminSiteDaemonTest, StatsJsonCarriesTimedVariables) {
  TimedVariable* timed = factory()->statistics()->AddTimedVariable(
      "test_timed_counter", Statistics::kDefaultGroup);
  timed->IncBy(7);
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/stats_json",
                              RequestHeaders::kGet, &body, false /* vhost */);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("\"timed_variables\": {"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"test_timed_counter\":"));
}

TEST_F(AdminSiteDaemonTest, HistogramsLeafIsStructuredJson) {
  Histogram* h = factory()->statistics()->AddHistogram("a1_latency_ms");
  h->Add(10);
  h->Add(20);
  h->Add(30);
  // A second histogram with enough samples to clear
  // Histogram::kMinSamplesForPercentiles: its percentile estimates are real
  // numbers rather than null.
  Histogram* h6 = factory()->statistics()->AddHistogram("a1_latency_ms_6");
  h6->Add(1);
  h6->Add(2);
  h6->Add(3);
  h6->Add(4);
  h6->Add(5);
  h6->Add(6);

  GoogleString body;
  auto fetch =
      FetchAdminPage(daemon_site_.get(),
                     "http://example.com/pagespeed_global_admin/histograms",
                     RequestHeaders::kGet, &body);
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("\"name\":\"a1_latency_ms\""));
  EXPECT_THAT(body, ::testing::HasSubstr("\"count\":3"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"buckets\":["));
  EXPECT_THAT(body, ::testing::Not(::testing::HasSubstr("<table")));

  // Too few samples (< Histogram::kMinSamplesForPercentiles) to estimate a
  // percentile: median/p90/p95/p99 come back JSON null rather than a
  // meaningless number (avg/stddev/min/max are unaffected).
  EXPECT_THAT(body, ::testing::HasSubstr("\"median\":null"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"p90\":null"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"p95\":null"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"p99\":null"));

  // The 6-sample histogram clears the threshold: its median/p90 are real
  // numbers, not null. (Substring-scoped so this doesn't depend on exact
  // double formatting -- only on "not the literal null".)
  size_t pos6 = body.find("\"name\":\"a1_latency_ms_6\"");
  ASSERT_NE(GoogleString::npos, pos6);
  size_t median_pos = body.find("\"median\":", pos6);
  ASSERT_NE(GoogleString::npos, median_pos);
  EXPECT_THAT(body.substr(median_pos, 20),
              ::testing::Not(::testing::HasSubstr("null")));
  size_t p90_pos = body.find("\"p90\":", pos6);
  ASSERT_NE(GoogleString::npos, p90_pos);
  EXPECT_THAT(body.substr(p90_pos, 20),
              ::testing::Not(::testing::HasSubstr("null")));

  // A real, populated bucket -- not just the array's opening bracket -- for
  // the 3-sample histogram's values (10, 20, 30). CountHistogram (what
  // factory()->statistics() creates) is bucket-less by design, so this uses
  // a small test-only histogram with real fixed-width buckets instead (see
  // FixedBucketHistogram above): 3 buckets 10 wide, one sample each.
  BucketedStatistics bucketed_stats(thread_system_.get());
  Histogram* bucketed_h = bucketed_stats.AddHistogram("a1_latency_ms");
  bucketed_h->Add(10);
  bucketed_h->Add(20);
  bucketed_h->Add(30);
  GoogleString bucket_body;
  StringAsyncFetch bucket_fetch(rewrite_driver()->request_context(),
                                &bucket_body);
  daemon_site_->PrintHistograms(AdminSite::kPageSpeedAdmin, &bucket_fetch,
                                &bucketed_stats);
  EXPECT_THAT(bucket_body,
              ::testing::HasSubstr("{\"start\":10,\"limit\":20,\"count\":1}"));
  EXPECT_THAT(bucket_body,
              ::testing::HasSubstr("{\"start\":20,\"limit\":30,\"count\":1}"));
  EXPECT_THAT(bucket_body,
              ::testing::HasSubstr("{\"start\":30,\"limit\":40,\"count\":1}"));
}

// =============================================================================
// Response hygiene: no-store JSON, SPA shell headers + ETag/304.
// =============================================================================

TEST_F(AdminSiteDaemonTest, JsonLeavesAreNoStoreAndNosniff) {
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/stats_json",
                              RequestHeaders::kGet, &body);
  // Cache-Control is one of Headers' comma-separated fields (see
  // IsCommaSeparatedField in pagespeed/kernel/http/headers.cc), so a
  // multi-directive value is indexed as separate directives and Lookup1
  // (which only succeeds for a single stored value) cannot return the
  // combined string. LookupJoined synthesizes the full serialized value
  // (joining with ", "), which pins the exact wire value instead of just
  // membership.
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
  EXPECT_STREQ("nosniff",
               fetch->response_headers()->Lookup1("X-Content-Type-Options"));
}

TEST_F(AdminSiteDaemonTest, SpaShellCarriesEtagAndFramingHeaders) {
  GoogleString body;
  auto fetch =
      FetchAdminPage(daemon_site_.get(), "http://example.com/pagespeed_admin/",
                     RequestHeaders::kGet, &body);
  const ResponseHeaders* h = fetch->response_headers();
  EXPECT_EQ(200, h->status_code());
  EXPECT_STREQ("text/html; charset=utf-8",
               h->Lookup1(HttpAttributes::kContentType));
  EXPECT_STREQ("DENY", h->Lookup1("X-Frame-Options"));
  EXPECT_THAT(h->Lookup1("Content-Security-Policy"),
              ::testing::HasSubstr("frame-ancestors 'none'"));
  ASSERT_NE(nullptr, h->Lookup1(HttpAttributes::kEtag));
  EXPECT_THAT(body, ::testing::HasSubstr("<title>"));
}

TEST_F(AdminSiteDaemonTest, MatchingEtagGets304) {
  GoogleString body;
  auto first =
      FetchAdminPage(daemon_site_.get(), "http://example.com/pagespeed_admin/",
                     RequestHeaders::kGet, &body);
  ASSERT_NE(nullptr, first->response_headers()->Lookup1(HttpAttributes::kEtag));
  GoogleString etag = first->response_headers()->Lookup1(HttpAttributes::kEtag);
  GoogleString body2;
  std::unique_ptr<StringAsyncFetch> fetch(
      new StringAsyncFetch(rewrite_driver()->request_context(), &body2));
  fetch->request_headers()->set_method(RequestHeaders::kGet);
  fetch->request_headers()->Add(HttpAttributes::kIfNoneMatch, etag);
  daemon_site_->ServeSpaConsole(fetch.get());
  EXPECT_EQ(304, fetch->response_headers()->status_code());
  EXPECT_TRUE(body2.empty());
  // RFC 7232 4.1: a 304 still carries the resource's current ETag.
  EXPECT_STREQ(etag.c_str(),
               fetch->response_headers()->Lookup1(HttpAttributes::kEtag));
}

TEST_F(AdminSiteDaemonTest, StaleEtagGetsFullBody) {
  GoogleString body;
  std::unique_ptr<StringAsyncFetch> fetch(
      new StringAsyncFetch(rewrite_driver()->request_context(), &body));
  fetch->request_headers()->set_method(RequestHeaders::kGet);
  fetch->request_headers()->Add(HttpAttributes::kIfNoneMatch, "\"stale\"");
  daemon_site_->ServeSpaConsole(fetch.get());
  EXPECT_EQ(200, fetch->response_headers()->status_code());
  EXPECT_THAT(body, ::testing::HasSubstr("<title>"));
}

// "Every admin JSON response" means every one, not just the ones the
// original brief happened to touch: ConsoleJsonHandler's answers, the
// trailing-slash redirect, and the daemon-proxy paths (in
// admin_daemon_handler.cc) all needed the same treatment.

TEST_F(AdminSiteDaemonTest, ConsoleJsonNoLoggerCarriesNoStoreAndNosniff) {
  // factory()->statistics() is SimpleStats, whose console_logger() is always
  // NULL (see the comment near line 75), so '?json' always answers 404 here.
  GoogleString body;
  auto fetch = FetchAdminPage(daemon_site_.get(),
                              "http://example.com/pagespeed_admin/console?json",
                              RequestHeaders::kGet, &body);
  EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code());
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
  EXPECT_STREQ("nosniff",
               fetch->response_headers()->Lookup1("X-Content-Type-Options"));
}

TEST_F(AdminSiteDaemonTest, RedirectWithoutSlashCarriesNoStoreAndNosniff) {
  GoogleString body;
  auto fetch =
      FetchAdminPage(daemon_site_.get(), "http://example.com/pagespeed_admin",
                     RequestHeaders::kGet, &body);
  EXPECT_EQ(HttpStatus::kMovedPermanently,
            fetch->response_headers()->status_code());
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
  EXPECT_STREQ("nosniff",
               fetch->response_headers()->Lookup1("X-Content-Type-Options"));
}

TEST_F(AdminSiteDaemonTest, NoReaderDaemonResponseCarriesNoStoreAndNosniff) {
  // A dedicated AdminSite with a null DaemonReader, distinct from the
  // fixture's daemon_site_/admin_site_ members, per the review's ask.
  AdminSite no_reader(timer(), message_handler(), nullptr);
  GoogleString body;
  auto fetch = FetchAdminPage(
      &no_reader, "http://localhost/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &body);
  EXPECT_EQ(HttpStatus::kUnavailable, fetch->response_headers()->status_code());
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
  EXPECT_STREQ("nosniff",
               fetch->response_headers()->Lookup1("X-Content-Type-Options"));
}

// =============================================================================
// A per-host console's view of the optimizer's statistics: only its own
// site's row of the serve savings per host.
// =============================================================================

// Two sites' rows, as the optimizer writes them.
constexpr char kTwoSiteStats[] =
    "{\"serve_savings_by_host\":{\"hosts\":["
    "{\"hits\":5,\"host\":\"www.example.test\",\"optimized_bytes\":500,"
    "\"original_bytes\":1000},"
    "{\"hits\":3,\"host\":\"static.example.test\",\"optimized_bytes\":300,"
    "\"original_bytes\":900}],"
    "\"limit\":32,"
    "\"other\":{\"hits\":1,\"optimized_bytes\":100,\"original_bytes\":200}},"
    "\"uptime_seconds\":3600}";

constexpr char kOwnRowOnly[] =
    "{\"serve_savings_by_host\":{\"hosts\":["
    "{\"hits\":5,\"host\":\"www.example.test\",\"optimized_bytes\":500,"
    "\"original_bytes\":1000}],"
    "\"limit\":32,"
    "\"other\":{\"hits\":4,\"optimized_bytes\":400,\"original_bytes\":1100},"
    "\"site\":\"www.example.test\"},"
    "\"uptime_seconds\":3600}";

TEST_F(AdminSiteDaemonTest, PerSiteStatsCarriesOnlyItsOwnHostRow) {
  fake_reader_->body_ = kTwoSiteStats;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/stats",
      RequestHeaders::kGet, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ("/v1/stats", fake_reader_->last_daemon_path_);
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ(kOwnRowOnly, buffer);
  EXPECT_STREQ("application/json", fetch->response_headers()->Lookup1(
                                       HttpAttributes::kContentType));
  EXPECT_EQ("no-store, private", fetch->response_headers()->LookupJoined(
                                     HttpAttributes::kCacheControl));
}

TEST_F(AdminSiteDaemonTest, PerSiteStatsWithoutAnOwnHostCarriesNoRow) {
  // A port that names no host for its site (IIS, Envoy, a site without a
  // usable name): every row is folded, none is shown.
  fake_reader_->body_ = kTwoSiteStats;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/stats",
      RequestHeaders::kGet, &buffer, false /* is_global */);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"hosts\":[]"));
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"site\":\"\""));
  EXPECT_THAT(buffer, ::testing::Not(::testing::HasSubstr("example.test")));
}

TEST_F(AdminSiteDaemonTest, WholeServerStatsAnswerIsTheUpstreamBytes) {
  // Spacing and key order the module's own writer would never produce: the
  // whole-server console gets exactly what the optimizer sent.
  const char kOddlySpaced[] =
      "{ \"zeta\" : 1, \"serve_savings_by_host\" : {\"hosts\":[{\"host\":"
      "\"static.example.test\",\"hits\":3,\"original_bytes\":900,"
      "\"optimized_bytes\":300}], \"limit\":32, \"other\":{\"hits\":0,"
      "\"original_bytes\":0,\"optimized_bytes\":0}} , \"alpha\" :2 }";
  fake_reader_->body_ = kOddlySpaced;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/stats",
      RequestHeaders::kGet, &buffer, true /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ(kOddlySpaced, buffer);
}

TEST_F(AdminSiteDaemonTest, PerSiteHealthIsTheUpstreamBytes) {
  // Only the leaves that name other sites are narrowed.
  fake_reader_->body_ = "{ \"status\" : \"ok\" }";
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/health",
      RequestHeaders::kGet, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ("{ \"status\" : \"ok\" }", buffer);
}

TEST_F(AdminSiteDaemonTest, PerSiteStatsAnswerThatIsNotAnObjectIsUnreachable) {
  for (const char* body :
       {"not json", "[]", "{\"a\":1} trailing",
        "{\"serve_savings_by_host\":{},\"serve_savings_by_host\":{\"hosts\":"
        "[{\"hits\":1,\"host\":\"static.example.test\",\"optimized_bytes\":1,"
        "\"original_bytes\":1}],\"limit\":32,\"other\":{\"hits\":0,"
        "\"optimized_bytes\":0,\"original_bytes\":0}}}"}) {
    fake_reader_->body_ = body;
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        "http://www.example.test/pagespeed_admin/v1/daemon/stats",
        RequestHeaders::kGet, &buffer, false /* is_global */, nullptr,
        nullptr, "www.example.test");
    ASSERT_TRUE(fetch->done()) << body;
    EXPECT_EQ(HttpStatus::kBadGateway,
              fetch->response_headers()->status_code())
        << body;
    EXPECT_EQ("{\"error\":\"daemon_unreachable\"}", buffer) << body;
  }
}

TEST_F(AdminSiteDaemonTest, PerSiteStatsErrorStatusCarriesNoUpstreamBody) {
  fake_reader_->status_ = 503;
  fake_reader_->body_ = "{\"error\":\"busy\",\"host\":\"static.example.test\"}";
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/stats",
      RequestHeaders::kGet, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(503, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"daemon_error\"}", buffer);
}

TEST_F(AdminSiteDaemonTest, PerSiteStatsHeadSendsNoBody) {
  fake_reader_->body_ = kTwoSiteStats;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/stats",
      RequestHeaders::kHead, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("", buffer);
}

TEST_F(AdminSiteDaemonTest, PerSiteStatsOverCapIsUnreachable) {
  fake_reader_->fail_ = true;
  fake_reader_->fail_reason_ = DaemonReadFailure::kTooLarge;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/stats",
      RequestHeaders::kGet, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"daemon_unreachable\"}", buffer);
}

TEST_F(AdminSiteDaemonTest, TheServerContextPassesTheOwnHostThrough) {
  // The ports call SystemServerContext::AdminPage; the own host must reach
  // the proxy from there.
  FakeDaemonReader* reader = new FakeDaemonReader(message_handler());
  reader->body_ = kTwoSiteStats;
  SystemServerContext* ssc =
      static_cast<SystemServerContext*>(server_context_.get());
  ssc->SetAdminSiteForTesting(
      std::make_unique<AdminSite>(timer(), message_handler(), reader));
  GoogleString buffer;
  StringAsyncFetch fetch(rewrite_driver()->request_context(), &buffer);
  fetch.request_headers()->set_method(RequestHeaders::kGet);
  GoogleUrl gurl("http://www.example.test/pagespeed_admin/v1/daemon/stats");
  QueryParams query_params;
  query_params.ParseFromUrl(gurl);
  ssc->AdminPage(false /* global */, gurl, query_params,
                 rewrite_driver()->options(), &fetch, "", "www.example.test");
  ASSERT_TRUE(fetch.done());
  EXPECT_EQ(kOwnRowOnly, buffer);
}

// Two sites' cooldown entries, as the optimizer writes them.
constexpr char kTwoSiteCooldowns[] =
    "{\"cooldowns\":["
    "{\"duration_seconds\":60,\"hostname\":\"www.example.test\","
    "\"reason\":\"processing\",\"remaining_seconds\":30,\"scheme\":\"https\","
    "\"url\":\"/a.css\"},"
    "{\"duration_seconds\":60,\"hostname\":\"static.example.test\","
    "\"reason\":\"processing\",\"remaining_seconds\":20,\"scheme\":\"https\","
    "\"url\":\"/b.js\"}],"
    "\"count\":2,\"enabled\":true}";

TEST_F(AdminSiteDaemonTest, PerSiteCooldownsListOnlyItsOwnHost) {
  fake_reader_->body_ = kTwoSiteCooldowns;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/cooldowns",
      RequestHeaders::kGet, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ("/v1/cache/cooldowns", fake_reader_->last_daemon_path_);
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ(
      "{\"cooldowns\":[{\"duration_seconds\":60,\"hostname\":"
      "\"www.example.test\",\"reason\":\"processing\",\"remaining_seconds\":"
      "30,\"scheme\":\"https\",\"url\":\"/a.css\"}],\"count\":1,"
      "\"enabled\":true}",
      buffer);
}

TEST_F(AdminSiteDaemonTest, WholeServerCooldownsAreTheUpstreamBytes) {
  fake_reader_->body_ = kTwoSiteCooldowns;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_global_admin/v1/daemon/cooldowns",
      RequestHeaders::kGet, &buffer, true /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(kTwoSiteCooldowns, buffer);
}

TEST_F(AdminSiteDaemonTest,
       PerSiteCooldownsAnswerThatIsNotAnObjectIsUnreachable) {
  fake_reader_->body_ =
      "[{\"hostname\":\"static.example.test\",\"url\":\"/b.js\"}]";
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/cooldowns",
      RequestHeaders::kGet, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"daemon_unreachable\"}", buffer);
}

TEST_F(AdminSiteDaemonTest, PerSiteCooldownsErrorStatusCarriesNoUpstreamBody) {
  fake_reader_->status_ = 503;
  fake_reader_->body_ = kTwoSiteCooldowns;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/cooldowns",
      RequestHeaders::kGet, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(503, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"error\":\"daemon_error\"}", buffer);
}

TEST_F(AdminSiteDaemonTest, PerSiteCooldownsHeadSendsNoBody) {
  fake_reader_->body_ = kTwoSiteCooldowns;
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://www.example.test/pagespeed_admin/v1/daemon/cooldowns",
      RequestHeaders::kHead, &buffer, false /* is_global */, nullptr, nullptr,
      "www.example.test");
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("", buffer);
}

}  // namespace

}  // namespace net_instaweb
