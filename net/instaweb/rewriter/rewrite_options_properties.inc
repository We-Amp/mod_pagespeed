// Copyright 2024 We-Amp B.V.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// X-macro table for standard AddBaseProperty() registrations in
// RewriteOptions::AddProperties().
//
// Each entry is:
//   X(default_value, member_name, id, option_constant, scope, help,
//     safe_to_print)
//
// The table is split into segments to preserve registration order with
// non-table calls (AddRequestProperty, options needing local variable
// initialization, or post-registration tweaks) that are interleaved in
// rewrite_options.cc.

// clang-format off

// Segment 1: kPassThrough through modify_caching_headers_.
// Followed by BeaconUrl (individual call, needs local variable init).
#define REWRITE_OPTION_PROPERTIES_SEGMENT_1(X) \
  X(kPassThrough, level_, "l", kRewriteLevel, \
    kDirectoryScope, \
    "Base level of rewriting (PassThrough, CoreFilters)", true) \
  X(kDefaultCssFlattenMaxBytes, css_flatten_max_bytes_, "cf", \
    kCssFlattenMaxBytes, kQueryScope, \
    "Number of bytes below which stylesheets will be flattened.", true) \
  X(kDefaultCssImageInlineMaxBytes, css_image_inline_max_bytes_, "cii", \
    kCssImageInlineMaxBytes, kQueryScope, \
    "Number of bytes below which CSS images will be inlined.", true) \
  X(kDefaultCssInlineMaxBytes, css_inline_max_bytes_, "ci", \
    kCssInlineMaxBytes, kQueryScope, \
    "Number of bytes below which stylesheets will be inlined.", true) \
  X(kDefaultGoogleFontCssInlineMaxBytes, \
    google_font_css_inline_max_bytes_, "gfci", \
    kGoogleFontCssInlineMaxBytes, kQueryScope, \
    "Number of bytes below which Google Font stylesheets will be inlined.", \
    true) \
  X(kDefaultCssOutlineMinBytes, css_outline_min_bytes_, "co", \
    kCssOutlineMinBytes, kDirectoryScope, \
    "Number of bytes above which inline CSS resources will be " \
    "outlined.", true) \
  X(kDefaultImageInlineMaxBytes, image_inline_max_bytes_, "ii", \
    kImageInlineMaxBytes, kQueryScope, \
    "Number of bytes below which images will be inlined.", true) \
  X(kDefaultJsInlineMaxBytes, js_inline_max_bytes_, "ji", \
    kJsInlineMaxBytes, kQueryScope, \
    "Number of bytes below which javascript will be inlined.", true) \
  X(kDefaultJsOutlineMinBytes, js_outline_min_bytes_, "jo", \
    kJsOutlineMinBytes, kDirectoryScope, \
    "Number of bytes above which inline Javascript resources will" \
    "be outlined.", true) \
  X(kDefaultProgressiveJpegMinBytes, progressive_jpeg_min_bytes_, "jp", \
    kProgressiveJpegMinBytes, kDirectoryScope, \
    "Minimum size in bytes for converting a jpeg to progressive", true) \
  X(kDefaultMaxCacheableResponseContentLength, \
    max_cacheable_response_content_length_, "rcl", \
    kMaxCacheableResponseContentLength, kServerScope, \
    "Maximum length of a cacheable response content. " \
    "To remove this limit, use -1.", true) \
  X(kDefaultMaxHtmlCacheTimeMs, max_html_cache_time_ms_, "hc", \
    kMaxHtmlCacheTimeMs, kDirectoryScope, nullptr, true) \
  X(kDefaultMaxHtmlParseBytes, max_html_parse_bytes_, "hpb", \
    kMaxHtmlParseBytes, kDirectoryScope, \
    "Maximum number of bytes of HTML that we parse, before " \
    "redirecting to ?ModPagespeed=off", true) \
  X(kDefaultMinResourceCacheTimeToRewriteMs, \
    min_resource_cache_time_to_rewrite_ms_, "rc", \
    kMinResourceCacheTimeToRewriteMs, kDirectoryScope, nullptr, true) \
  X(false, oblivious_pagespeed_urls_, "opu", \
    kObliviousPagespeedUrls, kDirectoryScope, nullptr, true) \
  X(false, rewrite_uncacheable_resources_, "rur", \
    kRewriteUncacheableResources, kServerScope, \
    "Allow optimization of uncacheable resources in the in-place rewriting" \
    " mode.", true) \
  X(kDefaultIdleFlushTimeMs, idle_flush_time_ms_, "if", \
    kIdleFlushTimeMs, kDirectoryScope, nullptr, true) \
  X(kDefaultFlushBufferLimitBytes, flush_buffer_limit_bytes_, "fbl", \
    kFlushBufferLimitBytes, kDirectoryScope, nullptr, true) \
  X(kDefaultImplicitCacheTtlMs, implicit_cache_ttl_ms_, "ict", \
    kImplicitCacheTtlMs, kDirectoryScope, \
    "Time in milliseconds to cache resources that lack an Expires or " \
    "Cache-Control header", true) \
  X(kDefaultLoadFromFileCacheTtlMs, load_from_file_cache_ttl_ms_, "lfct", \
    kLoadFromFileCacheTtlMs, kDirectoryScope, \
    "Time in milliseconds to cache resources loaded from file that lack an " \
    "Expires or Cache-Control header. If not explicitly set, defaults to " \
    "using the value set by implicit_cache_ttl_ms", true) \
  X(kDefaultImageMaxRewritesAtOnce, image_max_rewrites_at_once_, "im", \
    kImageMaxRewritesAtOnce, kLegacyProcessScope, \
    "Set bound on number of images being rewritten at one time " \
    "(0 = unbounded).", true) \
  X(kDefaultMaxUrlSegmentSize, max_url_segment_size_, "uss", \
    kMaxUrlSegmentSize, kDirectoryScope, \
    "Maximum size of a URL segment.", true) \
  X(kDefaultMaxUrlSize, max_url_size_, "us", \
    kMaxUrlSize, kDirectoryScope, nullptr, true) \
  X(false, forbid_all_disabled_filters_, "fadf", \
    kForbidAllDisabledFilters, kDirectoryScope, \
    "Prevents the use of disabled filters", true) \
  X(kDefaultRewriteDeadlineMs, rewrite_deadline_ms_, "rdm", \
    kRewriteDeadlineMs, kDirectoryScope, \
    "Time to wait for resource optimization (per flush window) before" \
    "falling back to the original resource for the request.", true) \
  X(kEnabledOn, enabled_, "e", kEnabled, kDirectoryScope, nullptr, true) \
  X(false, add_options_to_urls_, "aou", kAddOptionsToUrls, \
    kDirectoryScope, \
    "Add query-params with configuration adjustments to rewritten " \
    "URLs.", true) \
  X(false, publicly_cache_mismatched_hashes_experimental_, "pcmh", \
    kPubliclyCacheMismatchedHashesExperimental, kDirectoryScope, \
    "When serving a request for a .pagespeed. URL with the wrong hash, allow " \
    "public caching based on the origin TTL.", false) \
  X(true, in_place_rewriting_enabled_, "ipro", \
    kInPlaceResourceOptimization, kDirectoryScope, \
    "Allow rewriting resources even when they are " \
    "fetched over non-pagespeed URLs.", true) \
  X(false, in_place_wait_for_optimized_, "ipwo", \
    kInPlaceWaitForOptimized, kDirectoryScope, \
    "Wait for optimizations to complete", true) \
  X(10, in_place_s_maxage_sec_, "ipsm", \
    kInPlaceSMaxAgeSec, kServerScope, \
    "What to set s-maxage to on not-yet-optimized ipro resources", true) \
  X(kDefaultRewriteDeadlineMs, in_place_rewrite_deadline_ms_, "iprdm", \
    kInPlaceRewriteDeadlineMs, kDirectoryScope, \
    "Time to wait for an in-place resource optimization before" \
    "falling back to the original resource for the request.", true) \
  X(true, in_place_preemptive_rewrite_css_, "ipprc", \
    kInPlacePreemptiveRewriteCss, kDirectoryScope, \
    "If set, issue preemptive rewrites of CSS on the HTML path when " \
    "configured to use IPRO.", true) \
  X(true, in_place_preemptive_rewrite_css_images_, "ipprci", \
    kInPlacePreemptiveRewriteCssImages, kDirectoryScope, \
    "If set, issue preemptive rewrites of CSS images on the IPRO " \
    "serving path.", true) \
  X(true, in_place_preemptive_rewrite_images_, "ippri", \
    kInPlacePreemptiveRewriteImages, kDirectoryScope, \
    "If set, issue preemptive rewrites of images on the HTML path " \
    "when configured to use IPRO.", true) \
  X(true, in_place_preemptive_rewrite_javascript_, "ipprj", \
    kInPlacePreemptiveRewriteJavascript, kDirectoryScope, \
    "If set, issue preemptive rewrites of JS on the HTML path when " \
    "configured to use IPRO.", true) \
  X(true, combine_across_paths_, "cp", \
    kCombineAcrossPaths, kDirectoryScope, \
    "Allow combining resources from different paths", true) \
  X(true, critical_images_beacon_enabled_, "cibe", \
    kCriticalImagesBeaconEnabled, kDirectoryScope, \
    "Enable insertion of client-side critical " \
    "image detection js for image optimization filters.", true) \
  X(false, \
    test_only_prioritize_critical_css_dont_apply_original_css_, \
    "dlacae", kTestOnlyPrioritizeCriticalCssDontApplyOriginalCss, \
    kDirectoryScope, \
    "Stops the prioritize_critical_css filter from invoking its JavaScript " \
    "that applies all the 'hidden' CSS at onload. Intended for testing.", \
    false) \
  X(kDefaultBeaconReinstrumentTimeSec, \
    beacon_reinstrument_time_sec_, "brts", \
    kBeaconReinstrumentTimeSec, kDirectoryScope, \
    "How often (in seconds) to reinstrument pages with beacons. " \
    "This is used for both critical image beaconing, and for the " \
    "prioritize_critical_css filter.", true) \
  X(false, log_background_rewrites_, "lbr", \
    kLogBackgroundRewrite, kServerScope, nullptr, false) \
  X(false, log_mobilization_samples_, "lms", \
    kLogMobilizationSamples, kDirectoryScope, \
    "Verbose debugging of all sample data" \
    " generated by mobilization_label_filter.", false) \
  X(false, log_rewrite_timing_, "lr", kLogRewriteTiming, \
    kDirectoryScope, \
    "Whether or not to report timing information about HtmlParse.", false) \
  X(false, log_url_indices_, "lui", kLogUrlIndices, \
    kDirectoryScope, \
    "Whether or not to log URL indices for rewriter applications.", false) \
  X(false, lowercase_html_names_, "lh", \
    kLowercaseHtmlNames, kDirectoryScope, \
    "Lowercase tag and attribute names for HTML.", true) \
  X(false, always_rewrite_css_, "arc", \
    kAlwaysRewriteCss, kDirectoryScope, nullptr, true) \
  X(false, respect_vary_, "rv", kRespectVary, kDirectoryScope, \
    "Whether to respect Vary headers for resources. " \
    "Vary is always respected for HTML.", true) \
  X(false, agent_optimize_, "ao", kAgentOptimize, kDirectoryScope, \
    "the design record: recognize Accept: text/markdown agent requests, license/" \
    "entitlement-gate them, and add Vary: Accept on HTML. 1.1 always serves " \
    "optimized HTML (no markdown render).", true) \
  X(false, respect_x_forwarded_proto_, "rxfp", \
    kRespectXForwardedProto, kDirectoryScope, \
    "Whether to respect the X-Forwarded-Proto header.", true) \
  X(false, flush_html_, "fh", kFlushHtml, kServerScope, \
    "Enable auto-flush heuristics for HTML in full proxy mode", true) \
  X(false, css_preserve_urls_, "cpu", \
    kCssPreserveURLs, kDirectoryScope, \
    "Disable the rewriting of CSS URLs.", true) \
  X(false, image_preserve_urls_, "ipu", \
    kImagePreserveURLs, kDirectoryScope, \
    "Disable the rewriting of Image URLs.", true) \
  X(true, preserve_image_provenance_, "pip", \
    kPreserveImageProvenance, kDirectoryScope, \
    "Preserve C2PA/Content-Credentials provenance metadata (JPEG APP11/JUMBF) " \
    "through image optimization. On by default; independent of EXIF stripping.", \
    true) \
  X(false, image_provenance_carry_, "ipc", \
    kImageProvenanceCarry, kDirectoryScope, \
    "the design record Level A carry-through: when true (and PreserveImageProvenance is "\
    "on), PNG images carrying a C2PA / Content Credentials manifest are "\
    "recompressed AND their original manifest chunks (caBX/iTXt) are carried "\
    "into the optimized output unmodified, keeping both the byte savings and "\
    "the provenance. (Manifest-bearing JPEGs already carry their APP11/JUMBF "\
    "through recompression under PreserveImageProvenance.) Cases where the "\
    "carrier cannot be preserved byte-exactly -- resize, format conversion "\
    "(PNG->JPEG/WebP), GIF, or extraction failure -- fall back to the default "\
    "detect-and-skip (Level B) pass-through. No effect when "\
    "PreserveImageProvenance is false.", true) \
  X(false, js_preserve_urls_, "jpu", \
    kJsPreserveURLs, kDirectoryScope, \
    "Disable the rewriting of Javascript URLs.", true) \
  X(true, serve_stale_if_fetch_error_, "ss", \
    kServeStaleIfFetchError, kDirectoryScope, nullptr, true) \
  X(false, proactively_freshen_user_facing_request_, "pfur", \
    kProactivelyFreshenUserFacingRequest, kDirectoryScope, nullptr, true) \
  X(0, serve_stale_while_revalidate_threshold_sec_, "sswrt", \
    kServeStaleWhileRevalidateThresholdSec, kDirectoryScope, \
    "Threshold for serving serving stale responses while revalidating in " \
    "background. 0 means don't serve stale content." \
    "Note: Stale response will be served only for non-html requests.", true) \
  X(true, follow_flushes_, "ff", kFollowFlushes, kDirectoryScope, \
    "Attempt to mirror incoming flushes for html streams in the output " \
    "when ProxyFetch is used.", true) \
  X(false, enable_defer_js_experimental_, "edje", \
    kEnableDeferJsExperimental, kDirectoryScope, \
    "Enable experimental options in defer javascript.", true) \
  X(false, disable_background_fetches_for_bots_, "dbfb", \
    kDisableBackgroundFetchesForBots, kDirectoryScope, \
    "Disable pre-emptive background fetches on bot requests.", true) \
  X(true, disable_rewrite_on_no_transform_, "drnt", \
    kDisableRewriteOnNoTransform, kDirectoryScope, \
    "If false, resource is rewritten even if no-transform header is set", \
    true) \
  X(false, enable_cache_purge_, "euci", kEnableCachePurge, \
    kServerScope, \
    "Allows individual resources to be flushed; adding some overhead to " \
    "the metadata cache", true) \
  X(false, proactive_resource_freshening_, "prf", \
    kProactiveResourceFreshening, kServerScope, \
    "If true, allows proactive freshening of inputs to the resource when " \
    "they are close to expiry.", true) \
  X(false, lazyload_highres_images_, "elhr", \
    kEnableLazyLoadHighResImages, kDirectoryScope, nullptr, true) \
  X(false, default_cache_html_, "dch", \
    kDefaultCacheHtml, kDirectoryScope, nullptr, true) \
  X(kDefaultDomainShardCount, domain_shard_count_, "dsc", \
    kDomainShardCount, kQueryScope, nullptr, true) \
  X(true, modify_caching_headers_, "mch", \
    kModifyCachingHeaders, kDirectoryScope, \
    "Set to false to disallow mod_pagespeed from editing HTML " \
    "Cache-Control headers. This is not safe in general and can cause " \
    "the incorrect versions of HTML to be served to users.", true)

