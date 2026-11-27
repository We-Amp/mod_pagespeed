// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed pagespeed/kernel/webbotauth/sfv.h @
// 626ddfa95 (2026-07-03). Keep logic in sync manually; the RFC 9421/8941 core
// is standards-frozen. Extends the original core (upstreamed to 1.15 in
// the 1.15 line): multi-member `Signature-Input` dictionaries
// (ParseSignatureInputDict, bounded) and component-parameter tolerance
// (any_component_params), both needed for Web Bot Auth tag selection when a
// request carries several RFC 9421 signatures (e.g. CDN + bot).
//
// Minimal RFC 8941 Structured-Field-Values parser, restricted to the small
// subset required to read the HTTP Message Signatures (RFC 9421)
// `Signature-Input` and `Signature` headers.  We deliberately do NOT implement
// the full RFC 8941 grammar -- only enough to:
//   * parse the `Signature-Input` value: a Dictionary of one or more Inner
//     Lists of sf-string component identifiers plus parameters (created,
//     keyid, alg, expires, nonce, tag).
//   * parse the `Signature` value: a Dictionary whose member is a Byte Sequence
//     (base64 inside colons, `:...:`).
//
// Anything we cannot parse causes the caller to fail closed
// (Verdict::kUnknown). This parser NEVER throws and NEVER reads out of bounds.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_SFV_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_SFV_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {
namespace webbotauth {

// A single parameter on the inner list, e.g. created=123 or keyid="foo".
// We keep both the typed value and the original textual serialization so the
// signature base (@signature-params) can be reproduced byte-exactly with the
// original parameter order preserved.
struct SfvParam {
  std::string name;  // lower-cased param name as parsed
  // Exactly one of the following is meaningful, selected by `type`.
  enum Type : std::uint8_t {
    kInteger,
    kString,
    kToken,
    kBoolean
  } type = kToken;
  int64_t int_value = 0;   // for kInteger
  std::string str_value;   // for kString / kToken (unquoted value)
  bool bool_value = true;  // for kBoolean
};

// The parsed inner list of a single Signature-Input dictionary member.
struct SfvInnerList {
  // Component identifiers as sf-strings, e.g. "@method", "@authority", "@path".
  // Stored without the surrounding quotes (the raw decoded string value).
  std::vector<std::string> components;
  // Parameters in original order.
  std::vector<SfvParam> params;
  // True when ANY component carried its own parameters (e.g.
  // `"@query-param";name="x"`). The parse stays total (so an unselected
  // dictionary member with component params cannot poison the whole header),
  // but the parameter content is NOT retained: the header parser must reject
  // such a member if it is the one selected for verification.
  bool any_component_params = false;
};

// One `label=(inner list)` member of a Signature-Input dictionary.
struct SfvDictMember {
  std::string label;
  SfvInnerList inner;
};

// Hostile-input bound: the maximum number of Signature-Input dictionary
// members we are willing to parse. Real requests carry one or two signatures
// (bot + CDN); anything beyond this is rejected outright (caller -> kUnknown).
constexpr size_t kMaxSignatureInputMembers = 8;

// Parse a Signature-Input header value into its ordered dictionary members
// (label -> inner list). Bounded by kMaxSignatureInputMembers; duplicate
// labels are rejected (RFC 8941 says last-wins, but a duplicate label in
// signature material is hostile/ambiguous input, so we fail closed instead).
//
// On any malformed input returns false (caller -> kUnknown). Never throws.
bool ParseSignatureInputDict(std::string_view value,
                             std::vector<SfvDictMember>* out);

// Single-label convenience wrapper (the original 1.15 entry point): returns
// true iff exactly one dictionary member was found, it parsed, and none of
// its components carried parameters. `label` receives the member name.
//
// On any malformed input returns false (caller -> kUnknown). Never throws.
bool ParseSignatureInput(std::string_view value, std::string* label,
                         SfvInnerList* out);

// Parse a Signature header value (Dictionary of Byte Sequences) and return the
// raw decoded bytes for the member matching `label`. Returns true iff the label
// exists and its value is a well-formed `:base64:` byte sequence that decoded.
// On any malformed input returns false. Never throws.
bool ParseSignatureBytes(std::string_view value, std::string_view label,
                         std::string* raw_bytes_out);

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_SFV_H_
