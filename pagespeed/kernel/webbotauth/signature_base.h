// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Kept in sync manually with pagespeed-optimizer src/crypto/webbotauth/
// signature_base.h (this change upstreams the optimizer's RFC 9421 section 2.1
// HTTP field components -- lowercase field names, e.g. "signature-agent" --
// in addition to the three derived components; the Web Bot Auth architecture
// draft covers ("@authority" "signature-agent"), which the old derived-only
// profile could not verify).
//
// RFC 9421 section 2.5 signature-base construction, restricted to the three
// supported derived components (@method, @authority, @path) plus RFC 9421
// section 2.1 HTTP field components resolved against the binding-provided
// field list, plus the trailing @signature-params line. The @signature-params
// line is reproduced byte-exactly with the original parameter order
// preserved, because the signature was computed over that exact
// serialization.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_SIGNATURE_BASE_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_SIGNATURE_BASE_H_

#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/sfv.h"

namespace net_instaweb {
namespace webbotauth {

// One HTTP field line the binding wires through for RFC 9421 section 2.1
// field components. `name` is matched ASCII-case-insensitively against the
// (lowercase) covered component id; `value` is the raw field value bytes as
// received on the wire (canonicalization -- per-line OWS trim + ", " join of
// repeated lines -- happens inside BuildSignatureBase).
struct HeaderField {
  StringPiece name;
  StringPiece value;
};

// A minimal view of the request fields needed to derive component values.
struct BaseRequestView {
  StringPiece method;  // already upper-cased (RFC 9421 derives @method as-is)
  StringPiece authority;  // Host header value (lower-cased authority)
  StringPiece path;       // absolute path of the request target
  // The HTTP fields the binding exposes as coverable components, in wire
  // order (repeated field lines appear as repeated entries). A covered field
  // component that has no entry here fails base construction (fail-closed) --
  // RFC 9421 requires verification to fail when a covered field is absent.
  std::vector<HeaderField> fields;
};

// Serialize the @signature-params inner list (components + params) exactly as
// it must appear in the signature base, with parameter order preserved.
// e.g.  ("@method" "@authority" "@path");created=123;keyid="k";alg="ed25519"
GoogleString SerializeSignatureParams(const SfvInnerList& list);

// Build the full RFC 9421 signature base for the supported derived components
// and HTTP field components plus the trailing @signature-params line.
// `covered` lists the component ids in order; `params_serialization` is the
// byte-exact @signature-params inner list value (from
// SerializeSignatureParams / the original header). Caller must have already
// validated that every covered id is supported (header_parser); this
// function additionally requires every covered FIELD component to resolve
// against `req.fields`. Returns false -- and the caller must fail closed --
// when a covered component cannot be resolved (unknown derived component or
// covered field absent from the request). Linear in the input sizes; never
// throws.
bool BuildSignatureBase(const BaseRequestView& req,
                        const std::vector<GoogleString>& covered,
                        StringPiece params_serialization, GoogleString* out);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_SIGNATURE_BASE_H_
