// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Parse the RFC 9421 `Signature-Input` and `Signature` header values into a
// typed, validated SignatureRequest. Enforces the minimal supported profile:
//   * exactly one signature label,
//   * alg == "ed25519",
//   * every covered component in {@method, @authority, @path},
//   * keyid present,
//   * decoded signature exactly 64 bytes.
// Anything outside this profile fails parsing (caller -> Verdict::kUnknown).

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_HEADER_PARSER_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_HEADER_PARSER_H_

#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/sfv.h"

namespace net_instaweb {
namespace webbotauth {

struct SignatureRequest {
  GoogleString label;                 // signature label, e.g. "sig1"
  std::vector<GoogleString> covered;  // component ids, original order
  GoogleString keyid;
  GoogleString alg;     // must be "ed25519"
  int64_t created = 0;  // 0 == absent
  int64_t expires = 0;  // 0 == absent
  bool has_created = false;
  bool has_expires = false;
  GoogleString signature_bytes;  // raw 64 bytes (decoded)
  // The exact textual serialization of the inner-list + params, used to build
  // the @signature-params line byte-exactly (param order preserved).
  GoogleString signature_params_value;
};

// Returns true iff both headers parse and satisfy the supported profile.
// On false, *reason (if non-null) gets a short diagnostic. Never throws.
bool ParseSignatureHeaders(StringPiece signature_input, StringPiece signature,
                           SignatureRequest* out, GoogleString* reason);

// Returns true iff `component_id` is one of the supported derived components.
bool IsSupportedComponent(StringPiece component_id);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_HEADER_PARSER_H_
