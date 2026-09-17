// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/static_key_directory.h"

#include "pagespeed/kernel/webbotauth/jwks.h"

namespace net_instaweb {
namespace webbotauth {

KeyLookupResult StaticKeyDirectory::GetKey(StringPiece /*host*/,
                                           StringPiece keyid,
                                           int64_t /*now_unix_sec*/) {
  if (jwks_document_.empty() || keyid.empty()) {
    return KeyLookupResult::NotFound();
  }
  GoogleString raw_key_32;
  if (ExtractEd25519Key(jwks_document_, keyid, &raw_key_32)) {
    return KeyLookupResult::Found(std::move(raw_key_32));
  }
  return KeyLookupResult::NotFound();
}

}  // namespace webbotauth
}  // namespace net_instaweb
