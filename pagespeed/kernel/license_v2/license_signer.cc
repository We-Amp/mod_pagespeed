// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/license_v2/license_signer.h"

#include <cstring>

#include "ed25519.h"  // NOLINT(build/include_subdir)
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/license_v2/license_token.h"

namespace net_instaweb {

void CreateKeypair(StringPiece seed, GoogleString* public_key,
                   GoogleString* private_key) {
  if (seed.size() < 32) return;  // ed25519 requires a 32-byte seed.

  unsigned char pk[32];
  unsigned char sk[64];

  ed25519_create_keypair(pk, sk,
                         reinterpret_cast<const unsigned char*>(seed.data()));

  public_key->assign(reinterpret_cast<const char*>(pk), 32);
  private_key->assign(reinterpret_cast<const char*>(sk), 64);
}

GoogleString SignLicenseToken(const LicensePayload& payload,
                              StringPiece public_key,
                              StringPiece private_key) {
  if (public_key.size() != 32 || private_key.size() != 64) return {};

  GoogleString json = SerializePayload(payload);

  unsigned char signature[64];
  ed25519_sign(signature, reinterpret_cast<const unsigned char*>(json.data()),
               json.size(),
               reinterpret_cast<const unsigned char*>(public_key.data()),
               reinterpret_cast<const unsigned char*>(private_key.data()));

  return BuildToken(
      StringPiece(reinterpret_cast<const char*>(signature), 64), json);
}

}  // namespace net_instaweb
