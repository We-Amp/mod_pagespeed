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

#include "net/instaweb/rewriter/public/collect_dependencies_filter.h"

#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/http_cache_failure.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/dependencies.pb.h"
#include "net/instaweb/rewriter/public/dependency_tracker.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/mock_property_page.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/kernel/base/cache_interface.h"
#include "pagespeed/kernel/base/hasher.h"
#include "pagespeed/kernel/base/proto_matcher.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/util/url_segment_encoder.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/opt/http/request_context.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"

namespace net_instaweb {

namespace {

const char kRequestUrl[] = "http://www.example.com/";

class CollectDependenciesFilterTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    options()->EnableFilter(RewriteOptions::kExperimentHttp2);

    // Setup pcache.
    pcache_ = rewrite_driver()->server_context()->page_property_cache();
    const PropertyCache::Cohort* deps_cohort =
        SetupCohort(pcache_, RewriteDriver::kDependenciesCohort);
    server_context()->set_dependencies_cohort(deps_cohort);
    ResetDriver();

    // TODO(morlovich): Once indirect dependency collection is in, make sure to
    // test that it interacts with this correctly.
    SetResponseWithDefaultHeaders("a.css", kContentTypeCss,
                                  " *  { display: block }", 100);
    SetResponseWithDefaultHeaders("c.css", kContentTypeCss,
                                  " *  { display: list-item }", 150);

    SetResponseWithDefaultHeaders("b.js", kContentTypeJavascript,
                                  " var b  = 42", 200);
    SetResponseWithDefaultHeaders("d.js", kContentTypeJavascript,
                                  " var d  = 32", 250);
    start_time_ms_ = timer()->NowMs();
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

  GoogleString FormatRelTimeSec(int delta_sec) {
    return Integer64ToString(start_time_ms_ + delta_sec * Timer::kSecondMs);
  }

  // Reconstructs the cdf context's metadata cache key for a resource. Mirrors
  // RewriteContext::SetPartitionKey() for a single-slot context with the
  // default encoder, no UA-specific cache key, and no CacheKeySuffix; if that
  // scheme changes, callers' asserts on the entry fail loudly rather than
  // going vacuous. (FontsNeverInOldMetadataCacheStore reconstructs the same
  // key inline; it predates this helper.)
  GoogleString CdfPartitionKey(StringPiece resource_url) {
    GoogleString signature = server_context()->lock_hasher()->Hash(
        rewrite_driver()->options()->signature());
    GoogleString encoding;
    UrlSegmentEncoder encoder;
    StringVector dummy_url_keys;
    dummy_url_keys.push_back("");
    encoder.Encode(dummy_url_keys, nullptr, &encoding);
    return StrCat(ServerContext::kCacheKeyResourceNamePrefix, "cdf", "_",
                  signature, "/", resource_url, "@", encoding, "@_");
  }

  // Reads back the OutputPartitions the collector wrote to the metadata cache
  // for a resource --- the store the tracker's content_type re-routing (and
  // Report()'s type repair) can never fix up on the way out.
  void ReadCdfPartitions(StringPiece resource_url, OutputPartitions* out) {
    const GoogleString key = CdfPartitionKey(resource_url);
    CacheInterface::SynchronousCallback callback;
    lru_cache()->Get(key, &callback);
    ASSERT_TRUE(callback.called());
    ASSERT_EQ(CacheInterface::kAvailable, callback.state())
        << "No cdf metadata entry at the reconstructed key " << key
        << "; RewriteContext::SetPartitionKey() may have changed.";
    StringPiece raw = callback.value_as_string_piece();
    ASSERT_TRUE(out->ParseFromArray(raw.data(), raw.size()));
  }

  PropertyCache* pcache_;
  PropertyPage* page_;
  int64 start_time_ms_;
  const int64 kYearSec = Timer::kYearMs / Timer::kSecondMs;
};

TEST_F(CollectDependenciesFilterTest, BasicOperation) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<link rel=stylesheet href=a.css>"
      "<script src=b.js></script>";

  const char kOutput[] =
      "<link rel=stylesheet href=A.a.css.pagespeed.cf.0.css>"
      "<script src=b.js.pagespeed.jm.0.js></script>";

  ValidateExpected("basic_res", kInput, kOutput);

