// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The HTML cache's agent class on Apache: computed and counted once per
// client request, in the pool an internal redirect shares.

#include <cctype>
#include <cstring>
#include <memory>

#include "apr_pools.h"   // NOLINT
#include "apr_tables.h"  // NOLINT
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/apache/apache_daemon_html.h"
#include "base/logging.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_html_agent_class.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/agent_vectors.h"

namespace net_instaweb {
namespace {

const char kGptBot[] = "Mozilla/5.0 (compatible; GPTBot/1.2; +https://openai.com/gptbot)";

class ApacheDaemonHtmlAgentClassTest : public testing::Test {
 protected:
  ApacheDaemonHtmlAgentClassTest()
      : thread_system_(Platform::CreateThreadSystem()),
        stats_(thread_system_.get()) {
    RewriteStats::InitStats(&stats_);
  }

  void SetUp() override {
    MockApache::Initialize();
    memset(&request_, 0, sizeof(request_));
    MockApache::PrepareRequest(&request_);
  }

  void TearDown() override {
    MockApache::CleanupRequest(&request_);
    MockApache::Terminate();
  }

  int64 Count(const char* name) { return stats_.GetVariable(name)->Get(); }

  int64 Classified() {
    return Count(kDaemonHtmlAgentClassC1) + Count(kDaemonHtmlAgentClassC3) +
           Count(kDaemonHtmlAgentClassC4);
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
  request_rec request_;
};

// Every shared vector, each as a request of its own.
TEST_F(ApacheDaemonHtmlAgentClassTest, EverySharedVector) {
  for (const AgentVector& v : LoadAgentVectors().vectors) {
    request_rec r;
    memset(&r, 0, sizeof(r));
    MockApache::PrepareRequest(&r);
    if (!v.user_agent.empty()) {
      apr_table_set(r.headers_in, HttpAttributes::kUserAgent,
                    v.user_agent.c_str());
    }
    EXPECT_EQ(v.expected_class, ApacheDaemonHtmlAgentClass(&r, true, &stats_))
        << "agent_vectors.txt:" << v.line << ": " << v.user_agent;
    MockApache::CleanupRequest(&r);
  }
}

// The first User-Agent line is the one classified.
TEST_F(ApacheDaemonHtmlAgentClassTest, TheFirstUserAgentLine) {
  apr_table_add(request_.headers_in, "User-Agent", "Mozilla/5.0");
  apr_table_add(request_.headers_in, "user-agent", kGptBot);
  EXPECT_EQ(0, ApacheDaemonHtmlAgentClass(&request_, true, &stats_));
}

TEST_F(ApacheDaemonHtmlAgentClassTest, NoUserAgentIsUnknown) {
  EXPECT_EQ(0, ApacheDaemonHtmlAgentClass(&request_, true, &stats_));
  EXPECT_EQ(1, Count(kDaemonHtmlAgentClassC4));
}

// With the cache off nothing is computed, counted or kept.
TEST_F(ApacheDaemonHtmlAgentClassTest, OffKeepsNothing) {
  apr_table_set(request_.headers_in, "User-Agent", kGptBot);
  EXPECT_EQ(0, ApacheDaemonHtmlAgentClass(&request_, false, &stats_));
  EXPECT_EQ(0, Classified());
  void* data = nullptr;
  apr_pool_userdata_get(&data, "mod_pagespeed_daemon_html_agent_class",
                        request_.pool);
  EXPECT_EQ(nullptr, data);
  // A later pass with the cache on is the first computation.
  EXPECT_EQ(3, ApacheDaemonHtmlAgentClass(&request_, true, &stats_));
  EXPECT_EQ(1, Classified());
}

// An internal redirect (ErrorDocument, a rewrite's internal redirect) is a
// new request on the same pool that runs post_read_request again: it reads
// the first pass's class, which is counted once, even when the redirect's
// own headers would say otherwise.
TEST_F(ApacheDaemonHtmlAgentClassTest, AnInternalRedirectClassifiesOnce) {
  apr_table_set(request_.headers_in, "User-Agent", kGptBot);
  EXPECT_EQ(3, ApacheDaemonHtmlAgentClass(&request_, true, &stats_));

  request_rec redirected;
  memset(&redirected, 0, sizeof(redirected));
  redirected.pool = request_.pool;
  redirected.headers_in = apr_table_make(request_.pool, 4);
  apr_table_set(redirected.headers_in, "User-Agent", "Mozilla/5.0");
  redirected.prev = &request_;
  EXPECT_EQ(3, ApacheDaemonHtmlAgentClass(&redirected, true, &stats_));

  request_rec again;
  memset(&again, 0, sizeof(again));
  again.pool = request_.pool;
  again.headers_in = redirected.headers_in;
  again.prev = &redirected;
  EXPECT_EQ(3, ApacheDaemonHtmlAgentClass(&again, true, &stats_));

  EXPECT_EQ(1, Count(kDaemonHtmlAgentClassC3));
  EXPECT_EQ(1, Classified());
}

// A subrequest has a pool of its own; it reads the main request's class.
TEST_F(ApacheDaemonHtmlAgentClassTest, ASubrequestReadsTheMainRequests) {
  apr_table_set(request_.headers_in, "User-Agent", kGptBot);
  EXPECT_EQ(3, ApacheDaemonHtmlAgentClass(&request_, true, &stats_));
  apr_pool_t* sub_pool = nullptr;
  ASSERT_EQ(APR_SUCCESS, apr_pool_create(&sub_pool, request_.pool));
  request_rec sub;
  memset(&sub, 0, sizeof(sub));
  sub.pool = sub_pool;
  sub.headers_in = apr_table_make(sub_pool, 4);
  sub.main = &request_;
  EXPECT_EQ(3, ApacheDaemonHtmlAgentClass(&sub, true, &stats_));
  EXPECT_EQ(1, Classified());
}

// A subrequest seen first computes for the client request, from its
// headers, in its pool.
TEST_F(ApacheDaemonHtmlAgentClassTest, ASubrequestFirstKeepsItForTheMain) {
  apr_table_set(request_.headers_in, "User-Agent", kGptBot);
  apr_pool_t* sub_pool = nullptr;
  ASSERT_EQ(APR_SUCCESS, apr_pool_create(&sub_pool, request_.pool));
  request_rec sub;
  memset(&sub, 0, sizeof(sub));
  sub.pool = sub_pool;
  sub.headers_in = apr_table_make(sub_pool, 4);
  sub.main = &request_;
  EXPECT_EQ(3, ApacheDaemonHtmlAgentClass(&sub, true, &stats_));
  EXPECT_EQ(3, ApacheDaemonHtmlAgentClass(&request_, true, &stats_));
  EXPECT_EQ(1, Classified());
}

// The post_read_request hook (it runs for the client request and again
// for each internal redirect) computes the class first, for a server whose
// HTML cache is on.  The hook needs a live httpd, so the call is pinned by
// reading mod_instaweb.cc.
TEST(ApacheDaemonHtmlAgentClassPinTest, ThePostReadRequestHookComputesIt) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/apache/mod_instaweb.cc");
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  GoogleString src;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) src.push_back(c);
  }
  const size_t hook = src.find("intpagespeed_modify_request(request_rec*r){");
  ASSERT_NE(GoogleString::npos, hook);
  const size_t end = src.find("voidmod_pagespeed_register_hooks(", hook);
  const size_t unplugged = src.find("unplugged()", hook);
  const size_t call = src.find(
      "ApacheDaemonHtmlAgentClass(r,server_context->global_config()->"
      "daemon_html_cache()!=RewriteOptions::kDaemonHtmlCacheOff,"
      "server_context->statistics());",
      hook);
  ASSERT_NE(GoogleString::npos, call);
  EXPECT_LT(unplugged, call);
  EXPECT_LT(call, end);
  EXPECT_NE(GoogleString::npos,
            src.find("ap_hook_post_read_request(pagespeed_modify_request,"));
}

}  // namespace
}  // namespace net_instaweb
