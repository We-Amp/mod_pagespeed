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

#include "net/instaweb/rewriter/public/in_place_rewrite_context.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "net/instaweb/http/public/cache_url_async_fetcher.h"
#include "net/instaweb/http/public/counting_url_async_fetcher.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/wait_url_async_fetcher.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/fake_filter.h"
#include "net/instaweb/rewriter/public/file_load_policy.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/request_properties.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/hasher.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/time_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/image_types.pb.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/http/semantic_type.h"
#include "pagespeed/kernel/http/user_agent_matcher.h"
#include "pagespeed/kernel/thread/scheduler.h"
#include "pagespeed/kernel/thread/scheduler_sequence.h"
#include "test/net/instaweb/http/mock_url_fetcher.h"
#include "test/net/instaweb/rewriter/notifying_fetch.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"
#include "test/pagespeed/kernel/thread/mock_scheduler.h"
#include "test/pagespeed/kernel/thread/worker_test_base.h"

namespace net_instaweb {

namespace {

class FakeImageFilter : public FakeFilter {
 public:
  class Context : public FakeFilter::Context {
   public:
    Context(FakeImageFilter* filter, RewriteDriver* driver,
            RewriteContext* parent, ResourceContext* resource_context)
        : FakeFilter::Context(filter, driver, parent, resource_context),
          filter_(filter) {}

    void DoRewriteSingle(const ResourcePtr input,
                         OutputResourcePtr output) override {
      CachedResult* cached = output->EnsureCachedResultCreated();
      cached->set_optimized_image_type(filter_->optimized_image_type());
      FakeFilter::Context::DoRewriteSingle(input, output);
    }

   private:
    FakeImageFilter* filter_;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
  };

  explicit FakeImageFilter(RewriteDriver* rewrite_driver)
      : FakeFilter(RewriteOptions::kImageCompressionId, rewrite_driver,
                   semantic_type::kImage),
        optimized_image_type_(IMAGE_WEBP) {}

  void set_optimized_image_type(ImageType type) {
    optimized_image_type_ = type;
  }
  ImageType optimized_image_type() const { return optimized_image_type_; }

  RewriteContext* MakeFakeContext(RewriteDriver* driver, RewriteContext* parent,
                                  ResourceContext* resource_context) override {
    return new FakeImageFilter::Context(this, driver, parent, resource_context);
  }

 private:
  ImageType optimized_image_type_;
  FakeImageFilter(const FakeImageFilter&) = delete;
  FakeImageFilter& operator=(const FakeImageFilter&) = delete;
};

class InPlaceRewriteContextTest : public RewriteTestBase {
 protected:
  static const bool kWriteToCache = true;
  static const bool kNoWriteToCache = false;
  static const bool kNoTransform = true;
  static const bool kTransform = false;
  InPlaceRewriteContextTest()
      : img_filter_(nullptr),
        other_img_filter_(nullptr),
        js_filter_(nullptr),
        css_filter_(nullptr),
        cache_html_url_("http://www.example.com/cacheable.html"),
        cache_jpg_url_("http://www.example.com/cacheable.jpg"),
        cache_jpg_no_extension_url_("http://www.example.com/cacheable_jpg"),
        cache_jpg_notransform_url_("http://www.example.com/notransform.jpg"),
        cache_jpg_vary_star_url_("http://www.example.com/vary_star.jpg"),
        cache_jpg_vary_ua_url_("http://www.example.com/vary_ua.jpg"),
        cache_jpg_vary_origin_url_("http://www.example.com/vary_origin.jpg"),
        cache_png_url_("http://www.example.com/cacheable.png"),
        cache_gif_url_("http://www.example.com/cacheable.gif"),
        cache_webp_url_("http://www.example.com/cacheable.webp"),
        cache_js_url_("http://www.example.com/cacheable.js"),
        cache_js_jpg_extension_url_("http://www.example.com/cacheable_js.jpg"),
        cache_css_url_("http://www.example.com/cacheable.css"),
        nocache_html_url_("http://www.example.com/nocacheable.html"),
        nocache_js_url_("http://www.example.com/nocacheable.js"),
        private_cache_js_url_("http://www.example.com/privatecacheable.js"),
        cache_js_no_max_age_url_("http://www.example.com/cacheablemod.js"),
        bad_url_("http://www.example.com/bad.url"),
        redirect_url_("http://www.example.com/redir.url"),
        rewritten_jpg_url_(
            "http://www.example.com/cacheable.jpg.pagespeed.ic.0.jpg"),
        rewritten_css_url_(
            "http://www.example.com/cacheable.css.pagespeed.cf.0.css"),
        json_js_type_url_("http://www.example.com/cacheable_js_type.json"),
        json_json_type_url_("http://www.example.com/cacheable_json_type.json"),
        json_json_type_synonym_url_(
            "http://www.example.com/cacheable_json_synonym_type.json"),
        cache_body_("good"),
        nocache_body_("bad"),
        bad_body_("ugly"),
        redirect_body_("Location: http://www.example.com/final.url"),
        ttl_ms_(Timer::kHourMs),
        etag_("W/\"PSA-aj-0\""),
        original_etag_("original_etag"),
        exceed_deadline_(false),
        optimize_for_browser_(false),
        oversized_stream_(nullptr),
        in_place_uncacheable_rewrites_(nullptr) {}

  virtual void Init() {
    SetTimeMs(start_time_ms());
    mock_url_fetcher()->set_fail_on_unexpected(false);

    const StringPiece kNoVary("");

    // Set fetcher result and headers.
    AddResponse(cache_html_url_, kContentTypeHtml, cache_body_, start_time_ms(),
                ttl_ms_, original_etag_, kNoVary, kNoWriteToCache, kTransform);
    AddResponse(cache_jpg_url_, kContentTypeJpeg, cache_body_, start_time_ms(),
                ttl_ms_, "", kNoVary, kNoWriteToCache, kTransform);
    AddResponse(cache_jpg_no_extension_url_, kContentTypeJpeg, cache_body_,
                start_time_ms(), ttl_ms_, "", kNoVary, kNoWriteToCache,
                kTransform);
    AddResponse(cache_jpg_notransform_url_, kContentTypeJpeg, cache_body_,
                start_time_ms(), ttl_ms_, "", kNoVary, kNoWriteToCache,
                kNoTransform);
    AddResponse(cache_jpg_vary_star_url_, kContentTypeJpeg, cache_body_,
                start_time_ms(), ttl_ms_, "", /* Vary: */ "*", kNoWriteToCache,
                kTransform);
    AddResponse(cache_jpg_vary_ua_url_, kContentTypeJpeg, cache_body_,
                start_time_ms(), ttl_ms_, "",
                /* Vary: */ "User-Agent", kNoWriteToCache, kTransform);
    AddResponse(cache_jpg_vary_origin_url_, kContentTypeJpeg, cache_body_,
                start_time_ms(), ttl_ms_, "", /* Vary: */ "Origin",
                kNoWriteToCache, kTransform);
    AddResponse(cache_png_url_, kContentTypePng, cache_body_, start_time_ms(),
                ttl_ms_, original_etag_, kNoVary, kWriteToCache, kTransform);
    AddResponse(cache_gif_url_, kContentTypeGif, cache_body_, start_time_ms(),
                ttl_ms_, original_etag_, kNoVary, kWriteToCache, kTransform);
    AddResponse(cache_webp_url_, kContentTypeWebp, cache_body_, start_time_ms(),
                ttl_ms_, original_etag_, kNoVary, kWriteToCache, kTransform);
    AddResponse(cache_js_url_, kContentTypeJavascript, cache_body_,
                start_time_ms(), ttl_ms_, "", kNoVary, kNoWriteToCache,
                kTransform);
    AddResponse(cache_js_jpg_extension_url_, kContentTypeJavascript,
                cache_body_, start_time_ms(), ttl_ms_, "", kNoVary,
                kNoWriteToCache, kTransform);
    AddResponse(cache_css_url_, kContentTypeCss, cache_body_, start_time_ms(),
                ttl_ms_, "", kNoVary, kNoWriteToCache, kTransform);
    AddResponse(nocache_html_url_, kContentTypeHtml, nocache_body_,
                start_time_ms(), -1, "", kNoVary, kNoWriteToCache, kTransform);
    AddResponse(nocache_js_url_, kContentTypeJavascript, cache_body_,
                start_time_ms(), -1 /*ttl*/, "" /*etag*/, kNoVary,
                kNoWriteToCache, kTransform);
    AddResponse(cache_js_no_max_age_url_, kContentTypeJavascript, cache_body_,
                start_time_ms(), 0, "", kNoVary, kNoWriteToCache, kTransform);
    AddResponseStrContentType(json_js_type_url_, "application/javascript",
                              cache_body_, start_time_ms(), ttl_ms_, "",
                              kNoVary, kNoWriteToCache, kTransform);
    AddResponseStrContentType(json_json_type_url_, "application/json",
                              cache_body_, start_time_ms(), ttl_ms_, "",
                              kNoVary, kNoWriteToCache, kTransform);
    AddResponseStrContentType(json_json_type_synonym_url_, "application/x-json",
                              cache_body_, start_time_ms(), ttl_ms_, "",
                              kNoVary, kNoWriteToCache, kTransform);

    ResponseHeaders private_headers;
    SetDefaultHeaders(kContentTypeJavascript.mime_type(), &private_headers);
    private_headers.SetDateAndCaching(start_time_ms(), 1200 /*ttl*/,
                                      ",private");
    mock_url_fetcher()->SetResponse(private_cache_js_url_, private_headers,
                                    cache_body_);

    ResponseHeaders bad_headers;
    bad_headers.set_first_line(1, 1, 404, "Not Found");
    bad_headers.SetDate(start_time_ms());
    mock_url_fetcher()->SetResponse(bad_url_, bad_headers, bad_body_);

    // Add a response for permanent redirect.
    ResponseHeaders redirect_headers;
    redirect_headers.set_first_line(1, 1, 301, "Moved Permanently");
    redirect_headers.ComputeCaching();
    redirect_headers.SetCacheControlMaxAge(36000);
    redirect_headers.Add(HttpAttributes::kCacheControl, "public");
    redirect_headers.Add(HttpAttributes::kContentType, "image/jpeg");
    mock_url_fetcher()->SetResponse(redirect_url_, redirect_headers,
                                    redirect_body_);

    img_filter_ = new FakeImageFilter(rewrite_driver());
    js_filter_ = MakeJsFilter();
    css_filter_ = new FakeFilter(RewriteOptions::kCssFilterId, rewrite_driver(),
                                 semantic_type::kStylesheet);

    rewrite_driver()->AppendRewriteFilter(img_filter_);
    rewrite_driver()->AppendRewriteFilter(js_filter_);
    rewrite_driver()->AppendRewriteFilter(css_filter_);
    options()->ClearSignatureForTesting();
    AddRecompressImageFilters();
    options()->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
    options()->EnableFilter(RewriteOptions::kRewriteJavascriptInline);
    options()->EnableFilter(RewriteOptions::kRewriteCss);
    if (optimize_for_browser()) {
      options()->EnableFilter(RewriteOptions::kInPlaceOptimizeForBrowser);
      options()->EnableFilter(RewriteOptions::kConvertJpegToWebp);
    }
    options()->set_in_place_rewriting_enabled(true);

    server_context()->ComputeSignature(options());
    // Clear stats since we may have added something to the cache.
    ClearStats();

    oversized_stream_ = statistics()->GetVariable(
        InPlaceRewriteContext::kInPlaceOversizedOptStream);
    in_place_uncacheable_rewrites_ = statistics()->GetVariable(
        InPlaceRewriteContext::kInPlaceUncacheableRewrites);
  }

  virtual FakeFilter* MakeJsFilter() {
    return new FakeFilter(RewriteOptions::kJavascriptMinId, rewrite_driver(),
                          semantic_type::kScript);
  }

  void AddResponseStrContentType(const GoogleString& url,
                                 const GoogleString& content_type,
                                 const GoogleString& body, int64 now_ms,
                                 int64 ttl_ms, const GoogleString& etag,
                                 const StringPiece& vary, bool write_to_cache,
                                 bool no_transform) {
    ResponseHeaders response_headers;
    SetDefaultHeaders(content_type, &response_headers);
    if (ttl_ms > 0) {
      response_headers.SetDateAndCaching(now_ms, ttl_ms);
    } else {
      response_headers.SetDate(now_ms);
      if (ttl_ms < 0) {
        response_headers.Replace(HttpAttributes::kCacheControl, "no-cache");
      } else {
        response_headers.Replace(HttpAttributes::kCacheControl, "public");
      }
    }
    if (!vary.empty()) {
      response_headers.Replace(HttpAttributes::kVary, vary);
    }
    if (no_transform) {
      response_headers.Replace(HttpAttributes::kCacheControl, "no-transform");
    }
    if (!etag.empty()) {
      response_headers.Add(HttpAttributes::kEtag, etag);
    }
    mock_url_fetcher()->SetResponse(url, response_headers, body);
    if (write_to_cache) {
      response_headers.ComputeCaching();
      http_cache()->Put(
          url, rewrite_driver_->CacheFragment(), RequestHeaders::Properties(),
          ResponseHeaders::GetVaryOption(options()->respect_vary()),
          &response_headers, body, message_handler());
    }
  }

  void AddResponse(const GoogleString& url, const ContentType& content_type,
                   const GoogleString& body, int64 now_ms, int64 ttl_ms,
                   const GoogleString& etag, const StringPiece& vary,
                   bool write_to_cache, bool no_transform) {
    AddResponseStrContentType(url, content_type.mime_type(), body, now_ms,
                              ttl_ms, etag, vary, write_to_cache, no_transform);
  }

  void SetDefaultHeaders(const GoogleString& content_type,
                         ResponseHeaders* header) const {
    header->set_major_version(1);
    header->set_minor_version(1);
    header->SetStatusAndReason(HttpStatus::kOK);
    header->Replace(HttpAttributes::kContentType, content_type);
  }

  void ResetUserAgent(StringPiece user_agent) {
    ClearRewriteDriver();
    SetCurrentUserAgent(user_agent);
  }

  void SetAcceptWebp() {
    AddRequestAttribute(HttpAttributes::kAccept, "image/webp");
  }

  void FetchAndCheckResponse(const GoogleString& url,
                             const GoogleString& expected_body,
                             bool expected_success, int64 expected_ttl,
                             const char* etag, int64 date_ms) {
    js_filter_->set_exceed_deadline(exceed_deadline_);
    img_filter_->set_exceed_deadline(exceed_deadline_);
    if (other_img_filter_ != nullptr) {
      other_img_filter_->set_exceed_deadline(exceed_deadline_);
    }
    css_filter_->set_exceed_deadline(exceed_deadline_);

    WorkerTestBase::SyncPoint sync(server_context()->thread_system());
    RequestContextPtr request_context(RequestContext::NewTestRequestContext(
        server_context()->thread_system()));
    NotifyingFetch notifying_fetch(request_context, options(), url, &sync,
                                   &response_headers_);
    const RequestHeaders* driver_request_headers =
        rewrite_driver()->request_headers();
    if (driver_request_headers != nullptr) {
      notifying_fetch.request_headers()->CopyFrom(*driver_request_headers);
    }
    rewrite_driver()->FetchResource(url, &notifying_fetch);

    // If we're testing if the rewrite takes too long, we need to push
    // time forward here.
    if (exceed_deadline()) {
      rewrite_driver()->BoundedWaitFor(RewriteDriver::kWaitForCompletion,
                                       rewrite_driver()->rewrite_deadline_ms());
    }

    sync.Wait();
    rewrite_driver()->WaitForShutDown();
    mock_scheduler()->AwaitQuiescence();  // needed for cache puts to finish.
    EXPECT_TRUE(notifying_fetch.done());
    EXPECT_EQ(expected_success, notifying_fetch.success()) << url;
    EXPECT_EQ(expected_body, notifying_fetch.content()) << url;
    EXPECT_EQ(expected_ttl, response_headers_.cache_ttl_ms()) << url;
    EXPECT_STREQ(etag, response_headers_.Lookup1(HttpAttributes::kEtag)) << url;
    EXPECT_EQ(date_ms, response_headers_.date_ms()) << url;
  }

