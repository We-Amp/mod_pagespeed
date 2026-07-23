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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_PRIORITIZE_CRITICAL_IMAGES_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_PRIORITIZE_CRITICAL_IMAGES_FILTER_H_

#include "net/instaweb/rewriter/public/common_filter.h"

namespace net_instaweb {

class HtmlElement;
class RewriteDriver;
class Statistics;
class Variable;

// Adds fetchpriority="high" to the first few <img> elements that the
// critical-images beacon has reported as above the fold, so the browser
// frontloads the fetches that determine Largest Contentful Paint. The filter
// is a strict no-op unless beacon-derived criticality data is available
// (unlike ImageRewriteFilter, which treats unknown criticality as critical):
// wrongly prioritizing a below-the-fold image would slow the images that
// matter, so no data means no rewrite. An author-supplied fetchpriority
// attribute always wins; if its value is "high" it consumes one slot of the
// cap, which bounds the TOTAL number of high-priority images per document.
// The filter injects no scripts and only touches attributes, so it is safe
// under strict CSP. Opt-in: not a member of any rewrite level's filter set.
class PrioritizeCriticalImagesFilter : public CommonFilter {
 public:
  // Counts images this filter added fetchpriority="high" to.
  static const char kPrioritizeCriticalImagesApplied[];

  // Maximum number of high-priority images per document, in document order.
  // fetchpriority="high" is a scarce signal: every prioritized fetch competes
  // with render-critical resources, so only the first couple of beacon-critical
  // images (typically the LCP candidate and its runner-up) are worth it.
  static const int kMaxPrioritizedImages = 2;

  explicit PrioritizeCriticalImagesFilter(RewriteDriver* driver);
  ~PrioritizeCriticalImagesFilter() override;

  static void InitStats(Statistics* statistics);

  void DetermineEnabled(GoogleString* disabled_reason) override;

  void StartDocumentImpl() override;
  void StartElementImpl(HtmlElement* element) override {}
  void EndElementImpl(HtmlElement* element) override;

  const char* Name() const override { return "PrioritizeCriticalImages"; }
  ScriptUsage GetScriptUsage() const override { return kNeverInjectsScripts; }

 private:
  // Returns true if the (possibly rewritten) image URL resolves to a
  // beacon-critical original URL that the options allow.
  bool IsCriticalImageUrl(StringPiece url);

  // Returns true if any srcset candidate of the element is beacon-critical.
  // Consulted only for srcset-only images, whose displayed URL is reachable
  // solely via srcset; the beacon keys those on the candidate the browser
  // actually selected. For images with a src attribute, criticality stays
  // keyed on src (fetchpriority is element-level, so that already raises
  // whichever candidate the browser selects).
  bool HasCriticalSrcsetCandidate(const HtmlElement& element);

  // Number of high-priority images seen so far in this document, counting
  // both images this filter annotated and author-supplied fetchpriority="high"
  // images.
  int num_high_priority_images_;

  Variable* prioritize_critical_images_applied_;

  PrioritizeCriticalImagesFilter(const PrioritizeCriticalImagesFilter&) =
      delete;
  PrioritizeCriticalImagesFilter& operator=(
      const PrioritizeCriticalImagesFilter&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_PRIORITIZE_CRITICAL_IMAGES_FILTER_H_
