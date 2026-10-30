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

#include "net/instaweb/rewriter/public/prioritize_critical_images_filter.h"

#include <memory>

#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/rewriter/public/lazyload_images_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "pagespeed/opt/logging/log_record.h"
#include "test/net/instaweb/rewriter/mock_critical_images_finder.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

namespace net_instaweb {

namespace {

// A finder that reports that beacon data is expected but has not arrived
// yet. MockCriticalImagesFinder hardwires Available() to kAvailable, so the
// no-data path needs its own override.
class NoDataYetCriticalImagesFinder : public MockCriticalImagesFinder {
 public:
  explicit NoDataYetCriticalImagesFinder(Statistics* stats)
      : MockCriticalImagesFinder(stats) {}
  Availability Available(RewriteDriver* driver) override { return kNoDataYet; }

 private:
  NoDataYetCriticalImagesFinder(const NoDataYetCriticalImagesFinder&) = delete;
  NoDataYetCriticalImagesFinder& operator=(
      const NoDataYetCriticalImagesFinder&) = delete;
};

// A finder reporting that critical-image data will never arrive, the state
// configuration produces when critical-image reporting is turned off or the
// factory does not use beacon results.
class DisabledCriticalImagesFinder : public MockCriticalImagesFinder {
 public:
  explicit DisabledCriticalImagesFinder(Statistics* stats)
      : MockCriticalImagesFinder(stats) {}
  Availability Available(RewriteDriver* driver) override { return kDisabled; }

 private:
  DisabledCriticalImagesFinder(const DisabledCriticalImagesFinder&) = delete;
  DisabledCriticalImagesFinder& operator=(const DisabledCriticalImagesFinder&) =
      delete;
};

}  // namespace

class PrioritizeCriticalImagesFilterTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    SetCurrentUserAgent(UserAgentMatcherTestBase::kChrome18UserAgent);
    SetHtmlMimetype();  // Prevent insertion of CDATA tags to static JS.
  }

  // The default harness wrapper prepends its own <html> tag, which would make
  // AmpDocumentFilter latch not-amp before ever seeing this test's
  // <html amp> tag (same reason CssInlineFilterTest disables it).
  bool AddHtmlTags() const override { return false; }

  void InitPrioritizeCriticalImagesFilter() {
    prioritize_critical_images_filter_ =
        std::make_unique<PrioritizeCriticalImagesFilter>(rewrite_driver());
    rewrite_driver()->AddFilter(prioritize_critical_images_filter_.get());
  }

  // Installs a MockCriticalImagesFinder reporting critical_images (full
  // absolute URLs; ownership transferred, may be NULL for "beacon data
  // available but empty").
  void SetCriticalImages(StringSet* critical_images) {
    MockCriticalImagesFinder* finder =
        new MockCriticalImagesFinder(statistics());
    finder->set_critical_images(critical_images);
    server_context()->set_critical_images_finder(finder);
  }

  // Verifies the html-level application status logged for this filter.
  void ExpectHtmlStatus(RewriterHtmlApplication::Status expected_status) {
    rewrite_driver_->log_record()->WriteLog();
    for (int i = 0; i < logging_info()->rewriter_stats_size(); i++) {
      if (logging_info()->rewriter_stats(i).id() == "pci" &&
          logging_info()->rewriter_stats(i).has_html_status()) {
        EXPECT_EQ(expected_status,
                  logging_info()->rewriter_stats(i).html_status());
        return;
      }
    }
    FAIL() << "No html status logged for filter id pci";
  }

  int64 StatValue(const char* name) {
    return statistics()->GetVariable(name)->Get();
  }

  // All Vary values of the last parse, comma-joined in order, so tests can
  // compare complete Vary sets across parses.
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

  // Resets the harness (fresh driver, cleared response headers), optionally
  // sends a Save-Data request header, parses html with the expected output,
  // and stores the Vary the parse produced.
  void ParseAndCollectVary(StringPiece case_id, StringPiece html,
                           bool with_save_data, StringPiece expected,
                           GoogleString* vary) {
    ClearRewriteDriver();
    response_headers_.Clear();
    SetHtmlMimetype();
    if (with_save_data) {
      AddRequestAttribute(HttpAttributes::kSaveData, "on");
    }
    ValidateExpected(case_id, html, expected);
    *vary = VaryValues();
  }

  std::unique_ptr<PrioritizeCriticalImagesFilter>
      prioritize_critical_images_filter_;
  std::unique_ptr<LazyloadImagesFilter> lazyload_images_filter_;
};

