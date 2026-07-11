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

#include "net/instaweb/rewriter/public/lazyload_images_filter.h"

#include <memory>

#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/rewriter/public/critical_images_beacon_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/escaping.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "pagespeed/opt/logging/log_record.h"
#include "test/net/instaweb/rewriter/mock_critical_images_finder.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

namespace net_instaweb {

namespace {

// A user agent modern enough to support the native loading="lazy" attribute
// (Chromium >= 77); auto mode selects native for it.
const char kChrome120UserAgent[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";

}  // namespace

class LazyloadImagesFilterTest : public RewriteTestBase {
 protected:
  LazyloadImagesFilterTest() : blank_image_src_("/psajs/1.0.gif") {}

  // TODO(matterbury): Delete this method as it should be redundant.
  void SetUp() override {
    RewriteTestBase::SetUp();
    SetCurrentUserAgent(UserAgentMatcherTestBase::kChrome18UserAgent);
    SetHtmlMimetype();  // Prevent insertion of CDATA tags to static JS.
  }

  virtual void InitLazyloadImagesFilter(bool debug) {
    // Most tests in this file predate LazyloadImagesSkipFirst; disable the
    // LCP protection so they exercise the unskipped behavior. Dedicated
    // tests cover the skip-first logic.
    InitLazyloadImagesFilterWithSkipFirst(debug, 0);
  }

  void InitLazyloadImagesFilterWithSkipFirst(bool debug, int skip_first) {
    if (debug) {
      options()->EnableFilter(RewriteOptions::kDebug);
    }
    options()->DisallowTroublesomeResources();
    options()->set_lazyload_images_skip_first(skip_first);
    lazyload_images_filter_ =
        std::make_unique<LazyloadImagesFilter>(rewrite_driver());
    rewrite_driver()->AddFilter(lazyload_images_filter_.get());
  }

  GoogleString GenerateRewrittenImageTag(
      const StringPiece& tag, const StringPiece& url,
      const StringPiece& additional_attributes) {
    return StrCat("<", tag, " data-pagespeed-lazy-src=\"", url, "\" ",
                  additional_attributes, "src=\"", blank_image_src_,
                  "\" onload=\"", LazyloadImagesFilter::kImageOnloadCode,
                  "\" onerror=\"this.onerror=null;",
                  LazyloadImagesFilter::kImageOnloadCode, "\"/>");
  }

  void ExpectLogRecord(int index, int status, bool is_blacklisted,
                       bool is_critical) {
    const RewriterInfo& rewriter_info = logging_info()->rewriter_info(index);
    EXPECT_EQ("ll", rewriter_info.id());
    EXPECT_EQ(status, rewriter_info.status());
    EXPECT_EQ(is_blacklisted,
              rewriter_info.rewrite_resource_info().is_blacklisted());
    EXPECT_EQ(is_critical, rewriter_info.rewrite_resource_info().is_critical());
  }

  int64 StatValue(const char* name) {
    return statistics()->GetVariable(name)->Get();
  }

