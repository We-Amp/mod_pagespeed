// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Kept in sync manually with pagespeed-optimizer src/crypto/webbotauth/verifier.h
// (this change upstreams the optimizer line: only signatures tagged "web-bot-auth" are
// verified -- untagged/other-tag signature material is classified as if
// unsigned -- and covered HTTP field components (e.g. "signature-agent")
// resolve against the binding-provided field list).
//
// Top-level Web-Bot-Auth verifier (RFC 9421 HTTP Message Signatures, minimal
// profile). Classifies a request as human / signed-agent / verified-bot /
// unknown. NEVER throws, NEVER blocks, NEVER enforces -- it only classifies.
// This is the FREE verifier; no 401/402, no license/RSL-CAP, no metering.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_VERIFIER_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_VERIFIER_H_

#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/classifier.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "pagespeed/kernel/webbotauth/signature_base.h"

namespace net_instaweb {
namespace webbotauth {

// Read-only view of the request bits we need. The nginx binding fills these
// from RequestHeaders + the request line; the unit test fills them directly.
struct RequestView {
  StringPiece method;     // e.g. "GET" (caller upper-cases)
  StringPiece authority;  // Host header value (RFC 9421 @authority)
  StringPiece path;       // request target path (RFC 9421 @path)
  StringPiece
      signature_input;     // raw Signature-Input header value (may be empty)
  StringPiece signature;   // raw Signature header value (may be empty)
  StringPiece user_agent;  // context only; NEVER trusted on its own
  // The request's HTTP fields the binding exposes as coverable RFC 9421
  // field components (e.g. every Signature-Agent field line, in wire order).
  // Raw wire bytes; a covered field component with no entry here fails
  // verification closed (kUnknown).
  std::vector<HeaderField> fields;
  // The directory host the operator maps this keyid's issuer to. In the nginx
  // binding this comes from operator config (host->directory mapping), NOT from
  // any request-controlled header. For the unit test it is set to the fixture
  // host. If empty, the provider is asked with an empty host (fails closed).
  StringPiece directory_host;
};

struct VerifyResult {
  Verdict verdict = Verdict::kUnknown;
  GoogleString keyid;     // populated when a signature was parsed (logging)
  GoogleString bot_name;  // populated when verdict == kVerifiedBot
  GoogleString reason;    // diagnostic only; NEVER a trust assertion
};

// The single entry point. `provider` resolves keys (injectable; the test uses a
// fake). `registry` promotes signed agents to verified bots. `now_unix_sec` is
// used for created/expires window checks. Never throws.
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
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_VERIFIER_H_
