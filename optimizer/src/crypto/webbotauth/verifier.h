// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed pagespeed/kernel/webbotauth/verifier.h @
// 626ddfa95 (2026-07-03). Keep logic in sync manually; the RFC 9421/8941 core
// is standards-frozen. Extends the original core (upstreamed to 1.15 in
// the 1.15 line): only signatures tagged "web-bot-auth" are
// verified (untagged/other-tag signature material is classified as if
// unsigned), and covered HTTP field components (e.g. "signature-agent") resolve
// against the binding-provided field list.
//
// Top-level Web-Bot-Auth verifier (RFC 9421 HTTP Message Signatures, minimal
// profile). Classifies a request as human / signed-agent / verified-bot /
// unknown. NEVER throws, NEVER blocks, NEVER enforces -- it only classifies.
// This is the FREE verifier; no 401/402, no license/RSL-CAP, no metering.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_VERIFIER_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_VERIFIER_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "src/crypto/webbotauth/classifier.h"
#include "src/crypto/webbotauth/key_directory.h"
#include "src/crypto/webbotauth/signature_base.h"

namespace pagespeed {
namespace webbotauth {

// Read-only view of the request bits we need. The server binding fills these
// from the request headers + the request line; the unit test fills them
// directly.
struct RequestView {
  std::string_view method;     // e.g. "GET" (caller upper-cases)
  std::string_view authority;  // Host header value (RFC 9421 @authority)
  std::string_view path;       // request target path (RFC 9421 @path)
  // Raw Signature-Input header value (may be empty).
  std::string_view signature_input;
  std::string_view signature;   // raw Signature header value (may be empty)
  std::string_view user_agent;  // context only; NEVER trusted on its own
  // The request's HTTP fields the binding exposes as coverable RFC 9421
  // field components (e.g. every Signature-Agent field line, in wire order).
  // Raw wire bytes; a covered field component with no entry here fails
  // verification closed (kUnknown).
  std::vector<HeaderField> fields;
  // The directory host the operator maps this keyid's issuer to. In the
  // server binding this comes from operator config (host->directory mapping),
  // NOT from any request-controlled header. For the unit test it is set to
  // the fixture host. If empty, the provider is asked with an empty host
  // (fails closed).
  std::string_view directory_host;
};

struct VerifyResult {
  Verdict verdict = Verdict::kUnknown;
  std::string keyid;     // populated when a signature was parsed (logging)
  std::string bot_name;  // populated when verdict == kVerifiedBot
  std::string reason;    // diagnostic only; NEVER a trust assertion
};

// The single entry point. `provider` resolves keys (injectable; the test uses
// a fake). `registry` promotes signed agents to verified bots. `now_unix_sec`
// is used for created/expires window checks. Never throws.
//
// Allowed clock skew for created/expires checks (seconds).
constexpr int64_t kMaxClockSkewSec = 300;
// Maximum age of a `created` timestamp we accept when no `expires` is given.
constexpr int64_t kMaxSignatureAgeSec = 3600;

VerifyResult VerifyAndClassify(const RequestView& req,
                               KeyDirectoryProvider* provider,
                               const VerifiedBotRegistry& registry,
                               int64_t now_unix_sec);

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_VERIFIER_H_
