// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// test/pagespeed/kernel/webbotauth/rsl_cap_validator_test.cc @
// f6c24e8d34ba0c58acb22d626f6213d666d73e41 (2026-07-04; AgentPass A3).
// Assertions kept VERBATIM; only the string/type idioms are mechanically
// substituted (GoogleString->std::string, StringVector->std::vector<std::string>,
// StrCat->absl::StrCat, net_instaweb->pagespeed).
//
// Hermetic, offline unit test for the RSL-CAP capability-token validator. Mints
// its own Ed25519 keypairs with @ed25519 (ed25519_create_keypair / ed25519_sign),
// hand-assembles RSL-CAP tokens, and injects a FAKE KeyDirectoryProvider (the
// same abstraction the observe-only verifier uses) -- so it touches NO network,
// NO front-end, and NO cache. Mirrors the style of verifier_test.cc.

#include "src/crypto/webbotauth/rsl_cap_validator.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "ed25519.h"  // NOLINT(build/include_subdir)
#include "gtest/gtest.h"
#include "src/crypto/webbotauth/base64.h"
#include "src/crypto/webbotauth/key_directory.h"
#include "src/crypto/webbotauth/rsl_cap_token.h"

namespace pagespeed {
namespace webbotauth {
namespace {

const char kDirHost[] = "directory.example";  // operator-mapped issuer host
const int64_t kNow = 1700000000;              // fixed wall clock (unix seconds)

// A fake KeyDirectoryProvider mapping (host, keyid) -> raw public key, modeling
// a static trusted-key set. Mirrors verifier_test.cc's FakeKeyDirectoryProvider.
// Records call_count so tests can assert no superfluous lookups.
class FakeKeyDirectoryProvider : public KeyDirectoryProvider {
 public:
  void Add(std::string_view host, std::string_view keyid,
           std::string_view raw_key_32) {
    keys_[Key(host, keyid)] = std::string(raw_key_32);
  }

  KeyLookupResult GetKey(std::string_view host, std::string_view keyid,
                         int64_t /*now*/) override {
    ++call_count_;
    auto it = keys_.find(Key(host, keyid));
    if (it == keys_.end()) {
      return KeyLookupResult::NotFound();
    }
    return KeyLookupResult::Found(it->second);
  }

  int call_count() const { return call_count_; }

 private:
  static std::string Key(std::string_view host, std::string_view keyid) {
    return absl::StrCat(host, "\x1f", keyid);
  }
  std::map<std::string, std::string> keys_;
  int call_count_ = 0;
};

// A provider that returns kError (off-allowlist / SSRF-refused) for any
// "forbidden" host and FAILS the test if it would ever have to perform a fetch.
// This models the real net-fetch directory: an off-allowlist issuer returns
// kError WITHOUT a network fetch. The invariant under test is that an
// off-allowlist host yields kUnknownIssuer and never triggers
// signature/expiry/authorize on unverifiable data, and that the request can
// never cause a network call to an off-allowlist host.
class OffAllowlistProvider : public KeyDirectoryProvider {
 public:
  void AllowStatic(std::string_view host, std::string_view keyid,
                   std::string_view key) {
    statics_[absl::StrCat(host, "\x1f", keyid)] = std::string(key);
  }
  void MarkForbiddenHost(std::string_view host) {
    forbidden_hosts_.insert(std::string(host));
  }

  KeyLookupResult GetKey(std::string_view host, std::string_view keyid,
                         int64_t /*now*/) override {
    ++call_count_;
    if (forbidden_hosts_.count(std::string(host)) != 0) {
      // Off-allowlist host: the real provider returns kError WITHOUT any network
      // fetch (it short-circuits on the empty/non-matching allowlist). We do the
      // same and record that we were consulted exactly once.
      ++forbidden_lookups_;
      return KeyLookupResult::Error();
    }
    auto it = statics_.find(absl::StrCat(host, "\x1f", keyid));
    if (it == statics_.end()) {
      return KeyLookupResult::NotFound();
    }
    return KeyLookupResult::Found(it->second);
  }

