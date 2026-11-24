// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The rules for which responses count towards the sizes of optimized
// responses sent, one row per rule.

#include "net/instaweb/rewriter/public/served_savings.h"

#include <memory>

#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

enum ExpectedType { kNone, kImage, kCss, kJavascript };

struct Row {
  const char* name;
  // The request and the response.
  bool nested_driver;
  RequestHeaders::Method method;
  int status_code;
  const char* range;       // Range header, or null.
  const char* user_agent;  // User-Agent header, or null.
  // The size record; no record when filter_id is null.
  const char* filter_id;
  int64 original_size;
  int64 optimized_size;
  // For a response without a record.
  const char* url_filter_id;
  int64 header_original_size;
  int64 uncompressed_body_size;
  // What must happen.
  ServedSavings::Outcome outcome;
  ExpectedType type;
  int64 added_original;
  int64 added_optimized;
};

const char kBrowser[] = "Mozilla/5.0 (X11; Linux x86_64)";
const char kModuleFetch[] = "Mozilla/5.0 (mod_pagespeed/2.10.0-0)";
const RequestHeaders::Method kGet = RequestHeaders::kGet;
const ServedSavings::Outcome kNotCounted = ServedSavings::kNotCounted;
const ServedSavings::Outcome kCounted = ServedSavings::kCounted;
const ServedSavings::Outcome kUnmeasured = ServedSavings::kUnmeasured;

