// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_SIGNATURE_FORMAT_H_
#define PAGESPEED_KERNEL_PACKS_SIGNATURE_FORMAT_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed::packs {

// The text formats of pack signing: the detached signature sidecar and the
// trust keys. No cryptography here; the server verifies the signature
// (pagespeed/system/rule_pack_signature.h).

inline constexpr size_t kMaxSidecarBytes = 1024;
inline constexpr size_t kEd25519PublicKeyBytes = 32;
inline constexpr size_t kEd25519SignatureBytes = 64;
inline constexpr size_t kMaxTrustKeysPerScope = 16;

// [a-z0-9-]{1,64}
bool IsValidKeyId(std::string_view key_id);

struct PackSidecar {
  std::string key_id;
  std::string signature;  // 64 raw bytes
};
// Exactly "ed25519 <key_id> <86 base64 chars>==" with an optional single
// trailing '\n'; at most kMaxSidecarBytes. nullopt = "signature unreadable".
std::optional<PackSidecar> ParsePackSidecar(std::string_view text);

// Exactly 43 base64 chars + "=", decoding to 32 bytes; nullopt otherwise.
std::optional<std::string> DecodeTrustKey(std::string_view base64);

struct TrustKey {
  std::string key_id;
  std::string public_key;  // 32 raw bytes
};
struct PackTrust {
  bool require_signed = false;
  std::vector<TrustKey> keys;  // sorted by key_id, unique

  const TrustKey* Find(std::string_view key_id) const;
  // Identifies this configuration in the store's caches: "1|" or "0|", then
  // "<key_id>=<lowercase hex of the key>;" per key.
  std::string Fingerprint() const;
};

enum class SignatureStatus {
  kVerified,
  kUnsigned,
  kUnreadable,
  kPublisherMismatch,
  kUnknownKey,
  kBadSignature,
};
// "verified", "unsigned", "signature unreadable", "publisher mismatch",
// "unknown key", "bad signature"
std::string_view SignatureStatusPhrase(SignatureStatus status);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_SIGNATURE_FORMAT_H_