  int call_count() const { return call_count_; }
  int forbidden_lookups() const { return forbidden_lookups_; }

 private:
  std::map<std::string, std::string> statics_;
  std::set<std::string> forbidden_hosts_;
  int call_count_ = 0;
  int forbidden_lookups_ = 0;
};

// Mints a real Ed25519 keypair (mirrors verifier_test.cc's SetUp).
void MintKeypair(std::string* pub, unsigned char public_key[32],
                 unsigned char private_key[64], int salt) {
  unsigned char seed[32] = {0};
  for (int i = 0; i < 32; ++i) {
    seed[i] = static_cast<unsigned char>(i + 1 + salt);
  }
  ed25519_create_keypair(public_key, private_key, seed);
  pub->assign(reinterpret_cast<const char*>(public_key), 32);
}

// Standard base64 encoder (with padding) -> base64url (no padding). The library
// only ships a DECODER (base64.h), so the test encodes locally, exactly like
// verifier_test.cc's Base64Encode / Base64UrlEncodeNoPad helpers.
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

// --- JSON assembly (hand-built; the engine JSON writer is not a dep here) ---

std::string JsonObject(const std::string& body) {
  return absl::StrCat("{", body, "}");
}
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

// Mints a full RSL-CAP token. `public_key`/`private_key` are the 32/64-byte
// Ed25519 keys. If `tamper_payload_byte` >= 0, re-encodes a tampered payload
// AFTER signing so the (unchanged) signature no longer matches.
std::string MintToken(const unsigned char public_key[32],
                      const unsigned char private_key[64], std::string_view kid,
                      std::string_view iss, std::string_view sub, int64_t exp,
                      const std::vector<std::string>& lic,
                      const std::vector<std::string>& scope,
                      std::string_view alg = "EdDSA",
                      std::string_view typ = "RSL-CAP",
                      int tamper_payload_byte = -1) {
  const std::string header_json = JsonObject(absl::StrCat(
      JsonStr("alg", alg), ",", JsonStr("typ", typ), ",", JsonStr("kid", kid)));
  std::string payload_body =
      absl::StrCat(JsonStr("iss", iss), ",", JsonStr("sub", sub), ",",
                   JsonInt("exp", exp), ",", JsonInt("iat", exp - 3600));
  absl::StrAppend(&payload_body, ",", JsonStrArray("lic", lic));
  absl::StrAppend(&payload_body, ",", JsonStrArray("scope", scope));
  const std::string payload_json = JsonObject(payload_body);

  const std::string header_b64 = Base64UrlEncodeNoPad(header_json);
  const std::string payload_b64 = Base64UrlEncodeNoPad(payload_json);
  const std::string signing_input = absl::StrCat(header_b64, ".", payload_b64);

  // ed25519_sign(signature, message, message_len, public_key, private_key).
  unsigned char sig[64];
  ed25519_sign(sig,
               reinterpret_cast<const unsigned char*>(signing_input.data()),
               signing_input.size(), public_key, private_key);
  const std::string sig_bytes(reinterpret_cast<const char*>(sig), 64);
  const std::string sig_b64 = Base64UrlEncodeNoPad(sig_bytes);

  if (tamper_payload_byte >= 0) {
    std::string tampered = payload_json;
    if (tamper_payload_byte < static_cast<int>(tampered.size())) {
      tampered[tamper_payload_byte] ^= 0x20;  // flip a bit
    }
    const std::string tampered_b64 = Base64UrlEncodeNoPad(tampered);
    return absl::StrCat(header_b64, ".", tampered_b64, ".", sig_b64);
  }
  return absl::StrCat(header_b64, ".", payload_b64, ".", sig_b64);
}

// Fixture: mints a primary keypair once.
class RslCapValidatorTest : public ::testing::Test {
 protected:
  void SetUp() override { MintKeypair(&pub_, public_key_, private_key_, 0); }