  GoogleString blank_image_src_;
  std::unique_ptr<LazyloadImagesFilter> lazyload_images_filter_;
};

TEST_F(LazyloadImagesFilterTest, SingleHead) {
  InitLazyloadImagesFilter(false);

  ValidateExpected(
      "lazyload_images",
      "<head></head>"
      "<body>"
      "<img />"
      "<img src=\"\" />"
      "<noscript>"
      "<img src=\"noscript.jpg\" />"
      "</noscript>"
      "<noembed>"
      "<img src=\"noembed.jpg\" />"
      "</noembed>"
      "<marquee>"
      "<img src=\"marquee.jpg\" />"
      "</marquee>"
      "<img src=\"1.jpg\" />"
      "<img src=\"1.jpg\" data-pagespeed-no-defer/>"
      "<img src=\"1.jpg\" pagespeed_no_defer/>"
      "<img src=\"1.jpg\" data-src=\"2.jpg\"/>"
      "<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhE\"/>"
      "<img src=\"2's.jpg\" height=\"300\" width=\"123\" />"
      "<input src=\"12.jpg\"type=\"image\" />"
      "<input src=\"12.jpg\" />"
      "<img src=\"1.jpg\" onload=\"blah();\" />"
      "<img src=\"1.jpg\" class=\"123 dfcg-metabox\" />"
      "</body>",
      StrCat("<head></head><body><img/>"
             "<img src=\"\"/>"
             "<noscript>"
             "<img src=\"noscript.jpg\"/>"
             "</noscript>",
             "<noembed>"
             "<img src=\"noembed.jpg\"/>"
             "</noembed>"
             "<marquee>"
             "<img src=\"marquee.jpg\"/>"
             "</marquee>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "1.jpg", ""),
             "<img src=\"1.jpg\" data-pagespeed-no-defer />"
             "<img src=\"1.jpg\" pagespeed_no_defer />"
             "<img src=\"1.jpg\" data-src=\"2.jpg\"/>",
             "<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhE\"/>",
             GenerateRewrittenImageTag("img", "2's.jpg",
                                       "height=\"300\" width=\"123\" "),
             "<input src=\"12.jpg\" type=\"image\"/>"
             "<input src=\"12.jpg\"/>"
             "<img src=\"1.jpg\" onload=\"blah();\"/>"
             "<img src=\"1.jpg\" class=\"123 dfcg-metabox\"/>",
             GetLazyloadPostscriptHtml(), "</body>"));
  EXPECT_EQ(4, logging_info()->rewriter_info().size());
  ExpectLogRecord(0, RewriterApplication::APPLIED_OK /* img with src 1.jpg */,
                  false, false);
  ExpectLogRecord(
      1, RewriterApplication::NOT_APPLIED /* img with src 1.jpg and data-src */,
      false, false);
  ExpectLogRecord(2, RewriterApplication::APPLIED_OK /* img with src 2's.jpg*/,
                  false, false);
  ExpectLogRecord(
      3, RewriterApplication::NOT_APPLIED /* img with src 1.jpg and onload */,
      false, false);
  EXPECT_EQ(2, StatValue(LazyloadImagesFilter::kLazyloadImagesApplied));
  EXPECT_EQ(0, StatValue(LazyloadImagesFilter::kLazyloadImagesNativeApplied));
}

TEST_F(LazyloadImagesFilterTest, Blacklist) {
  options()->Disallow("*blacklist*");
  InitLazyloadImagesFilter(false);

  GoogleString input_html =
      "<head></head>"
      "<body>"
      "<img src=\"http://www.1.com/blacklist.jpg\"/>"
      "<img src=\"http://www.1.com/img1\"/>"
      "<img src=\"img2\"/>"
      "</body>";

  ValidateExpected(
      "lazyload_images", input_html,
      StrCat("<head></head><body>"
             "<img src=\"http://www.1.com/blacklist.jpg\"/>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "http://www.1.com/img1", ""),
             GenerateRewrittenImageTag("img", "img2", ""),
             GetLazyloadPostscriptHtml(), "</body>"));
  EXPECT_EQ(3, logging_info()->rewriter_info().size());
  ExpectLogRecord(0, RewriterApplication::NOT_APPLIED, true, false);
  ExpectLogRecord(1, RewriterApplication::APPLIED_OK, false, false);
  ExpectLogRecord(2, RewriterApplication::APPLIED_OK, false, false);
}

TEST_F(LazyloadImagesFilterTest, CriticalImages) {
  InitLazyloadImagesFilter(false);
  MockCriticalImagesFinder* finder = new MockCriticalImagesFinder(statistics());
  server_context()->set_critical_images_finder(finder);

  StringSet* critical_images = new StringSet;
  critical_images->insert("http://www.1.com/critical");
  critical_images->insert("www.1.com/critical2");
  critical_images->insert("http://test.com/critical3");
  critical_images->insert("http://test.com/critical4.jpg");
  finder->set_critical_images(critical_images);

  GoogleString rewritten_url =
      Encode("http://test.com/", "ce", "HASH", "critical4.jpg", "jpg");

  GoogleString input_html = StrCat(
      "<head></head>"
      "<body>"
      "<img src=\"http://www.1.com/critical\"/>"
      "<img src=\"http://www.1.com/critical2\"/>"
      "<img src=\"critical3\"/>"
      "<img src=\"",
      rewritten_url,
      "\"/>"
      "</body>");

  ValidateExpected(
      "lazyload_images", input_html,
      StrCat("<head></head><body>"
             "<img src=\"http://www.1.com/critical\"/>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "http://www.1.com/critical2", ""),
             "<img src=\"critical3\"/>"
             "<img src=\"",
             rewritten_url, "\"/>", GetLazyloadPostscriptHtml(), "</body>"));
  EXPECT_EQ(4, logging_info()->rewriter_info().size());
  ExpectLogRecord(0, RewriterApplication::NOT_APPLIED, false, true);
  ExpectLogRecord(1, RewriterApplication::APPLIED_OK, false, false);
  ExpectLogRecord(2, RewriterApplication::NOT_APPLIED, false, true);
  ExpectLogRecord(3, RewriterApplication::NOT_APPLIED, false, true);
  EXPECT_EQ(-1, logging_info()->num_html_critical_images());
  EXPECT_EQ(-1, logging_info()->num_css_critical_images());
  EXPECT_EQ(3,
            StatValue(LazyloadImagesFilter::kLazyloadImagesSkippedCritical));
  rewrite_driver_->log_record()->WriteLog();
  for (int i = 0; i < logging_info()->rewriter_stats_size(); i++) {
    if (logging_info()->rewriter_stats(i).id() == "ll" &&
        logging_info()->rewriter_stats(i).has_html_status()) {
      EXPECT_EQ(RewriterHtmlApplication::ACTIVE,
                logging_info()->rewriter_stats(i).html_status());
      const RewriteStatusCount& count_applied =
          logging_info()->rewriter_stats(i).status_counts(0);
      EXPECT_EQ(RewriterApplication::APPLIED_OK,
                count_applied.application_status());
      EXPECT_EQ(1, count_applied.count());
      const RewriteStatusCount& count_not_applied =
          logging_info()->rewriter_stats(i).status_counts(1);
      EXPECT_EQ(RewriterApplication::NOT_APPLIED,
                count_not_applied.application_status());
      EXPECT_EQ(3, count_not_applied.count());
      return;
    }
  }
  FAIL();
}

TEST_F(LazyloadImagesFilterTest, SingleHeadLoadOnOnload) {
  options()->set_lazyload_images_after_onload(true);
  InitLazyloadImagesFilter(false);
  ValidateExpected("lazyload_images",
                   "<head></head>"
                   "<body>"
                   "<img src=\"1.jpg\" />"
                   "</body>",
                   StrCat("<head></head>"
                          "<body>",
                          GetLazyloadScriptHtml(),
                          GenerateRewrittenImageTag("img", "1.jpg", ""),
                          GetLazyloadPostscriptHtml(), "</body>"));
}

// Verify that lazyload_images does not get applied on image elements that have
// an onload handler defined for them whose value does not match the
// CriticalImagesBeaconFilter::kImageOnloadCode, indicating that this is
// not an onload attribute added by PageSpeed. Since no image is rewritten,
// no loader script is injected either.
TEST_F(LazyloadImagesFilterTest, NoLazyloadImagesWithOnloadAttribute) {
  InitLazyloadImagesFilter(false);
  ValidateNoChanges("lazyload_images",
                    "<head></head>"
                    "<body>"
                    "<img src=\"1.jpg\" onload=\"do_something();\"/>"
                    "</body>");
}

// Verify that lazyload_images gets applied on image elements that have an
// onload handler whose value is CriticalImagesBeaconFilter::kImageOnloadCode.
TEST_F(LazyloadImagesFilterTest, LazyloadWithPagespeedAddedOnloadAttribute) {
  InitLazyloadImagesFilter(false);
  ValidateExpected("lazyload_images",
                   StrCat("<head></head>"
                          "<body>"
                          "<img src=\"1.jpg\" onload=\"",
                          CriticalImagesBeaconFilter::kImageOnloadCode,
                          "\"/>"
                          "</body>"),
                   StrCat("<head></head>"
                          "<body>",
                          GetLazyloadScriptHtml(),
                          GenerateRewrittenImageTag("img", "1.jpg", ""),
                          GetLazyloadPostscriptHtml(), "</body>"));
}

TEST_F(LazyloadImagesFilterTest, MultipleBodies) {
  InitLazyloadImagesFilter(false);
  ValidateExpected(
      "lazyload_images",
      "<body><img src=\"1.jpg\" /></body>"
      "<body></body>"
      "<body>"
      "<script></script>"
      "<img src=\"2.jpg\" />"
      "<script></script>"
      "<img src=\"3.jpg\" />"
      "<script></script>"
      "</body>",
      StrCat("<body>", GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "1.jpg", ""),
             GetLazyloadPostscriptHtml(),
             "</body><body></body><body>"
             "<script></script>",
             GenerateRewrittenImageTag("img", "2.jpg", ""),
             GetLazyloadPostscriptHtml(), "<script></script>",
             GenerateRewrittenImageTag("img", "3.jpg", ""),
             GetLazyloadPostscriptHtml(), "<script></script>", "</body>"));
}

TEST_F(LazyloadImagesFilterTest, NoHeadTag) {
  InitLazyloadImagesFilter(false);
  ValidateExpected("lazyload_images",
                   "<body>"
                   "<img src=\"1.jpg\" />"
                   "</body>",
                   StrCat("<body>", GetLazyloadScriptHtml(),
                          GenerateRewrittenImageTag("img", "1.jpg", ""),
                          GetLazyloadPostscriptHtml(), "</body>"));
}

TEST_F(LazyloadImagesFilterTest, LazyloadImagesPreserveURLsOn) {
  // Make sure that we do not lazyload images when preserve urls is off.
  // This is a modification of the NoHeadTag test.
  options()->set_image_preserve_urls(true);
  options()->set_support_noscript_enabled(false);
  options()->SoftEnableFilterForTesting(RewriteOptions::kLazyloadImages);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("lazyload_images",
                    "<body>"
                    "<img src=\"1.jpg\"/>"
                    "</body>");
}

TEST_F(LazyloadImagesFilterTest, CustomImageUrl) {
  GoogleString blank_image_url = "http://blank.com/1.gif";
  options()->set_lazyload_images_blank_url(blank_image_url);
  blank_image_src_ = blank_image_url;
  InitLazyloadImagesFilter(false);
  ValidateExpected("lazyload_images",
                   "<body>"
                   "<img src=\"1.jpg\" />"
                   "</body>",
                   StrCat("<body>", GetLazyloadScriptHtml(),
                          GenerateRewrittenImageTag("img", "1.jpg", ""),
                          GetLazyloadPostscriptHtml(), "</body>"));
}

// The configurable blank image url is spliced into the inline loader script,
// so it must be escaped as a JS string literal.
TEST_F(LazyloadImagesFilterTest, BlankImageUrlIsJsEscaped) {
  GoogleString blank_image_url = "http://blank.com/1\".gif";
  options()->set_lazyload_images_blank_url(blank_image_url);
  InitLazyloadImagesFilter(false);
  Parse("blank_url_escaping",
        "<head></head><body><img src=\"1.jpg\"></body>");
  GoogleString escaped;
  EscapeToJsStringLiteral(blank_image_url, true /* add_quotes */, &escaped);
  EXPECT_NE(GoogleString::npos, output_buffer_.find(escaped))
      << "The blank image url should appear JS-escaped in the loader script";
  EXPECT_EQ(GoogleString::npos,
            output_buffer_.find(StrCat("\"", blank_image_url, "\"")))
      << "The raw blank image url must not appear unescaped in a script";
}

TEST_F(LazyloadImagesFilterTest, DfcgClass) {
  InitLazyloadImagesFilter(false);
  ValidateNoChanges("DfcgClass",
                    "<body class=\"dfcg-slideshow\">"
                    "<img src=\"1.jpg\"/>"
                    "<div class=\"dfcg\">"
                    "<img src=\"1.jpg\"/>"
                    "</div>"
                    "</body>");
}

TEST_F(LazyloadImagesFilterTest, NivoClass) {
  InitLazyloadImagesFilter(false);
  ValidateNoChanges("NivoClass",
                    "<body>"
                    "<div class=\"nivo_sl\">"
                    "<img src=\"1.jpg\"/>"
                    "</div>"
                    "<img class=\"nivo\" src=\"1.jpg\"/>"
                    "</body>");
}

TEST_F(LazyloadImagesFilterTest, ClassContainsSlider) {
  InitLazyloadImagesFilter(false);
  ValidateNoChanges("SliderClass",
                    "<body>"
                    "<div class=\"SliderName2\">"
                    "<img src=\"1.jpg\"/>"
                    "</div>"
                    "<img class=\"my_sLiDer\" src=\"1.jpg\"/>"
                    "</body>");
}

// With lazy script injection, pages without eligible images stay untouched.
TEST_F(LazyloadImagesFilterTest, NoImages) {
  InitLazyloadImagesFilter(false);
  ValidateNoChanges("NoImages", "<head></head><body></body>");
  EXPECT_EQ(0, logging_info()->rewriter_info().size());
}

TEST_F(LazyloadImagesFilterTest, LazyloadScriptOptimized) {
  InitLazyloadImagesFilter(false);
  Parse("optimized", "<head></head><body><img src=\"1.jpg\"></body>");
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("/*"))
      << "There should be no comments in the optimized code";
}

