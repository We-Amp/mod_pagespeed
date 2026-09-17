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

// This file contains the filter-registration and filter-chain-building
// methods of RewriteDriver.  They are separated from rewrite_driver.cc
// so that targets needing only the core driver infrastructure can avoid
// pulling in every filter implementation and their heavy dependencies
// (image codecs, CSS parser, JS minifier, etc.).

#include <memory>

#include "base/logging.h"
#include "net/instaweb/rewriter/public/add_head_filter.h"
#include "net/instaweb/rewriter/public/add_ids_filter.h"
#include "net/instaweb/rewriter/public/add_instrumentation_filter.h"
#include "net/instaweb/rewriter/public/agent_optimize_vary_filter.h"
#include "net/instaweb/rewriter/public/base_tag_filter.h"
#include "net/instaweb/rewriter/public/cache_extender.h"
#include "net/instaweb/rewriter/public/collect_dependencies_filter.h"
#include "net/instaweb/rewriter/public/critical_css_beacon_filter.h"
#include "net/instaweb/rewriter/public/critical_images_beacon_filter.h"
#include "net/instaweb/rewriter/public/critical_selector_filter.h"
#include "net/instaweb/rewriter/public/css_combine_filter.h"
#include "net/instaweb/rewriter/public/css_filter.h"
#include "net/instaweb/rewriter/public/css_inline_filter.h"
#include "net/instaweb/rewriter/public/css_inline_import_to_link_filter.h"
#include "net/instaweb/rewriter/public/css_move_to_head_filter.h"
#include "net/instaweb/rewriter/public/css_outline_filter.h"
#include "net/instaweb/rewriter/public/css_summarizer_base.h"
#include "net/instaweb/rewriter/public/debug_filter.h"
#include "net/instaweb/rewriter/public/decode_rewritten_urls_filter.h"
#include "net/instaweb/rewriter/public/dedup_inlined_images_filter.h"
#include "net/instaweb/rewriter/public/defer_iframe_filter.h"
#include "net/instaweb/rewriter/public/delay_images_filter.h"
#include "net/instaweb/rewriter/public/deterministic_js_filter.h"
#include "net/instaweb/rewriter/public/dom_stats_filter.h"
#include "net/instaweb/rewriter/public/domain_rewrite_filter.h"
#include "net/instaweb/rewriter/public/fix_reflow_filter.h"
#include "net/instaweb/rewriter/public/flush_html_filter.h"
#include "net/instaweb/rewriter/public/google_font_css_inline_filter.h"
#include "net/instaweb/rewriter/public/handle_noscript_redirect_filter.h"
#include "net/instaweb/rewriter/public/image_combine_filter.h"
#include "net/instaweb/rewriter/public/image_rewrite_filter.h"
#include "net/instaweb/rewriter/public/in_place_rewrite_context.h"
#include "net/instaweb/rewriter/public/insert_amp_link_filter.h"
#include "net/instaweb/rewriter/public/insert_dns_prefetch_filter.h"
#include "net/instaweb/rewriter/public/insert_ga_filter.h"
#include "net/instaweb/rewriter/public/insert_speculation_rules_filter.h"
#include "net/instaweb/rewriter/public/javascript_filter.h"
#include "net/instaweb/rewriter/public/js_combine_filter.h"
#include "net/instaweb/rewriter/public/js_defer_disabled_filter.h"
#include "net/instaweb/rewriter/public/js_disable_filter.h"
#include "net/instaweb/rewriter/public/js_inline_filter.h"
#include "net/instaweb/rewriter/public/js_outline_filter.h"
#include "net/instaweb/rewriter/public/lazyload_images_filter.h"
#include "net/instaweb/rewriter/public/local_storage_cache_filter.h"
#include "net/instaweb/rewriter/public/make_show_ads_async_filter.h"
#include "net/instaweb/rewriter/public/meta_tag_filter.h"
#include "net/instaweb/rewriter/public/pedantic_filter.h"
#include "net/instaweb/rewriter/public/prioritize_critical_images_filter.h"
#include "net/instaweb/rewriter/public/push_preload_filter.h"
#include "net/instaweb/rewriter/public/redirect_on_size_limit_filter.h"
#include "net/instaweb/rewriter/public/responsive_image_filter.h"
#include "net/instaweb/rewriter/public/rewrite_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/strip_scripts_filter.h"
#include "net/instaweb/rewriter/public/strip_subresource_hints_filter.h"
#include "net/instaweb/rewriter/public/support_noscript_filter.h"
#include "net/instaweb/rewriter/public/url_input_resource.h"
#include "net/instaweb/rewriter/public/url_left_trim_filter.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/callback.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/amp_document_filter.h"
#include "pagespeed/kernel/html/collapse_whitespace_filter.h"
#include "pagespeed/kernel/html/elide_attributes_filter.h"
#include "pagespeed/kernel/html/html_attribute_quote_removal.h"
#include "pagespeed/kernel/html/remove_comments_filter.h"

