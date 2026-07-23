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

#include "net/instaweb/rewriter/dependencies.pb.h"
#include "net/instaweb/rewriter/public/property_cache_util.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/mock_property_page.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/opt/http/request_context.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"

namespace net_instaweb {

namespace {

const char kRequestUrl[] = "http://www.example.com/";

class PushPreloadFilterTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    options()->EnableFilter(RewriteOptions::kHintPreloadSubresources);

    // Setup pcache.
    pcache_ = rewrite_driver()->server_context()->page_property_cache();
    const PropertyCache::Cohort* deps_cohort =
        SetupCohort(pcache_, RewriteDriver::kDependenciesCohort);
    server_context()->set_dependencies_cohort(deps_cohort);
    ResetDriver();

    SetResponseWithDefaultHeaders("a.css", kContentTypeCss,
                                  " *  { display: block }", 100);
    SetResponseWithDefaultHeaders("b.js", kContentTypeJavascript,
                                  " var b  = 42", 200);
  }

  void ResetDriver() {
    rewrite_driver()->Clear();
    rewrite_driver()->set_request_context(
        RequestContext::NewTestRequestContext(factory()->thread_system()));
    page_ = NewMockPage(kRequestUrl);
    rewrite_driver()->set_property_page(page_);
    pcache_->Read(page_);
    rewrite_driver()->PropertyCacheSetupDone();
    SetHtmlMimetype();  // Don't wrap scripts in <![CDATA[ ]]>
  }

  PropertyCache* pcache_;
  PropertyPage* page_;
};

TEST_F(PushPreloadFilterTest, WeirdTiming) {
  // Event buffering causes us to clear mutable_response_headers()
  // at first flush window even if we haven't yet even delivered StartDocument.
  // At the very least, that shouldn't cause us to crash.
  // TODO(morlovich): Discuss what the API should be with Josh.
  rewrite_driver()->AddFilters();
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->Flush();
  rewrite_driver()->FinishParse();
}

TEST_F(PushPreloadFilterTest, BasicOperation) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
  rewrite_driver()->AddFilters();

  static const char kInput[] =
      "<link rel=stylesheet href=a.css>"
      "<script src=b.js></script>";

  static const char kOutput[] =
      "<link rel=stylesheet href=A.a.css.pagespeed.cf.0.css>"
      "<script src=b.js.pagespeed.jm.0.js></script>";

  ValidateExpected("basic_res", kInput, kOutput);

  // Now that we've collected stuff, see if it produces proper headers.
  ResetDriver();
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->ParseText("<!doctype html><html>");
  rewrite_driver()->Flush();  // Run filters
  ConstStringStarVector links;

  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
  rewrite_driver()->FinishParse();

  ASSERT_EQ(2, links.size());
  EXPECT_STREQ("</A.a.css.pagespeed.cf.0.css>; rel=preload; as=style; nopush",
               *links[0]);
  EXPECT_STREQ("</b.js.pagespeed.jm.0.js>; rel=preload; as=script; nopush",
               *links[1]);
}

TEST_F(PushPreloadFilterTest, ModuleScriptNotHinted) {
  // Module srcs are not collected, so no as=script Link hint is emitted for
  // them: a plain preload occupies a different preload-cache slot than the
  // module map fetch and would only double-fetch.
  rewrite_driver()->AddFilters();

  static const char kInput[] =
      "<link rel=stylesheet href=a.css>"
      "<script type=module src=b.js></script>";

  ValidateNoChanges("module_not_hinted", kInput);

  ResetDriver();
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->ParseText("<!doctype html><html>");
  rewrite_driver()->Flush();  // Run filters
  ConstStringStarVector links;

  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
  rewrite_driver()->FinishParse();

  ASSERT_EQ(1, links.size());
  EXPECT_STREQ("</a.css>; rel=preload; as=style; nopush", *links[0]);
}

