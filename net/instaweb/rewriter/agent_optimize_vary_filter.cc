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

#include "net/instaweb/rewriter/public/agent_optimize_vary_filter.h"

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

namespace {

// Mirror of MPS 2.0's AcceptContains (capability_mask.cc): the `text/markdown`
// media-range present as a comma/space/tab-delimited token (tolerating a
// trailing `;q=...`), never matched by `*/*`. Kept byte-for-byte in lockstep
// with 2.0 so the agent negotiation signal is identical across products.
bool AcceptContainsMarkdown(StringPiece accept) {
  const StringPiece kMarkdown("text/markdown");
  size_t pos = 0;
  while (pos < accept.size()) {
    size_t found = accept.find(kMarkdown, pos);
    if (found == StringPiece::npos) {
      return false;
    }
    // Left boundary: start-of-string, comma, or whitespace.
    bool left_ok = (found == 0) || accept[found - 1] == ',' ||
                   accept[found - 1] == ' ' || accept[found - 1] == '\t';
    // Right boundary: end-of-string, comma, semicolon (q-params), whitespace.
    size_t end = found + kMarkdown.size();
    bool right_ok = (end >= accept.size()) || accept[end] == ',' ||
                    accept[end] == ';' || accept[end] == ' ' ||
                    accept[end] == '\t';
    if (left_ok && right_ok) {
      return true;
    }
    pos = found + 1;
  }
  return false;
}

}  // namespace

AgentOptimizeVaryFilter::AgentOptimizeVaryFilter(RewriteDriver* driver)
    : CommonFilter(driver) {}

AgentOptimizeVaryFilter::~AgentOptimizeVaryFilter() {}

void AgentOptimizeVaryFilter::StartDocumentImpl() {
  // Response headers are mutable only until the first Flush; after that
  // mutable_response_headers() returns NULL and we must not touch them.
  ResponseHeaders* headers = driver()->mutable_response_headers();
  if (headers == nullptr) {
    return;
  }

  // This filter is only added when options()->agent_optimize() is set (see
  // rewrite_driver_filter_init.cc). The remaining gate: the server-wide
  // agent_optimize license entitlement AND an Accept: text/markdown request.
  // 1.1 never serves markdown — the sole effect here is the Vary: Accept signal.
  if (!driver()->server_context()->IsAgentOptimizeEntitled()) {
    return;
  }

  const RequestHeaders* request_headers = driver()->request_headers();
  if (request_headers == nullptr) {
    return;
  }

  ConstStringStarVector accept_values;
  if (!request_headers->Lookup(HttpAttributes::kAccept, &accept_values)) {
    return;
  }
  bool wants_markdown = false;
  for (const GoogleString* value : accept_values) {
    if (value != nullptr && AcceptContainsMarkdown(*value)) {
      wants_markdown = true;
      break;
    }
  }
  if (!wants_markdown) {
    return;
  }

  // Idempotent: never duplicate an existing Vary: Accept (e.g. one already added
  // for image/WebP negotiation).
  if (!headers->HasValue(HttpAttributes::kVary, HttpAttributes::kAccept)) {
    headers->Add(HttpAttributes::kVary, HttpAttributes::kAccept);
  }
}

}  // namespace net_instaweb
