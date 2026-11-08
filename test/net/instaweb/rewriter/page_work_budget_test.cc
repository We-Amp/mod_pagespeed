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

// The rewrite work one HTML document may cause is shared by all its
// rewrites; see PageWorkBudget.

#include "net/instaweb/rewriter/public/page_work_budget.h"

#include "base/logging.h"
#include "net/instaweb/http/public/counting_url_async_fetcher.h"
#include "net/instaweb/rewriter/public/rewrite_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const int kInputBytes = 1000;

class PageWorkBudgetTest : public RewriteTestBase {
 protected:
  void SetLimits(const PageWorkBudget::Limits& limits) {
    rewrite_driver()->page_work_budget()->set_limits_for_testing(limits);
  }

  const PageWorkBudget* budget() {
    return rewrite_driver()->page_work_budget();
  }

  int64 Stat(const char* name) {
    return statistics()->GetVariable(name)->Get();
  }

  static int CountOf(StringPiece haystack, StringPiece needle) {
    int count = 0;
    for (size_t pos = haystack.find(needle); pos != StringPiece::npos;
         pos = haystack.find(needle, pos + needle.size())) {
      ++count;
    }
    return count;
  }

  // A script of exactly kInputBytes.
  static GoogleString Script(char fill) {
    GoogleString script =
        StrCat("var s = '", GoogleString(kInputBytes - 12, fill), "';\n");
    CHECK_EQ(kInputBytes, script.size());
    return script;
  }

  // A stylesheet of exactly kInputBytes that the CSS filter minifies.
  static GoogleString Stylesheet(int n) {
    GoogleString css = StrCat(".c", IntegerToString(n), " { color: red; }\n");
    css.append(kInputBytes - css.size() - 1, ' ');
    css.append("\n");
    CHECK_EQ(kInputBytes, css.size());
    return css;
  }

  static GoogleString Repeat(StringPiece element, int n) {
    GoogleString out;
    for (int i = 0; i < n; ++i) {
      StrAppend(&out, element);
    }
    return out;
  }
};

// One script named many times is loaded once per element; for one document
// no more loads start once its byte budget is loaded.
TEST_F(PageWorkBudgetTest, CombinedScriptLoadsStopAtInputBytes) {
  SetResponseWithDefaultHeaders("big.js", kContentTypeJavascript, Script('x'),
                                100);
  AddFilter(RewriteOptions::kCombineJavascript);
  PageWorkBudget::Limits limits;
  limits.input_bytes = 3 * kInputBytes;
  SetLimits(limits);

  ParseUrl(kTestDomain, Repeat("<script src=big.js></script>", 12));
  EXPECT_LT(budget()->input_bytes_loaded(), 12 * kInputBytes);
  EXPECT_LE(budget()->input_bytes_loaded(),
            limits.input_bytes + limits.input_loads_in_flight * kInputBytes);
  EXPECT_LE(budget()->max_input_loads_in_flight(),
            limits.input_loads_in_flight);
  EXPECT_LE(1, Stat(RewriteContext::kRewriteBudgetInputBytesHits));
  EXPECT_EQ(0, CountOf(output_buffer_, ".pagespeed.jc."));
}

TEST_F(PageWorkBudgetTest, CombinedStylesheetLoadsStopAtInputBytes) {
  SetResponseWithDefaultHeaders("big.css", kContentTypeCss, Stylesheet(0), 100);
  AddFilter(RewriteOptions::kCombineCss);
  PageWorkBudget::Limits limits;
  limits.input_bytes = 3 * kInputBytes;
  SetLimits(limits);

  ParseUrl(kTestDomain, Repeat(CssLinkHref("big.css"), 12));
  EXPECT_LT(budget()->input_bytes_loaded(), 12 * kInputBytes);
  EXPECT_LE(budget()->input_bytes_loaded(),
            limits.input_bytes + limits.input_loads_in_flight * kInputBytes);
  EXPECT_LE(budget()->max_input_loads_in_flight(),
            limits.input_loads_in_flight);
  EXPECT_LE(1, Stat(RewriteContext::kRewriteBudgetInputBytesHits));
  EXPECT_EQ(0, CountOf(output_buffer_, ".pagespeed.cc."));
}