TEST_F(PrioritizeCriticalImagesFilterTest, CriticalImageGetsFetchPriority) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);

  ValidateExpected("critical_annotated",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\"/>"
                   "<img src=\"other.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"other.jpg\"/>"
                   "</body>");
  EXPECT_EQ(
      1, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
  ExpectHtmlStatus(RewriterHtmlApplication::ACTIVE);
}

TEST_F(PrioritizeCriticalImagesFilterTest, CapLimitsToFirstTwoCriticalImages) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical1.jpg");
  critical_images->insert("http://test.com/critical2.jpg");
  critical_images->insert("http://test.com/critical3.jpg");
  SetCriticalImages(critical_images);

  // The cap bounds high-priority images in document order: with three
  // beacon-critical images only the first two are annotated.
  ValidateExpected("cap_first_two",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical1.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"critical2.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>");
  EXPECT_EQ(
      2, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest, AuthorFetchPriorityRespected) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical1.jpg");
  critical_images->insert("http://test.com/critical2.jpg");
  critical_images->insert("http://test.com/critical3.jpg");
  SetCriticalImages(critical_images);

  // An author fetchpriority="low" is left untouched and does not consume a
  // slot of the cap: both remaining critical images are annotated.
  ValidateExpected("author_low",
                   "<head></head>"
                   "<body>"
                   "<img fetchpriority=\"low\" src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img fetchpriority=\"low\" src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"critical3.jpg\" fetchpriority=\"high\"/>"
                   "</body>");

  // An author fetchpriority="high" is left untouched but does consume a
  // slot: the cap bounds the total number of high-priority images, so only
  // one more image is annotated.
  ValidateExpected("author_high",
                   "<head></head>"
                   "<body>"
                   "<img fetchpriority=\"high\" src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img fetchpriority=\"high\" src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>");
  // Cap accounting: two annotations in the author-low document plus one in
  // the author-high document; author-supplied attributes are never counted
  // as applications of this filter.
  EXPECT_EQ(
      3, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest,
       AuthorFetchPriorityCaseVariantsAndValuelessRespected) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical1.jpg");
  critical_images->insert("http://test.com/critical2.jpg");
  critical_images->insert("http://test.com/critical3.jpg");
  SetCriticalImages(critical_images);

  // Attribute name lookup is case-insensitive (the keyword table is built
  // with gperf %ignore-case), so FETCHPRIORITY="high" is author markup too:
  // it is left untouched and consumes one slot of the cap.
  ValidateExpected("author_high_uppercase_name",
                   "<head></head>"
                   "<body>"
                   "<img FETCHPRIORITY=\"high\" src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img FETCHPRIORITY=\"high\" src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>");

  // The value is compared case-insensitively as well: fetchpriority="HIGH"
  // counts as high, so it is left untouched and consumes a slot.
  ValidateExpected("author_high_uppercase_value",
                   "<head></head>"
                   "<body>"
                   "<img fetchpriority=\"HIGH\" src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img fetchpriority=\"HIGH\" src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>");

  // A valueless fetchpriority attribute is still author markup and wins, but
  // with no "high" value it does not consume a slot of the cap: both
  // remaining critical images are annotated.
  ValidateExpected("author_valueless",
                   "<head></head>"
                   "<body>"
                   "<img fetchpriority src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\"/>"
                   "<img src=\"critical3.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img fetchpriority src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"critical3.jpg\" fetchpriority=\"high\"/>"
                   "</body>");
  // One annotation in each of the first two documents, two in the third;
  // author-supplied attributes are never counted.
  EXPECT_EQ(
      4, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest, NoOpWithoutCriticalImagesData) {
  InitPrioritizeCriticalImagesFilter();

  // Beacon data available but empty: the filter stays active but annotates
  // nothing.
  SetCriticalImages(nullptr);
  ValidateNoChanges("empty_critical_set",
                    "<head></head>"
                    "<body>"
                    "<img src=\"critical.jpg\"/>"
                    "</body>");

  // Beacon data expected but not yet arrived: strict no-op, the filter
  // disables itself for the document.
  server_context()->set_critical_images_finder(
      new NoDataYetCriticalImagesFinder(statistics()));
  ValidateNoChanges("no_data_yet",
                    "<head></head>"
                    "<body>"
                    "<img src=\"critical.jpg\"/>"
                    "</body>");
  EXPECT_EQ(
      0, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
  ExpectHtmlStatus(RewriterHtmlApplication::DISABLED);
}

TEST_F(PrioritizeCriticalImagesFilterTest, SaveDataRequestDisablesFilter) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);

  // Without a Save-Data header the filter applies normally.
  ValidateExpected("no_save_data",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\" fetchpriority=\"high\"/>"
                   "</body>");

  // A Save-Data client asked to conserve bytes; frontloading image fetches
  // works against that request, so the filter is a no-op. The driver holds on
  // to the request headers installed for the previous validation, so reset it
  // before queueing the Save-Data attribute for the next parse.
  ClearRewriteDriver();
  AddRequestAttribute(HttpAttributes::kSaveData, "on");
  ValidateNoChanges("save_data_on",
                    "<head></head>"
                    "<body>"
                    "<img src=\"critical.jpg\"/>"
                    "</body>");
  ExpectHtmlStatus(RewriterHtmlApplication::DISABLED);
}

