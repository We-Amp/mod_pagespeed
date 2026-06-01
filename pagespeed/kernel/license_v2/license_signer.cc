// GENERATED — DO NOT EDIT BY HAND.
// Vendored from ModPageSpeed 2.0 src/crypto/license_signer.cc by tools/sync-crypto.sh.
// Canonical source: github.com/We-Amp/pagespeed-optimizer src/crypto/.
// Synced from commit ebc48ef98186a07300b2f42576e8a14602525368.
// To update: bump PINNED_MPS2_COMMIT in tools/sync-crypto.sh and re-run it.
// Drift guard: the crypto-drift CI check. See the design record.

// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/license_v2/license_signer.h"

#include <cstring>
#include <string>
#include <string_view>

#include "ed25519.h"  // NOLINT(build/include_subdir)
#include "pagespeed/kernel/license_v2/license_token.h"

#ifndef PAGESPEED_LICENSE_NAMESPACE
#define PAGESPEED_LICENSE_NAMESPACE pagespeed
#endif

namespace PAGESPEED_LICENSE_NAMESPACE {

void CreateKeypair(std::string_view seed, std::string* public_key,
                   std::string* private_key) {
  if (seed.size() < 32) return;  // ed25519 requires a 32-byte seed.

  unsigned char pk[32];
  unsigned char sk[64];

  ed25519_create_keypair(pk, sk,
                         reinterpret_cast<const unsigned char*>(seed.data()));

  public_key->assign(reinterpret_cast<const char*>(pk), 32);
  private_key->assign(reinterpret_cast<const char*>(sk), 64);
}

std::string SignLicenseToken(const LicensePayload& payload,
                             std::string_view public_key,
                             std::string_view private_key) {
  if (public_key.size() != 32 || private_key.size() != 64) return {};

  std::string json = SerializePayload(payload);

  unsigned char signature[64];
  ed25519_sign(signature, reinterpret_cast<const unsigned char*>(json.data()),
               json.size(),
               reinterpret_cast<const unsigned char*>(public_key.data()),
               reinterpret_cast<const unsigned char*>(private_key.data()));

  return BuildToken(
      std::string_view(reinterpret_cast<const char*>(signature), 64), json);
}

}  // namespace PAGESPEED_LICENSE_NAMESPACE
