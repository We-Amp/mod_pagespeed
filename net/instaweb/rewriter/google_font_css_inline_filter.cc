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
// Filter that inlines small loader CSS files made by Google Font Service.

#include "net/instaweb/rewriter/public/google_font_css_inline_filter.h"

#include "net/instaweb/rewriter/public/google_font_service_input_resource.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/callback.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/http/google_url.h"

namespace net_instaweb {

namespace {

// Host serving the font files referenced by the Font Service loader CSS.
const char kFontFileHost[] = "fonts.gstatic.com";
const char kRelPreconnect[] = "preconnect";

// Returns true if token appears among the space-separated rel tokens held by
// the attribute, matched case-insensitively. HTML link types are a token
// list, so rel="preconnect dns-prefetch" is a valid preconnect; comparing the
// whole attribute value would miss it.
bool HasRelToken(HtmlElement::Attribute* rel, const char* token) {
  StringPieceVector tokens;
  SplitStringPieceToVector(rel->DecodedValueOrNull(), " ", &tokens,
                           true /* skip empty */);
  for (int i = 0, n = tokens.size(); i < n; ++i) {
    if (StringCaseEqual(tokens[i], token)) {
      return true;
    }
  }
  return false;
}

// Returns true if the element's crossorigin attribute puts a preconnect in
// the Anonymous state, the only state that warms a connection the font fetch
// can reuse. Fonts are fetched in CORS mode with same-origin credentials, so
// cross-origin they omit credentials: a missing crossorigin (no CORS) and
// crossorigin="use-credentials" (credentialed) both warm connections the font
// fetch can't use. Anonymous is the attribute's default state, so a
// value-less, empty, or invalid value counts as anonymous.
bool HasAnonymousCrossorigin(HtmlElement* element) {
  HtmlElement::Attribute* crossorigin = element->FindAttribute("crossorigin");
  if (crossorigin == nullptr) {
    return false;
  }
  const char* value = crossorigin->DecodedValueOrNull();
  return value == nullptr || !StringCaseEqual(value, "use-credentials");
}

}  // namespace

GoogleFontCssInlineFilter::GoogleFontCssInlineFilter(RewriteDriver* driver)
    : CssInlineFilter(driver),
      font_css_element_(nullptr),
      preconnect_inserted_(false),
      author_preconnect_seen_(false) {
  set_id(RewriteOptions::kGoogleFontCssInlineId);
  set_size_threshold_bytes(
      driver->options()->google_font_css_inline_max_bytes());
  driver->AddResourceUrlClaimant(NewPermanentCallback(
      this, &GoogleFontCssInlineFilter::CheckIfFontServiceUrl));
}

GoogleFontCssInlineFilter::~GoogleFontCssInlineFilter() {}

void GoogleFontCssInlineFilter::InitStats(Statistics* statistics) {
  GoogleFontServiceInputResource::InitStats(statistics);
}

void GoogleFontCssInlineFilter::StartDocumentImpl() {
  CssInlineFilter::StartDocumentImpl();
  preconnect_inserted_ = false;
  author_preconnect_seen_ = false;
  font_css_element_ = nullptr;
}

void GoogleFontCssInlineFilter::StartElementImpl(HtmlElement* element) {
  CssInlineFilter::StartElementImpl(element);
  if (preconnect_inserted_ || author_preconnect_seen_ ||
      element->keyword() != HtmlName::kLink) {
    return;
  }
  HtmlElement::Attribute* rel = element->FindAttribute(HtmlName::kRel);
  HtmlElement::Attribute* href = element->FindAttribute(HtmlName::kHref);
  if (rel == nullptr || href == nullptr || !HasRelToken(rel, kRelPreconnect) ||
      href->DecodedValueOrNull() == nullptr ||
      // Fonts are fetched in CORS mode, so only a preconnect whose crossorigin
      // resolves to the Anonymous state opens a connection the font fetch can
      // use; anything else must not suppress ours.
      !HasAnonymousCrossorigin(element)) {
    return;
  }
  GoogleUrl url(base_url(), href->DecodedValueOrNull());
  if (url.IsWebValid() && StringCaseEqual(url.Host(), kFontFileHost)) {
    author_preconnect_seen_ = true;
  }
}

void GoogleFontCssInlineFilter::EndElementImpl(HtmlElement* element) {
  // CssInlineFilter::EndElementImpl synchronously reaches CreateResource for
  // candidate elements; remember the element so MaybeInsertPreconnect can
  // place the hint relative to it.
  font_css_element_ = element;
  CssInlineFilter::EndElementImpl(element);
  font_css_element_ = nullptr;
}

ResourcePtr GoogleFontCssInlineFilter::CreateResource(const char* url,
                                                      bool* is_authorized) {
  *is_authorized = true;  // Google font resources don't have to be authorized.
  GoogleUrl abs_url;
  ResolveUrl(url, &abs_url);
  ResourcePtr resource(GoogleFontServiceInputResource::Make(abs_url, driver()));
  if (resource.get() != nullptr) {
    // Insert the hint before the compatibility checks below: when they bail,
    // the surviving <link> still makes the browser fetch font files from
    // fonts.gstatic.com, so the warmed-up connection helps either way.
    MaybeInsertPreconnect(abs_url);

    // Unfortunately some options prevent us from doing anything, since they
    // can make the HTML cached in a way unaware of font UA dependencies.
    const RewriteOptions* options = driver()->options();
    if (!options->modify_caching_headers()) {
      ResetAndExplainReason(
          "Cannot inline font loader CSS when ModifyCachingHeaders is off",
          &resource);
    }

    if (!options->downstream_cache_purge_location_prefix().empty()) {
      ResetAndExplainReason(
          "Cannot inline font loader CSS when using downstream cache",
          &resource);
    }
  }
  return resource;
}

void GoogleFontCssInlineFilter::MaybeInsertPreconnect(
    const GoogleUrl& font_css_url) {
  if (preconnect_inserted_ || author_preconnect_seen_ ||
      font_css_element_ == nullptr) {
    return;
  }
  preconnect_inserted_ = true;
  // The loader CSS references fonts.gstatic.com over the same scheme it was
  // fetched with, so the hint follows the font link's scheme, not the page's.
  HtmlElement* preconnect =
      driver()->NewElement(font_css_element_->parent(), HtmlName::kLink);
  driver()->AddAttribute(preconnect, HtmlName::kRel, kRelPreconnect);
  driver()->AddAttribute(preconnect, HtmlName::kHref,
                         StrCat(font_css_url.Scheme(), "://", kFontFileHost));
  // Fonts are fetched in CORS mode, so only a crossorigin preconnect warms up
  // a usable connection. There is no HtmlName keyword for crossorigin; the
  // string-name overload with a null value emits it value-less.
  driver()->AddAttribute(preconnect, "crossorigin", StringPiece());
  // Insert before the font CSS element itself, not before the current event:
  // we run during that element's end event, so InsertNodeBeforeCurrent would
  // nest the hint inside the <link>, where it gets discarded when inlining
  // replaces the element.
  driver()->InsertNodeBeforeNode(font_css_element_, preconnect);
}

void GoogleFontCssInlineFilter::ResetAndExplainReason(const char* reason,
                                                      ResourcePtr* resource) {
  resource->reset(nullptr);
  if (DebugMode()) {
    // Note that since we only call this after a success of
    // GoogleFontServiceInputResource::Make, this will only be adding comments
    // near font links, and not anything else.
    driver()->InsertComment(reason);
  }
}

void GoogleFontCssInlineFilter::CheckIfFontServiceUrl(const GoogleUrl& url,
                                                      bool* result) {
  *result = GoogleFontServiceInputResource::IsFontServiceUrl(url);
}

}  // namespace net_instaweb
