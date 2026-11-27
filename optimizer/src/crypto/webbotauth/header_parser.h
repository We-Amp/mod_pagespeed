// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed
// pagespeed/kernel/webbotauth/header_parser.h @ 626ddfa95 (2026-07-03). Keep
// logic in sync manually; the RFC 9421/8941 core is standards-frozen. Extends
// the original core (upstreamed to 1.15): RFC 9421
// `tag` selection (only signatures tagged "web-bot-auth" are web-bot-auth
// material; the Web Bot Auth architecture draft requires the tag), multi-
// signature Signature-Input dictionaries (first tagged member wins,
// deterministically), and HTTP field covered components (lowercase field names,
// e.g. "signature-agent").
//
// Parse the RFC 9421 `Signature-Input` and `Signature` header values into a
// typed, validated SignatureRequest. Enforces the minimal supported profile
// on the SELECTED (first web-bot-auth-tagged) signature:
//   * effective tag == "web-bot-auth" (last tag param wins, RFC 8941),
//   * alg == "ed25519",
//   * every covered component in {@method, @authority, @path} or a valid
//     lowercase HTTP field name (bounded count/length, no duplicates, no
//     component parameters),
//   * "@authority" covered (Web Bot Auth architecture draft section 4.2
//     MUST -- the request-identity anchor against cross-host replay),
//   * keyid present,
//   * decoded signature exactly 64 bytes.
// A parseable header with NO web-bot-auth-tagged signature is NOT an error:
// it is simply not web-bot-auth material (caller -> classified as if
// unsigned). Anything malformed or outside the profile fails parsing
// (caller -> Verdict::kUnknown).

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_HEADER_PARSER_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_HEADER_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "src/crypto/webbotauth/sfv.h"

namespace pagespeed {
namespace webbotauth {

// The RFC 9421 signature tag the Web Bot Auth architecture draft requires.
inline constexpr std::string_view kWebBotAuthTag = "web-bot-auth";

// Hostile-input bounds for the covered-components list.
constexpr size_t kMaxCoveredComponents = 16;
constexpr size_t kMaxFieldComponentNameLen = 64;

struct SignatureRequest {
  std::string label;                 // signature label, e.g. "sig1"
  std::vector<std::string> covered;  // component ids, original order
  std::string keyid;
  std::string alg;      // must be "ed25519"
  std::string tag;      // must be "web-bot-auth" (selection criterion)
  int64_t created = 0;  // 0 == absent
  int64_t expires = 0;  // 0 == absent
  bool has_created = false;
  bool has_expires = false;
  std::string signature_bytes;  // raw 64 bytes (decoded)
  // The exact textual serialization of the inner-list + params, used to build
  // the @signature-params line byte-exactly (param order preserved).
  std::string signature_params_value;
};

enum class ParseStatus : std::uint8_t {
  kOk,  // a web-bot-auth-tagged signature was selected and validated
  // The headers parsed, but no signature carries tag="web-bot-auth": the
  // request has signature material of some OTHER scheme (e.g. a CDN
  // signature). Not an error — the caller classifies as if unsigned.
  kNoWebBotAuthSignature,
  kMalformed,  // anything else: malformed input or outside the profile
};

// Parses both headers and selects the FIRST dictionary member whose params
// carry tag="web-bot-auth" (deterministic; at most one signature is ever
// verified per request). On kMalformed, *reason (if non-null) gets a short
// diagnostic. Never throws.
ParseStatus ParseSignatureHeaders(std::string_view signature_input,
                                  std::string_view signature,
                                  SignatureRequest* out, std::string* reason);

// Returns true iff `component_id` is one of the supported derived components
// or a valid (lowercase, bounded-length) HTTP field name.
bool IsSupportedComponent(std::string_view component_id);

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_HEADER_PARSER_H_