const Row kRows[] = {
    // Each type, from its own filter.
    {"image", false, kGet, 200, nullptr, kBrowser, "ic", 1000, 400, "", -1, -1,
     kCounted, kImage, 1000, 400},
    {"stylesheet", false, kGet, 200, nullptr, kBrowser, "cf", 900, 700, "", -1,
     -1, kCounted, kCss, 900, 700},
    {"script", false, kGet, 200, nullptr, kBrowser, "jm", 800, 500, "", -1, -1,
     kCounted, kJavascript, 800, 500},
    {"no User-Agent at all", false, kGet, 200, nullptr, nullptr, "ic", 1000,
     400, "", -1, -1, kCounted, kImage, 1000, 400},
    // A response that is larger than what it was made from is counted as it
    // is; so is one of the same size.
    {"larger than the original", false, kGet, 200, nullptr, kBrowser, "ic", 400,
     1000, "", -1, -1, kCounted, kImage, 400, 1000},
    {"same size, optimizing filter", false, kGet, 200, nullptr, kBrowser, "cf",
     700, 700, "", -1, -1, kCounted, kCss, 700, 700},
    // A combination counts under the combiner only when it is smaller than
    // what it was made from, which is a saving carried in by inputs that were
    // optimized first.  Combining itself saves requests, not bytes: a plain
    // combination is the same size or slightly larger, and that overhead is
    // not shown as a negative saving.
    {"combined scripts, smaller", false, kGet, 200, nullptr, kBrowser, "jc",
     2000, 1500, "", -1, -1, kCounted, kJavascript, 2000, 1500},
    {"combined stylesheets, smaller", false, kGet, 200, nullptr, kBrowser, "cc",
     2000, 1900, "", -1, -1, kCounted, kCss, 2000, 1900},
    {"combined scripts, same size", false, kGet, 200, nullptr, kBrowser, "jc",
     2000, 2000, "", -1, -1, kNotCounted, kNone, 0, 0},
    {"combined stylesheets, same size", false, kGet, 200, nullptr, kBrowser,
     "cc", 2000, 2000, "", -1, -1, kNotCounted, kNone, 0, 0},
    {"combined scripts, larger", false, kGet, 200, nullptr, kBrowser, "jc",
     2000, 2100, "", -1, -1, kNotCounted, kNone, 0, 0},
    {"combined stylesheets, larger", false, kGet, 200, nullptr, kBrowser, "cc",
     2000, 2001, "", -1, -1, kNotCounted, kNone, 0, 0},
    // Filters that do not change bytes.
    {"cache extension", false, kGet, 200, nullptr, kBrowser, "ce", 1000, 1000,
     "", -1, -1, kNotCounted, kNone, 0, 0},
    {"image sprite", false, kGet, 200, nullptr, kBrowser, "is", 1000, 800, "",
     -1, -1, kNotCounted, kNone, 0, 0},
    // Requests whose answer is not a whole body sent to a visitor.
    {"nested driver", true, kGet, 200, nullptr, kBrowser, "ic", 1000, 400, "",
     -1, -1, kNotCounted, kNone, 0, 0},
    {"HEAD", false, RequestHeaders::kHead, 200, nullptr, kBrowser, "ic", 1000,
     400, "", -1, -1, kNotCounted, kNone, 0, 0},
    {"POST", false, RequestHeaders::kPost, 200, nullptr, kBrowser, "ic", 1000,
     400, "", -1, -1, kNotCounted, kNone, 0, 0},
    {"304", false, kGet, 304, nullptr, kBrowser, "ic", 1000, 400, "", -1, -1,
     kNotCounted, kNone, 0, 0},
    {"500", false, kGet, 500, nullptr, kBrowser, "ic", 1000, 400, "", -1, -1,
     kNotCounted, kNone, 0, 0},
    {"range request", false, kGet, 200, "bytes=0-99", kBrowser, "ic", 1000, 400,
     "", -1, -1, kNotCounted, kNone, 0, 0},
    {"the module's own fetch", false, kGet, 200, nullptr, kModuleFetch, "ic",
     1000, 400, "", -1, -1, kNotCounted, kNone, 0, 0},
    // A record that cannot be right is not counted.
    {"negative original", false, kGet, 200, nullptr, kBrowser, "ic", -1, 400,
     "", -1, -1, kNotCounted, kNone, 0, 0},
    {"negative optimized", false, kGet, 200, nullptr, kBrowser, "ic", 1000, -5,
     "", -1, -1, kNotCounted, kNone, 0, 0},
    // An output without a record is not counted, whatever it is.  The cache
    // was emptied by the change that introduced the record, so every stored
    // output of a filter that knows its inputs has one.
    {"image without a record", false, kGet, 200, nullptr, kBrowser, nullptr, 0,
     0, "ic", 1000, 400, kNotCounted, kNone, 0, 0},
    {"stylesheet without a record", false, kGet, 200, nullptr, kBrowser,
     nullptr, 0, 0, "cf", 900, 700, kNotCounted, kNone, 0, 0},
    {"script without a record", false, kGet, 200, nullptr, kBrowser, nullptr, 0,
     0, "jm", 800, 500, kNotCounted, kNone, 0, 0},
    {"combination without a record", false, kGet, 200, nullptr, kBrowser,
     nullptr, 0, 0, "jc", 800, 500, kNotCounted, kNone, 0, 0},
};

class ServedSavingsTest : public testing::Test {
 protected:
  ServedSavingsTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(Platform::CreateTimer()),
        stats_(thread_system_.get()) {
    RewriteStats::InitStats(&stats_);
    rewrite_stats_ = std::make_unique<RewriteStats>(
        false, &stats_, thread_system_.get(), timer_.get());
  }

  int64 Value(const char* name) { return stats_.GetVariable(name)->Get(); }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<Timer> timer_;
  SimpleStats stats_;
  std::unique_ptr<RewriteStats> rewrite_stats_;
};

TEST_F(ServedSavingsTest, TheMarkerInTheTableIsTheModules) {
  EXPECT_NE(GoogleString::npos,
            GoogleString(kModuleFetch).find(kModPagespeedSubrequestUserAgent));
  EXPECT_EQ(GoogleString::npos,
            GoogleString(kBrowser).find(kModPagespeedSubrequestUserAgent));
}

