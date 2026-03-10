// Copyright (c) 2024-2026 We-Amp B.V.

// Signing utilities for license token generation. Used in key generation
// and test code; NOT compiled into the nginx module or worker.

#ifndef PAGESPEED_KERNEL_LICENSE_V2_LICENSE_SIGNER_H_
#define PAGESPEED_KERNEL_LICENSE_V2_LICENSE_SIGNER_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/license_v2/license_token.h"

namespace net_instaweb {

// Generate an Ed25519 keypair from a 32-byte seed.
// Outputs: |public_key| (32 bytes), |private_key| (64 bytes).
void CreateKeypair(StringPiece seed, GoogleString* public_key,
                   GoogleString* private_key);

// Sign a license token.  Returns the base64url-encoded token.
// |private_key| must be exactly 64 bytes (as produced by CreateKeypair).
// |public_key| must be exactly 32 bytes.
GoogleString SignLicenseToken(const LicensePayload& payload,
                              StringPiece public_key,
                              StringPiece private_key);

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_LICENSE_V2_LICENSE_SIGNER_H_