// Segment 2: lazyload_images_after_onload_ through enable_prioritizing_scripts_.
// Preceded by BeaconUrl (individual call).
// Followed by AddRequestProperty calls for pre_connect_url_ and
// property_cache_http_status_stability_threshold_.
#define REWRITE_OPTION_PROPERTIES_SEGMENT_2(X) \
  X(true, lazyload_images_after_onload_, "llio", \
    kLazyloadImagesAfterOnload, kDirectoryScope, \
    "Wait until page onload before loading lazy images", true) \
  X("", request_option_override_, "roo", \
    kRequestOptionOverride, kDirectoryScope, \
    "Token passed in URL to enable pagespeed options in params.", false) \
  X("", url_signing_key_, "usk", kUrlSigningKey, kServerScope, \
    "Key used for signing .pagespeed resource URLs.", false) \
  X(false, accept_invalid_signatures_, "ais", \
    kAcceptInvalidSignatures, kServerScope, \
    "Accept resources with invalid signatures.", false) \
  X(Timer::kSecondMs, remote_configuration_timeout_ms_, "rcfgt", \
    kRemoteConfigurationTimeoutMs, kServerScope, \
    "Timeout for fetch of remote configuration file.", true) \
  X("", remote_configuration_url_, "rcfgu", \
    kRemoteConfigurationUrl, kDirectoryScope, \
    "URL of site from which to pull remote configuration files", true) \
  X(9, http_cache_compression_level_, "hccl", \
    kHttpCacheCompressionLevel, kServerScope, \
    "Compression level for HTTPCache. [-1-9] where 0 is off, 1 is minimum" \
    "compression, and 9 (the default) is maximum compression.", true) \
  X("", lazyload_images_blank_url_, "llbu", \
    kLazyloadImagesBlankUrl, kDirectoryScope, \
    "URL of image used to display prior to loading the lazy image. " \
    "Empty means use a site-local copy.", true) \
  X(kLazyloadImagesModeAuto, lazyload_images_mode_, "llmo", \
    kLazyloadImagesMode, kDirectoryScope, \
    "How to lazyload images: 'native' annotates non-critical images with " \
    "loading=\"lazy\", 'js' uses the JavaScript loader, and 'auto' (the " \
    "default) picks per user agent.", true) \
  X(1, lazyload_images_skip_first_, "llsf", \
    kLazyloadImagesSkipFirst, kDirectoryScope, \
    "Number of leading images to leave untouched when critical image data " \
    "is unavailable, protecting likely LCP candidates.", true) \
  X(false, use_blank_image_for_inline_preview_, "biip", \
    kUseBlankImageForInlinePreview, kDirectoryScope, \
    "Use a blank image for inline preview", true) \
  X(true, inline_only_critical_images_, "ioci", \
    kInlineOnlyCriticalImages, kDirectoryScope, \
    "Inline only critical images", true) \
  X(ResourceCategorySet(), inline_unauthorized_resource_types_, "irwea", \
    kInlineResourcesWithoutExplicitAuthorization, kDirectoryScope, \
    "Specifies the resource types that can be inlined into HTML even if " \
    "they do not belong to explicitly authorized domains.", true) \
  X(false, domain_rewrite_cookies_, "drc", \
    kDomainRewriteCookies, kDirectoryScope, \
    "Allow rewrite_domains to rewrite domains in Set-Cookie headers.", true) \
  X(false, domain_rewrite_hyperlinks_, "drh", \
    kDomainRewriteHyperlinks, kDirectoryScope, \
    "Allow rewrite_domains to rewrite <form> and <a> tags in addition " \
    "to resource tags.", true) \
  X(false, client_domain_rewrite_, "cdr", \
    kClientDomainRewrite, kDirectoryScope, \
    "Allow rewrite_domains to rewrite urls on the client side.", true) \
  X(kDefaultImageJpegRecompressQuality, \
    image_jpeg_recompress_quality_, "iq", \
    kImageJpegRecompressionQuality, kQueryScope, \
    "Set quality parameter for recompressing jpeg images [-1,100], " \
    "100 is lossless, -1 uses ImageRecompressionQuality", true) \
  X(kDefaultImageJpegRecompressQualityForSmallScreens, \
    image_jpeg_recompress_quality_for_small_screens_, "iqss", \
    kImageJpegRecompressionQualityForSmallScreens, kQueryScope, \
    "Set quality parameter for recompressing jpeg images for small " \
    "screens. [-1,100], 100 refers to best quality, -1 falls back to " \
    "ImageJpegRecompressionQuality.", true) \
  X(kDefaultImageJpegQualityForSaveData, \
    image_jpeg_quality_for_save_data_, "iqsd", \
    kImageJpegQualityForSaveData, kQueryScope, \
    "Set quality for the images which will be optimized to JPEG format in " \
    "the Save-Data mode. Use a value in [0,100] to explicitly set the " \
    "quality. Use -1 to ignore the Save-Data header.", true) \
  X(kDefaultImageRecompressQuality, image_recompress_quality_, "irq", \
    kImageRecompressionQuality, kQueryScope, \
    "Set quality parameter for recompressing images [-1,100], " \
    "100 refers to best quality, -1 disables lossy compression. " \
    "JpegRecompressionQuality and WebpRecompressionQuality override " \
    "this.", true) \
  X(kDefaultImageLimitOptimizedPercent, \
    image_limit_optimized_percent_, "ip", \
    kImageLimitOptimizedPercent, kDirectoryScope, \
    "Replace images whose size after recompression is less than the " \
    "given percent of original image size; 100 means replace if " \
    "smaller.", true) \
  X(kDefaultImageLimitRenderedAreaPercent, \
    image_limit_rendered_area_percent_, "ira", \
    kImageLimitRenderedAreaPercent, kDirectoryScope, \
    "Limit on percentage of rendered image wxh to the original " \
    "image wxh that should be stored in the property cache. This is to " \
    "avoid corner cases where rounding off decreases the rendered " \
    "image size by a few pixels.", true) \
  X(kDefaultImageLimitResizeAreaPercent, \
    image_limit_resize_area_percent_, "ia", \
    kImageLimitResizeAreaPercent, kDirectoryScope, \
    "Consider resizing images whose area in pixels is less than the " \
    "given percent of original image area; 100 means replace if " \
    "smaller.", true) \
  X(kDefaultImageWebpRecompressQuality, \
    image_webp_recompress_quality_, "iw", \
    kImageWebpRecompressionQuality, kQueryScope, \
    "Quality for rewritten webp images [-1,100], 100 refers to best quality, " \
    "-1 uses ImageRecompressionQuality.", true) \
  X(kDefaultImageWebpRecompressQualityForSmallScreens, \
    image_webp_recompress_quality_for_small_screens_, "iwss", \
    kImageWebpRecompressionQualityForSmallScreens, kQueryScope, \
    "Quality for rewritten webp images for small screens. [-1,100], " \
    "100 refers to best quality, -1 falls back to " \
    "WebpRecompressionQuality.", true) \
  X(kDefaultImageWebpAnimatedRecompressQuality, \
    image_webp_animated_recompress_quality_, "iwa", \
    kImageWebpAnimatedRecompressionQuality, kQueryScope, \
    "Quality for rewritten animated webp images [-1,100], " \
    "100 refers to best quality, -1 uses ImageRecompressionQuality.", true) \
  X(kDefaultImageWebpQualityForSaveData, \
    image_webp_quality_for_save_data_, "iwsd", \
    kImageWebpQualityForSaveData, kQueryScope, \
    "Set quality for the images which will be optimized to lossy WebP " \
    "format in the Save-Data mode. Use a value in [0,100] to explicitly set " \
    "the quality. Use -1 to ignore the Save-Data header.", true) \
  X(kDefaultImageWebpTimeoutMs, image_webp_timeout_ms_, "wt", \
    kImageWebpTimeoutMs, kLegacyProcessScope, nullptr, true) \
  X(kDefaultImageAvifRecompressQuality, \
    image_avif_recompress_quality_, "iav", \
    kImageAvifRecompressionQuality, kQueryScope, \
    "Quality for rewritten avif images [-1,100], 100 refers to best quality, " \
    "-1 uses ImageRecompressionQuality.", true) \
  X(kDefaultImageAvifRecompressQualityForSmallScreens, \
    image_avif_recompress_quality_for_small_screens_, "iavss", \
    kImageAvifRecompressionQualityForSmallScreens, kQueryScope, \
    "Quality for rewritten avif images for small screens. [-1,100], " \
    "100 refers to best quality, -1 falls back to " \
    "AvifRecompressionQuality.", true) \
  X(kDefaultImageAvifAnimatedRecompressQuality, \
    image_avif_animated_recompress_quality_, "iava", \
    kImageAvifAnimatedRecompressionQuality, kQueryScope, \
    "Quality for rewritten animated avif images [-1,100], " \
    "100 refers to best quality, -1 uses ImageRecompressionQuality.", true) \
  X(kDefaultImageAvifQualityForSaveData, \
    image_avif_quality_for_save_data_, "iavsd", \
    kImageAvifQualityForSaveData, kQueryScope, \
    "Set quality for the images which will be optimized to lossy AVIF " \
    "format in the Save-Data mode. Use a value in [0,100] to explicitly set " \
    "the quality. Use -1 to ignore the Save-Data header.", true) \
  X(kDefaultImageAvifTimeoutMs, image_avif_timeout_ms_, "avt", \
    kImageAvifTimeoutMs, kLegacyProcessScope, \
    "Timeout for AVIF still-image encoding, in milliseconds; the encoder " \
    "speed is derived from this budget.", true) \
  X(kDefaultMaxInlinedPreviewImagesIndex, \
    max_inlined_preview_images_index_, "mdii", \
    kMaxInlinedPreviewImagesIndex, kDirectoryScope, \
    "Number of first N images for which low resolution image is " \
    "generated. Negative values result in generation for all images.", true) \
  X(kDefaultMinImageSizeLowResolutionBytes, \
    min_image_size_low_resolution_bytes_, "nislr", \
    kMinImageSizeLowResolutionBytes, kDirectoryScope, \
    "Minimum image size above which low resolution image is " \
    "generated.", true) \
  X(kDefaultMaxImageSizeLowResolutionBytes, \
    max_image_size_low_resolution_bytes_, "xislr", \
    kMaxImageSizeLowResolutionBytes, kDirectoryScope, \
    "Maximum image size below which low resolution image is " \
    "generated.", true) \
  X(kDefaultFinderPropertiesCacheExpirationTimeMs, \
    finder_properties_cache_expiration_time_ms_, "fpce", \
    kFinderPropertiesCacheExpirationTimeMs, kDirectoryScope, \
    "Number of ms that beacon results for the critical selector finders " \
    "should be considered valid.", true) \
  X(kDefaultFinderPropertiesCacheRefreshTimeMs, \
    finder_properties_cache_refresh_time_ms_, "fpcr", \
    kFinderPropertiesCacheRefreshTimeMs, kDirectoryScope, nullptr, true) \
  X(kDefaultExperimentCookieDurationMs, \
    experiment_cookie_duration_ms_, "fcd", \
    kExperimentCookieDurationMs, kDirectoryScope, nullptr, true) \
  X(kDefaultImageJpegNumProgressiveScans, \
    image_jpeg_num_progressive_scans_, "ijps", \
    kImageJpegNumProgressiveScans, kDirectoryScope, \
    "Number of progressive scans [1,10] to emit when rewriting images as " \
    "ten-scan progressive jpegs. " \
    "A value of -1 outputs all progressive scans.", true) \
  X(kDefaultImageJpegNumProgressiveScans, \
    image_jpeg_num_progressive_scans_for_small_screens_, "ijpst", \
    kImageJpegNumProgressiveScansForSmallScreens, kDirectoryScope, \
    "Number of progressive scans [1,10] to emit when rewriting images as" \
    "ten-scan progressive jpegs for small screens. A value of -1 falls " \
    "back to kImageJpegNumProgressiveScans.", true) \
  X(false, cache_small_images_unrewritten_, "csiu", \
    kCacheSmallImagesUnrewritten, kDirectoryScope, nullptr, true) \
  X(kDefaultImageResolutionLimitBytes, \
    image_resolution_limit_bytes_, "irlb", \
    kImageResolutionLimitBytes, kDirectoryScope, \
    "Maximum byte size of an image for optimization", true) \
  X(0, rewrite_random_drop_percentage_, "rrdp", \
    kRewriteRandomDropPercentage, kDirectoryScope, \
    "The percentage of time that pagespeed should randomly drop an " \
    "opportunity to optimize an image.  The value should be an integer " \
    "between 0 and 100 inclusive.", true) \
  X("", ga_id_, "ig", kAnalyticsID, kDirectoryScope, \
    "Google Analytics ID to use on site.", true) \
  X("", content_experiment_id_, "cxid", \
    kContentExperimentID, kDirectoryScope, \
    "Which Google Analytics content experiment to log to.", true) \
  X("", content_experiment_variant_id_, "cxvid", \
    kContentExperimentVariantID, kDirectoryScope, \
    "Which Google Analytics content experiment variant to log to.", true) \
  X(true, use_analytics_js_, "uajs", kUseAnalyticsJs, kQueryScope, \
    "Log to analytics.js instead of ga.js with insert_ga.", true) \
  X(true, increase_speed_tracking_, "st", \
    kIncreaseSpeedTracking, kDirectoryScope, nullptr, true) \
  X(false, running_experiment_, "fur", \
    kRunningExperiment, kDirectoryScope, nullptr, true) \
  X(kDefaultExperimentSlot, experiment_ga_slot_, "fga", \
    kExperimentSlot, kDirectoryScope, nullptr, true) \
  X(experiment::kForceNoExperiment, enroll_experiment_id_, "eeid", \
    kEnrollExperiment, kQueryScope, \
    "Assign users to a specific experiment setting.", true) \
  X(false, report_unload_time_, "rut", \
    kReportUnloadTime, kDirectoryScope, \
    "Deprecated, no effect: the beacon is now sent on page-hide, which " \
    "covers pre-onload abandonment without breaking bfcache.", true) \
  X("", x_header_value_, "xhv", \
    kXModPagespeedHeaderValue, kDirectoryScope, \
    "Set the value for the X-Mod-Pagespeed HTTP header", true) \
  X(true, avoid_renaming_introspective_javascript_, "aris", \
    kAvoidRenamingIntrospectiveJavascript, kDirectoryScope, \
    "Don't combine, inline, cache extend, or otherwise modify " \
    "javascript in ways that require changing the URL if we see " \
    "introspection in the form of " \
    "document.getElementsByTagName('script').", true) \
  X(false, reject_blacklisted_, "rbl", \
    kRejectBlacklisted, kDirectoryScope, nullptr, false) \
  X(HttpStatus::kForbidden, reject_blacklisted_status_code_, "rbls", \
    kRejectBlacklistedStatusCode, kDirectoryScope, nullptr, false) \
  X(kDefaultBlockingRewriteKey, blocking_rewrite_key_, "blrw", \
    kXPsaBlockingRewrite, kServerScope, \
    "If the X-PSA-Pagespeed-Blocking-Rewrite header is present, and " \
    "its value matches the configured value, ensure that all " \
    "rewrites are completed before sending the response to the " \
    "client.", false) \
  X(false, use_fallback_property_cache_values_, "fbcv", \
    kUseFallbackPropertyCacheValues, kServerScope, \
    "If this is set to true, fallback values will be used from property " \
    "cache if actual value is not present. Here fallback values means " \
    "properties which are shared across all requests which have same url " \
    "if query paramaters are removed. Example: http://www.test.com?a=1 and " \
    "http://www.test.com?a=2 share same fallback properties though they " \
    "are two different urls.", true) \
  X(false, await_pcache_lookup_, "wpcl", \
    kAwaitPcacheLookup, kServerScope, nullptr, true) \
  X(true, support_noscript_enabled_, "snse", \
    kSupportNoScriptEnabled, kDirectoryScope, \
    "Support for clients with no script support, in filters that " \
    "insert new javascript.", true) \
  X(false, enable_extended_instrumentation_, "eei", \
    kEnableExtendedInstrumentation, kDirectoryScope, \
    "If set to true, addition instrumentation js is added to that page that " \
    "the beacon can collect more information.", true) \
  X(kDefaultMaxCombinedCssBytes, max_combined_css_bytes_, "xcc", \
    kMaxCombinedCssBytes, kQueryScope, \
    "Maximum size allowed for the combined CSS resource.", true) \
  X(kDefaultMaxCombinedJsBytes, max_combined_js_bytes_, "xcj", \
    kMaxCombinedJsBytes, kDirectoryScope, \
    "Maximum size allowed for the combined JavaScript resource.", true) \
  X(-1, override_caching_ttl_ms_, "octm", \
    kOverrideCachingTtlMs, kDirectoryScope, nullptr, true) \
  X(5 * Timer::kSecondMs, blocking_fetch_timeout_ms_, "bfto", \
    RewriteOptions::kFetcherTimeOutMs, kDirectoryScope, nullptr, true) \
  X(false, enable_prioritizing_scripts_, "eps", \
    kEnablePrioritizingScripts, kDirectoryScope, nullptr, true)