TEST_F(LazyloadImagesFilterTest, LazyloadScriptDebug) {
  InitLazyloadImagesFilter(true);
  Parse("debug", "<head></head><body><img src=\"1.jpg\"></body>");
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("/*"))
      << "There should be no comments in the debug code";
}

TEST_F(LazyloadImagesFilterTest, LazyloadDisabledWithJquerySlider) {
  InitLazyloadImagesFilter(false);
  // No change in the html: the jquery slider script aborts the rewrite
  // before any image is touched, so no loader script is injected either.
  ValidateNoChanges("JQuerySlider",
                    "<body>"
                    "<head>"
                    "<script src=\"jquery.sexyslider.js\"/>"
                    "</head>"
                    "<body>"
                    "<img src=\"1.jpg\"/>"
                    "</body>");
}

TEST_F(LazyloadImagesFilterTest, LazyloadDisabledWithJquerySliderAfterHead) {
  InitLazyloadImagesFilter(false);
  ValidateNoChanges("abort_script_inserted",
                    "<head>"
                    "</head>"
                    "<body>"
                    "<script src=\"jquery.sexyslider.js\"/>"
                    "<img src=\"1.jpg\"/>"
                    "</body>");
}

TEST_F(LazyloadImagesFilterTest, LazyloadDisabledForOldBlackberry) {
  SetCurrentUserAgent(UserAgentMatcherTestBase::kBlackBerryOS5UserAgent);
  InitLazyloadImagesFilter(false);
  GoogleString input_html =
      "<head>"
      "</head>"
      "<body>"
      "<img src=\"1.jpg\"/>"
      "</body>";
  ValidateNoChanges("blackberry_useragent", input_html);
}

