// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// pagespeed/kernel/webbotauth/static_key_directory.cc @ 8e15a19ce
// (2026-07-02). Keep logic in sync manually; the RFC 9421/8941 core is
// standards-frozen.

#include "src/crypto/webbotauth/static_key_directory.h"

#include <string>
#include <string_view>
#include <utility>

#include "src/crypto/webbotauth/jwks.h"

namespace pagespeed {
namespace webbotauth {

KeyLookupResult StaticKeyDirectory::GetKey(std::string_view /*host*/,
                                           std::string_view keyid,
                                           int64_t /*now_unix_sec*/) {
  if (jwks_document_.empty() || keyid.empty()) {
    return KeyLookupResult::NotFound();
  }
  std::string raw_key_32;
  if (ExtractEd25519Key(jwks_document_, keyid, &raw_key_32)) {
    return KeyLookupResult::Found(std::move(raw_key_32));
  }
  return KeyLookupResult::NotFound();
}

}  // namespace webbotauth
}  // namespace pagespeed
