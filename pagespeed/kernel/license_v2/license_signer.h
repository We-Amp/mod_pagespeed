// GENERATED — DO NOT EDIT BY HAND.
// Vendored from ModPageSpeed 2.0 src/crypto/license_signer.h by tools/sync-crypto.sh.
// Canonical source: github.com/We-Amp/pagespeed-optimizer src/crypto/.
// Synced from commit ebc48ef98186a07300b2f42576e8a14602525368.
// To update: bump PINNED_MPS2_COMMIT in tools/sync-crypto.sh and re-run it.
// Drift guard: the crypto-drift CI check. See the design record.

// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2024-2026 We-Amp B.V.

// and test code; NOT compiled into the nginx module or worker.

#ifndef PAGESPEED_SRC_CRYPTO_LICENSE_SIGNER_H_
#define PAGESPEED_SRC_CRYPTO_LICENSE_SIGNER_H_

#ifndef PAGESPEED_LICENSE_NAMESPACE
#define PAGESPEED_LICENSE_NAMESPACE pagespeed
#endif

#include <string>
#include <string_view>

#include "pagespeed/kernel/license_v2/license_token.h"

namespace PAGESPEED_LICENSE_NAMESPACE {

// Generate an Ed25519 keypair from a 32-byte seed.
// Outputs: |public_key| (32 bytes), |private_key| (64 bytes).
void CreateKeypair(std::string_view seed, std::string* public_key,
                   std::string* private_key);

// Sign a license token.  Returns the base64url-encoded token.
// |private_key| must be exactly 64 bytes (as produced by CreateKeypair).
// |public_key| must be exactly 32 bytes.
std::string SignLicenseToken(const LicensePayload& payload,
                             std::string_view public_key,
                             std::string_view private_key);

}  // namespace PAGESPEED_LICENSE_NAMESPACE

#endif  // PAGESPEED_SRC_CRYPTO_LICENSE_SIGNER_H_