// Segment 3: max_rewrite_info_log_size_ through
// downstream_cache_rewritten_percentage_threshold_.
// Preceded by AddRequestProperty calls for pre_connect_url_ and
// property_cache_http_status_stability_threshold_.
// Followed by AddRequestProperty calls for
// metadata_input_errors_cache_ttl_ms_ and override_ie_document_mode_.
#define REWRITE_OPTION_PROPERTIES_SEGMENT_3(X) \
  X(kDefaultMaxRewriteInfoLogSize, max_rewrite_info_log_size_, "mrils", \
    kMaxRewriteInfoLogSize, kDirectoryScope, nullptr, false) \
  X(kDefaultMetadataCacheStalenessThresholdMs, \
    metadata_cache_staleness_threshold_ms_, "mcst", \
    kMetadataCacheStalenessThresholdMs, kDirectoryScope, nullptr, true) \
  X(kDefaultDownstreamCachePurgeMethod, \
    downstream_cache_purge_method_, "dcpm", \
    kDownstreamCachePurgeMethod, kDirectoryScope, \
    "Method to be used for purging responses from the downstream cache", \
    false) \
  X("", downstream_cache_rebeaconing_key_, "dcrk", \
    kDownstreamCacheRebeaconingKey, kDirectoryScope, \
    "The key used to authenticate rebeaconing requests from downstream " \
    "caches. The value specified for this key in the pagespeed server " \
    "config should be used in the caching layer configuration also.", false) \
  X(kDefaultDownstreamCacheRewrittenPercentageThreshold, \
    downstream_cache_rewritten_percentage_threshold_, "dcrpt", \
    kDownstreamCacheRewrittenPercentageThreshold, kDirectoryScope, \
    "Threshold for percentage of rewriting to be finished before the " \
    "response is served out and simultaneously stored in the downstream " \
    "cache, beyond which the response will not be purged from the cache even" \
    "if more rewriting is possible now", true)

