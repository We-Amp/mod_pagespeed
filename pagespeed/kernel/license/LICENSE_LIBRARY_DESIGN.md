# PageSpeed License Validation Library Design

## Overview

A shared C++ library for validating JWT-based license tokens across all PageSpeed modules (Apache, NGINX, IIS, Envoy).

## Location

```
pagespeed/kernel/license/
├── BUILD
├── license.h              # Main public interface
├── license.cc             # Implementation
├── license_claims.h       # License claims data structure
├── jwt_util.h             # JWT parsing utilities
├── jwt_util.cc
├── jwt_util_openssl.cc    # RSA verification (Linux/BoringSSL)
└── jwt_util_win.cc        # RSA verification (Windows/BCrypt)
```

## Public Interface

### license.h

```cpp
#ifndef PAGESPEED_KERNEL_LICENSE_LICENSE_H_
#define PAGESPEED_KERNEL_LICENSE_LICENSE_H_

#include <memory>
#include <vector>
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/license/license_claims.h"

namespace net_instaweb {

class MessageHandler;

// License validation result
struct LicenseValidationResult {
  bool is_valid;
  GoogleString error_message;
  std::unique_ptr<LicenseClaims> claims;  // nullptr if invalid
};

// License manager - validates and caches license state
class LicenseManager {
 public:
  // Create with embedded public key (PEM format)
  explicit LicenseManager(const GoogleString& public_key_pem,
                          MessageHandler* handler);
  ~LicenseManager();

  // Load license from file path
  bool LoadLicenseFile(const GoogleString& file_path);

  // Load license from string (JWT token)
  bool LoadLicenseToken(const GoogleString& jwt_token);

  // Validate the loaded license
  LicenseValidationResult Validate();

  // Check if a specific feature is enabled
  bool IsFeatureEnabled(const GoogleString& feature_name) const;

  // Get the license tier (e.g., "pro", "enterprise", "community")
  GoogleString GetTier() const;

  // Check if license is valid and not expired
  bool IsValid() const;

  // Get expiration time (Unix timestamp)
  int64_t GetExpirationTime() const;

  // Get server limit
  int GetServerLimit() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  DISALLOW_COPY_AND_ASSIGN(LicenseManager);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_LICENSE_LICENSE_H_
```

### license_claims.h

```cpp
#ifndef PAGESPEED_KERNEL_LICENSE_LICENSE_CLAIMS_H_
#define PAGESPEED_KERNEL_LICENSE_LICENSE_CLAIMS_H_

#include <vector>
#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

// Represents the claims from a license JWT
struct LicenseClaims {
  GoogleString customer_id;
  GoogleString subscription_id;
  GoogleString tier;              // "community", "pro", "enterprise"
  std::vector<GoogleString> features;  // ["analytics", "redis_cache", etc.]
  int server_limit;
  int64_t issued_at;              // Unix timestamp
  int64_t expires_at;             // Unix timestamp
  GoogleString refresh_url;

  // Convenience methods
  bool HasFeature(const GoogleString& feature) const;
  bool IsExpired() const;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_LICENSE_LICENSE_CLAIMS_H_
```

### jwt_util.h

```cpp
#ifndef PAGESPEED_KERNEL_LICENSE_JWT_UTIL_H_
#define PAGESPEED_KERNEL_LICENSE_JWT_UTIL_H_

#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

class MessageHandler;

namespace jwt_util {

// Decode a Base64URL encoded string (JWT uses URL-safe base64 without padding)
bool Base64UrlDecode(const GoogleString& input, GoogleString* output);

// Parse a JWT token into its three parts (header, payload, signature)
// Does NOT verify the signature
bool ParseJwt(const GoogleString& token,
              GoogleString* header_json,
              GoogleString* payload_json,
              GoogleString* signature_bytes,
              GoogleString* signing_input);  // header.payload for verification

// Verify an RS256 signature
// public_key_pem: PEM-encoded RSA public key
// signing_input: The data that was signed (header.payload)
// signature: The raw signature bytes (already base64url decoded)
bool VerifyRS256Signature(const GoogleString& public_key_pem,
                          const GoogleString& signing_input,
                          const GoogleString& signature,
                          MessageHandler* handler);

}  // namespace jwt_util
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_LICENSE_JWT_UTIL_H_
```

## BUILD File