TEST_F(LazyloadImagesFilterTest, LazyloadDisabledForGooglebot) {
  SetCurrentUserAgent(UserAgentMatcherTestBase::kGooglebotUserAgent);
  InitLazyloadImagesFilter(false);
  GoogleString input_html =
      "<head>"
      "</head>"
      "<body>"
      "<img src=\"1.jpg\"/>"
      "</body>";
  ValidateNoChanges("googlebot_useragent", input_html);
  rewrite_driver_->log_record()->WriteLog();
  LoggingInfo* logging_info = rewrite_driver_->log_record()->logging_info();
  for (int i = 0; i < logging_info->rewriter_stats_size(); i++) {
    if (logging_info->rewriter_stats(i).id() == "ll" &&
        logging_info->rewriter_stats(i).has_html_status()) {
      EXPECT_EQ(RewriterHtmlApplication::USER_AGENT_NOT_SUPPORTED,
                logging_info->rewriter_stats(i).html_status());
      return;
    }
  }
  FAIL();
}

TEST_F(LazyloadImagesFilterTest, LazyloadDisabledForXHR) {
  InitLazyloadImagesFilter(false);
  AddRequestAttribute(HttpAttributes::kXRequestedWith,
                      HttpAttributes::kXmlHttpRequest);
  GoogleString input_html =
      "<head>"
      "</head>"
      "<body>"
      "<img src=\"1.jpg\"/>"
      "</body>";
  ValidateNoChanges("xhr_requests", input_html);
  rewrite_driver_->log_record()->WriteLog();
  LoggingInfo* logging_info = rewrite_driver_->log_record()->logging_info();
  for (int i = 0; i < logging_info->rewriter_stats_size(); i++) {
    if (logging_info->rewriter_stats(i).id() == "ll" &&
        logging_info->rewriter_stats(i).has_html_status()) {
      EXPECT_EQ(RewriterHtmlApplication::DISABLED,
                logging_info->rewriter_stats(i).html_status());
      EXPECT_TRUE(logging_info->has_is_xhr());
      EXPECT_TRUE(logging_info->is_xhr());
      return;
    }
  }
  FAIL();
}

