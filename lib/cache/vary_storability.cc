// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/vary_storability.h"

#include <cstddef>
#include <string_view>

namespace pagespeed {
namespace {

// ASCII case-insensitive equality.  Folds only A-Z, never a locale's extra
// case pairs, so a field-name comparison cannot change meaning with the
// process locale — the same fold the front stages have always applied.
bool AsciiEqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    auto ca = static_cast<unsigned char>(a[i]);
    auto cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca | 0x20);
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb | 0x20);
    if (ca != cb) return false;
  }
  return true;
}

}  // namespace

VaryStoreVerdict ClassifyVaryForStore(std::string_view combined_vary) {
  VaryStoreVerdict verdict;
  if (combined_vary.empty()) {
    return verdict;
  }
  std::string_view val(combined_vary);

  // The one field a stored response may vary on (vary_storability.h):
  // case-insensitive match.
  static constexpr std::string_view kAllowed[] = {
      "Accept-Encoding",
  };

  // Split on ',' and check each list member.
  while (!val.empty()) {
    size_t comma = val.find(',');
    std::string_view token;
    if (comma == std::string_view::npos) {
      token = val;
      val = {};
    } else {
      token = val.substr(0, comma);
      val.remove_prefix(comma + 1);
    }
    // Trim OWS: SP (0x20) and HTAB (0x09) per RFC 9110 Section 5.6.1.
    auto is_ows = [](char c) { return c == ' ' || c == '\t'; };
    while (!token.empty() && is_ows(token.front())) token.remove_prefix(1);
    while (!token.empty() && is_ows(token.back())) token.remove_suffix(1);
    if (token.empty()) continue;
    // Vary: * means the response varies on everything (RFC 9110 §12.5.5).
    if (token == "*") {
      verdict.storable = false;
      verdict.varies_accept = false;
      return verdict;
    }

    bool allowed = false;
    for (const auto& a : kAllowed) {
      if (AsciiEqualsIgnoreCase(token, a)) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      // A field outside the allowed set refuses the whole response.  `Accept`
      // is outside it, so `varies_accept` stays false: there is no entry to
      // mark.
      verdict.storable = false;
      verdict.varies_accept = false;
      return verdict;
    }
  }
  return verdict;
}

bool VaryUncacheable(std::string_view combined_vary) {
  // One parse, one tokenizer.  Keeping this as a thin wrapper is what
  // guarantees the refusal the C ABI exports and the refusal the store path
  // applies are the same decision, not two that agree today.
  return !ClassifyVaryForStore(combined_vary).storable;
}

}  // namespace pagespeed
