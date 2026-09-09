// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// StaticKeyDirectory: a synchronous, in-memory KeyDirectoryProvider backed by a
// single operator-supplied JWKS document (the published signer key directory,
// copied to a local file by the operator). It performs NO network I/O and is
// safe to call directly on the request thread -- unlike NetFetchKeyDirectory,
// whose async fetch cannot complete inline in an nginx phase handler.
//
// v1 scope: the FREE verifier resolves keys from an operator-local key
// directory file. Automatic network refresh of the directory
// (NetFetchKeyDirectory + an off-request-path warm) is a deliberate FOLLOW-UP.
//
// Construction is from the JWKS document STRING (the nginx wiring reads the file
// via the engine FileSystem and passes the contents), keeping this unit hermetic
// and trivially unit-testable with a literal document and no filesystem.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_STATIC_KEY_DIRECTORY_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_STATIC_KEY_DIRECTORY_H_

#include <cstdint>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"

namespace net_instaweb {
namespace webbotauth {

// In-memory provider over one JWKS document. `host` and `now_unix_sec` are
// ignored (a single local directory serves every signer the operator trusts;
// there is no TTL because there is no fetch). Total and never throws.
class StaticKeyDirectory : public KeyDirectoryProvider {
 public:
  explicit StaticKeyDirectory(StringPiece jwks_document)
      : jwks_document_(jwks_document.data(), jwks_document.size()) {}

  // kFound + the 32-byte key if `keyid` is a valid OKP/Ed25519 entry in the
  // document; kNotFound otherwise (unknown keyid, malformed doc, bad length).
  // Never returns kError: there is no fetch/SSRF surface to fail.
  KeyLookupResult GetKey(StringPiece host, StringPiece keyid,
                         int64_t now_unix_sec) override;

  bool empty() const { return jwks_document_.empty(); }

 private:
  const GoogleString jwks_document_;
};

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_STATIC_KEY_DIRECTORY_H_
