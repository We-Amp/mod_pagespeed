// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Parse a JWKS document (`{"keys":[ ... ]}`) or a single JWK and extract the
// raw 32-byte Ed25519 public key for a given `kid`. Only OKP/Ed25519 keys are
// accepted; the `x` member is base64url-decoded to exactly 32 bytes.
//
// A small, tolerant JSON scanner restricted to the JWK shape is used (objects,
// string members, arrays of objects). It never throws and never reads out of
// bounds; anything it cannot parse yields a not-found result.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_JWKS_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_JWKS_H_

#include <utility>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace webbotauth {

// On success, *raw_key_32 receives the 32-byte Ed25519 public key and returns
// true. Returns false if no matching OKP/Ed25519 key with the given kid is
// found, or the document is malformed, or `x` does not decode to 32 bytes.
bool ExtractEd25519Key(StringPiece jwks_document, StringPiece kid,
                       GoogleString* raw_key_32);

// Enumerate EVERY kid-bearing OKP/Ed25519 key in `jwks_document`, appending each
// as a (kid, raw_key_32) pair to *out. This is the whole-directory-per-host read
// the background warmer uses to populate the cache from one fetch
// (D2): the request path then resolves each kid cache-only, never fetching.
//
// kid-LESS entries are intentionally SKIPPED: a cache entry is keyed by
// (host, kid), so a key with no kid cannot be pre-populated under the keyid the
// request will ask for. The kid-less single-JWK operator convenience remains
// available via the local-file StaticKeyDirectory fallback, not via warm-fetch.
// Malformed entries, non-OKP/Ed25519 keys, and `x` values that do not decode to
// 32 bytes are skipped. Never throws. Returns the number of keys appended.
size_t ExtractAllEd25519Keys(
    StringPiece jwks_document,
    std::vector<std::pair<GoogleString, GoogleString> >* out);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_JWKS_H_
