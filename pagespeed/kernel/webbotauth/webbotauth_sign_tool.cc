// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// webbotauth_sign_tool: a tiny offline CLI that mints a DETERMINISTIC Ed25519
// keypair and produces, for a given (method, authority, path, kid), the
// RFC 9421 / Web Bot Auth `Signature-Input` and `Signature` header values --
// built with the SAME serializer the verifier uses (SerializeSignatureParams /
// BuildSignatureBase + ed25519_sign), so the bytes are bit-identical to what
// the engine verifies. It can also emit the matching JWKS document (the signer
// public key under the kid). Used only by the AgentPass A1 live smoke test to
// drive a running nginx; NOT shipped in any runtime.
//
// Because the keypair is derived from a fixed seed, the JWKS (public key) is
// stable and can be committed as a fixture, while signatures are generated
// fresh at test time (so the RFC 9421 `created` timestamp is within the
// verifier's freshness window).
//
// Usage:
//   webbotauth_sign_tool --emit=jwks   --kid=K
//   webbotauth_sign_tool --emit=headers --kid=K --method=GET \
//       --authority=localhost --path=/wba-probe [--created=UNIXSEC] [--tamper]
//   --emit=all prints labeled lines for jwks + headers.
//
// For the AgentPass A3 (RSL-CAP) enforcement smoke it can also mint a signed
// capability token (same deterministic key the JWKS publishes):
//   webbotauth_sign_tool --emit=rslcap --kid=K --iss=I --sub=S \
//       --license=premium,basic --scope=render,fetch [--exp-in=SECONDS] [--tamper]
//   -> prints "TOKEN=License <compact-token>" (the full Authorization value).
//   A negative --exp-in mints an already-expired token; --tamper flips a
//   signature bit (-> kBadSignature). NOT shipped in any runtime.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "ed25519.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/signature_base.h"

