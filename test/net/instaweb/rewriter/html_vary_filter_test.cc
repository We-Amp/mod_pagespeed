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

// Unit tests for HtmlVaryFilter: the pure token rule (a table of options
// against the expected Vary tokens) and the parse-level behaviour (the
// header is a function of the configuration alone).

#include "net/instaweb/rewriter/public/html_vary_filter.h"

#include <algorithm>
#include <memory>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/net/instaweb/rewriter/mock_critical_images_finder.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

namespace net_instaweb {

namespace {

const char kHtml[] = "<html><head></head><body>hello</body></html>";

// Comma-joins the tokens HtmlVaryTokens computes.
GoogleString Tokens(const RewriteOptions& options) {
  StringVector tokens;
  HtmlVaryFilter::HtmlVaryTokens(options, &tokens);
  GoogleString joined;
  for (const GoogleString& token : tokens) {
    if (!joined.empty()) {
      StrAppend(&joined, ",");
    }
    StrAppend(&joined, token);
  }
  return joined;
}

// rewrite_test_base.h declares its own RewriteOptionsTestBase, so this fixture
// does the RewriteOptions Initialize/Terminate bracketing itself.
class HtmlVaryTokensTest : public testing::Test {
 protected:
  HtmlVaryTokensTest()
      : thread_system_(Platform::CreateThreadSystem()),
        options_(thread_system_.get()) {}

  ThreadSystem* thread_system() { return thread_system_.get(); }

  // Starts from a configuration with no filter enabled, so each case names
  // exactly the filters and qualities it wants.
  void SetUp() override {
    options_.SetRewriteLevel(RewriteOptions::kPassThrough);
    // Equal small-screen and Save-Data qualities (inherit the base) unless a
    // case sets them.
    options_.set_image_jpeg_recompress_quality_for_small_screens(-1);
    options_.set_image_webp_recompress_quality_for_small_screens(-1);
    options_.set_image_jpeg_quality_for_save_data(-1);
    options_.set_image_webp_quality_for_save_data(-1);
  }

  struct OptionsScope {
    OptionsScope() { RewriteOptions::Initialize(); }
    ~OptionsScope() { RewriteOptions::Terminate(); }
  };