// The same holds for separate rewrites of one document.
TEST_F(PageWorkBudgetTest, SeparateRewritesShareInputBytes) {
  for (int i = 0; i < 10; ++i) {
    SetResponseWithDefaultHeaders(StrCat("big.js?v=", IntegerToString(i)),
                                  kContentTypeJavascript, Script('x'), 100);
  }
  AddFilter(RewriteOptions::kRewriteJavascriptExternal);
  PageWorkBudget::Limits limits;
  limits.input_bytes = 3 * kInputBytes;
  limits.input_loads_in_flight = 1;
  SetLimits(limits);

  GoogleString html;
  for (int i = 0; i < 10; ++i) {
    StrAppend(&html, "<script src=big.js?v=", IntegerToString(i), "></script>");
  }
  ParseUrl(kTestDomain, html);
  EXPECT_EQ(3 * kInputBytes, budget()->input_bytes_loaded());
  EXPECT_EQ(1, budget()->max_input_loads_in_flight());
  EXPECT_EQ(7, Stat(RewriteContext::kRewriteBudgetInputBytesHits));
  EXPECT_EQ(3, CountOf(output_buffer_, ".pagespeed.jm."));
}

// Only a few inputs of one document load at once; the rest wait for them
// and, once they end, are all rewritten.
TEST_F(PageWorkBudgetTest, InputLoadsWaitForEachOther) {
  SetupWaitFetcher();
  AddFilter(RewriteOptions::kRewriteCss);
  GoogleString html;
  for (int i = 0; i < 10; ++i) {
    GoogleString name = StrCat("s", IntegerToString(i), ".css");
    SetResponseWithDefaultHeaders(name, kContentTypeCss, Stylesheet(i), 100);
    StrAppend(&html, CssLinkHref(name));
  }
  PageWorkBudget::Limits limits;
  limits.input_loads_in_flight = 2;
  SetLimits(limits);

  // The loads do not end before the deadline.
  ParseUrl(kTestDomain, html);
  EXPECT_EQ(0, CountOf(output_buffer_, ".pagespeed.cf."));
  EXPECT_EQ(2, budget()->max_input_loads_in_flight());
  EXPECT_EQ(8, Stat(RewriteContext::kRewriteBudgetInputLoadsHits));
  CallFetcherCallbacks();
  EXPECT_EQ(0, Stat(RewriteContext::kRewriteBudgetInputBytesHits));
  EXPECT_EQ(10, counting_url_async_fetcher()->fetch_count());

  ParseUrl(kTestDomain, html);
  EXPECT_EQ(10, CountOf(output_buffer_, ".pagespeed.cf."));
}

// The default bound on loads in progress holds too.
TEST_F(PageWorkBudgetTest, DefaultInputLoadsInFlight) {
  SetupWaitFetcher();
  AddFilter(RewriteOptions::kRewriteCss);
  GoogleString html;
  for (int i = 0; i < 12; ++i) {
    GoogleString name = StrCat("s", IntegerToString(i), ".css");
    SetResponseWithDefaultHeaders(name, kContentTypeCss, Stylesheet(i), 100);
    StrAppend(&html, CssLinkHref(name));
  }
  ParseUrl(kTestDomain, html);
  const int kLimit = PageWorkBudget::Limits().input_loads_in_flight;
  EXPECT_EQ(kLimit, budget()->max_input_loads_in_flight());
  EXPECT_EQ(12 - kLimit, Stat(RewriteContext::kRewriteBudgetInputLoadsHits));
  CallFetcherCallbacks();
  ParseUrl(kTestDomain, html);
  EXPECT_EQ(12, CountOf(output_buffer_, ".pagespeed.cf."));
}

// Many query strings of one stylesheet are separate rewrites; the document
// starts only as many as it may.
TEST_F(PageWorkBudgetTest, StylesheetQueryStringsStopAtContexts) {
  for (int i = 0; i < 10; ++i) {
    SetResponseWithDefaultHeaders(StrCat("a.css?v=", IntegerToString(i)),
                                  kContentTypeCss, Stylesheet(0), 100);
  }
  AddFilter(RewriteOptions::kRewriteCss);
  PageWorkBudget::Limits limits;
  limits.contexts = 4;
  SetLimits(limits);

  GoogleString html;
  for (int i = 0; i < 10; ++i) {
    StrAppend(&html, CssLinkHref(StrCat("a.css?v=", IntegerToString(i))));
  }
  ParseUrl(kTestDomain, html);
  EXPECT_EQ(4, budget()->contexts_taken());
  EXPECT_EQ(6, Stat(RewriteContext::kRewriteBudgetContextsHits));
  EXPECT_EQ(4, CountOf(output_buffer_, ".pagespeed.cf."));
  // What was left undone is not cached as a failure: with room, the next
  // view rewrites all of them.
  limits.contexts = 100;
  SetLimits(limits);
  ParseUrl(kTestDomain, html);
  EXPECT_EQ(10, CountOf(output_buffer_, ".pagespeed.cf."));
}