TEST_F(PushPreloadFilterTest, Invalidation) {
  // Test for keeping track of us remembering when things expire.
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
  rewrite_driver()->AddFilters();

  // b.js expires in 200, a.css expires in 100.
  static const char kInput[] =
      "<script src=b.js></script>"
      "<link rel=stylesheet href=a.css>";
  static const char kOutput[] =
      "<script src=b.js.pagespeed.jm.0.js></script>"
      "<link rel=stylesheet href=A.a.css.pagespeed.cf.0.css>";

  ValidateExpected("invalidation", kInput, kOutput);

  // Enough to invalidate a, but not b.
  AdvanceTimeMs(150 * 1000);
  ResetDriver();
  ValidateExpected("invalidation2", kInput, kOutput);

  ConstStringStarVector links;
  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));

  // Only b.js should be pushed --- or rather the .pagespeed version.
  ASSERT_EQ(1, links.size());
  EXPECT_STREQ("</b.js.pagespeed.jm.0.js>; rel=preload; as=script; nopush",
               *links[0]);
}

TEST_F(PushPreloadFilterTest, Invalidation2) {
  // Test for keeping track of us remembering when things expire.
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
  rewrite_driver()->AddFilters();

  // b.js expires in 200, a.css expires in 100.
  static const char kInput[] =
      "<script src=b.js></script>"
      "<link rel=stylesheet href=a.css>";
  static const char kOutput[] =
      "<script src=b.js.pagespeed.jm.0.js></script>"
      "<link rel=stylesheet href=A.a.css.pagespeed.cf.0.css>";

  ValidateExpected("invalidation", kInput, kOutput);

  // Enough to invalidate both a and b.
  AdvanceTimeMs(250 * 1000);
  ResetDriver();
  ValidateExpected("invalidation2", kInput, kOutput);

  ConstStringStarVector links;
  EXPECT_FALSE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
}

TEST_F(PushPreloadFilterTest, InvalidationOrder) {
  // Test for keeping track of us remembering when things expire.
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
  rewrite_driver()->AddFilters();

  // b.js expires in 200, a.css expires in 100.
  static const char kInput[] =
      "<link rel=stylesheet href=a.css>"
      "<script src=b.js></script>";
  static const char kOutput[] =
      "<link rel=stylesheet href=A.a.css.pagespeed.cf.0.css>"
      "<script src=b.js.pagespeed.jm.0.js></script>";

  ValidateExpected("invalidation", kInput, kOutput);

  // Enough to invalidate a, but not b. However, since a is before b, nothing
  // will be hinted.
  AdvanceTimeMs(150 * 1000);
  ResetDriver();
  ValidateExpected("invalidation2", kInput, kOutput);

  ConstStringStarVector links;
  EXPECT_FALSE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
}

TEST_F(PushPreloadFilterTest, IndirectCollected) {
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss,
                                "@import \"i1.css\" all;\n"
                                "@import \"i2.css\" print, screen;\n"
                                "@import \"i3.css\" print;         ",
                                100);
  SetResponseWithDefaultHeaders("d.css", kContentTypeCss,
                                "@import \"i1.css\" all;    \n"
                                "@import \"i4.css\";    ",
                                100);
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<link rel=stylesheet href=c.css>"
      "<link rel=stylesheet href=d.css>";
  const char kOutput[] =
      "<link rel=stylesheet href=A.c.css.pagespeed.cf.0.css>"
      "<link rel=stylesheet href=A.d.css.pagespeed.cf.0.css>";

  ValidateExpected("basic_res", kInput, kOutput);

  // Now that we've collected stuff, see if it produces proper headers.
  ResetDriver();
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->ParseText("<!doctype html><html>");
  rewrite_driver()->Flush();  // Run filters
  ConstStringStarVector links;

  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
  rewrite_driver()->FinishParse();

  ASSERT_EQ(5, links.size());
  // These should be in preorder wrt to the dependencies between
  // CSS and things in it
  EXPECT_STREQ("</A.c.css.pagespeed.cf.0.css>; rel=preload; as=style; nopush",
               *links[0]);
  EXPECT_STREQ("</i1.css>; rel=preload; as=style; nopush", *links[1]);
  EXPECT_STREQ("</i2.css>; rel=preload; as=style; nopush", *links[2]);
  EXPECT_STREQ("</A.d.css.pagespeed.cf.0.css>; rel=preload; as=style; nopush",
               *links[3]);
  // not i3, since it's print only.

  // i1 already hinted.
  // i4 isn't, though.
  EXPECT_STREQ("</i4.css>; rel=preload; as=style; nopush", *links[4]);
}

