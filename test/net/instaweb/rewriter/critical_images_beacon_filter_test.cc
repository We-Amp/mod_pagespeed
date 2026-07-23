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

#include "net/instaweb/rewriter/public/critical_images_beacon_filter.h"

#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/critical_keys.pb.h"
#include "net/instaweb/rewriter/public/beacon_critical_images_finder.h"
#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/lazyload_images_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/mock_property_page.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/kernel/base/escaping.h"
#include "pagespeed/kernel/base/hasher.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_hash.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

using testing::HasSubstr;
using testing::Not;

namespace {

const char kChefGifFile[] = "IronChef2.gif";
// Set image dimensions such that image will be inlined.
const char kChefGifDims[] = "width=48 height=64";

const char kRequestUrl[] = "http://www.example.com";

}  // namespace

namespace net_instaweb {

class CriticalImagesBeaconFilterTest : public RewriteTestBase {
 protected:
  CriticalImagesBeaconFilterTest() {}

  void SetUp() override {
    options()->set_beacon_url("http://example.com/beacon");
    CriticalImagesBeaconFilter::InitStats(statistics());
    // Enable a filter that uses critical images, which in turn will enable
    // beacon insertion.
    factory()->set_use_beacon_results_in_filters(true);
    options()->EnableFilter(RewriteOptions::kDelayImages);
    RewriteTestBase::SetUp();
    https_mode_ = false;
    // Setup the property cache. The DetermineEnable logic for the
    // CriticalImagesBeaconFinder will only inject the beacon if the property
    // cache is enabled, since beaconed results are intended to be stored in the
    // pcache.
    PropertyCache* pcache = page_property_cache();
    server_context_->set_enable_property_cache(true);
    const PropertyCache::Cohort* beacon_cohort =
        SetupCohort(pcache, RewriteDriver::kBeaconCohort);
    const PropertyCache::Cohort* dom_cohort =
        SetupCohort(pcache, RewriteDriver::kDomCohort);
    server_context()->set_beacon_cohort(beacon_cohort);
    server_context()->set_dom_cohort(dom_cohort);

    server_context()->set_critical_images_finder(new BeaconCriticalImagesFinder(
        beacon_cohort, factory()->nonce_generator(), statistics()));

    GoogleUrl base(GetTestUrl());
    image_gurl_.Reset(base, kChefGifFile);
    ResetDriver();
  }

  void ResetDriver() {
    rewrite_driver()->Clear();
    SetCurrentUserAgent(UserAgentMatcherTestBase::kChrome18UserAgent);
    rewrite_driver()->set_request_context(
        RequestContext::NewTestRequestContext(factory()->thread_system()));
    MockPropertyPage* page = NewMockPage(kRequestUrl);
    rewrite_driver()->set_property_page(page);
    PropertyCache* pcache = server_context_->page_property_cache();
    pcache->Read(page);
  }

  void WriteToPropertyCache() {
    rewrite_driver()->property_page()->WriteCohort(
        server_context()->beacon_cohort());
  }

  // Simulates the beacon response arriving for the nonce handed out by the
  // most recent injection, recording `critical_key` as the html critical
  // image set.
  void SimulateBeaconResponse(StringPiece critical_key) {
    // The injected beacon init call ends with the quoted nonce as its last
    // argument; the first pass emits no other "');" sequence.
    size_t end = output_buffer_.rfind("');");
    ASSERT_NE(GoogleString::npos, end);
    size_t start = output_buffer_.rfind('\'', end - 1);
    ASSERT_NE(GoogleString::npos, start);
    GoogleString nonce = output_buffer_.substr(start + 1, end - start - 1);
    StringSet html_critical_images;
    html_critical_images.insert(critical_key.as_string());
    EXPECT_TRUE(BeaconCriticalImagesFinder::UpdateCriticalImagesCacheEntry(
        &html_critical_images, nullptr, nullptr, nonce,
        server_context()->beacon_cohort(), rewrite_driver()->property_page(),
        factory()->mock_timer()));
    WriteToPropertyCache();
  }

