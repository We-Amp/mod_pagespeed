// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed
// pagespeed/kernel/webbotauth/signature_base.h @ 626ddfa95 (2026-07-03). Keep
// logic in sync manually; the RFC 9421/8941 core is standards-frozen. Extends
// the original core (upstreamed to 1.15): RFC 9421
// section 2.1 HTTP field components (lowercase field names, e.g. "signature-
// agent") in addition to the three derived components — the Web Bot Auth
// architecture draft covers ("@authority" "signature-agent"), which the 1.15
// derived-only profile cannot verify.
//
// RFC 9421 section 2.5 signature-base construction, restricted to the three
// supported derived components (@method, @authority, @path) plus RFC 9421
// section 2.1 HTTP field components resolved against the binding-provided
// field list, plus the trailing @signature-params line. The
// @signature-params line is reproduced byte-exactly with the original
// parameter order preserved, because the signature was computed over that
// exact serialization.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_SIGNATURE_BASE_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_SIGNATURE_BASE_H_

#include <string>
#include <string_view>
#include <vector>

#include "src/crypto/webbotauth/sfv.h"

namespace pagespeed {
namespace webbotauth {

// One HTTP field line the binding wires through for RFC 9421 section 2.1
// field components. `name` is matched ASCII-case-insensitively against the
// (lowercase) covered component id; `value` is the raw field value bytes as
// received on the wire (canonicalization — per-line OWS trim + ", " join of
// repeated lines — happens inside BuildSignatureBase).
struct HeaderField {
  std::string_view name;
  std::string_view value;
};

// A minimal view of the request fields needed to derive component values.
struct BaseRequestView {
  std::string_view method;     // already upper-cased (RFC 9421 @method as-is)
  std::string_view authority;  // Host header value (lower-cased authority)
  std::string_view path;       // absolute path of the request target
  // The HTTP fields the binding exposes as coverable components, in wire
  // order (repeated field lines appear as repeated entries). A covered field
  // component that has no entry here fails base construction (fail-closed) —
  // RFC 9421 requires verification to fail when a covered field is absent.
  std::vector<HeaderField> fields;
};

// ASCII-case-insensitive equality, used for HTTP field-name matching (field
// names are ASCII by construction: covered ids are validated lowercase and
// bindings provide wire header names).
bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b);

// Serialize the @signature-params inner list (components + params) exactly as
// it must appear in the signature base, with parameter order preserved.
// e.g.  ("@method" "@authority" "@path");created=123;keyid="k";alg="ed25519"
std::string SerializeSignatureParams(const SfvInnerList& list);

// Build the full RFC 9421 signature base for the supported derived components
// and HTTP field components plus the trailing @signature-params line.
// `covered` lists the component ids in order; `params_serialization` is the
// byte-exact @signature-params inner list value (from
// SerializeSignatureParams / the original header). Caller must have already
// validated that every covered id is supported (header_parser); this
// function additionally requires every covered FIELD component to resolve
// against `req.fields`. Returns false — and the caller must fail closed —
// when a covered component cannot be resolved (unknown derived component or
// covered field absent from the request). Linear in the input sizes; never
// throws.
bool BuildSignatureBase(const BaseRequestView& req,
                        const std::vector<std::string>& covered,
                        std::string_view params_serialization,
                        std::string* out);

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_SIGNATURE_BASE_H_