TEST_F(ServedSavingsTest, EveryRule) {
  for (const Row& row : kRows) {
    SCOPED_TRACE(row.name);
    stats_.Clear();

    RequestHeaders request_headers;
    request_headers.set_method(row.method);
    request_headers.Add("Accept", "*/*");
    if (row.range != nullptr) {
      request_headers.Add("Range", row.range);
    }
    if (row.user_agent != nullptr) {
      request_headers.Add(HttpAttributes::kUserAgent, row.user_agent);
    }
    ResponseHeaders::SizeRecord record;
    ServedSavings::Response response;
    response.nested_driver = row.nested_driver;
    response.request_headers = &request_headers;
    response.status_code = row.status_code;
    if (row.filter_id != nullptr) {
      record.original_size = row.original_size;
      record.optimized_size = row.optimized_size;
      record.filter_id = row.filter_id;
      response.size_record = &record;
    }
    response.url_filter_id = row.url_filter_id;
    response.header_original_size = row.header_original_size;
    response.uncompressed_body_size = row.uncompressed_body_size;

    EXPECT_EQ(row.outcome,
              ServedSavings::Record(response, rewrite_stats_.get()));

    EXPECT_EQ(row.type == kImage ? row.added_original : 0,
              Value(RewriteStats::kImageServedOriginalBytes));
    EXPECT_EQ(row.type == kImage ? row.added_optimized : 0,
              Value(RewriteStats::kImageServedOptimizedBytes));
    EXPECT_EQ(row.type == kCss ? row.added_original : 0,
              Value(RewriteStats::kCssServedOriginalBytes));
    EXPECT_EQ(row.type == kCss ? row.added_optimized : 0,
              Value(RewriteStats::kCssServedOptimizedBytes));
    EXPECT_EQ(row.type == kJavascript ? row.added_original : 0,
              Value(RewriteStats::kJavascriptServedOriginalBytes));
    EXPECT_EQ(row.type == kJavascript ? row.added_optimized : 0,
              Value(RewriteStats::kJavascriptServedOptimizedBytes));
  }
}

// There is no count of unmeasured responses: nothing is left to be unmeasured.
TEST_F(ServedSavingsTest, NoUnmeasuredResponsesVariable) {
  EXPECT_TRUE(stats_.FindVariable("served_savings_unmeasured_responses") ==
              nullptr);
}

// The header names are matched whatever their case, and a response without
// request headers is not counted.
TEST_F(ServedSavingsTest, HeaderNamesAreCaseInsensitive) {
  ResponseHeaders::SizeRecord record;
  record.original_size = 1000;
  record.optimized_size = 400;
  record.filter_id = "ic";
  ServedSavings::Response response;
  response.status_code = 200;
  response.size_record = &record;

  EXPECT_EQ(kNotCounted, ServedSavings::Record(response, rewrite_stats_.get()));

  RequestHeaders ranged;
  ranged.Add("rAnGe", "bytes=0-1");
  response.request_headers = &ranged;
  EXPECT_EQ(kNotCounted, ServedSavings::Record(response, rewrite_stats_.get()));

  RequestHeaders module_fetch;
  module_fetch.Add("user-agent", kModuleFetch);
  response.request_headers = &module_fetch;
  EXPECT_EQ(kNotCounted, ServedSavings::Record(response, rewrite_stats_.get()));

  RequestHeaders plain;
  plain.Add("user-agent", kBrowser);
  response.request_headers = &plain;
  EXPECT_EQ(kCounted, ServedSavings::Record(response, rewrite_stats_.get()));
  EXPECT_EQ(1000, Value(RewriteStats::kImageServedOriginalBytes));
}

// The variables only grow: two responses add up.
TEST_F(ServedSavingsTest, ResponsesAddUp) {
  RequestHeaders request_headers;
  ResponseHeaders::SizeRecord record;
  record.original_size = 1000;
  record.optimized_size = 400;
  record.filter_id = "ic";
  ServedSavings::Response response;
  response.request_headers = &request_headers;
  response.status_code = 200;
  response.size_record = &record;
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(kCounted, ServedSavings::Record(response, rewrite_stats_.get()));
  }
  EXPECT_EQ(3000, Value(RewriteStats::kImageServedOriginalBytes));
  EXPECT_EQ(1200, Value(RewriteStats::kImageServedOptimizedBytes));
}

}  // namespace
}  // namespace net_instaweb