// Segment 4: amp_link_pattern_ through option_cookies_duration_ms_.
// Preceded by AddRequestProperty calls for
// metadata_input_errors_cache_ttl_ms_ and override_ie_document_mode_.
// Followed by ResponsiveDensities and AllowVaryOn (individual calls).
#define REWRITE_OPTION_PROPERTIES_SEGMENT_4(X) \
  X("", amp_link_pattern_, "alp", \
    kAmpLinkPattern, kDirectoryScope, nullptr, true) \
  X(true, honor_csp_, "hcsp", kHonorCsp, kServerScope, \
    "Controls whether PageSpeed should pay attention to " \
    "Content-Security-Policy directives", false) \
  X(true, enable_aggressive_rewriters_for_mobile_, "earm", \
    kEnableAggressiveRewritersForMobile, kDirectoryScope, \
    "Allows defer_javascript and defer_iframe for mobile browsers", true) \
  X(false, serve_xhr_access_control_headers_, "shach", \
    kServeXhrAccessControlHeaders, kDirectoryScope, \
    "Serve access control headers with response headers", false) \
  X("", access_control_allow_origins_, "acao", \
    kAccessControlAllowOrigins, kDirectoryScope, \
    "Comma separated list of origins that are allowed to make cross-origin " \
    "requests", false) \
  X(false, hide_referer_using_meta_, "hrum", \
    kHideRefererUsingMeta, kDirectoryScope, \
    "Hides the referer by adding meta tag to the HTML", true) \
  X(false, preserve_subresource_hints_, "psrh", \
    kPreserveSubresourceHints, kQueryScope, \
    "Keep original subresource hints in place.", true) \
  X(true, preserve_url_relativity_, "pur", \
    kPreserveUrlRelativity, kDirectoryScope, \
    "Keep rewritten URLs as relative as the original resource URL was.", \
    true) \
  X(false, allow_logging_urls_in_log_record_, "alulr", \
    kAllowLoggingUrlsInLogRecord, kDirectoryScope, nullptr, false) \
  X(true, allow_options_to_be_set_by_cookies_, "aotbsbc", \
    kAllowOptionsToBeSetByCookies, kDirectoryScope, \
    "Allow options to be set by cookies in addition to query parameters " \
    "and request headers.", true) \
  X("", non_cacheables_for_cache_partial_html_, "nccp", \
    kNonCacheablesForCachePartialHtml, kDirectoryScope, nullptr, false) \
  X(false, no_transform_optimized_images_, "ntoi", \
    kNoTransformOptimizedImages, kDirectoryScope, \
    "Add no-transform header to cache-control for optimized images", true) \
  X(kDefaultMaxLowResImageSizeBytes, \
    max_low_res_image_size_bytes_, "lris", \
    kMaxLowResImageSizeBytes, kDirectoryScope, nullptr, true) \
  X(kDefaultMaxLowResToFullResImageSizePercentage, \
    max_low_res_to_full_res_image_size_percentage_, "lrhrs", \
    kMaxLowResToHighResImageSizePercentage, kDirectoryScope, nullptr, true) \
  X(true, serve_rewritten_webp_urls_to_any_agent_, "swaa", \
    kServeWebpToAnyAgent, kDirectoryScope, \
    "Serve rewritten .webp images to any user-agent", true) \
  X("", cache_fragment_, "ckp", kCacheFragment, kDirectoryScope, \
    "Set a cache fragment to allow servers with different hostnames to " \
    "share a cache.  Allowed: letters, numbers, underscores, and hyphens.", \
    false) \
  X("", sticky_query_parameters_, "sqp", \
    kStickyQueryParameters, kDirectoryScope, \
    "The token that must be set by the PageSpeedStickyQueryParameters query " \
    "parameter/header in a request to enable the setting of cookies for all " \
    "other PageSpeed query parameters/headers in the request. Blank means " \
    "it is disabled.", false) \
  X(kDefaultOptionCookiesDurationMs, option_cookies_duration_ms_, "ocd", \
    kOptionCookiesDurationMs, kDirectoryScope, \
    "The max-age in ms of cookies that set PageSpeed options.", true)

// clang-format on