// A Content-Security-Policy that forbids inline script disables the js
// machinery: the blanked-out images would never be restored.
TEST_F(LazyloadImagesFilterTest, CspBlocksJsMode) {
  InitLazyloadImagesFilter(false);
  ValidateNoChanges(
      "lazyload_csp_no_inline",
      "<head><meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src 'self';\"></head>"
      "<body><img src=\"1.jpg\"/></body>");
  EXPECT_EQ(1, StatValue(LazyloadImagesFilter::kLazyloadImagesSkippedCsp));
  EXPECT_EQ(0, StatValue(LazyloadImagesFilter::kLazyloadImagesApplied));
}

// 'unsafe-inline' permits the inline loader script and the inline
// onload/onerror handlers, so js mode applies normally.
TEST_F(LazyloadImagesFilterTest, CspUnsafeInlineAllowsJsMode) {
  InitLazyloadImagesFilter(false);
  static const char kCsp[] =
      "<head><meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src * 'unsafe-inline';\"></head>";
  ValidateExpected(
      "lazyload_csp_unsafe_inline",
      StrCat(kCsp, "<body><img src=\"1.jpg\"/></body>"),
      StrCat(kCsp, "<body>", GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "1.jpg", ""),
             GetLazyloadPostscriptHtml(), "</body>"));
  EXPECT_EQ(0, StatValue(LazyloadImagesFilter::kLazyloadImagesSkippedCsp));
}