// Nested rewrites count as well: a stylesheet's images are rewrites of
// their own.
TEST_F(PageWorkBudgetTest, NestedRewritesCountAsContexts) {
  options()->EnableFilter(RewriteOptions::kExtendCacheImages);
  AddFilter(RewriteOptions::kRewriteCss);
  GoogleString css;
  for (int i = 0; i < 10; ++i) {
    GoogleString image = StrCat("i", IntegerToString(i), ".png");
    // Distinct lifetimes give each image its own input info.
    SetResponseWithDefaultHeaders(image, kContentTypePng, "not really",
                                  100 + i);
    StrAppend(&css, ".i", IntegerToString(i), "{background:url(", image, ")}");
  }
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, css, 100);
  PageWorkBudget::Limits limits;
  limits.contexts = 5;
  SetLimits(limits);

  ParseUrl(kTestDomain, CssLinkHref("a.css"));
  EXPECT_EQ(5, budget()->contexts_taken());
  // The images past the budget get no nested rewrite at all; the stylesheet
  // counts the hit once.
  EXPECT_EQ(0, budget()->contexts_refused());
  EXPECT_EQ(1, Stat(RewriteContext::kRewriteBudgetContextsHits));
  // A stylesheet whose images were not all rewritten is left as it is.
  EXPECT_EQ(0, CountOf(output_buffer_, ".pagespeed.cf."));
  // With room, the next view rewrites it.
  limits.contexts = 100;
  SetLimits(limits);
  ParseUrl(kTestDomain, CssLinkHref("a.css"));
  EXPECT_EQ(1, CountOf(output_buffer_, ".pagespeed.cf."));
}

// A stylesheet with many images creates no more nested rewrites than the
// document has left, whether each image gets one nested rewrite or two.
TEST_F(PageWorkBudgetTest, StylesheetCreatesNestedRewritesWithinBudget) {
  options()->EnableFilter(RewriteOptions::kExtendCacheImages);
  options()->EnableFilter(RewriteOptions::kRecompressPng);
  AddFilter(RewriteOptions::kRewriteCss);
  GoogleString css;
  for (int i = 0; i < 200; ++i) {
    GoogleString image = StrCat("i", IntegerToString(i), ".png");
    SetResponseWithDefaultHeaders(image, kContentTypePng, "not really", 100);
    StrAppend(&css, ".i", IntegerToString(i), "{background:url(", image, ")}");
  }
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, css, 100);
  PageWorkBudget::Limits limits;
  limits.contexts = 10;
  SetLimits(limits);

  ParseUrl(kTestDomain, CssLinkHref("a.css"));
  // contexts_taken() includes the stylesheet; every nested rewrite created
  // was either taken or refused.
  EXPECT_EQ(10, budget()->contexts_taken());
  EXPECT_EQ(0, budget()->contexts_refused());
  EXPECT_EQ(1, Stat(RewriteContext::kRewriteBudgetContextsHits));
  EXPECT_EQ(0, CountOf(output_buffer_, ".pagespeed.cf."));
}

// A stylesheet that does not parse has its URLs rewritten textually; it too
// stops creating anything for them once the document has no rewrite left.
TEST_F(PageWorkBudgetTest, UnparsedStylesheetStopsAtContexts) {
  options()->EnableFilter(RewriteOptions::kFallbackRewriteCssUrls);
  options()->EnableFilter(RewriteOptions::kExtendCacheImages);
  options()->EnableFilter(RewriteOptions::kDebug);
  AddFilter(RewriteOptions::kRewriteCss);
  GoogleString css;
  for (int i = 0; i < 50; ++i) {
    StrAppend(&css, ".i", IntegerToString(i), "{background:url(i",
              IntegerToString(i), ".png)}");
  }
  StrAppend(&css, "}}}}");
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, css, 100);
  PageWorkBudget::Limits limits;
  limits.contexts = 1;
  SetLimits(limits);

  ParseUrl(kTestDomain, CssLinkHref("a.css"));
  EXPECT_EQ(1, budget()->contexts_taken());
  // Only the stylesheet itself became an input resource.
  EXPECT_EQ(1, Stat(RewriteStats::kResourceUrlDomainAcceptances));
  EXPECT_EQ(1, CountOf(output_buffer_, RewriteContext::kPageBudgetDebugMessage))
      << output_buffer_;
  EXPECT_EQ(0, CountOf(output_buffer_, ".pagespeed.cf."));
}