  // Fetches `url`, whose origin fails after part of the body, and checks
  // that the client is answered with a failure and the driver is released
  // (WaitForShutDown returns).
  void FetchFailingMidStream(const GoogleString& url) {
    WorkerTestBase::SyncPoint sync(server_context()->thread_system());
    RequestContextPtr request_context(RequestContext::NewTestRequestContext(
        server_context()->thread_system()));
    NotifyingFetch notifying_fetch(request_context, options(), url, &sync,
                                   &response_headers_);
    rewrite_driver()->FetchResource(url, &notifying_fetch);
    sync.Wait();
    rewrite_driver()->WaitForShutDown();
    mock_scheduler()->AwaitQuiescence();
    EXPECT_TRUE(notifying_fetch.done());
    EXPECT_FALSE(notifying_fetch.success());
    EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  }

  void ResetHeadersAndStats() {
    response_headers_.Clear();
    img_filter_->ClearStats();
    if (other_img_filter_ != nullptr) {
      other_img_filter_->ClearStats();
    }
    js_filter_->ClearStats();
    css_filter_->ClearStats();
    RewriteTestBase::ClearStats();
    ClearRewriteDriver();
  }

  void CheckWarmCache(StringPiece id) {
    EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count()) << id;
    EXPECT_EQ(1, http_cache()->cache_hits()->Get()) << id;
    EXPECT_EQ(0, http_cache()->cache_misses()->Get()) << id;
    EXPECT_EQ(0, http_cache()->cache_inserts()->Get()) << id;
    EXPECT_EQ(2, lru_cache()->num_hits()) << id;
    EXPECT_EQ(0, lru_cache()->num_misses()) << id;
    EXPECT_EQ(0, lru_cache()->num_inserts()) << id;
    EXPECT_EQ(0, img_filter_->num_rewrites()) << id;
    EXPECT_EQ(0, js_filter_->num_rewrites()) << id;
    EXPECT_EQ(0, css_filter_->num_rewrites()) << id;
    EXPECT_EQ(0, oversized_stream_->Get()) << id;
  }

  void ExpectInPlaceImageSuccessFlow(const GoogleString& url) {
    // The first serve streams the not-yet-optimized bytes with the
    // injected s-maxage window.
    FetchAndCheckResponse(url, cache_body_, true, kInjectedSMaxAgeMs,
                          original_etag_, start_time_ms());

    // First fetch misses initial metadata cache lookup, finds original in
    // cache; the resource gets rewritten and the rewritten
    // resource gets inserted into cache.
    // The module's own anonymous fetch before the optimization is built.
    EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
    EXPECT_EQ(2, http_cache()->cache_hits()->Get());
    EXPECT_EQ(1, http_cache()->cache_misses()->Get());
    EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
    EXPECT_EQ(2, lru_cache()->num_hits());
    EXPECT_EQ(3, lru_cache()->num_misses());
    EXPECT_EQ(4, lru_cache()->num_inserts());
    EXPECT_EQ(1, img_filter_->num_rewrites());
    EXPECT_EQ(0, js_filter_->num_rewrites());
    EXPECT_EQ(0, css_filter_->num_rewrites());

    ResetHeadersAndStats();
    SetTimeMs(start_time_ms() + ttl_ms_ / 2);
    FetchAndCheckResponse(url, "good:ic", true, ttl_ms_ / 2, etag_,
                          start_time_ms() + ttl_ms_ / 2);
    // Second fetch hits the metadata cache and the rewritten resource is
    // served out.
    CheckWarmCache("second_fetch_1");

    AdvanceTimeMs(2 * ttl_ms_);
    ResetHeadersAndStats();
    // Serves the original bytes again, with the injected window.
    FetchAndCheckResponse(url, cache_body_, true, kInjectedSMaxAgeMs,
                          original_etag_, timer()->NowMs());
    // The metadata and cache entry is stale now. Fetch the content and serve
    // out the original. The background rewrite work then revalidates
    // the response and updates metadata.
    // Rebuilding after the input expired starts from the module's own fetch.
    EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
    EXPECT_EQ(1, http_cache()->cache_hits()->Get());
    EXPECT_EQ(2, http_cache()->cache_misses()->Get());
    EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
    EXPECT_EQ(4, lru_cache()->num_hits());  // (expired) orig., aj, ic metadata
    EXPECT_EQ(1, lru_cache()->num_misses());
    EXPECT_EQ(4, lru_cache()->num_inserts());
    EXPECT_EQ(0, img_filter_->num_rewrites());
    EXPECT_EQ(0, js_filter_->num_rewrites());
    EXPECT_EQ(0, css_filter_->num_rewrites());
  }

  void CheckCachingAndContentType(const GoogleString& url,
                                  const GoogleString& expected_mime_type,
                                  const GoogleString& cache_body,
                                  const GoogleString& filter_prefix) {
    // First serve streams the not-yet-optimized bytes with the injected
    // s-maxage window.
    FetchAndCheckResponse(url, cache_body, true, kInjectedSMaxAgeMs, nullptr,
                          start_time_ms());
    // The module's own anonymous fetch before the optimization is built.
    EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
    EXPECT_EQ(1, http_cache()->cache_hits()->Get());
    EXPECT_EQ(2, http_cache()->cache_misses()->Get());
    EXPECT_EQ(3, http_cache()->cache_inserts()->Get());
    EXPECT_EQ(1, lru_cache()->num_hits());
    EXPECT_EQ(4, lru_cache()->num_misses());
    EXPECT_EQ(5, lru_cache()->num_inserts());
    EXPECT_EQ(0, img_filter_->num_rewrites());
    EXPECT_EQ(1, js_filter_->num_rewrites());
    EXPECT_EQ(0, css_filter_->num_rewrites());

    // Make sure the content type is unmodified.
    EXPECT_STREQ(expected_mime_type,
                 response_headers_.Lookup1(HttpAttributes::kContentType));

    // Try a second fetch and ensure we get a cache hit.
    ResetHeadersAndStats();
    FetchAndCheckResponse(url, StrCat(cache_body, ":", filter_prefix), true,
                          ttl_ms_, etag_, start_time_ms());
    EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
    EXPECT_EQ(1, http_cache()->cache_hits()->Get());
    EXPECT_EQ(0, http_cache()->cache_misses()->Get());
    EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
    EXPECT_EQ(2, lru_cache()->num_hits());
    EXPECT_EQ(0, lru_cache()->num_misses());
    EXPECT_EQ(0, lru_cache()->num_inserts());
    EXPECT_EQ(0, img_filter_->num_rewrites());
    EXPECT_EQ(0, js_filter_->num_rewrites());
    EXPECT_EQ(0, css_filter_->num_rewrites());
    EXPECT_STREQ(expected_mime_type,
                 response_headers_.Lookup1(HttpAttributes::kContentType));
  }

  bool exceed_deadline() { return exceed_deadline_; }
  void set_exceed_deadline(bool x) { exceed_deadline_ = x; }

  bool optimize_for_browser() { return optimize_for_browser_; }
  void set_optimize_for_browser(bool x) { optimize_for_browser_ = x; }

  FakeImageFilter* img_filter_;
  FakeImageFilter* other_img_filter_;
  FakeFilter* js_filter_;
  FakeFilter* css_filter_;

  ResponseHeaders response_headers_;

  const GoogleString cache_html_url_;
  const GoogleString cache_jpg_url_;
  const GoogleString cache_jpg_no_extension_url_;
  const GoogleString cache_jpg_notransform_url_;
  const GoogleString cache_jpg_vary_star_url_;
  const GoogleString cache_jpg_vary_ua_url_;
  const GoogleString cache_jpg_vary_origin_url_;
  const GoogleString cache_png_url_;
  const GoogleString cache_gif_url_;
  const GoogleString cache_webp_url_;
  const GoogleString cache_js_url_;
  const GoogleString cache_js_jpg_extension_url_;
  const GoogleString cache_css_url_;
  const GoogleString nocache_html_url_;
  const GoogleString nocache_js_url_;
  const GoogleString private_cache_js_url_;
  const GoogleString cache_js_no_max_age_url_;
  const GoogleString bad_url_;
  const GoogleString redirect_url_;
  const GoogleString rewritten_jpg_url_;
  const GoogleString rewritten_css_url_;
  const GoogleString json_js_type_url_;
  const GoogleString json_json_type_url_;
  const GoogleString json_json_type_synonym_url_;

  const GoogleString cache_body_;
  const GoogleString nocache_body_;
  const GoogleString bad_body_;
  const GoogleString redirect_body_;

  const int ttl_ms_;
  // The in-place flow injects s-maxage=10 (InPlaceSMaxAgeSec's default)
  // on serves of not-yet-optimized bytes, and the served TTL is clamped
  // to it: the injected window may only shorten the origin's TTL.
  static const int64 kInjectedSMaxAgeMs = 10 * Timer::kSecondMs;
  const char* etag_;
  const char* original_etag_;
  bool exceed_deadline_;
  bool optimize_for_browser_;

  Variable* oversized_stream_;
  Variable* in_place_uncacheable_rewrites_;
};

TEST_F(InPlaceRewriteContextTest, CacheableHtmlUrlNoRewriting) {
  // All these entries find no in-place rewrite metadata and no rewriting
  // happens.
  Init();
  FetchAndCheckResponse(cache_html_url_, cache_body_, true, ttl_ms_,
                        original_etag_, start_time_ms());
  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());  // metadata + html
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_html_url_, cache_body_, true, ttl_ms_,
                        original_etag_, start_time_ms());
  // Second fetch hits initial cache lookup and no extra fetches are needed.
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());  // metadata
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  AdvanceTimeMs(2 * ttl_ms_);
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_html_url_, cache_body_, true, ttl_ms_,
                        original_etag_, start_time_ms() + 2 * ttl_ms_);
  // Cache entry is stale, so we must fetch again.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());  // HTML is in LRU cache, just expired.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
}

TEST_F(InPlaceRewriteContextTest, WaitForOptimizedFirstRequest) {
  // By setting this flag we should get an optimized response on the first
  // request unless we hit a rewrite timeout but in this test it will complete
  // in time.
  options()->set_in_place_wait_for_optimized(true);
  Init();

  // The optimized content from the fake rewriter has ":ic" appended to original
  // content.
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());

  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache. Also, the resource gets rewritten and the rewritten
  // resource gets inserted into cache. The optimized version should be
  // returned.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(3, http_cache()->cache_inserts()->Get());  // rewritten + original
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(5, lru_cache()->num_inserts());
  EXPECT_EQ(1, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
  EXPECT_EQ(0, oversized_stream_->Get());

  ResetHeadersAndStats();
  SetTimeMs((start_time_ms() + ttl_ms_ / 2));
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  // Second fetch hits the metadata cache and the rewritten resource is
  // served out.
  CheckWarmCache("second_fetch_2");
}

// When proxying, a resource whose response varies on the client's request
// beyond Accept-Encoding is passed through as the origin sent it for this
// client: not stored, not rewritten, and not loaded again by the module,
// whose copy would otherwise be served to every client in its place.
TEST_F(InPlaceRewriteContextTest, ProxiedAnswerVaryingOnRequestPassesThrough) {
  Init();
  for (const GoogleString* url :
       {&cache_jpg_vary_origin_url_, &cache_jpg_vary_ua_url_}) {
    SCOPED_TRACE(*url);
    for (int i = 0; i < 2; ++i) {
      ResetHeadersAndStats();
      FetchAndCheckResponse(*url, cache_body_, true, ttl_ms_, nullptr,
                            start_time_ms());
      EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count()) << i;
      EXPECT_EQ(0, http_cache()->cache_hits()->Get()) << i;
      EXPECT_EQ(0, http_cache()->cache_inserts()->Get()) << i;
      EXPECT_EQ(0, img_filter_->num_rewrites()) << i;
    }
  }
}

// A copy the module stored itself is not served by the proxying lookup either
// when it varies on the request: the origin is asked for this client.
TEST_F(InPlaceRewriteContextTest,
       ProxiedLookupSkipsStoredCopyVaryingOnRequest) {
  Init();
  ResponseHeaders stored;
  SetDefaultHeaders(kContentTypeJpeg.mime_type(), &stored);
  stored.SetDateAndCaching(start_time_ms(), ttl_ms_);
  stored.Replace(HttpAttributes::kVary, "Origin");
  stored.ComputeCaching();
  HTTPValue value;
  value.SetHeaders(&stored);
  value.Write("module's copy", message_handler());
  http_cache()->PutFetchedInput(
      cache_jpg_vary_origin_url_, rewrite_driver_->CacheFragment(),
      RequestHeaders::Properties(), kDefaultHttpOptionsForTests, &value,
      message_handler());
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_jpg_vary_origin_url_, cache_body_, true, ttl_ms_,
                        nullptr, start_time_ms());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, img_filter_->num_rewrites());
}

// A resource URL that does not decode as an optimized one is fetched in
// place, as a proxy, with the visitor's own request.  When that request
// carried a cookie, the answer is not stored unless the origin marks it
// shareable.
TEST_F(InPlaceRewriteContextTest,
       UndecodableResourceFetchedWithCookieIsNotShared) {
  Init();
  const GoogleString url = "http://www.example.com/data.txt.pagespeed.zz.0.txt";
  AddResponse(url, kContentTypeText, cache_body_, start_time_ms(), ttl_ms_, "",
              "", kNoWriteToCache, kTransform);
  for (bool with_cookie : {true, false}) {
    SCOPED_TRACE(with_cookie ? "cookie" : "no cookie");
    lru_cache()->Clear();
    ResetHeadersAndStats();
    if (with_cookie) {
      AddRequestAttribute(HttpAttributes::kCookie, "session=1");
    }
    SetDriverRequestHeaders();
    FetchAndCheckResponse(url, cache_body_, true, ttl_ms_, nullptr,
                          start_time_ms());
    EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
    EXPECT_EQ(with_cookie ? 0 : 1, http_cache()->cache_inserts()->Get());
  }
}

TEST_F(InPlaceRewriteContextTest, WaitForOptimizeWithDisabledFilter) {
  // Wait for optimized but if the resource fails to optimize we should get
  // back the original resource.
  options()->set_in_place_wait_for_optimized(true);
  // We'll also test that the hash values we get are legitimate and not
  // hard-coded 0s
  UseMd5Hasher();

  Init();

  // Turn off optimization. The filter will still run but return false in
  // rewrite.
  img_filter_->set_enabled(false);
  FetchAndCheckResponse(cache_jpg_url_, cache_body_, true, ttl_ms_, nullptr,
                        start_time_ms());

  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache. Failure to rewrite means original should be returned.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());  // original only
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(4, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
  EXPECT_EQ(0, oversized_stream_->Get());

  ResetHeadersAndStats();
  // The second time we get the cached original, which should have an md5'd
  // etag.

  // TODO(jkarlin): Note that if we advance time here, we'd expect the TTL of
  // the cached resource to decrease on the second fetch, but that doesn't
  // happen. That should be fixed.
  GoogleString expected_etag =
      StrCat("W/\"PSA-", hasher()->Hash(cache_body_), "\"");
  FetchAndCheckResponse(cache_jpg_url_, cache_body_, true, ttl_ms_,
                        expected_etag.c_str(), start_time_ms());
  // Second fetch hits the metadata cache, sees that the rewrite failed and
  // fetches and serves the original resource from cache.
  CheckWarmCache("second_fetch_3");
}