// Native mode injects no JavaScript, so it stays enabled under a strict CSP.
TEST_F(LazyloadImagesFilterTest, CspDoesNotDisableNativeMode) {
  options()->set_lazyload_images_mode(
      RewriteOptions::kLazyloadImagesModeNative);
  InitLazyloadImagesFilter(false);
  static const char kCsp[] =
      "<head><meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src 'self';\"></head>";
  ValidateExpected(
      "lazyload_csp_native",
      StrCat(kCsp, "<body><img src=\"1.jpg\"/></body>"),
      StrCat(kCsp,
             "<body><img src=\"1.jpg\" loading=\"lazy\" decoding=\"async\"/>"
             "</body>"));
  EXPECT_EQ(0, StatValue(LazyloadImagesFilter::kLazyloadImagesSkippedCsp));
  EXPECT_EQ(1, StatValue(LazyloadImagesFilter::kLazyloadImagesNativeApplied));
}

TEST_F(LazyloadImagesFilterTest, NativeModeAddsAttributes) {
  options()->set_lazyload_images_mode(
      RewriteOptions::kLazyloadImagesModeNative);
  InitLazyloadImagesFilter(false);
  ValidateExpected(
      "native_mode",
      "<head></head>"
      "<body>"
      "<img src=\"1.jpg\"/>"
      "<img src=\"2.jpg\" srcset=\"2.jpg 1x, 2x.jpg 2x\"/>"
      "<img src=\"3.jpg\" decoding=\"sync\"/>"
      "</body>",
      "<head></head>"
      "<body>"
      "<img src=\"1.jpg\" loading=\"lazy\" decoding=\"async\"/>"
      "<img src=\"2.jpg\" srcset=\"2.jpg 1x, 2x.jpg 2x\" loading=\"lazy\" "
      "decoding=\"async\"/>"
      "<img src=\"3.jpg\" decoding=\"sync\" loading=\"lazy\"/>"
      "</body>");
  EXPECT_EQ(3, StatValue(LazyloadImagesFilter::kLazyloadImagesNativeApplied));
  EXPECT_EQ(0, StatValue(LazyloadImagesFilter::kLazyloadImagesApplied));
}