TEST_F(PrioritizeCriticalImagesFilterTest, SaveDataVariantsCarryVaryHeader) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);

  // The filter's HTML output depends on the request's Save-Data header, so
  // both variants must carry "Vary: Save-Data"; otherwise a shared or browser
  // cache can serve the prioritized variant to a Save-Data client, or the
  // untouched variant to everyone else.
  ValidateExpected("no_save_data",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\" fetchpriority=\"high\"/>"
                   "</body>");
  EXPECT_TRUE(response_headers_.HasValue(HttpAttributes::kVary,
                                         HttpAttributes::kSaveData));

  // The disabled (Save-Data) variant must carry the same Vary. Clearing the
  // driver unwires its response-headers pointer, so re-install the harness
  // headers (after wiping the ones accumulated above) before re-parsing.
  ClearRewriteDriver();
  response_headers_.Clear();
  SetHtmlMimetype();
  AddRequestAttribute(HttpAttributes::kSaveData, "on");
  ValidateNoChanges("save_data_on",
                    "<head></head>"
                    "<body>"
                    "<img src=\"critical.jpg\"/>"
                    "</body>");
  EXPECT_TRUE(response_headers_.HasValue(HttpAttributes::kVary,
                                         HttpAttributes::kSaveData));

  // Without beacon data the filter is a strict no-op regardless of Save-Data,
  // but the page still carries the Vary: it describes how the resource is
  // selected over its lifetime, not how this one response was produced, so it
  // must not depend on whether a beacon has landed yet (#27).
  ClearRewriteDriver();
  response_headers_.Clear();
  SetHtmlMimetype();
  server_context()->set_critical_images_finder(
      new NoDataYetCriticalImagesFinder(statistics()));
  ValidateNoChanges("no_data_yet",
                    "<head></head>"
                    "<body>"
                    "<img src=\"critical.jpg\"/>"
                    "</body>");
  EXPECT_TRUE(response_headers_.HasValue(HttpAttributes::kVary,
                                         HttpAttributes::kSaveData));
}

TEST_F(PrioritizeCriticalImagesFilterTest,
       CachingFieldsAreCleanAfterVaryStamp) {
  // Regression test for the cache_fields_dirty_ invariant (#385): this
  // filter stamps Vary: Save-Data before HtmlVaryFilter runs, and when the
  // origin already sends a Vary token HtmlVaryFilter adds nothing itself, so
  // its recompute does not fire for this addition. The stamp must recompute:
  // without it, later caching-field readers DCHECK-abort in debug builds and
  // the public cache_fields_dirty() flag observes the dirty state in release
  // builds too.
  InitPrioritizeCriticalImagesFilter();
  SetHtmlMimetype();  // wires and computes the served headers, pre-parse.
  const bool browser_cacheable = response_headers_.IsBrowserCacheable();
  // An origin Vary token keeps HtmlVaryFilter from adding one itself.
  response_headers_.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);
  ValidateExpected("clean_after_stamp",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\" fetchpriority=\"high\"/>"
                   "</body>");
  EXPECT_TRUE(response_headers_.HasValue(HttpAttributes::kVary,
                                         HttpAttributes::kSaveData));
  EXPECT_FALSE(response_headers_.cache_fields_dirty());
  EXPECT_EQ(browser_cacheable, response_headers_.IsBrowserCacheable());
}

