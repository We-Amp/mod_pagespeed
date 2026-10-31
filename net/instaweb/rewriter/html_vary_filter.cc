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

#include "net/instaweb/rewriter/public/html_vary_filter.h"

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

HtmlVaryFilter::HtmlVaryFilter(RewriteDriver* driver) : CommonFilter(driver) {}

HtmlVaryFilter::~HtmlVaryFilter() {}

void HtmlVaryFilter::HtmlVaryTokens(const RewriteOptions& options,
                                    StringVector* tokens) {
  tokens->clear();
  // The image tokens apply when the image filter can change HTML URLs. With
  // preserved URLs it only warms the in-place cache, so the markup does not
  // depend on Accept or Save-Data (the filter being in the chain is not enough).
  const bool images = options.ImageRewriteFilterInHtmlChain() &&
                      options.ImageUrlsCanChangeInHtml();
  // WebP is also granted by User-Agent (the navigation-Accept fallback and the
  // legacy browser); AVIF is Accept-only.
  const bool webp = options.Enabled(RewriteOptions::kConvertJpegToWebp) ||
                    options.Enabled(RewriteOptions::kConvertToWebpLossless) ||
                    options.Enabled(RewriteOptions::kConvertToWebpAnimated) ||
                    options.Enabled(RewriteOptions::kRecompressWebp);
  const bool avif = options.Enabled(RewriteOptions::kConvertJpegToAvif) ||
                    options.Enabled(RewriteOptions::kConvertToAvifLossless) ||
                    options.Enabled(RewriteOptions::kConvertToAvifAnimated) ||
                    options.Enabled(RewriteOptions::kRecompressAvif);
  // inline_images, delay_images and dedup_inlined_images
  // read the User-Agent (inlining support, low-res previews, device class, the
  // lazyload allowlist, the noscript redirect) and instrument the
  // critical-image beacon per browser, so they count even when image URLs are
  // preserved.
  const bool user_agent =
      (images && (webp || options.HasValidSmallScreenQualities())) ||
      options.Enabled(RewriteOptions::kInlineImages) ||
      options.Enabled(RewriteOptions::kDelayImages) ||
      options.Enabled(RewriteOptions::kDedupInlinedImages) ||
      options.Enabled(RewriteOptions::kLazyloadImages) ||
      options.Enabled(RewriteOptions::kResizeToRenderedImageDimensions) ||
      options.Enabled(RewriteOptions::kPrioritizeCriticalImages);

  if (images && (webp || avif)) {
    tokens->push_back(HttpAttributes::kAccept);
  }
  if (images && options.SupportSaveData()) {
    tokens->push_back(HttpAttributes::kSaveData);
  }
  if (user_agent) {
    tokens->push_back(HttpAttributes::kUserAgent);
  }
}

void HtmlVaryFilter::StartDocumentImpl() { AddVaryTokens(driver()); }

void HtmlVaryFilter::AddVaryTokens(RewriteDriver* driver) {
  // Response headers are mutable only until the first flush, and may be
  // unwired entirely (non-HTML rewrites, some tests).
  ResponseHeaders* headers = driver->mutable_response_headers();
  if (headers == nullptr) {
    return;
  }
  // Vary: * already says everything; adding named tokens would only narrow it.
  if (headers->HasValueCaseInsensitive(HttpAttributes::kVary, "*")) {
    return;
  }
  StringVector tokens;
  HtmlVaryTokens(*driver->options(), &tokens);
  bool added_token = false;
  for (const GoogleString& token : tokens) {
    if (!headers->HasValueCaseInsensitive(HttpAttributes::kVary, token)) {
      headers->Add(HttpAttributes::kVary, token);
      driver->AddHtmlVaryTokenAdded(token);
      added_token = true;
    }
  }
  // Add() marks the precomputed caching fields dirty, and the document's
  // headers were already computed before the parse began (e.g. by
  // ProxyFetch::AddPagespeedHeader). Restore the invariant that mutator
  // establishes, so later readers of the caching fields (IsBrowserCacheable
  // and friends) see valid data rather than tripping their DCHECKs. The
  // tokens are named values and never '*', which is the only Vary input
  // ComputeCaching() consults, so this recompute changes no cached value.
  if (added_token) {
    headers->ComputeCaching();
  }
}

}  // namespace net_instaweb
