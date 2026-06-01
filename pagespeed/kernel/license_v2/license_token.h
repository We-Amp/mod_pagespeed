// GENERATED — DO NOT EDIT BY HAND.
// Vendored from ModPageSpeed 2.0 src/crypto/license_token.h by tools/sync-crypto.sh.
// Canonical source: github.com/We-Amp/pagespeed-optimizer src/crypto/.
// Synced from commit ebc48ef98186a07300b2f42576e8a14602525368.
// To update: bump PINNED_MPS2_COMMIT in tools/sync-crypto.sh and re-run it.
// Drift guard: the crypto-drift CI check. See the design record.

// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2024-2026 We-Amp B.V.

// Token format: base64url(64-byte-signature || json-payload)
// Payload: {"sub":"email","iss":"modpagespeed.com","iat":unix_ts,"plan":"pro"}

#ifndef PAGESPEED_SRC_CRYPTO_LICENSE_TOKEN_H_
#define PAGESPEED_SRC_CRYPTO_LICENSE_TOKEN_H_

#ifndef PAGESPEED_LICENSE_NAMESPACE
#define PAGESPEED_LICENSE_NAMESPACE pagespeed
#endif

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PAGESPEED_LICENSE_NAMESPACE {

// Parsed license token payload.
struct LicensePayload {
  std::string sub;   // Subject (customer email)
  std::string iss;   // Issuer (modpagespeed.com)
  int64_t iat = 0;   // Issued-at (Unix timestamp)
  std::string plan;  // Plan name (e.g. "pro")
  int64_t exp = 0;   // Expiry (Unix timestamp, 0 = no expiry, v2)
  std::string sid;   // FastSpring subscription ID (optional, v2)
  std::string kid;   // Key ID for rotation (optional, v2)
  std::vector<std::string> products;  // Product IDs (v3, e.g. ["the 2.0 optimizer line"])
  int32_t max_instances = 0;  // Max concurrent instances (v3, 0 = unlimited)
  // Capability entitlements (v4, the design record D-H, e.g. ["agent_optimize"]).
  // Additive + absence-tolerant: a pre-v4 token parses with an empty list.
  // NOTE: no `{}` default member initializer (clang-tidy
  // readability-redundant-member-init is on, as -Werror) — instead every
  // LicensePayload designated-initializer (tests) lists `.entitlements`
  // explicitly to satisfy -Wmissing-designated-field-initializers.
  std::vector<std::string> entitlements;
};

// Encode bytes to base64url (no padding).
std::string Base64UrlEncode(std::string_view input);

// Decode base64url to bytes.  Returns false on invalid input.
bool Base64UrlDecode(std::string_view input, std::string* output);

// Serialize a LicensePayload to JSON.
std::string SerializePayload(const LicensePayload& payload);

// Parse JSON into a LicensePayload.  Returns false on parse error.
bool ParsePayload(std::string_view json, LicensePayload* payload);

// Build a complete token: base64url(signature || json_payload).
// |signature| must be exactly 64 bytes.
std::string BuildToken(std::string_view signature,
                       std::string_view json_payload);

// Split a token into signature (64 bytes) and JSON payload.
// Returns false if the token is invalid or too short.
bool SplitToken(std::string_view token, std::string* signature,
                std::string* json_payload);

}  // namespace PAGESPEED_LICENSE_NAMESPACE

#endif  // PAGESPEED_SRC_CRYPTO_LICENSE_TOKEN_H_
