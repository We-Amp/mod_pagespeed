// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/apache/apache_conditional_stash.h"

#include <cstring>

#include "apr_pools.h"   // NOLINT
#include "apr_tables.h"  // NOLINT
#include "pagespeed/kernel/http/http_names.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kMarkdownTag[] = "W/\"psm-0123456789abcdef\"";
const char kDate[] = "Sun, 15 Nov 2026 11:00:00 GMT";

class ApacheConditionalStashTest : public testing::Test {
 protected:
  void SetUp() override {
    MockApache::Initialize();
    memset(&request_, 0, sizeof(request_));
    MockApache::PrepareRequest(&request_);
  }

  void TearDown() override {
    MockApache::CleanupRequest(&request_);
    MockApache::Terminate();
  }

  const char* In(const char* name) {
    return apr_table_get(request_.headers_in, name);
  }

  request_rec request_;
};

TEST_F(ApacheConditionalStashTest, ModuleTagDropsBothFields) {
  apr_table_add(request_.headers_in, "If-None-Match", "\"v1\"");
  apr_table_add(request_.headers_in, "if-none-match", kMarkdownTag);
  apr_table_add(request_.headers_in, "If-Modified-Since", kDate);
  apr_table_add(request_.headers_in, "Accept", "text/markdown");
  ApacheStashAndStripClientConditionals(&request_);
  EXPECT_EQ(nullptr, In(HttpAttributes::kIfNoneMatch));
  EXPECT_EQ(nullptr, In(HttpAttributes::kIfModifiedSince));
  EXPECT_STREQ("text/markdown", In("Accept"));

  const ClientConditionalStash* stash =
      ApacheClientConditionalStash(&request_);
  ASSERT_NE(nullptr, stash);
  EXPECT_TRUE(stash->stripped);
  EXPECT_TRUE(stash->has_if_none_match);
  EXPECT_EQ(StrCat("\"v1\", ", kMarkdownTag), stash->if_none_match);
  EXPECT_TRUE(stash->has_if_modified_since);
  EXPECT_EQ(kDate, stash->if_modified_since);
}

TEST_F(ApacheConditionalStashTest, ForeignTagIsLeftAlone) {
  apr_table_add(request_.headers_in, "If-None-Match", "W/\"v1\"");
  apr_table_add(request_.headers_in, "If-Modified-Since", kDate);
  ApacheStashAndStripClientConditionals(&request_);
  EXPECT_STREQ("W/\"v1\"", In(HttpAttributes::kIfNoneMatch));
  EXPECT_STREQ(kDate, In(HttpAttributes::kIfModifiedSince));
  const ClientConditionalStash* stash =
      ApacheClientConditionalStash(&request_);
  ASSERT_NE(nullptr, stash);
  EXPECT_FALSE(stash->stripped);
  EXPECT_EQ("W/\"v1\"", stash->if_none_match);
}

TEST_F(ApacheConditionalStashTest, NoHookNoStash) {
  EXPECT_EQ(nullptr, ApacheClientConditionalStash(&request_));
}

// An internal redirect (DirectoryIndex, ErrorDocument) is a new request on
// the same pool and headers_in, and runs the hook again: it still reads the
// fields as the client sent them.
TEST_F(ApacheConditionalStashTest, InternalRedirectKeepsTheOriginalStash) {
  apr_table_add(request_.headers_in, "If-None-Match", kMarkdownTag);
  ApacheStashAndStripClientConditionals(&request_);

  request_rec redirected;
  memset(&redirected, 0, sizeof(redirected));
  redirected.pool = request_.pool;
  redirected.headers_in = request_.headers_in;
  redirected.prev = &request_;
  ApacheStashAndStripClientConditionals(&redirected);

  request_rec again;
  memset(&again, 0, sizeof(again));
  again.pool = request_.pool;
  again.headers_in = request_.headers_in;
  again.prev = &redirected;

  const ClientConditionalStash* stash = ApacheClientConditionalStash(&again);
  ASSERT_NE(nullptr, stash);
  EXPECT_EQ(stash, ApacheClientConditionalStash(&request_));
  EXPECT_TRUE(stash->stripped);
  EXPECT_EQ(kMarkdownTag, stash->if_none_match);
}

// A subrequest has a pool of its own; its stash lookup reaches the main
// request's.
TEST_F(ApacheConditionalStashTest, SubrequestReadsTheMainRequestsStash) {
  apr_table_add(request_.headers_in, "If-None-Match", kMarkdownTag);
  ApacheStashAndStripClientConditionals(&request_);

  apr_pool_t* sub_pool = nullptr;
  ASSERT_EQ(APR_SUCCESS, apr_pool_create(&sub_pool, request_.pool));
  request_rec sub;
  memset(&sub, 0, sizeof(sub));
  sub.pool = sub_pool;
  sub.headers_in = apr_table_copy(sub_pool, request_.headers_in);
  sub.main = &request_;
  EXPECT_EQ(nullptr, apr_table_get(sub.headers_in, "If-None-Match"));
  const ClientConditionalStash* stash = ApacheClientConditionalStash(&sub);
  ASSERT_NE(nullptr, stash);
  EXPECT_EQ(kMarkdownTag, stash->if_none_match);
}

}  // namespace
}  // namespace net_instaweb
