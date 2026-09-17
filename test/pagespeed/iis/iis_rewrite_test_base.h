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

#ifndef PAGESPEED_IIS_IIS_REWRITE_TEST_BASE_H_
#define PAGESPEED_IIS_IIS_REWRITE_TEST_BASE_H_

// Test base class combining IIS mock infrastructure with RewriteDriver.
// This enables testing IIS module behavior with actual HTML rewriting.
//
// Usage pattern:
//   class MyTest : public IisRewriteTestBase {
//     void SetUp() override {
//       IisRewriteTestBase::SetUp();
//       // Enable filters
//       AddFilter(RewriteOptions::kExtendCacheCss);
//       rewrite_driver()->AddFilters();
//     }
//
//     TEST_F(MyTest, RewritesHtml) {
//       GoogleString output;
//       RewriteHtml("<html>...</html>", &output);
//       EXPECT_TRUE(output.find(".pagespeed.") != GoogleString::npos);
//     }
//   };

#include <memory>

#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/iis/mock_iis.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"

namespace net_instaweb {

class RequestHeaders;
class ResponseHeaders;

// Test base that combines RewriteTestBase (for RewriteDriver infrastructure)
// with IIS mock context infrastructure.
//
// This provides:
// - Full RewriteDriver and filter testing capabilities
// - Mock IIS HTTP context for simulating IIS requests
// - Action recording for verifying IIS API call sequences
// - Helper methods for simulating IIS rewriting flow
class IisRewriteTestBase : public RewriteTestBase {
 protected:
  static const char kTestDomain[];  // "http://test.com/"

  IisRewriteTestBase();
  ~IisRewriteTestBase() override;

  void SetUp() override;
  void TearDown() override;

  // ==========================================================================
  // IIS Context Management
  // ==========================================================================

  // Create a new mock IIS context for testing.
  // The context is stored internally and accessible via iis_context().
  void CreateIisContext(const GoogleString& url);
  void CreateIisContext(const GoogleString& url, const GoogleString& method);

  // Access the current IIS context
  MockHttpContext* iis_context() { return iis_context_.get(); }
  MockHttpRequest* iis_request() { return iis_context_->request(); }
  MockHttpResponse* iis_response() { return iis_context_->response(); }

  // Clear and reset IIS context
  void ResetIisContext();

  // ==========================================================================
  // HTML Rewriting Simulation
  // ==========================================================================

  // Rewrite HTML content through the RewriteDriver, storing the result.
  // This simulates the complete IIS request cycle with HTML rewriting.
  //
  // Returns true on success, false if rewriting failed.
  bool RewriteHtml(const StringPiece& input_html, GoogleString* output_html);

  // Rewrite HTML for a specific URL
  bool RewriteHtmlForUrl(const StringPiece& url,
                         const StringPiece& input_html,
                         GoogleString* output_html);

  // Rewrite HTML and also capture response headers
  bool RewriteHtmlWithHeaders(const StringPiece& input_html,
                              GoogleString* output_html,
                              ResponseHeaders* response_headers);

  // ==========================================================================
  // Resource Rewriting Simulation
  // ==========================================================================

  // Set up a resource that can be fetched during rewriting
  void SetupResource(const StringPiece& url,
                     const ContentType& content_type,
                     const StringPiece& content);

  // Set up a resource with custom TTL
  void SetupResourceWithTtl(const StringPiece& url,
                            const ContentType& content_type,
                            const StringPiece& content,
                            int64 ttl_sec);

  // Fetch a rewritten resource by URL
  bool FetchResource(const StringPiece& url, GoogleString* content);

  // Fetch a resource and capture headers
  bool FetchResourceWithHeaders(const StringPiece& url,
                                GoogleString* content,
                                ResponseHeaders* headers);

  // ==========================================================================
  // IIS-Specific Testing Helpers
  // ==========================================================================

  // Simulate IIS BeginRequest phase
  void SimulateBeginRequest(const GoogleString& url);

  // Simulate IIS SendResponse phase (after rewriting)
  void SimulateSendResponse(const GoogleString& content);

  // Simulate IIS EndRequest phase
  void SimulateEndRequest();

  // Get all recorded IIS API actions
  GoogleString GetIisActions();

  // Clear recorded IIS actions
  void ClearIisActions();

  // ==========================================================================
  // Configuration Helpers
  // ==========================================================================

  // Create a mock IIS configuration
  std::unique_ptr<MockAppHostElement> CreateIisConfig();
  MockConfigBuilder& iis_config_builder() { return config_builder_; }

  // Helper to check if URL is a PageSpeed resource
  static bool IsPageSpeedUrl(const GoogleString& url);

  // Helper to encode a PageSpeed URL
  static GoogleString EncodePageSpeedUrl(const GoogleString& url,
                                         const GoogleString& filter_id);

 private:
  std::unique_ptr<MockHttpContext> iis_context_;
  MockConfigBuilder config_builder_;

  IisRewriteTestBase(const IisRewriteTestBase&) = delete;
  IisRewriteTestBase& operator=(const IisRewriteTestBase&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_REWRITE_TEST_BASE_H_