TEST_F(PrioritizeCriticalImagesFilterTest,
       LowercaseOriginVarySaveDataIsNotDuplicated) {
  // Vary values are RFC 9110 tokens: an origin's lowercase "save-data" names
  // the same field our "Save-Data" does and must not be duplicated.
  InitPrioritizeCriticalImagesFilter();
  response_headers_.Add(HttpAttributes::kVary, "save-data");
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);
  ValidateExpected("lowercase_not_duplicated",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\" fetchpriority=\"high\"/>"
                   "</body>");
  ConstStringStarVector values;
  ASSERT_TRUE(response_headers_.Lookup(HttpAttributes::kVary, &values));
  ASSERT_EQ(1, values.size());
  EXPECT_STREQ("save-data", values[0]->c_str());
}

TEST_F(PrioritizeCriticalImagesFilterTest, SaveDataUnsupportedEmitsNoVary) {
  // With Save-Data qualities equal to the normal ones SupportSaveData() is
  // false, so the Save-Data header no longer influences the filter: the
  // request gets the prioritized variant and no Vary is emitted. Options
  // freeze at the first parse, so the qualities must be set up front.
  options()->set_image_jpeg_quality_for_save_data(
      options()->ImageJpegQuality());
  options()->set_image_webp_quality_for_save_data(
      options()->ImageWebpQuality());
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);

  AddRequestAttribute(HttpAttributes::kSaveData, "on");
  ValidateExpected("save_data_unsupported",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img src=\"critical.jpg\" fetchpriority=\"high\"/>"
                   "</body>");
  EXPECT_FALSE(response_headers_.Has(HttpAttributes::kVary));
}

TEST_F(PrioritizeCriticalImagesFilterTest, VaryIsIndependentOfBeaconData) {
  InitPrioritizeCriticalImagesFilter();

  const char kHtml[] =
      "<head></head>"
      "<body>"
      "<img src=\"critical.jpg\"/>"
      "</body>";
  const char kAnnotatedHtml[] =
      "<head></head>"
      "<body>"
      "<img src=\"critical.jpg\" fetchpriority=\"high\"/>"
      "</body>";

  // The issue's matrix: the same page under three finder states (cold: no
  // beacon data yet; warm with a critical set; warm with an empty set), each
  // with and without a Save-Data request header. Vary keys the resource's
  // selection over its lifetime, so all six parses must carry the identical
  // Vary, with Save-Data in it exactly once: neither cache warmth nor the
  // request's own Save-Data header may move it (#27).
  GoogleString vary_values[3][2];

  server_context()->set_critical_images_finder(
      new NoDataYetCriticalImagesFinder(statistics()));
  ParseAndCollectVary("vary_no_data_yet", kHtml, false, kHtml,
                      &vary_values[0][0]);
  ParseAndCollectVary("vary_no_data_yet_save_data", kHtml, true, kHtml,
                      &vary_values[0][1]);

  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);
  ParseAndCollectVary("vary_warm_critical", kHtml, false, kAnnotatedHtml,
                      &vary_values[1][0]);
  ParseAndCollectVary("vary_warm_critical_save_data", kHtml, true, kHtml,
                      &vary_values[1][1]);

  SetCriticalImages(nullptr);
  ParseAndCollectVary("vary_warm_empty", kHtml, false, kHtml,
                      &vary_values[2][0]);
  ParseAndCollectVary("vary_warm_empty_save_data", kHtml, true, kHtml,
                      &vary_values[2][1]);

  for (auto& vary_value : vary_values) {
    for (int j = 0; j < 2; ++j) {
      EXPECT_EQ(vary_values[0][0], vary_value[j]);
      EXPECT_EQ(GoogleString(HttpAttributes::kSaveData), vary_value[j]);
    }
  }
}

TEST_F(PrioritizeCriticalImagesFilterTest, BeaconDisabledEmitsNoVary) {
  InitPrioritizeCriticalImagesFilter();
  server_context()->set_critical_images_finder(
      new DisabledCriticalImagesFinder(statistics()));

  // kDisabled is configuration state: critical-image reporting is off, so
  // the filter's output never depends on Save-Data and no Vary is emitted,
  // with or without the request header.
  ValidateNoChanges("beacon_disabled_no_save_data",
                    "<head></head>"
                    "<body>"
                    "<img src=\"critical.jpg\"/>"
                    "</body>");
  EXPECT_FALSE(response_headers_.Has(HttpAttributes::kVary));

  ClearRewriteDriver();
  response_headers_.Clear();
  SetHtmlMimetype();
  AddRequestAttribute(HttpAttributes::kSaveData, "on");
  ValidateNoChanges("beacon_disabled_save_data",
                    "<head></head>"
                    "<body>"
                    "<img src=\"critical.jpg\"/>"
                    "</body>");
  EXPECT_FALSE(response_headers_.Has(HttpAttributes::kVary));
}