  unsigned char public_key_[32];
  unsigned char private_key_[64];
  std::string pub_;
};

// (a) valid + authorized -> kAuthorized (allow).
TEST_F(RslCapValidatorTest, ValidAuthorized) {
  FakeKeyDirectoryProvider dir;
  dir.Add(kDirHost, "kid-1", pub_);

  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                "agent-007", kNow + 3600, /*lic=*/{"premium"},
                /*scope=*/{"render"});

  RslCapValidator validator(&dir);
  EXPECT_EQ(RslCapStatus::kAuthorized,
            validator.Validate(absl::StrCat("License ", token), "premium",
                               "render", kDirHost, kNow));
}

// (b) no token -> kNoToken (401).
TEST_F(RslCapValidatorTest, NoToken) {
  FakeKeyDirectoryProvider dir;
  RslCapValidator validator(&dir);
  EXPECT_EQ(RslCapStatus::kNoToken,
            validator.Validate("", "premium", "render", kDirHost, kNow));
  EXPECT_EQ(
      RslCapStatus::kNoToken,
      validator.Validate("Bearer xyz", "premium", "render", kDirHost, kNow));
  EXPECT_EQ(
      RslCapStatus::kNoToken,
      validator.Validate("License ", "premium", "render", kDirHost, kNow));
  // The provider must not have been consulted for a no-token request.
  EXPECT_EQ(0, dir.call_count());
}

// (c) valid identity, requested license/scope NOT granted -> kUnlicensed (402).
TEST_F(RslCapValidatorTest, ValidIdentityUnlicensed) {
  FakeKeyDirectoryProvider dir;
  dir.Add(kDirHost, "kid-1", pub_);

  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                "agent-007", kNow + 3600, /*lic=*/{"basic"},
                /*scope=*/{"crawl"});

  RslCapValidator validator(&dir);
  // Neither license nor scope granted.
  EXPECT_EQ(RslCapStatus::kUnlicensed,
            validator.Validate(absl::StrCat("License ", token), "premium",
                               "render", kDirHost, kNow));
  // License granted but scope not -> still unlicensed.
  EXPECT_EQ(RslCapStatus::kUnlicensed,
            validator.Validate(absl::StrCat("License ", token), "basic",
                               "render", kDirHost, kNow));
}

// (d1) tampered byte -> kBadSignature (401). Proves signature is checked BEFORE
// expiry/authorization.
TEST_F(RslCapValidatorTest, TamperedPayload) {
  FakeKeyDirectoryProvider dir;
  dir.Add(kDirHost, "kid-1", pub_);

  // Tamper a byte well inside the payload JSON.
  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                "agent-007", kNow + 3600, /*lic=*/{"premium"},
                /*scope=*/{"render"}, "EdDSA", "RSL-CAP",
                /*tamper_payload_byte=*/10);

  RslCapValidator validator(&dir);
  EXPECT_EQ(RslCapStatus::kBadSignature,
            validator.Validate(absl::StrCat("License ", token), "premium",
                               "render", kDirHost, kNow));
}

// (d2) expired -> kExpired (401).
TEST_F(RslCapValidatorTest, Expired) {
  FakeKeyDirectoryProvider dir;
  dir.Add(kDirHost, "kid-1", pub_);

  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                "agent-007", kNow - 1, /*lic=*/{"premium"},
                /*scope=*/{"render"});

  RslCapValidator validator(&dir);
  EXPECT_EQ(RslCapStatus::kExpired,
            validator.Validate(absl::StrCat("License ", token), "premium",
                               "render", kDirHost, kNow));
}

// (e) introspection off-allowlist -> kUnknownIssuer (401), NO fetch. Asserts the
// SSRF guard + plane-split: an off-allowlist host yields unknown-issuer and the
// request can never cause a network call to an off-allowlist host.
TEST_F(RslCapValidatorTest, IntrospectionOffAllowlistNoFetch) {
  OffAllowlistProvider dir;
  dir.MarkForbiddenHost("evil.example");  // off-allowlist => kError, no fetch

  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "evil.example", "agent-007",
                kNow + 3600, /*lic=*/{"premium"}, /*scope=*/{"render"});

  RslCapValidator validator(&dir);
  EXPECT_EQ(
      RslCapStatus::kUnknownIssuer,
      validator.Validate(absl::StrCat("License ", token), "premium", "render",
                         /*directory_host=*/"evil.example", kNow));
  // Resolution was attempted exactly once, returned kError WITHOUT a network
  // fetch, and no crypto/expiry/authorize ran on the unverifiable issuer.
  EXPECT_EQ(1, dir.forbidden_lookups());
  EXPECT_EQ(1, dir.call_count());
}