namespace net_instaweb {
namespace webbotauth {
namespace {

// Standard base64 (with padding) -- for the Signature header byte sequence.
// Implemented locally because the library ships only a decoder.
GoogleString Base64Encode(StringPiece in) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  GoogleString out;
  size_t i = 0;
  while (i + 3 <= in.size()) {
    unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                 (static_cast<unsigned char>(in[i + 1]) << 8) |
                 static_cast<unsigned char>(in[i + 2]);
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(kAlphabet[(v >> 6) & 0x3F]);
    out.push_back(kAlphabet[v & 0x3F]);
    i += 3;
  }
  size_t rem = in.size() - i;
  if (rem == 1) {
    unsigned v = static_cast<unsigned char>(in[i]) << 16;
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                 (static_cast<unsigned char>(in[i + 1]) << 8);
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(kAlphabet[(v >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

GoogleString Base64UrlEncodeNoPad(StringPiece in) {
  GoogleString std = Base64Encode(in);
  GoogleString out;
  for (char c : std) {
    if (c == '+') {
      out.push_back('-');
    } else if (c == '/') {
      out.push_back('_');
    } else if (c == '=') {
      continue;  // strip padding
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// --- JSON assembly for RSL-CAP tokens (hand-built; same shape the validator's
// parser accepts, mirroring rsl_cap_validator_test.cc's MintToken). ---
GoogleString JsonStr(StringPiece key, StringPiece val) {
  return StrCat("\"", key, "\":\"", val, "\"");
}
GoogleString JsonInt(StringPiece key, int64_t val) {
  return StrCat("\"", key, "\":", Integer64ToString(val));
}
GoogleString JsonStrArray(StringPiece key, const StringPieceVector& vals) {
  GoogleString out = StrCat("\"", key, "\":[");
  for (size_t i = 0; i < vals.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    StrAppend(&out, "\"", vals[i], "\"");
  }
  out.push_back(']');
  return out;
}

// Read a --flag=value argument; returns true and sets *out if `arg` starts with
// `prefix` (e.g. "--kid=").
bool FlagValue(const char* arg, const char* prefix, GoogleString* out) {
  size_t plen = strlen(prefix);
  if (strncmp(arg, prefix, plen) == 0) {
    out->assign(arg + plen);
    return true;
  }
  return false;
}

int Run(int argc, char** argv) {
  GoogleString emit = "all";
  GoogleString method = "GET";
  GoogleString authority = "localhost";
  GoogleString path = "/wba-probe";
  GoogleString kid = "test-key-1";
  GoogleString created_str;
  bool tamper = false;
  // RSL-CAP token fields (used by --emit=rslcap). All operator-supplied at mint
  // time; the smoke drives them. lic/scope are comma-separated lists.
  GoogleString iss = "issuer.example";
  GoogleString sub = "agent";
  GoogleString lic_csv;
  GoogleString scope_csv;
  GoogleString exp_in_str;  // seconds from now until expiry (default 3600)

  for (int i = 1; i < argc; ++i) {
    GoogleString v;
    if (FlagValue(argv[i], "--emit=", &v)) {
      emit = v;
    } else if (FlagValue(argv[i], "--method=", &v)) {
      method = v;
    } else if (FlagValue(argv[i], "--authority=", &v)) {
      authority = v;
    } else if (FlagValue(argv[i], "--path=", &v)) {
      path = v;
    } else if (FlagValue(argv[i], "--kid=", &v)) {
      kid = v;
    } else if (FlagValue(argv[i], "--created=", &v)) {
      created_str = v;
    } else if (FlagValue(argv[i], "--iss=", &v)) {
      iss = v;
    } else if (FlagValue(argv[i], "--sub=", &v)) {
      sub = v;
    } else if (FlagValue(argv[i], "--license=", &v)) {
      lic_csv = v;
    } else if (FlagValue(argv[i], "--scope=", &v)) {
      scope_csv = v;
    } else if (FlagValue(argv[i], "--exp-in=", &v)) {
      exp_in_str = v;
    } else if (strcmp(argv[i], "--tamper") == 0) {
      tamper = true;
    } else {
      fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }

  // Deterministic keypair (same seed shape as verifier_test): bytes 1..32.
  unsigned char seed[32];
  for (int i = 0; i < 32; ++i) {
    seed[i] = static_cast<unsigned char>(i + 1);
  }
  unsigned char public_key[32];
  unsigned char private_key[64];
  ed25519_create_keypair(public_key, private_key, seed);
  GoogleString pub(reinterpret_cast<const char*>(public_key), 32);

  if (emit == "jwks" || emit == "all") {
    // JWKS shape ExtractEd25519Key accepts: OKP/Ed25519, base64url(x), kid.
    GoogleString x = Base64UrlEncodeNoPad(pub);
    GoogleString jwks =
        StrCat("{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"",
               kid, "\",\"x\":\"", x, "\"}]}");
    if (emit == "jwks") {
      printf("%s\n", jwks.c_str());
    } else {
      printf("JWKS=%s\n", jwks.c_str());
    }
  }

  if (emit == "headers" || emit == "all") {
    int64_t created =
        created_str.empty()
            ? static_cast<int64_t>(time(nullptr))
            : static_cast<int64_t>(strtoll(created_str.c_str(), nullptr, 10));

    // Build @signature-params exactly as the header and the signature base must
    // agree on, using the library serializer (component order + params order).
    SfvInnerList list;
    list.components = {"@method", "@authority", "@path"};
    SfvParam created_p;
    created_p.name = "created";
    created_p.type = SfvParam::kInteger;
    created_p.int_value = created;
    list.params.push_back(created_p);
    SfvParam keyid_p;
    keyid_p.name = "keyid";
    keyid_p.type = SfvParam::kString;
    keyid_p.str_value = kid;
    list.params.push_back(keyid_p);
    SfvParam alg_p;
    alg_p.name = "alg";
    alg_p.type = SfvParam::kString;
    alg_p.str_value = "ed25519";
    list.params.push_back(alg_p);

    GoogleString params = SerializeSignatureParams(list);

    BaseRequestView brv;
    brv.method = method;
    brv.authority = authority;
    brv.path = path;
    GoogleString base = BuildSignatureBase(brv, list.components, params);

    unsigned char sig[64];
    ed25519_sign(sig, reinterpret_cast<const unsigned char*>(base.data()),
                 base.size(), public_key, private_key);
    GoogleString sig_bytes(reinterpret_cast<const char*>(sig), 64);
    if (tamper) {
      // Flip a bit so verification must fail (-> Verdict::kUnknown).
      sig_bytes[0] = static_cast<char>(sig_bytes[0] ^ 0x01);
    }

    GoogleString sig_input = StrCat("sig1=", params);
    GoogleString signature = StrCat("sig1=:", Base64Encode(sig_bytes), ":");

    printf("SIGINPUT=%s\n", sig_input.c_str());
    printf("SIG=%s\n", signature.c_str());
  }

  if (emit == "rslcap") {
    // Mint an RSL-CAP capability token signed by the SAME deterministic key the
    // JWKS publishes, so the A3 enforcement smoke can drive a running nginx.
    // JSON shape mirrors rsl_cap_validator_test.cc's MintToken (the parser the
    // validator uses). NOT shipped in any runtime.
    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t exp_in =
        exp_in_str.empty()
            ? 3600
            : static_cast<int64_t>(strtoll(exp_in_str.c_str(), nullptr, 10));
    int64_t exp = now + exp_in;  // a negative --exp-in mints an expired token.

    StringPieceVector lic;
    StringPieceVector scope;
    SplitStringPieceToVector(lic_csv, ",", &lic, true /* omit_empty */);
    SplitStringPieceToVector(scope_csv, ",", &scope, true /* omit_empty */);

    const GoogleString header_json =
        StrCat("{", JsonStr("alg", "EdDSA"), ",", JsonStr("typ", "RSL-CAP"),
               ",", JsonStr("kid", kid), "}");
    GoogleString payload_body =
        StrCat(JsonStr("iss", iss), ",", JsonStr("sub", sub), ",",
               JsonInt("exp", exp), ",", JsonInt("iat", now));
    StrAppend(&payload_body, ",", JsonStrArray("lic", lic));
    StrAppend(&payload_body, ",", JsonStrArray("scope", scope));
    const GoogleString payload_json = StrCat("{", payload_body, "}");

    const GoogleString header_b64 = Base64UrlEncodeNoPad(header_json);
    const GoogleString payload_b64 = Base64UrlEncodeNoPad(payload_json);
    const GoogleString signing_input = StrCat(header_b64, ".", payload_b64);

    unsigned char sig[64];
    ed25519_sign(sig,
                 reinterpret_cast<const unsigned char*>(signing_input.data()),
                 signing_input.size(), public_key, private_key);
    GoogleString sig_bytes(reinterpret_cast<const char*>(sig), 64);
    if (tamper) {
      sig_bytes[0] =
          static_cast<char>(sig_bytes[0] ^ 0x01);  // -> kBadSignature
    }
    const GoogleString sig_b64 = Base64UrlEncodeNoPad(sig_bytes);
    const GoogleString token = StrCat(signing_input, ".", sig_b64);

    // Emit the full Authorization header value, ready to send verbatim.
    printf("TOKEN=License %s\n", token.c_str());
  }

  return 0;
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb

int main(int argc, char** argv) {
  return net_instaweb::webbotauth::Run(argc, argv);
}
