// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed pagespeed/kernel/webbotauth/jwks.h
// @ 8e15a19ce (2026-07-02). Keep logic in sync manually; the RFC 9421/8941
// core is standards-frozen.
//
// Parse a JWKS document (`{"keys":[ ... ]}`) or a single JWK and extract the
// raw 32-byte Ed25519 public key for a given `kid`. Only OKP/Ed25519 keys are
// accepted; the `x` member is base64url-decoded to exactly 32 bytes.
//
// A small, tolerant JSON scanner restricted to the JWK shape is used (objects,
// string members, arrays of objects). It never throws and never reads out of
// bounds; anything it cannot parse yields a not-found result.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_JWKS_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_JWKS_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pagespeed {
namespace webbotauth {

// On success, *raw_key_32 receives the 32-byte Ed25519 public key and returns
// true. Returns false if no matching OKP/Ed25519 key with the given kid is
// found, or the document is malformed, or `x` does not decode to 32 bytes.
bool ExtractEd25519Key(std::string_view jwks_document, std::string_view kid,
                       std::string* raw_key_32);

// Enumerate EVERY kid-bearing OKP/Ed25519 key in `jwks_document`, appending
// each as a (kid, raw_key_32) pair to *out. This is the
// whole-directory-per-host read the background warmer uses to
// populate the key store from one fetch (D2): the request path then resolves
// each kid store-only, never fetching.
//
// kid-LESS entries are intentionally SKIPPED: a store entry is keyed by
// (host, kid), so a key with no kid cannot be pre-populated under the keyid
// the request will ask for. The kid-less single-JWK operator convenience
// remains available via the local-file StaticKeyDirectory fallback, not via
// warm-fetch. Malformed entries, non-OKP/Ed25519 keys, and `x` values that do
// not decode to 32 bytes are skipped. Never throws. Returns the number of keys
// appended.
size_t ExtractAllEd25519Keys(
    std::string_view jwks_document,
    std::vector<std::pair<std::string, std::string>>* out);

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_JWKS_H_
