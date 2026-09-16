// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Kept in sync manually with pagespeed-optimizer src/crypto/webbotauth/
// header_parser.h (this change upstreams the optimizer's RFC 9421 `tag` selection
// -- only signatures tagged "web-bot-auth" are web-bot-auth material; the Web
// Bot Auth architecture draft requires the tag -- multi-signature
// Signature-Input dictionaries (first tagged member wins, deterministically),
// and HTTP field covered components (lowercase field names, e.g.
// "signature-agent")).
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

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_HEADER_PARSER_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_HEADER_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/sfv.h"

namespace net_instaweb {
namespace webbotauth {

// The RFC 9421 signature tag the Web Bot Auth architecture draft requires.
constexpr char kWebBotAuthTag[] = "web-bot-auth";

// Hostile-input bounds for the covered-components list.
constexpr size_t kMaxCoveredComponents = 16;
constexpr size_t kMaxFieldComponentNameLen = 64;

struct SignatureRequest {
  GoogleString label;                 // signature label, e.g. "sig1"
  std::vector<GoogleString> covered;  // component ids, original order
  GoogleString keyid;
  GoogleString alg;     // must be "ed25519"
  GoogleString tag;     // must be "web-bot-auth" (selection criterion)
  int64_t created = 0;  // 0 == absent
  int64_t expires = 0;  // 0 == absent
  bool has_created = false;
  bool has_expires = false;
  GoogleString signature_bytes;  // raw 64 bytes (decoded)
  // The exact textual serialization of the inner-list + params, used to build
  // the @signature-params line byte-exactly (param order preserved).
  GoogleString signature_params_value;
};

enum class ParseStatus : uint8_t {
  kOk,  // a web-bot-auth-tagged signature was selected and validated
  // The headers parsed, but no signature carries tag="web-bot-auth": the
  // request has signature material of some OTHER scheme (e.g. a CDN
  // signature). Not an error -- the caller classifies as if unsigned.
  kNoWebBotAuthSignature,
  kMalformed,  // anything else: malformed input or outside the profile
};

// Parses both headers and selects the FIRST dictionary member whose params
// carry tag="web-bot-auth" (deterministic; at most one signature is ever
// verified per request). On kMalformed, *reason (if non-null) gets a short
// diagnostic. Never throws.
ParseStatus ParseSignatureHeaders(StringPiece signature_input,
                                  StringPiece signature, SignatureRequest* out,
                                  GoogleString* reason);

// Returns true iff `component_id` is one of the supported derived components
// or a valid (lowercase, bounded-length) HTTP field name.
bool IsSupportedComponent(StringPiece component_id);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_HEADER_PARSER_H_
