// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_AGENT_MARKDOWN_HEADERS_H_
#define PAGESPEED_SRC_NGINX_AGENT_MARKDOWN_HEADERS_H_

// Headers that describe a Markdown response for agents (the page's Markdown
// copy and /llms.txt).  Pure C++ (no nginx headers), so the values are
// unit-testable without an nginx build.
//
// Both follow the "Markdown for Agents" response contract that agents and
// CDNs already read:
//
//   x-markdown-tokens  an ESTIMATE of the number of tokens in the Markdown
//                      body, so an agent can budget its context before it
//                      reads the body.  The estimate is the body size in
//                      bytes divided by four, rounded down: no tokenizer,
//                      the same estimate that contract documents.  Any real
//                      tokenizer gives a different number; treat the value
//                      as an order of magnitude.
//
//   content-signal     the operator's stated use policy for the content
//                      (contentsignals.org, for example
//                      `search=yes, ai-input=yes, ai-train=no`).  Sent only
//                      when the operator configures one: the module never
//                      states a policy on the operator's behalf.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {

// floor(body_bytes / 4).
uint64_t EstimateMarkdownTokens(size_t body_bytes);

// The x-markdown-tokens value for a body of `body_bytes`: the estimate in
// decimal, no sign, no leading zeros ("0" for a body under four bytes).
std::string MarkdownTokensHeaderValue(size_t body_bytes);

// Longest content-signal value the directive accepts.
inline constexpr size_t kMaxContentSignalBytes = 512;

// True when `value` may be sent as a content-signal field value: 1 to
// kMaxContentSignalBytes bytes of visible ASCII, spaces and tabs, not
// starting or ending with a space or tab.  A value with a line break, a
// control byte or a non-ASCII byte is refused at configuration time.
bool IsValidContentSignalValue(std::string_view value);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_AGENT_MARKDOWN_HEADERS_H_
