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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_INSERT_SPECULATION_RULES_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_INSERT_SPECULATION_RULES_FILTER_H_

#include "net/instaweb/rewriter/public/common_filter.h"

namespace net_instaweb {

class HtmlElement;
class RewriteDriver;

// The ruleset injected by InsertSpeculationRulesFilter: prefetch same-origin
// links with moderate eagerness (the browser prefetches a link when the user
// hovers/starts interacting with it). `href_matches: "/*"` is resolved by the
// browser against the document's own origin, so cross-origin links are never
// speculated. v1 deliberately ships no exclusion patterns; per-URL exclusions
// (e.g. logout links) are a possible future option.
inline constexpr char kSpeculationRulesJson[] =
    "{\"prefetch\":[{\"where\":{\"href_matches\":\"/*\"},"
    "\"eagerness\":\"moderate\"}]}";

// Injects a <script type="speculationrules"> JSON ruleset at the end of the
// body, instructing supporting browsers (Chromium-based; others ignore the
// tag) to prefetch same-origin links with moderate eagerness. The scope is
// deliberately conservative: same-origin document prefetch only, no
// prerender, no exclusions. The filter is opt-in (not part of any rewrite
// level's filter set) and backs off when the page already carries a
// speculationrules script, when the response is not a plain cacheable 200
// (non-OK status, Set-Cookie, Cache-Control: no-store), when the page's CSP
// forbids inline scripts (only checked while HonorCsp is enabled, the
// default), or when the request looks like an AI-agent negotiation
// (Accept: text/markdown).
class InsertSpeculationRulesFilter : public CommonFilter {
 public:
  explicit InsertSpeculationRulesFilter(RewriteDriver* driver);
  ~InsertSpeculationRulesFilter() override;

  void StartDocumentImpl() override;
  void StartElementImpl(HtmlElement* element) override;
  void EndElementImpl(HtmlElement* element) override {}
  void EndDocument() override;

  const char* Name() const override { return "InsertSpeculationRules"; }
  ScriptUsage GetScriptUsage() const override { return kWillInjectScripts; }

 private:
  // Whether the document already contains a <script type=speculationrules>;
  // if so we must not inject a second ruleset.
  bool found_speculation_rules_;

  InsertSpeculationRulesFilter(const InsertSpeculationRulesFilter&) = delete;
  InsertSpeculationRulesFilter& operator=(const InsertSpeculationRulesFilter&) =
      delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_INSERT_SPECULATION_RULES_FILTER_H_