TEST_F(InPlaceRewriteContextTest, WaitForOptimizeNoTransform) {
  // Confirm that when cache-control:no-transform is present in the response
  // headers that the in-place optimizer does not optimize the resource.
  options()->set_in_place_wait_for_optimized(true);
  Init();

  // Don't rewrite since it's no-transform.
  FetchAndCheckResponse(cache_jpg_notransform_url_, cache_body_, true, ttl_ms_,
                        nullptr, start_time_ms());
  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // into cache.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());  // original + ipro metadata
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  EXPECT_TRUE(response_headers_.HasValue(HttpAttributes::kCacheControl,
                                         "no-transform"));

  ResetHeadersAndStats();

  // Don't rewrite since it's no-transform.
  FetchAndCheckResponse(cache_jpg_notransform_url_, cache_body_, true, ttl_ms_,
                        kEtag0.c_str(), start_time_ms());
  // The second fetch should return the cached original after seeing that it
  // can't be rewritten.
  CheckWarmCache("second_fetch_4");
}

TEST_F(InPlaceRewriteContextTest, OptimizeOnNoTransformIfOptionFalse) {
  options()->set_disable_rewrite_on_no_transform(false);
  Init();
  FetchAndCheckResponse(cache_jpg_notransform_url_, cache_body_, true, ttl_ms_,
                        nullptr, start_time_ms());
  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // into cache. Also the resource gets rewritten and the rewritten resource
  // gets inserted into cache.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(3, http_cache()->cache_inserts()->Get());  // rewritten + original
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(4, lru_cache()->num_misses());
  EXPECT_EQ(5, lru_cache()->num_inserts());
  EXPECT_EQ(1, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  ResetHeadersAndStats();
  SetTimeMs((start_time_ms() + ttl_ms_ / 2));
  FetchAndCheckResponse(cache_jpg_notransform_url_, "good:ic", true,
                        ttl_ms_ / 2, etag_, start_time_ms() + ttl_ms_ / 2);
  // Second fetch hits the metadata cache and the rewritten resource is served
  // out.
  CheckWarmCache("second_fetch_notransform");
}

TEST_F(InPlaceRewriteContextTest, WaitForOptimizeTimeout) {
  // Confirm that rewrite deadlines cause the original resource to be returned
  // (but caches the optimized) even if in_place_wait_for_optimize is on.
  options()->set_in_place_wait_for_optimized(true);
  Init();

  // Tells the optimizing filter to slow down.
  exceed_deadline_ = true;

  FetchAndCheckResponse(cache_jpg_url_, cache_body_, true, ttl_ms_, nullptr,
                        start_time_ms());
  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache. Rewrite succeeds but is slow so original returned.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(3, http_cache()->cache_inserts()->Get());  // rewritten + original
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(5, lru_cache()->num_inserts());
  EXPECT_EQ(1, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
  EXPECT_EQ(0, oversized_stream_->Get());

  ResetHeadersAndStats();
  SetTimeMs((start_time_ms() + ttl_ms_ / 2));

  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  // Second fetch hits the metadata cache and the rewritten resource is served
  // out.
  CheckWarmCache("second_fetch_5");
}

TEST_F(InPlaceRewriteContextTest, WaitForOptimizeResourceTooBig) {
  // Wait for optimized but if it's larger than the RecordingFetch can handle
  // make sure we piece together the original resource properly.
  options()->set_in_place_wait_for_optimized(true);

  Init();

  // To make this more interesting there should be something in the cache to
  // recover when we fail.  Let's split the url_fetch from 'good' into 'go' and
  // 'od' writes.
  mock_url_fetcher()->set_split_writes(true);

  // By setting cache max to 2, the second write ('od') will cause an overflow.
  // Test that we recover.
  http_cache()->set_max_cacheable_response_content_length(2);

  FetchAndCheckResponse(cache_jpg_url_, cache_body_, true, ttl_ms_, nullptr,
                        start_time_ms());

  // First fetch misses initial cache lookup, succeeds at fetch but resource too
  // big for cache.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());  // rewritten + original
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
  EXPECT_EQ(1, oversized_stream_->Get());

  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_jpg_url_, cache_body_, true, ttl_ms_, nullptr,
                        start_time_ms());
  // Second fetch should also completely miss because the first fetch was too
  // big to stuff in the cache.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());  // rewritten + original
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
  EXPECT_EQ(1, oversized_stream_->Get());
}

TEST_F(InPlaceRewriteContextTest,
       WaitForOptimizeOversizedNoShortContentLength) {
  // When a buffered response crosses the cache size limit mid-stream, the
  // fetch falls back to passing the original body through.  It must not
  // declare a Content-Length shorter than the body it actually delivers.
  // With the cache limit below the first chunk's size, the fallback happens
  // on the first of two split writes, so more body bytes arrive after the
  // headers are sent.
  options()->set_in_place_wait_for_optimized(true);
  Init();
  mock_url_fetcher()->set_split_writes(true);
  http_cache()->set_max_cacheable_response_content_length(1);

  WorkerTestBase::SyncPoint sync(server_context()->thread_system());
  RequestContextPtr request_context(
      RequestContext::NewTestRequestContext(server_context()->thread_system()));
  NotifyingFetch notifying_fetch(request_context, options(), cache_jpg_url_,
                                 &sync, &response_headers_);
  rewrite_driver()->FetchResource(cache_jpg_url_, &notifying_fetch);
  sync.Wait();
  rewrite_driver()->WaitForShutDown();
  mock_scheduler()->AwaitQuiescence();

  EXPECT_TRUE(notifying_fetch.done());
  EXPECT_TRUE(notifying_fetch.success());
  // The client receives the complete, unmodified body, and no Content-Length
  // shorter than the body is declared.
  EXPECT_EQ(cache_body_, notifying_fetch.content());
  EXPECT_FALSE(notifying_fetch.content_length_known());
  EXPECT_EQ(1, oversized_stream_->Get());
}

TEST_F(InPlaceRewriteContextTest, CacheableJpgUrlRewritingSucceeds) {
  Init();
  // First serve streams the not-yet-optimized bytes.
  FetchAndCheckResponse(cache_jpg_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, start_time_ms());

  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache. Also, the resource gets rewritten and the rewritten
  // resource gets inserted into cache.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(3, http_cache()->cache_inserts()->Get());  // rewritten + original
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(4, lru_cache()->num_misses());
  EXPECT_EQ(5, lru_cache()->num_inserts());
  EXPECT_EQ(1, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  ResetHeadersAndStats();
  SetTimeMs((start_time_ms() + ttl_ms_ / 2));
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  // Second fetch hits the metadata cache and the rewritten resource is served
  // out.
  CheckWarmCache("second_fetch_6");

  ResetHeadersAndStats();
  // We get a 304 if we send a request with an If-None-Match matching the hash
  // of the rewritten resource.
  AddRequestAttribute(HttpAttributes::kIfNoneMatch, etag_);
  SetDriverRequestHeaders();
  // A bare 304 carries no caching headers, so it has no cache lifetime.
  FetchAndCheckResponse(cache_jpg_url_, "", true, 0, nullptr, 0);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers_.status_code());
  // We hit the metadata cache and find that the etag matches the hash of the
  // rewritten resource.
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  ResetHeadersAndStats();
  // The etag doesn't match and hence we serve the full response.
  AddRequestAttribute(HttpAttributes::kIfNoneMatch, "no-match");
  SetDriverRequestHeaders();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  EXPECT_EQ(HttpStatus::kOK, response_headers_.status_code());
  // We hit the metadata cache, but the etag doesn't match so we fetch the
  // rewritten resource from the HTTPCache and serve it out.
  CheckWarmCache("etag_mismatch");

  // Delete the rewritten resource from cache to check if reconstruction works.
  lru_cache()->Delete(HttpCacheKey(rewritten_jpg_url_));

  ResetHeadersAndStats();
  // Original resource is served with the date set to start time, with the
  // injected s-maxage window.
  // The ETag we check for here is the ETag HTTPCache synthesized for
  // the original resource.
  FetchAndCheckResponse(cache_jpg_url_, "good", true, kInjectedSMaxAgeMs,
                        kEtag0.c_str(), start_time_ms());
  // We find the metadata in cache, but don't find the rewritten resource.
  // Hence, we reconstruct the resource and insert it into cache. We see 2
  // identical reinserts - one for the image rewrite filter metadata and one for
  // the in-place metadata.
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  // Reconstruction loads the module's own entry from the cache.
  EXPECT_EQ(2, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(3, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_identical_reinserts());
  EXPECT_EQ(1, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  // For only the next request, update the date header so that freshening
  // succeeds.
  FetcherUpdateDateHeaders();
  ResetHeadersAndStats();
  int64 time_ms = start_time_ms() + ttl_ms_ - 2 * Timer::kMinuteMs;
  SetTimeMs(time_ms);
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, 2 * Timer::kMinuteMs,
                        etag_, time_ms);
  // This fetch hits the metadata cache and the rewritten resource is served
  // out. Freshening is triggered here and we insert the freshened response and
  // metadata into the cache.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(3, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(2, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  mock_url_fetcher()->set_update_date_headers(false);

  ResetHeadersAndStats();
  SetTimeMs((start_time_ms() + ttl_ms_ * 5 / 4));
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true,
                        ttl_ms_ * 3 / 4 - 2 * Timer::kMinuteMs, etag_,
                        start_time_ms() + ttl_ms_ * 5 / 4);
  // Since the previous request freshened the metadata, this fetch hits the
  // metadata cache and the rewritten resource is served out. Note that no
  // freshening needs to be triggered here.
  CheckWarmCache("freshened_metadata");

  AdvanceTimeMs(2 * ttl_ms_);
  ResetHeadersAndStats();
  // Serves the original bytes again, with the injected window.
  FetchAndCheckResponse(cache_jpg_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, timer()->NowMs());
  // The metadata and cache entry is stale now. Fetch the content and serve out
  // the original. We will however notice that the contents did not
  // actually change and update the metadata cache promptly, without
  // rewriting.
  // Rebuilding after the input expired starts from the module's own fetch.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(4, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(4, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
}

// Pin: a successfully in-place-optimized resource served at its
// ORIGINAL URL must not gain 'public' or 'immutable'. Those tokens are a
// serving-time upgrade reserved for hash-committed .pagespeed. URLs
// (ServerContext::ApplyRewrittenUrlCacheControl). The nested rewritten
// resource's STORED headers carry 'public' only when every input explicitly
// said so, and RewriteContext::FetchContext::FetchFallbackDoneImpl
// propagates exactly that signal onto the in-place response -- explicit
// 'public' the origin never sent would newly authorize shared caches to
// store Authorization-bearing responses (RFC 9111 s3.5). A regression here
// usually means the stored .pagespeed. entry got stamped unconditionally.
TEST_F(InPlaceRewriteContextTest, OptimizedOriginalUrlServeNotPublicImmutable) {
  Init();
  // First fetch triggers the background rewrite; it streams the
  // not-yet-optimized bytes.
  FetchAndCheckResponse(cache_jpg_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, start_time_ms());
  ResetHeadersAndStats();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  // Second fetch serves the optimized bytes at the original URL.
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  // The origin authored no Cache-Control beyond its TTL, so the in-place
  // serve must carry exactly the remaining max-age -- nothing else.
  EXPECT_EQ(
      StrCat("max-age=", Integer64ToString(ttl_ms_ / 2 / Timer::kSecondMs)),
      response_headers_.LookupJoined(HttpAttributes::kCacheControl));
  EXPECT_FALSE(
      response_headers_.HasValue(HttpAttributes::kCacheControl, "public"));
  EXPECT_FALSE(
      response_headers_.HasValue(HttpAttributes::kCacheControl, "immutable"));
}

TEST_F(InPlaceRewriteContextTest, CacheablePngUrlRewritingSucceeds) {
  Init();
  ExpectInPlaceImageSuccessFlow(cache_png_url_);
}

TEST_F(InPlaceRewriteContextTest, CacheablePngUrlRewritingSucceedsWithShards) {
  Init();
  const char kShard1[] = "http://s1.example.com/";
  const char kShard2[] = "http://s2.example.com/";
  AddShard("http://www.example.com", StrCat(kShard1, ",", kShard2));
  ExpectInPlaceImageSuccessFlow(cache_png_url_);
}

TEST_F(InPlaceRewriteContextTest, CacheableiGifUrlRewritingSucceeds) {
  Init();
  ExpectInPlaceImageSuccessFlow(cache_gif_url_);
}

TEST_F(InPlaceRewriteContextTest, CacheableWebpUrlRewritingSucceeds) {
  Init();
  ExpectInPlaceImageSuccessFlow(cache_webp_url_);
}

TEST_F(InPlaceRewriteContextTest, CacheablePngUrlRewritingFails) {
  // Setup the image filter to fail at rewriting.
  Init();
  img_filter_->set_enabled(false);
  // The first serve streams the not-yet-optimized bytes.
  FetchAndCheckResponse(cache_png_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        original_etag_, start_time_ms());

  // First fetch misses initial metadata lookup, finds original in cache.
  // The rewrite fails and metadata is inserted into the
  // cache indicating that the rewriting didn't succeed.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(2, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_png_url_, cache_body_, true, ttl_ms_,
                        original_etag_, start_time_ms());
  // Second fetch hits the metadata cache, sees that the rewrite failed and
  // fetches and serves the original resource from cache.
  CheckWarmCache("second_fetch_7");
}

// A resource the optimizer cannot improve, from an origin that gives it a
// one-year lifetime (archived upstream issue
// apache/incubator-pagespeed-mod#1485): only the serve before the in-place
// decision carries the injected s-maxage. Once the rewrite is recorded as not
// applicable, every later serve keeps the origin's full lifetime and has no
// s-maxage. This covers the in-place rewrite context only; the nginx and
// Apache adapters' header handling is not exercised here.
TEST_F(InPlaceRewriteContextTest, UnimprovableResourceLosesSMaxAgeOnceDecided) {
  Init();
  const GoogleString url = "http://www.example.com/already_optimal.png";
  AddResponse(url, kContentTypePng, cache_body_, start_time_ms(),
              Timer::kYearMs, original_etag_, "", kWriteToCache, kTransform);
  img_filter_->set_enabled(false);

  FetchAndCheckResponse(url, cache_body_, true, kInjectedSMaxAgeMs,
                        original_etag_, start_time_ms());
  EXPECT_TRUE(
      response_headers_.HasValue(HttpAttributes::kCacheControl, "s-maxage=10"))
      << response_headers_.ToString();

  for (int i = 0; i < 3; ++i) {
    ResetHeadersAndStats();
    FetchAndCheckResponse(url, cache_body_, true, Timer::kYearMs,
                          original_etag_, start_time_ms());
    EXPECT_EQ(GoogleString::npos,
              response_headers_.LookupJoined(HttpAttributes::kCacheControl)
                  .find("s-maxage"))
        << "serve " << i + 2 << ": " << response_headers_.ToString();
  }
}