  void ReinstrumentAndReprocess() {
    factory()->mock_timer()->AdvanceMs(
        options()->beacon_reinstrument_time_sec() * 1000);
    ResetDriver();
    SetupAndProcessUrl();
  }

  void PrepareInjection() {
    rewrite_driver()->AddFilters();
    AddFileToMockFetcher(image_gurl_.Spec(), kChefGifFile, kContentTypeJpeg,
                         100);
  }

  // Srcset candidates get input resources created for them, so they need
  // mock fetcher entries of their own.
  void PrepareSrcsetInjection() {
    PrepareInjection();
    AddFileToMockFetcher(StrCat(kTestDomain, "small.jpg"), kChefGifFile,
                         kContentTypeJpeg, 100);
    AddFileToMockFetcher(StrCat(kTestDomain, "large.jpg"), kChefGifFile,
                         kContentTypeJpeg, 100);
  }

  void AddImageTags(GoogleString* html) {
    // Add the relative image URL.
    StrAppend(html, "<img src=\"", kChefGifFile, "\" ", kChefGifDims, ">");
    // Add the absolute image URL.
    StrAppend(html, "<img src=\"", image_gurl_.Spec(), "\" ", kChefGifDims,
              ">");
  }

  void RunInjection() {
    PrepareInjection();
    SetupAndProcessUrl();
  }

  void SetupAndProcessUrl() {
    GoogleString html = "<head></head><body>";
    AddImageTags(&html);
    StrAppend(&html, "</body>");
    ParseUrl(GetTestUrl(), html);
  }

  void RunInjectionNoBody() {
    // As above, but we omit <head> and (more relevant) <body> tags.  We should
    // still inject the script at the end of the document.  The filter used to
    // get this wrong.
    PrepareInjection();
    GoogleString html;
    AddImageTags(&html);
    ParseUrl(GetTestUrl(), html);
  }

  void VerifyInjection(int expected_beacon_count) {
    EXPECT_EQ(
        expected_beacon_count,
        statistics()
            ->GetVariable(
                CriticalImagesBeaconFilter::kCriticalImagesBeaconAddedCount)
            ->Get());
    EXPECT_THAT(output_buffer_, HasSubstr(CreateInitString()));
  }

  void VerifyNoInjection(int expected_beacon_count) {
    EXPECT_EQ(
        expected_beacon_count,
        statistics()
            ->GetVariable(
                CriticalImagesBeaconFilter::kCriticalImagesBeaconAddedCount)
            ->Get());
    EXPECT_THAT(output_buffer_, Not(HasSubstr("pagespeed.CriticalImages.Run")));
  }

  void VerifyWithNoImageRewrite() {
    const GoogleString hash_str = ImageUrlHash(kChefGifFile);
    EXPECT_THAT(output_buffer_,
                HasSubstr(StrCat("data-pagespeed-url-hash=\"", hash_str)));
  }

  void AssumeHttps() { https_mode_ = true; }

  GoogleString GetTestUrl() {
    return StrCat((https_mode_ ? "https://example.com/" : kTestDomain),
                  "index.html?a&b");
  }

  GoogleString ImageUrlHash(StringPiece url) {
    // Absolutify the URL before hashing.
    unsigned int hash_val = HashString<CasePreserve, unsigned int>(
        image_gurl_.spec_c_str(), strlen(image_gurl_.spec_c_str()));
    return UintToString(hash_val);
  }

  // Hash of `url` resolved against the page, mirroring the production code.
  GoogleString UrlHash(StringPiece url) {
    GoogleUrl gurl(rewrite_driver()->base_url(), url);
    unsigned int hash_val = HashString<CasePreserve, unsigned int>(
        gurl.spec_c_str(), strlen(gurl.spec_c_str()));
    return UintToString(hash_val);
  }

