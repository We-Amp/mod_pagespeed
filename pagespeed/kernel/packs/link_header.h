// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_LINK_HEADER_H_
#define PAGESPEED_KERNEL_PACKS_LINK_HEADER_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {

// One link-value of a `Link` header field (RFC 8288 section 3):
// `<target>; name=value; name="quoted value"`.
struct LinkValue {
  // The URI reference between '<' and '>', as written.
  std::string target;
  // Parameters in order. Names are lowercased; quoted values are unquoted and
  // unescaped. A parameter with no "=" has an empty value.
  std::vector<std::pair<std::string, std::string>> params;
  // The link-value exactly as it appeared (from '<' to the end of its last
  // parameter, without surrounding whitespace), for writing it back.
  std::string raw;

  // The value of the first parameter called `name` (ASCII case-insensitive).
  std::optional<std::string> Param(std::string_view name) const;
  // True when the `rel` parameter lists `token` (whitespace-separated,
  // ASCII case-insensitive), e.g. HasRel("canonical").
  bool HasRel(std::string_view token) const;
};

// Splits one header field value into its link-values. A comma separates
// link-values only outside `<...>` and outside a quoted string, so
// `<https://a.test/x,y>; rel="a,b", <https://a.test/z>` holds two. Text that
// is not a link-value (no leading '<', or an unterminated '<' or quote) is
// skipped up to the next top-level comma; nothing here fails or throws.
//
// When `clean` is given it is set to whether every non-blank piece of the
// field value was parsed; false means some text was skipped, so rebuilding
// the field from the returned link-values would lose it.
std::vector<LinkValue> SplitLinkHeader(std::string_view field_value,
                                       bool* clean = nullptr);

// Percent-encodes the bytes of `url` that may not appear raw in a link
// target: space, control characters, `<`, `>`, `"`, `,`, `;`, `\`, and every
// byte above 0x7E. Existing '%' escapes are kept as they are.
std::string EncodeLinkTarget(std::string_view url);

// `<encoded url>; rel="canonical"`, or nullopt when `url` or the encoded
// target is longer than kMaxUrlBytes.
std::optional<std::string> MakeCanonicalLinkValue(std::string_view url);

// `value` with `token` taken out of its `rel` parameter, written back as a
// link-value (parameters in order, values quoted). Returns the original raw
// text when `value` has no such token, and an empty string when `token` was
// its only `rel` token (nothing is left that the link-value says).
std::string LinkValueWithoutRelToken(const LinkValue& value,
                                     std::string_view token);

// What to do about the `Link: rel=canonical` response header of a page whose
// document ends with one canonical URL.
struct CanonicalLinkEdit {
  // False: nothing to record (and nothing to change).
  bool applicable = false;
  Action action = Action::kNone;
  Reason reason = Reason::kPresent;
  std::string defect;
  // Hash of the first conflicting header value; empty when there is none.
  std::string before_hash;
  // The `Link` field lines after the change. Valid when `mutate`.
  bool mutate = false;
  std::vector<std::string> lines;
  // The change only appends `lines.back()` to the unchanged `lines`.
  bool append_only = false;
};

// Pure. `lines` are the `Link` field values of the response, one per header
// line. `final_norm` / `final_url` are the canonical the document ends with
// (empty: the rule has nothing to say) and `target_norm` the rule's own
// value. Relative targets in the header resolve against `page_url`.
//
//   no canonical in the header   add one when the document's canonical is the
//                                rule's value
//   all equal to the document's  nothing to do (equal)
//   one differs                  `keep` reports it (header_conflict);
//                                `repair` and `replace` take out the
//                                canonical entries that differ (keeping
//                                every other link-value) and add the
//                                document's if none is left
CanonicalLinkEdit PlanCanonicalLinkHeader(const std::vector<std::string>& lines,
                                          std::string_view page_url,
                                          std::string_view final_norm,
                                          std::string_view final_url,
                                          std::string_view target_norm,
                                          OnPresent on_present);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_LINK_HEADER_H_