// (f) alg-confusion -> kMalformed, rejected before any verify.
TEST_F(RslCapValidatorTest, AlgConfusionRejected) {
  FakeKeyDirectoryProvider dir;
  dir.Add(kDirHost, "kid-1", pub_);
  RslCapValidator validator(&dir);

  for (const char* bad_alg : {"none", "HS256", "RS256", "ES256", ""}) {
    const std::string token =
        MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                  "agent-007", kNow + 3600, /*lic=*/{"premium"},
                  /*scope=*/{"render"}, /*alg=*/bad_alg);
    EXPECT_EQ(RslCapStatus::kMalformed,
              validator.Validate(absl::StrCat("License ", token), "premium",
                                 "render", kDirHost, kNow))
        << "alg=" << bad_alg << " should be rejected as malformed";
  }
  // No key lookup should have happened -- rejection is pre-resolution.
  EXPECT_EQ(0, dir.call_count());
}

// (f') wrong typ -> kMalformed.
TEST_F(RslCapValidatorTest, WrongTypRejected) {
  FakeKeyDirectoryProvider dir;
  dir.Add(kDirHost, "kid-1", pub_);

  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                "agent-007", kNow + 3600, /*lic=*/{"premium"},
                /*scope=*/{"render"}, "EdDSA", /*typ=*/"JWT");
  RslCapValidator validator(&dir);
  EXPECT_EQ(RslCapStatus::kMalformed,
            validator.Validate(absl::StrCat("License ", token), "premium",
                               "render", kDirHost, kNow));
}

// (g) malformed: not 3 parts / bad base64url.
TEST_F(RslCapValidatorTest, MalformedShapes) {
  FakeKeyDirectoryProvider dir;
  RslCapValidator validator(&dir);

  // Not 3 parts.
  EXPECT_EQ(RslCapStatus::kMalformed,
            validator.Validate("License aaa.bbb", "premium", "render", kDirHost,
                               kNow));
  EXPECT_EQ(RslCapStatus::kMalformed,
            validator.Validate("License a.b.c.d", "premium", "render", kDirHost,
                               kNow));
  // Bad base64url (contains '+' '/' '=' which Base64UrlDecode rejects).
  EXPECT_EQ(RslCapStatus::kMalformed,
            validator.Validate("License a+b.c/d.e=f", "premium", "render",
                               kDirHost, kNow));
  // 3 parts but decodes to non-JSON.
  EXPECT_EQ(RslCapStatus::kMalformed,
            validator.Validate("License QQ.QQ.QQ", "premium", "render",
                               kDirHost, kNow));
}

// Unknown kid via a static directory -> kUnknownIssuer.
TEST_F(RslCapValidatorTest, UnknownKidStatic) {
  FakeKeyDirectoryProvider dir;  // intentionally empty

  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                "agent-007", kNow + 3600, /*lic=*/{"premium"},
                /*scope=*/{"render"});
  RslCapValidator validator(&dir);
  EXPECT_EQ(RslCapStatus::kUnknownIssuer,
            validator.Validate(absl::StrCat("License ", token), "premium",
                               "render", kDirHost, kNow));
}

// kid resolves to a DIFFERENT pubkey -> kBadSignature.
TEST_F(RslCapValidatorTest, WrongKeyBadSignature) {
  unsigned char other_pub[32], other_priv[64];
  std::string other_pub_str;
  MintKeypair(&other_pub_str, other_pub, other_priv, 100);

  FakeKeyDirectoryProvider dir;
  dir.Add(kDirHost, "kid-1", other_pub_str);  // resolves, wrong key

  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                "agent-007", kNow + 3600, /*lic=*/{"premium"},
                /*scope=*/{"render"});
  RslCapValidator validator(&dir);
  EXPECT_EQ(RslCapStatus::kBadSignature,
            validator.Validate(absl::StrCat("License ", token), "premium",
                               "render", kDirHost, kNow));
}

// Empty grant arrays -> unlicensed for any request.
TEST_F(RslCapValidatorTest, EmptyGrantsUnlicensed) {
  FakeKeyDirectoryProvider dir;
  dir.Add(kDirHost, "kid-1", pub_);

  const std::string token =
      MintToken(public_key_, private_key_, "kid-1", "issuer.example",
                "agent-007", kNow + 3600, /*lic=*/{}, /*scope=*/{});
  RslCapValidator validator(&dir);
  EXPECT_EQ(RslCapStatus::kUnlicensed,
            validator.Validate(absl::StrCat("License ", token), "premium",
                               "render", kDirHost, kNow));
}

// The kernel verdict->HTTP-status mapping (front-end-free). Every RslCapStatus
// value must map: kAuthorized -> 0 (allow), kUnlicensed -> 402, every other
// verdict -> 401. Mirrors the status table the enforcement handler defers to.
TEST(RslCapStatusToHttpStatusTest, RslCapStatusToHttpStatusMapsEveryVerdict) {
  EXPECT_EQ(0, RslCapStatusToHttpStatus(RslCapStatus::kAuthorized));
  EXPECT_EQ(402, RslCapStatusToHttpStatus(RslCapStatus::kUnlicensed));
  EXPECT_EQ(401, RslCapStatusToHttpStatus(RslCapStatus::kNoToken));
  EXPECT_EQ(401, RslCapStatusToHttpStatus(RslCapStatus::kMalformed));
  EXPECT_EQ(401, RslCapStatusToHttpStatus(RslCapStatus::kUnknownIssuer));
  EXPECT_EQ(401, RslCapStatusToHttpStatus(RslCapStatus::kBadSignature));
  EXPECT_EQ(401, RslCapStatusToHttpStatus(RslCapStatus::kExpired));
}

// --- Parser-level direct tests (rsl_cap_token) ---

TEST(RslCapTokenTest, ParsesWellFormed) {
  // Build a token by hand and ensure the parser extracts every field.
  const std::string header =
      "{\"alg\":\"EdDSA\",\"typ\":\"RSL-CAP\",\"kid\":\"k9\"}";
  const std::string payload =
      "{\"iss\":\"iss.example\",\"sub\":\"sub-1\",\"exp\":1700001234,"
      "\"iat\":1700000000,\"lic\":[\"a\",\"b\"],\"scope\":[\"x\"]}";
  const std::string hb = Base64UrlEncodeNoPad(header);
  const std::string pb = Base64UrlEncodeNoPad(payload);
  // Signature segment can be any base64url for a pure-parse test.
  const std::string sb = Base64UrlEncodeNoPad(std::string(64, '\x01'));
  const std::string compact = absl::StrCat(hb, ".", pb, ".", sb);

  RslCapToken tok;
  ASSERT_EQ(RslCapParseStatus::kOk, ParseRslCapToken(compact, &tok));
  EXPECT_TRUE(tok.parsed);
  EXPECT_EQ("RSL-CAP", tok.typ);
  EXPECT_EQ("EdDSA", tok.alg);
  EXPECT_EQ("k9", tok.kid);
  EXPECT_EQ("iss.example", tok.iss);
  EXPECT_EQ("sub-1", tok.sub);
  EXPECT_EQ(1700001234, tok.exp);
  EXPECT_EQ(1700000000, tok.iat);
  ASSERT_EQ(2u, tok.lic.size());
  EXPECT_EQ("a", tok.lic[0]);
  EXPECT_EQ("b", tok.lic[1]);
  ASSERT_EQ(1u, tok.scope.size());
  EXPECT_EQ("x", tok.scope[0]);
  // Signing input is the original received header.payload bytes, byte-exact.
  EXPECT_EQ(absl::StrCat(hb, ".", pb), tok.signing_input);
  EXPECT_EQ(64u, tok.signature_bytes.size());
}

