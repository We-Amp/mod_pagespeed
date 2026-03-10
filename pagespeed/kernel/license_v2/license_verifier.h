// Copyright (c) 2024-2026 We-Amp B.V.

// Both the nginx module and worker call this on startup; invalid
// tokens produce a warning but do NOT prevent the module from working
// (BSL is the legal enforcement, not technical DRM).

#ifndef PAGESPEED_KERNEL_LICENSE_V2_LICENSE_VERIFIER_H_
#define PAGESPEED_KERNEL_LICENSE_V2_LICENSE_VERIFIER_H_

#include <cstdint>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/license_v2/license_token.h"

namespace net_instaweb {

// Result of license verification.
struct LicenseResult {
  bool valid = false;      // True if signature is cryptographically valid
  bool expired = false;    // True if exp < now (valid but expired)
  int64_t expires_at = 0;  // exp value, 0 if no expiry
  LicensePayload payload;  // Parsed payload (only meaningful if valid)
  GoogleString error;       // Human-readable error if !valid
};

// Verify a base64url-encoded license token against the embedded
// public key.  Returns a LicenseResult indicating validity.
LicenseResult VerifyLicenseToken(StringPiece token);

// Verify a token using a specific public key (for testing).
// |public_key| must be exactly 32 bytes.
LicenseResult VerifyLicenseTokenWithKey(StringPiece token,
                                        StringPiece public_key);

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_LICENSE_V2_LICENSE_VERIFIER_H_
