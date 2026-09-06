// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Kept in sync manually with pagespeed-optimizer src/crypto/webbotauth/verifier.cc
// (this change upstreams the optimizer line: tag selection + HTTP field covered
// components, see verifier.h).

#include "pagespeed/kernel/webbotauth/verifier.h"

#include "ed25519.h"
#include "pagespeed/kernel/webbotauth/header_parser.h"
#include "pagespeed/kernel/webbotauth/signature_base.h"

namespace net_instaweb {
namespace webbotauth {

namespace {

VerifyResult Unknown(const char* reason) {
  VerifyResult r;
  r.verdict = Verdict::kUnknown;
  r.reason = reason;
  return r;
}

// RFC 9421 created/expires freshness check. Fails closed on anything stale or
// future-dated beyond allowed skew.
bool TimestampsFresh(const SignatureRequest& sig, int64_t now) {
  // A signature with neither created nor expires has no bounded lifetime and is
  // replayable forever; require at least one (RFC 9421 / Web Bot Auth practice
  // expect `created`). Fail closed.
  if (!sig.has_created && !sig.has_expires) return false;
  if (sig.has_created) {
    if (sig.created > now + kMaxClockSkewSec) return false;  // future-dated
    if (!sig.has_expires) {
      // No explicit expiry: bound the acceptable age.
      if (now - sig.created > kMaxSignatureAgeSec) return false;
    }
  }
  if (sig.has_expires) {
    if (now > sig.expires + kMaxClockSkewSec) return false;  // expired
  }
  return true;
}

}  // namespace

VerifyResult VerifyAndClassify(const RequestView& req,
                               KeyDirectoryProvider* provider,
                               const VerifiedBotRegistry& registry,
                               int64_t now_unix_sec) {
  // 1. No signature headers at all => a plain (human / non-signing) request.
  //    A spoofed User-Agent alone earns no trust: it never reaches the signed
  //    tiers below.
  if (req.signature_input.empty() && req.signature.empty()) {
    VerifyResult r;
    r.verdict = Verdict::kHuman;
    r.reason = "no-signature";
    return r;
  }
  // Present-but-empty one side, or any partial state, is treated strictly.
  if (req.signature_input.empty() || req.signature.empty()) {
    return Unknown("partial-signature-headers");
  }

  // 2. Parse + validate the headers into the supported profile, selecting
  //    the first web-bot-auth-tagged signature.
  SignatureRequest sig;
  GoogleString reason;
  ParseStatus status =
      ParseSignatureHeaders(req.signature_input, req.signature, &sig, &reason);
  if (status == ParseStatus::kNoWebBotAuthSignature) {
    // Parseable signature material, but none of it is tagged "web-bot-auth":
    // some OTHER signing scheme (e.g. a CDN signature). NOT web-bot-auth
    // material -- classify exactly as an unsigned request (not invalid).
    VerifyResult r;
    r.verdict = Verdict::kHuman;
    r.reason = "no-web-bot-auth-signature";
    return r;
  }
  if (status != ParseStatus::kOk) {
    VerifyResult r = Unknown("parse-failed");
    r.reason = reason;
    return r;
  }

  // 2b. Web Bot Auth architecture draft section 4.2: a Signature-Agent
  //     header, when sent, MUST be covered by the signature -- an uncovered
  //     Signature-Agent could be swapped in transit without breaking the
  //     signature. Fail closed. (@authority coverage is enforced by the
  //     header parser.)
  bool has_signature_agent_field = false;
  for (const HeaderField& field : req.fields) {
    if (StringCaseEqual(field.name, "signature-agent")) {
      has_signature_agent_field = true;
      break;
    }
  }
  if (has_signature_agent_field) {
    bool agent_covered = false;
    for (const GoogleString& c : sig.covered) {
      if (c == "signature-agent") {
        agent_covered = true;
        break;
      }
    }
    if (!agent_covered) {
      VerifyResult r = Unknown("signature-agent-not-covered");
      r.keyid = sig.keyid;
      return r;
    }
  }

  // 3. Freshness.
  if (!TimestampsFresh(sig, now_unix_sec)) {
    VerifyResult r = Unknown("stale-or-future-signature");
    r.keyid = sig.keyid;
    return r;
  }

  // 4. Resolve the signer's public key via the injectable provider. The host
  //    is operator-mapped (NOT request-controlled). Any provider error (off
  //    allowlist / SSRF refusal / fetch failure / not found) -> kUnknown.
  if (provider == nullptr) {
    return Unknown("no-provider");
  }
  KeyLookupResult key =
      provider->GetKey(req.directory_host, sig.keyid, now_unix_sec);
  if (key.status != KeyLookupResult::kFound) {
    VerifyResult r =
        Unknown(key.status == KeyLookupResult::kNotFound ? "key-not-found"
                                                         : "key-lookup-error");
    r.keyid = sig.keyid;
    return r;
  }
  if (key.raw_key_32.size() != 32) {
    VerifyResult r = Unknown("bad-key-length");
    r.keyid = sig.keyid;
    return r;
  }

  // 5. Rebuild the canonical signature base and verify with the reused
  //    @ed25519 primitive (same dep as proofwire).
  BaseRequestView brv;
  brv.method = req.method;
  brv.authority = req.authority;
  brv.path = req.path;
  brv.fields = req.fields;
  GoogleString base;
  if (!BuildSignatureBase(brv, sig.covered, sig.signature_params_value,
                          &base)) {
    // A covered field component is absent from the request (or an unknown
    // derived component slipped through): the base cannot be constructed, so
    // the signature can never verify. Fail closed.
    VerifyResult r = Unknown("missing-covered-field");
    r.keyid = sig.keyid;
    return r;
  }

  if (sig.signature_bytes.size() != 64) {
    VerifyResult r = Unknown("signature-not-64-bytes");
    r.keyid = sig.keyid;
    return r;
  }

  int ok = ed25519_verify(
      reinterpret_cast<const unsigned char*>(sig.signature_bytes.data()),
      reinterpret_cast<const unsigned char*>(base.data()), base.size(),
      reinterpret_cast<const unsigned char*>(key.raw_key_32.data()));
  if (ok != 1) {
    VerifyResult r = Unknown("signature-verify-failed");
    r.keyid = sig.keyid;
    return r;
  }

  // 6. Cryptographically valid -> classify signed-agent vs verified-bot.
  VerifyResult r;
  GoogleString bot_name;
  r.verdict = ClassifyVerified(sig.keyid, registry, &bot_name);
  r.keyid = sig.keyid;
  r.bot_name = bot_name;
  r.reason = "verified";
  return r;
}

}  // namespace webbotauth
}  // namespace net_instaweb
