// Copyright (c) 2024-2026 We-Amp B.V.

// Token format: base64url(64-byte-signature || json-payload)
// Payload: {"sub":"email","iss":"modpagespeed.com","iat":unix_ts,"plan":"pro"}

#ifndef PAGESPEED_KERNEL_LICENSE_V2_LICENSE_TOKEN_H_
#define PAGESPEED_KERNEL_LICENSE_V2_LICENSE_TOKEN_H_

#include <cstdint>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Parsed license token payload.
struct LicensePayload {
  GoogleString sub;   // Subject (customer email)
  GoogleString iss;   // Issuer (modpagespeed.com)
  int64_t iat = 0;    // Issued-at (Unix timestamp)
  GoogleString plan;  // Plan name (e.g. "pro")
  int64_t exp = 0;    // Expiry (Unix timestamp, 0 = no expiry, v2)
  GoogleString sid;   // FastSpring subscription ID (optional, v2)
  GoogleString kid;   // Key ID for rotation (optional, v2)
};

// Encode bytes to base64url (no padding).
GoogleString Base64UrlEncode(StringPiece input);

// Decode base64url to bytes.  Returns false on invalid input.
bool Base64UrlDecode(StringPiece input, GoogleString* output);

// Serialize a LicensePayload to JSON.
GoogleString SerializePayload(const LicensePayload& payload);

// Parse JSON into a LicensePayload.  Returns false on parse error.
bool ParsePayload(StringPiece json, LicensePayload* payload);

// Build a complete token: base64url(signature || json_payload).
// |signature| must be exactly 64 bytes.
GoogleString BuildToken(StringPiece signature, StringPiece json_payload);

// Split a token into signature (64 bytes) and JSON payload.
// Returns false if the token is invalid or too short.
bool SplitToken(StringPiece token, GoogleString* signature,
                GoogleString* json_payload);

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_LICENSE_V2_LICENSE_TOKEN_H_