  OptionsScope options_scope_;
  std::unique_ptr<ThreadSystem> thread_system_;
  RewriteOptions options_;
};

TEST_F(HtmlVaryTokensTest, NoFiltersNoTokens) {
  EXPECT_EQ("", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, CoreFiltersNameAllThree) {
  RewriteOptions core(thread_system());
  core.SetRewriteLevel(RewriteOptions::kCoreFilters);
  EXPECT_EQ("Accept,Save-Data,User-Agent", Tokens(core));
}

TEST_F(HtmlVaryTokensTest, ImagesOffNoTokens) {
  RewriteOptions core(thread_system());
  core.SetRewriteLevel(RewriteOptions::kCoreFilters);
  core.DisableFilter(RewriteOptions::kRecompressJpeg);
  core.DisableFilter(RewriteOptions::kRecompressPng);
  core.DisableFilter(RewriteOptions::kRecompressWebp);
  core.DisableFilter(RewriteOptions::kConvertGifToPng);
  core.DisableFilter(RewriteOptions::kConvertJpegToProgressive);
  core.DisableFilter(RewriteOptions::kConvertPngToJpeg);
  core.DisableFilter(RewriteOptions::kConvertJpegToWebp);
  core.DisableFilter(RewriteOptions::kConvertToWebpLossless);
  core.DisableFilter(RewriteOptions::kResizeImages);
  core.DisableFilter(RewriteOptions::kInlineImages);
  core.DisableFilter(RewriteOptions::kJpegSubsampling);
  core.DisableFilter(RewriteOptions::kStripImageMetaData);
  core.DisableFilter(RewriteOptions::kStripImageColorProfile);
  core.DisableFilter(RewriteOptions::kInsertImageDimensions);
  core.DisableFilter(RewriteOptions::kLazyloadImages);
  EXPECT_FALSE(core.ImageRewriteFilterInHtmlChain());
  EXPECT_EQ("", Tokens(core));
}

TEST_F(HtmlVaryTokensTest, WebpWithEqualQualitiesIsAcceptAndUserAgent) {
  // No Save-Data qualities, no small-screen qualities, no inlining: the WebP
  // fallback and legacy grants still read User-Agent.
  options_.EnableFilter(RewriteOptions::kConvertJpegToWebp);
  EXPECT_EQ("Accept,User-Agent", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, SaveDataOnlyWhenNoFormatAndNoUserAgentInput) {
  options_.EnableFilter(RewriteOptions::kRecompressJpeg);
  options_.set_image_jpeg_quality_for_save_data(40);
  EXPECT_EQ("Save-Data", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, SaveDataQualitiesEqualToBaseDropSaveData) {
  options_.EnableFilter(RewriteOptions::kRecompressJpeg);
  options_.set_image_jpeg_recompress_quality(80);
  options_.set_image_jpeg_quality_for_save_data(80);
  EXPECT_EQ("", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, AvifOnlyIsAcceptOnly) {
  options_.EnableFilter(RewriteOptions::kConvertJpegToAvif);
  EXPECT_EQ("Accept", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, AvifWithSmallScreenQualitiesAddsUserAgent) {
  options_.EnableFilter(RewriteOptions::kRecompressAvif);
  options_.set_image_jpeg_recompress_quality_for_small_screens(60);
  EXPECT_EQ("Accept,User-Agent", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, InlineImagesAddsUserAgent) {
  options_.EnableFilter(RewriteOptions::kInlineImages);
  EXPECT_EQ("User-Agent", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, DelayImagesAloneIsUserAgent) {
  // The low-res preview insertion reads the User-Agent; resize_mobile_images
  // is not needed.
  options_.EnableFilter(RewriteOptions::kDelayImages);
  EXPECT_EQ("User-Agent", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, DedupInlinedImagesAloneIsUserAgent) {
  options_.EnableFilter(RewriteOptions::kDedupInlinedImages);
  EXPECT_EQ("User-Agent", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, LocalStorageCacheAloneIsUserAgent) {
  options_.EnableFilter(RewriteOptions::kLocalStorageCache);
  EXPECT_EQ("User-Agent", Tokens(options_));
  options_.set_image_preserve_urls(true);
  EXPECT_EQ("User-Agent", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, PreservedImageUrlsDropImageTokens) {
  options_.EnableFilter(RewriteOptions::kConvertJpegToWebp);
  options_.set_image_jpeg_quality_for_save_data(40);
  EXPECT_EQ("Accept,Save-Data,User-Agent", Tokens(options_));
  // Preserved URLs only leave the image rewriter out of the HTML chain when
  // in-place preemptive image rewriting is off.
  options_.set_image_preserve_urls(true);
  options_.set_in_place_preemptive_rewrite_images(false);
  EXPECT_EQ("", Tokens(options_));
  // Preemptive in-place rewriting puts the image rewriter back in the chain.
  options_.set_in_place_preemptive_rewrite_images(true);
  EXPECT_EQ("Accept,Save-Data,User-Agent", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, ExplicitInlineOrDelayKeepUserAgentWhenPreserved) {
  // The critical-image beacon still instruments per browser.
  options_.set_image_preserve_urls(true);
  options_.EnableFilter(RewriteOptions::kInlineImages);
  EXPECT_EQ("User-Agent", Tokens(options_));
  RewriteOptions delay(thread_system());
  delay.set_image_preserve_urls(true);
  delay.set_in_place_preemptive_rewrite_images(false);
  delay.set_image_jpeg_quality_for_save_data(-1);
  delay.set_image_webp_quality_for_save_data(-1);
  delay.EnableFilter(RewriteOptions::kDelayImages);
  EXPECT_EQ("User-Agent", Tokens(delay));
}

TEST_F(HtmlVaryTokensTest, LazyloadAloneIsUserAgent) {
  options_.EnableFilter(RewriteOptions::kLazyloadImages);
  EXPECT_EQ("User-Agent", Tokens(options_));
  // Independent of the image chain.
  options_.set_image_preserve_urls(true);
  EXPECT_EQ("User-Agent", Tokens(options_));
}

TEST_F(HtmlVaryTokensTest, NeverNamesAcceptEncodingOrClientHints) {
  RewriteOptions core(thread_system());
  core.SetRewriteLevel(RewriteOptions::kCoreFilters);
  core.EnableFilter(RewriteOptions::kConvertJpegToAvif);
  core.EnableFilter(RewriteOptions::kLazyloadImages);
  StringVector tokens;
  HtmlVaryFilter::HtmlVaryTokens(core, &tokens);
  for (const GoogleString& token : tokens) {
    EXPECT_NE("Accept-Encoding", token);
    EXPECT_NE("Sec-CH-DPR", token);
  }
}

TEST_F(HtmlVaryTokensTest, ClearsTheOutputVector) {
  StringVector tokens;
  tokens.push_back("stale");
  HtmlVaryFilter::HtmlVaryTokens(options_, &tokens);
  EXPECT_TRUE(tokens.empty());
}

// Parse-level behaviour.
class HtmlVaryFilterTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    SetHtmlMimetype();
  }

  // The default CoreFilters configuration, which carries WebP conversion,
  // inlining and distinct Save-Data / small-screen qualities.
  void EnableCore() {
    options()->ClearSignatureForTesting();
    options()->SetRewriteLevel(RewriteOptions::kCoreFilters);
    rewrite_driver()->AddFilters();
  }

  // Parses kHtml as the given client and returns the comma-joined Vary values
  // the response ended up with. Resets the driver first, so each call is a
  // fresh request on the same (cold, then warm) caches.
  GoogleString VaryFor(const char* user_agent, const char* accept,
                       bool save_data, const char* capability_list) {
    ClearRewriteDriver();
    response_headers_.Clear();
    SetHtmlMimetype();
    SetCurrentUserAgent(user_agent);
    if (accept != nullptr) {
      AddRequestAttribute(HttpAttributes::kAccept, accept);
    }
    if (save_data) {
      AddRequestAttribute(HttpAttributes::kSaveData, "on");
    }
    if (capability_list != nullptr) {
      AddRequestAttribute("PS-CapabilityList", capability_list);
    }
    ParseUrl(StrCat(kTestDomain, "index.html"), kHtml);
    return VaryValues();
  }

  std::unique_ptr<HtmlVaryFilter> filter_;

  GoogleString VaryValues() {
    ConstStringStarVector values;
    response_headers_.Lookup(HttpAttributes::kVary, &values);
    GoogleString joined;
    for (const GoogleString* value : values) {
      if (!joined.empty()) {
        StrAppend(&joined, ",");
      }
      StrAppend(&joined, *value);
    }
    return joined;
  }

  GoogleString Added() {
    GoogleString joined;
    for (const GoogleString& token :
         rewrite_driver()->html_vary_tokens_added()) {
      if (!joined.empty()) {
        StrAppend(&joined, ",");
      }
      StrAppend(&joined, token);
    }
    return joined;
  }
};

TEST_F(HtmlVaryFilterTest, SameVaryForEveryRequest) {
  EnableCore();
  const char kExpected[] = "Accept,Save-Data,User-Agent";
  // Chrome that accepts WebP.
  EXPECT_EQ(kExpected, VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent,
                               "text/html,image/webp,*/*", false, nullptr));
  // Safari 17 without WebP in the navigation Accept.
  EXPECT_EQ(kExpected,
            VaryFor("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 "
                    "Safari/605.1.15",
                    "text/html,*/*", false, nullptr));
  // An Android phone.
  EXPECT_EQ(kExpected, VaryFor("Mozilla/5.0 (Linux; Android 13; Pixel 7) "
                               "AppleWebKit/537.36 (KHTML, like Gecko) "
                               "Chrome/120.0.0.0 Mobile Safari/537.36",
                               "image/webp,*/*", false, nullptr));
  // Googlebot.
  EXPECT_EQ(kExpected, VaryFor("Mozilla/5.0 (compatible; Googlebot/2.1; "
                               "+http://www.google.com/bot.html)",
                               nullptr, false, nullptr));
  // Save-Data on and off, and a capability list that removes capabilities.
  EXPECT_EQ(kExpected, VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent,
                               "image/webp", true, nullptr));
  EXPECT_EQ(kExpected, VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent,
                               "image/webp", false, nullptr));
  EXPECT_EQ(kExpected, VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent,
                               "image/webp", false, "none"));
}

TEST_F(HtmlVaryFilterTest, ColdAndWarmCacheAgree) {
  EnableCore();
  const GoogleString cold =
      VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent, "image/webp", false,
              nullptr);
  const GoogleString warm =
      VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent, "image/webp", false,
              nullptr);
  EXPECT_EQ("Accept,Save-Data,User-Agent", cold);
  EXPECT_EQ(cold, warm);
}

TEST_F(HtmlVaryFilterTest, EachTokenAppearsOnceAndAddedSetIsRecorded) {
  EnableCore();
  VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent, "image/webp", true,
          nullptr);
  ConstStringStarVector values;
  ASSERT_TRUE(response_headers_.Lookup(HttpAttributes::kVary, &values));
  EXPECT_EQ(3, values.size());
  EXPECT_EQ("Accept,Save-Data,User-Agent", Added());
}

TEST_F(HtmlVaryFilterTest, OriginVaryIsMergedWithoutDuplicates) {
  EnableCore();
  ClearRewriteDriver();
  response_headers_.Clear();
  SetHtmlMimetype();
  response_headers_.Add(HttpAttributes::kVary, "accept-encoding, user-agent");
  ParseUrl(StrCat(kTestDomain, "index.html"), kHtml);
  EXPECT_EQ("accept-encoding,user-agent,Accept,Save-Data", VaryValues());
  // The origin's User-Agent is not ours to subtract later.
  EXPECT_EQ("Accept,Save-Data", Added());
}

TEST_F(HtmlVaryFilterTest, PrioritizeCriticalImagesSaveDataIsRecorded) {
  // PrioritizeCriticalImagesFilter adds Save-Data first (DetermineEnabled);
  // HtmlVaryFilter finds it present. The added set still lists it once.
  options()->ClearSignatureForTesting();
  options()->SetRewriteLevel(RewriteOptions::kCoreFilters);
  options()->EnableFilter(RewriteOptions::kPrioritizeCriticalImages);
  // The mock finder reports Available(), so the prioritize filter itself
  // writes Save-Data (the test factory has beacon results off by default).
  server_context()->set_critical_images_finder(
      new MockCriticalImagesFinder(statistics()));
  rewrite_driver()->AddFilters();
  VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent, "image/webp", false,
          nullptr);
  ConstStringStarVector values;
  ASSERT_TRUE(response_headers_.Lookup(HttpAttributes::kVary, &values));
  EXPECT_EQ(3, values.size());
  // Save-Data was written by the prioritize filter and recorded once; the
  // other two by HtmlVaryFilter.
  StringVector added = rewrite_driver()->html_vary_tokens_added();
  std::sort(added.begin(), added.end());
  ASSERT_EQ(3, added.size());
  EXPECT_EQ("Accept", added[0]);
  EXPECT_EQ("Save-Data", added[1]);
  EXPECT_EQ("User-Agent", added[2]);
}

TEST_F(HtmlVaryFilterTest, VaryStarIsLeftAlone) {
  EnableCore();
  ClearRewriteDriver();
  response_headers_.Clear();
  SetHtmlMimetype();
  response_headers_.Add(HttpAttributes::kVary, "*");
  ParseUrl(StrCat(kTestDomain, "index.html"), kHtml);
  EXPECT_EQ("*", VaryValues());
  EXPECT_EQ("", Added());
}

TEST_F(HtmlVaryFilterTest, ModifyCachingHeadersOffStillEmits) {
  options()->ClearSignatureForTesting();
  options()->set_modify_caching_headers(false);
  EnableCore();
  EXPECT_EQ("Accept,Save-Data,User-Agent",
            VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent, "image/webp",
                    false, nullptr));
}

TEST_F(HtmlVaryFilterTest, ImagesOffEmitsNothing) {
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  EXPECT_EQ("", VaryFor(UserAgentMatcherTestBase::kChrome18UserAgent,
                        "image/webp", false, nullptr));
  EXPECT_EQ("", Added());
}

TEST_F(HtmlVaryFilterTest, UnwiredResponseHeadersDoNotCrash) {
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kConvertJpegToWebp);
  server_context()->ComputeSignature(options());
  filter_ = std::make_unique<HtmlVaryFilter>(rewrite_driver());
  rewrite_driver()->AddFilter(filter_.get());
  ClearRewriteDriver();  // detaches the response headers
  ParseUrl(StrCat(kTestDomain, "index.html"), kHtml);
  EXPECT_EQ("", Added());
}

TEST_F(HtmlVaryFilterTest, ResourceFetchEmitsNothing) {
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kExtendCacheScripts);
  options()->EnableFilter(RewriteOptions::kConvertJpegToWebp);
  options()->EnableFilter(RewriteOptions::kRecompressWebp);
  rewrite_driver()->AddFilters();
  SetResponseWithDefaultHeaders("c.js", kContentTypeJavascript, "var x = 1;",
                                100);
  GoogleString content;
  ResponseHeaders headers;
  ASSERT_TRUE(
      FetchResource(kTestDomain, "ce", "c.js", "js", &content, &headers));
  EXPECT_FALSE(headers.Has(HttpAttributes::kVary));
}

}  // namespace

}  // namespace net_instaweb
