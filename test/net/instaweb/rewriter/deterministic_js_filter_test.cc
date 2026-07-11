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

#include "net/instaweb/rewriter/public/deterministic_js_filter.h"

#include <memory>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class DeterministicJsFilterTest : public RewriteTestBase {
 public:
  DeterministicJsFilterTest() {}

 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    SetHtmlMimetype();  // Prevent insertion of CDATA tags to static JS.
    deterministic_js_filter_ =
        std::make_unique<DeterministicJsFilter>(rewrite_driver());
    rewrite_driver()->AddFilter(deterministic_js_filter_.get());
  }

 private:
  std::unique_ptr<DeterministicJsFilter> deterministic_js_filter_;

  DeterministicJsFilterTest(const DeterministicJsFilterTest&) = delete;
  DeterministicJsFilterTest& operator=(const DeterministicJsFilterTest&) = delete;
};

TEST_F(DeterministicJsFilterTest, DeterministicJsInjection) {
  StringPiece deterministic_js_code =
      server_context()->static_asset_manager()->GetAsset(
          StaticAssetEnum::DETERMINISTIC_JS, options());
  GoogleString expected_str = StrCat(
      "<head><script type=\"text/javascript\" "
      "data-pagespeed-no-defer>",
      deterministic_js_code, "</script></head><body></body>");

  // Check if StaticAssetManager populated the script correctly.
  EXPECT_NE(GoogleString::npos, deterministic_js_code.find("Date"));
  EXPECT_NE(GoogleString::npos, deterministic_js_code.find("Math.random"));
  // Check if the deterministic js is inserted correctly.
  ValidateExpected("deterministicJs_injection", "<head></head><body></body>",
                   expected_str);
}

TEST_F(DeterministicJsFilterTest, CspForbidsInlineScript) {
  // The Date()/random() overrides are an inline script, which a
  // script-src policy without 'unsafe-inline' would block; the page must
  // be left untouched.
  ValidateNoChanges("csp_no_inline",
                    "<head>"
                    "<meta http-equiv=\"Content-Security-Policy\" "
                    "content=\"script-src *;\">"
                    "</head><body></body>");
}

TEST_F(DeterministicJsFilterTest, CspAllowsInlineScript) {
  // With 'unsafe-inline' permitted the filter behaves as usual. The
  // script is injected at the start of <head>, before the policy tag.
  const char kCsp[] =
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src * 'unsafe-inline';\">";
  StringPiece deterministic_js_code =
      server_context()->static_asset_manager()->GetAsset(
          StaticAssetEnum::DETERMINISTIC_JS, options());
  GoogleString expected_str =
      StrCat("<head><script type=\"text/javascript\" "
             "data-pagespeed-no-defer>",
             deterministic_js_code, "</script>", kCsp, "</head><body></body>");
  ValidateExpected("csp_unsafe_inline",
                   StrCat("<head>", kCsp, "</head><body></body>"),
                   expected_str);
}

TEST_F(DeterministicJsFilterTest, DeterministicJsInjectionWithSomeHeadContent) {
  StringPiece deterministic_js_code =
      server_context()->static_asset_manager()->GetAsset(
          StaticAssetEnum::DETERMINISTIC_JS, options());
  GoogleString expected_str = StrCat(
      "<head><script type=\"text/javascript\" "
      "data-pagespeed-no-defer>",
      deterministic_js_code,
      "</script>"
      "<link rel=\"stylesheet\" href=\"a.css\">"
      "</head><body></body>");

  // Check if StaticAssetManager populated the script correctly.
  EXPECT_NE(GoogleString::npos, deterministic_js_code.find("Date"));
  EXPECT_NE(GoogleString::npos, deterministic_js_code.find("Math.random"));
  // Check if the deterministic js is inserted correctly.
  ValidateExpected("deterministicJs_injection",
                   "<head><link rel=\"stylesheet\" href=\"a.css\">"
                   "</head><body></body>",
                   expected_str);
}
}  // namespace net_instaweb