  // Read stuff back in from pcache.
  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);

  EXPECT_THAT(
      *tracker->read_in_info(),
      EqualsProto(StrCat("dependency {"
                         "url: 'http://test.com/A.a.css.pagespeed.cf.0.css'"
                         "content_type: DEP_CSS "
                         "validity_info {"
                         "type: CACHED "
                         "expiration_time_ms: ",
                         FormatRelTimeSec(100),
                         " "
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "validity_info {"
                         "type: CACHED "
                         "last_modified_time_ms: ",
                         FormatRelTimeSec(0), " ",
                         "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
                         " "
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "order_key: 0"
                         "}"
                         "dependency {"
                         "url: 'http://test.com/b.js.pagespeed.jm.0.js'"
                         "content_type: DEP_JAVASCRIPT "
                         "validity_info {"
                         "type: CACHED "
                         "expiration_time_ms: ",
                         FormatRelTimeSec(200),
                         " "
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "validity_info {"
                         "type: CACHED "
                         "last_modified_time_ms: ",
                         FormatRelTimeSec(0), " ",
                         "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
                         " "
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "order_key: 1"
                         "}")));

  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, MediaTopLevel) {
  SetResponseWithDefaultHeaders("e.css", kContentTypeCss,
                                " *  { display: inline-block }", 400);

  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<link rel=stylesheet href=a.css media=\"screen,print\">"
      "<link rel=stylesheet href=c.css media=print>"
      "<link rel=stylesheet href=e.css media=all>";

  ValidateNoChanges("media_top_level", kInput);

  // Read stuff back in from pcache.
  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/a.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "dependency {"
                                 "url: 'http://test.com/e.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(400),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 1"
                                 "}")));

  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, ModuleScriptCollected) {
  // Module scripts are collected as DEP_MODULE, into the dedicated
  // module_dependency field, so they can be hinted with rel=modulepreload.
  // Classic scripts on the same page keep going into 'dependency'.
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<script type=module src=b.js></script>"
      "<script src=d.js></script>";
  ValidateNoChanges("module_script", kInput);

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/d.js'"
                                 "content_type: DEP_JAVASCRIPT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(250),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 1"
                                 "}"
                                 "module_dependency {"
                                 "url: 'http://test.com/b.js'"
                                 "content_type: DEP_MODULE "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(200),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, ModuleScriptWithIntegrityNotCollected) {
  // A modulepreload entry is matched on its integrity metadata, and Dependency
  // has nowhere to carry it, so an integrity-bearing module would get a hint
  // the browser cannot match --- a double fetch, worse than the no hint these
  // got before. Classic scripts are unaffected by the integrity attribute.
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<script type=module src=b.js integrity=sha384-abc></script>"
      "<script src=d.js></script>";
  ValidateNoChanges("module_integrity", kInput);

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  const Dependencies& deps = *tracker->read_in_info();
  EXPECT_EQ(0, deps.module_dependency_size());
  ASSERT_EQ(1, deps.dependency_size());
  EXPECT_EQ("http://test.com/d.js", deps.dependency(0).url());
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, ModuleScriptUseCredentialsNotCollected) {
  // crossorigin=use-credentials is credentials mode 'include'; a
  // modulepreload hint carries no crossorigin and so fetches same-origin.
  // Those are different module-map entries, so the hinted fetch would never
  // be reused. Dependency cannot carry the credentials mode, so skip.
  //
  // Only the exact value is excluded, and the value is compared unmodified:
  // per HTML, crossorigin is an enumerated attribute whose invalid values
  // (including a padded " use-credentials ") map to Anonymous --- which is
  // exactly what a modulepreload fetches with, so d.js IS hintable.
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<script type=module src=b.js crossorigin=use-credentials></script>"
      "<script type=module src=d.js crossorigin=\" use-credentials "
      "\"></script>";
  ValidateNoChanges("module_use_credentials", kInput);

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  const Dependencies& deps = *tracker->read_in_info();
  EXPECT_EQ(0, deps.dependency_size());
  ASSERT_EQ(1, deps.module_dependency_size());
  EXPECT_EQ("http://test.com/d.js", deps.module_dependency(0).url());
  EXPECT_EQ(DEP_MODULE, deps.module_dependency(0).content_type());
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest,
       ModuleScriptAnonymousCrossoriginCollected) {
  // Anonymous is the crossorigin attribute's default state, and it is exactly
  // what a modulepreload hint fetches with, so a value-less or
  // crossorigin=anonymous module is collected like any other.
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<script type=module src=b.js crossorigin=anonymous></script>"
      "<script type=module src=d.js crossorigin></script>";
  ValidateNoChanges("module_anonymous_crossorigin", kInput);

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  const Dependencies& deps = *tracker->read_in_info();
  EXPECT_EQ(0, deps.dependency_size());
  ASSERT_EQ(2, deps.module_dependency_size());
  EXPECT_EQ("http://test.com/b.js", deps.module_dependency(0).url());
  EXPECT_EQ(DEP_MODULE, deps.module_dependency(0).content_type());
  EXPECT_EQ("http://test.com/d.js", deps.module_dependency(1).url());
  EXPECT_EQ(DEP_MODULE, deps.module_dependency(1).content_type());
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, ModuleTypeFollowsPageNotMetadataCache) {
  // The cdf metadata cache key does not include the dependency type, so the
  // partition written for a URL loaded as a classic script is replayed
  // verbatim for a page that loads the same URL as a module. The type must
  // follow *this* page's parse, otherwise the module gets an as=script hint
  // (the double fetch) --- or, in the other direction, a classic script gets
  // rel=modulepreload.
  //
  // Report()'s repair is unconditional, so it equally governs the
  // pre-existing CSS/JS cross-type replay: a URL loaded as a stylesheet on
  // one page and as a script on another no longer carries the other page's
  // stale as= param.
  rewrite_driver()->AddFilters();

  // First page: b.js as a classic script. This writes the cdf partition.
  ValidateNoChanges("classic_first", "<script src=b.js></script>");

  // Second page: same URL, now a module. The cdf context hits the metadata
  // cache written above.
  ResetDriver();
  ValidateNoChanges("module_second", "<script type=module src=b.js></script>");

  // Prove the replay actually happened rather than the module page silently
  // recomputing: the metadata entry must still be the classic partition the
  // first page wrote. A miss would have run Rewrite() again and left
  // collected_module_dependency here instead. Without this the test could go
  // vacuous if the key scheme ever stopped colliding.
  {
    OutputPartitions partitions;
    ASSERT_NO_FATAL_FAILURE(
        ReadCdfPartitions("http://test.com/b.js", &partitions));
    ASSERT_EQ(1, partitions.partition_size());
    EXPECT_EQ(0, partitions.partition(0).collected_module_dependency_size())
        << "The module page recomputed the partition; it must have replayed "
           "the classic one, or this test proves nothing.";
    ASSERT_EQ(1, partitions.partition(0).collected_dependency_size());
    EXPECT_EQ(DEP_JAVASCRIPT,
              partitions.partition(0).collected_dependency(0).content_type());
  }

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  const Dependencies& deps = *tracker->read_in_info();
  EXPECT_EQ(0, deps.dependency_size());
  ASSERT_EQ(1, deps.module_dependency_size());
  EXPECT_EQ("http://test.com/b.js", deps.module_dependency(0).url());
  EXPECT_EQ(DEP_MODULE, deps.module_dependency(0).content_type());
  rewrite_driver()->FinishParse();

  // And back the other way: the module partition just written must not make
  // b.js read back as a module on a page that loads it classically.
  ResetDriver();
  ValidateNoChanges("classic_again", "<script src=b.js></script>");

  ResetDriver();
  tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  const Dependencies& deps2 = *tracker->read_in_info();
  EXPECT_EQ(0, deps2.module_dependency_size());
  ASSERT_EQ(1, deps2.dependency_size());
  EXPECT_EQ("http://test.com/b.js", deps2.dependency(0).url());
  EXPECT_EQ(DEP_JAVASCRIPT, deps2.dependency(0).content_type());
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, ModulesNeverInOldSerializedFields) {
  // The downgrade contract, the DEP_FONT rule applied to DEP_MODULE: a
  // DEP_MODULE entry must only ever appear in the dedicated module fields.
  // In 'dependency' a binary predating DEP_MODULE would read its content_type
  // as the closed-enum field default (DEP_JAVASCRIPT) and emit as=script for
  // a module --- the exact double fetch this feature exists to avoid.
  rewrite_driver()->AddFilters();

  ValidateNoChanges("module_downgrade_contract",
                    "<script type=module src=b.js></script>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  const Dependencies& deps = *tracker->read_in_info();
  for (int i = 0; i < deps.dependency_size(); ++i) {
    EXPECT_NE(DEP_MODULE, deps.dependency(i).content_type())
        << "DEP_MODULE leaked into the pre-DEP_MODULE 'dependency' field: "
        << deps.dependency(i).url();
  }
  ASSERT_EQ(1, deps.module_dependency_size());
  EXPECT_EQ(DEP_MODULE, deps.module_dependency(0).content_type());
  EXPECT_EQ("http://test.com/b.js", deps.module_dependency(0).url());
  // NOTE: this pcache-side check cannot fail for a module mis-filed by the
  // collector --- Report() forces content_type from this page's parse and the
  // tracker re-routes by content_type, repairing it on the way out. The store
  // that matters is pinned by ModulesNeverInOldMetadataCacheStore.
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, ModulesNeverInOldMetadataCacheStore) {
  // Companion to ModulesNeverInOldSerializedFields, pinning the OTHER store:
  // the cdf OutputPartitions entry in the metadata cache, which the collector
  // writes directly and which nothing downstream can repair. This is the test
  // that actually guards the downgrade contract --- a module mis-filed into
  // collected_dependency reaches the pcache correctly anyway (Report() forces
  // the type, the tracker re-routes on it), but sits wrong in the metadata
  // cache forever. An r20 binary replaying that partition reads DEP_MODULE
  // out of a field it predates, gets the closed-enum field default
  // DEP_JAVASCRIPT, and emits rel=preload; as=script for a module: the double
  // fetch the field split exists to prevent.
  rewrite_driver()->AddFilters();

  ValidateNoChanges("module_metadata_contract",
                    "<script type=module src=b.js></script>");

  OutputPartitions partitions;
  ASSERT_NO_FATAL_FAILURE(
      ReadCdfPartitions("http://test.com/b.js", &partitions));
  ASSERT_EQ(1, partitions.partition_size());
  const CachedResult& result = partitions.partition(0);
  EXPECT_EQ(0, result.collected_dependency_size())
      << "A module leaked into CachedResult.collected_dependency, where a "
         "binary predating DEP_MODULE would read it as DEP_JAVASCRIPT.";
  ASSERT_EQ(1, result.collected_module_dependency_size());
  EXPECT_EQ(DEP_MODULE, result.collected_module_dependency(0).content_type());
  EXPECT_EQ("http://test.com/b.js",
            result.collected_module_dependency(0).url());
}

TEST_F(CollectDependenciesFilterTest, HandleEmptyResources) {
  // We expect failures to still be hinted (since the browser will try to fetch
  // them), but we need to be careful not to crash. (We used to).

  // Using RememberFailure seems like the easiest way of getting empty
  // Resource objects.
  http_cache()->RememberFailure(StrCat(kTestDomain, "e.css"),
                                rewrite_driver()->CacheFragment(),
                                kFetchStatusOtherError, message_handler());

  http_cache()->RememberFailure(StrCat(kTestDomain, "f.js"),
                                rewrite_driver()->CacheFragment(),
                                kFetchStatusOtherError, message_handler());

  const char kInput[] =
      "<link rel=stylesheet href=e.css>"
      "<script src=f.js></script>";
  ValidateNoChanges("unoptimized", kInput);

  // Read stuff back in from pcache.
  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto("dependency {"
                          "url: 'http://test.com/e.css'"
                          "content_type: DEP_CSS "
                          "order_key: 0"
                          "}"
                          "dependency {"
                          "url: 'http://test.com/f.js'"
                          "content_type: DEP_JAVASCRIPT "
                          "order_key: 1"
                          "}"));

  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, Unoptimized) {
  const char kInput[] =
      "<link rel=stylesheet href=a.css>"
      "<script src=b.js></script>";
  ValidateNoChanges("unoptimized", kInput);

  // Read stuff back in from pcache.
  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/a.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "dependency {"
                                 "url: 'http://test.com/b.js'"
                                 "content_type: DEP_JAVASCRIPT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(200),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 1"
                                 "}")));

  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, Inliners) {
  // Currently we don't collect info on inline resources --- the filters
  // themsleves are expected to help --- but we should at least behave
  // sanely on them.
  options()->EnableFilter(RewriteOptions::kInlineCss);
  options()->EnableFilter(RewriteOptions::kInlineJavascript);
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kRewriteJavascriptInline);
  options()->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<link rel=stylesheet href=a.css>"
      "<script src=b.js></script>";

  const char kOutput[] =
      "<style>*{display:block}</style>"
      "<script>var b=42</script>";

  ValidateExpected("inliners", kInput, kOutput);

  // Read stuff back in from pcache.
  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  EXPECT_EQ(tracker->read_in_info(), nullptr);
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, Combiners) {
  // Because of implementation details of each, we notice output of CombineCss
  // but not of CombineJavascript (as it create a new <script> we don't see).
  // This is not thought to be a problem since we'll near certainly be turning
  // off Combine for H2 anyway.
  options()->EnableFilter(RewriteOptions::kCombineCss);
  options()->EnableFilter(RewriteOptions::kCombineJavascript);
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<link rel=stylesheet href=a.css>"
      "<link rel=stylesheet href=c.css>"
      "<script src=b.js></script>"
      "<script src=d.js></script>";
  const char kOutput[] =
      "<link rel=stylesheet href=a.css+c.css.pagespeed.cc.0.css>"
      "<script src=\"b.js+d.js.pagespeed.jc.0.js\"></script>"
      "<script>eval(mod_pagespeed_0);</script>"
      "<script>eval(mod_pagespeed_0);</script>";
  ValidateExpected("combiners", kInput, kOutput);

  // Read stuff back in from pcache.
  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(
      *tracker->read_in_info(),
      EqualsProto(StrCat("dependency {"
                         "url: 'http://test.com/a.css+c.css.pagespeed.cc.0.css'"
                         "content_type: DEP_CSS "
                         "validity_info {"
                         "type: CACHED "
                         "expiration_time_ms: ",
                         FormatRelTimeSec(100),
                         " "  // a.css
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "validity_info {"
                         "type: CACHED "
                         "expiration_time_ms: ",
                         FormatRelTimeSec(150),
                         " "  // c.css
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "validity_info {"
                         "type: CACHED "
                         "last_modified_time_ms: ",
                         FormatRelTimeSec(0), " ",  // a + c
                         "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
                         " "
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "order_key: 0"
                         "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, Chain) {
  // With lots of filters involved. (Not turning on combine JS here since
  // it would make us lose track of the JS. Ditto for inliners).
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
  options()->EnableFilter(RewriteOptions::kCombineCss);
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  options()->EnableFilter(RewriteOptions::kExtendCacheScripts);
  rewrite_driver()->AddFilters();

  const char kInput[] =
      "<link rel=stylesheet href=a.css>"
      "<link rel=stylesheet href=c.css>"
      "<script src=b.js></script>"
      "<script src=d.js></script>";
  const char kOutput[] =
      "<link rel=stylesheet href=A.a.css+c.css,Mcc.0.css.pagespeed.cf.0.css>"
      "<script src=b.js.pagespeed.jm.0.js></script>"
      "<script src=d.js.pagespeed.jm.0.js></script>";
  ValidateExpected("combiners", kInput, kOutput);

  // Read stuff back in from pcache.
  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(
      *tracker->read_in_info(),
      EqualsProto(StrCat(
          StrCat("dependency {"
                 "url: "
                 "'http://test.com/A.a.css+c.css,Mcc.0.css.pagespeed.cf.0.css'"
                 "content_type: DEP_CSS "
                 "validity_info {"
                 "type: CACHED "
                 "expiration_time_ms: ",
                 FormatRelTimeSec(100),
                 " "  // a.css
                 "date_ms: ",
                 FormatRelTimeSec(0),
                 "}"
                 "validity_info {"
                 "type: CACHED "
                 "expiration_time_ms: ",
                 FormatRelTimeSec(150),
                 " "  // c.css
                 "date_ms: ",
                 FormatRelTimeSec(0),
                 "}"
                 "validity_info {"
                 "type: CACHED "
                 "last_modified_time_ms: ",
                 FormatRelTimeSec(0), " ",  // a + c
                 "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
                 " "
                 "date_ms: ",
                 FormatRelTimeSec(0),
                 "}"
                 "validity_info {"
                 "type: CACHED "
                 "last_modified_time_ms: ",
                 FormatRelTimeSec(0), " ",  // (a + c).cf
                 "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
                 " "
                 "date_ms: ",
                 FormatRelTimeSec(0),
                 "}"
                 "order_key: 0"
                 "}"),
          "dependency {"
          "url: 'http://test.com/b.js.pagespeed.jm.0.js'"
          "content_type: DEP_JAVASCRIPT "
          "validity_info {"
          "type: CACHED "
          "expiration_time_ms: ",
          FormatRelTimeSec(200),
          " "  // b.js
          "date_ms: ",
          FormatRelTimeSec(0),
          "}"
          "validity_info {"
          "type: CACHED "
          "last_modified_time_ms: ",
          FormatRelTimeSec(0), " ",  // b.js.jm
          "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
          " "
          "date_ms: ",
          FormatRelTimeSec(0),
          "}"
          "order_key: 2"
          "}"
          "dependency {"
          "url: 'http://test.com/d.js.pagespeed.jm.0.js'"
          "content_type: DEP_JAVASCRIPT "
          "validity_info {"
          "type: CACHED "
          "expiration_time_ms: ",
          FormatRelTimeSec(250),
          " "  // d.js
          "date_ms: ",
          FormatRelTimeSec(0),
          "}"
          "validity_info {"
          "type: CACHED "
          "last_modified_time_ms: ",
          FormatRelTimeSec(0), " ",  // d.js.jm
          "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
          " "
          "date_ms: ",
          FormatRelTimeSec(0),
          "}"
          "order_key: 3"
          "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, Indirect) {
  // Test that we collect indirect dependencies.
  SetResponseWithDefaultHeaders("d.css", kContentTypeCss,
                                "@import \"i1.css\" all;\n"
                                "@import \"i2.css\" print, screen;\n"
                                "@import \"i3.css\" print;         ",
                                300);
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  rewrite_driver()->AddFilters();

  const char kInput[] = "<link rel=stylesheet href=d.css>";
  const char kOutput[] =
      "<link rel=stylesheet href=A.d.css.pagespeed.cf.0.css>";

  ValidateExpected("basic_res", kInput, kOutput);

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat(
                  StrCat("dependency {"
                         "url: 'http://test.com/A.d.css.pagespeed.cf.0.css'"
                         "content_type: DEP_CSS "
                         "validity_info {"
                         "type: CACHED "
                         "expiration_time_ms: ",
                         FormatRelTimeSec(300),
                         " "  // d.css
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "validity_info {"
                         "type: CACHED "
                         "last_modified_time_ms: ",
                         FormatRelTimeSec(0), " ",  // d.cf
                         "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
                         " "
                         "date_ms: ",
                         FormatRelTimeSec(0),
                         "}"
                         "order_key: 0"
                         "}"),
                  "dependency {"
                  "url: 'http://test.com/i1.css'"
                  "content_type: DEP_CSS "
                  "validity_info {"
                  "type: CACHED "
                  "expiration_time_ms: ",
                  FormatRelTimeSec(300),
                  " "  // d.css
                  "date_ms: ",
                  FormatRelTimeSec(0),
                  "}"
                  "validity_info {"
                  "type: CACHED "
                  "last_modified_time_ms: ",
                  FormatRelTimeSec(0), " ",  // d.cf
                  "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
                  " "
                  "date_ms: ",
                  FormatRelTimeSec(0),
                  "}"
                  "order_key: 0 "
                  "order_key: 1"
                  "}"
                  "dependency {"
                  "url: 'http://test.com/i2.css'"
                  "content_type: DEP_CSS "
                  "validity_info {"
                  "type: CACHED "
                  "expiration_time_ms: ",
                  FormatRelTimeSec(300),
                  " "  // d.css
                  "date_ms: ",
                  FormatRelTimeSec(0),
                  "}"
                  "validity_info {"
                  "type: CACHED "
                  "last_modified_time_ms: ",
                  FormatRelTimeSec(0), " ",  // d.cf
                  "expiration_time_ms: ", FormatRelTimeSec(kYearSec),
                  " "
                  "date_ms: ",
                  FormatRelTimeSec(0),
                  "}"
                  "order_key: 0 "
                  "order_key: 2"
                  "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, IndirectEmbeddedNul) {
  // Regression test: the CSS body is parsed over its full byte length, not up
  // to the first '\0'. Previously the filter constructed Css::Parser from a
  // StringPiece's .data() with the single-arg (strlen-based) ctor, which both
  // risked an out-of-bounds read past the un-terminated buffer and, on any
  // embedded NUL, silently truncated parsing -- dropping every @import after
  // the NUL from dependency collection. Here a NUL sits inside a comment
  // between two @imports; with the strlen bug the comment would never close
  // and i2.css would be lost. Both imports must be collected.
  GoogleString css_with_nul;
  StrAppend(&css_with_nul, "@import \"i1.css\" all;\n/* ");
  css_with_nul.push_back('\0');  // embedded NUL inside a CSS comment
  StrAppend(&css_with_nul, " */\n@import \"i2.css\" all;\n");
  SetResponseWithDefaultHeaders("d.css", kContentTypeCss, css_with_nul, 300);

  options()->EnableFilter(RewriteOptions::kRewriteCss);
  rewrite_driver()->AddFilters();

  const char kInput[] = "<link rel=stylesheet href=d.css>";
  const char kOutput[] =
      "<link rel=stylesheet href=A.d.css.pagespeed.cf.0.css>";
  ValidateExpected("embedded_nul", kInput, kOutput);

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);

  const Dependencies& deps = *tracker->read_in_info();
  bool found_i1 = false;
  bool found_i2 = false;
  for (int i = 0; i < deps.dependency_size(); ++i) {
    if (deps.dependency(i).url() == "http://test.com/i1.css") {
      found_i1 = true;
    }
    if (deps.dependency(i).url() == "http://test.com/i2.css") {
      found_i2 = true;
    }
  }
  EXPECT_TRUE(found_i1) << "@import before the embedded NUL was not collected";
  EXPECT_TRUE(found_i2)
      << "@import after the embedded NUL was dropped (strlen truncation)";

  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, CollectsWoff2FontFace) {
  // A woff2 @font-face in top-level CSS is collected as a DEP_FONT child:
  // absolute URL, the parent's validity_info, and an order key continuing
  // the parent's child sequence.
  SetResponseWithDefaultHeaders("f.css", kContentTypeCss,
                                "@font-face { font-family: Demo;"
                                " src: url(x.woff2) format(\"woff2\"); }"
                                " * { display: block }",
                                100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_basic", "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/x.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, PicksWoff2FromSrcFallbackList) {
  // Only the woff2 entry of a multi-format src list is collected, wherever
  // it sits in the list.
  SetResponseWithDefaultHeaders(
      "f.css", kContentTypeCss,
      "@font-face { font-family: Demo;"
      " src: local(\"Demo\"), url(x.woff) format(\"woff\"),"
      " url(x.woff2) format(\"woff2\"), url(x.ttf) format(\"truetype\"); }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_fallback_list", "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/x.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, SkipsFaceWithoutWoff2) {
  // A face offering no woff2 source is skipped entirely: guessing among
  // fallback formats is user-agent-dependent, and a wrong guess means a
  // double fetch.
  SetResponseWithDefaultHeaders(
      "f.css", kContentTypeCss,
      "@font-face { font-family: Demo;"
      " src: url(x.ttf) format(\"truetype\"), url(x.woff) format(\"woff\"); }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_no_woff2", "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, UnicodeRangeGatesHarvest) {
  // A face whose unicode-range does not include any printable ASCII
  // characters (U+0020 - U+007E) is a specialized subset and skipped; one
  // that does is harvested. Both faces lex cleanly here: the harvested
  // face uses the realistic latin-subset shape --- a list including the
  // letter-leading U+FEFF, which lexes as IDENT "U" + OPERATOR "+" + IDENT
  // "FEFF" --- and is reconstructed from the parsed values; the skipped
  // face's digit-leading range reconstructs and evaluates outside
  // printable ASCII (see DigitLeadingCleanParseRangeSkipped for the
  // dedicated pin).
  SetResponseWithDefaultHeaders(
      "f.css", kContentTypeCss,
      "@font-face { font-family: Arrows;"
      " src: url(arrows.woff2) format(\"woff2\");"
      " unicode-range: U+2190-21FF; }"
      "@font-face { font-family: Latin;"
      " src: url(latin.woff2) format(\"woff2\");"
      " unicode-range: U+0000-00FF, U+FEFF; }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_unicode_range", "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/latin.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, DigitLeadingCleanParseRangeSkipped) {
  // Digit-leading, wildcard-free unicode-range values (Google Fonts'
  // cyrillic/greek/vietnamese subset shape) lex CLEANLY as IDENT "U" +
  // NUMBER, arriving as an ordinary parsed declaration. The declaration is
  // reconstructed from the parsed values and evaluated: U+0400-045F lies
  // outside printable ASCII, so the face must be skipped --- a non-latin
  // subset face must never fill the font cap. (The companion test
  // UnicodeRangeDigitLeadingAsciiHarvested pins the ASCII-covering shapes.)
  SetResponseWithDefaultHeaders("f.css", kContentTypeCss,
                                "@font-face { font-family: Cyr;"
                                " src: url(cyr.woff2) format(\"woff2\");"
                                " unicode-range: U+0400-045F; }",
                                100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_digit_leading_range",
                    "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, UnicodeRangeWildcardAndEdgeForms) {
  // Wildcard and edge unicode-range forms. Wildcard values fail to lex and
  // demote to scannable UNPARSEABLE declarations:
  //  - U+00?? covers U+0000-00FF -> intersects ASCII -> harvested.
  //  - U+?? (no leading hex digits) covers U+00-FF -> harvested.
  //  - U+26?? covers U+2600-26FF -> no intersection -> skipped.
  // Whereas:
  //  - U+41 has only digits, so it parses cleanly; reconstruction yields
  //    U+41 == 'A', inside printable ASCII -> harvested.
  //  - "garbage" parses cleanly as an identifier; reconstruction yields
  //    "garbage", not a unicode-range token -> no coverage -> skipped.
  //  - U+GGGG lexes cleanly (IDENT "U" + OPERATOR "+" + IDENT "GGGG") and
  //    reconstructs to "U+GGGG", which is not a parseable token ->
  //    no coverage -> skipped.
  SetResponseWithDefaultHeaders(
      "f.css", kContentTypeCss,
      "@font-face { font-family: A;"
      " src: url(w0.woff2) format(\"woff2\"); unicode-range: U+00??; }"
      "@font-face { font-family: B;"
      " src: url(w1.woff2) format(\"woff2\"); unicode-range: U+??; }"
      "@font-face { font-family: C;"
      " src: url(w2.woff2) format(\"woff2\"); unicode-range: U+26??; }"
      "@font-face { font-family: D;"
      " src: url(w3.woff2) format(\"woff2\"); unicode-range: U+41; }"
      "@font-face { font-family: E;"
      " src: url(w4.woff2) format(\"woff2\"); unicode-range: garbage; }"
      "@font-face { font-family: F;"
      " src: url(w5.woff2) format(\"woff2\"); unicode-range: U+GGGG; }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_unicode_range_edge_forms",
                    "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/w0.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/w1.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 2"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/w3.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 3"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, UnicodeRangeLastDeclarationWins) {
  // Per CSS cascade, the last unicode-range declaration in a face wins:
  //  - Face A: a non-intersecting letter-leading range followed by an
  //    intersecting list -> harvested (would be skipped if the first won).
  //  - Face B: an intersecting letter-leading list followed by a
  //    digit-leading one -> the digit-leading declaration wins, is
  //    reconstructed, and evaluates outside printable ASCII -> skipped
  //    (pins the interaction between the two lexing shapes).
  SetResponseWithDefaultHeaders(
      "f.css", kContentTypeCss,
      "@font-face { font-family: A;"
      " src: url(a.woff2) format(\"woff2\");"
      " unicode-range: U+FEFF;"
      " unicode-range: U+0020-007E, U+FEFF; }"
      "@font-face { font-family: B;"
      " src: url(b.woff2) format(\"woff2\");"
      " unicode-range: U+0000-00FF, U+FEFF;"
      " unicode-range: U+0400-045F; }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_unicode_range_last_wins",
                    "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/a.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, UnicodeRangeDigitLeadingAsciiHarvested) {
  // Digit-leading, wildcard-free unicode-range tokens lex CLEANLY (IDENT "U"
  // + NUMBER) and arrive as ordinary parsed declarations; they must still be
  // evaluated, not blanket-treated as unevaluatable. The canonical
  // glyphhanger/pyftsubset subset shapes cover printable ASCII and must be
  // harvested:
  //  - U+0020-007E is exactly printable ASCII -> harvested.
  //  - U+0-7F covers it -> harvested.
  // Ranges that evaluate outside printable ASCII stay skipped:
  //  - U+4E00-9FFF (CJK-only) -> skipped.
  //  - U+0400-045F (cyrillic) -> skipped.
  // And a face with no unicode-range declaration at all is harvested.
  // Comma-separated lists without any letter-leading token also lex cleanly
  // (IDENT, NUMBER, COMMA, ...) and exercise the COMMA reconstruction path:
  //  - U+0020-007E, U+0100-017F: first token covers ASCII -> harvested.
  //  - U+0400-045F, U+0370-03FF: no token intersects ASCII -> skipped.
  SetResponseWithDefaultHeaders(
      "f.css", kContentTypeCss,
      "@font-face { font-family: Basic;"
      " src: url(basic.woff2) format(\"woff2\");"
      " unicode-range: U+0020-007E; }"
      "@font-face { font-family: Short;"
      " src: url(short.woff2) format(\"woff2\");"
      " unicode-range: U+0-7F; }"
      "@font-face { font-family: Cjk;"
      " src: url(cjk.woff2) format(\"woff2\");"
      " unicode-range: U+4E00-9FFF; }"
      "@font-face { font-family: Cyr;"
      " src: url(cyr.woff2) format(\"woff2\");"
      " unicode-range: U+0400-045F; }"
      "@font-face { font-family: NoRange;"
      " src: url(norange.woff2) format(\"woff2\"); }"
      "@font-face { font-family: Multi;"
      " src: url(multi.woff2) format(\"woff2\");"
      " unicode-range: U+0020-007E, U+0100-017F; }"
      "@font-face { font-family: MultiSkip;"
      " src: url(multiskip.woff2) format(\"woff2\");"
      " unicode-range: U+0400-045F, U+0370-03FF; }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_unicode_range_digit_leading",
                    "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/basic.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/short.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 2"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/norange.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 3"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/multi.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 4"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, SrcLastDeclarationWinsAndFormatTrusted) {
  // Per CSS cascade, the last src declaration in a face wins --- a.woff2
  // from the overridden first declaration must not be picked. And an
  // explicit format() hint is trusted in both directions: a .woff2 path
  // declared format("woff") is not a candidate, so face B yields nothing.
  SetResponseWithDefaultHeaders(
      "f.css", kContentTypeCss,
      "@font-face { font-family: A;"
      " src: url(a.woff2) format(\"woff2\");"
      " src: url(b.woff) format(\"woff\"), url(c.woff2) format(\"woff2\"); }"
      "@font-face { font-family: B;"
      " src: url(m.woff2) format(\"woff\"); }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_src_last_wins",
                    "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/c.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, MediaGatesFontHarvest) {
  // Same conservative media rule as for imports: print-only faces are
  // skipped, screen faces are harvested.
  SetResponseWithDefaultHeaders(
      "f.css", kContentTypeCss,
      "@media print { @font-face { font-family: P;"
      " src: url(p.woff2) format(\"woff2\"); } }"
      "@media screen { @font-face { font-family: S;"
      " src: url(s.woff2) format(\"woff2\"); } }",
      100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_media", "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "font_dependency {"
                                 "url: 'http://test.com/s.woff2'"
                                 "content_type: DEP_FONT "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, MalformedCssSkipsFonts) {
  // CSS the parser cannot fully make sense of (error mask set --- here via
  // an @-rule left unterminated at EOF) yields no font dependencies, while
  // the independent @import preamble scan is unaffected.
  SetResponseWithDefaultHeaders("f.css", kContentTypeCss,
                                "@import \"i1.css\";"
                                "@font-face { font-family: Demo;"
                                " src: url(x.woff2) format(\"woff2\"); }"
                                "@bogus-at-rule {",
                                100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_malformed", "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/f.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "dependency {"
                                 "url: 'http://test.com/i1.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, FontsNeverInOldSerializedFields) {
  // The downgrade contract: DEP_FONT entries must only ever appear in the
  // dedicated font fields. If one leaked into 'dependency', a binary
  // predating DEP_FONT would read its content_type as the closed-enum field
  // default (DEP_JAVASCRIPT) and emit as=script for a font.
  SetResponseWithDefaultHeaders("f.css", kContentTypeCss,
                                "@font-face { font-family: Demo;"
                                " src: url(x.woff2) format(\"woff2\"); }"
                                " * { display: block }",
                                100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_downgrade_contract",
                    "<link rel=stylesheet href=f.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  const Dependencies& deps = *tracker->read_in_info();
  for (int i = 0; i < deps.dependency_size(); ++i) {
    EXPECT_NE(DEP_FONT, deps.dependency(i).content_type())
        << "DEP_FONT leaked into the pre-DEP_FONT 'dependency' field: "
        << deps.dependency(i).url();
  }
  ASSERT_EQ(1, deps.font_dependency_size());
  EXPECT_EQ(DEP_FONT, deps.font_dependency(0).content_type());
  EXPECT_EQ("http://test.com/x.woff2", deps.font_dependency(0).url());
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, FontsNeverInOldMetadataCacheStore) {
  // Companion to FontsNeverInOldSerializedFields, pinning the OTHER store:
  // the cdf OutputPartitions entry in the metadata cache, which the
  // collector writes directly. The pcache test alone cannot catch a
  // collector regression here --- on replay the tracker re-routes entries
  // by content_type, which would silently repair a font mis-filed into
  // collected_dependency.
  SetResponseWithDefaultHeaders("f.css", kContentTypeCss,
                                "@font-face { font-family: Demo;"
                                " src: url(x.woff2) format(\"woff2\"); }"
                                " * { display: block }",
                                100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("font_metadata_contract",
                    "<link rel=stylesheet href=f.css>");

  // Reconstruct the cdf context's metadata cache key. This mirrors
  // RewriteContext::SetPartitionKey() for a single-slot context with the
  // default encoder, no UA-specific cache key, and no CacheKeySuffix; if
  // that key scheme changes, the ASSERT below fails loudly.
  GoogleString signature = server_context()->lock_hasher()->Hash(
      rewrite_driver()->options()->signature());
  GoogleString encoding;
  UrlSegmentEncoder encoder;
  StringVector dummy_url_keys;
  dummy_url_keys.push_back("");
  encoder.Encode(dummy_url_keys, nullptr, &encoding);
  GoogleString suffix = StrCat(encoding, "@", "_");
  GoogleString partition_key =
      StrCat(ServerContext::kCacheKeyResourceNamePrefix, "cdf", "_", signature,
             "/", "http://test.com/f.css", "@", suffix);

  CacheInterface::SynchronousCallback callback;
  lru_cache()->Get(partition_key, &callback);
  ASSERT_TRUE(callback.called());
  ASSERT_EQ(CacheInterface::kAvailable, callback.state())
      << "No cdf metadata entry at the reconstructed key " << partition_key
      << "; RewriteContext::SetPartitionKey() may have changed.";

  StringPiece raw = callback.value_as_string_piece();
  OutputPartitions partitions;
  ASSERT_TRUE(partitions.ParseFromArray(raw.data(), raw.size()));
  ASSERT_EQ(1, partitions.partition_size());
  const CachedResult& result = partitions.partition(0);
  ASSERT_EQ(1, result.collected_dependency_size());
  for (int i = 0; i < result.collected_dependency_size(); ++i) {
    EXPECT_NE(DEP_FONT, result.collected_dependency(i).content_type())
        << "DEP_FONT leaked into CachedResult.collected_dependency: "
        << result.collected_dependency(i).url();
  }
  EXPECT_EQ(DEP_CSS, result.collected_dependency(0).content_type());
  ASSERT_EQ(1, result.collected_font_dependency_size());
  EXPECT_EQ(DEP_FONT, result.collected_font_dependency(0).content_type());
  EXPECT_EQ("http://test.com/x.woff2",
            result.collected_font_dependency(0).url());
}

TEST_F(CollectDependenciesFilterTest, ImportTargetFontsNotCollected) {
  // Documented limitation: the collector records @import URLs but never
  // fetches the import bodies, so @font-face rules inside an import target
  // are invisible to the font harvest. (With import flattening on, the
  // flattened top-level bytes ARE parsed --- see
  // ImportFlatteningExposesFonts.)
  SetResponseWithDefaultHeaders("i1.css", kContentTypeCss,
                                "@font-face { font-family: Demo;"
                                " src: url(x.woff2) format(\"woff2\"); }"
                                " * { color: red }",
                                100);
  SetResponseWithDefaultHeaders("outer.css", kContentTypeCss,
                                "@import \"i1.css\";", 100);
  rewrite_driver()->AddFilters();

  ValidateNoChanges("import_target_fonts",
                    "<link rel=stylesheet href=outer.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  EXPECT_THAT(*tracker->read_in_info(),
              EqualsProto(StrCat("dependency {"
                                 "url: 'http://test.com/outer.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0"
                                 "}"
                                 "dependency {"
                                 "url: 'http://test.com/i1.css'"
                                 "content_type: DEP_CSS "
                                 "validity_info {"
                                 "type: CACHED "
                                 "expiration_time_ms: ",
                                 FormatRelTimeSec(100),
                                 " "
                                 "date_ms: ",
                                 FormatRelTimeSec(0),
                                 "}"
                                 "order_key: 0 "
                                 "order_key: 1"
                                 "}")));
  rewrite_driver()->FinishParse();
}

TEST_F(CollectDependenciesFilterTest, ImportFlatteningExposesFonts) {
  // The flattening variant of the limitation above: with import flattening
  // the collector parses the flattened top-level bytes, so fonts declared in
  // former import targets are harvested implicitly.
  SetResponseWithDefaultHeaders("i1.css", kContentTypeCss,
                                "@font-face { font-family: Demo;"
                                " src: url(x.woff2) format(\"woff2\"); }"
                                " * { color: red }",
                                100);
  SetResponseWithDefaultHeaders("outer.css", kContentTypeCss,
                                "@import \"i1.css\";", 100);
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kFlattenCssImports);
  rewrite_driver()->AddFilters();

  ValidateExpected("import_flatten_fonts",
                   "<link rel=stylesheet href=outer.css>",
                   "<link rel=stylesheet href=A.outer.css.pagespeed.cf.0.css>");

  ResetDriver();
  DependencyTracker* tracker = rewrite_driver()->dependency_tracker();
  rewrite_driver()->StartParse(kTestDomain);
  ASSERT_TRUE(tracker->read_in_info() != nullptr);
  const Dependencies& deps = *tracker->read_in_info();
  // Flattening removed the @import, so there is no CSS child --- just the
  // rewritten top-level stylesheet.
  ASSERT_EQ(1, deps.dependency_size());
  EXPECT_EQ("http://test.com/A.outer.css.pagespeed.cf.0.css",
            deps.dependency(0).url());
  ASSERT_EQ(1, deps.font_dependency_size());
  EXPECT_EQ(DEP_FONT, deps.font_dependency(0).content_type());
  EXPECT_EQ("http://test.com/x.woff2", deps.font_dependency(0).url());
  // The font is the sole child of the sole top-level dependency, so its
  // order key continues the child sequence right after the parent: (0, 1).
  ASSERT_EQ(2, deps.font_dependency(0).order_key_size());
  EXPECT_EQ(0, deps.font_dependency(0).order_key(0));
  EXPECT_EQ(1, deps.font_dependency(0).order_key(1));
  // The font carries its parent's validity info.
  EXPECT_EQ(deps.dependency(0).validity_info_size(),
            deps.font_dependency(0).validity_info_size());
  rewrite_driver()->FinishParse();
}

}  // namespace

}  // namespace net_instaweb