// Revalidating cached results loads their inputs through the same budget;
// a result whose inputs the budget no longer covers is left for a later view.
TEST_F(PageWorkBudgetTest, RevalidationSharesInputBytes) {
  const int kNumStylesheets = 4;
  AddFilter(RewriteOptions::kRewriteCss);
  GoogleString html;
  for (int i = 0; i < kNumStylesheets; ++i) {
    GoogleString name = StrCat("s", IntegerToString(i), ".css");
    SetResponseWithDefaultHeaders(name, kContentTypeCss, Stylesheet(i), 100);
    StrAppend(&html, CssLinkHref(name));
  }
  ParseUrl(kTestDomain, html);
  EXPECT_EQ(kNumStylesheets, CountOf(output_buffer_, ".pagespeed.cf."));

  // The inputs expire; their cached results can be revalidated.
  AdvanceTimeMs(200 * Timer::kSecondMs);
  PageWorkBudget::Limits limits;
  limits.input_bytes = 2 * kInputBytes + 1;
  limits.input_loads_in_flight = 1;
  SetLimits(limits);
  ClearStats();
  ParseUrl(kTestDomain, html);
  EXPECT_EQ(3, metadata_cache_info().num_successful_revalidates());
  EXPECT_EQ(3, CountOf(output_buffer_, ".pagespeed.cf."));
  EXPECT_EQ(3 * kInputBytes, budget()->input_bytes_loaded());
  EXPECT_EQ(1, budget()->max_input_loads_in_flight());
  EXPECT_EQ(1, Stat(RewriteContext::kRewriteBudgetInputBytesHits));

  ParseUrl(kTestDomain, html);
  EXPECT_EQ(kNumStylesheets, CountOf(output_buffer_, ".pagespeed.cf."));
}

// An input that is itself an optimized resource is rebuilt by a separate
// fetch; the document counts what that fetch hands back, once, not what the
// fetch loaded to build it.
TEST_F(PageWorkBudgetTest, RebuiltInputCountsOnce) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  AddFilter(RewriteOptions::kCombineCss);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, Stylesheet(0), 100);
  SetResponseWithDefaultHeaders("b.css", kContentTypeCss, Stylesheet(1), 100);
  const GoogleString a = Encode("", "cf", "0", "a.css", "css");
  const GoogleString b = Encode("", "cf", "0", "b.css", "css");

  ParseUrl(kTestDomain, StrCat(CssLinkHref(a), CssLinkHref(b)));
  // The combination (itself minified after) names both rebuilt inputs.
  EXPECT_EQ(1, CountOf(output_buffer_, "cc.0.css")) << output_buffer_;
  EXPECT_LT(0, budget()->input_bytes_loaded());
  EXPECT_GT(kInputBytes, budget()->input_bytes_loaded());
  EXPECT_EQ(2, budget()->contexts_taken());
}

// A page whose resources take more than one view's budget gets more of them
// rewritten on each view: what is done is a metadata hit that loads nothing.
TEST_F(PageWorkBudgetTest, LargePageConvergesOverViews) {
  const int kNumStylesheets = 7;
  AddFilter(RewriteOptions::kRewriteCss);
  GoogleString html;
  for (int i = 0; i < kNumStylesheets; ++i) {
    GoogleString name = StrCat("s", IntegerToString(i), ".css");
    SetResponseWithDefaultHeaders(name, kContentTypeCss, Stylesheet(i), 100);
    StrAppend(&html, CssLinkHref(name));
  }
  PageWorkBudget::Limits limits;
  limits.input_bytes = 2 * kInputBytes + 1;
  limits.input_loads_in_flight = 1;
  SetLimits(limits);

  int previous = 0;
  int views = 0;
  while (previous < kNumStylesheets) {
    ASSERT_GT(kNumStylesheets, views++) << "No progress after " << views;
    ParseUrl(kTestDomain, html);
    int rewritten = CountOf(output_buffer_, ".pagespeed.cf.");
    EXPECT_LT(previous, rewritten) << "view " << views;
    EXPECT_LE(budget()->input_bytes_loaded(), 3 * kInputBytes);
    previous = rewritten;
  }
  EXPECT_EQ(3, views);
  // Once all are done, a view loads nothing.
  ParseUrl(kTestDomain, html);
  EXPECT_EQ(kNumStylesheets, CountOf(output_buffer_, ".pagespeed.cf."));
  EXPECT_EQ(0, budget()->input_bytes_loaded());
}

