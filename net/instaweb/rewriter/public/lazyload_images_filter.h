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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_LAZYLOAD_IMAGES_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_LAZYLOAD_IMAGES_FILTER_H_

#include "net/instaweb/rewriter/public/common_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_filter.h"
#include "pagespeed/opt/logging/enums.pb.h"

namespace net_instaweb {

class StaticAssetManager;
class Statistics;
class Variable;

// Filter to lazyload images. Depending on LazyloadImagesMode this either:
//
// - (native mode) annotates non-critical <img> elements with loading="lazy"
//   and decoding="async", keeping src/srcset intact and injecting no
//   JavaScript. When critical-image data is available, critical images are
//   not lazy loaded and instead get fetchpriority="high" and
//   decoding="async".
//
// - (js mode) replaces the src with a data-pagespeed-lazy-src attribute and
//   injects a javascript loader that detects which images are in the user's
//   viewport and swaps the src back.
//
// - (auto mode, the default) applies native mode for user agents supporting
//   loading="lazy", js mode otherwise.
//
// In js mode the loader script is injected immediately before the first
// image that is rewritten, so pages without eligible images stay untouched.
// In order to immediately load images that are above the fold, we attach an
// onload event to each rewritten image. This onload event determines if the
// image is visible and immediately replaces the src with the
// data-pagespeed-lazy-src. Otherwise, the image is added to the deferred
// queue. Since the onload event is only fired if the image src is valid, we
// add a fixed inlined image to each image node we are deferring.
//
// When the user scrolls, we scan through the deferred queue and determine
// which images are now visible, and switch the src and
// data-pagespeed-lazy-src.
//
// Given the following input html:
// <html>
//  <head>
//  </head>
//  <body>
//   <img src="1.jpeg" />
//  </body>
// </html>
//
// The output in js mode will be
// <html>
//  <head>
//  </head>
//  <body>
//   <script>
//    Javascript that determines which images are visible and attaches a
//    window.scroll event.
//   </script>
//   <img data-pagespeed-lazy-src="1.jpeg" onload="kImageOnloadCode"
//    src="kBlankImageSrc" />
//  </body>
//
// and in native mode
// <html>
//  <head>
//  </head>
//  <body>
//   <img src="1.jpeg" loading="lazy" decoding="async" />
//  </body>
//
class LazyloadImagesFilter : public CommonFilter {
 public:
  static const char kImageOnloadCode[];
  static const char kLoadAllImages[];
  static const char kOverrideAttributeFunctions[];
  static const char kIsLazyloadScriptInsertedPropertyName[];

  // Statistics variable names.
  static const char kLazyloadImagesApplied[];
  static const char kLazyloadImagesNativeApplied[];
  static const char kLazyloadImagesSkippedCritical[];
  static const char kLazyloadImagesSkippedCsp[];

  explicit LazyloadImagesFilter(RewriteDriver* driver);
  ~LazyloadImagesFilter() override;

  const char* Name() const override { return "Lazyload Images"; }
  ScriptUsage GetScriptUsage() const override {
    // In native mode no scripts are injected. Auto mode conservatively
    // reports kWillInjectScripts since the per-request user agent decides.
    return driver()->options()->lazyload_images_mode() ==
                   RewriteOptions::kLazyloadImagesModeNative
               ? kNeverInjectsScripts
               : kWillInjectScripts;
  }

  static void InitStats(Statistics* statistics);

  // Lazyload filter will be no op for the request if ShouldApply returns
  // false.
  static RewriterHtmlApplication::Status ShouldApply(RewriteDriver* driver);
  // Whether native mode applies to this request: true when the mode option is
  // 'native', or when it is 'auto' and the user agent supports the native
  // loading="lazy" attribute.
  static bool ShouldApplyNativeMode(RewriteDriver* driver);
  static GoogleString GetLazyloadJsSnippet(
      const RewriteOptions* options, StaticAssetManager* static_asset_manager);

 private:
  void StartDocumentImpl() override;
  void EndDocument() override;
  void StartElementImpl(HtmlElement* element) override;
  void EndElementImpl(HtmlElement* element) override;
  void DetermineEnabled(GoogleString* disabled_reason) override;

  // Clears all state associated with the filter.
  void Clear();

  // Whether the (js mode) inline loader script and inline onload/onerror
  // handlers are permitted by the Content-Security-Policy seen so far.
  static bool CspPermitsJsMode(RewriteDriver* driver);

  static GoogleString GetBlankImageSrc(
      const RewriteOptions* options,
      const StaticAssetManager* static_asset_manager);

  // Inserts the lazyload JS code before the given element.
  void InsertLazyloadJsCode(HtmlElement* element);

  // Inserts a script to override attributes of all the images that have been
  // lazily loaded so far.
  void InsertOverrideAttributesScript(HtmlElement* element,
                                      bool is_before_script);

  // Returns true if any srcset candidate of the element is beacon-critical.
  // Consulted only for srcset-only images (no src to defer): in native mode a
  // critical candidate earns the same eager-fetch treatment as a critical
  // src, keyed on the candidate the beacon reported as displayed.
  bool HasCriticalSrcsetCandidate(const HtmlElement& element);

  // The initial image url to be used.
  GoogleString blank_image_url_;
  // If non-NULL, we skip rewriting till we reach
  // LazyloadImagesFilter::EndElement(skip_rewrite_).
  HtmlElement* skip_rewrite_;
  // Whether native mode applies to the current request.
  bool native_mode_;
  // Indicates if the main javascript has been inserted into the page.
  bool main_script_inserted_;
  // Indicates whether we should abort rewriting the page (js mode only).
  bool abort_rewrite_;
  // Indicates if the javascript to abort the rewrite has been inserted into
  // the page.
  bool abort_script_inserted_;
  // Whether kLazyloadImagesSkippedCsp has been counted for this document.
  bool csp_skip_counted_;
  // The number of lazily loaded images early since the last time
  // InsertOverrideAttributesScript was called.
  int num_images_lazily_loaded_;
  // The number of otherwise eligible images seen while critical-image data is
  // unavailable; used for the LazyloadImagesSkipFirst LCP protection.
  int num_eligible_images_seen_;

  Variable* lazyload_images_applied_;
  Variable* lazyload_images_native_applied_;
  Variable* lazyload_images_skipped_critical_;
  Variable* lazyload_images_skipped_csp_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_LAZYLOAD_IMAGES_FILTER_H_