TEST_F(LazyloadImagesFilterTest, NativeModeCriticalGetsFetchPriority) {
  options()->set_lazyload_images_mode(
      RewriteOptions::kLazyloadImagesModeNative);
  InitLazyloadImagesFilter(false);
  MockCriticalImagesFinder* finder = new MockCriticalImagesFinder(statistics());
  server_context()->set_critical_images_finder(finder);
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  finder->set_critical_images(critical_images);
  ValidateExpected(
      "native_critical",
      "<head></head>"
      "<body>"
      "<img src=\"critical.jpg\"/>"
      "<img src=\"other.jpg\"/>"
      "</body>",
      "<head></head>"
      "<body>"
      "<img src=\"critical.jpg\" fetchpriority=\"high\" decoding=\"async\"/>"
      "<img src=\"other.jpg\" loading=\"lazy\" decoding=\"async\"/>"
      "</body>");
  EXPECT_EQ(1, StatValue(LazyloadImagesFilter::kLazyloadImagesNativeApplied));
  EXPECT_EQ(1,
            StatValue(LazyloadImagesFilter::kLazyloadImagesSkippedCritical));
}

// Auto mode selects native for a user agent supporting loading="lazy".
TEST_F(LazyloadImagesFilterTest, AutoModeUsesNativeForModernUa) {
  SetCurrentUserAgent(kChrome120UserAgent);
  InitLazyloadImagesFilter(false);
  ValidateExpected(
      "auto_native",
      "<body><img src=\"1.jpg\"/></body>",
      "<body><img src=\"1.jpg\" loading=\"lazy\" decoding=\"async\"/></body>");
  EXPECT_EQ(1, StatValue(LazyloadImagesFilter::kLazyloadImagesNativeApplied));
  EXPECT_EQ(0, StatValue(LazyloadImagesFilter::kLazyloadImagesApplied));
}

// Auto mode falls back to the js machinery for older user agents; the SetUp
// user agent (Chrome 18) predates native lazyload support.
TEST_F(LazyloadImagesFilterTest, AutoModeUsesJsForOldUa) {
  InitLazyloadImagesFilter(false);
  ValidateExpected("auto_js",
                   "<body><img src=\"1.jpg\"/></body>",
                   StrCat("<body>", GetLazyloadScriptHtml(),
                          GenerateRewrittenImageTag("img", "1.jpg", ""),
                          GetLazyloadPostscriptHtml(), "</body>"));
  EXPECT_EQ(0, StatValue(LazyloadImagesFilter::kLazyloadImagesNativeApplied));
  EXPECT_EQ(1, StatValue(LazyloadImagesFilter::kLazyloadImagesApplied));
}

// When critical image data is unavailable (beaconing disabled), the first
// LazyloadImagesSkipFirst eligible images are left untouched to protect the
// likely LCP image.
TEST_F(LazyloadImagesFilterTest, SkipFirstImageWhenNoCriticalImageData) {
  InitLazyloadImagesFilterWithSkipFirst(false, 1);
  ValidateExpected(
      "js_skip_first",
      "<body><img src=\"1.jpg\"/><img src=\"2.jpg\"/></body>",
      StrCat("<body><img src=\"1.jpg\"/>", GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "2.jpg", ""),
             GetLazyloadPostscriptHtml(), "</body>"));
}