TEST_F(PrioritizeCriticalImagesFilterTest, VaryWithoutImages) {
  InitPrioritizeCriticalImagesFilter();
  SetCriticalImages(nullptr);

  // The Vary rule is configuration-level, not content-level: a page with no
  // <img> at all still carries "Vary: Save-Data" while the filter and
  // critical-image reporting are enabled. Deciding the header from the page's
  // content would need the whole page buffered before the first flush, and
  // would flip with flush-window boundaries.
  ValidateNoChanges("vary_without_images",
                    "<head></head>"
                    "<body>"
                    "<p>nothing to prioritize here</p>"
                    "</body>");
  EXPECT_TRUE(response_headers_.HasValue(HttpAttributes::kVary,
                                         HttpAttributes::kSaveData));
}

TEST_F(PrioritizeCriticalImagesFilterTest, SkipsDataUrlMissingSrcAndNoscript) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);

  // data: URLs, images without a src, and images inside <noscript> are all
  // left untouched, even when the noscript image is beacon-critical.
  ValidateNoChanges("skipped_elements",
                    "<head></head>"
                    "<body>"
                    "<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhE\"/>"
                    "<img/>"
                    "<img src=\"\"/>"
                    "<noscript>"
                    "<img src=\"critical.jpg\"/>"
                    "</noscript>"
                    "</body>");
  EXPECT_EQ(
      0, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest, DisallowedUrlNotAnnotated) {
  options()->Disallow("*disallowed*");
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/disallowed.jpg");
  SetCriticalImages(critical_images);

  // A Disallow'ed image stays untouched even when it is beacon-critical.
  ValidateNoChanges("disallowed_url",
                    "<head></head>"
                    "<body>"
                    "<img src=\"disallowed.jpg\"/>"
                    "</body>");
  EXPECT_EQ(
      0, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest, AmpDocumentIsNoOp) {
  // Use the real registration path so the AMP document detector runs.
  AddFilter(RewriteOptions::kPrioritizeCriticalImages);
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);

  // AMP documents manage resource loading themselves and reject unexpected
  // attributes in validation, so the filter must leave them untouched.
  ValidateNoChanges("amp_no_op",
                    "<html amp><body>"
                    "<img src=\"critical.jpg\"/>"
                    "</body></html>");
  EXPECT_EQ(
      0, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));

  // The same page without the amp attribute is annotated normally.
  ValidateExpected("non_amp_applies",
                   "<html><body>"
                   "<img src=\"critical.jpg\"/>"
                   "</body></html>",
                   "<html><body>"
                   "<img src=\"critical.jpg\" fetchpriority=\"high\"/>"
                   "</body></html>");
  EXPECT_EQ(
      1, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest,
       RewrittenUrlDecodedToCriticalOriginal) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical4.jpg");
  SetCriticalImages(critical_images);

  // Criticality is keyed on the original URL; a .pagespeed. src is decoded
  // back to it before the lookup.
  GoogleString rewritten_url =
      Encode("http://test.com/", "ce", "HASH", "critical4.jpg", "jpg");
  ValidateExpected("rewritten_url_decoded",
                   StrCat("<head></head>"
                          "<body>"
                          "<img src=\"",
                          rewritten_url,
                          "\"/>"
                          "</body>"),
                   StrCat("<head></head>"
                          "<body>"
                          "<img src=\"",
                          rewritten_url,
                          "\" fetchpriority=\"high\"/>"
                          "</body>"));
  EXPECT_EQ(
      1, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest,
       NonCriticalSrcsetCandidatesNotAnnotated) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/hires.jpg");
  critical_images->insert("http://test.com/critical1.jpg");
  critical_images->insert("http://test.com/critical2.jpg");
  SetCriticalImages(critical_images);

  // For an image that has a src attribute, criticality stays keyed on src: a
  // beacon-critical URL appearing merely as its srcset candidate says nothing
  // about which candidate that image's browser selected; annotating it could
  // mis-prioritize a below-fold image and burn a slot of the cap, pushing a
  // genuinely critical image out. The mixed image must stay untouched while
  // both src-critical images are annotated.
  ValidateExpected("srcset_candidates_not_consulted",
                   "<head></head>"
                   "<body>"
                   "<img src=\"thumb.jpg\" srcset=\"hires.jpg 2x\"/>"
                   "<img src=\"critical1.jpg\"/>"
                   "<img src=\"critical2.jpg\"/>"
                   "</body>",
                   "<head></head>"
                   "<body>"
                   "<img src=\"thumb.jpg\" srcset=\"hires.jpg 2x\"/>"
                   "<img src=\"critical1.jpg\" fetchpriority=\"high\"/>"
                   "<img src=\"critical2.jpg\" fetchpriority=\"high\"/>"
                   "</body>");
  EXPECT_EQ(
      2, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest,
       SrcsetOnlyImageAnnotatedOnCriticalCandidate) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/hires.jpg");
  SetCriticalImages(critical_images);

  // Srcset-only images have no src to key on; the beacon reports the hash of
  // the candidate the browser actually displayed, so a critical candidate
  // makes the element eligible. fetchpriority is element-level and applies to
  // whichever candidate the browser selects. A srcset-only image with no
  // critical candidate stays untouched.
  ValidateExpected(
      "srcset_only_critical_candidate",
      "<head></head>"
      "<body>"
      "<img srcset=\"lowres.jpg 1x, hires.jpg 2x\"/>"
      "<img srcset=\"lowres.jpg 1x, other.jpg 2x\"/>"
      "</body>",
      "<head></head>"
      "<body>"
      "<img srcset=\"lowres.jpg 1x, hires.jpg 2x\" fetchpriority=\"high\"/>"
      "<img srcset=\"lowres.jpg 1x, other.jpg 2x\"/>"
      "</body>");
  EXPECT_EQ(
      1, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest, SrcsetOnlyImageConsumesCapSlot) {
  InitPrioritizeCriticalImagesFilter();
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/hires.jpg");
  critical_images->insert("http://test.com/critical1.jpg");
  critical_images->insert("http://test.com/critical2.jpg");
  SetCriticalImages(critical_images);

  // A prioritized srcset-only image consumes one of the two cap slots in
  // document order, squeezing the second src-critical image out.
  ValidateExpected(
      "srcset_only_consumes_cap_slot",
      "<head></head>"
      "<body>"
      "<img srcset=\"lowres.jpg 1x, hires.jpg 2x\"/>"
      "<img src=\"critical1.jpg\"/>"
      "<img src=\"critical2.jpg\"/>"
      "</body>",
      "<head></head>"
      "<body>"
      "<img srcset=\"lowres.jpg 1x, hires.jpg 2x\" fetchpriority=\"high\"/>"
      "<img src=\"critical1.jpg\" fetchpriority=\"high\"/>"
      "<img src=\"critical2.jpg\"/>"
      "</body>");
  EXPECT_EQ(
      2, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
}

TEST_F(PrioritizeCriticalImagesFilterTest, WorksWithNativeLazyload) {
  // Mirror the real pre-render order: this filter runs before lazyload.
  options()->set_lazyload_images_mode(
      RewriteOptions::kLazyloadImagesModeNative);
  options()->set_lazyload_images_skip_first(0);
  options()->DisallowTroublesomeResources();
  InitPrioritizeCriticalImagesFilter();
  lazyload_images_filter_ =
      std::make_unique<LazyloadImagesFilter>(rewrite_driver());
  rewrite_driver()->AddFilter(lazyload_images_filter_.get());

  StringSet* critical_images = new StringSet;
  critical_images->insert("http://test.com/critical.jpg");
  SetCriticalImages(critical_images);

  // The critical image gets exactly one fetchpriority attribute (this filter
  // sets it, lazyload's own guard then skips it), decoding="async" from
  // lazyload, and no loading="lazy". The non-critical image is lazyloaded.
  ValidateExpected(
      "with_native_lazyload",
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
  EXPECT_EQ(
      1, StatValue(
             PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied));
  EXPECT_EQ(1, StatValue(LazyloadImagesFilter::kLazyloadImagesSkippedCritical));
  EXPECT_EQ(1, StatValue(LazyloadImagesFilter::kLazyloadImagesNativeApplied));
}

}  // namespace net_instaweb