TEST_F(InPlaceRewriteContextTest, CacheableJsUrlRewritingSucceeds) {
  Init();
  // First serve streams the not-yet-optimized bytes.
  FetchAndCheckResponse(cache_js_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, start_time_ms());

  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache. Also, the resource gets rewritten and the rewritten
  // resource gets inserted into cache.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(3, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(4, lru_cache()->num_misses());
  EXPECT_EQ(5, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(1, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  ResetHeadersAndStats();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  FetchAndCheckResponse(cache_js_url_, "good:jm", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  // Second fetch hits the metadata cache and the rewritten resource is served
  // out.
  CheckWarmCache("second_fetch_8");

  AdvanceTimeMs(2 * ttl_ms_);
  ResetHeadersAndStats();
  // Serves the original bytes again, with the injected window.
  FetchAndCheckResponse(cache_js_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, timer()->NowMs());
  // The metadata and cache entry is stale now. Fetch the content and serve it
  // out without rewriting. The background rewrite will then revalidate
  // a previous rewrite's result and reuse it.
  // Rebuilding after the input expired starts from the module's own fetch.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(4, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(4, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
}

TEST_F(InPlaceRewriteContextTest, CacheableJsUrlRewritingWithStaleServing) {
  Init();
  options()->ClearSignatureForTesting();
  options()->set_metadata_cache_staleness_threshold_ms(ttl_ms_);
  server_context()->ComputeSignature(options());

  // First serve streams the not-yet-optimized bytes.
  FetchAndCheckResponse(cache_js_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, start_time_ms());

  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache. Also, the resource gets rewritten and the rewritten
  // resource gets inserted into cache.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(3, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(4, lru_cache()->num_misses());
  EXPECT_EQ(5, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(1, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  ResetHeadersAndStats();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  FetchAndCheckResponse(cache_js_url_, "good:jm", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  CheckWarmCache("second_fetch_9");

  SetTimeMs(start_time_ms() + (3 * ttl_ms_) / 2);
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_js_url_, "good:jm", true,
                        RewriteOptions::kDefaultImplicitCacheTtlMs, etag_,
                        start_time_ms() + (3 * ttl_ms_) / 2);
  // The metadata and cache entry is stale now. We serve the rewritten resource
  // here, but trigger a fetch and rewrite to update the metadata.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(3, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(2, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
}

TEST_F(InPlaceRewriteContextTest, CacheableJsUrlModifiedImplicitCacheTtl) {
  Init();
  response_headers_.set_implicit_cache_ttl_ms(500 * Timer::kSecondMs);
  // The implicit TTL applies to the recorded entry; the streamed
  // not-yet-optimized serve is clamped to the injected s-maxage window.
  FetchAndCheckResponse(cache_js_no_max_age_url_, cache_body_,
                        /* expected_success= */ true,
                        /* expected_ttl= */ kInjectedSMaxAgeMs,
                        /* etag= */ nullptr,
                        /* date_ms= */ start_time_ms());
}

TEST_F(InPlaceRewriteContextTest, CacheableCssUrlIfCssRewritingDisabled) {
  Init();
  options()->ClearSignatureForTesting();
  options()->DisableFilter(RewriteOptions::kRewriteCss);
  server_context()->ComputeSignature(options());
  // First serve streams the not-yet-optimized bytes.
  FetchAndCheckResponse(cache_css_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, start_time_ms());

  // First fetch succeeds at the fetcher, no rewriting happens since the css
  // filter is disabled, and metadata indicating a rewriting failure gets
  // inserted into cache.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  ResetHeadersAndStats();

  // The ETag we check for here is the ETag HTTPCache synthesized for
  // the original resource.
  FetchAndCheckResponse(cache_css_url_, cache_body_, true, ttl_ms_,
                        kEtag0.c_str(), start_time_ms());

  // Second fetch hits the metadata cache, finds that the result is not
  // optimizable. It then looks up cache for the original and finds it.
  CheckWarmCache("second_fetch_10");
}

TEST_F(InPlaceRewriteContextTest, CacheableCssUrlRewritingSucceeds) {
  Init();
  EnableCachePurge();
  // First serve streams the not-yet-optimized bytes.
  FetchAndCheckResponse(cache_css_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, start_time_ms());

  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache. Also, the resource gets rewritten and the rewritten
  // resource gets inserted into cache.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(3, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(4, lru_cache()->num_misses());
  EXPECT_EQ(5, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(1, css_filter_->num_rewrites());

  ResetHeadersAndStats();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  FetchAndCheckResponse(cache_css_url_, "good:cf", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  // Second fetch hits the metadata cache and the rewritten resource is served
  // out.
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  AdvanceTimeMs(2 * ttl_ms_);
  ResetHeadersAndStats();
  int64 date_of_css_ms = timer()->NowMs();
  // Serves the original bytes again, with the injected window.
  FetchAndCheckResponse(cache_css_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, date_of_css_ms);
  // The metadata and cache entry is stale now. Fetch the content and serve it
  // out without rewriting. The background rewrite attempt will end up reusing
  // the old result due to revalidation, however.
  // Rebuilding after the input expired starts from the module's own fetch.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(4, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(4, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  mock_url_fetcher()->set_timer(timer());
  mock_url_fetcher()->set_update_date_headers(true);
  SetCacheInvalidationTimestamp();
  date_of_css_ms = timer()->NowMs();

  // Having flushed cache, we are now back to serving the origin content,
  // with the injected window.
  FetchAndCheckResponse(cache_css_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, date_of_css_ms);

  // Next time we'll serve optimized content.
  AdvanceTimeMs(ttl_ms_ / 2);
  ResetHeadersAndStats();
  int64 expected_ttl_ms = ttl_ms_ - (timer()->NowMs() - date_of_css_ms);
  FetchAndCheckResponse(cache_css_url_, "good:cf", true, expected_ttl_ms, etag_,
                        timer()->NowMs());
}

// A HEAD request for a cached, not yet rewritten original gets no body and
// does not start a rewrite of an empty input: later GETs get the original and
// then the full optimized body.
TEST_F(InPlaceRewriteContextTest, HeadRequestDoesNotRewriteEmptyInput) {
  Init();
  // The original is in the cache, not yet rewritten.
  AddResponse(cache_css_url_, kContentTypeCss, cache_body_, start_time_ms(),
              ttl_ms_, "", StringPiece(""), kWriteToCache, kTransform);
  ResetHeadersAndStats();

  {
    WorkerTestBase::SyncPoint sync(server_context()->thread_system());
    RequestContextPtr request_context(RequestContext::NewTestRequestContext(
        server_context()->thread_system()));
    NotifyingFetch head_fetch(request_context, options(), cache_css_url_, &sync,
                              &response_headers_);
    head_fetch.request_headers()->set_method(RequestHeaders::kHead);
    rewrite_driver()->FetchResource(cache_css_url_, &head_fetch);
    sync.Wait();
    rewrite_driver()->WaitForShutDown();
    mock_scheduler()->AwaitQuiescence();
    EXPECT_TRUE(head_fetch.done());
    EXPECT_EQ("", head_fetch.content());
  }
  EXPECT_EQ(0, css_filter_->num_rewrites());

  // GETs afterwards: the first serves the original bytes and rewrites in the
  // background, the next serves the full optimized body.
  for (const char* expected : {cache_body_.c_str(), "good:cf"}) {
    WorkerTestBase::SyncPoint sync(server_context()->thread_system());
    RequestContextPtr request_context(RequestContext::NewTestRequestContext(
        server_context()->thread_system()));
    NotifyingFetch get_fetch(request_context, options(), cache_css_url_, &sync,
                             &response_headers_);
    rewrite_driver()->FetchResource(cache_css_url_, &get_fetch);
    sync.Wait();
    rewrite_driver()->WaitForShutDown();
    mock_scheduler()->AwaitQuiescence();
    EXPECT_TRUE(get_fetch.done());
    EXPECT_EQ(expected, get_fetch.content());
  }
  EXPECT_EQ(1, css_filter_->num_rewrites());
}

// The in-place resource below was recorded from a response served to a
// client, and the module's own fetch of it gets 'fetch_status'.
class InPlaceRecordedInputTest : public InPlaceRewriteContextTest {
 protected:
  void RecordAndAnswerModuleFetch(HttpStatus::Code fetch_status,
                                  const GoogleString& fetched_body) {
    AddResponse(cache_css_url_, kContentTypeCss, cache_body_, start_time_ms(),
                ttl_ms_, "", StringPiece(""), kWriteToCache, kTransform);
    ResponseHeaders fetched;
    SetDefaultHeaders(kContentTypeCss.mime_type(), &fetched);
    fetched.SetStatusAndReason(fetch_status);
    fetched.SetDateAndCaching(start_time_ms(), ttl_ms_);
    mock_url_fetcher()->SetResponse(cache_css_url_, fetched, fetched_body);
    ResetHeadersAndStats();
  }

  // Requests the in-place resource; returns the body served.
  GoogleString FetchInPlace() {
    WorkerTestBase::SyncPoint sync(server_context()->thread_system());
    RequestContextPtr request_context(RequestContext::NewTestRequestContext(
        server_context()->thread_system()));
    NotifyingFetch fetch(request_context, options(), cache_css_url_, &sync,
                         &response_headers_);
    rewrite_driver()->FetchResource(cache_css_url_, &fetch);
    sync.Wait();
    rewrite_driver()->WaitForShutDown();
    mock_scheduler()->AwaitQuiescence();
    EXPECT_TRUE(fetch.done());
    EXPECT_TRUE(fetch.success());
    GoogleString body = fetch.content().as_string();
    ResetHeadersAndStats();
    return body;
  }
};

// The module's own fetch is refused: the recorded response keeps being
// served as recorded, and is never optimized.
TEST_F(InPlaceRecordedInputTest, RefusedToModuleIsNotOptimized) {
  Init();
  RecordAndAnswerModuleFetch(HttpStatus::kForbidden, "refused");
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(cache_body_, FetchInPlace()) << i;
    EXPECT_EQ(0, css_filter_->num_rewrites()) << i;
  }
}

// Records completion of a fetch in a flag a sequence can wait on.
class DoneFlagFetch : public StringAsyncFetch {
 public:
  DoneFlagFetch(const RequestContextPtr& request_context, bool* done)
      : StringAsyncFetch(request_context), done_flag_(done) {}

 protected:
  void HandleDone(bool success) override {
    StringAsyncFetch::HandleDone(success);
    *done_flag_ = true;
  }

 private:
  bool* done_flag_;
};

// When the driver runs its tasks on the request thread, the outcome of the
// module's own load is handled there, not on the thread that finished the
// load: a response held back for the optimized version is written from the
// request thread.
TEST_F(InPlaceRecordedInputTest, RefusedLoadHandledOnDriverSequence) {
  options()->set_in_place_wait_for_optimized(true);
  Init();
  RecordAndAnswerModuleFetch(HttpStatus::kForbidden, "refused");
  SetupWaitFetcher();
  rewrite_driver()->RunTasksOnRequestThread();
  Scheduler::Sequence* sequence = rewrite_driver()->scheduler_sequence();
  Scheduler* scheduler = server_context()->scheduler();
  bool done = false;
  DoneFlagFetch fetch(rewrite_driver()->request_context(), &done);
  rewrite_driver()->FetchResource(cache_css_url_, &fetch);
  {
    ScopedMutex lock(scheduler->mutex());
    sequence->AssertSchedulerMutexHeld();
    EXPECT_FALSE(sequence->RunTasksUntil(Timer::kSecondMs, &done));
  }
  // The module's fetch finishes on this (not the request's) sequence.
  factory()->wait_url_async_fetcher()->CallCallbacks();
  EXPECT_FALSE(done);
  {
    ScopedMutex lock(scheduler->mutex());
    sequence->AssertSchedulerMutexHeld();
    rewrite_driver()->AssertRewriteMutexHeld();
    EXPECT_TRUE(sequence->RunTasksUntil(Timer::kSecondMs, &done));
    rewrite_driver()->SwitchToQueuedWorkerPool();
  }
  EXPECT_EQ(cache_body_, fetch.buffer());
  mock_scheduler()->AwaitQuiescence();
}

// The same while the response is held back for the optimized version.
TEST_F(InPlaceRecordedInputTest, RefusedToModuleIsNotOptimizedWhenWaiting) {
  options()->set_in_place_wait_for_optimized(true);
  Init();
  RecordAndAnswerModuleFetch(HttpStatus::kForbidden, "refused");
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(cache_body_, FetchInPlace()) << i;
    EXPECT_EQ(0, css_filter_->num_rewrites()) << i;
  }
}

// Proxying stores the client-facing response and the module then fetches
// the resource itself; the entry that stays is the module's own.
TEST_F(InPlaceRecordedInputTest, ModuleFetchedEntryIsTheOneStored) {
  Init();
  FetchInPlace();
  class RawCallback : public CacheInterface::Callback {
   public:
    void Done(CacheInterface::KeyState state) override {
      if (state == CacheInterface::kAvailable) {
        raw = value().Value().as_string();
      }
    }
    GoogleString raw;
  } callback;
  lru_cache()->Get(HttpCacheKey(cache_css_url_), &callback);
  EXPECT_NE(GoogleString::npos, callback.raw.find("@Fetched-Input"));
}

// An entry the module stored from its own fetch, served in place before an
// optimized version exists, is presented with the caching headers the server
// sent: a lifetime derived for a response that stated none is not served.
class InPlaceServedHeadersTest : public InPlaceRecordedInputTest {
 protected:
  // Keeps the response headers as they were when the response started,
  // before anything after the serve recomputes them.
  class HeadersAtStartFetch : public NotifyingFetch {
   public:
    HeadersAtStartFetch(const RequestContextPtr& request_context,
                        RewriteOptions* options, const GoogleString& url,
                        WorkerTestBase::SyncPoint* sync,
                        ResponseHeaders* response_headers,
                        ResponseHeaders* at_start)
        : NotifyingFetch(request_context, options, url, sync, response_headers),
          at_start_(at_start) {}

   protected:
    void HandleHeadersComplete() override {
      at_start_->CopyFrom(*response_headers());
      NotifyingFetch::HandleHeadersComplete();
    }

   private:
    ResponseHeaders* at_start_;
  };

  // Stores cache_css_url_ with header 'name: value' added (none if 'name' is
  // null), as the module's own fetch stores it, or as the in-place recorder
  // stores a response (whose own Cache-Control, here none, replaces the
  // computed one), and returns the headers of an in-place response.
  void ServeStored(const char* name, const GoogleString& value,
                   ResponseHeaders* served, bool recorded = false) {
    Init();
    lru_cache()->Clear();
    ResponseHeaders stored;
    SetDefaultHeaders(kContentTypeCss.mime_type(), &stored);
    stored.SetDate(timer()->NowMs());
    if (name != nullptr) {
      stored.Add(name, value);
    }
    stored.ComputeCaching();
    if (recorded) {
      stored.RemoveAll(HttpAttributes::kCacheControl);
    }
    HTTPValue entry;
    entry.SetHeaders(&stored);
    entry.Write(cache_body_, message_handler());
    if (recorded) {
      http_cache()->Put(cache_css_url_, rewrite_driver()->CacheFragment(),
                        RequestHeaders::Properties(),
                        options()->ComputeHttpOptions(), &entry,
                        message_handler());
    } else {
      http_cache()->PutFetchedInput(
          cache_css_url_, rewrite_driver()->CacheFragment(),
          RequestHeaders::Properties(), options()->ComputeHttpOptions(), &entry,
          message_handler());
    }
    ResetHeadersAndStats();
    css_filter_->set_exceed_deadline(false);
    WorkerTestBase::SyncPoint sync(server_context()->thread_system());
    RequestContextPtr request_context(RequestContext::NewTestRequestContext(
        server_context()->thread_system()));
    HeadersAtStartFetch fetch(request_context, options(), cache_css_url_, &sync,
                              &response_headers_, served);
    // Served at the origin, as the server modules serve in place.
    rewrite_driver()->SetRequestHeaders(*fetch.request_headers());
    GoogleUrl gurl(cache_css_url_);
    rewrite_driver()->FetchInPlaceResource(gurl, false /* proxy_mode */,
                                           &fetch);
    sync.Wait();
    rewrite_driver()->WaitForShutDown();
    mock_scheduler()->AwaitQuiescence();
    EXPECT_EQ(cache_body_, fetch.content());
  }
};

// For a response that states no lifetime, an in-place response served from
// the module's own entry looks like one served from a recorded entry.
TEST_F(InPlaceServedHeadersTest, NoDerivedLifetime) {
  ResponseHeaders from_recorded;
  ServeStored(nullptr, "", &from_recorded, true /* recorded */);
  ResponseHeaders from_fetched;
  ServeStored(nullptr, "", &from_fetched);
  EXPECT_EQ(from_recorded.LookupJoined(HttpAttributes::kCacheControl),
            from_fetched.LookupJoined(HttpAttributes::kCacheControl))
      << from_recorded.ToString() << from_fetched.ToString();
  EXPECT_TRUE(
      from_recorded.HasValue(HttpAttributes::kCacheControl, "s-maxage=10"));
  EXPECT_TRUE(from_recorded.Has(HttpAttributes::kExpires));
  EXPECT_TRUE(from_fetched.Has(HttpAttributes::kExpires));
}

TEST_F(InPlaceServedHeadersTest, OriginMaxAgeKept) {
  ResponseHeaders served;
  ServeStored(HttpAttributes::kCacheControl, "max-age=600", &served);
  EXPECT_TRUE(served.HasValue(HttpAttributes::kCacheControl, "max-age=600"));
  EXPECT_TRUE(served.HasValue(HttpAttributes::kCacheControl, "s-maxage=10"));
}

TEST_F(InPlaceServedHeadersTest, OriginExpiresKept) {
  GoogleString expires;
  ConvertTimeToString(timer()->NowMs() + 600 * Timer::kSecondMs, &expires);
  ResponseHeaders served;
  ServeStored(HttpAttributes::kExpires, expires, &served);
  EXPECT_STREQ(expires.c_str(), served.Lookup1(HttpAttributes::kExpires));
}

// The optimized version is built from what the module fetched, not from
// the recorded response.
TEST_F(InPlaceRecordedInputTest, OptimizesWhatTheModuleFetched) {
  Init();
  RecordAndAnswerModuleFetch(HttpStatus::kOK, "fetched");
  FetchInPlace();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  EXPECT_EQ("fetched:cf", FetchInPlace());
}

// A stylesheet the origin sends in a coding the rewriters cannot decode is
// served to each client as the origin sent it, coding header included.  It
// is not stored, so every request goes to the origin, and it is never
// optimized.
TEST_F(InPlaceRewriteContextTest, CssInUndecodableCodingIsServedAsSent) {
  Init();
  const GoogleString url = "http://www.example.com/coded.css";
  ResponseHeaders headers;
  SetDefaultHeaders(kContentTypeCss.mime_type(), &headers);
  headers.SetDateAndCaching(start_time_ms(), ttl_ms_);
  headers.Add(HttpAttributes::kContentEncoding, "zstd");
  mock_url_fetcher()->SetResponse(url, headers, cache_body_);

  int total_origin_fetches = 0;
  for (const char* round : {"first", "repeat"}) {
    ResetHeadersAndStats();
    FetchAndCheckResponse(url, cache_body_, true, ttl_ms_, nullptr,
                          start_time_ms());
    EXPECT_STREQ("zstd",
                 response_headers_.Lookup1(HttpAttributes::kContentEncoding))
        << round;
    total_origin_fetches += counting_url_async_fetcher()->fetch_count();
    EXPECT_EQ(0, http_cache()->cache_hits()->Get()) << round;
    EXPECT_EQ(0, http_cache()->cache_inserts()->Get()) << round;
    EXPECT_EQ(0, css_filter_->num_rewrites()) << round;
  }
  EXPECT_EQ(2, total_origin_fetches);
}

// A copy of a stylesheet in a coding the cache cannot adapt, stored before
// that was refused on the way in, is neither served nor used as the input
// of the optimization: the stylesheet is fetched again, and the optimized
// result is built from what the origin sends now.
TEST_F(InPlaceRewriteContextTest,
       StoredCssInOtherCodingIsNotServedOrRewritten) {
  Init();
  EnableCachePurge();
  const char kStoredCopy[] = "stored copy in another coding";
  ResponseHeaders coded_headers;
  SetDefaultHeaders(kContentTypeCss.mime_type(), &coded_headers);
  coded_headers.SetDateAndCaching(start_time_ms(), ttl_ms_);
  coded_headers.Add(HttpAttributes::kContentEncoding, "br");
  coded_headers.ComputeCaching();
  http_cache()->Put(cache_css_url_, rewrite_driver()->CacheFragment(),
                    RequestHeaders::Properties(),
                    ResponseHeaders::GetVaryOption(options()->respect_vary()),
                    &coded_headers, kStoredCopy, message_handler());

  // Served path: the origin's own bytes, not the stored copy.
  FetchAndCheckResponse(cache_css_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, start_time_ms());
  EXPECT_FALSE(response_headers_.Has(HttpAttributes::kContentEncoding));
  EXPECT_LE(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, css_filter_->num_rewrites());

  // Rewrite input: the optimized result is the origin's stylesheet, rewritten.
  ResetHeadersAndStats();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  FetchAndCheckResponse(cache_css_url_, "good:cf", true, ttl_ms_ / 2, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
}

TEST_F(InPlaceRewriteContextTest, NonCacheableUrlNoRewriting) {
  Init();
  FetchAndCheckResponse(nocache_html_url_, nocache_body_, true, 0, nullptr,
                        timer()->NowMs());
  // First fetch misses initial cache lookup, succeeds at fetch and we don't
  // insert into cache because it's not cacheable. Don't attempt to rewrite
  // this since its not cacheable.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
}

// Tests that with correct flags set, the uncacheable resource will be
// rewritten. Also checks, that resource will not be inserted.
TEST_F(InPlaceRewriteContextTest, NonCacheableUrlRewriting) {
  Init();

  // Modify options for our test.
  options()->ClearSignatureForTesting();
  options()->set_in_place_wait_for_optimized(true);
  options()->set_rewrite_uncacheable_resources(true);
  server_context()->ComputeSignature(options());

  // An uncacheable response has no cache lifetime; the cacheability itself
  // is checked below.
  FetchAndCheckResponse(nocache_js_url_, StrCat(cache_body_, ":", "jm"),
                        true /* success */, 0 /* ttl (ms) */, etag_ /* etag */,
                        timer()->NowMs());

  // Shouldn't be cacheable at all.
  EXPECT_FALSE(response_headers_.IsBrowserCacheable());
  EXPECT_FALSE(response_headers_.IsProxyCacheable());

  // First fetch misses initial cache lookup, succeeds at fetch and we don't
  // insert into cache because it's not cacheable. But since flags are set to
  // rewrite uncacheable resources, JS rewriting should occur.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  // Should have been rewritten.
  EXPECT_EQ(1, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
  EXPECT_EQ(1, in_place_uncacheable_rewrites_->Get());
}

// Tests, that with correct flags set the private cacheable resource will be
// rewritten. Also checks, that the resource will not be cached.
TEST_F(InPlaceRewriteContextTest, PrivateCacheableUrlRewriting) {
  Init();

  // Modify options for our test.
  options()->ClearSignatureForTesting();
  options()->set_in_place_wait_for_optimized(true);
  options()->set_rewrite_uncacheable_resources(true);
  server_context()->ComputeSignature(options());

  // The ttl is just a value in proto, actual cacheable values will be checked
  // below.
  FetchAndCheckResponse(private_cache_js_url_, StrCat(cache_body_, ":", "jm"),
                        true /* success */, 1000 /* ttl (s) */,
                        etag_ /* etag */, timer()->NowMs());
  // Should be cacheable.
  EXPECT_TRUE(response_headers_.IsBrowserCacheable());

  // But only in a private way.
  EXPECT_FALSE(response_headers_.IsProxyCacheable());

  // First fetch misses initial cache lookup, succeeds at fetch and we don't
  // insert into cache because it's not cacheable. But since flags are set to
  // rewrite uncacheable resources, JS rewriting should occur.
  // The module's own anonymous fetch before the optimization is built.
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  // Should have been rewritten.
  EXPECT_EQ(1, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
  EXPECT_EQ(1, in_place_uncacheable_rewrites_->Get());
}

TEST_F(InPlaceRewriteContextTest, BadUrlNoRewriting) {
  Init();
  FetchAndCheckResponse(bad_url_, bad_body_, true, 0, nullptr, start_time_ms());
  // First fetch misses initial cache lookup, succeeds at fetch and we don't
  // insert into cache because it's not cacheable. Don't attempt to rewrite
  // this since its not cacheable.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
}

TEST_F(InPlaceRewriteContextTest, PermanentRedirectNoRewriting) {
  options()->set_in_place_wait_for_optimized(true);
  Init();
  FetchAndCheckResponse(redirect_url_, redirect_body_,
                        true /* expected_success */, 36000 /* ttl (s) */,
                        nullptr /* etag */, start_time_ms());

  // Don't attempt to rewrite this since it's not a 200 response.
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
}

TEST_F(InPlaceRewriteContextTest, FetchFailedNoRewriting) {
  Init();
  FetchAndCheckResponse("http://www.notincache.com", "", false, 0, nullptr,
                        start_time_ms());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());
}

TEST_F(InPlaceRewriteContextTest, MidStreamFetchFailureWhileStreaming) {
  Init();
  mock_url_fetcher()->SetResponseFailure(cache_js_url_);
  FetchFailingMidStream(cache_js_url_);
  EXPECT_EQ(0, js_filter_->num_rewrites());
}

TEST_F(InPlaceRewriteContextTest, MidStreamFetchFailureWhileWaitingToOptimize) {
  options()->set_in_place_wait_for_optimized(true);
  Init();
  mock_url_fetcher()->SetResponseFailure(cache_js_url_);
  FetchFailingMidStream(cache_js_url_);
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_TRUE(response_headers_.IsErrorStatus());
}

TEST_F(InPlaceRewriteContextTest, HandleResourceCreationFailure) {
  // Regression test. Trying to in-place optimize https resources with
  // a fetcher that didn't support https would fail to invoke the callbacks
  // and leak the rewrite driver.
  Init();
  factory()->mock_url_async_fetcher()->set_fetcher_supports_https(false);
  FetchAndCheckResponse("https://www.example.com", "", false, 0, nullptr, 0);
}

TEST_F(InPlaceRewriteContextTest, ResponseHeaderMimeTypeUpdate) {
  options()->set_in_place_wait_for_optimized(true);
  Init();
  // We are going to rewrite a PNG image below. Assume it will be converted
  // to a JPEG.
  img_filter_->set_output_content_type(&kContentTypeJpeg);
  FetchAndCheckResponse(cache_png_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_STREQ(kContentTypeJpeg.mime_type(),
               response_headers_.Lookup1(HttpAttributes::kContentType));
}

// In-place optimization must never produce browser-dependent bytes, so it must
// never emit a Vary: header -- whatever the request advertises, and whatever
// conversion filters are enabled.  Enabling the retired
// in_place_optimize_for_browser filter changes nothing.
TEST_F(InPlaceRewriteContextTest, InPlaceNeverAddsVary) {
  options()->set_in_place_wait_for_optimized(true);
  set_optimize_for_browser(true);
  Init();

  // A WebP-advertising browser.
  ResetUserAgent(UserAgentMatcher::kTestUserAgentWebP);
  SetAcceptWebp();
  SetDriverRequestHeaders();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(nullptr, response_headers_.Lookup1(HttpAttributes::kVary));
  EXPECT_FALSE(response_headers_.HasValue(HttpAttributes::kCacheControl,
                                          HttpAttributes::kPrivate));

  // A non-WebP browser gets the same treatment.
  ResetHeadersAndStats();
  ResetUserAgent(UserAgentMatcher::kTestUserAgentNoWebP);
  SetDriverRequestHeaders();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(nullptr, response_headers_.Lookup1(HttpAttributes::kVary));

  // CSS, whose constituent images used to force Vary: User-Agent.
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_css_url_, "good:cf", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(nullptr, response_headers_.Lookup1(HttpAttributes::kVary));

  // Internet Explorer no longer gets Cache-Control: private either.
  ResetHeadersAndStats();
  ResetUserAgent(
      "Mozilla/5.0 (compatible; MSIE 9.0; Windows NT 6.1; Trident/5.0)");
  SetDriverRequestHeaders();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(nullptr, response_headers_.Lookup1(HttpAttributes::kVary));
  EXPECT_FALSE(response_headers_.HasValue(HttpAttributes::kCacheControl,
                                          HttpAttributes::kPrivate));
}

// The in-place metadata key must be user-agent independent: a WebP browser and
// a non-WebP browser share one cache entry, so the second request is a hit.
TEST_F(InPlaceRewriteContextTest, InPlaceMetadataKeyIsUserAgentIndependent) {
  options()->set_in_place_wait_for_optimized(true);
  set_optimize_for_browser(true);
  Init();

  ResetUserAgent(UserAgentMatcher::kTestUserAgentWebP);
  SetAcceptWebp();
  SetDriverRequestHeaders();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(1, img_filter_->num_rewrites());

  // A differently-capable agent reuses the very same optimized result.
  ResetHeadersAndStats();
  ResetUserAgent(UserAgentMatcher::kTestUserAgentNoWebP);
  SetDriverRequestHeaders();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(nullptr, response_headers_.Lookup1(HttpAttributes::kVary));
}

// The in-place path hands the nested rewriter a capability-free
// ResourceContext, so no browser capability is encoded into it at all.
TEST_F(InPlaceRewriteContextTest, InPlaceDoesNotEncodeBrowserCapabilities) {
  options()->set_in_place_wait_for_optimized(true);
  set_optimize_for_browser(true);
  Init();

  ResetUserAgent(UserAgentMatcher::kTestUserAgentWebP);
  SetAcceptWebp();
  SetDriverRequestHeaders();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(0, img_filter_->num_encode_user_agent());
  EXPECT_EQ(0, css_filter_->num_encode_user_agent());
  EXPECT_EQ(0, js_filter_->num_encode_user_agent());
}

// Companion to the three request-independence tests above, exercising
// the one population they do not: a client whose WebP capability is DERIVED
// FROM THE USER-AGENT STRING (Safari 16+ sends no image types in a navigation
// Accept), rather than from an Accept header.  That verdict is a guess, so it
// may only ever shape rewritten URLs, whose format is committed in the URL;
// in-place responses are cached under a request-independent key and served
// with no Vary, where a guessed WebP would be handed to clients that cannot
// decode it.  Request-independence guarantees this structurally --
// nothing on the in-place path consults request-derived WebP at all -- and
// this test pins that the guarantee holds for the UA-derived grant
// specifically: should per-request capability encoding ever return to this
// path, the D1 population must not come with it.
TEST_F(InPlaceRewriteContextTest, UserAgentDerivedWebpDoesNotReachInPlace) {
  options()->set_in_place_wait_for_optimized(true);
  set_optimize_for_browser(true);
  Init();

  // Safari 16 with a navigation Accept: no SetAcceptWebp() here, so the WebP
  // grant on this request can only be UA-derived.  Precondition first: the
  // grant really is live, so the assertions below cannot pass vacuously.
  ResetUserAgent(UserAgentMatcherTestBase::kSafari16UserAgent);
  SetDriverRequestHeaders();
  EXPECT_TRUE(
      rewrite_driver()->request_properties()->SupportsWebpRewrittenUrls());
  EXPECT_FALSE(rewrite_driver()->request_properties()->SupportsWebpInPlace());

  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(1, img_filter_->num_rewrites());
  // No Vary, no per-request capability encoding.
  EXPECT_EQ(nullptr, response_headers_.Lookup1(HttpAttributes::kVary));
  EXPECT_EQ(0, img_filter_->num_encode_user_agent());

  // A WebP-incapable client is served the very same cache entry -- which is
  // only safe because the Safari verdict never shaped it.
  ResetHeadersAndStats();
  ResetUserAgent(UserAgentMatcher::kTestUserAgentNoWebP);
  SetDriverRequestHeaders();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(nullptr, response_headers_.Lookup1(HttpAttributes::kVary));
}

TEST_F(InPlaceRewriteContextTest, LoadFromFile) {
  const int64 kIproFileTtlMs = 15000;
  options()->file_load_policy()->Associate("http://www.example.com", "/test/");
  options()->set_load_from_file_cache_ttl_ms(kIproFileTtlMs);
  WriteFile("/test/cacheable.js", cache_body_ /*"   alert ( 'foo ')   "*/);

  Init();

  // Serves of the file's original bytes carry the injected s-maxage window,
  // which is shorter than the file TTL.
  FetchAndCheckResponse(cache_js_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, start_time_ms());

  // First fetch misses initial cache lookup, succeeds at fetch and inserts
  // result into cache. Also, the resource gets rewritten and the rewritten
  // resource gets inserted into cache.
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(1, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  // Note that without file-input resources, we would expect that our
  // TTL would be reduced to ttl_ms_/2.  But it doesn't work like that
  // for files.  The TTL stays the same.
  ResetHeadersAndStats();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  FetchAndCheckResponse(cache_js_url_, "good:jm", true, kIproFileTtlMs, etag_,
                        start_time_ms() + ttl_ms_ / 2);
  // Second fetch hits the metadata cache and the rewritten resource is served
  // out.
  CheckWarmCache("second_fetch_11");
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(0, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  // Third fetch is the same exact deal.  The file hasn't actually
  // changed and the existing rewrite still is valid.  The metadata
  // cache does not go stale until the file is actually touched.
  AdvanceTimeMs(2 * ttl_ms_);
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_js_url_, "good:jm", true, kIproFileTtlMs, etag_,
                        timer()->NowMs());
  CheckWarmCache("third_fetch");

  // OK let's now move time forward a little and touch the file
  // without changing it.  This results in a total reset back to
  // the original state.  It seems like we could read the file
  // and see if it's changed, but we wind up queuing up the asynchronous
  // rewrite.
  AdvanceTimeMs(1 * Timer::kSecondMs);
  WriteFile("/test/cacheable.js", cache_body_ /*"   alert ( 'foo ')   "*/);
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_js_url_, cache_body_, true, kInjectedSMaxAgeMs,
                        nullptr, timer()->NowMs());
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(2, lru_cache()->num_hits());  // ipro-metadata, metadata
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());  // http, metadata, ipro-metadata
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(1, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  AdvanceTimeMs(1 * Timer::kSecondMs);
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_js_url_, "good:jm", true, kIproFileTtlMs, etag_,
                        timer()->NowMs());
  CheckWarmCache("second_fetch_after_touch");

  // Now change the content.
  AdvanceTimeMs(1 * Timer::kSecondMs);
  WriteFile("/test/cacheable.js", "new_content");
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_js_url_, "new_content", true, kInjectedSMaxAgeMs,
                        nullptr, timer()->NowMs());
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(2, lru_cache()->num_hits());  // ipro-metadata, metadata
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());  // http, metadata, ipro-metadata
  EXPECT_EQ(0, img_filter_->num_rewrites());
  EXPECT_EQ(1, js_filter_->num_rewrites());
  EXPECT_EQ(0, css_filter_->num_rewrites());

  AdvanceTimeMs(1 * Timer::kSecondMs);
  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_js_url_, "new_content:jm", true, kIproFileTtlMs,
                        etag_, timer()->NowMs());
  CheckWarmCache("second_fetch_after_mutation");
}

TEST_F(InPlaceRewriteContextTest, JsonWithJsContentTypeSucceeds) {
  Init();
  CheckCachingAndContentType(json_js_type_url_, "application/javascript",
                             cache_body_, RewriteOptions::kJavascriptMinId);
}

TEST_F(InPlaceRewriteContextTest, JsonWithJsonContentTypeSucceeds) {
  Init();
  CheckCachingAndContentType(json_json_type_url_, "application/json",
                             cache_body_, RewriteOptions::kJavascriptMinId);
}

TEST_F(InPlaceRewriteContextTest, JsonWithJsonContentTypeSynonymSucceeds) {
  Init();
  CheckCachingAndContentType(json_json_type_synonym_url_, "application/x-json",
                             cache_body_, RewriteOptions::kJavascriptMinId);
}

// In-place serving and cross-origin CORS requests (Origin set).  A copy is
// served to such a request only when the copy's own
// Access-Control-Allow-Origin allows the request's origin; otherwise the
// request goes past the stored copies: at the origin the server answers it
// itself, and a proxy fetches it for this client.
// Like the JavaScript minifier, keeps the input's headers that are not about
// caching, such as Access-Control-Allow-Origin, on the optimized version.
class HeaderKeepingJsFilter : public FakeFilter {
 public:
  class Context : public FakeFilter::Context {
   public:
    using FakeFilter::Context::Context;

    void DoRewriteSingle(const ResourcePtr input,
                         OutputResourcePtr output) override {
      FindServerContext()->MergeNonCachingResponseHeaders(input, output);
      FakeFilter::Context::DoRewriteSingle(input, output);
    }
  };

  explicit HeaderKeepingJsFilter(RewriteDriver* rewrite_driver)
      : FakeFilter(RewriteOptions::kJavascriptMinId, rewrite_driver,
                   semantic_type::kScript) {}

  RewriteContext* MakeFakeContext(RewriteDriver* driver, RewriteContext* parent,
                                  ResourceContext* resource_context) override {
    return new Context(this, driver, parent, resource_context);
  }
};

class InPlaceCorsGuardTest : public InPlaceRewriteContextTest {
 protected:
  // Notes what the response looked like each time its headers were
  // committed, which is when a server sends them on.
  class CommitNotingFetch : public NotifyingFetch {
   public:
    CommitNotingFetch(const RequestContextPtr& request_context,
                      RewriteOptions* options, const GoogleString& url,
                      WorkerTestBase::SyncPoint* sync,
                      ResponseHeaders* response_headers)
        : NotifyingFetch(request_context, options, url, sync,
                         response_headers) {}

    int commits = 0;
    int status_at_commit = 0;
    bool content_type_at_commit = false;
    int64 bytes_at_commit = -1;

   protected:
    void HandleHeadersComplete() override {
      ++commits;
      status_at_commit = response_headers()->status_code();
      content_type_at_commit =
          response_headers()->Has(HttpAttributes::kContentType);
      bytes_at_commit = content().size();
      NotifyingFetch::HandleHeadersComplete();
    }
  };

  FakeFilter* MakeJsFilter() override {
    return new HeaderKeepingJsFilter(rewrite_driver());
  }

  static constexpr char kOriginA[] = "https://a.example";
  static constexpr char kOriginB[] = "https://b.example";

  // What one in-place request got.
  struct Served {
    bool success = false;
    int status = 0;
    GoogleString body;
    GoogleString acao;   // Access-Control-Allow-Origin, "" when absent.
    int64 declined = 0;  // in_place_cors_declined after the request.
    int commits = 0;     // Times the response headers were committed.
    int status_at_commit = 0;
    bool content_type_at_commit = false;
    int64 bytes_at_commit = -1;  // Body bytes written before the commit.
  };

  // Answers the module's own fetch of cache_js_url_ (which carries no
  // Origin) with the given CORS headers, and stores the same response as
  // the in-place recording of it.  A null header is left out.
  void StoreJs(const char* acao, const char* vary) {
    Store(cache_js_url_, kContentTypeJavascript.mime_type(), acao, vary,
          nullptr);
  }

  // The same for any `url` and Content-Type, optionally with
  // Access-Control-Allow-Credentials.
  void Store(const GoogleString& url, const GoogleString& content_type,
             const char* acao, const char* vary, const char* credentials) {
    Init();
    ResponseHeaders headers;
    SetDefaultHeaders(content_type, &headers);
    headers.SetDateAndCaching(start_time_ms(), ttl_ms_);
    if (acao != nullptr) {
      headers.Add(HttpAttributes::kAccessControlAllowOrigin, acao);
    }
    if (vary != nullptr) {
      headers.Add(HttpAttributes::kVary, vary);
    }
    if (credentials != nullptr) {
      headers.Add(HttpAttributes::kAccessControlAllowCredentials, credentials);
    }
    mock_url_fetcher()->SetResponse(url, headers, cache_body_);
    headers.ComputeCaching();
    http_cache()->Put(url, rewrite_driver()->CacheFragment(),
                      RequestHeaders::Properties(),
                      ResponseHeaders::GetVaryOption(options()->respect_vary()),
                      &headers, cache_body_, message_handler());
    ResetHeadersAndStats();
  }

  // Requests `url` in place, with `origin` as the Origin request header
  // (none if null), at the origin (as the server modules do) or as a proxy.
  // A non-null `cookie` is sent as the Cookie request header, as a
  // credentialed cross-origin request sends it.
  Served Serve(const GoogleString& url, const char* origin, bool proxy_mode,
               const char* cookie = nullptr) {
    WorkerTestBase::SyncPoint sync(server_context()->thread_system());
    RequestContextPtr request_context(RequestContext::NewTestRequestContext(
        server_context()->thread_system()));
    CommitNotingFetch fetch(request_context, options(), url, &sync,
                            &response_headers_);
    if (origin != nullptr) {
      fetch.request_headers()->Add(HttpAttributes::kOrigin, origin);
    }
    if (cookie != nullptr) {
      fetch.request_headers()->Add(HttpAttributes::kCookie, cookie);
    }
    rewrite_driver()->SetRequestHeaders(*fetch.request_headers());
    GoogleUrl gurl(url);
    rewrite_driver()->FetchInPlaceResource(gurl, proxy_mode, &fetch);
    sync.Wait();
    rewrite_driver()->WaitForShutDown();
    mock_scheduler()->AwaitQuiescence();
    EXPECT_TRUE(fetch.done());
    Served served;
    served.success = fetch.success();
    served.status = response_headers_.status_code();
    served.body = fetch.content().as_string();
    const char* acao =
        response_headers_.Lookup1(HttpAttributes::kAccessControlAllowOrigin);
    served.acao = (acao == nullptr) ? "" : acao;
    served.commits = fetch.commits;
    served.status_at_commit = fetch.status_at_commit;
    served.content_type_at_commit = fetch.content_type_at_commit;
    served.bytes_at_commit = fetch.bytes_at_commit;
    served.declined =
        statistics()
            ->GetVariable(InPlaceRewriteContext::kInPlaceCorsDeclined)
            ->Get();
    return served;
  }

  // Serves cache_js_url_ at the origin twice without Origin, so the
  // optimized version exists and is what such a request gets.
  void OptimizeJs() {
    EXPECT_EQ(cache_body_, Serve(cache_js_url_, nullptr, false).body);
    ResetHeadersAndStats();
    SetTimeMs(start_time_ms() + ttl_ms_ / 2);
    EXPECT_EQ("good:jm", Serve(cache_js_url_, nullptr, false).body);
    ResetHeadersAndStats();
  }

  // The answer that sends the request to the server's own handler.
  static void ExpectDeclined(const Served& served) {
    EXPECT_FALSE(served.success);
    EXPECT_EQ(CacheUrlAsyncFetcher::kNotInCacheStatus, served.status);
    EXPECT_EQ("", served.body);
    EXPECT_EQ(1, served.declined);
  }
};

// The origin adds Access-Control-Allow-Origin only to requests that carry
// Origin, so the optimized version (built from the module's own fetch) has
// none.  A CORS request is not given it: the server answers it.
TEST_F(InPlaceCorsGuardTest, ConditionalOriginOptimizedCopyDeclined) {
  StoreJs(nullptr, nullptr);
  OptimizeJs();
  ExpectDeclined(Serve(cache_js_url_, kOriginA, false));
  // A request without Origin still gets the optimized version.
  ResetHeadersAndStats();
  Served plain = Serve(cache_js_url_, nullptr, false);
  EXPECT_EQ("good:jm", plain.body);
  EXPECT_EQ(0, plain.declined);
}

// The same request through a proxy is fetched for this client, with its
// Origin, instead of being answered from the stored copies.
TEST_F(InPlaceCorsGuardTest, ConditionalOriginProxyFetchesForTheClient) {
  StoreJs(nullptr, nullptr);
  OptimizeJs();
  Served served = Serve(cache_js_url_, kOriginA, true);
  EXPECT_TRUE(served.success);
  EXPECT_EQ(cache_body_, served.body);
  EXPECT_EQ(1, served.declined);
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, js_filter_->num_rewrites());
}

// The recorded response was what a request without Origin got; it is not
// replayed to a CORS request either.
TEST_F(InPlaceCorsGuardTest, RecordedCopyWithoutAcaoDeclined) {
  StoreJs(nullptr, nullptr);
  ExpectDeclined(Serve(cache_js_url_, kOriginA, false));
  EXPECT_EQ(0, js_filter_->num_rewrites());
}

// An origin that allows every origin keeps full optimization: the CORS
// request gets the optimized version, with the header it needs.
TEST_F(InPlaceCorsGuardTest, WildcardOriginServedOptimized) {
  StoreJs("*", nullptr);
  OptimizeJs();
  Served served = Serve(cache_js_url_, kOriginA, false);
  EXPECT_TRUE(served.success);
  EXPECT_EQ("good:jm", served.body);
  EXPECT_EQ("*", served.acao);
  EXPECT_EQ(0, served.declined);
}

// A response whose Access-Control-Allow-Origin names one origin was made
// for one request: it is never optimized, and a proxy serves the stored copy
// only to that origin.
TEST_F(InPlaceCorsGuardTest, SpecificOriginNotOptimized) {
  StoreJs(kOriginA, nullptr);
  Served first = Serve(cache_js_url_, kOriginA, true);
  EXPECT_TRUE(first.success);
  EXPECT_EQ(cache_body_, first.body);
  EXPECT_EQ(kOriginA, first.acao);
  EXPECT_EQ(0, first.declined);
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  ResetHeadersAndStats();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  EXPECT_EQ(cache_body_, Serve(cache_js_url_, nullptr, true).body);
  EXPECT_EQ(0, js_filter_->num_rewrites());
  ResetHeadersAndStats();
  Served other = Serve(cache_js_url_, kOriginB, true);
  EXPECT_EQ(1, other.declined);
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
}

// At the origin, such a response is left to the server to answer.
TEST_F(InPlaceCorsGuardTest, SpecificOriginLeftToTheServer) {
  StoreJs(kOriginA, nullptr);
  Served served = Serve(cache_js_url_, kOriginA, false);
  EXPECT_FALSE(served.success);
  EXPECT_EQ(CacheUrlAsyncFetcher::kNotInCacheStatus, served.status);
  EXPECT_EQ(0, js_filter_->num_rewrites());
}

// Vary: Origin says the response differs per origin, and the optimized
// version would lose that header: such a response is not optimized.
TEST_F(InPlaceCorsGuardTest, VaryOriginNotOptimized) {
  StoreJs("*", "Origin");
  EXPECT_EQ(cache_body_, Serve(cache_js_url_, nullptr, true).body);
  ResetHeadersAndStats();
  SetTimeMs(start_time_ms() + ttl_ms_ / 2);
  Served served = Serve(cache_js_url_, nullptr, true);
  EXPECT_EQ(cache_body_, served.body);
  EXPECT_TRUE(response_headers_.HasValue(HttpAttributes::kVary, "Origin"));
  EXPECT_EQ(0, js_filter_->num_rewrites());
}

// A proxy does not store a response made for one origin, and does not
// serve a stored one to another origin.
TEST_F(InPlaceCorsGuardTest, ProxyKeepsPerOriginResponsesApart) {
  StoreJs(kOriginA, "Origin");
  ResponseHeaders for_b;
  SetDefaultHeaders(kContentTypeJavascript.mime_type(), &for_b);
  for_b.SetDateAndCaching(start_time_ms(), ttl_ms_);
  for_b.Add(HttpAttributes::kAccessControlAllowOrigin, kOriginB);
  for_b.Add(HttpAttributes::kVary, "Origin");
  mock_url_fetcher()->SetResponse(cache_js_url_, for_b, cache_body_);
  Served served = Serve(cache_js_url_, kOriginB, true);
  EXPECT_TRUE(served.success);
  EXPECT_EQ(cache_body_, served.body);
  EXPECT_EQ(kOriginB, served.acao);
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
}

// While the response is held back for the optimized version, a CORS
// request is not given an optimized version built from a fetch whose
// headers do not allow its origin; it gets the response it was answered.
TEST_F(InPlaceCorsGuardTest, WaitForOptimizedKeepsAllowedResponse) {
  options()->set_in_place_wait_for_optimized(true);
  StoreJs("*", nullptr);
  ResponseHeaders without_acao;
  SetDefaultHeaders(kContentTypeJavascript.mime_type(), &without_acao);
  without_acao.SetDateAndCaching(start_time_ms(), ttl_ms_);
  mock_url_fetcher()->SetResponse(cache_js_url_, without_acao, cache_body_);
  Served served = Serve(cache_js_url_, kOriginA, false);
  EXPECT_TRUE(served.success);
  EXPECT_EQ(cache_body_, served.body);
  EXPECT_EQ("*", served.acao);
  EXPECT_EQ(0, js_filter_->num_rewrites());
}

// A file the module loads itself carries no CORS headers.
TEST_F(InPlaceCorsGuardTest, LoadFromFileDeclined) {
  options()->file_load_policy()->Associate("http://www.example.com", "/test/");
  WriteFile("/test/cacheable.js", cache_body_);
  Init();
  ResetHeadersAndStats();
  ExpectDeclined(Serve(cache_js_url_, kOriginA, false));
}

// The decline is decided before any response header goes out: the only
// headers committed are the not-in-cache answer, with no body, so no part
// of the optimized version's response reaches the server.
TEST_F(InPlaceCorsGuardTest, DeclineCommitsNoHeadersOfTheCopy) {
  StoreJs(nullptr, nullptr);
  OptimizeJs();
  Served served = Serve(cache_js_url_, kOriginA, false);
  ExpectDeclined(served);
  EXPECT_EQ(1, served.commits);
  EXPECT_EQ(CacheUrlAsyncFetcher::kNotInCacheStatus, served.status_at_commit);
  EXPECT_FALSE(served.content_type_at_commit);
  EXPECT_EQ(0, served.bytes_at_commit);
}

// Web fonts are always requested in CORS mode. A proxy serves a stored font
// to another origin when it allows every origin...
TEST_F(InPlaceCorsGuardTest, FontAllowingAnyOriginServedFromProxyCache) {
  const GoogleString font_url = "http://www.example.com/font.woff2";
  Store(font_url, "font/woff2", "*", nullptr, nullptr);
  Served served = Serve(font_url, kOriginA, true);
  EXPECT_TRUE(served.success);
  EXPECT_EQ(cache_body_, served.body);
  EXPECT_EQ("*", served.acao);
  EXPECT_EQ(0, served.declined);
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
}

// ...and fetches it for the visitor when the stored font names another
// origin, without storing the answer made for this one.  The stored font
// names origin A without a Vary, so the CORS check is what refuses it.
TEST_F(InPlaceCorsGuardTest, FontForAnotherOriginFetchedForTheVisitor) {
  const GoogleString font_url = "http://www.example.com/font.woff2";
  Store(font_url, "font/woff2", kOriginA, nullptr, nullptr);
  ResponseHeaders for_b;
  SetDefaultHeaders("font/woff2", &for_b);
  for_b.SetDateAndCaching(start_time_ms(), ttl_ms_);
  for_b.Add(HttpAttributes::kAccessControlAllowOrigin, kOriginB);
  for_b.Add(HttpAttributes::kVary, "Origin");
  mock_url_fetcher()->SetResponse(font_url, for_b, cache_body_);
  Served served = Serve(font_url, kOriginB, true);
  EXPECT_TRUE(served.success);
  EXPECT_EQ(cache_body_, served.body);
  EXPECT_EQ(kOriginB, served.acao);
  EXPECT_EQ(1, served.declined);
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
}

// A stored font that also says Vary: Origin is a miss for every visitor's
// request already (a stored answer to a visitor that varies beyond
// Accept-Encoding is never served), before the CORS check sees it: the font
// is fetched for the visitor all the same and the answer is not stored, but
// it is not counted as a CORS decline.
TEST_F(InPlaceCorsGuardTest, FontVaryingOnOriginFetchedForTheVisitor) {
  const GoogleString font_url = "http://www.example.com/font.woff2";
  Store(font_url, "font/woff2", kOriginA, "Origin", nullptr);
  ResponseHeaders for_b;
  SetDefaultHeaders("font/woff2", &for_b);
  for_b.SetDateAndCaching(start_time_ms(), ttl_ms_);
  for_b.Add(HttpAttributes::kAccessControlAllowOrigin, kOriginB);
  for_b.Add(HttpAttributes::kVary, "Origin");
  mock_url_fetcher()->SetResponse(font_url, for_b, cache_body_);
  Served served = Serve(font_url, kOriginB, true);
  EXPECT_TRUE(served.success);
  EXPECT_EQ(cache_body_, served.body);
  EXPECT_EQ(kOriginB, served.acao);
  EXPECT_EQ(0, served.declined);
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
}

// A credentialed request (it sends cookies) is refused "*" by the browser,
// so a copy allowing every origin is not served to it...
TEST_F(InPlaceCorsGuardTest, CredentialedRequestNotGivenWildcardCopy) {
  Store(cache_js_url_, kContentTypeJavascript.mime_type(), "*", nullptr,
        nullptr);
  Served served = Serve(cache_js_url_, kOriginA, true, "session=1");
  EXPECT_EQ(1, served.declined);
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
}

// ...but one naming its origin and allowing credentials is.
TEST_F(InPlaceCorsGuardTest, CredentialedRequestGivenCopyAllowingIt) {
  Store(cache_js_url_, kContentTypeJavascript.mime_type(), kOriginA, nullptr,
        "true");
  Served served = Serve(cache_js_url_, kOriginA, true, "session=1");
  EXPECT_TRUE(served.success);
  EXPECT_EQ(cache_body_, served.body);
  EXPECT_EQ(kOriginA, served.acao);
  EXPECT_EQ(0, served.declined);
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
}

// With InPlaceCorsGuard off, in-place serving ignores Origin, as before.
TEST_F(InPlaceCorsGuardTest, GuardOffServesOptimizedCopy) {
  options()->set_in_place_cors_guard(false);
  StoreJs(nullptr, nullptr);
  OptimizeJs();
  Served served = Serve(cache_js_url_, kOriginA, false);
  EXPECT_TRUE(served.success);
  EXPECT_EQ("good:jm", served.body);
  EXPECT_EQ(0, served.declined);
}

}  // namespace

// In-place optimization does not store its result under the resource's own
// URL.  The optimized bytes are written by a nested rewrite of the type's own
// filter, as that filter's ordinary output: an image under the image filter's
// id, a stylesheet under the CSS filter's, a script under the JavaScript
// minifier's.  That hidden output is what a later request for the original
// URL is answered from, so anything recorded with a filter's output when it
// is written is also there for in-place serving, keyed by the same filter id.
TEST_F(InPlaceRewriteContextTest,
       OptimizedBytesAreStoredAsTheTypesFilterOutput) {
  Init();
  const struct {
    const GoogleString* url;
    const char* hidden_output_url;
    const char* filter_id;
    const char* content_type;
    const char* first_serve_etag;
  } kCases[] = {
      {&cache_png_url_,
       "http://www.example.com/cacheable.png.pagespeed.ic.0.png",
       RewriteOptions::kImageCompressionId, kContentTypePng.mime_type(),
       original_etag_},
      {&cache_css_url_,
       "http://www.example.com/cacheable.css.pagespeed.cf.0.css",
       RewriteOptions::kCssFilterId, kContentTypeCss.mime_type(), nullptr},
      {&cache_js_url_, "http://www.example.com/cacheable.js.pagespeed.jm.0.js",
       RewriteOptions::kJavascriptMinId, kContentTypeJavascript.mime_type(),
       nullptr},
  };
  for (const auto& c : kCases) {
    SCOPED_TRACE(c.hidden_output_url);
    // Nothing is stored for the filter's output before the first request.
    EXPECT_EQ(HTTPCache::kNotFound,
              HttpBlockingFindStatus(c.hidden_output_url, http_cache()).status);

    // The first request streams the original bytes and optimizes behind it.
    FetchAndCheckResponse(*c.url, cache_body_, true, kInjectedSMaxAgeMs,
                          c.first_serve_etag, start_time_ms());

    HTTPValue value;
    ResponseHeaders headers;
    ASSERT_EQ(
        HTTPCache::kFound,
        HttpBlockingFind(c.hidden_output_url, http_cache(), &value, &headers)
            .status);
    StringPiece contents;
    ASSERT_TRUE(value.ExtractContents(&contents));
    EXPECT_EQ(StrCat(cache_body_, ":", c.filter_id), contents);
    EXPECT_STREQ(c.content_type, headers.Lookup1(HttpAttributes::kContentType));
    EXPECT_FALSE(headers.IsGzipped());
    // The stored headers carry the size of the input as the original length.
    EXPECT_STREQ(IntegerToString(static_cast<int>(cache_body_.size())),
                 headers.Lookup1(HttpAttributes::kXOriginalContentLength));
    // It is an ordinary output entry, not a recorded input.
    EXPECT_FALSE(value.IsFetchedInput());
    // It carries the size record of that filter's output: the size of the
    // input, the size of the optimized body and the filter's id.
    ResponseHeaders::SizeRecord record;
    ASSERT_TRUE(headers.GetSizeRecord(&record));
    EXPECT_EQ(static_cast<int64>(cache_body_.size()), record.original_size);
    EXPECT_EQ(static_cast<int64>(cache_body_.size() + 1 + strlen(c.filter_id)),
              record.optimized_size);
    EXPECT_STREQ(c.filter_id, record.filter_id);
    // The response that streamed the original carried none.
    EXPECT_FALSE(response_headers_.HasSizeRecord());

    // The second request for the original URL is answered with those bytes.
    ResetHeadersAndStats();
    FetchAndCheckResponse(*c.url, StrCat(cache_body_, ":", c.filter_id), true,
                          ttl_ms_, etag_, start_time_ms());
    // And the optimized response served for the original URL carries none.
    EXPECT_FALSE(response_headers_.HasSizeRecord());
    ResetHeadersAndStats();
  }
}

// With the first request held until the optimized bytes are ready, the
// response is built from the nested filter's output.  That output's size
// record goes into the stored entry and stays out of the response, on the
// first request and on the ones answered from the cache after it.
TEST_F(InPlaceRewriteContextTest, WaitForOptimizedResponseHasNoSizeRecord) {
  options()->set_in_place_wait_for_optimized(true);
  Init();

  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_FALSE(response_headers_.HasSizeRecord());

  HTTPValue value;
  ResponseHeaders stored;
  ASSERT_EQ(HTTPCache::kFound,
            HttpBlockingFind(rewritten_jpg_url_, http_cache(), &value, &stored)
                .status);
  ResponseHeaders::SizeRecord record;
  ASSERT_TRUE(stored.GetSizeRecord(&record));
  EXPECT_EQ(4, record.original_size);   // "good"
  EXPECT_EQ(7, record.optimized_size);  // "good:ic"
  EXPECT_STREQ(RewriteOptions::kImageCompressionId, record.filter_id);

  ResetHeadersAndStats();
  FetchAndCheckResponse(cache_jpg_url_, "good:ic", true, ttl_ms_, etag_,
                        start_time_ms());
  EXPECT_FALSE(response_headers_.HasSizeRecord());
}

// ============================================================================
// Sizes of optimized responses sent (the *_served_*_bytes statistics), for
// fetches of the rewritten URL of each type.
// ============================================================================

class ServedSavingsFetchTest : public InPlaceRewriteContextTest {
 protected:
  struct Sizes {
    int64 image_original, image_optimized;
    int64 css_original, css_optimized;
    int64 js_original, js_optimized;
  };

  Sizes Served() {
    RewriteStats* stats = server_context()->rewrite_stats();
    return Sizes{stats->image_served_original_bytes()->Get(),
                 stats->image_served_optimized_bytes()->Get(),
                 stats->css_served_original_bytes()->Get(),
                 stats->css_served_optimized_bytes()->Get(),
                 stats->javascript_served_original_bytes()->Get(),
                 stats->javascript_served_optimized_bytes()->Get()};
  }

  void ExpectServed(const Sizes& expected) {
    const Sizes actual = Served();
    EXPECT_EQ(expected.image_original, actual.image_original);
    EXPECT_EQ(expected.image_optimized, actual.image_optimized);
    EXPECT_EQ(expected.css_original, actual.css_original);
    EXPECT_EQ(expected.css_optimized, actual.css_optimized);
    EXPECT_EQ(expected.js_original, actual.js_original);
    EXPECT_EQ(expected.js_optimized, actual.js_optimized);
  }

  // Fetches 'url' with 'request_headers' and returns the body.
  GoogleString Fetch(const GoogleString& url, RequestHeaders* request_headers) {
    GoogleString contents;
    ResponseHeaders response_headers;
    EXPECT_TRUE(
        FetchResourceUrl(url, request_headers, &contents, &response_headers))
        << url;
    return contents;
  }

  GoogleString Fetch(const GoogleString& url) {
    RequestHeaders request_headers;
    return Fetch(url, &request_headers);
  }

  int64 CachedFetches() {
    return server_context()->rewrite_stats()->cached_resource_fetches()->Get();
  }

  const GoogleString rewritten_png_url_ =
      "http://www.example.com/cacheable.png.pagespeed.ic.0.png";
  const GoogleString rewritten_js_url_ =
      "http://www.example.com/cacheable.js.pagespeed.jm.0.js";
};

// A rewritten URL built on request and then answered from the cache: each
// response adds the size of the input and the size of the optimized body to
// its own type, and to no other.  The sizes are "good" (4) and "good:xx" (7).
TEST_F(ServedSavingsFetchTest, EachOptimizedResponseAddsItsSizes) {
  Init();
  // Built on request: nothing is in the cache for it yet.
  EXPECT_STREQ("good:ic", Fetch(rewritten_png_url_));
  EXPECT_EQ(0, CachedFetches());
  ExpectServed({4, 7, 0, 0, 0, 0});
  // From the cache, twice.
  EXPECT_STREQ("good:ic", Fetch(rewritten_png_url_));
  EXPECT_STREQ("good:ic", Fetch(rewritten_png_url_));
  EXPECT_EQ(2, CachedFetches());
  ExpectServed({12, 21, 0, 0, 0, 0});

  EXPECT_STREQ("good:cf", Fetch(rewritten_css_url_));
  EXPECT_STREQ("good:cf", Fetch(rewritten_css_url_));
  EXPECT_EQ(3, CachedFetches());
  ExpectServed({12, 21, 8, 14, 0, 0});

  EXPECT_STREQ("good:jm", Fetch(rewritten_js_url_));
  EXPECT_STREQ("good:jm", Fetch(rewritten_js_url_));
  EXPECT_EQ(4, CachedFetches());
  ExpectServed({12, 21, 8, 14, 8, 14});

  // The totals the filters keep per rewrite are another matter: each filter
  // ran once however often its output was sent.
  EXPECT_EQ(1, img_filter_->num_rewrites());
  EXPECT_EQ(1, css_filter_->num_rewrites());
  EXPECT_EQ(1, js_filter_->num_rewrites());
}

// Requests that are answered from the cache without a whole body going to a
// visitor add nothing.
TEST_F(ServedSavingsFetchTest, RequestsThatDoNotCount) {
  Init();
  EXPECT_STREQ("good:ic", Fetch(rewritten_png_url_));
  ExpectServed({4, 7, 0, 0, 0, 0});

  RequestHeaders head;
  head.set_method(RequestHeaders::kHead);
  EXPECT_STREQ("", Fetch(rewritten_png_url_, &head));
  ExpectServed({4, 7, 0, 0, 0, 0});

  RequestHeaders ranged;
  ranged.Add("Range", "bytes=0-2");
  Fetch(rewritten_png_url_, &ranged);
  ExpectServed({4, 7, 0, 0, 0, 0});

  RequestHeaders module_fetch;
  module_fetch.Add(
      HttpAttributes::kUserAgent,
      StrCat("Fetcher (", kModPagespeedSubrequestUserAgent, "/2.0-0)"));
  EXPECT_STREQ("good:ic", Fetch(rewritten_png_url_, &module_fetch));
  ExpectServed({4, 7, 0, 0, 0, 0});

  // A conditional request is answered with a 304 and no body.
  RequestHeaders conditional;
  conditional.Add(HttpAttributes::kIfModifiedSince,
                  "Fri, 01 Jan 2016 00:00:00 GMT");
  GoogleString contents;
  ResponseHeaders response_headers;
  EXPECT_TRUE(FetchResourceUrl(rewritten_png_url_, &conditional, &contents,
                               &response_headers));
  EXPECT_EQ(HttpStatus::kNotModified, response_headers.status_code());
  EXPECT_STREQ("", contents);
  ExpectServed({4, 7, 0, 0, 0, 0});

  // An ordinary request still counts afterwards.
  EXPECT_STREQ("good:ic", Fetch(rewritten_png_url_));
  ExpectServed({8, 14, 0, 0, 0, 0});
}

// An entry without a size record is served as it is and not counted, an
// image with an X-Original-Content-Length header included.  The cache was
// emptied when the record was introduced, so such an entry is an output
// written without knowing the size of what it was made from, or a fault.
TEST_F(ServedSavingsFetchTest, EntriesWithoutASizeRecordAreNotCounted) {
  Init();
  // Store each output, then store it again without its record.
  for (const GoogleString* url :
       {&rewritten_png_url_, &rewritten_css_url_, &rewritten_js_url_}) {
    Fetch(*url);
    HTTPValue value;
    ResponseHeaders stored;
    ASSERT_EQ(HTTPCache::kFound,
              HttpBlockingFind(*url, http_cache(), &value, &stored).status);
    ASSERT_TRUE(stored.HasSizeRecord());
    ASSERT_TRUE(stored.Has(HttpAttributes::kXOriginalContentLength));
    stored.ClearSizeRecord();
    StringPiece contents;
    ASSERT_TRUE(value.ExtractContents(&contents));
    GoogleString body = contents.as_string();
    http_cache()->Put(*url, rewrite_driver()->CacheFragment(),
                      RequestHeaders::Properties(),
                      ResponseHeaders::GetVaryOption(options()->respect_vary()),
                      &stored, body, message_handler());
  }
  ClearStats();

  EXPECT_STREQ("good:ic", Fetch(rewritten_png_url_));
  EXPECT_STREQ("good:cf", Fetch(rewritten_css_url_));
  EXPECT_STREQ("good:jm", Fetch(rewritten_js_url_));
  EXPECT_EQ(3, CachedFetches());
  RewriteStats* stats = server_context()->rewrite_stats();
  EXPECT_EQ(0, stats->image_served_original_bytes()->Get());
  EXPECT_EQ(0, stats->image_served_optimized_bytes()->Get());
  EXPECT_EQ(0, stats->css_served_original_bytes()->Get());
  EXPECT_EQ(0, stats->css_served_optimized_bytes()->Get());
  EXPECT_EQ(0, stats->javascript_served_original_bytes()->Get());
  EXPECT_EQ(0, stats->javascript_served_optimized_bytes()->Get());
  EXPECT_TRUE(statistics()->FindVariable(
                  "served_savings_unmeasured_responses") == nullptr);
}

// ============================================================================
// The same statistics for responses that leave through the fallback path:
// resources optimized in place under their own URL, and rewritten URLs
// answered with an output found under another hash.  What is sent decides,
// per response: optimized bytes count, the original bytes do not.
// ============================================================================

class ServedSavingsFallbackTest : public ServedSavingsFetchTest {
 protected:
  // Fetches 'url' the way a server does for a request it hands over, with
  // 'request_headers' when given, and returns the body.  With 'past_deadline'
  // the filters finish only after the rewrite deadline has passed.
  GoogleString FetchThroughDriver(const GoogleString& url,
                                  const RequestHeaders* request_headers,
                                  bool past_deadline) {
    js_filter_->set_exceed_deadline(past_deadline);
    img_filter_->set_exceed_deadline(past_deadline);
    css_filter_->set_exceed_deadline(past_deadline);
    response_headers_.Clear();
    WorkerTestBase::SyncPoint sync(server_context()->thread_system());
    RequestContextPtr request_context(RequestContext::NewTestRequestContext(
        server_context()->thread_system()));
    NotifyingFetch fetch(request_context, options(), url, &sync,
                         &response_headers_);
    if (request_headers != nullptr) {
      fetch.request_headers()->CopyFrom(*request_headers);
    }
    rewrite_driver()->FetchResource(url, &fetch);
    if (past_deadline) {
      rewrite_driver()->BoundedWaitFor(RewriteDriver::kWaitForCompletion,
                                       rewrite_driver()->rewrite_deadline_ms());
    }
    sync.Wait();
    rewrite_driver()->WaitForShutDown();
    mock_scheduler()->AwaitQuiescence();  // Lets cache writes finish.
    EXPECT_TRUE(fetch.done()) << url;
    GoogleString body = fetch.content().as_string();
    ClearRewriteDriver();
    return body;
  }

  GoogleString InPlace(const GoogleString& url) {
    return FetchThroughDriver(url, nullptr, false);
  }

  const GoogleString wrong_hash_png_url_ =
      "http://www.example.com/cacheable.png.pagespeed.ic.1.png";
};

// A resource requested under its own URL: the first response streams the
// original and adds nothing; once the optimized bytes are in the cache every
// response adds the size of the original and the size of what was sent, to
// its own type and to no other.
TEST_F(ServedSavingsFallbackTest, InPlaceResponsesAddTheirSizes) {
  Init();
  EXPECT_STREQ("good", InPlace(cache_png_url_));
  ExpectServed({0, 0, 0, 0, 0, 0});
  EXPECT_STREQ("good:ic", InPlace(cache_png_url_));
  ExpectServed({4, 7, 0, 0, 0, 0});
  EXPECT_STREQ("good:ic", InPlace(cache_png_url_));
  ExpectServed({8, 14, 0, 0, 0, 0});

  EXPECT_STREQ("good", InPlace(cache_css_url_));
  ExpectServed({8, 14, 0, 0, 0, 0});
  EXPECT_STREQ("good:cf", InPlace(cache_css_url_));
  EXPECT_STREQ("good:cf", InPlace(cache_css_url_));
  ExpectServed({8, 14, 8, 14, 0, 0});

  EXPECT_STREQ("good", InPlace(cache_js_url_));
  ExpectServed({8, 14, 8, 14, 0, 0});
  EXPECT_STREQ("good:jm", InPlace(cache_js_url_));
  EXPECT_STREQ("good:jm", InPlace(cache_js_url_));
  ExpectServed({8, 14, 8, 14, 8, 14});

  // None of these was a cache hit on a rewritten URL, and each filter ran
  // once.
  EXPECT_EQ(0, CachedFetches());
  EXPECT_EQ(1, img_filter_->num_rewrites());
  EXPECT_EQ(1, css_filter_->num_rewrites());
  EXPECT_EQ(1, js_filter_->num_rewrites());
}

// When the first request waits for the optimized bytes, the response is built
// from the output that in-place optimization makes out of the nested filter's
// output.  That output takes over the nested size record, so the very first
// response counts, with the sizes the nested filter recorded; so do the ones
// answered from the cache after it.
TEST_F(ServedSavingsFallbackTest, WaitForOptimizedResponseAddsItsSizes) {
  options()->set_in_place_wait_for_optimized(true);
  Init();
  EXPECT_STREQ("good:ic", InPlace(cache_jpg_url_));
  ExpectServed({4, 7, 0, 0, 0, 0});
  EXPECT_STREQ("good:ic", InPlace(cache_jpg_url_));
  ExpectServed({8, 14, 0, 0, 0, 0});

  EXPECT_STREQ("good:cf", InPlace(cache_css_url_));
  ExpectServed({8, 14, 4, 7, 0, 0});
  EXPECT_STREQ("good:jm", InPlace(cache_js_url_));
  ExpectServed({8, 14, 4, 7, 4, 7});
}

// A request that waits for the optimized bytes and runs into the rewrite
// deadline is answered with the original, which adds nothing.  The rewrite
// finishes behind it, and the next response is the optimized one.
TEST_F(ServedSavingsFallbackTest, WaitForOptimizedPastDeadlineSendsOriginal) {
  options()->set_in_place_wait_for_optimized(true);
  Init();
  EXPECT_STREQ("good", FetchThroughDriver(cache_jpg_url_, nullptr, true));
  ExpectServed({0, 0, 0, 0, 0, 0});
  EXPECT_EQ(1, img_filter_->num_rewrites());
  EXPECT_STREQ("good:ic", InPlace(cache_jpg_url_));
  ExpectServed({4, 7, 0, 0, 0, 0});
}

// A resource the filter does not improve is served as it is, on the first
// request and from the cache afterwards, waiting for the optimized bytes or
// not: nothing is added.
TEST_F(ServedSavingsFallbackTest, InPlaceOriginalIsNotCounted) {
  Init();
  img_filter_->set_enabled(false);
  EXPECT_STREQ("good", InPlace(cache_png_url_));
  EXPECT_STREQ("good", InPlace(cache_png_url_));
  ExpectServed({0, 0, 0, 0, 0, 0});
}

TEST_F(ServedSavingsFallbackTest, WaitForOptimizedOriginalIsNotCounted) {
  options()->set_in_place_wait_for_optimized(true);
  Init();
  img_filter_->set_enabled(false);
  EXPECT_STREQ("good", InPlace(cache_jpg_url_));
  EXPECT_STREQ("good", InPlace(cache_jpg_url_));
  ExpectServed({0, 0, 0, 0, 0, 0});
}

// Requests for a resource optimized in place that do not send a whole body
// to a visitor add nothing.
TEST_F(ServedSavingsFallbackTest, InPlaceRequestsThatDoNotCount) {
  Init();
  EXPECT_STREQ("good", InPlace(cache_png_url_));
  EXPECT_STREQ("good:ic", InPlace(cache_png_url_));
  ExpectServed({4, 7, 0, 0, 0, 0});

  RequestHeaders head;
  head.set_method(RequestHeaders::kHead);
  EXPECT_STREQ("", FetchThroughDriver(cache_png_url_, &head, false));
  ExpectServed({4, 7, 0, 0, 0, 0});

  RequestHeaders ranged;
  ranged.Add("Range", "bytes=0-2");
  FetchThroughDriver(cache_png_url_, &ranged, false);
  ExpectServed({4, 7, 0, 0, 0, 0});

  RequestHeaders module_fetch;
  module_fetch.Add(
      HttpAttributes::kUserAgent,
      StrCat("Fetcher (", kModPagespeedSubrequestUserAgent, "/2.0-0)"));
  FetchThroughDriver(cache_png_url_, &module_fetch, false);
  ExpectServed({4, 7, 0, 0, 0, 0});

  // A conditional request naming the optimized response is answered with a
  // 304 and no body.
  RequestHeaders conditional;
  conditional.Add(HttpAttributes::kIfNoneMatch, etag_);
  EXPECT_STREQ("", FetchThroughDriver(cache_png_url_, &conditional, false));
  EXPECT_EQ(HttpStatus::kNotModified, response_headers_.status_code());
  ExpectServed({4, 7, 0, 0, 0, 0});

  // An ordinary request still counts afterwards.
  EXPECT_STREQ("good:ic", InPlace(cache_png_url_));
  ExpectServed({8, 14, 0, 0, 0, 0});
}

// A rewritten URL naming a hash that is not the one the rewrite produces is
// answered with the optimized bytes: built on request when nothing is known
// yet, and from the output stored under the right hash afterwards.  Both
// responses count, and neither is a cache hit on the requested URL.
TEST_F(ServedSavingsFallbackTest, OtherHashResponsesAddTheirSizes) {
  Init();
  // Built on request.
  EXPECT_STREQ("good:ic", Fetch(wrong_hash_png_url_));
  EXPECT_EQ(1, img_filter_->num_rewrites());
  ExpectServed({4, 7, 0, 0, 0, 0});
  // From the output stored under the right hash.
  EXPECT_STREQ("good:ic", Fetch(wrong_hash_png_url_));
  EXPECT_STREQ("good:ic", Fetch(wrong_hash_png_url_));
  EXPECT_EQ(1, img_filter_->num_rewrites());
  ExpectServed({12, 21, 0, 0, 0, 0});
  EXPECT_EQ(0, CachedFetches());

  // The requests that do not count on other paths do not count here.
  RequestHeaders head;
  head.set_method(RequestHeaders::kHead);
  EXPECT_STREQ("", Fetch(wrong_hash_png_url_, &head));
  RequestHeaders ranged;
  ranged.Add("Range", "bytes=0-2");
  Fetch(wrong_hash_png_url_, &ranged);
  RequestHeaders module_fetch;
  module_fetch.Add(
      HttpAttributes::kUserAgent,
      StrCat("Fetcher (", kModPagespeedSubrequestUserAgent, "/2.0-0)"));
  EXPECT_STREQ("good:ic", Fetch(wrong_hash_png_url_, &module_fetch));
  ExpectServed({12, 21, 0, 0, 0, 0});
}

// A fetch of a rewritten URL whose rewrite does not finish before the
// deadline is answered with the original, which adds nothing; the same URL
// fetched again is answered with the optimized bytes from the cache.
TEST_F(ServedSavingsFallbackTest, PastDeadlineSendsOriginalUncounted) {
  Init();
  EXPECT_STREQ("good", FetchThroughDriver(rewritten_png_url_, nullptr, true));
  ExpectServed({0, 0, 0, 0, 0, 0});
  EXPECT_STREQ("good:ic", Fetch(rewritten_png_url_));
  EXPECT_EQ(1, CachedFetches());
  ExpectServed({4, 7, 0, 0, 0, 0});
}

// A rewritten URL whose rewrite fails is answered with the original, when it
// is first asked for and from the cache once the failure is remembered:
// nothing is added.
TEST_F(ServedSavingsFallbackTest, FailedRewriteSendsOriginalUncounted) {
  Init();
  img_filter_->set_enabled(false);
  EXPECT_STREQ("good", Fetch(rewritten_png_url_));
  EXPECT_STREQ("good", Fetch(rewritten_png_url_));
  ExpectServed({0, 0, 0, 0, 0, 0});
}

}  // namespace net_instaweb
