// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Hermetic, offline unit test for the Web-Bot-Auth (RFC 9421) verifier core.
// Mints its own Ed25519 keypair with @ed25519 (ed25519_create_keypair /
// ed25519_sign), builds a canonical signature base with the SAME serializer the
// library uses, and injects a FAKE key-directory provider -- so it touches NO
// network, NO nginx, NO serf fetcher, and NO Cyclone. Mirrors the style of
// license_verifier_test.cc.

#include "pagespeed/kernel/webbotauth/verifier.h"

#include <string>

#include "ed25519.h"
#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/base64.h"
#include "pagespeed/kernel/webbotauth/classifier.h"
#include "pagespeed/kernel/webbotauth/header_parser.h"
#include "pagespeed/kernel/webbotauth/jwks.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "pagespeed/kernel/webbotauth/signature_base.h"

namespace net_instaweb {
namespace webbotauth {
namespace {

const char kTestKeyId[] = "test-key-1";
const char kTestHost[] = "directory.example";  // operator-mapped dir host
const char kAuthority[] = "example.com";

// Encode raw bytes as standard base64 (with padding), for building a Signature
// header value. Implemented locally so the test does not depend on the library
// encoder (the library only provides a decoder).
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

// A fake key-directory provider that records the (host, keyid) it was asked for
// and returns the test key only for the matching host. Any other host yields
// kError, modeling an off-allowlist / SSRF-refused lookup WITHOUT a network.
class FakeKeyDirectoryProvider : public KeyDirectoryProvider {
 public:
  FakeKeyDirectoryProvider(StringPiece host, StringPiece keyid,
                           StringPiece raw_key_32)
      : host_(host.data(), host.size()),
        keyid_(keyid.data(), keyid.size()),
        raw_key_(raw_key_32.data(), raw_key_32.size()) {}

  KeyLookupResult GetKey(StringPiece host, StringPiece keyid,
                         int64_t /*now*/) override {
    ++call_count_;
    last_host_.assign(host.data(), host.size());
    last_keyid_.assign(keyid.data(), keyid.size());
    if (StringPiece(host_) != host) {
      // Off-allowlist host: refuse, return no key material.
      return KeyLookupResult::Error();
    }
    if (StringPiece(keyid_) != keyid) {
      return KeyLookupResult::NotFound();
    }
    return KeyLookupResult::Found(raw_key_);
  }

  int call_count() const { return call_count_; }
  const GoogleString& last_host() const { return last_host_; }

 private:
  GoogleString host_;
  GoogleString keyid_;
  GoogleString raw_key_;
  int call_count_ = 0;
  GoogleString last_host_;
  GoogleString last_keyid_;
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

  // Build a well-formed signed request over (@method @authority @path).
  // `created` is the signature timestamp; pass now_ for fresh.
  void BuildSignedRequest(int64_t created, RequestView* req,
                          GoogleString* sig_input_storage,
                          GoogleString* sig_storage) {
    // Build the @signature-params serialization that BOTH the header and the
    // signature base must agree on, using the library serializer.
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
    keyid_p.str_value = kTestKeyId;
    list.params.push_back(keyid_p);
    SfvParam alg_p;
    alg_p.name = "alg";
    alg_p.type = SfvParam::kString;
    alg_p.str_value = "ed25519";
    list.params.push_back(alg_p);

    GoogleString params = SerializeSignatureParams(list);

    // Signature base.
    BaseRequestView brv;
    brv.method = "GET";
    brv.authority = kAuthority;
    brv.path = "/";
    GoogleString base = BuildSignatureBase(brv, list.components, params);

    // Sign.
    unsigned char sig[64];
    ed25519_sign(sig, reinterpret_cast<const unsigned char*>(base.data()),
                 base.size(), public_key_, private_key_);
    GoogleString sig_bytes(reinterpret_cast<const char*>(sig), 64);

    *sig_input_storage = StrCat("sig1=", params);
    *sig_storage = StrCat("sig1=:", Base64Encode(sig_bytes), ":");

    req->method = "GET";
    req->authority = kAuthority;
    req->path = "/";
    req->signature_input = *sig_input_storage;
    req->signature = *sig_storage;
    req->user_agent = "Mozilla/5.0";
    req->directory_host = kTestHost;
  }