  GoogleString CreateInitString() {
    GoogleString url;
    EscapeToJsStringLiteral(rewrite_driver()->google_url().Spec(), false, &url);
    // Mirror the production code, which JS-escapes the beacon URL before
    // splicing it into the single-quoted Run(...) argument.
    StringPiece raw_beacon_url = https_mode_ ? options()->beacon_url().https
                                             : options()->beacon_url().http;
    GoogleString beacon_url;
    EscapeToJsStringLiteral(raw_beacon_url, false, &beacon_url);
    GoogleString options_signature_hash =
        rewrite_driver()->server_context()->hasher()->Hash(
            rewrite_driver()->options()->signature());
    bool lazyload_will_run_beacon =
        rewrite_driver()->options()->Enabled(RewriteOptions::kLazyloadImages) &&
        LazyloadImagesFilter::ShouldApply(rewrite_driver()) ==
            RewriterHtmlApplication::ACTIVE &&
        !LazyloadImagesFilter::ShouldApplyNativeMode(rewrite_driver());
    GoogleString str = "pagespeed.CriticalImages.Run(";
    StrAppend(&str, "'", beacon_url, "',");
    StrAppend(&str, "'", url, "',");
    StrAppend(&str, "'", options_signature_hash, "',");
    StrAppend(&str, BoolToString(!lazyload_will_run_beacon), ",");
    StrAppend(&str,
              BoolToString(rewrite_driver()->options()->Enabled(
                  RewriteOptions::kResizeToRenderedImageDimensions)),
              ",");
    StrAppend(&str, "'", ExpectedNonce(), "');");
    return str;
  }

