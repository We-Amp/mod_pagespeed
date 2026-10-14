// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/signature_format.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"

namespace pagespeed::packs {

namespace {

bool IsBase64Char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '+' || c == '/';
}

// `text` is `chars` base64 characters followed by `pad` '=' characters and
// decodes to `decoded_bytes` bytes.
std::optional<std::string> DecodeExact(std::string_view text, size_t chars,
                                       size_t pad, size_t decoded_bytes) {
  if (text.size() != chars + pad) return std::nullopt;
  for (size_t i = 0; i < chars; ++i) {
    if (!IsBase64Char(text[i])) return std::nullopt;
  }
  for (size_t i = chars; i < text.size(); ++i) {
    if (text[i] != '=') return std::nullopt;
  }
  std::string out;
  if (!absl::Base64Unescape(text, &out) || out.size() != decoded_bytes) {
    return std::nullopt;
  }
  return out;
}

}  // namespace

bool IsValidKeyId(std::string_view key_id) {
  if (key_id.empty() || key_id.size() > 64) return false;
  for (char c : key_id) {
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::optional<PackSidecar> ParsePackSidecar(std::string_view text) {
  if (text.size() > kMaxSidecarBytes) return std::nullopt;
  if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
  constexpr std::string_view kTag = "ed25519 ";
  if (text.substr(0, kTag.size()) != kTag) return std::nullopt;
  text.remove_prefix(kTag.size());
  const size_t space = text.find(' ');
  if (space == std::string_view::npos) return std::nullopt;
  const std::string_view key_id = text.substr(0, space);
  if (!IsValidKeyId(key_id)) return std::nullopt;
  std::optional<std::string> sig =
      DecodeExact(text.substr(space + 1), 86, 2, kEd25519SignatureBytes);
  if (!sig.has_value()) return std::nullopt;
  return PackSidecar{std::string(key_id), *std::move(sig)};
}

std::optional<std::string> DecodeTrustKey(std::string_view base64) {
  return DecodeExact(base64, 43, 1, kEd25519PublicKeyBytes);
}

std::vector<TrustKey> WithBuiltInTrustKey(std::vector<TrustKey> configured) {
  auto it = std::lower_bound(
      configured.begin(), configured.end(),
      std::string_view(kBuiltInTrustKeyId),
      [](const TrustKey& k, std::string_view id) { return k.key_id < id; });
  if (it != configured.end() && it->key_id == kBuiltInTrustKeyId) {
    return configured;
  }
  static const std::optional<std::string>* const key =
      new std::optional<std::string>(DecodeTrustKey(kBuiltInTrustKeyBase64));
  if (key->has_value()) {
    configured.insert(it, TrustKey{kBuiltInTrustKeyId, **key});
  }
  return configured;
}

const TrustKey* PackTrust::Find(std::string_view key_id) const {
  auto it = std::lower_bound(
      keys.begin(), keys.end(), key_id,
      [](const TrustKey& k, std::string_view id) { return k.key_id < id; });
  return it != keys.end() && it->key_id == key_id ? &*it : nullptr;
}

std::string PackTrust::Fingerprint() const {
  std::string out = require_signed ? "1|" : "0|";
  for (const TrustKey& k : keys) {
    absl::StrAppend(&out, k.key_id, "=", absl::BytesToHexString(k.public_key),
                    ";");
  }
  return out;
}

std::string_view SignatureStatusPhrase(SignatureStatus status) {
  switch (status) {
    case SignatureStatus::kVerified:
      return "verified";
    case SignatureStatus::kUnsigned:
      return "unsigned";
    case SignatureStatus::kUnreadable:
      return "signature unreadable";
    case SignatureStatus::kPublisherMismatch:
      return "publisher mismatch";
    case SignatureStatus::kUnknownKey:
      return "unknown key";
    case SignatureStatus::kBadSignature:
      return "bad signature";
  }
  return "unknown";
}

}  // namespace pagespeed::packs
