// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// pagespeed/kernel/webbotauth/static_key_directory.h @ 8e15a19ce (2026-07-02).
// Keep logic in sync manually; the RFC 9421/8941 core is standards-frozen.
//
// StaticKeyDirectory: a synchronous, in-memory KeyDirectoryProvider backed by
// a single operator-supplied JWKS document (the published signer key
// directory, copied to a local file by the operator). It performs NO network
// I/O and is safe to call directly on the request thread.
//
// In this v1 scope, the FREE verifier resolves keys from an
// operator-local key directory file. Automatic network refresh of the
// directory (a net-fetch provider + an off-request-path warm) is a deliberate
// FOLLOW-UP.
//
// Construction is from the JWKS document STRING (the server wiring reads the
// file and passes the contents), keeping this unit hermetic and trivially
// unit-testable with a literal document and no filesystem.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_STATIC_KEY_DIRECTORY_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_STATIC_KEY_DIRECTORY_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "src/crypto/webbotauth/key_directory.h"

namespace pagespeed {
namespace webbotauth {

// In-memory provider over one JWKS document. `host` and `now_unix_sec` are
// ignored (a single local directory serves every signer the operator trusts;
// there is no TTL because there is no fetch). Total and never throws.
class StaticKeyDirectory : public KeyDirectoryProvider {
 public:
  explicit StaticKeyDirectory(std::string_view jwks_document)
      : jwks_document_(jwks_document) {}

  // kFound + the 32-byte key if `keyid` is a valid OKP/Ed25519 entry in the
  // document; kNotFound otherwise (unknown keyid, malformed doc, bad length).
  // Never returns kError: there is no fetch/SSRF surface to fail.
  KeyLookupResult GetKey(std::string_view host, std::string_view keyid,
                         int64_t now_unix_sec) override;

  bool empty() const { return jwks_document_.empty(); }

 private:
  const std::string jwks_document_;
};

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_STATIC_KEY_DIRECTORY_H_