namespace net_instaweb {

namespace {

// Implementation of RemoveCommentsFilter::OptionsInterface that wraps
// a RewriteOptions instance.
class RemoveCommentsFilterOptions
    : public RemoveCommentsFilter::OptionsInterface {
 public:
  explicit RemoveCommentsFilterOptions(const RewriteOptions* options)
      : options_(options) {}

  bool IsRetainedComment(const StringPiece& comment) const override {
    return options_->IsRetainedComment(comment);
  }

 private:
  const RewriteOptions* options_;

  RemoveCommentsFilterOptions(const RemoveCommentsFilterOptions&) = delete;
  RemoveCommentsFilterOptions& operator=(const RemoveCommentsFilterOptions&) =
      delete;
};

}  // namespace

void RewriteDriver::Initialize() {
  ++initialized_count_;
  if (initialized_count_ == 1) {
    RewriteOptions::Initialize();
    ImageRewriteFilter::Initialize();
    CssFilter::Initialize();
  }
}

void RewriteDriver::InitStats(Statistics* statistics) {
  AddInstrumentationFilter::InitStats(statistics);
  CacheExtender::InitStats(statistics);
  CriticalCssBeaconFilter::InitStats(statistics);
  CriticalImagesBeaconFilter::InitStats(statistics);
  CssCombineFilter::InitStats(statistics);
  CssFilter::InitStats(statistics);
  CssInlineFilter::InitStats(statistics);
  CssInlineImportToLinkFilter::InitStats(statistics);
  CssMoveToHeadFilter::InitStats(statistics);
  CssSummarizerBase::InitStats(statistics);
  DedupInlinedImagesFilter::InitStats(statistics);
  DomainRewriteFilter::InitStats(statistics);
  GoogleFontCssInlineFilter::InitStats(statistics);
  ImageCombineFilter::InitStats(statistics);
  ImageRewriteFilter::InitStats(statistics);
  InPlaceRewriteContext::InitStats(statistics);
  InsertGAFilter::InitStats(statistics);
  JavascriptFilter::InitStats(statistics);
  JsCombineFilter::InitStats(statistics);
  JsInlineFilter::InitStats(statistics);
  LazyloadImagesFilter::InitStats(statistics);
  LocalStorageCacheFilter::InitStats(statistics);
  MakeShowAdsAsyncFilter::InitStats(statistics);
  MetaTagFilter::InitStats(statistics);
  PrioritizeCriticalImagesFilter::InitStats(statistics);
  RewriteContext::InitStats(statistics);
  UrlInputResource::InitStats(statistics);
  UrlLeftTrimFilter::InitStats(statistics);
}

void RewriteDriver::Terminate() {
  // Clean up statics.
  --initialized_count_;
  if (initialized_count_ == 0) {
    CssFilter::Terminate();
    ImageRewriteFilter::Terminate();
    RewriteOptions::Terminate();
  }
}

void RewriteDriver::RegisterBuiltinRewriteFilters() {
  DCHECK(resource_filter_map_.empty());

  // Add the rewriting filters to the map unconditionally -- we may
  // need them to process resource requests due to a query-specific
  // 'rewriters' specification.  We still use the passed-in options
  // to determine whether they get added to the html parse filter chain.
  // Note: RegisterRewriteFilter takes ownership of these filters.
  CacheExtender* cache_extender = new CacheExtender(this);
  ImageCombineFilter* image_combiner = new ImageCombineFilter(this);
  ImageRewriteFilter* image_rewriter = new ImageRewriteFilter(this);

  RegisterRewriteFilter(new CssCombineFilter(this));
  RegisterRewriteFilter(
      new CssFilter(this, cache_extender, image_rewriter, image_combiner));
  RegisterRewriteFilter(new JavascriptFilter(this));
  RegisterRewriteFilter(new JsCombineFilter(this));
  RegisterRewriteFilter(image_rewriter);
  RegisterRewriteFilter(cache_extender);
  RegisterRewriteFilter(image_combiner);
  RegisterRewriteFilter(new LocalStorageCacheFilter(this));
  RegisterRewriteFilter(new JavascriptSourceMapFilter(this));

  // These filters are needed to rewrite and trim urls in modified CSS files.
  domain_rewriter_ = std::make_unique<DomainRewriteFilter>(this, statistics());
  url_trim_filter_ = std::make_unique<UrlLeftTrimFilter>(this, statistics());
}

void RewriteDriver::AddFilters() {
  CHECK(html_writer_filter_ == nullptr);
  CHECK(!filters_added_);
  server_context_->ComputeSignature(options_.get());
  filters_added_ = true;

  AddPreRenderFilters();
  AddPostRenderFilters();
}

void RewriteDriver::AddPreRenderFilters() {
  // This function defines the order that filters are run.  We document
  // in pagespeed.conf.template that the order specified in the conf
  // file does not matter, but we give the filters there in the order
  // they are actually applied, for the benefit of the understanding
  // of the site owner.  So if you change that here, change it in
  // install/common/pagespeed.conf.template as well.
  //
  // Also be sure to update the doc in net/instaweb/doc/docs/config_filters.ezt.
  //
  // Now process boolean options, which may include propagating non-boolean
  // and boolean parameter settings to filters.
  const RewriteOptions* rewrite_options = options();

  if (rewrite_options->flush_html()) {
    // Note that this does not get hooked into the normal html-parse
    // filter-chain as it gets run immediately after every call to
    // ParseText, possibly inducing the system to trigger a Flush
    // based on the content it sees.
    add_event_listener(new FlushHtmlFilter(this));
  }
  add_event_listener(new AmpDocumentFilter(
      this, NewPermanentCallback(this, &RewriteDriver::SetIsAmpDocument)));

  if (rewrite_options->Enabled(RewriteOptions::kComputeStatistics)) {
    dom_stats_filter_ = new DomStatsFilter(this);
    AddOwnedEarlyPreRenderFilter(dom_stats_filter_);
  }
  if (!rewrite_options->preserve_subresource_hints()) {
    AddOwnedEarlyPreRenderFilter(new StripSubresourceHintsFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kDecodeRewrittenUrls)) {
    AddOwnedEarlyPreRenderFilter(new DecodeRewrittenUrlsFilter(this));
  }

  if (rewrite_options->Enabled(RewriteOptions::kResponsiveImages) &&
      rewrite_options->Enabled(RewriteOptions::kResizeImages)) {
    ResponsiveImageFirstFilter* resp_filter1 =
        new ResponsiveImageFirstFilter(this);
    AddOwnedEarlyPreRenderFilter(resp_filter1);

    ResponsiveImageSecondFilter* resp_filter2 =
        new ResponsiveImageSecondFilter(this, resp_filter1);
    AddOwnedPostRenderFilter(resp_filter2);
  }

  if (rewrite_options->RequiresAddHead()) {
    // Adds a filter that adds a 'head' section to html documents if
    // none found prior to the body.
    AddOwnedEarlyPreRenderFilter(new AddHeadFilter(
        this, rewrite_options->Enabled(RewriteOptions::kCombineHeads)));
  }
  if (rewrite_options->Enabled(RewriteOptions::kAddBaseTag)) {
    AddOwnedEarlyPreRenderFilter(new BaseTagFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kAddIds)) {
    AddOwnedEarlyPreRenderFilter(new AddIdsFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kStripScripts)) {
    // Experimental filter that blindly strips all scripts from a page.
    AppendOwnedPreRenderFilter(new StripScriptsFilter(this));
  }
  if (is_critical_images_beacon_enabled()) {
    // This filter should be enabled early, at least before image rewriting,
    // because it depends on seeing the original image URLs.
    AppendOwnedPreRenderFilter(new CriticalImagesBeaconFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kPrioritizeCriticalImages)) {
    // Runs before image rewriting so it sees the original image URLs the
    // beacon criticality data is keyed on.
    AppendOwnedPreRenderFilter(new PrioritizeCriticalImagesFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kMakeShowAdsAsync)) {
    // We want this filter early in case we ever inline the loader JS.
    AppendOwnedPreRenderFilter(new MakeShowAdsAsyncFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kInlineImportToLink) ||
      (!rewrite_options->Forbidden(RewriteOptions::kInlineImportToLink) &&
       (rewrite_options->Enabled(RewriteOptions::kPrioritizeCriticalCss) ||
        rewrite_options->Enabled(RewriteOptions::kComputeCriticalCss)))) {
    // If we're converting simple embedded CSS @imports into a href link
    // then we need to do that before any other CSS processing.
    AppendOwnedPreRenderFilter(
        new CssInlineImportToLinkFilter(this, statistics()));
  }
  if (!rewrite_options->Enabled(RewriteOptions::kPrioritizeCriticalCss) &&
      // If we're inlining styles that resolved initially, skip outlining
      // css since that works against this.
      rewrite_options->Enabled(RewriteOptions::kOutlineCss)) {
    // Cut out inlined styles and make them into external resources.
    // This can only be called once and requires a server_context_ to be set.
    CHECK(server_context_ != nullptr);
    AppendOwnedPreRenderFilter(new CssOutlineFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kInlineGoogleFontCss)) {
    // Inline small Google Font Service CSS files.
    // Do this before MoveCssToHead / MoveCssAboveScripts.
    AppendOwnedPreRenderFilter(new GoogleFontCssInlineFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kMoveCssToHead) ||
      rewrite_options->Enabled(RewriteOptions::kMoveCssAboveScripts)) {
    // It's good to move CSS links to the head prior to running CSS combine,
    // which only combines CSS links that are already in the head.
    AppendOwnedPreRenderFilter(new CssMoveToHeadFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kCombineCss)) {
    // Combine external CSS resources after we've outlined them.
    // CSS files in html document.  This can only be called
    // once and requires a server_context_ to be set.
    EnableRewriteFilter(RewriteOptions::kCssCombinerId);
  }
  if (rewrite_options->Enabled(RewriteOptions::kRewriteCss) ||
      (!rewrite_options->Forbidden(RewriteOptions::kRewriteCss) &&
       FlattenCssImportsEnabled())) {
    // Since AddFilters only applies to the HTML rewrite path, we check here
    // if IPRO preemptive rewrites are disabled and skip the filter if so.
    if (!rewrite_options->css_preserve_urls() ||
        rewrite_options->in_place_preemptive_rewrite_css()) {
      EnableRewriteFilter(RewriteOptions::kCssFilterId);
    }
  }
  if ((rewrite_options->Enabled(RewriteOptions::kPrioritizeCriticalCss) &&
       server_context()->factory()->UseBeaconResultsInFilters()) ||
      rewrite_options->Enabled(RewriteOptions::kComputeCriticalCss)) {
    // Add the critical selector instrumentation before the rewriting filter.
    AppendOwnedPreRenderFilter(new CriticalCssBeaconFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kPrioritizeCriticalCss)) {
    AppendOwnedPreRenderFilter(new CriticalSelectorFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kInlineCss)) {
    // Inline small CSS files.  Give CSS minification and flattening a chance to
    // run before we decide what counts as "small".
    CHECK(server_context_ != nullptr);
    AppendOwnedPreRenderFilter(new CssInlineFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kOutlineJavascript)) {
    // Cut out inlined scripts and make them into external resources.
    // This can only be called once and requires a server_context_ to be set.
    CHECK(server_context_ != nullptr);
    AppendOwnedPreRenderFilter(new JsOutlineFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kInsertGA) &&
      rewrite_options->ga_id() != "") {
    // InsertGA should be before js rewriting.
    AppendOwnedPreRenderFilter(new InsertGAFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kCombineJavascript)) {
    // Combine external JS resources. Done after minification and analytics
    // detection, as it converts script sources into string literals, making
    // them opaque to analysis.
    EnableRewriteFilter(RewriteOptions::kJavascriptCombinerId);
  }
  if (rewrite_options->Enabled(RewriteOptions::kRewriteJavascriptExternal) ||
      rewrite_options->Enabled(RewriteOptions::kRewriteJavascriptInline) ||
      rewrite_options->Enabled(
          RewriteOptions::kCanonicalizeJavascriptLibraries)) {
    // Since AddFilters only applies to the HTML rewrite path, we check here
    // if IPRO preemptive rewrites are disabled and skip the filter if so.
    //
    // Note that we minify before we inline, so if you enable
    // rewrite_javascript_inline but not rewrite_javascript_external, we
    // will only minify the already-inlined JavaScript, and we will not
    // minify external JS that we decided later to inline.  It seems unlikely
    // that someone would want to enable inline_javascript and not enable
    // rewrite_javascript_external though.
    if (!rewrite_options->js_preserve_urls() ||
        rewrite_options->in_place_preemptive_rewrite_javascript() ||
        rewrite_options->Enabled(RewriteOptions::kRewriteJavascriptInline)) {
      // Rewrite (minify etc.) JavaScript code to reduce time to first
      // interaction.
      EnableRewriteFilter(RewriteOptions::kJavascriptMinId);
    }
  }

  if (rewrite_options->Enabled(RewriteOptions::kInlineJavascript)) {
    // Inline small Javascript files.  Give JS minification a chance to run
    // before we decide what counts as "small".
    CHECK(server_context_ != nullptr);
    AppendOwnedPreRenderFilter(new JsInlineFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kConvertJpegToProgressive) ||
      rewrite_options->ImageOptimizationEnabled() ||
      rewrite_options->Enabled(RewriteOptions::kResizeImages) ||
      rewrite_options->Enabled(
          RewriteOptions::kResizeToRenderedImageDimensions) ||
      rewrite_options->Enabled(RewriteOptions::kInlineImages) ||
      rewrite_options->Enabled(RewriteOptions::kInsertImageDimensions) ||
      rewrite_options->Enabled(RewriteOptions::kJpegSubsampling) ||
      rewrite_options->Enabled(RewriteOptions::kStripImageColorProfile) ||
      rewrite_options->Enabled(RewriteOptions::kStripImageMetaData) ||
      rewrite_options->Enabled(RewriteOptions::kDelayImages)) {
    // Since AddFilters only applies to the HTML rewrite path, we check here
    // if IPRO preemptive rewrites are disabled and skip the filter if so.
    if (!rewrite_options->image_preserve_urls() ||
        rewrite_options->in_place_preemptive_rewrite_images()) {
      EnableRewriteFilter(RewriteOptions::kImageCompressionId);
    }
  }
  if (rewrite_options->Enabled(RewriteOptions::kRemoveComments)) {
    AppendOwnedPreRenderFilter(new RemoveCommentsFilter(
        this, new RemoveCommentsFilterOptions(rewrite_options)));
  }
  if (rewrite_options->Enabled(RewriteOptions::kElideAttributes)) {
    // Remove HTML element attribute values where
    // http://www.w3.org/TR/html4/loose.dtd says that the name is all
    // that's necessary
    AppendOwnedPreRenderFilter(new ElideAttributesFilter(this));
  }
  bool ext_cache_css =
      rewrite_options->Enabled(RewriteOptions::kExtendCacheCss);
  bool ext_cache_img =
      rewrite_options->Enabled(RewriteOptions::kExtendCacheImages);
  bool ext_cache_pdf =
      rewrite_options->Enabled(RewriteOptions::kExtendCachePdfs);
  bool ext_cache_js =
      rewrite_options->Enabled(RewriteOptions::kExtendCacheScripts);
  LOG(INFO) << "AddPreRenderFilters: ext_cache_css=" << ext_cache_css
            << " ext_cache_img=" << ext_cache_img
            << " ext_cache_pdf=" << ext_cache_pdf
            << " ext_cache_js=" << ext_cache_js;
  if (ext_cache_css || ext_cache_img || ext_cache_pdf || ext_cache_js) {
    // Extend the cache lifetime of resources.
    LOG(INFO) << "AddPreRenderFilters: Enabling CacheExtender filter";
    EnableRewriteFilter(RewriteOptions::kCacheExtenderId);
  }
  if (rewrite_options->Enabled(RewriteOptions::kSpriteImages)) {
    EnableRewriteFilter(RewriteOptions::kImageCombineId);
  }
  if (rewrite_options->Enabled(RewriteOptions::kLocalStorageCache)) {
    EnableRewriteFilter(RewriteOptions::kLocalStorageCacheId);
  }

  if (options()->NeedsDependenciesCohort()) {
    AppendOwnedPreRenderFilter(new CollectDependenciesFilter(this));
  }
}

void RewriteDriver::AddPostRenderFilters() {
  const RewriteOptions* rewrite_options = options();
  if (rewrite_options->Enabled(RewriteOptions::kInsertDnsPrefetch)) {
    InsertDnsPrefetchFilter* insert_dns_prefetch_filter =
        new InsertDnsPrefetchFilter(this);
    AddOwnedPostRenderFilter(insert_dns_prefetch_filter);
  }
  if (rewrite_options->Enabled(RewriteOptions::kInsertAmpLink)) {
    InsertAmpLinkFilter* insert_amp_link_filter = new InsertAmpLinkFilter(this);
    AddOwnedPostRenderFilter(insert_amp_link_filter);
  }
  if (rewrite_options->Enabled(RewriteOptions::kInsertSpeculationRules)) {
    AddOwnedPostRenderFilter(new InsertSpeculationRulesFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kAddInstrumentation)) {
    // Inject javascript to instrument loading-time. This should run before
    // defer js so that its onload handler can fire before JS starts executing.
    AddOwnedPostRenderFilter(new AddInstrumentationFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kDeferJavascript)) {
    // Defers javascript download and execution to post onload. This filter
    // should be applied before JsDisableFilter and JsDeferFilter.
    // DeferIframeFilter is an integral part of defer_js/disable_js: its
    // inline conversion scripts defer only because the js-defer machinery
    // defers them. The standalone 'defer_iframe' config name is a
    // deprecated no-op (kDeferIframeDeprecated).
    AddOwnedPostRenderFilter(new DeferIframeFilter(this));
    AddOwnedPostRenderFilter(new JsDisableFilter(this));
    // Though we are adding JsDeferDisabledFilter here, if we are flushing
    // cached html or we have flushed cached html, this filter will disable
    // itself.
    AddOwnedPostRenderFilter(new JsDeferDisabledFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kFixReflows)) {
    AddOwnedPostRenderFilter(new FixReflowFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kDeterministicJs)) {
    AddOwnedPostRenderFilter(new DeterministicJsFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kConvertMetaTags)) {
    AddOwnedPostRenderFilter(new MetaTagFilter(this));
  }
  // When agent_optimize is on, advertise Vary: Accept on HTML for an
  // Accept: text/markdown request (no body change — 1.1 never renders
  // markdown). This flag is the whole gate; the filter checks only Accept.
  if (rewrite_options->agent_optimize()) {
    AddOwnedPostRenderFilter(new AgentOptimizeVaryFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kDisableJavascript)) {
    // DeferIframeFilter is an integral part of defer_js/disable_js: its
    // inline conversion scripts defer only because the js-defer machinery
    // defers them. The standalone 'defer_iframe' config name is a
    // deprecated no-op (kDeferIframeDeprecated).
    AddOwnedPostRenderFilter(new DeferIframeFilter(this));
    AddOwnedPostRenderFilter(new JsDisableFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kDelayImages)) {
    // kInsertImageDimensions should be enabled to avoid drastic reflows.
    AddOwnedPostRenderFilter(new DelayImagesFilter(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kDedupInlinedImages)) {
    AddOwnedPostRenderFilter(new DedupInlinedImagesFilter(this));
  }
  // TODO(nikhilmadan): Should we disable this for bots?
  // LazyLoadImagesFilter should be applied after DelayImagesFilter.
  if (rewrite_options->Enabled(RewriteOptions::kLazyloadImages)) {
    AddOwnedPostRenderFilter(new LazyloadImagesFilter(this));
  }
  if (rewrite_options->support_noscript_enabled()) {
    AddOwnedPostRenderFilter(new SupportNoscriptFilter(this));
  }

  if (rewrite_options->Enabled(RewriteOptions::kHandleNoscriptRedirect)) {
    AddOwnedPostRenderFilter(new HandleNoscriptRedirectFilter(this));
  }

  if (rewrite_options->max_html_parse_bytes() > 0) {
    AddOwnedPostRenderFilter(new RedirectOnSizeLimitFilter(this));
    set_size_limit(rewrite_options->max_html_parse_bytes());
  }

  if (rewrite_options->Enabled(RewriteOptions::kPedantic)) {
    // Add HTML type attributes where HTML4 says that it's necessary.
    PedanticFilter* filter = new PedanticFilter(this);
    AddOwnedPostRenderFilter(filter);
  }
  // All filters that might add urls should come before the domain rewriter,
  // so they'll get rewritten.
  if (rewrite_options->domain_lawyer()->can_rewrite_domains() &&
      rewrite_options->Enabled(RewriteOptions::kRewriteDomains)) {
    // Rewrite mapped domains and shard any resources not otherwise rewritten.
    // We want do do this after all the content-changing rewrites, because they
    // will map & shard as part of their execution.
    //
    // TODO(jmarantz): Consider removing all the domain-mapping functionality
    // from other rewrites and do it exclusively in this filter.  Before we
    // do that we'll need to validate this filter so we can turn it on by
    // default.
    //
    // Note that the "domain_lawyer" filter controls whether we rewrite
    // domains for resources in HTML files.  However, when we cache-extend
    // CSS files, we rewrite the domains in them whether this filter is
    // specified or not.
    AddUnownedPostRenderFilter(domain_rewriter_.get());
  }
  if (rewrite_options->Enabled(RewriteOptions::kLeftTrimUrls)) {
    // Trim extraneous prefixes from urls in attribute values.
    // Happens before RemoveQuotes but after everything else.  Note:
    // we Must left trim urls BEFORE quote removal.
    AddUnownedPostRenderFilter(url_trim_filter_.get());
  }
  // Remove quotes and collapse whitespace at the very end for maximum effect.
  if (rewrite_options->Enabled(RewriteOptions::kRemoveQuotes)) {
    // Remove extraneous quotes from html attributes.
    AddOwnedPostRenderFilter(new HtmlAttributeQuoteRemoval(this));
  }
  if (rewrite_options->Enabled(RewriteOptions::kCollapseWhitespace)) {
    // Remove excess whitespace in HTML.
    AddOwnedPostRenderFilter(new CollapseWhitespaceFilter(this));
  }
  if (options()->Enabled(RewriteOptions::kHintPreloadSubresources)) {
    AppendOwnedPreRenderFilter(new PushPreloadFilter(this));
  }

  if (DebugMode()) {
    debug_filter_ = new DebugFilter(this);
    AddOwnedPostRenderFilter(debug_filter_);
  }

  // NOTE(abliss): Adding a new filter?  Does it export any statistics?  If it
  // doesn't, it probably should.  If it does, be sure to add it to the
  // InitStats() function above or it will break under Apache!
}

}  // namespace net_instaweb
