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

// Unit tests for InsertSpeculationRulesFilter.

#include "net/instaweb/rewriter/public/insert_speculation_rules_filter.h"

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

constexpr char kHtmlInput[] =
    "<head>\n"
    "<title>Something</title>\n"
    "</head>"
    "<body> Hello World!</body>";

// Same as kHtmlInput but the body (and html) are never closed; the filter
// must still inject at the stream end.
constexpr char kHtmlNoCloseBody[] =
    "<head>\n"
    "<title>Something</title>\n"
    "</head>"
    "<body> Hello World!";

class InsertSpeculationRulesFilterTest : public RewriteTestBase {
 protected:
  // The exact script tag the filter injects.
  GoogleString ScriptTag() const {
    return StrCat("<script type=\"speculationrules\">", kSpeculationRulesJson,
                  "</script>");
  }

  // kHtmlInput with the ruleset injected at the end of the body.
  GoogleString ExpectedOutput() const {
    return StrCat(
        "<head>\n"
        "<title>Something</title>\n"
        "</head>"
        "<body> Hello World!",
        ScriptTag(), "</body>");
  }

  ResponseHeaders response_headers_;
};

TEST_F(InsertSpeculationRulesFilterTest, InjectsAtBodyEnd) {
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  ValidateExpected("inject_at_body_end", kHtmlInput, ExpectedOutput());
}

TEST_F(InsertSpeculationRulesFilterTest, NoDoubleInjection) {
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  // Pre-existing ruleset in the head.
  ValidateNoChanges("existing_in_head",
                    "<head>\n"
                    "<script type=\"speculationrules\">"
                    "{\"prerender\":[]}</script>\n"
                    "<title>Something</title>\n"
                    "</head>"
                    "<body> Hello World!</body>");
  // Pre-existing ruleset in the body.
  ValidateNoChanges("existing_in_body",
                    "<head>\n"
                    "<title>Something</title>\n"
                    "</head>"
                    "<body> Hello World!"
                    "<script type=\"speculationrules\">"
                    "{\"prerender\":[]}</script>"
                    "</body>");
  // Case-variant type attribute still counts as an existing ruleset.
  ValidateNoChanges("existing_case_variant",
                    "<head>\n"
                    "<title>Something</title>\n"
                    "</head>"
                    "<body> Hello World!"
                    "<script type=\"SpeculationRules\">"
                    "{\"prerender\":[]}</script>"
                    "</body>");
  // Whitespace around the attribute value is trimmed before matching.
  ValidateNoChanges("existing_whitespace_variant",
                    "<head>\n"
                    "<title>Something</title>\n"
                    "</head>"
                    "<body> Hello World!"
                    "<script type=\" speculationrules \">"
                    "{\"prerender\":[]}</script>"
                    "</body>");
}

TEST_F(InsertSpeculationRulesFilterTest, AgentRequestSuppressed) {
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kAccept, "text/markdown");
  rewrite_driver()->SetRequestHeaders(request_headers);
  ValidateNoChanges("agent_request", kHtmlInput);
}

TEST_F(InsertSpeculationRulesFilterTest, NonMarkdownAcceptStillInjects) {
  // Positive control for AgentRequestSuppressed: an ordinary browser Accept
  // header does not suppress the injection ("*/*" must not match).
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kAccept, "text/html, */*");
  rewrite_driver()->SetRequestHeaders(request_headers);
  ValidateExpected("browser_accept", kHtmlInput, ExpectedOutput());
}

TEST_F(InsertSpeculationRulesFilterTest, SetCookieSuppresses) {
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  response_headers_.set_status_code(HttpStatus::kOK);
  response_headers_.Add(HttpAttributes::kSetCookie, "id=1");
  rewrite_driver()->set_response_headers_ptr(&response_headers_);
  ValidateNoChanges("set_cookie", kHtmlInput);
}

TEST_F(InsertSpeculationRulesFilterTest, SetCookie2Suppresses) {
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  response_headers_.set_status_code(HttpStatus::kOK);
  response_headers_.Add(HttpAttributes::kSetCookie2, "id=1");
  rewrite_driver()->set_response_headers_ptr(&response_headers_);
  ValidateNoChanges("set_cookie2", kHtmlInput);
}

TEST_F(InsertSpeculationRulesFilterTest, NoStoreSuppresses) {
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  response_headers_.set_status_code(HttpStatus::kOK);
  // Comma-separated and case-variant: the value is split on commas when
  // added, so the filter sees a separate "No-Store" token.
  response_headers_.Add(HttpAttributes::kCacheControl, "no-cache, No-Store");
  rewrite_driver()->set_response_headers_ptr(&response_headers_);
  ValidateNoChanges("no_store", kHtmlInput);
}