  // Build a signed request whose @signature-params carries NEITHER created NOR
  // expires (only keyid + alg). Such a signature has no bounded lifetime. The
  // crypto is genuinely valid over the no-timestamp params, so only the
  // freshness gate can keep it from being trusted.
  void BuildSignedRequestNoTimestamps(RequestView* req,
                                      GoogleString* sig_input_storage,
                                      GoogleString* sig_storage) {
    SfvInnerList list;
    list.components = {"@method", "@authority", "@path"};
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

    GoogleString params = SerializeSignatureParams(list);
    BaseRequestView brv;
    brv.method = "GET";
    brv.authority = kAuthority;
    brv.path = "/";
    GoogleString base = BuildSignatureBase(brv, list.components, params);
    unsigned char sig[64];
    ed25519_sign(sig, reinterpret_cast<const unsigned char*>(base.data()),
                 base.size(), public_key_, private_key_);
    GoogleString sig_bytes(reinterpret_cast<const char*>(sig), 64);
    *sig_input_storage = StrCat("sig1=", params);
    *sig_storage = StrCat("sig1=:", Base64Encode(sig_bytes), ":");
    req->method = "GET";
    req->authority = kAuthority;
    req->path = "/";
    req->signature_input = *sig_input_storage;
    req->signature = *sig_storage;
    req->user_agent = "Mozilla/5.0";
    req->directory_host = kTestHost;
  }

