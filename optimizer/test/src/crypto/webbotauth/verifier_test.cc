// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// test/pagespeed/kernel/webbotauth/verifier_test.cc @ 8e15a19ce (2026-07-02).
// Keep logic in sync manually; the RFC 9421/8941 core is standards-frozen.
// The 1.15 CacheEntryCodecTest is not ported (the CacheInterface TTL codec is
// not ported to 2.0; see src/crypto/webbotauth/key_directory.h).
//
// Hermetic, offline unit test for the Web-Bot-Auth (RFC 9421) verifier core.
// Mints its own Ed25519 keypair with @ed25519 (ed25519_create_keypair /
// ed25519_sign), builds a canonical signature base with the SAME serializer
// the library uses, and injects a FAKE key-directory provider -- so it touches
// NO network and NO server runtime.

#include "src/crypto/webbotauth/verifier.h"

#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "ed25519.h"  // NOLINT(build/include_subdir)
#include "gtest/gtest.h"
#include "src/crypto/webbotauth/base64.h"
#include "src/crypto/webbotauth/classifier.h"
#include "src/crypto/webbotauth/header_parser.h"
#include "src/crypto/webbotauth/jwks.h"
#include "src/crypto/webbotauth/key_directory.h"
#include "src/crypto/webbotauth/signature_base.h"

namespace pagespeed {
namespace webbotauth {
namespace {

const char kTestKeyId[] = "test-key-1";
const char kTestHost[] = "directory.example";  // operator-mapped dir host
const char kAuthority[] = "example.com";

// Encode raw bytes as standard base64 (with padding), for building a Signature
// header value. Implemented locally so the test does not depend on the library
// encoder (the library only provides a decoder).
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
    if (c == '+')
      out.push_back('-');
    else if (c == '/')
      out.push_back('_');
    else if (c == '=')
      continue;  // strip padding
    else
      out.push_back(c);
  }
  return out;
}

// A fake key-directory provider that records the (host, keyid) it was asked
// for and returns the test key only for the matching host. Any other host
// yields kError, modeling an off-allowlist / SSRF-refused lookup WITHOUT a
// network.
class FakeKeyDirectoryProvider : public KeyDirectoryProvider {
 public:
  FakeKeyDirectoryProvider(std::string_view host, std::string_view keyid,
                           std::string_view raw_key_32)
      : host_(host), keyid_(keyid), raw_key_(raw_key_32) {}

  KeyLookupResult GetKey(std::string_view host, std::string_view keyid,
                         int64_t /*now*/) override {
    ++call_count_;
    last_host_ = std::string(host);
    last_keyid_ = std::string(keyid);
    if (host_ != host) {
      // Off-allowlist host: refuse, return no key material.
      return KeyLookupResult::Error();
    }
    if (keyid_ != keyid) {
      return KeyLookupResult::NotFound();
    }
    return KeyLookupResult::Found(raw_key_);
  }

  int call_count() const { return call_count_; }
  const std::string& last_host() const { return last_host_; }

 private:
  std::string host_;
  std::string keyid_;
  std::string raw_key_;
  int call_count_ = 0;
  std::string last_host_;
  std::string last_keyid_;
};

// Test fixture: mints a keypair once.
class VerifierTest : public ::testing::Test {
 protected:
  void SetUp() override {
    unsigned char seed[32] = {0};
    // A non-zero, deterministic seed.
    for (int i = 0; i < 32; ++i) seed[i] = static_cast<unsigned char>(i + 1);
    ed25519_create_keypair(public_key_, private_key_, seed);
    pub_.assign(reinterpret_cast<const char*>(public_key_), 32);
    now_ = 1700000000;  // fixed wall clock for the test
  }

  // Common signature params: created (optional), keyid, alg, tag (optional).
  static void AppendParams(SfvInnerList* list, int64_t created,
                           bool with_created, std::string_view tag) {
    if (with_created) {
      SfvParam created_p;
      created_p.name = "created";
      created_p.type = SfvParam::kInteger;
      created_p.int_value = created;
      list->params.push_back(created_p);
    }
    SfvParam keyid_p;
    keyid_p.name = "keyid";
    keyid_p.type = SfvParam::kString;
    keyid_p.str_value = kTestKeyId;
    list->params.push_back(keyid_p);
    SfvParam alg_p;
    alg_p.name = "alg";
    alg_p.type = SfvParam::kString;
    alg_p.str_value = "ed25519";
    list->params.push_back(alg_p);
    if (!tag.empty()) {
      SfvParam tag_p;
      tag_p.name = "tag";
      tag_p.type = SfvParam::kString;
      tag_p.str_value = std::string(tag);
      list->params.push_back(tag_p);
    }
  }

  // Sign `base` and fill the request's signature headers under `label`.
  void SignInto(std::string_view base, std::string_view label,
                std::string_view params, RequestView* req,
                std::string* sig_input_storage, std::string* sig_storage) {
    unsigned char sig[64];
    ed25519_sign(sig, reinterpret_cast<const unsigned char*>(base.data()),
                 base.size(), public_key_, private_key_);
    std::string sig_bytes(reinterpret_cast<const char*>(sig), 64);
    *sig_input_storage = absl::StrCat(label, "=", params);
    *sig_storage = absl::StrCat(label, "=:", Base64Encode(sig_bytes), ":");
    req->signature_input = *sig_input_storage;
    req->signature = *sig_storage;
  }

  // Build a well-formed web-bot-auth-tagged signed request over
  // (@method @authority @path). `created` is the signature timestamp; pass
  // now_ for fresh. `tag` defaults to the required web-bot-auth tag; pass ""
  // to omit it or another value for non-web-bot-auth material.
  void BuildSignedRequest(int64_t created, RequestView* req,
                          std::string* sig_input_storage,
                          std::string* sig_storage,
                          std::string_view tag = kWebBotAuthTag) {
    // Build the @signature-params serialization that BOTH the header and the
    // signature base must agree on, using the library serializer.
    SfvInnerList list;
    list.components = {"@method", "@authority", "@path"};
    AppendParams(&list, created, /*with_created=*/true, tag);

    std::string params = SerializeSignatureParams(list);

    // Signature base.
    BaseRequestView brv;
    brv.method = "GET";
    brv.authority = kAuthority;
    brv.path = "/";
    std::string base;
    ASSERT_TRUE(BuildSignatureBase(brv, list.components, params, &base));

    req->method = "GET";
    req->authority = kAuthority;
    req->path = "/";
    req->user_agent = "Mozilla/5.0";
    req->directory_host = kTestHost;
    SignInto(base, "sig1", params, req, sig_input_storage, sig_storage);
  }

  // Build a signed request whose @signature-params carries NEITHER created NOR
  // expires (only keyid + alg). Such a signature has no bounded lifetime. The
  // crypto is genuinely valid over the no-timestamp params, so only the
  // freshness gate can keep it from being trusted.
  void BuildSignedRequestNoTimestamps(RequestView* req,
                                      std::string* sig_input_storage,
                                      std::string* sig_storage) {
    SfvInnerList list;
    list.components = {"@method", "@authority", "@path"};
    AppendParams(&list, 0, /*with_created=*/false, kWebBotAuthTag);

    std::string params = SerializeSignatureParams(list);
    BaseRequestView brv;
    brv.method = "GET";
    brv.authority = kAuthority;
    brv.path = "/";
    std::string base;
    ASSERT_TRUE(BuildSignatureBase(brv, list.components, params, &base));
    req->method = "GET";
    req->authority = kAuthority;
    req->path = "/";
    req->user_agent = "Mozilla/5.0";
    req->directory_host = kTestHost;
    SignInto(base, "sig1", params, req, sig_input_storage, sig_storage);
  }

  // Build a scanner-probe-shaped signed request: covered components
  // ("@authority" "signature-agent"), params created/expires/keyid/alg/
  // nonce/tag="web-bot-auth", plus the Signature-Agent request field carrying
  // an sf-string key-directory URL — the exact shape
  // agent-readability-scanner's signedAgentVerification probe sends.
  void BuildScannerShapedRequest(int64_t now, RequestView* req,
                                 std::string* sig_input_storage,
                                 std::string* sig_storage) {
    SfvInnerList list;
    list.components = {"@authority", "signature-agent"};
    SfvParam created_p;
    created_p.name = "created";
    created_p.type = SfvParam::kInteger;
    created_p.int_value = now;
    list.params.push_back(created_p);
    SfvParam expires_p;
    expires_p.name = "expires";
    expires_p.type = SfvParam::kInteger;
    expires_p.int_value = now + 300;
    list.params.push_back(expires_p);
    SfvParam keyid_p;
    keyid_p.name = "keyid";
    keyid_p.type = SfvParam::kString;
    keyid_p.str_value = kTestKeyId;
    list.params.push_back(keyid_p);
    SfvParam alg_p;
    alg_p.name = "alg";
    alg_p.type = SfvParam::kString;
    alg_p.str_value = "ed25519";
    list.params.push_back(alg_p);
    SfvParam nonce_p;
    nonce_p.name = "nonce";
    nonce_p.type = SfvParam::kString;
    nonce_p.str_value = "abc123nonce";
    list.params.push_back(nonce_p);
    SfvParam tag_p;
    tag_p.name = "tag";
    tag_p.type = SfvParam::kString;
    tag_p.str_value = std::string(kWebBotAuthTag);
    list.params.push_back(tag_p);

    std::string params = SerializeSignatureParams(list);

    BaseRequestView brv;
    brv.authority = kAuthority;
    brv.fields.push_back({"signature-agent", kSignatureAgentValue});
    std::string base;
    ASSERT_TRUE(BuildSignatureBase(brv, list.components, params, &base));

    req->method = "GET";
    req->authority = kAuthority;
    req->path = "/";
    req->user_agent = "RenderPeek-SignedAgentProbe/1.0";
    req->directory_host = kTestHost;
    req->fields.push_back({"Signature-Agent", kSignatureAgentValue});
    SignInto(base, "sig1", params, req, sig_input_storage, sig_storage);
  }

  // The Signature-Agent field value as sent on the wire: an sf-string, i.e.
  // INCLUDING the quotes (the signature base covers the raw serialization).
  static constexpr std::string_view kSignatureAgentValue =
      "\"https://directory.example/.well-known/"
      "http-message-signatures-directory\"";

  // Typed-param factories for ad-hoc signature profiles.
  static SfvParam IntParam(std::string_view name, int64_t v) {
    SfvParam p;
    p.name = std::string(name);
    p.type = SfvParam::kInteger;
    p.int_value = v;
    return p;
  }
  static SfvParam StrParam(std::string_view name, std::string_view v) {
    SfvParam p;
    p.name = std::string(name);
    p.type = SfvParam::kString;
    p.str_value = std::string(v);
    return p;
  }

  // Sign an arbitrary components+params profile over the fixture request
  // (GET example.com /). Uses any fields already present on *req for base
  // construction, so callers can cover field components.
  void BuildCustomSignedRequest(const std::vector<std::string>& components,
                                const std::vector<SfvParam>& params,
                                RequestView* req,
                                std::string* sig_input_storage,
                                std::string* sig_storage) {
    SfvInnerList list;
    list.components = components;
    list.params = params;
    std::string p = SerializeSignatureParams(list);
    BaseRequestView brv;
    brv.method = "GET";
    brv.authority = kAuthority;
    brv.path = "/";
    brv.fields = req->fields;
    std::string base;
    ASSERT_TRUE(BuildSignatureBase(brv, components, p, &base));
    req->method = "GET";
    req->authority = kAuthority;
    req->path = "/";
    req->directory_host = kTestHost;
    SignInto(base, "sig1", p, req, sig_input_storage, sig_storage);
  }

  unsigned char public_key_[32];
  unsigned char private_key_[64];
  std::string pub_;
  int64_t now_;
};

// (a) Valid signed request, key from fake dir -> signed-agent.
TEST_F(VerifierTest, ValidSignedRequestIsSignedAgent) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;  // empty: not a verified bot

  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kSignedAgent, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ(kTestKeyId, r.keyid);
}

// (a') Same request but keyid registered in the operator map -> verified-bot.
TEST_F(VerifierTest, RegisteredKeyIdIsVerifiedBot) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  registry.Register(kTestKeyId, "ExampleBot");

  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kVerifiedBot, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ("ExampleBot", r.bot_name);
}

// (b) Spoofed UA, NO signature headers -> NOT a trusted tier (human here).
TEST_F(VerifierTest, SpoofedUaNoSignatureEarnsNoTrust) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.user_agent = "GPTBot/1.0";  // spoofed bot UA
  req.signature_input = "";
  req.signature = "";
  req.directory_host = kTestHost;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  registry.Register(kTestKeyId, "ExampleBot");  // even if registered...

  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  // A spoofed UA with no signature gains no trust.
  EXPECT_NE(Verdict::kSignedAgent, r.verdict);
  EXPECT_NE(Verdict::kVerifiedBot, r.verdict);
  EXPECT_EQ(Verdict::kHuman, r.verdict) << "reason=" << r.reason;
  // The provider must not have been consulted.
  EXPECT_EQ(0, provider.call_count());
}

// (b') Spoofed UA WITH a present-but-garbage Signature-Input -> strict unknown.
TEST_F(VerifierTest, SpoofedUaGarbageSignatureIsUnknown) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.user_agent = "GPTBot/1.0";
  req.signature_input = "this is not a valid structured field !!!";
  req.signature = "sig1=:AAAA:";
  req.directory_host = kTestHost;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;

  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// (c) Tampered signature (flip one byte) -> unknown.
TEST_F(VerifierTest, TamperedSignatureIsUnknown) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);

  // Decode the signature, flip a byte, re-encode.
  // s looks like  sig1=:BASE64:
  size_t colon1 = s.find(':');
  size_t colon2 = s.rfind(':');
  ASSERT_NE(colon1, std::string::npos);
  ASSERT_NE(colon2, colon1);
  std::string b64 = s.substr(colon1 + 1, colon2 - colon1 - 1);
  std::string raw;
  ASSERT_TRUE(Base64Decode(b64, &raw));
  ASSERT_EQ(64u, raw.size());
  raw[10] = static_cast<char>(raw[10] ^ 0x01);  // flip 1 bit
  std::string tampered = absl::StrCat("sig1=:", Base64Encode(raw), ":");
  req.signature = tampered;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;

  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// (d) Off-allowlist directory host -> unknown, and no key returned.
TEST_F(VerifierTest, OffAllowlistHostIsUnknownNoKey) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);
  // Point the operator-mapped directory host at a host the provider refuses.
  req.directory_host = "evil.example";  // not the fake provider's host

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;

  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
  // The provider was asked, but refused (off-allowlist) and returned no key.
  EXPECT_EQ(1, provider.call_count());
  EXPECT_EQ("evil.example", provider.last_host());
}

// --- Robustness cases (cheap, high value) ---

// Unsupported covered component (@query) -> unknown.
TEST_F(VerifierTest, UnsupportedComponentIsUnknown) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  // RequestView fields are non-owning string_views, so back the header value
  // with storage that outlives the VerifyAndClassify call (assigning a StrCat
  // temporary directly would leave signature_input dangling).
  std::string sig_input =
      absl::StrCat("sig1=(\"@method\" \"@query\");created=", now_, ";keyid=\"",
                   kTestKeyId, "\";alg=\"ed25519\";tag=\"web-bot-auth\"");
  req.signature_input = sig_input;
  // Any non-empty signature; parse fails before crypto.
  req.signature = "sig1=:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// alg != ed25519 -> unknown.
TEST_F(VerifierTest, NonEd25519AlgIsUnknown) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  // Non-owning string_view field: keep the value alive past the verify call.
  std::string sig_input = absl::StrCat(
      "sig1=(\"@method\" \"@authority\" \"@path\");created=", now_, ";keyid=\"",
      kTestKeyId, "\";alg=\"rsa-pss-sha512\";tag=\"web-bot-auth\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Signature not 64 bytes -> unknown.
TEST_F(VerifierTest, ShortSignatureIsUnknown) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);
  // Replace with a 10-byte signature. Reuse the persistent `s` storage so the
  // non-owning req.signature string_view does not dangle on a StrCat
  // temporary.
  std::string shortsig(10, '\x01');
  s = absl::StrCat("sig1=:", Base64Encode(shortsig), ":");
  req.signature = s;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Expired (created far in the past, no expires) -> unknown.
TEST_F(VerifierTest, ExpiredSignatureIsUnknown) {
  RequestView req;
  std::string si, s;
  // created 10 hours ago; default max age is 1 hour.
  BuildSignedRequest(now_ - 36000, &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Future-dated (created far in the future) -> unknown.
TEST_F(VerifierTest, FutureDatedSignatureIsUnknown) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_ + 36000, &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// A signature carrying NEITHER created NOR expires has an unbounded lifetime
// and is replayable forever. The fake provider holds the key AND the keyid is
// registered as a verified bot, so the freshness gate is the ONLY thing that
// can keep it from being trusted -- even ten years after signing. (bug 274-h2)
TEST_F(VerifierTest, NoTimestampSignatureIsUnknown) {
  RequestView req;
  std::string si, s;
  BuildSignedRequestNoTimestamps(&req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  registry.Register(kTestKeyId, "ExampleBot");

  VerifyResult r =
      VerifyAndClassify(req, &provider, registry, now_ + 315360000);  // +10y
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Malformed structured field -> unknown, no throw/crash.
TEST_F(VerifierTest, MalformedSignatureInputIsUnknownNoThrow) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  req.signature_input = "sig1=((((";  // garbage
  req.signature = "sig1=:!!!!:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Key not found in the (allowlisted) directory -> unknown.
TEST_F(VerifierTest, KeyNotFoundIsUnknown) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);

  // Provider has a DIFFERENT keyid registered, so the lookup is kNotFound.
  FakeKeyDirectoryProvider provider(kTestHost, "some-other-key", pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// --- Covered-components / tag-filtering follow-up ---

// The scanner probe shape — ("@authority" "signature-agent"), created/
// expires/keyid/alg/nonce/tag="web-bot-auth", quoted sf-string
// Signature-Agent — verifies. The request field is provided with the
// original wire-case name ("Signature-Agent") to prove field-name matching
// is ASCII-case-insensitive.
TEST_F(VerifierTest, ScannerShapedProbeVerifies) {
  RequestView req;
  std::string si, s;
  BuildScannerShapedRequest(now_, &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kSignedAgent, r.verdict) << "reason=" << r.reason;

  // With the keyid registered, the same probe is a verified bot.
  registry.Register(kTestKeyId, "RenderPeek");
  VerifyResult r2 = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kVerifiedBot, r2.verdict) << "reason=" << r2.reason;
  EXPECT_EQ("RenderPeek", r2.bot_name);
}

// A covered field component with no matching request field fails closed.
TEST_F(VerifierTest, MissingCoveredFieldIsUnknown) {
  RequestView req;
  std::string si, s;
  BuildScannerShapedRequest(now_, &req, &si, &s);
  req.fields.clear();  // the Signature-Agent field never arrived

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ("missing-covered-field", r.reason);
}

// A signature with NO tag param is not web-bot-auth material: classified as
// if unsigned (kHuman), never invalid, and no key lookup happens.
TEST_F(VerifierTest, UntaggedSignatureClassifiedAsUnsigned) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s, /*tag=*/"");

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kHuman, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ(0, provider.call_count());
}

// Same for a signature tagged with some OTHER scheme's tag.
TEST_F(VerifierTest, WrongTagClassifiedAsUnsigned) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s, /*tag=*/"some-other-protocol");

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kHuman, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ(0, provider.call_count());
}

// The tag must be an sf-string: a bare token `tag=web-bot-auth` is not a
// match (fail toward "not web-bot-auth material").
TEST_F(VerifierTest, TokenTypedTagClassifiedAsUnsigned) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string sig_input =
      absl::StrCat("sig1=(\"@method\");created=", now_, ";keyid=\"", kTestKeyId,
                   "\";alg=\"ed25519\";tag=web-bot-auth");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kHuman, r.verdict) << "reason=" << r.reason;
}

// A multi-signature request (CDN signature + web-bot-auth signature): the
// tagged member is selected and verified even when it is not first, and an
// unselected member's component params cannot poison the parse.
TEST_F(VerifierTest, MultiSignatureSelectsTaggedMember) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);

  // Prepend an untagged CDN-style member (with component params) to BOTH
  // headers; the storage strings must outlive the verify call.
  std::string multi_si = absl::StrCat(
      "cdn=(\"@method\" \"@query-param\";name=\"x\");created=1;"
      "keyid=\"cdn-key\";alg=\"rsa-v1_5-sha256\", ",
      si);
  std::string multi_s = absl::StrCat("cdn=:QUJD:, ", s);
  req.signature_input = multi_si;
  req.signature = multi_s;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kSignedAgent, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ(kTestKeyId, r.keyid);
  // Exactly one key lookup: at most one signature is ever verified.
  EXPECT_EQ(1, provider.call_count());
}

// Deterministic selection: the FIRST web-bot-auth-tagged member wins. When
// it fails validation, the verdict is kUnknown — we never "try the next
// tagged signature" (that would multiply verify work under hostile input).
TEST_F(VerifierTest, FirstTaggedMemberWinsAndFailsClosed) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);

  // A tagged-but-garbage member BEFORE the valid one.
  std::string multi_si = absl::StrCat(
      "siga=(\"@method\");created=", now_,
      ";keyid=\"other\";alg=\"ed25519\";tag=\"web-bot-auth\", ", si);
  std::string multi_s = absl::StrCat("siga=:AAAA:, ", s);
  req.signature_input = multi_si;
  req.signature = multi_s;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
  // The valid second signature was never verified.
  EXPECT_EQ(0, provider.call_count());
}

// Duplicate dictionary labels are ambiguous signature material: fail closed.
TEST_F(VerifierTest, DuplicateLabelsAreUnknown) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);
  std::string dup_si = absl::StrCat(si, ", ", si);
  req.signature_input = dup_si;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Hostile input: more dictionary members than the bound -> kUnknown.