// Repeated rewrites of one stylesheet copy its result; the copies count
// against the document's partition entries.
TEST_F(PageWorkBudgetTest, RepeatedRewritesStopAtPartitionEntries) {
  options()->EnableFilter(RewriteOptions::kExtendCacheImages);
  AddFilter(RewriteOptions::kRewriteCss);
  GoogleString css;
  for (int i = 0; i < 20; ++i) {
    GoogleString image = StrCat("i", IntegerToString(i), ".png");
    // Distinct lifetimes give each image its own input info.
    SetResponseWithDefaultHeaders(image, kContentTypePng, "not really",
                                  100 + i);
    StrAppend(&css, ".i", IntegerToString(i), "{background:url(", image, ")}");
  }
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, css, 100);
  PageWorkBudget::Limits limits;
  limits.partition_entries = 50;
  SetLimits(limits);

  ParseUrl(kTestDomain, Repeat(CssLinkHref("a.css"), 10));
  EXPECT_LE(budget()->partition_entries_taken(), limits.partition_entries);
  EXPECT_LE(1, Stat(RewriteContext::kRewriteBudgetPartitionEntriesHits));
  EXPECT_GT(10, CountOf(output_buffer_, ".pagespeed.cf."));
}

// With debug on, an element left for a later view says so, once.
TEST_F(PageWorkBudgetTest, DebugMessageOnElementLeftUndone) {
  options()->EnableFilter(RewriteOptions::kDebug);
  AddFilter(RewriteOptions::kRewriteCss);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, Stylesheet(0), 100);
  SetResponseWithDefaultHeaders("b.css", kContentTypeCss, Stylesheet(1), 100);
  PageWorkBudget::Limits limits;
  limits.contexts = 1;
  SetLimits(limits);

  ParseUrl(kTestDomain, StrCat(CssLinkHref("a.css"), CssLinkHref("b.css")));
  EXPECT_EQ(1, CountOf(output_buffer_, ".pagespeed.cf."));
  EXPECT_EQ(1, CountOf(output_buffer_, RewriteContext::kPageBudgetDebugMessage))
      << output_buffer_;
  size_t b_link = output_buffer_.find("b.css");
  ASSERT_NE(GoogleString::npos, b_link);
  EXPECT_LT(b_link,
            output_buffer_.find(RewriteContext::kPageBudgetDebugMessage));
}

// A .pagespeed. fetch keeps the bounds of a fetch and does not use the
// document budget of the driver that serves it, nested rewrites included.
TEST_F(PageWorkBudgetTest, FetchDoesNotUseDocumentBudget) {
  options()->EnableFilter(RewriteOptions::kExtendCacheImages);
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  AddFilter(RewriteOptions::kCombineCss);
  SetResponseWithDefaultHeaders("i.png", kContentTypePng, "not really", 100);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss,
                                ".a{background:url(i.png)}", 100);
  SetResponseWithDefaultHeaders("b.css", kContentTypeCss, ".b{color:red}", 100);
  PageWorkBudget::Limits limits;
  limits.input_bytes = 0;
  limits.input_loads_in_flight = 1;
  limits.contexts = 0;
  limits.partition_entries = 0;
  SetLimits(limits);

  GoogleString content;
  EXPECT_TRUE(FetchResourceUrl(
      Encode(kTestDomain, "cc", "0", MultiUrl("a.css", "b.css"), "css"),
      &content));
  EXPECT_NE(GoogleString::npos, content.find(".a{background:url(i.png)}"))
      << content;
  EXPECT_NE(GoogleString::npos, content.find(".b{color:red}")) << content;
  EXPECT_TRUE(FetchResourceUrl(Encode(kTestDomain, "cf", "0", "a.css", "css"),
                               &content));
  EXPECT_EQ(
      StrCat(".a{background:url(", Encode("", "ce", "0", "i.png", "png"), ")}"),
      content);
  EXPECT_EQ(0, Stat(RewriteContext::kRewriteBudgetInputBytesHits));
  EXPECT_EQ(0, Stat(RewriteContext::kRewriteBudgetInputLoadsHits));
  EXPECT_EQ(0, Stat(RewriteContext::kRewriteBudgetContextsHits));
  EXPECT_EQ(0, Stat(RewriteContext::kRewriteBudgetPartitionEntriesHits));
}

}  // namespace

}  // namespace net_instaweb