```python
load("//bazel:pagespeed.bzl", "pagespeed_cc_library")

pagespeed_cc_library(
    name = "license",
    srcs = [
        "license.cc",
        "license_claims.cc",
        "jwt_util.cc",
    ] + select({
        "@platforms//os:windows": ["jwt_util_win.cc"],
        "//conditions:default": ["jwt_util_openssl.cc"],
    }),
    hdrs = [
        "license.h",
        "license_claims.h",
        "jwt_util.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "@jsoncpp",
    ] + select({
        "@platforms//os:windows": [],  # BCrypt is system library
        "//conditions:default": ["@boringssl//:ssl"],
    }),
)
```

## Usage in Modules

### Apache (mod_pagespeed)

```cpp
// In apache/mod_instaweb.cc or similar
#include "pagespeed/kernel/license/license.h"

// At startup
static LicenseManager* g_license_manager = nullptr;

void InitializeLicense(const char* license_path, const char* public_key) {
  g_license_manager = new LicenseManager(public_key, handler);
  if (!g_license_manager->LoadLicenseFile(license_path)) {
    LOG(WARNING) << "No license file found, running in community mode";
  }
}

// When checking features
bool IsProFeatureEnabled() {
  return g_license_manager &&
         g_license_manager->IsValid() &&
         g_license_manager->IsFeatureEnabled("analytics");
}
```

### Configuration Directive

```apache
# Apache
ModPagespeedLicenseFile /etc/pagespeed/license.jwt
```

```nginx
# NGINX
pagespeed_license_file /etc/pagespeed/license.jwt;
```

```xml
<!-- IIS web.config -->
<pagespeed licenseFile="C:\pagespeed\license.jwt" />
```

```yaml
# Envoy
http_filters:
- name: pagespeed
  typed_config:
    license_file: /etc/pagespeed/license.jwt
```

## Implementation Notes

### JWT Structure (RS256)

```
header.payload.signature

Header (base64url encoded JSON):
{
  "alg": "RS256",
  "typ": "JWT"
}

Payload (base64url encoded JSON):
{
  "customer_id": "cus_xxx",
  "subscription_id": "sub_xxx",
  "tier": "pro",
  "features": ["analytics", "redis_cache"],
  "server_limit": 5,
  "iat": 1706918400,
  "exp": 1738454400,
  "refresh_url": "https://we-amp.com/api/license/refresh"
}

Signature:
RS256(base64url(header) + "." + base64url(payload), private_key)
```

### Platform-Specific Crypto

**Linux (BoringSSL):**
```cpp
// jwt_util_openssl.cc
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/evp.h>

bool VerifyRS256Signature(...) {
  // 1. Parse PEM public key
  // 2. Create EVP_MD_CTX with SHA256
  // 3. EVP_DigestVerifyInit, EVP_DigestVerifyUpdate, EVP_DigestVerifyFinal
}
```

**Windows (BCrypt):**
```cpp
// jwt_util_win.cc
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

bool VerifyRS256Signature(...) {
  // 1. BCryptOpenAlgorithmProvider(BCRYPT_RSA_ALGORITHM)
  // 2. BCryptImportKeyPair (from PEM/DER)
  // 3. BCryptVerifySignature with BCRYPT_PAD_PKCS1 + SHA256
}
```

### Public Key Embedding

The RSA public key will be embedded at compile time:

```cpp
// In license.cc or a generated header
static const char kLicensePublicKey[] = R"(
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA...
-----END PUBLIC KEY-----
)";
```

For development, allow overriding via environment variable or config.

## Feature Gating

Features that require a license:
- `analytics` - Performance analytics dashboard data collection
- `redis_cache` - Redis cache backend support
- `distributed_cache` - Multi-server cache coordination
- `custom_filters` - Custom filter development support
- `priority_support` - (Not a code feature, but tracked)

Example usage in filter code:

```cpp
// In some_filter.cc
if (driver->license_manager()->IsFeatureEnabled("redis_cache")) {
  // Enable Redis cache backend
} else {
  // Fall back to file cache
}
```

## Graceful Degradation

- If no license file: Run in "community" mode (all features, no restrictions)
- If license expired: Log warning, continue in community mode with 7-day grace
- If license invalid signature: Log error, run in community mode
- If feature not in license: Silently disable that feature

## Testing

Test file: `test/pagespeed/kernel/license/license_test.cc`

Test cases:
1. Valid license parsing and validation
2. Expired license detection
3. Invalid signature rejection
4. Missing features handling
5. Base64URL edge cases
6. Platform-specific crypto tests