  unsigned char public_key_[32];
  unsigned char private_key_[64];
  GoogleString pub_;
  int64_t now_;
};

// (a) Valid signed request, key from fake dir -> signed-agent.
TEST_F(VerifierTest, ValidSignedRequestIsSignedAgent) {
  RequestView req;
  GoogleString si, s;
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
  GoogleString si, s;
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
  GoogleString si, s;
  BuildSignedRequest(now_, &req, &si, &s);

  // Decode the signature, flip a byte, re-encode.
  // s looks like  sig1=:BASE64:
  size_t colon1 = s.find(':');
  size_t colon2 = s.rfind(':');
  ASSERT_NE(colon1, GoogleString::npos);
  ASSERT_NE(colon2, colon1);
  GoogleString b64 = s.substr(colon1 + 1, colon2 - colon1 - 1);
  GoogleString raw;
  ASSERT_TRUE(Base64Decode(b64, &raw));
  ASSERT_EQ(64u, raw.size());
  raw[10] = static_cast<char>(raw[10] ^ 0x01);  // flip 1 bit
  GoogleString tampered = StrCat("sig1=:", Base64Encode(raw), ":");
  req.signature = tampered;

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;

  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// (d) Off-allowlist directory host -> unknown, and no key returned.
TEST_F(VerifierTest, OffAllowlistHostIsUnknownNoKey) {
  RequestView req;
  GoogleString si, s;
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
  req.signature_input =
      StrCat("sig1=(\"@method\" \"@query\");created=", Integer64ToString(now_),
             ";keyid=\"", kTestKeyId, "\";alg=\"ed25519\"");
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
  req.signature_input =
      StrCat("sig1=(\"@method\" \"@authority\" \"@path\");created=",
             Integer64ToString(now_), ";keyid=\"", kTestKeyId,
             "\";alg=\"rsa-pss-sha512\"");
  req.signature = "sig1=:AAAA:";

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Signature not 64 bytes -> unknown.
TEST_F(VerifierTest, ShortSignatureIsUnknown) {
  RequestView req;
  GoogleString si, s;
  BuildSignedRequest(now_, &req, &si, &s);
  // Replace with a 10-byte signature.
  GoogleString shortsig(10, '\x01');
  req.signature = StrCat("sig1=:", Base64Encode(shortsig), ":");

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// Expired (created far in the past, no expires) -> unknown.
TEST_F(VerifierTest, ExpiredSignatureIsUnknown) {
  RequestView req;
  GoogleString si, s;
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
  GoogleString si, s;
  BuildSignedRequest(now_ + 36000, &req, &si, &s);

  FakeKeyDirectoryProvider provider(kTestHost, kTestKeyId, pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// A signature carrying NEITHER created NOR expires has an unbounded lifetime and
// is replayable forever. The fake provider holds the key AND the keyid is
// registered as a verified bot, so the freshness gate is the ONLY thing that can
// keep it from being trusted -- even ten years after signing. (bug 274-h2)
TEST_F(VerifierTest, NoTimestampSignatureIsUnknown) {
  RequestView req;
  GoogleString si, s;
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
  GoogleString si, s;
  BuildSignedRequest(now_, &req, &si, &s);

  // Provider has a DIFFERENT keyid registered, so the lookup is kNotFound.
  FakeKeyDirectoryProvider provider(kTestHost, "some-other-key", pub_);
  VerifiedBotRegistry registry;
  VerifyResult r = VerifyAndClassify(req, &provider, registry, now_);
  EXPECT_EQ(Verdict::kUnknown, r.verdict) << "reason=" << r.reason;
}

// --- Unit tests of internal helpers ---

TEST(Base64Test, RoundTripStandardAndUrl) {
  // Good input decodes; bad input is rejected.
  GoogleString good;
  ASSERT_TRUE(Base64Decode("QUJD", &good));  // "ABC"
  EXPECT_EQ("ABC", good);
  GoogleString urlgood;
  ASSERT_TRUE(Base64UrlDecode("QUJD", &urlgood));
  EXPECT_EQ("ABC", urlgood);
  // Unpadded base64url (JWK style) decodes too.
  GoogleString urlnopad;
  ASSERT_TRUE(Base64UrlDecode("QUJD", &urlnopad));
  EXPECT_EQ("ABC", urlnopad);
  // Bad char.
  GoogleString bad;
  EXPECT_FALSE(Base64Decode("@@@@", &bad));
  EXPECT_FALSE(Base64UrlDecode("ab+/", &bad));  // std chars invalid in url mode
}

TEST(JwksTest, ExtractsEd25519Key) {
  // Build a JWKS with a 32-byte key.
  GoogleString raw32(32, '\x07');
  GoogleString x = Base64UrlEncodeNoPad(raw32);
  GoogleString doc = StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\",\"x\":\"",
      x, "\"}]}");
  GoogleString out;
  ASSERT_TRUE(ExtractEd25519Key(doc, "k1", &out));
  EXPECT_EQ(raw32, out);
  // Wrong kid -> not found.
  GoogleString out2;
  EXPECT_FALSE(ExtractEd25519Key(doc, "nope", &out2));
}

TEST(CacheEntryCodecTest, RoundTrip) {
  GoogleString key32(32, '\x09');
  GoogleString blob =
      SerializeCacheEntry(KeyLookupResult::kFound, key32, 1000, 1300);
  KeyLookupResult::Status st;
  GoogleString k;
  int64_t fetched = 0, expires = 0;
  ASSERT_TRUE(DeserializeCacheEntry(blob, &st, &k, &fetched, &expires));
  EXPECT_EQ(KeyLookupResult::kFound, st);
  EXPECT_EQ(key32, k);
  EXPECT_EQ(1000, fetched);
  EXPECT_EQ(1300, expires);
}

// VerdictToken stability (used by the nginx variable surface).
TEST(ClassifierTest, VerdictTokens) {
  EXPECT_STREQ("human", VerdictToken(Verdict::kHuman));
  EXPECT_STREQ("signed-agent", VerdictToken(Verdict::kSignedAgent));
  EXPECT_STREQ("verified-bot", VerdictToken(Verdict::kVerifiedBot));
  EXPECT_STREQ("unknown", VerdictToken(Verdict::kUnknown));
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb
