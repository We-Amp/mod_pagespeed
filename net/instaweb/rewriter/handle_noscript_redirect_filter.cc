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

#include "net/instaweb/rewriter/public/handle_noscript_redirect_filter.h"

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"

namespace {
const char kCanonical[] = "canonical";
}  // namespace

namespace net_instaweb {

HandleNoscriptRedirectFilter::HandleNoscriptRedirectFilter(
    RewriteDriver* rewrite_driver)
    : rewrite_driver_(rewrite_driver) {
  Init();
}

HandleNoscriptRedirectFilter::~HandleNoscriptRedirectFilter() {}

void HandleNoscriptRedirectFilter::Init() {
  canonical_present_ = false;
  canonical_inserted_ = false;
}

void HandleNoscriptRedirectFilter::StartDocument() { Init(); }

void HandleNoscriptRedirectFilter::StartElement(HtmlElement* element) {
  if (!canonical_inserted_ && !canonical_present_ &&
      element->keyword() == HtmlName::kLink) {
    // Checks if a <link rel=canonical href=...> is present.
    HtmlElement::Attribute* rel_attr = element->FindAttribute(HtmlName::kRel);
    HtmlElement::Attribute* href_attr = element->FindAttribute(HtmlName::kHref);
    canonical_present_ =
        (rel_attr != nullptr && href_attr != nullptr &&
         StringCaseEqual(rel_attr->DecodedValueOrNull(), kCanonical));
  }
}

void HandleNoscriptRedirectFilter::EndElement(HtmlElement* element) {
  if (!canonical_inserted_ && !canonical_present_ &&
      element->keyword() == HtmlName::kHead) {
    // We insert the <link rel=canonical href=original_url> at the end of the
    // first head, if the first head did not already contain a
    // <link rel=canonical href=...>
    // Use AllExceptQuery() to strip PageSpeed-added query params like
    // ?PageSpeed=noscript from the canonical URL.
    // TODO(sriharis):  Should we check all heads for
    // <link rel=canonical href=...> ?   If we want to do this then if there is
    // no such element, to insert our link element we might need to add a head
    // (since all heads might have been flushed already).
    // Build a real <link> element rather than injecting formatted text, so the
    // href goes through HtmlElement::AddAttribute (which applies the same
    // HtmlKeywords::Escape) and the tag is serialized by HtmlWriterFilter.
    // BRIEF_CLOSE keeps the emitted bytes identical to the old raw-text form.
    HtmlElement* link_element =
        rewrite_driver_->NewElement(element, HtmlName::kLink);
    link_element->set_style(HtmlElement::BRIEF_CLOSE);
    rewrite_driver_->AddAttribute(link_element, HtmlName::kRel, kCanonical);
    rewrite_driver_->AddAttribute(
        link_element, HtmlName::kHref,
        rewrite_driver_->google_url().AllExceptQuery());
    rewrite_driver_->AppendChild(element, link_element);
    canonical_inserted_ = true;
  }
}

}  // namespace net_instaweb
