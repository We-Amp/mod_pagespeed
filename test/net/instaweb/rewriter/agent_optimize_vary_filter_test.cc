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

// Unit tests for AgentOptimizeVaryFilter. The
// `Vary: Accept` negotiation signal is gated by exactly two things: the
// operator's AgentOptimize flag (which adds the filter) and an
// `Accept: text/markdown` request. There is no server-side entitlement --
// the base ServerContext used by this test rig has none, and the signal
// fires here all the same.

#include "net/instaweb/rewriter/public/agent_optimize_vary_filter.h"

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kHtml[] = "<html><head></head><body>hello</body></html>";

class AgentOptimizeVaryFilterTest : public RewriteTestBase {
 protected:
  // The operator flag: AddFilters installs the filter iff it is set
  // (rewrite_driver_filter_init.cc). Nothing else gates it.
  void EnableAgentOptimize() {
    options()->ClearSignatureForTesting();
    options()->set_agent_optimize(true);
    rewrite_driver()->AddFilters();
  }

  // Parses kHtml with the given Accept header (nullptr = no Accept header),
  // leaving the response headers the filter saw in response_headers_.
  void ParseWithAccept(const char* accept) {
    RequestHeaders request_headers;
    if (accept != nullptr) {
      request_headers.Add(HttpAttributes::kAccept, accept);
    }
    rewrite_driver()->SetRequestHeaders(request_headers);
    rewrite_driver()->set_response_headers_ptr(&response_headers_);
    ParseUrl(StrCat(kTestDomain, "index.html"), kHtml);
  }

  bool VariesOnAccept() const {
    return response_headers_.HasValue(HttpAttributes::kVary,
                                      HttpAttributes::kAccept);
  }
};

TEST_F(AgentOptimizeVaryFilterTest, MarkdownAcceptVariesWithOperatorFlagOnly) {
  EnableAgentOptimize();
  ParseWithAccept("text/markdown");
  EXPECT_TRUE(VariesOnAccept());
}

TEST_F(AgentOptimizeVaryFilterTest, MarkdownAcceptWithQualityAndSiblings) {
  EnableAgentOptimize();
  ParseWithAccept("text/html;q=0.8, text/markdown;q=0.9, */*;q=0.1");
  EXPECT_TRUE(VariesOnAccept());
}

TEST_F(AgentOptimizeVaryFilterTest, BrowserAcceptDoesNotVary) {
  EnableAgentOptimize();
  ParseWithAccept(
      "text/html,application/xhtml+xml,application/xml;q=0.9,"
      "*/*;q=0.8");
  EXPECT_FALSE(VariesOnAccept());
}

TEST_F(AgentOptimizeVaryFilterTest, WildcardAcceptDoesNotVary) {
  EnableAgentOptimize();
  ParseWithAccept("*/*");
  EXPECT_FALSE(VariesOnAccept());
}

TEST_F(AgentOptimizeVaryFilterTest, NoAcceptHeaderDoesNotVary) {
  EnableAgentOptimize();
  ParseWithAccept(nullptr);
  EXPECT_FALSE(VariesOnAccept());
}

TEST_F(AgentOptimizeVaryFilterTest, FlagOffNeverVaries) {
  // AgentOptimize is off by default: the filter is not even in the chain.
  rewrite_driver()->AddFilters();
  ParseWithAccept("text/markdown");
  EXPECT_FALSE(VariesOnAccept());
}

TEST_F(AgentOptimizeVaryFilterTest, ExistingVaryAcceptIsNotDuplicated) {
  EnableAgentOptimize();
  response_headers_.Add(HttpAttributes::kVary, HttpAttributes::kAccept);
  ParseWithAccept("text/markdown");
  ConstStringStarVector values;
  ASSERT_TRUE(response_headers_.Lookup(HttpAttributes::kVary, &values));
  EXPECT_EQ(1, values.size());
  EXPECT_TRUE(VariesOnAccept());
}

TEST_F(AgentOptimizeVaryFilterTest, BodyIsNeverChanged) {
  // 1.1 has no markdown render: the signal is header-only, so a markdown-
  // accepting request gets byte-identical HTML.
  EnableAgentOptimize();
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kAccept, "text/markdown");
  rewrite_driver()->SetRequestHeaders(request_headers);
  rewrite_driver()->set_response_headers_ptr(&response_headers_);
  ValidateNoChanges("markdown_accept", kHtml);
}

// The Accept-token matcher, mirrored from 2.0's AcceptContains: the
// text/markdown media range as a delimited token, never matched by */* or by
// a longer token that merely contains it.
TEST(AgentOptimizeVaryFilterAcceptTest, TokenBoundaries) {
  EXPECT_FALSE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(nullptr));

  RequestHeaders headers;
  EXPECT_FALSE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));

  headers.Replace(HttpAttributes::kAccept, "text/markdown");
  EXPECT_TRUE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));

  headers.Replace(HttpAttributes::kAccept, "text/markdown;q=0.5");
  EXPECT_TRUE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));

  headers.Replace(HttpAttributes::kAccept, "text/html, text/markdown");
  EXPECT_TRUE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));

  headers.Replace(HttpAttributes::kAccept, "text/html,text/markdown,*/*");
  EXPECT_TRUE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));

  headers.Replace(HttpAttributes::kAccept, "text/markdownx");
  EXPECT_FALSE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));

  headers.Replace(HttpAttributes::kAccept, "xtext/markdown");
  EXPECT_FALSE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));

  headers.Replace(HttpAttributes::kAccept, "*/*");
  EXPECT_FALSE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));

  headers.Replace(HttpAttributes::kAccept, "text/html");
  EXPECT_FALSE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));
}

}  // namespace

}  // namespace net_instaweb
