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

//
// HtmlVaryFilter names, in the Vary header of a rewritten HTML response, the
// request headers that the configured image rewriting makes the markup depend
// on. With image optimization on, the image URLs (format, quality, resize
// key) and the inlining decisions in the markup are chosen per client from
// Accept (WebP/AVIF support), Save-Data and User-Agent; without a matching
// Vary a shared cache that stores the HTML can hand one client's markup to
// another.
//
// Vary describes how a URL is selected over its lifetime, so the token list
// is a function of the configuration alone (the rule PrioritizeCritical-
// ImagesFilter follows since #27): it never depends on the request headers,
// the cache state, Cache-Control, ModifyCachingHeaders or downstream-cache
// integration. The filter lives in the shared rewrite chain, so Apache, nginx
// and IIS behave identically; no port code is involved.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_HTML_VARY_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_HTML_VARY_FILTER_H_

#include "net/instaweb/rewriter/public/common_filter.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class HtmlElement;
class RewriteDriver;
class RewriteOptions;

class HtmlVaryFilter : public CommonFilter {
 public:
  explicit HtmlVaryFilter(RewriteDriver* driver);
  ~HtmlVaryFilter() override;

  // Replaces *tokens with the Vary tokens the HTML depends on under these
  // options, in the fixed order Accept, Save-Data, User-Agent, each at most
  // once. Pure: a function of the options only.
  //
  //   Accept      the image rewriter is in the HTML chain and a WebP or AVIF
  //               conversion or recompression filter is enabled.
  //   Save-Data   the image rewriter is in the HTML chain and the Save-Data
  //               qualities differ from the base qualities (SupportSaveData).
  //   User-Agent  the image rewriter is in the HTML chain and a WebP filter
  //               is on (AVIF alone is Accept-only) or small-screen qualities
  //               differ from the base; or, whatever the image chain,
  //               inline_images, delay_images, dedup_inlined_images,
  //               local_storage_cache, lazyload_images,
  //               resize_to_rendered_image_dimensions or
  //               prioritize_critical_images is on (they read the browser or
  //               instrument the critical-image beacon per browser).
  //
  // Never contains Accept-Encoding (compression adds its own) or Sec-CH-DPR
  // (the HTML rewriter does not read it).
  static void HtmlVaryTokens(const RewriteOptions& options,
                             StringVector* tokens);

  // Adds each token of HtmlVaryTokens that the response headers do not
  // already name (case-insensitive), unless the headers carry Vary: *. The
  // tokens it added are recorded on the driver
  // (RewriteDriver::html_vary_tokens_added()). Does nothing when the response
  // headers are not wired, or no longer mutable after the first flush.
  void StartDocumentImpl() override;
  void StartElementImpl(HtmlElement* element) override {}
  void EndElementImpl(HtmlElement* element) override {}

  const char* Name() const override { return "HtmlVary"; }

 private:
  HtmlVaryFilter(const HtmlVaryFilter&) = delete;
  HtmlVaryFilter& operator=(const HtmlVaryFilter&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_HTML_VARY_FILTER_H_
