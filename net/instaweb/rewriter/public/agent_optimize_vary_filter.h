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
// the design record: the AgentOptimizeVaryFilter advertises `Vary: Accept` on an HTML
// response when an agent_optimize negotiation is active and entitled. mod_
// pagespeed 1.1 has NO markdown render moat (no headless browser), so the
// response BODY is never changed and no markdown is ever served — this filter
// is the negotiation/cloaking-safety signal only. It lives in the shared
// rewrite filter chain so all four ports (Apache/nginx/Envoy/IIS) behave
// identically; the per-port code only sources the Accept header.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_AGENT_OPTIMIZE_VARY_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_AGENT_OPTIMIZE_VARY_FILTER_H_

#include "net/instaweb/rewriter/public/common_filter.h"

namespace net_instaweb {

class HtmlElement;
class RequestHeaders;
class RewriteDriver;

// Adds `Vary: Accept` to the HTML response when:
//   options()->agent_optimize()  (gates this filter's presence, set at AddFilters)
//   AND server_context()->IsAgentOptimizeEntitled()  (license entitlement)
//   AND the request's Accept header contains the text/markdown token.
// Never modifies the body. Idempotent (won't duplicate an existing Vary: Accept).
class AgentOptimizeVaryFilter : public CommonFilter {
 public:
  explicit AgentOptimizeVaryFilter(RewriteDriver* driver);
  ~AgentOptimizeVaryFilter() override;

  void StartDocumentImpl() override;
  void StartElementImpl(HtmlElement* element) override {}
  void EndElementImpl(HtmlElement* element) override {}

  const char* Name() const override { return "AgentOptimizeVary"; }

  // Returns true if any of the request's Accept header values contains the
  // `text/markdown` media-range token — the agent_optimize negotiation
  // signal. Null request_headers (or no Accept header) returns false.
  static bool RequestAcceptsMarkdown(const RequestHeaders* request_headers);

 private:
  AgentOptimizeVaryFilter(const AgentOptimizeVaryFilter&) = delete;
  AgentOptimizeVaryFilter& operator=(const AgentOptimizeVaryFilter&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_AGENT_OPTIMIZE_VARY_FILTER_H_
