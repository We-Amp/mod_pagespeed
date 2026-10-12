// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/rule_pack_signature.h"

#include <openssl/curve25519.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace net_instaweb {

using pagespeed::packs::SignatureStatus;

bool VerifyEd25519(std::string_view message, std::string_view signature,
                   std::string_view public_key) {
  if (signature.size() != pagespeed::packs::kEd25519SignatureBytes ||
      public_key.size() != pagespeed::packs::kEd25519PublicKeyBytes) {
    return false;
  }
  return ED25519_verify(
             reinterpret_cast<const uint8_t*>(message.data()), message.size(),
             reinterpret_cast<const uint8_t*>(signature.data()),
             reinterpret_cast<const uint8_t*>(public_key.data())) == 1;
}

SignatureStatus CheckPackSignature(std::string_view pack_bytes,
                                   std::string_view publisher,
                                   const SidecarState& sidecar,
                                   const pagespeed::packs::PackTrust& trust,
                                   std::string* key_id) {
  if (!sidecar.present) return SignatureStatus::kUnsigned;
  if (!sidecar.read_ok) return SignatureStatus::kUnreadable;
  const std::optional<pagespeed::packs::PackSidecar> parsed =
      pagespeed::packs::ParsePackSidecar(sidecar.text);
  if (!parsed.has_value()) return SignatureStatus::kUnreadable;
  if (parsed->key_id != publisher) return SignatureStatus::kPublisherMismatch;
  const pagespeed::packs::TrustKey* key = trust.Find(parsed->key_id);
  if (key == nullptr) return SignatureStatus::kUnknownKey;
  if (!VerifyEd25519(pack_bytes, parsed->signature, key->public_key)) {
    return SignatureStatus::kBadSignature;
  }
  if (key_id != nullptr) *key_id = parsed->key_id;
  return SignatureStatus::kVerified;
}

}  // namespace net_instaweb
