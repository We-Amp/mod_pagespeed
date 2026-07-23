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

#include "net/instaweb/rewriter/public/prioritize_critical_images_filter.h"

#include <vector>

#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/request_properties.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/srcset_slot.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/http/data_url.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "pagespeed/opt/logging/log_record.h"

namespace net_instaweb {

const char PrioritizeCriticalImagesFilter::kPrioritizeCriticalImagesApplied[] =
    "prioritize_critical_images_applied";

PrioritizeCriticalImagesFilter::PrioritizeCriticalImagesFilter(
    RewriteDriver* driver)
    : CommonFilter(driver), num_high_priority_images_(0) {
  Statistics* stats = driver->server_context()->statistics();
  prioritize_critical_images_applied_ =
      stats->GetVariable(kPrioritizeCriticalImagesApplied);
}

PrioritizeCriticalImagesFilter::~PrioritizeCriticalImagesFilter() {}

void PrioritizeCriticalImagesFilter::InitStats(Statistics* statistics) {
  statistics->AddVariable(kPrioritizeCriticalImagesApplied);
}

void PrioritizeCriticalImagesFilter::DetermineEnabled(
    GoogleString* disabled_reason) {
  RewriterHtmlApplication::Status status = RewriterHtmlApplication::ACTIVE;
  CriticalImagesFinder* finder =
      driver()->server_context()->critical_images_finder();
  if (finder->Available(driver()) != CriticalImagesFinder::kAvailable) {
    // Strict no-op without beacon data. This covers both kDisabled (data will
    // never come, e.g. beaconing off or unsupported by the platform) and
    // kNoDataYet (the page is instrumented but no beacon has landed).
    // Deliberately NOT copying ImageRewriteFilter's critical-by-default
    // behavior: guessing wrong here would prioritize a below-the-fold image
    // at the expense of the real LCP image.
    status = RewriterHtmlApplication::DISABLED;
    if (disabled_reason != nullptr) {
      *disabled_reason = "no critical images data available";
    }
  } else if (driver()->options()->SupportSaveData()) {
    // The filter's output depends on the request's Save-Data header, so both
    // variants must carry "Vary: Save-Data"; otherwise shared/browser caches
    // can serve the prioritized variant to a Save-Data client, or the
    // untouched variant to everyone else. DetermineEnabled runs before the
    // first flush, so the response headers are still mutable; they may be
    // unwired entirely for non-HTML rewrites.
    ResponseHeaders* headers = driver()->mutable_response_headers();
    if (headers != nullptr &&
        !headers->HasValue(HttpAttributes::kVary, HttpAttributes::kSaveData)) {
      headers->Add(HttpAttributes::kVary, HttpAttributes::kSaveData);
    }
    if (driver()->request_properties()->RequestsSaveData()) {
      // A Save-Data client asked us to conserve bytes; frontloading image
      // fetches works against that request, so stay out of the way.
      status = RewriterHtmlApplication::DISABLED;
      if (disabled_reason != nullptr) {
        *disabled_reason = "request has Save-Data enabled";
      }
    }
  }
  set_is_enabled(status == RewriterHtmlApplication::ACTIVE);
  driver()->log_record()->LogRewriterHtmlStatus(
      RewriteOptions::FilterId(RewriteOptions::kPrioritizeCriticalImages),
      status);
}

void PrioritizeCriticalImagesFilter::StartDocumentImpl() {
  num_high_priority_images_ = 0;
}

void PrioritizeCriticalImagesFilter::EndElementImpl(HtmlElement* element) {
  if (element->keyword() != HtmlName::kImg ||
      !driver()->IsRewritable(element) || noscript_element() != nullptr) {
    return;
  }
  if (num_high_priority_images_ >= kMaxPrioritizedImages) {
    return;
  }
  // AMP documents manage resource loading themselves and reject unexpected
  // attributes in validation; AMP-ness is discovered at the <html> tag, well
  // before any <img>, so this per-element check is reliable.
  if (driver()->is_amp_document()) {
    return;
  }
  // An author-supplied fetchpriority always wins, whatever its value. If the
  // author already marked this image "high" it consumes a slot of the cap:
  // the cap bounds the total number of high-priority images per document,
  // not just the ones this filter adds.
  const HtmlElement::Attribute* author_priority =
      element->FindAttribute(HtmlName::kFetchpriority);
  if (author_priority != nullptr) {
    StringPiece value(author_priority->DecodedValueOrNull());
    if (StringCaseEqual(value, "high")) {
      ++num_high_priority_images_;
    }
    return;
  }
  HtmlElement::Attribute* src = element->FindAttribute(HtmlName::kSrc);
  if (src != nullptr) {
    StringPiece url(src->DecodedValueOrNull());
    if (url.empty() || IsDataUrl(url) || !IsCriticalImageUrl(url)) {
      return;
    }
  } else if (!HasCriticalSrcsetCandidate(*element)) {
    return;
  }
  driver()->AddAttribute(element, HtmlName::kFetchpriority, "high");
  ++num_high_priority_images_;
  prioritize_critical_images_applied_->Add(1);
}

bool PrioritizeCriticalImagesFilter::IsCriticalImageUrl(StringPiece url) {
  // The beacon keys criticality on the full absolute original URL. This
  // filter runs before image rewriting so src is normally still the original,
  // but decode a .pagespeed. URL back to its origin form anyway in case an
  // upstream stage (or the origin HTML itself) carries a rewritten URL.
  GoogleUrl gurl(base_url(), url);
  StringVector decoded_url_vector;
  if (driver()->DecodeUrl(gurl, &decoded_url_vector) &&
      decoded_url_vector.size() == 1) {
    // Only the single-original case can be mapped back; sprites cannot.
    gurl.Reset(decoded_url_vector[0]);
  }
  if (!gurl.IsAnyValid()) {
    return false;
  }
  StringPiece full_url = gurl.Spec();
  if (full_url.empty()) {
    return false;
  }
  if (!driver()->options()->IsAllowed(full_url)) {
    // Do not raise the priority of images with Disallow'ed urls.
    return false;
  }
  CriticalImagesFinder* finder =
      driver()->server_context()->critical_images_finder();
  return finder->IsHtmlCriticalImage(full_url, driver());
}

bool PrioritizeCriticalImagesFilter::HasCriticalSrcsetCandidate(
    const HtmlElement& element) {
  const HtmlElement::Attribute* srcset =
      element.FindAttribute(HtmlName::kSrcset);
  if (srcset == nullptr || srcset->DecodedValueOrNull() == nullptr) {
    return false;
  }
  std::vector<SrcSetSlotCollection::ImageCandidate> candidates;
  SrcSetSlotCollection::ParseSrcSet(srcset->DecodedValueOrNull(), &candidates);
  for (int i = 0, n = candidates.size(); i < n; ++i) {
    if (!candidates[i].url.empty() && IsCriticalImageUrl(candidates[i].url)) {
      return true;
    }
  }
  return false;
}

}  // namespace net_instaweb
