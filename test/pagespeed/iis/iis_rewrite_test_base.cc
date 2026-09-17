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

#include <memory>

#include "test/pagespeed/iis/iis_rewrite_test_base.h"

#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

const char IisRewriteTestBase::kTestDomain[] = "http://test.com/";

IisRewriteTestBase::IisRewriteTestBase() {
}

IisRewriteTestBase::~IisRewriteTestBase() {
}

void IisRewriteTestBase::SetUp() {
  RewriteTestBase::SetUp();
  // Create a default IIS context
  CreateIisContext(kTestDomain);
}

void IisRewriteTestBase::TearDown() {
  iis_context_.reset();
  RewriteTestBase::TearDown();
}

void IisRewriteTestBase::CreateIisContext(const GoogleString& url) {
  CreateIisContext(url, "GET");
}

void IisRewriteTestBase::CreateIisContext(const GoogleString& url,
                                          const GoogleString& method) {
  iis_context_ = MockHttpContextBuilder()
      .SetUrl(url)
      .SetMethod(method)
      .SetSiteId(1)
      .SetSiteName("Default Web Site")
      .SetPhysicalPath("C:\\inetpub\\wwwroot")
      .Build();
}

void IisRewriteTestBase::ResetIisContext() {
  iis_context_.reset();
  CreateIisContext(kTestDomain);
}

bool IisRewriteTestBase::RewriteHtml(const StringPiece& input_html,
                                     GoogleString* output_html) {
  return RewriteHtmlForUrl(kTestDomain, input_html, output_html);
}

bool IisRewriteTestBase::RewriteHtmlForUrl(const StringPiece& url,
                                           const StringPiece& input_html,
                                           GoogleString* output_html) {
  ResponseHeaders response_headers;
  return RewriteHtmlWithHeaders(input_html, output_html, &response_headers);
}

bool IisRewriteTestBase::RewriteHtmlWithHeaders(
    const StringPiece& input_html,
    GoogleString* output_html,
    ResponseHeaders* response_headers) {
  // Clear previous IIS actions for clean recording
  ClearIisActions();

  // Simulate BeginRequest - record context setup
  iis_response()->ClearRecordedActions();

  // Use RewriteTestBase's validation mechanism to parse and rewrite HTML
  // The ValidateExpected method runs the HTML through the rewrite driver
  output_html->clear();

  // Parse the HTML through the driver
  ParseUrl(StrCat(kTestDomain, "test.html"), input_html);

  // Get the output
  *output_html = output_buffer_;

  // Set up response headers for IIS
  response_headers->set_status_code(200);
  response_headers->Add(HttpAttributes::kContentType,
                        kContentTypeHtml.mime_type());

  // Simulate writing to IIS response
  iis_response()->SetStatus(200, "OK");
  iis_response()->SetHeader("Content-Type", kContentTypeHtml.mime_type());
  iis_response()->WriteEntityChunks(output_html->data(), output_html->size());
  iis_response()->Flush(true);
  iis_response()->set_completed(true);

  return true;
}

void IisRewriteTestBase::SetupResource(const StringPiece& url,
                                       const ContentType& content_type,
                                       const StringPiece& content) {
  SetupResourceWithTtl(url, content_type, content, 300);  // 5 minute TTL
}

void IisRewriteTestBase::SetupResourceWithTtl(const StringPiece& url,
                                              const ContentType& content_type,
                                              const StringPiece& content,
                                              int64 ttl_sec) {
  SetResponseWithDefaultHeaders(url, content_type, content, ttl_sec);
}

bool IisRewriteTestBase::FetchResource(const StringPiece& url,
                                       GoogleString* content) {
  ResponseHeaders headers;
  return FetchResourceWithHeaders(url, content, &headers);
}

bool IisRewriteTestBase::FetchResourceWithHeaders(const StringPiece& url,
                                                  GoogleString* content,
                                                  ResponseHeaders* headers) {
  return FetchResourceUrl(url, content, headers);
}

void IisRewriteTestBase::SimulateBeginRequest(const GoogleString& url) {
  CreateIisContext(url);
  iis_context()->ClearRecordedActions();
}

void IisRewriteTestBase::SimulateSendResponse(const GoogleString& content) {
  iis_response()->SetStatus(200, "OK");
  iis_response()->SetHeader("Content-Type", "text/html");
  iis_response()->WriteEntityChunks(content.data(), content.size());
}

void IisRewriteTestBase::SimulateEndRequest() {
  iis_response()->Flush(true);
  iis_response()->set_completed(true);
}

GoogleString IisRewriteTestBase::GetIisActions() {
  return iis_context()->AllActions();
}

void IisRewriteTestBase::ClearIisActions() {
  iis_context()->ClearRecordedActions();
}

std::unique_ptr<MockAppHostElement> IisRewriteTestBase::CreateIisConfig() {
  return config_builder_.Build();
}

bool IisRewriteTestBase::IsPageSpeedUrl(const GoogleString& url) {
  return url.find(".pagespeed.") != GoogleString::npos ||
         url.find(",Mcc.") != GoogleString::npos ||    // Combined CSS
         url.find(",Mjc.") != GoogleString::npos;       // Combined JS
}

GoogleString IisRewriteTestBase::EncodePageSpeedUrl(
    const GoogleString& url,
    const GoogleString& filter_id) {
  // Find extension position
  size_t dot_pos = url.rfind('.');
  if (dot_pos == GoogleString::npos) {
    return StrCat(url, ".pagespeed.", filter_id, ".0");
  }

  GoogleString base = url.substr(0, dot_pos);
  GoogleString ext = url.substr(dot_pos);
  return StrCat(base, ".pagespeed.", filter_id, ".0", ext);
}

}  // namespace net_instaweb
