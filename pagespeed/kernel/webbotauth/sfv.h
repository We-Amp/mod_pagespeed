// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// Minimal RFC 8941 Structured-Field-Values parser, restricted to the small
// subset required to read the HTTP Message Signatures (RFC 9421)
// `Signature-Input` and `Signature` headers.  We deliberately do NOT implement
// the full RFC 8941 grammar -- only enough to:
//   * parse the `Signature-Input` value: a Dictionary whose member is an Inner
//     List of sf-string component identifiers plus parameters (created,
//     keyid, alg, expires).
//   * parse the `Signature` value: a Dictionary whose member is a Byte Sequence
//     (base64 inside colons, `:...:`).
//
// Anything we cannot parse causes the caller to fail closed (Verdict::kUnknown).
// This parser NEVER throws and NEVER reads out of bounds.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_SFV_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_SFV_H_

#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace webbotauth {

// A single parameter on the inner list, e.g. created=123 or keyid="foo".
// We keep both the typed value and the original textual serialization so the
// signature base (@signature-params) can be reproduced byte-exactly with the
// original parameter order preserved.
struct SfvParam {
  GoogleString name;  // lower-cased param name as parsed
  // Exactly one of the following is meaningful, selected by `type`.
  enum Type { kInteger, kString, kToken, kBoolean } type = kToken;
  int64_t int_value = 0;   // for kInteger
  GoogleString str_value;  // for kString / kToken (unquoted value)
  bool bool_value = true;  // for kBoolean
};

// The parsed inner list of a single Signature-Input dictionary member.
struct SfvInnerList {
  // Component identifiers as sf-strings, e.g. "@method", "@authority", "@path".
  // Stored without the surrounding quotes (the raw decoded string value).
  std::vector<GoogleString> components;
  // Parameters in original order.
  std::vector<SfvParam> params;
};

// Parse a Signature-Input header value into (label -> inner list). We only
// support the single-label case (RFC 9421 typically uses one label such as
// `sig1`). Returns true iff exactly one dictionary member was found and parsed
// and it was an inner list with parameters. `label` receives the member name.
//
// On any malformed input returns false (caller -> kUnknown). Never throws.
bool ParseSignatureInput(StringPiece value, GoogleString* label,
                         SfvInnerList* out);

// Parse a Signature header value (Dictionary of Byte Sequences) and return the
// raw decoded bytes for the member matching `label`. Returns true iff the label
// exists and its value is a well-formed `:base64:` byte sequence that decoded.
// On any malformed input returns false. Never throws.
bool ParseSignatureBytes(StringPiece value, StringPiece label,
                         GoogleString* raw_bytes_out);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_SFV_H_
