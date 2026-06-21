// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// RFC 9421 section 2.5 signature-base construction, restricted to the three
// supported derived components (@method, @authority, @path) plus the trailing
// @signature-params line. The @signature-params line is reproduced
// byte-exactly with the original parameter order preserved, because the
// signature was computed over that exact serialization.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_SIGNATURE_BASE_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_SIGNATURE_BASE_H_

#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/sfv.h"

namespace net_instaweb {
namespace webbotauth {

// A minimal view of the request fields needed to derive component values.
struct BaseRequestView {
  StringPiece method;  // already upper-cased (RFC 9421 derives @method as-is)
  StringPiece authority;  // Host header value (lower-cased authority)
  StringPiece path;       // absolute path of the request target
};

// Serialize the @signature-params inner list (components + params) exactly as
// it must appear in the signature base, with parameter order preserved.
// e.g.  ("@method" "@authority" "@path");created=123;keyid="k";alg="ed25519"
GoogleString SerializeSignatureParams(const SfvInnerList& list);

// Build the full RFC 9421 signature base for the supported derived components
// plus the trailing @signature-params line. `covered` lists the component ids
// in order; `params_serialization` is the byte-exact @signature-params inner
// list value (from SerializeSignatureParams / the original header). Returns
// the base string. Caller must have already validated that every covered id is
// supported.
GoogleString BuildSignatureBase(const BaseRequestView& req,
                                const std::vector<GoogleString>& covered,
                                StringPiece params_serialization);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_SIGNATURE_BASE_H_
