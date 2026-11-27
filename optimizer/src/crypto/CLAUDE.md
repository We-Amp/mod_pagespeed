# Crypto — Web Bot Auth / RSL-CAP verification

`src/crypto/webbotauth/` is the only code here: the Web Bot Auth core ported
from mod_pagespeed 1.15, verification-only. It reuses the
`ed25519_verify` primitive from `@ed25519`; there is no other cryptography in
the tree. (The Ed25519 license-token signer/verifier that used to live in this
directory was removed at 2.1 GA. The daemon carries no license state.)

## Layers (one target: `//src/crypto/webbotauth:webbotauth_core`)

**A1 — observe-only RFC 9421 verifier.** Classifies a request as
human / signed-agent / verified-bot / unknown. Never blocks, never enforces.

- `sfv.h` — minimal RFC 8941 structured-field parser for `Signature-Input` /
  `Signature` (bounded, fails closed, never throws)
- `header_parser.h` — selects the first `tag="web-bot-auth"` signature and
  enforces the profile (alg `ed25519`, `@authority` covered, 64-byte signature)
- `signature_base.h` — RFC 9421 §2.5 signature-base construction (derived
  components + covered HTTP fields, byte-exact `@signature-params`)
- `jwks.h` — extract OKP/Ed25519 public keys from a JWKS document (by `kid`,
  or every kid-bearing key for the warmer)
- `key_directory.h` — `KeyDirectoryProvider` interface + in-memory
  `WarmedKeyStore` (TTL-clamped, strict expiry)
- `static_key_directory.h` — provider over one operator-supplied JWKS file
- `classifier.h` — `Verdict` + operator-curated `VerifiedBotRegistry`
  (keyid → bot name; empty by default, nothing hardcoded as trusted)
- `verifier.h` — the entry point, `VerifyAndClassify(req, provider, registry, now)`
- `base64.h` — RFC 4648 base64/base64url decoders (encoding, not cryptography)

**A3 — RSL-CAP capability-token enforcement** (experimental, default-off).
Validates an `Authorization: License` token and maps the verdict to an HTTP
status: allow / 401 (malformed, unknown issuer, bad signature, expired) /
402 (valid identity, requested license or scope not granted). Status only —
it never settles, meters or handles money.

- `rsl_cap_token.h` — `ParseRslCapToken`: 3-part compact form, header must be
  `typ=RSL-CAP` + `alg=EdDSA` (defeats alg confusion); no crypto, no I/O
- `rsl_cap_validator.h` — `RslCapValidator` + `RslCapStatusToHttpStatus`

Keys come only from operator-configured sources (the `https://` JWKS
key-directory URLs the worker warms, or a local JWKS file). Those fetches are
the only outbound requests this code causes.

`webbotauth_sign_tool` is an offline test-support CLI (deterministic keypair +
matching JWKS and RFC 9421 headers). It is not shipped in any runtime.

## Sync discipline

Every header names the mod_pagespeed 1.15 file and commit it mirrors. The
RFC 9421/8941 core is standards-frozen: keep logic in sync manually and record
any extension (tag selection, multi-signature dictionaries, HTTP field
components) in the header comment.

## Testing
```bash
bazel test //test/src/crypto/...
```