TEST_F(LazyloadImagesFilterTest, SkipFirstImageInNativeMode) {
  options()->set_lazyload_images_mode(
      RewriteOptions::kLazyloadImagesModeNative);
  InitLazyloadImagesFilterWithSkipFirst(false, 1);
  ValidateExpected(
      "native_skip_first",
      "<body><img src=\"1.jpg\"/><img src=\"2.jpg\"/></body>",
      "<body><img src=\"1.jpg\"/>"
      "<img src=\"2.jpg\" loading=\"lazy\" decoding=\"async\"/></body>");
}

TEST_F(LazyloadImagesFilterTest, SkipFirstCountsOnlyEligibleImages) {
  InitLazyloadImagesFilterWithSkipFirst(false, 1);
  // The data: image is not eligible, so it does not consume the skip slot.
  ValidateExpected(
      "js_skip_first_eligible",
      "<body>"
      "<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhE\"/>"
      "<img src=\"1.jpg\"/>"
      "<img src=\"2.jpg\"/>"
      "</body>",
      StrCat("<body>"
             "<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhE\"/>"
             "<img src=\"1.jpg\"/>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "2.jpg", ""),
             GetLazyloadPostscriptHtml(), "</body>"));
}

// In js mode an <img> inside <picture> must stay untouched: the sibling
// <source> elements are not rewritten and would either load eagerly or make
// the browser pick the blank pixel.
TEST_F(LazyloadImagesFilterTest, PictureImgSkippedInJsMode) {
  InitLazyloadImagesFilter(false);
  ValidateExpected(
      "picture_js",
      "<body>"
      "<picture>"
      "<source srcset=\"1.webp\"/>"
      "<img src=\"1.jpg\"/>"
      "</picture>"
      "<img src=\"2.jpg\"/>"
      "</body>",
      StrCat("<body>"
             "<picture>"
             "<source srcset=\"1.webp\"/>"
             "<img src=\"1.jpg\"/>"
             "</picture>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "2.jpg", ""),
             GetLazyloadPostscriptHtml(), "</body>"));
}

// The native loading="lazy" attribute is valid and effective on an <img>
// inside <picture>, so native mode does rewrite it.
TEST_F(LazyloadImagesFilterTest, PictureImgAppliedInNativeMode) {
  options()->set_lazyload_images_mode(
      RewriteOptions::kLazyloadImagesModeNative);
  InitLazyloadImagesFilter(false);
  ValidateExpected(
      "picture_native",
      "<body>"
      "<picture>"
      "<source srcset=\"1.webp\"/>"
      "<img src=\"1.jpg\"/>"
      "</picture>"
      "</body>",
      "<body>"
      "<picture>"
      "<source srcset=\"1.webp\"/>"
      "<img src=\"1.jpg\" loading=\"lazy\" decoding=\"async\"/>"
      "</picture>"
      "</body>");
}

// An author-supplied loading attribute is respected in all modes: the
// element is left completely untouched.
TEST_F(LazyloadImagesFilterTest, AuthorLoadingAttributeRespectedJsMode) {
  InitLazyloadImagesFilter(false);
  ValidateNoChanges("author_loading_js",
                    "<body>"
                    "<img loading=\"lazy\" src=\"1.jpg\"/>"
                    "<img loading=\"eager\" src=\"2.jpg\"/>"
                    "</body>");
}

TEST_F(LazyloadImagesFilterTest, AuthorLoadingAttributeRespectedNativeMode) {
  options()->set_lazyload_images_mode(
      RewriteOptions::kLazyloadImagesModeNative);
  InitLazyloadImagesFilter(false);
  ValidateNoChanges("author_loading_native",
                    "<body>"
                    "<img loading=\"lazy\" src=\"1.jpg\"/>"
                    "<img loading=\"eager\" src=\"2.jpg\"/>"
                    "</body>");
  EXPECT_EQ(0, StatValue(LazyloadImagesFilter::kLazyloadImagesNativeApplied));
}

}  // namespace net_instaweb