TEST_F(PushPreloadFilterTest, FontPreloadEmission) {
  // A collected woff2 @font-face emits as=font with crossorigin (fonts are
  // always fetched in anonymous CORS mode; a preload without matching
  // crossorigin would be a separate cache entry and cause a double fetch),
  // positioned right after its parent stylesheet's entry.
  SetResponseWithDefaultHeaders("f.css", kContentTypeCss,
                                "@font-face { font-family: Demo;"
                                " src: url(x.woff2) format(\"woff2\"); }"
                                " * { display: block }",
                                100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_emission", "<link rel=stylesheet href=f.css>");

  ResetDriver();
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->ParseText("<!doctype html><html>");
  rewrite_driver()->Flush();  // Run filters
  ConstStringStarVector links;

  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
  rewrite_driver()->FinishParse();

  ASSERT_EQ(2, links.size());
  EXPECT_STREQ("</f.css>; rel=preload; as=style; nopush", *links[0]);
  EXPECT_STREQ("</x.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[1]);
}

TEST_F(PushPreloadFilterTest, NoCrossoriginOnStylesAndScripts) {
  // Regression pin: CSS and JS preloads are no-cors destinations and must
  // stay byte-identical to what was emitted before font support ---
  // in particular, no crossorigin param.
  rewrite_driver()->AddFilters();

  ValidateNoChanges("no_crossorigin",
                    "<link rel=stylesheet href=a.css>"
                    "<script src=b.js></script>");

  ResetDriver();
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->ParseText("<!doctype html><html>");
  rewrite_driver()->Flush();  // Run filters
  ConstStringStarVector links;

  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
  rewrite_driver()->FinishParse();

  ASSERT_EQ(2, links.size());
  EXPECT_STREQ("</a.css>; rel=preload; as=style; nopush", *links[0]);
  EXPECT_STREQ("</b.js>; rel=preload; as=script; nopush", *links[1]);
  for (int i = 0, n = links.size(); i < n; ++i) {
    EXPECT_EQ(GoogleString::npos, links[i]->find("crossorigin"))
        << "crossorigin leaked onto a non-font preload: " << *links[i];
  }
}

TEST_F(PushPreloadFilterTest, FontPreloadCap) {
  // At most four font preloads per page, counted in emission order;
  // the stylesheet hint itself is not capped.
  SetResponseWithDefaultHeaders(
      "many.css", kContentTypeCss,
      "@font-face { font-family: A; src: url(w1.woff2) format(\"woff2\"); }"
      "@font-face { font-family: B; src: url(w2.woff2) format(\"woff2\"); }"
      "@font-face { font-family: C; src: url(w3.woff2) format(\"woff2\"); }"
      "@font-face { font-family: D; src: url(w4.woff2) format(\"woff2\"); }"
      "@font-face { font-family: E; src: url(w5.woff2) format(\"woff2\"); }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_cap", "<link rel=stylesheet href=many.css>");

  ResetDriver();
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->ParseText("<!doctype html><html>");
  rewrite_driver()->Flush();  // Run filters
  ConstStringStarVector links;

  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
  rewrite_driver()->FinishParse();

  ASSERT_EQ(5, links.size());  // 1 stylesheet + 4 of the 5 fonts.
  EXPECT_STREQ("</many.css>; rel=preload; as=style; nopush", *links[0]);
  EXPECT_STREQ("</w1.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[1]);
  EXPECT_STREQ("</w2.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[2]);
  EXPECT_STREQ("</w3.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[3]);
  EXPECT_STREQ("</w4.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[4]);
  for (int i = 0, n = links.size(); i < n; ++i) {
    EXPECT_EQ(GoogleString::npos, links[i]->find("w5.woff2"))
        << "The fifth font must not be hinted: " << *links[i];
  }
}

TEST_F(PushPreloadFilterTest, FontPreloadCapIsPageGlobal) {
  // The font cap is per page, not per stylesheet: 3 faces in one stylesheet
  // plus 2 in another emit exactly 4 font hints, in emission order --- the
  // second stylesheet only gets the one remaining slot.
  SetResponseWithDefaultHeaders(
      "f1.css", kContentTypeCss,
      "@font-face { font-family: A; src: url(a1.woff2) format(\"woff2\"); }"
      "@font-face { font-family: B; src: url(a2.woff2) format(\"woff2\"); }"
      "@font-face { font-family: C; src: url(a3.woff2) format(\"woff2\"); }",
      100);
  SetResponseWithDefaultHeaders(
      "f2.css", kContentTypeCss,
      "@font-face { font-family: D; src: url(b1.woff2) format(\"woff2\"); }"
      "@font-face { font-family: E; src: url(b2.woff2) format(\"woff2\"); }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_cap_page_global",
                    "<link rel=stylesheet href=f1.css>"
                    "<link rel=stylesheet href=f2.css>");

  ResetDriver();
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->ParseText("<!doctype html><html>");
  rewrite_driver()->Flush();  // Run filters
  ConstStringStarVector links;

  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
  rewrite_driver()->FinishParse();

  ASSERT_EQ(6, links.size());  // 2 stylesheets + 4 of the 5 fonts.
  EXPECT_STREQ("</f1.css>; rel=preload; as=style; nopush", *links[0]);
  EXPECT_STREQ("</a1.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[1]);
  EXPECT_STREQ("</a2.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[2]);
  EXPECT_STREQ("</a3.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[3]);
  EXPECT_STREQ("</f2.css>; rel=preload; as=style; nopush", *links[4]);
  EXPECT_STREQ("</b1.woff2>; rel=preload; as=font; crossorigin; nopush",
               *links[5]);
  for (int i = 0, n = links.size(); i < n; ++i) {
    EXPECT_EQ(GoogleString::npos, links[i]->find("b2.woff2"))
        << "The fifth font must not be hinted: " << *links[i];
  }
}

TEST_F(PushPreloadFilterTest, OldEntryNewBinary) {
  // A pcache entry written before DEP_FONT existed carries only the
  // 'dependency' field. Emission for such an entry must stay byte-identical
  // to what that binary produced: same params, same order, nothing added.
  rewrite_driver()->AddFilters();

  ResetDriver();
  Dependencies old_deps;
  Dependency* css_dep = old_deps.add_dependency();
  css_dep->set_url("http://test.com/old.css");
  css_dep->set_content_type(DEP_CSS);
  css_dep->add_order_key(0);
  Dependency* js_dep = old_deps.add_dependency();
  js_dep->set_url("http://test.com/old.js");
  js_dep->set_content_type(DEP_JAVASCRIPT);
  js_dep->add_order_key(1);
  // "dependencies" is kDepProp in dependency_tracker.cc.
  UpdateInPropertyCache(old_deps, rewrite_driver(),
                        server_context()->dependencies_cohort(), "dependencies",
                        true /* write_cohort */);

  ResetDriver();  // Re-read the pcache with the injected legacy entry.
  rewrite_driver()->StartParse(kTestDomain);
  rewrite_driver()->ParseText("<!doctype html><html>");
  rewrite_driver()->Flush();  // Run filters
  ConstStringStarVector links;

  EXPECT_TRUE(rewrite_driver()->response_headers()->Lookup(
      HttpAttributes::kLink, &links));
  rewrite_driver()->FinishParse();

  ASSERT_EQ(2, links.size());
  EXPECT_STREQ("</old.css>; rel=preload; as=style; nopush", *links[0]);
  EXPECT_STREQ("</old.js>; rel=preload; as=script; nopush", *links[1]);
}

}  // namespace

}  // namespace net_instaweb
