// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CSP inline-injection policy helper implementation

#include "lib/html/csp_inline_policy.h"

#include <string_view>

#include "lib/base/string_util.h"

namespace net_instaweb {

namespace {

// Scans a single (already-resolved) CSP directive value's source list and
// decides whether an inline element governed by it would be HONORED.
//
// The token scanner mirrors ParseCspDirectiveValue in
// subresource_collector_filter.cc, but the DECISION here is different: the
// collector treats a nonce/hash as "not restrictive" for its own bookkeeping,
// whereas we care whether an inline element WE inject (which carries no nonce
// and whose hash we cannot know) would run. A nonce or hash in the source list
// makes the browser IGNORE 'unsafe-inline' (CSP3), so it NEUTRALIZES our
// injection just as surely as its absence would.
//
// Rules:
//   no 'unsafe-inline'                        -> false (blocked)
//   'unsafe-inline' + any nonce/hash present  -> false (unsafe-inline ignored)
//   'unsafe-inline' + no nonce/hash           -> true  (honored)
//
// `is_script`: when true (the script-src chain), 'strict-dynamic' ALSO
// neutralizes 'unsafe-inline' (and host/scheme allowlists) per CSP3, so a
// nonce-less inline <script> we inject is dropped. 'strict-dynamic' is
// meaningless for style-src, so it must NOT affect the style chain.
bool InlineHonoredByValue(std::string_view value, bool is_script) {
  bool has_nonce = false;
  bool has_hash = false;
  bool has_unsafe_inline = false;
  bool has_strict_dynamic = false;

  size_t i = 0;
  while (i < value.size()) {
    if (IsHtmlSpace(value[i])) {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < value.size() && !IsHtmlSpace(value[i])) {
      ++i;
    }
    std::string_view token = value.substr(start, i - start);

    if (StringCaseStartsWith(token, "'nonce-")) {
      has_nonce = true;
    } else if (StringCaseStartsWith(token, "'sha256-") ||
               StringCaseStartsWith(token, "'sha384-") ||
               StringCaseStartsWith(token, "'sha512-")) {
      has_hash = true;
    } else if (StringCaseEqual(token, "'unsafe-inline'")) {
      has_unsafe_inline = true;
    } else if (StringCaseEqual(token, "'strict-dynamic'")) {
      has_strict_dynamic = true;
    }
  }

  if (!has_unsafe_inline) return false;
  // 'unsafe-inline' is present but ignored by the browser when a nonce or hash
  // source is also specified for the directive.
  if (has_nonce || has_hash) return false;
  // For script-src, 'strict-dynamic' likewise makes the browser ignore
  // 'unsafe-inline' (style-src has no such interaction).
  if (is_script && has_strict_dynamic) return false;
  return true;
}

// Resolves the CSP3 fallback chain for a given kind of inline content and
// returns whether inline injection would be honored.
//   elem_name: the most-specific directive (e.g. "style-src-elem")
//   base_name: the mid directive (e.g. "style-src")
//   default-src is always the final fallback.
// If NONE of the three directives is present, inline is allowed (no policy
// governs it). This also covers the empty-CSP case.
bool InlineAllowedForChain(std::string_view csp, std::string_view elem_name,
                           std::string_view base_name, bool is_script) {
  bool found_elem = false;
  bool found_base = false;
  bool found_default = false;
  std::string_view elem_value;
  std::string_view base_value;
  std::string_view default_value;

  size_t pos = 0;
  while (pos < csp.size()) {
    size_t end = csp.find(';', pos);
    if (end == std::string_view::npos) {
      end = csp.size();
    }
    std::string_view directive = csp.substr(pos, end - pos);
    pos = end + 1;

    // Trim leading/trailing HTML whitespace from the directive.
    size_t ds = 0;
    while (ds < directive.size() && IsHtmlSpace(directive[ds])) ++ds;
    size_t de = directive.size();
    while (de > ds && IsHtmlSpace(directive[de - 1])) --de;
    directive = directive.substr(ds, de - ds);

    if (directive.empty()) continue;

    // Split directive name from value at the first whitespace.
    size_t space = 0;
    while (space < directive.size() && !IsHtmlSpace(directive[space])) {
      ++space;
    }
    std::string_view name = directive.substr(0, space);
    std::string_view value;
    if (space < directive.size()) {
      size_t vs = space;
      while (vs < directive.size() && IsHtmlSpace(directive[vs])) ++vs;
      value = directive.substr(vs);
    }

    // Per CSP spec, a repeated directive keeps the FIRST occurrence and the
    // user agent ignores later duplicates — so only capture if not yet seen.
    if (StringCaseEqual(name, elem_name)) {
      if (!found_elem) {
        found_elem = true;
        elem_value = value;
      }
    } else if (StringCaseEqual(name, base_name)) {
      if (!found_base) {
        found_base = true;
        base_value = value;
      }
    } else if (StringCaseEqual(name, "default-src")) {
      if (!found_default) {
        found_default = true;
        default_value = value;
      }
    }
  }

  std::string_view effective_value;
  if (found_elem) {
    effective_value = elem_value;
  } else if (found_base) {
    effective_value = base_value;
  } else if (found_default) {
    effective_value = default_value;
  } else {
    // No governing directive present — inline is unrestricted.
    return true;
  }

  return InlineHonoredByValue(effective_value, is_script);
}

// A CSP string may carry a comma-separated LIST of policies (header-list
// semantics: "policyA, policyB"), and Chromium enforces every member of the
// list even when it arrives via a single <meta> tag. Inline is therefore
// honored only if EVERY policy in the list allows it — evaluating the list as
// one policy would treat a later policy's 'unsafe-inline' as part of an
// earlier restrictive directive's source list and fail open. A ',' cannot
// occur inside a serialized policy (directive names, keywords, base64 nonce/
// hash values, and host-sources all exclude it), so a flat split is exact.
bool InlineAllowedForList(std::string_view csp, std::string_view elem_name,
                          std::string_view base_name, bool is_script) {
  size_t pos = 0;
  for (;;) {
    size_t end = csp.find(',', pos);
    if (end == std::string_view::npos) {
      end = csp.size();
    }
    std::string_view policy = csp.substr(pos, end - pos);
    if (!InlineAllowedForChain(policy, elem_name, base_name, is_script)) {
      return false;
    }
    if (end == csp.size()) {
      return true;
    }
    pos = end + 1;
  }
}

}  // namespace

bool InlineStyleAllowed(std::string_view csp) {
  return InlineAllowedForList(csp, "style-src-elem", "style-src",
                              /*is_script=*/false);
}

bool InlineScriptAllowed(std::string_view csp) {
  return InlineAllowedForList(csp, "script-src-elem", "script-src",
                              /*is_script=*/true);
}

}  // namespace net_instaweb
