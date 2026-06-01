// GENERATED — DO NOT EDIT BY HAND.
// Vendored from ModPageSpeed 2.0 src/crypto/license_verifier.h by tools/sync-crypto.sh.
// Canonical source: github.com/We-Amp/pagespeed-optimizer src/crypto/.
// Synced from commit ebc48ef98186a07300b2f42576e8a14602525368.
// To update: bump PINNED_MPS2_COMMIT in tools/sync-crypto.sh and re-run it.
// Drift guard: the crypto-drift CI check. See the design record.

// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2024-2026 We-Amp B.V.

// Both the nginx module and worker call this on startup; invalid
// tokens produce a warning but do NOT prevent the module from working
// (BSL is the legal enforcement, not technical DRM).

#ifndef PAGESPEED_SRC_CRYPTO_LICENSE_VERIFIER_H_
#define PAGESPEED_SRC_CRYPTO_LICENSE_VERIFIER_H_

#ifndef PAGESPEED_LICENSE_NAMESPACE
#define PAGESPEED_LICENSE_NAMESPACE pagespeed
#endif

#include <cstdint>
#include <string>
#include <string_view>

#include "pagespeed/kernel/license_v2/license_token.h"

namespace PAGESPEED_LICENSE_NAMESPACE {

// Result of license verification.
struct LicenseResult {
  bool valid = false;      // True if signature is cryptographically valid
  bool expired = false;    // True if exp < now (valid but expired)
  int64_t expires_at = 0;  // exp value, 0 if no expiry
  LicensePayload payload;  // Parsed payload (only meaningful if valid)
  std::string error;       // Human-readable error if !valid
};

// Verify a base64url-encoded license token against the embedded
// public key.  Returns a LicenseResult indicating validity.
LicenseResult VerifyLicenseToken(std::string_view token, int64_t now_sec = 0);

// Verify a token using a specific public key (for testing).
// |public_key| must be exactly 32 bytes.
LicenseResult VerifyLicenseTokenWithKey(std::string_view token,
                                        std::string_view public_key,
                                        int64_t now_sec = 0);

// Check if the token's products list includes the given product ID.
// Returns false if products is empty or product_id is not in the list.
bool CheckProductAuthorization(const LicensePayload& payload,
                               std::string_view product_id);

// Check if the token's entitlements list includes the given capability
//.  Returns false if entitlements is
// empty or the capability is not present.
bool CheckEntitlement(const LicensePayload& payload,
                      std::string_view entitlement);

}  // namespace PAGESPEED_LICENSE_NAMESPACE

#endif  // PAGESPEED_SRC_CRYPTO_LICENSE_VERIFIER_H_
