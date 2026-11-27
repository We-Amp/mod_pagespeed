// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed
// pagespeed/kernel/webbotauth/webbotauth_sign_tool.cc @
// f6c24e8d34ba0c58acb22d626f6213d666d73e41 (2026-07-04). Keep logic in sync
// manually; the RFC 9421/8941 core is standards-frozen. Extends the original
// tool (upstreamed to 1.15): selectable covered
// components incl. HTTP fields (--components/--field), expires/nonce/tag params.
// --emit=keystore stays 2.0-only (1.15 has no WarmedKeyStore codec).
//
// AgentPass A3 (RSL-CAP) enforcement smoke: --emit=rslcap mints a signed
// capability token (same deterministic key the JWKS publishes), and
// --emit=keystore takes a --realm so a pre-warmed store can hold RSL keys under
// realm "rsl" (distinct trust domain from the observe-only "wba" verifier).
//
// webbotauth_sign_tool: a tiny offline CLI that mints a DETERMINISTIC Ed25519
// keypair and produces, for a given (method, authority, path, kid), the
// RFC 9421 / Web Bot Auth `Signature-Input` and `Signature` header values --
// built with the SAME serializer the verifier uses (SerializeSignatureParams /
// BuildSignatureBase + ed25519_sign), so the bytes are bit-identical to what
// the engine verifies. It can also emit the matching JWKS document (the signer
// public key under the kid). Test support only; NOT shipped in any runtime.
//
// Because the keypair is derived from a fixed seed, the JWKS (public key) is
// stable and can be committed as a fixture, while signatures are generated
// fresh at test time (so the RFC 9421 `created` timestamp is within the
// verifier's freshness window).
//
// Usage:
//   webbotauth_sign_tool --emit=jwks   --kid=K
//   webbotauth_sign_tool --emit=headers --kid=K --method=GET \
//       --authority=localhost --path=/wba-probe [--created=UNIXSEC] \
//       [--expires=UNIXSEC] [--nonce=N] [--tag=web-bot-auth] \
//       [--components=@authority,signature-agent] \
//       [--field=signature-agent="https://..."] [--tamper]
//   webbotauth_sign_tool --emit=keystore --kid=K \
//       --directory-host=keys.example.com [--key-expires=UNIXSEC] [--realm=R]
//   webbotauth_sign_tool --emit=rslcap --kid=K --iss=I --sub=S \
//       --license=premium,basic --scope=render,fetch [--exp-in=SECONDS] [--tamper]
//     -> prints "TOKEN=License <compact-token>" (the full Authorization value).
//        A negative --exp-in mints an already-expired token; --tamper flips a
//        signature bit so verification must fail (kBadSignature).
//   --emit=all prints labeled lines for jwks + headers.
//
// --components selects the covered components (comma-separated; lowercase
// names are RFC 9421 field components). --field (repeatable, name=value)
// supplies the request field values the signature base covers — e.g. the
// scanner probe's Signature-Agent sf-string. --tag defaults to
// "web-bot-auth" (the verifier only selects tagged signatures); pass --tag=
// to omit it, or another value to mint non-web-bot-auth material for tests.
// --emit=keystore prints a pagespeed-webbotauth-keys.conf document (the
// WarmedKeyStore codec) holding this tool's public key under
// StoreKey("wba", --directory-host, --kid), so a .t harness can stand up a
// pre-warmed nginx key store without running the worker.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "ed25519.h"  // NOLINT(build/include_subdir)
#include "src/crypto/webbotauth/key_directory.h"
#include "src/crypto/webbotauth/signature_base.h"

