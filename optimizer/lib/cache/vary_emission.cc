// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/vary_emission.h"

#include <string_view>

#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"

namespace pagespeed {

std::string_view VaryForServedResponse(ContentType ct,
                                       bool entry_varies_accept) {
  switch (ct) {
    case ContentType::kImage:
      // Sec-CH-DPR drives the density bit (image resize width) independently
      // of User-Agent, so downstream caches must key on it too.  Already
      // names Accept: the origin's own Accept negotiation adds no axis this
      // row was missing, and appending a second `Accept` would be a
      // duplicate list member.  The value is the selection's own header
      // list (capability_mask.h), so the two cannot drift apart.
      return kImageSelectionVary;
    case ContentType::kHtml:
      return entry_varies_accept ? "Accept-Encoding, User-Agent, Accept"
                                 : "Accept-Encoding, User-Agent";
    case ContentType::kCss:
    case ContentType::kJs:
      return entry_varies_accept ? "Accept-Encoding, Accept"
                                 : "Accept-Encoding";
    case ContentType::kOther:
      break;
  }
  // kOther: we negotiate nothing for it, so the table had no row.  The
  // origin's own Accept negotiation still has to be declared — it is the one
  // axis this response genuinely varies on.
  return entry_varies_accept ? "Accept" : std::string_view();
}

bool EmittedVaryNamesAccept(ContentType ct, bool origin_varies_accept,
                            bool origin_line_lists_accept,
                            bool agent_optimize) {
  // An origin line on this response already lists Accept: never a second
  // member.
  if (origin_line_lists_accept) return false;
  return origin_varies_accept || (agent_optimize && ct == ContentType::kHtml);
}

bool VaryListsAccept(std::string_view combined_vary) {
  while (!combined_vary.empty()) {
    const size_t comma = combined_vary.find(',');
    std::string_view member = combined_vary.substr(0, comma);
    combined_vary = comma == std::string_view::npos
                        ? std::string_view()
                        : combined_vary.substr(comma + 1);
    while (!member.empty() &&
           (member.front() == ' ' || member.front() == '\t')) {
      member.remove_prefix(1);
    }
    while (!member.empty() && (member.back() == ' ' || member.back() == '\t')) {
      member.remove_suffix(1);
    }
    constexpr std::string_view kAccept = "accept";
    if (member.size() != kAccept.size()) continue;
    bool same = true;
    for (size_t i = 0; i < member.size(); ++i) {
      char ch = member[i];
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch | 0x20);
      if (ch != kAccept[i]) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }
  return false;
}

}  // namespace pagespeed