TEST(RslCapTokenTest, RejectsMissingExp) {
  const std::string header =
      "{\"alg\":\"EdDSA\",\"typ\":\"RSL-CAP\",\"kid\":\"k9\"}";
  const std::string payload =
      "{\"iss\":\"iss.example\",\"lic\":[\"a\"],\"scope\":[\"x\"]}";  // no exp
  const std::string compact = absl::StrCat(
      Base64UrlEncodeNoPad(header), ".", Base64UrlEncodeNoPad(payload), ".",
      Base64UrlEncodeNoPad(std::string(64, '\x01')));
  RslCapToken tok;
  EXPECT_EQ(RslCapParseStatus::kMalformed, ParseRslCapToken(compact, &tok));
}

// A string field value that EMBEDS an (escaped) key-like substring must NOT
// confuse the substring-based field extractor: the only "exp" that gates expiry
// is the real top-level key. Here the iss value literally contains the text
// `"exp":1` (escaped), which must be ignored -- the real exp is 1700001234. Pins
// the parser-robustness property the security review called out: a value's
// escaped quotes never form the `"exp"` needle, so FindKeyValueStart skips it.
TEST(RslCapTokenTest, StringValueWithKeyLikeSubstringIsNotConfused) {
  const std::string header =
      "{\"alg\":\"EdDSA\",\"typ\":\"RSL-CAP\",\"kid\":\"k9\"}";
  // Raw payload: {"iss":"evil\"exp\":1","exp":1700001234,"lic":["a"],"scope":["x"]}
  const std::string payload =
      "{\"iss\":\"evil\\\"exp\\\":1\",\"exp\":1700001234,"
      "\"lic\":[\"a\"],\"scope\":[\"x\"]}";
  const std::string compact = absl::StrCat(
      Base64UrlEncodeNoPad(header), ".", Base64UrlEncodeNoPad(payload), ".",
      Base64UrlEncodeNoPad(std::string(64, '\x01')));
  RslCapToken tok;
  ASSERT_EQ(RslCapParseStatus::kOk, ParseRslCapToken(compact, &tok));
  EXPECT_EQ(1700001234, tok.exp);  // the REAL top-level exp, not the embedded 1
  EXPECT_EQ("evil\"exp\":1", tok.iss);  // value decoded with its escapes intact
}

// Duplicate top-level keys resolve to the FIRST occurrence (the substring
// extractor returns the first match, then stops). Pinned so a future hardening
// that rejects duplicates is a conscious, test-updating change. Safe today: the
// Ed25519 signature covers the exact received bytes, alg is hardcoded at verify
// (the later "none" is never dispatched on), and a smaller first exp fails
// closed (treated as expired) rather than open.
TEST(RslCapTokenTest, DuplicateKeysUseFirstOccurrence) {
  const std::string header =
      "{\"alg\":\"EdDSA\",\"alg\":\"none\",\"typ\":\"RSL-CAP\",\"kid\":\"k9\"}";
  const std::string payload =
      "{\"exp\":1,\"exp\":1700001234,\"lic\":[\"a\"],\"scope\":[\"x\"]}";
  const std::string compact = absl::StrCat(
      Base64UrlEncodeNoPad(header), ".", Base64UrlEncodeNoPad(payload), ".",
      Base64UrlEncodeNoPad(std::string(64, '\x01')));
  RslCapToken tok;
  ASSERT_EQ(RslCapParseStatus::kOk, ParseRslCapToken(compact, &tok));
  EXPECT_EQ("EdDSA", tok.alg);  // first alg wins; the later "none" is inert
  EXPECT_EQ(1, tok.exp);        // first exp wins (fail-closed: smaller/expired)
}

}  // namespace
}  // namespace webbotauth
}  // namespace pagespeed