TEST_F(VerifierTest, TooManySignatureInputMembersIsUnknown) {
  RequestView req;
  std::string si, s;
  BuildSignedRequest(now_, &req, &si, &s);
  std::string many;
  for (int i = 0; i < 9; ++i) {
    if (i != 0) many += ", ";
    absl::StrAppend(&many, "m", i, "=(\"@method\");created=1");
  }
  req.signature_input = many;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Hostile input: a huge covered-components list -> kUnknown (bounded).
TEST_F(VerifierTest, TooManyCoveredComponentsIsUnknown) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string comps;
  for (int i = 0; i < 17; ++i) {
    if (i != 0) comps += " ";
    absl::StrAppend(&comps, "\"x-hdr-", i, "\"");
  }
  std::string sig_input =
      absl::StrCat("sig1=(", comps, ");created=", now_, ";keyid=\"", kTestKeyId,
                   "\";alg=\"ed25519\";tag=\"web-bot-auth\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// RFC 9421: the same component identifier must not appear twice.
TEST_F(VerifierTest, DuplicateCoveredComponentIsUnknown) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string sig_input =
      absl::StrCat("sig1=(\"@method\" \"@method\");created=", now_, ";keyid=\"",
                   kTestKeyId, "\";alg=\"ed25519\";tag=\"web-bot-auth\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// RFC 9421 component identifiers are lowercase: an uppercase field-name
// component is unsupported.
TEST_F(VerifierTest, UppercaseFieldComponentIsUnknown) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string sig_input = absl::StrCat(
      "sig1=(\"@authority\" \"Signature-Agent\");created=", now_, ";keyid=\"",
      kTestKeyId, "\";alg=\"ed25519\";tag=\"web-bot-auth\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Component parameters on the SELECTED member are unsupported profile.
TEST_F(VerifierTest, ComponentParamsOnSelectedMemberIsUnknown) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string sig_input =
      absl::StrCat("sig1=(\"@method\";req);created=", now_, ";keyid=\"",
                   kTestKeyId, "\";alg=\"ed25519\";tag=\"web-bot-auth\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// --- Signature-base field canonicalization (RFC 9421 section 2.1) ---

TEST(SignatureBaseTest, FieldValueOwsTrimmedPerLine) {
  BaseRequestView req;
  req.fields.push_back({"signature-agent", "  \t\"https://d.example\" \t "});
  std::string base;
  ASSERT_TRUE(BuildSignatureBase(req, {"signature-agent"}, "(x)", &base));
  EXPECT_EQ(
      "\"signature-agent\": \"https://d.example\"\n"
      "\"@signature-params\": (x)",
      base);
}

TEST(SignatureBaseTest, RepeatedFieldLinesJoinedWithCommaSpace) {
  BaseRequestView req;
  req.fields.push_back({"x-multi", " a "});
  req.fields.push_back({"x-other", "zzz"});
  req.fields.push_back({"X-Multi", "b"});  // wire-case name still matches
  std::string base;
  ASSERT_TRUE(BuildSignatureBase(req, {"x-multi"}, "(x)", &base));
  EXPECT_EQ("\"x-multi\": a, b\n\"@signature-params\": (x)", base);
}

TEST(SignatureBaseTest, EmptyFieldValueIsEmptyComponentValue) {
  BaseRequestView req;
  req.fields.push_back({"x-empty", ""});
  std::string base;
  ASSERT_TRUE(BuildSignatureBase(req, {"x-empty"}, "(x)", &base));
  EXPECT_EQ("\"x-empty\": \n\"@signature-params\": (x)", base);
}

TEST(SignatureBaseTest, MissingCoveredFieldFailsBaseConstruction) {
  BaseRequestView req;
  req.method = "GET";
  std::string base;
  EXPECT_FALSE(BuildSignatureBase(req, {"@method", "x-absent"}, "(x)", &base));
}

TEST(SignatureBaseTest, UnknownDerivedComponentFailsBaseConstruction) {
  BaseRequestView req;
  req.method = "GET";
  std::string base;
  EXPECT_FALSE(BuildSignatureBase(req, {"@query"}, "(x)", &base));
}

// --- Header-parser tag selection unit coverage ---

TEST(HeaderParserTest, SupportedComponentGrammar) {
  EXPECT_TRUE(IsSupportedComponent("@method"));
  EXPECT_TRUE(IsSupportedComponent("@authority"));
  EXPECT_TRUE(IsSupportedComponent("@path"));
  EXPECT_TRUE(IsSupportedComponent("signature-agent"));
  EXPECT_TRUE(IsSupportedComponent("user-agent"));
  EXPECT_FALSE(IsSupportedComponent("@query"));
  EXPECT_FALSE(IsSupportedComponent(""));
  EXPECT_FALSE(IsSupportedComponent("Signature-Agent"));     // uppercase
  EXPECT_FALSE(IsSupportedComponent("bad header"));          // space
  EXPECT_FALSE(IsSupportedComponent("hdr\"quote"));          // dquote
  EXPECT_FALSE(IsSupportedComponent(std::string(65, 'a')));  // too long
}

TEST(HeaderParserTest, NoTagYieldsNoWebBotAuthSignature) {
  SignatureRequest out;
  std::string reason;
  EXPECT_EQ(ParseStatus::kNoWebBotAuthSignature,
            ParseSignatureHeaders(
                "sig1=(\"@method\");created=1;keyid=\"k\";alg=\"ed25519\"",
                "sig1=:AAAA:", &out, &reason));
  EXPECT_EQ("no-web-bot-auth-tag", reason);
}

TEST(HeaderParserTest, MalformedInputYieldsMalformed) {
  SignatureRequest out;
  std::string reason;
  EXPECT_EQ(ParseStatus::kMalformed,
            ParseSignatureHeaders("sig1=((((", "sig1=:AAAA:", &out, &reason));
}

// --- Required covered components (draft section 4.2) ---

// "@authority" is mandatory: a signature covering only "signature-agent"
// carries no request identity and would replay against any host. Rejected.
TEST_F(VerifierTest, CoveredWithoutAuthorityIsUnknown) {
  RequestView req;
  req.fields.push_back({"signature-agent", kSignatureAgentValue});
  std::string si, s;
  BuildCustomSignedRequest(
      {"signature-agent"},
      {IntParam("created", now_), StrParam("keyid", kTestKeyId),
       StrParam("alg", "ed25519"), StrParam("tag", "web-bot-auth")},
      &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ(0, provider.call_count());  // rejected before any key lookup
}

// "@authority" alone is a valid minimal profile (no Signature-Agent header
// sent, so nothing else is mandatory).
TEST_F(VerifierTest, AuthorityOnlyCoveredVerifies) {
  RequestView req;
  std::string si, s;
  BuildCustomSignedRequest(
      {"@authority"},
      {IntParam("created", now_), StrParam("keyid", kTestKeyId),
       StrParam("alg", "ed25519"), StrParam("tag", "web-bot-auth")},
      &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kSignedAgent, r.verdict) << "reason=" << r.reason;
}

// Draft section 4.2: a Signature-Agent header, when sent, MUST be covered.
// A valid @authority-only signature on a request that DOES carry
// Signature-Agent fails closed.
TEST_F(VerifierTest, SignatureAgentHeaderNotCoveredIsUnknown) {
  RequestView req;
  std::string si, s;
  BuildCustomSignedRequest(
      {"@authority"},
      {IntParam("created", now_), StrParam("keyid", kTestKeyId),
       StrParam("alg", "ed25519"), StrParam("tag", "web-bot-auth")},
      &req, &si, &s);
  // The header arrives on the wire but the signature does not cover it.
  req.fields.push_back({"Signature-Agent", kSignatureAgentValue});

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ("signature-agent-not-covered", r.reason);
}

// A signature with expires but no created is acceptable (bounded lifetime).
TEST_F(VerifierTest, ExpiresOnlySignatureVerifies) {
  RequestView req;
  std::string si, s;
  BuildCustomSignedRequest(
      {"@authority"},
      {IntParam("expires", now_ + 300), StrParam("keyid", kTestKeyId),
       StrParam("alg", "ed25519"), StrParam("tag", "web-bot-auth")},
      &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kSignedAgent, r.verdict) << "reason=" << r.reason;
}

// --- Tag last-wins semantics (RFC 8941) ---

// tag="x";tag="web-bot-auth": the LAST tag wins -> selected (and then fails
// on the garbage signature, i.e. kUnknown — proving selection happened).
TEST_F(VerifierTest, DuplicateTagLastIsWebBotAuthSelects) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string sig_input = absl::StrCat(
      "sig1=(\"@authority\");created=", now_, ";keyid=\"", kTestKeyId,
      "\";alg=\"ed25519\";tag=\"x\";tag=\"web-bot-auth\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// tag="web-bot-auth";tag="x": effective tag is "x" -> NOT selected, treated
// as non-web-bot-auth material.
TEST_F(VerifierTest, DuplicateTagLastIsOtherNotSelected) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string sig_input = absl::StrCat(
      "sig1=(\"@authority\");created=", now_, ";keyid=\"", kTestKeyId,
      "\";alg=\"ed25519\";tag=\"web-bot-auth\";tag=\"x\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kHuman, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ(0, provider.call_count());
}

// An escaped quote inside the tag sf-string is part of the VALUE:
// tag="web-bot-auth\" x" is the string `web-bot-auth" x` -> not selected.
TEST_F(VerifierTest, EscapedTagValueNotSelected) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string sig_input =
      absl::StrCat("sig1=(\"@authority\");created=", now_, ";keyid=\"",
                   kTestKeyId, "\";alg=\"ed25519\";tag=\"web-bot-auth\\\" x\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kHuman, r.verdict) << "reason=" << r.reason;
}

// RFC 8941 integers cap at 15 digits: a 16-digit created is malformed.
TEST_F(VerifierTest, SixteenDigitCreatedIsUnknown) {
  RequestView req;
  req.method = "GET";
  req.authority = kAuthority;
  req.path = "/";
  req.directory_host = kTestHost;
  std::string sig_input =
      absl::StrCat("sig1=(\"@authority\");created=1234567890123456;keyid=\"",
                   kTestKeyId, "\";alg=\"ed25519\";tag=\"web-bot-auth\"");
  req.signature_input = sig_input;
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// --- Cross-implementation wire-compat fixture (production probe signer) ---

// The literals below were minted by the PRODUCTION scanner signer
// (agent-readability-scanner src/signedAgentVerification.mjs,
// buildSignedHeaders) with a throwaway deterministic test seed
// (sha256("mps2-crossimpl-fixture-v1") — never a real probe key), url
// https://example.com/, now=1700000000, nonce "fixture-nonce-abc123".
// This pins wire compatibility with the real probe signer across BOTH
// implementations.
// If this test breaks, wire compat broke: fix the code — do NOT re-mint the
// fixture to make it pass unless the scanner itself changed intentionally.
TEST(CrossImplementationTest, ScannerMintedFixtureVerifies) {
  constexpr std::string_view kFixtureKeyId =
      "QE4FtAHnaqPt9OILJOsHWmlAdLtJnlxq2C0woGWztrY";
  constexpr std::string_view kFixturePubB64Url =
      "Dq9hy_8pCLuBmokHy_M0jZ83pMR8_Clh3V6odzZ3V-E";
  constexpr int64_t kFixtureNow = 1700000000;
  constexpr std::string_view kFixtureSignatureAgent =
      "\"https://modpagespeed.com/.well-known/"
      "http-message-signatures-directory\"";
  constexpr std::string_view kFixtureSignatureInput =
      "sig1=(\"@authority\" \"signature-agent\");created=1700000000;"
      "expires=1700000300;"
      "keyid=\"QE4FtAHnaqPt9OILJOsHWmlAdLtJnlxq2C0woGWztrY\";"
      "alg=\"ed25519\";nonce=\"fixture-nonce-abc123\";tag=\"web-bot-auth\"";
  constexpr std::string_view kFixtureSignature =
      "sig1=:/lRBQFCArSw+RSPOYKE7w+McFV8yTSidOyr4gag+hvz793mLz9zKSMCUvUF5o/"
      "X2U+ClkALsy++qAkl+I/z2BQ==:";

  std::string raw_key;
  ASSERT_TRUE(Base64UrlDecode(kFixturePubB64Url, &raw_key));
  ASSERT_EQ(32u, raw_key.size());

  RequestView req;
  req.method = "GET";
  req.authority = "example.com";
  req.path = "/";
  req.signature_input = kFixtureSignatureInput;
  req.signature = kFixtureSignature;
  req.user_agent = "RenderPeek-SignedAgentProbe/1.0";
  req.directory_host = "directory.example";
  req.fields.push_back({"Signature-Agent", kFixtureSignatureAgent});

  FakeKeyDirectoryProvider provider("directory.example", kFixtureKeyId,
                                    raw_key);
  VerifiedBotRegistry registry;
  registry.Register(kFixtureKeyId, "RenderPeek");

  VerifyResult r = VerifyAndClassify(req, &provider, registry, kFixtureNow);
  EXPECT_EQ(Verdict::kVerifiedBot, r.verdict) << "reason=" << r.reason;
  EXPECT_EQ("RenderPeek", r.bot_name);
  EXPECT_EQ(kFixtureKeyId, r.keyid);
}

// --- Unit tests of internal helpers ---

TEST(Base64Test, RoundTripStandardAndUrl) {
  // Good input decodes; bad input is rejected.
  std::string good;
  ASSERT_TRUE(Base64Decode("QUJD", &good));  // "ABC"
  EXPECT_EQ("ABC", good);
  std::string urlgood;
  ASSERT_TRUE(Base64UrlDecode("QUJD", &urlgood));
  EXPECT_EQ("ABC", urlgood);
  // Unpadded base64url (JWK style) decodes too.
  std::string urlnopad;
  ASSERT_TRUE(Base64UrlDecode("QUJD", &urlnopad));
  EXPECT_EQ("ABC", urlnopad);
  // Bad char.
  std::string bad;
  EXPECT_FALSE(Base64Decode("@@@@", &bad));
  EXPECT_FALSE(Base64UrlDecode("ab+/", &bad));  // std chars invalid in url mode
}

TEST(JwksTest, ExtractsEd25519Key) {
  // Build a JWKS with a 32-byte key.
  std::string raw32(32, '\x07');
  std::string x = Base64UrlEncodeNoPad(raw32);
  std::string doc = absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\",\"x\":\"",
      x, "\"}]}");
  std::string out;
  ASSERT_TRUE(ExtractEd25519Key(doc, "k1", &out));
  EXPECT_EQ(raw32, out);
  // Wrong kid -> not found.
  std::string out2;
  EXPECT_FALSE(ExtractEd25519Key(doc, "nope", &out2));
}

// VerdictToken stability (used by the server variable surface).
TEST(ClassifierTest, VerdictTokens) {
  EXPECT_STREQ("human", VerdictToken(Verdict::kHuman));
  EXPECT_STREQ("signed-agent", VerdictToken(Verdict::kSignedAgent));
  EXPECT_STREQ("verified-bot", VerdictToken(Verdict::kVerifiedBot));
  EXPECT_STREQ("unknown", VerdictToken(Verdict::kUnknown));
}

}  // namespace
}  // namespace webbotauth
}  // namespace pagespeed
