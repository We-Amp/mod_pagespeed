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

#include "net/instaweb/rewriter/public/insert_speculation_rules_filter.h"

#include "net/instaweb/rewriter/public/agent_optimize_vary_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/script_tag_scanner.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

InsertSpeculationRulesFilter::InsertSpeculationRulesFilter(
    RewriteDriver* driver)
    : CommonFilter(driver), found_speculation_rules_(false) {}

InsertSpeculationRulesFilter::~InsertSpeculationRulesFilter() {}

void InsertSpeculationRulesFilter::StartDocumentImpl() {
  found_speculation_rules_ = false;
}

void InsertSpeculationRulesFilter::StartElementImpl(HtmlElement* element) {
  if (found_speculation_rules_ || element->keyword() != HtmlName::kScript) {
    return;
  }
  const HtmlElement::Attribute* type_attr =
      element->FindAttribute(HtmlName::kType);
  if (type_attr == nullptr) {
    return;
  }
  StringPiece type_str = type_attr->DecodedValueOrNull();
  if (type_attr->decoding_error() || type_str.data() == nullptr) {
    return;
  }
  // Match the attribute the way browsers (and ScriptTagScanner) do: trim
  // leading/trailing whitespace and compare ASCII-case-insensitively.
  if (ScriptTagScanner::Normalized(type_str) == "speculationrules") {
    found_speculation_rules_ = true;
  }
}

void InsertSpeculationRulesFilter::EndDocument() {
  // <script type="speculationrules"> is not valid in AMP documents (only the
  // AMP runtime and application/ld+json scripts are allowed). The primary
  // protection is kWillInjectScripts: AmpDocumentFilter disables this filter
  // outright on AMP documents before any buffered event is delivered. This
  // early return is defense-in-depth so the filter stays correct even if its
  // declared GetScriptUsage() ever changes.
  if (driver()->is_amp_document()) {
    return;
  }
  // The page already carries its own ruleset; a second one could speculate
  // URLs the author deliberately scoped out.
  if (found_speculation_rules_) {
    return;
  }
  // The ruleset is an inline script; if the page's CSP forbids inline
  // scripts the browser would just block it.
  if (!CspPermitsInlineScript()) {
    return;
  }
  // Every production port attaches response headers to the driver before HTML
  // rewriting, so a null pointer here only occurs in HTML-only test and
  // embedding contexts; deliberately fail open there rather than making every
  // such context synthesize a 200.
  const ResponseHeaders* headers = driver()->response_headers();
  if (headers != nullptr) {
    // Only inject on plain 200 responses; error pages, redirects and other
    // special responses should not trigger speculative loading.
    if (headers->status_code() != HttpStatus::kOK) {
      return;
    }
    // A response that sets cookies or is marked no-store is stateful or
    // sensitive; speculative prefetching such pages risks side effects.
    if (headers->Has(HttpAttributes::kSetCookie) ||
        headers->Has(HttpAttributes::kSetCookie2)) {
      return;
    }
    ConstStringStarVector values;
    if (headers->Lookup(HttpAttributes::kCacheControl, &values)) {
      for (const GoogleString* value : values) {
        if (value != nullptr && StringCaseEqual(*value, "no-store")) {
          return;
        }
      }
    }
  }
  // AI agents negotiating markdown (Accept: text/markdown) don't run script
  // and don't benefit from speculation hints; keep their payload clean. This
  // also keeps the injected tag out of the markdown-negotiated variant that
  // downstream caches key on Vary: Accept.
  if (AgentOptimizeVaryFilter::RequestAcceptsMarkdown(
          driver()->request_headers())) {
    return;
  }
  HtmlElement* script = driver()->NewElement(nullptr, HtmlName::kScript);
  driver()->AddAttribute(script, HtmlName::kType, "speculationrules");
  InsertNodeAtBodyEnd(script);
  HtmlNode* json = driver()->NewCharactersNode(script, kSpeculationRulesJson);
  driver()->AppendChild(script, json);
}

}  // namespace net_instaweb
