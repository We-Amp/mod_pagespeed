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
    // Combinations count under the combiner when the sizes differ.
    {"combined scripts, sizes differ", false, kGet, 200, nullptr, kBrowser,
     "jc", 2000, 1500, "", -1, -1, kCounted, kJavascript, 2000, 1500},
    {"combined stylesheets, sizes differ", false, kGet, 200, nullptr, kBrowser,
     "cc", 2000, 1900, "", -1, -1, kCounted, kCss, 2000, 1900},
    {"combined scripts, same size", false, kGet, 200, nullptr, kBrowser, "jc",
     2000, 2000, "", -1, -1, kNotCounted, kNone, 0, 0},
    {"combined stylesheets, same size", false, kGet, 200, nullptr, kBrowser,
     "cc", 2000, 2000, "", -1, -1, kNotCounted, kNone, 0, 0},
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
    // Entries without a record.
    {"old image entry with the length header", false, kGet, 200, nullptr,
     kBrowser, nullptr, 0, 0, "ic", 1000, 400, kCounted, kImage, 1000, 400},
    {"old image entry without the header", false, kGet, 200, nullptr, kBrowser,
     nullptr, 0, 0, "ic", -1, 400, kUnmeasured, kNone, 0, 0},
    {"old image entry, body stored compressed", false, kGet, 200, nullptr,
     kBrowser, nullptr, 0, 0, "ic", 1000, -1, kUnmeasured, kNone, 0, 0},
    {"old stylesheet entry", false, kGet, 200, nullptr, kBrowser, nullptr, 0, 0,
     "cf", 900, 700, kUnmeasured, kNone, 0, 0},
    {"old script entry", false, kGet, 200, nullptr, kBrowser, nullptr, 0, 0,
     "jm", 800, 500, kUnmeasured, kNone, 0, 0},
    {"old combination entry", false, kGet, 200, nullptr, kBrowser, nullptr, 0,
     0, "jc", 800, 500, kNotCounted, kNone, 0, 0},
    {"old cache-extension entry", false, kGet, 200, nullptr, kBrowser, nullptr,
     0, 0, "ce", 800, 800, kNotCounted, kNone, 0, 0},
    {"no record and not a cache answer", false, kGet, 200, nullptr, kBrowser,
     nullptr, 0, 0, "", -1, -1, kNotCounted, kNone, 0, 0},
    // The request rules come first for entries without a record as well.
    {"old stylesheet entry, HEAD", false, RequestHeaders::kHead, 200, nullptr,
     kBrowser, nullptr, 0, 0, "cf", 900, 700, kNotCounted, kNone, 0, 0},
    {"old image entry, the module's own fetch", false, kGet, 200, nullptr,
     kModuleFetch, nullptr, 0, 0, "ic", 1000, 400, kNotCounted, kNone, 0, 0},
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
    EXPECT_EQ(row.outcome == kUnmeasured ? 1 : 0,
              Value(RewriteStats::kServedSavingsUnmeasuredResponses));
  }
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