  bool https_mode_;
  GoogleUrl image_gurl_;
};

TEST_F(CriticalImagesBeaconFilterTest, CspForbidsInlineScript) {
  // Under a script-src policy without 'unsafe-inline' the browser would
  // block the beacon script and the onload handlers, so the page must not
  // be instrumented at all.
  PrepareInjection();
  GoogleString html =
      "<head><meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src *;\"></head><body>";
  AddImageTags(&html);
  StrAppend(&html, "</body>");
  ParseUrl(GetTestUrl(), html);
  VerifyNoInjection(0);
  EXPECT_THAT(output_buffer_, Not(HasSubstr("data-pagespeed-url-hash")));
  EXPECT_THAT(output_buffer_,
              Not(HasSubstr(CriticalImagesBeaconFilter::kImageOnloadCode)));
}

TEST_F(CriticalImagesBeaconFilterTest, CspAllowsInlineScript) {
  // With 'unsafe-inline' permitted the filter behaves as usual.
  PrepareInjection();
  GoogleString html =
      "<head><meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src * 'unsafe-inline';\"></head><body>";
  AddImageTags(&html);
  StrAppend(&html, "</body>");
  ParseUrl(GetTestUrl(), html);
  VerifyInjection(1);
  VerifyWithNoImageRewrite();
}

TEST_F(CriticalImagesBeaconFilterTest, ScriptInjection) {
  RunInjection();
  VerifyInjection(1);

  // Verify that image onload criticality check has been added.
  int img_begin = output_buffer_.find("IronChef2");
  EXPECT_TRUE(img_begin != GoogleString::npos);
  int img_end = output_buffer_.substr(img_begin).find(">");
  EXPECT_TRUE(img_end != GoogleString::npos);
  EXPECT_TRUE(output_buffer_.substr(img_begin, img_end)
                  .find("onload=\"pagespeed.CriticalImages."
                        "checkImageForCriticality(this);\"") !=
              GoogleString::npos);
  VerifyWithNoImageRewrite();
}

TEST_F(CriticalImagesBeaconFilterTest, ScriptInjectionNoBody) {
  RunInjectionNoBody();
  VerifyInjection(1);
  VerifyWithNoImageRewrite();
}

TEST_F(CriticalImagesBeaconFilterTest, ScriptInjectionWithHttps) {
  AssumeHttps();
  RunInjection();
  VerifyInjection(1);
  VerifyWithNoImageRewrite();
}

TEST_F(CriticalImagesBeaconFilterTest, ScriptInjectionWithImageInlining) {
  // Verify that the URL hash is applied to the absolute image URL, and not to
  // the rewritten URL. In this case, make sure that an image inlined to a data
  // URI has the correct hash. We need to add the image hash to the critical
  // image set to make sure that the image is inlined.
  GoogleString hash_str = ImageUrlHash(kChefGifFile);
  StringSet* crit_img_set =
      server_context()->critical_images_finder()->mutable_html_critical_images(
          rewrite_driver());
  crit_img_set->insert(hash_str);
  options()->set_image_inline_max_bytes(10000);
  options()->EnableFilter(RewriteOptions::kResizeImages);
  options()->EnableFilter(RewriteOptions::kResizeToRenderedImageDimensions);
  options()->EnableFilter(RewriteOptions::kInlineImages);
  options()->EnableFilter(RewriteOptions::kInsertImageDimensions);
  options()->EnableFilter(RewriteOptions::kConvertGifToPng);
  options()->DisableFilter(RewriteOptions::kDelayImages);
  RunInjection();
  VerifyInjection(1);

  EXPECT_TRUE(output_buffer_.find("data:") != GoogleString::npos);
  EXPECT_TRUE(output_buffer_.find(hash_str) != GoogleString::npos);
  EXPECT_EQ(-1, logging_info()->num_html_critical_images());
  EXPECT_EQ(-1, logging_info()->num_css_critical_images());
}

// A srcset-only image (no src attribute) carries no data-pagespeed-url-hash.
// Instead the filter stamps the hashes of all srcset candidates positionally,
// so the beacon JS can report the candidate the browser actually selected
// (matched against currentSrc); the candidate hashes are registered as beacon
// candidates alongside the src hashes.
TEST_F(CriticalImagesBeaconFilterTest, ScriptInjectionSrcsetOnly) {
  PrepareSrcsetInjection();
  ParseUrl(GetTestUrl(),
           "<head></head><body>"
           "<img srcset=\"small.jpg 480w, large.jpg 1024w\"/>"
           "</body>");
  VerifyInjection(1);
  // Positional candidate hashes, resolved against the page URL.
  EXPECT_THAT(
      output_buffer_,
      HasSubstr(StrCat("data-pagespeed-srcset-url-hashes=\"",
                       UrlHash("small.jpg"), ",", UrlHash("large.jpg"), "\"")));
  // No src attribute means no src hash is stamped (the attribute-usage form
  // excludes the literal appearing inside the injected beacon JS).
  EXPECT_THAT(output_buffer_, Not(HasSubstr("data-pagespeed-url-hash=\"")));
  // The image-onload criticality hook is added, as for src images.
  EXPECT_THAT(
      output_buffer_,
      HasSubstr(StrCat("onload=\"",
                       CriticalImagesBeaconFilter::kImageOnloadCode, "\"")));
}

// Mixed <img src srcset> markup keeps src-keyed stamping: the beacon reports
// the src hash, and fetchpriority is element-level so it raises whichever
// candidate the browser selects. No candidate hash list is emitted.
TEST_F(CriticalImagesBeaconFilterTest, ScriptInjectionMixedSrcSrcset) {
  PrepareSrcsetInjection();
  ParseUrl(GetTestUrl(),
           StrCat("<head></head><body>"
                  "<img src=\"",
                  kChefGifFile, "\" srcset=\"large.jpg 1024w\" ", kChefGifDims,
                  ">"
                  "</body>"));
  VerifyInjection(1);
  VerifyWithNoImageRewrite();
  // Attribute-usage form: the literal also appears inside the beacon JS.
  EXPECT_THAT(output_buffer_,
              Not(HasSubstr("data-pagespeed-srcset-url-hashes=\"")));
}

// The srcset candidate hashes are registered as beacon candidate keys
// alongside src hashes, so candidate-aware beacon responses follow the same
// re-beacon scheduling as src-keyed ones.
TEST_F(CriticalImagesBeaconFilterTest, SrcsetCandidatesRegisteredForBeaconing) {
  PrepareSrcsetInjection();
  ParseUrl(GetTestUrl(),
           "<head></head><body>"
           "<img srcset=\"small.jpg 480w, large.jpg 1024w\"/>"
           "</body>");
  VerifyInjection(1);
  // Capture the expected hashes before ResetDriver() clears the base URL.
  const GoogleString small_hash = UrlHash("small.jpg");
  const GoogleString large_hash = UrlHash("large.jpg");
  WriteToPropertyCache();

  ResetDriver();
  CriticalImagesFinder* finder = server_context()->critical_images_finder();
  ASSERT_TRUE(finder->IsCriticalImageInfoPresent(rewrite_driver()));
  const CriticalKeys& keys = rewrite_driver()
                                 ->critical_images_info()
                                 ->proto.html_critical_image_support();
  StringSet candidate_keys;
  for (int i = 0; i < keys.key_evidence_size(); ++i) {
    candidate_keys.insert(keys.key_evidence(i).key());
  }
  EXPECT_NE(0, candidate_keys.count(small_hash));
  EXPECT_NE(0, candidate_keys.count(large_hash));
}

TEST_F(CriticalImagesBeaconFilterTest, NoScriptInjectionWithNoScript) {
  PrepareInjection();
  GoogleString html = "<head></head><body><noscript>";
  AddImageTags(&html);
  StrAppend(&html, "</noscript></body>");
  ParseUrl(GetTestUrl(), html);
  VerifyNoInjection(0);
  VerifyWithNoImageRewrite();
}

TEST_F(CriticalImagesBeaconFilterTest, DontRebeaconBeforeTimeout) {
  RunInjection();
  VerifyInjection(1);
  VerifyWithNoImageRewrite();

  // Write a dummy value to the property cache.
  WriteToPropertyCache();

  // No beacon injection happens on the immediately succeeding request.
  ResetDriver();
  SetDriverRequestHeaders();
  SetupAndProcessUrl();
  VerifyNoInjection(1);

  // Beacon injection happens when the pcache value expires or when the
  // reinstrumentation time interval is exceeded.
  factory()->mock_timer()->AdvanceMs(options()->beacon_reinstrument_time_sec() *
                                     1000);
  ResetDriver();
  SetDriverRequestHeaders();
  SetupAndProcessUrl();
  VerifyInjection(2);
}

TEST_F(CriticalImagesBeaconFilterTest, BeaconReinstrumentationWithHeader) {
  RunInjection();
  VerifyInjection(1);
  VerifyWithNoImageRewrite();

  // Write a dummy value to the property cache.
  WriteToPropertyCache();

  // Beacon injection happens when the PS-ShouldBeacon header is present even
  // when the pcache value has not expired and the reinstrumentation time
  // interval has not been exceeded.
  ResetDriver();
  SetDownstreamCacheDirectives("", "localhost:80", "random_rebeaconing_key");
  AddRequestAttribute(kPsaShouldBeacon, "random_rebeaconing_key");
  SetDriverRequestHeaders();
  SetupAndProcessUrl();
  VerifyInjection(2);
}

TEST_F(CriticalImagesBeaconFilterTest, UnsupportedUserAgent) {
  // Test that the filter is not applied for unsupported user agents.
  SetCurrentUserAgent("Firefox/1.0");
  RunInjection();
  VerifyNoInjection(0);
}

TEST_F(CriticalImagesBeaconFilterTest, Googlebot) {
  // Verify that the filter is not applied for bots.
  SetCurrentUserAgent(UserAgentMatcherTestBase::kGooglebotUserAgent);
  RunInjection();
  VerifyNoInjection(0);
}

// Verify that the init string is set correctly to not run the beacon's onload
// handler when lazyload is enabled. The lazyload JS will take care of running
// the beacon when all images have been loaded.
TEST_F(CriticalImagesBeaconFilterTest, LazyloadEnabled) {
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  // On the first page access, there will be no critical image data and lazyload
  // will be disabled.
  RunInjection();
  VerifyInjection(1);

  // Advance time to force re-beaconing.  Now there are extant non-critical
  // images, and lazyload ought to be enabled.
  factory()->mock_timer()->AdvanceMs(options()->beacon_reinstrument_time_sec() *
                                     1000);
  ResetDriver();
  SetupAndProcessUrl();
  VerifyInjection(2);
}

// Native-mode lazyload injects no JavaScript, so the beacon must run its
// onload handler itself. The send-at-onload flag is the argument after the
// quoted options hash, hence the "',true," pattern.
TEST_F(CriticalImagesBeaconFilterTest, LazyloadNativeModeBeaconsAtOnload) {
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  options()->set_lazyload_images_mode(
      RewriteOptions::kLazyloadImagesModeNative);
  RunInjection();
  VerifyInjection(1);
  EXPECT_THAT(output_buffer_, HasSubstr("',true,"));
}

// In JS mode the lazyload loader fires the beacon once it has loaded all
// images, so the beacon must not also fire at onload.
TEST_F(CriticalImagesBeaconFilterTest, LazyloadJsModeDelegatesBeacon) {
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  options()->set_lazyload_images_mode(RewriteOptions::kLazyloadImagesModeJs);
  // First access: no critical image data yet, so js lazyload is disabled and
  // the beacon fires at onload.
  RunInjection();
  VerifyInjection(1);
  EXPECT_THAT(output_buffer_, HasSubstr("',true,"));

  // Simulate the beacon response arriving with a hash that matches no image
  // on the page, so the page's images stay non-critical.
  SimulateBeaconResponse("1234");

  // Re-instrument with data present: lazyload is active and owns the beacon.
  ReinstrumentAndReprocess();
  VerifyInjection(2);
  EXPECT_THAT(output_buffer_, HasSubstr("',false,"));
  // The lazyload machinery actually rewrote the images (attribute form, to
  // not match the attribute name appearing as a literal in the loader js).
  EXPECT_THAT(output_buffer_, HasSubstr("data-pagespeed-lazy-src=\""));
}

// Even when every image on the page is critical (nothing gets lazyloaded),
// an active js-mode lazyload must still emit its loader so the delegated
// beacon fires.
TEST_F(CriticalImagesBeaconFilterTest, LazyloadJsModeAllCriticalStillBeacons) {
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  options()->set_lazyload_images_mode(RewriteOptions::kLazyloadImagesModeJs);
  RunInjection();
  VerifyInjection(1);

  // The beacon response marks the page's only image critical, keyed by the
  // same url hash BeaconCriticalImagesFinder::GetKeyForUrl computes.
  SimulateBeaconResponse(UintToString(HashString<CasePreserve, unsigned int>(
      image_gurl_.Spec().data(), image_gurl_.Spec().size())));

  ReinstrumentAndReprocess();
  VerifyInjection(2);
  // Delegated to lazyload...
  EXPECT_THAT(output_buffer_, HasSubstr("',false,"));
  // ...whose loader must therefore be present despite zero rewritten images
  // (the attribute-form check skips the literals inside the loader js).
  EXPECT_THAT(output_buffer_, HasSubstr("pagespeed.lazyLoadInit"));
  EXPECT_THAT(output_buffer_, Not(HasSubstr("data-pagespeed-lazy-src=\"")));
}

// Regression: the beacon URL is JS-escaped (matching the page URL) before it
// is spliced into the single-quoted argument of pagespeed.CriticalImages.Run,
// so a single quote in the (admin-configured) beacon URL is emitted as \' and
// cannot terminate the JS string literal early.
TEST_F(CriticalImagesBeaconFilterTest, BeaconUrlJsEscaped) {
  options()->set_beacon_url("http://example.com/beacon'x");
  RunInjection();
  VerifyInjection(1);
  // The single quote is backslash-escaped in the emitted Run(...) call.
  EXPECT_THAT(output_buffer_, HasSubstr("'http://example.com/beacon\\'x','"));
  // The raw, unescaped single-quote form must not appear.
  EXPECT_THAT(output_buffer_, Not(HasSubstr("beacon'x")));
}

}  // namespace net_instaweb