namespace pagespeed {
namespace webbotauth {
namespace {

// Standard base64 (with padding) -- for the Signature header byte sequence.
// Implemented locally because the library ships only a decoder.
std::string Base64Encode(std::string_view in) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
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

std::string Base64UrlEncodeNoPad(std::string_view in) {
  std::string std_b64 = Base64Encode(in);
  std::string out;
  for (char c : std_b64) {
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
std::string JsonStr(std::string_view key, std::string_view val) {
  return absl::StrCat("\"", key, "\":\"", val, "\"");
}
std::string JsonInt(std::string_view key, int64_t val) {
  return absl::StrCat("\"", key, "\":", val);
}
std::string JsonStrArray(std::string_view key,
                         const std::vector<std::string>& vals) {
  std::string out = absl::StrCat("\"", key, "\":[");
  for (size_t i = 0; i < vals.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    absl::StrAppend(&out, "\"", vals[i], "\"");
  }
  out.push_back(']');
  return out;
}

// Split a comma-separated list into non-empty items.
std::vector<std::string> SplitCsv(std::string_view spec) {
  std::vector<std::string> out;
  while (!spec.empty()) {
    size_t comma = spec.find(',');
    std::string_view item = spec.substr(0, comma);
    if (!item.empty()) out.emplace_back(item);
    if (comma == std::string_view::npos) break;
    spec.remove_prefix(comma + 1);
  }
  return out;
}

// Read a --flag=value argument; returns true and sets *out if `arg` starts
// with `prefix` (e.g. "--kid=").
bool FlagValue(const char* arg, const char* prefix, std::string* out) {
  size_t plen = strlen(prefix);
  if (strncmp(arg, prefix, plen) == 0) {
    out->assign(arg + plen);
    return true;
  }
  return false;
}

// Split a comma-separated component list into ids.
std::vector<std::string> SplitComponents(std::string_view spec) {
  std::vector<std::string> out;
  while (!spec.empty()) {
    size_t comma = spec.find(',');
    std::string_view item = spec.substr(0, comma);
    if (!item.empty()) out.emplace_back(item);
    if (comma == std::string_view::npos) break;
    spec.remove_prefix(comma + 1);
  }
  return out;
}

int Run(int argc, char** argv) {
  std::string emit = "all";
  std::string method = "GET";
  std::string authority = "localhost";
  std::string path = "/wba-probe";
  std::string kid = "test-key-1";
  std::string created_str;
  std::string expires_str;
  std::string nonce;
  std::string tag = "web-bot-auth";  // the tag the verifier selects on
  std::string components_spec = "@method,@authority,@path";
  std::string directory_host = "keys.example.com";
  std::string key_expires_str;
  std::string realm = "wba";  // keystore trust domain: "wba" or "rsl"
  std::vector<std::pair<std::string, std::string>> field_values;
  bool tamper = false;
  // RSL-CAP token fields (used by --emit=rslcap). All operator-supplied at mint
  // time; the smoke drives them. lic/scope are comma-separated lists.
  std::string iss = "issuer.example";
  std::string sub = "agent";
  std::string lic_csv;
  std::string scope_csv;
  std::string exp_in_str;  // seconds from now until expiry (default 3600)

  for (int i = 1; i < argc; ++i) {
    std::string v;
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
    } else if (FlagValue(argv[i], "--expires=", &v)) {
      expires_str = v;
    } else if (FlagValue(argv[i], "--nonce=", &v)) {
      nonce = v;
    } else if (FlagValue(argv[i], "--tag=", &v)) {
      tag = v;  // empty value omits the tag param entirely
    } else if (FlagValue(argv[i], "--components=", &v)) {
      components_spec = v;
    } else if (FlagValue(argv[i], "--field=", &v)) {
      size_t eq = v.find('=');
      if (eq == std::string::npos) {
        fprintf(stderr, "--field expects name=value: %s\n", argv[i]);
        return 2;
      }
      field_values.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (FlagValue(argv[i], "--directory-host=", &v)) {
      directory_host = v;
    } else if (FlagValue(argv[i], "--key-expires=", &v)) {
      key_expires_str = v;
    } else if (FlagValue(argv[i], "--realm=", &v)) {
      realm = v;
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
  std::string pub(reinterpret_cast<const char*>(public_key), 32);

  if (emit == "jwks" || emit == "all") {
    // JWKS shape ExtractEd25519Key accepts: OKP/Ed25519, base64url(x), kid.
    std::string x = Base64UrlEncodeNoPad(pub);
    std::string jwks = absl::StrCat(
        "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"", kid,
        "\",\"x\":\"", x, "\"}]}");
    if (emit == "jwks") {
      printf("%s\n", jwks.c_str());
    } else {
      printf("JWKS=%s\n", jwks.c_str());
    }
  }

  if (emit == "keystore") {
    // A pagespeed-webbotauth-keys.conf document (the WarmedKeyStore codec)
    // holding this tool's public key, so a test harness can pre-warm the
    // nginx key store without running the worker.  --realm selects the trust
    // domain: "wba" for the observe-only verifier binding, "rsl" for the
    // RSL-CAP enforcement binding (both use MultiHostWarmedKeyDirectoryProvider
    // with the matching realm).
    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t key_expires = key_expires_str.empty()
                              ? now + 3600
                              : static_cast<int64_t>(strtoll(
                                    key_expires_str.c_str(), nullptr, 10));
    WarmedKeyStore store;
    store.Put(WarmedKeyStore::StoreKey(realm, directory_host, kid), pub,
              key_expires);
    printf("%s", store.Serialize(now).c_str());
    return 0;
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

    std::vector<std::string> lic = SplitCsv(lic_csv);
    std::vector<std::string> scope = SplitCsv(scope_csv);

    const std::string header_json =
        absl::StrCat("{", JsonStr("alg", "EdDSA"), ",",
                     JsonStr("typ", "RSL-CAP"), ",", JsonStr("kid", kid), "}");
    std::string payload_body =
        absl::StrCat(JsonStr("iss", iss), ",", JsonStr("sub", sub), ",",
                     JsonInt("exp", exp), ",", JsonInt("iat", now));
    absl::StrAppend(&payload_body, ",", JsonStrArray("lic", lic));
    absl::StrAppend(&payload_body, ",", JsonStrArray("scope", scope));
    const std::string payload_json = absl::StrCat("{", payload_body, "}");

    const std::string header_b64 = Base64UrlEncodeNoPad(header_json);
    const std::string payload_b64 = Base64UrlEncodeNoPad(payload_json);
    const std::string signing_input =
        absl::StrCat(header_b64, ".", payload_b64);

    unsigned char sig[64];
    ed25519_sign(sig,
                 reinterpret_cast<const unsigned char*>(signing_input.data()),
                 signing_input.size(), public_key, private_key);
    std::string sig_bytes(reinterpret_cast<const char*>(sig), 64);
    if (tamper) {
      sig_bytes[0] =
          static_cast<char>(sig_bytes[0] ^ 0x01);  // -> kBadSignature
    }
    const std::string sig_b64 = Base64UrlEncodeNoPad(sig_bytes);
    const std::string token = absl::StrCat(signing_input, ".", sig_b64);

    // Emit the full Authorization header value, ready to send verbatim.
    printf("TOKEN=License %s\n", token.c_str());
    return 0;
  }

  if (emit == "headers" || emit == "all") {
    int64_t created =
        created_str.empty()
            ? static_cast<int64_t>(time(nullptr))
            : static_cast<int64_t>(strtoll(created_str.c_str(), nullptr, 10));

    // Build @signature-params exactly as the header and the signature base
    // must agree on, using the library serializer (component order + params
    // order). Scanner-probe param order: created, expires, keyid, alg,
    // nonce, tag.
    SfvInnerList list;
    list.components = SplitComponents(components_spec);
    SfvParam created_p;
    created_p.name = "created";
    created_p.type = SfvParam::kInteger;
    created_p.int_value = created;
    list.params.push_back(created_p);
    if (!expires_str.empty()) {
      SfvParam expires_p;
      expires_p.name = "expires";
      expires_p.type = SfvParam::kInteger;
      expires_p.int_value =
          static_cast<int64_t>(strtoll(expires_str.c_str(), nullptr, 10));
      list.params.push_back(expires_p);
    }
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
    if (!nonce.empty()) {
      SfvParam nonce_p;
      nonce_p.name = "nonce";
      nonce_p.type = SfvParam::kString;
      nonce_p.str_value = nonce;
      list.params.push_back(nonce_p);
    }
    if (!tag.empty()) {
      SfvParam tag_p;
      tag_p.name = "tag";
      tag_p.type = SfvParam::kString;
      tag_p.str_value = tag;
      list.params.push_back(tag_p);
    }

    std::string params = SerializeSignatureParams(list);

    BaseRequestView brv;
    brv.method = method;
    brv.authority = authority;
    brv.path = path;
    for (const auto& [name, value] : field_values) {
      brv.fields.push_back({name, value});
    }
    std::string base;
    if (!BuildSignatureBase(brv, list.components, params, &base)) {
      fprintf(stderr,
              "cannot build signature base: a covered component is not "
              "supported or has no --field value\n");
      return 2;
    }

    unsigned char sig[64];
    ed25519_sign(sig, reinterpret_cast<const unsigned char*>(base.data()),
                 base.size(), public_key, private_key);
    std::string sig_bytes(reinterpret_cast<const char*>(sig), 64);
    if (tamper) {
      // Flip a bit so verification must fail (-> Verdict::kUnknown).
      sig_bytes[0] = static_cast<char>(sig_bytes[0] ^ 0x01);
    }

    std::string sig_input = absl::StrCat("sig1=", params);
    std::string signature =
        absl::StrCat("sig1=:", Base64Encode(sig_bytes), ":");

    printf("SIGINPUT=%s\n", sig_input.c_str());
    printf("SIG=%s\n", signature.c_str());
  }

  return 0;
}

}  // namespace
}  // namespace webbotauth
}  // namespace pagespeed

int main(int argc, char** argv) {
  return pagespeed::webbotauth::Run(argc, argv);
}