TEST_F(InsertSpeculationRulesFilterTest, OkResponseInjects) {
  // Positive control for the response-header gates: a plain cacheable 200
  // (no-cache alone is fine, only no-store suppresses) still injects.
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  response_headers_.set_status_code(HttpStatus::kOK);
  response_headers_.Add(HttpAttributes::kCacheControl, "no-cache");
  rewrite_driver()->set_response_headers_ptr(&response_headers_);
  ValidateExpected("ok_response", kHtmlInput, ExpectedOutput());
}

TEST_F(InsertSpeculationRulesFilterTest, NonOkStatusSuppresses) {
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  response_headers_.set_status_code(HttpStatus::kNotFound);
  rewrite_driver()->set_response_headers_ptr(&response_headers_);
  ValidateNoChanges("non_ok_status", kHtmlInput);
}

TEST_F(InsertSpeculationRulesFilterTest, OptInGating) {
  // The filter is opt-in: it must not run without an explicit EnableFilter
  // (this fixture's default level does not include it, and the filter is not
  // a member of any level's filter set).
  rewrite_driver()->AddFilters();
  ValidateNoChanges("not_enabled", kHtmlInput);
}

TEST_F(InsertSpeculationRulesFilterTest, CspSuppresses) {
  // The ruleset is an inline script, which a script-src policy without
  // 'unsafe-inline' would block; insert nothing.
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  ValidateNoChanges("csp_no_inline",
                    "<head>\n"
                    "<meta http-equiv=\"Content-Security-Policy\" "
                    "content=\"script-src *;\">"
                    "<title>Something</title>\n"
                    "</head>"
                    "<body> Hello World!</body>");
}

TEST_F(InsertSpeculationRulesFilterTest, CspPermissiveInjects) {
  // With 'unsafe-inline' permitted the filter behaves as usual.
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  const char kCsp[] =
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src * 'unsafe-inline';\">";
  GoogleString input = StrCat("<head>\n", kCsp,
                              "<title>Something</title>\n"
                              "</head>"
                              "<body> Hello World!</body>");
  GoogleString expected = StrCat("<head>\n", kCsp,
                                 "<title>Something</title>\n"
                                 "</head>"
                                 "<body> Hello World!",
                                 ScriptTag(), "</body>");
  ValidateExpected("csp_unsafe_inline", input, expected);
}

TEST_F(InsertSpeculationRulesFilterTest, UnclosedBody) {
  // With no </body> the ruleset is injected at the end of the stream.
  AddFilter(RewriteOptions::kInsertSpeculationRules);
  ValidateExpected("no_close_body", kHtmlNoCloseBody,
                   StrCat(kHtmlNoCloseBody, ScriptTag()));
}

// Fixture with the harness <html> wrapper disabled: the wrapper's plain
// <html> tag would make AmpDocumentFilter latch not-amp before ever seeing a
// test's <html amp> tag (same reason PrioritizeCriticalImagesFilterTest
// disables it).
class InsertSpeculationRulesFilterAmpTest
    : public InsertSpeculationRulesFilterTest {
 protected:
  bool AddHtmlTags() const override { return false; }
};

TEST_F(InsertSpeculationRulesFilterAmpTest, AmpDocumentIsNoOp) {
  // Use the real registration path so the AMP document detector runs.
  AddFilter(RewriteOptions::kInsertSpeculationRules);

  // <script type="speculationrules"> is not valid in AMP documents (only the
  // AMP runtime and application/ld+json scripts are allowed), so the filter
  // must leave AMP pages untouched.
  ValidateNoChanges("amp_no_op",
                    "<html amp><head></head>"
                    "<body>Hello World!</body></html>");
  EXPECT_TRUE(rewrite_driver()->is_amp_document());

  // The same page without the amp attribute still gets the ruleset.
  ValidateExpected("non_amp_injects",
                   "<html><head></head>"
                   "<body>Hello World!</body></html>",
                   StrCat("<html><head></head>"
                          "<body>Hello World!",
                          ScriptTag(), "</body></html>"));
}

TEST_F(InsertSpeculationRulesFilterTest, DeferJsCoexistence) {
  // defer_javascript must leave the injected tag alone: type=speculationrules
  // is not a JavaScript mimetype, so the defer machinery must not rewrite it.
  options()->EnableFilter(RewriteOptions::kInsertSpeculationRules);
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  rewrite_driver()->AddFilters();
  ParseUrl("http://example.com/defer.html", kHtmlInput);
  GoogleString script_tag = ScriptTag();
  size_t first = output_buffer_.find(script_tag);
  EXPECT_NE(GoogleString::npos, first) << output_buffer_;
  // Exactly once, and untouched (no orig-type/psajs rewriting of our tag).
  EXPECT_EQ(first, output_buffer_.rfind(script_tag));
  EXPECT_EQ(
      GoogleString::npos,
      output_buffer_.find("data-pagespeed-orig-type=\"speculationrules\""));
}

}  // namespace

}  // namespace net_instaweb
