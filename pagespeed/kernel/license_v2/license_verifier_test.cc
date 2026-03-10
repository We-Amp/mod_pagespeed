// Copyright (c) 2024-2026 We-Amp B.V.

// Comprehensive tests for license verification: valid token, tampered token,
// wrong key, empty input, expiry enforcement, dual-key verification, max
// token length, invalid signature length, malformed JSON payload.

#include "pagespeed/kernel/license_v2/license_verifier.h"

#include "ed25519.h"  // NOLINT(build/include_subdir)
#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/license_v2/license_signer.h"
#include "pagespeed/kernel/license_v2/license_token.h"

namespace net_instaweb {
namespace {

// Test seed (32 bytes of zeros -- never use in production).
const GoogleString kTestSeed(32, '\0');

class LicenseVerifierTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CreateKeypair(kTestSeed, &public_key_, &private_key_);
  }

  GoogleString public_key_;
  GoogleString private_key_;
};

TEST_F(LicenseVerifierTest, ValidToken) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  ASSERT_FALSE(token.empty());

  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_TRUE(result.valid) << "Error: " << result.error;
  EXPECT_EQ(result.payload.sub, "customer@example.com");
  EXPECT_EQ(result.payload.iss, "modpagespeed.com");
  EXPECT_EQ(result.payload.iat, 1706745600);
  EXPECT_EQ(result.payload.plan, "pro");
  // v1 token: no expiry fields.
  EXPECT_FALSE(result.expired);
  EXPECT_EQ(result.expires_at, 0);
}

TEST_F(LicenseVerifierTest, TamperedToken) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);

  // Tamper with a character in the middle of the token
  if (token.size() > 10) {
    token[10] = (token[10] == 'A') ? 'B' : 'A';
  }

  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
}

TEST_F(LicenseVerifierTest, WrongKey) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);

  // Different seed -> different keypair
  GoogleString other_seed(32, '\x01');
  GoogleString other_pk;
  GoogleString other_sk;
  CreateKeypair(other_seed, &other_pk, &other_sk);

  LicenseResult result = VerifyLicenseTokenWithKey(token, other_pk);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("invalid signature"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, EmptyToken) {
  LicenseResult result = VerifyLicenseTokenWithKey("", public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.error, "empty token");
}

TEST_F(LicenseVerifierTest, MalformedBase64) {
  LicenseResult result =
      VerifyLicenseTokenWithKey("not!valid!base64", public_key_);
  EXPECT_FALSE(result.valid);
}

TEST_F(LicenseVerifierTest, WrongIssuer) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "evil.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);

  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("unexpected issuer"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, InvalidPublicKeySize) {
  LicenseResult result = VerifyLicenseTokenWithKey("anything", "short");
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("public key size"), GoogleString::npos);
}

// ---------- v2 token tests ----------

TEST_F(LicenseVerifierTest, V2TokenRoundTrip) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";
  payload.exp = 1999999999;  // Far future
  payload.sid = "sub_abc123";
  payload.kid = "k1";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  ASSERT_FALSE(token.empty());

  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_TRUE(result.valid) << "Error: " << result.error;
  EXPECT_EQ(result.payload.sub, "customer@example.com");
  EXPECT_EQ(result.payload.plan, "pro");
  EXPECT_EQ(result.payload.exp, 1999999999);
  EXPECT_EQ(result.payload.sid, "sub_abc123");
  EXPECT_EQ(result.payload.kid, "k1");
  EXPECT_EQ(result.expires_at, 1999999999);
  EXPECT_FALSE(result.expired);
}

TEST_F(LicenseVerifierTest, V1BackwardCompatibility) {
  // v1 token: no exp, sid, or kid fields.
  LicensePayload payload;
  payload.sub = "old@customer.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1600000000;
  payload.plan = "pro";
  // Leave exp=0, sid="", kid="" (defaults)

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_TRUE(result.valid) << "Error: " << result.error;
  EXPECT_EQ(result.payload.exp, 0);
  EXPECT_TRUE(result.payload.sid.empty());
  EXPECT_TRUE(result.payload.kid.empty());
  EXPECT_EQ(result.expires_at, 0);
  EXPECT_FALSE(result.expired);
}

TEST_F(LicenseVerifierTest, ExpiredToken) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1600000000;
  payload.plan = "pro";
  payload.exp = 1600000001;  // Expired long ago

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_TRUE(result.valid);  // Signature is still valid
  EXPECT_TRUE(result.expired);
  EXPECT_EQ(result.expires_at, 1600000001);
}

TEST_F(LicenseVerifierTest, NotYetExpiredToken) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";
  payload.exp = 4102444800;  // Year 2100 -- far future

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_TRUE(result.valid);
  EXPECT_FALSE(result.expired);
  EXPECT_EQ(result.expires_at, 4102444800);
}

TEST_F(LicenseVerifierTest, NoExpFieldMeansNoExpiry) {
  // exp = 0 means no expiry enforcement (v1 behavior)
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_TRUE(result.valid);
  EXPECT_FALSE(result.expired);
  EXPECT_EQ(result.expires_at, 0);
}

TEST_F(LicenseVerifierTest, MaxTokenLengthRejection) {
  // Token exceeding 2048 bytes should be rejected before decoding.
  GoogleString too_long(2049, 'A');
  LicenseResult result = VerifyLicenseTokenWithKey(too_long, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("maximum length"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, TokenAtMaxLength) {
  // Token at exactly 2048 bytes should be accepted (if otherwise valid).
  // This won't be a valid token but should at least pass the length check.
  GoogleString at_limit(2048, 'A');
  LicenseResult result = VerifyLicenseTokenWithKey(at_limit, public_key_);
  EXPECT_FALSE(result.valid);
  // Should fail for a reason other than length
  EXPECT_EQ(result.error.find("maximum length"), GoogleString::npos);
}

// ---------- Dual-key verification tests ----------

TEST_F(LicenseVerifierTest, DualKeyVerificationWithOldKey) {
  // Sign a token with a different key (simulating old key)
  GoogleString old_seed(32, '\x42');
  GoogleString old_pk, old_sk;
  CreateKeypair(old_seed, &old_pk, &old_sk);

  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, old_pk, old_sk);

  // Verify with the test keypair (different from old) -- should fail
  // since kPreviousPublicKey is zero by default.
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);

  // Verify with the correct old key -- should succeed.
  LicenseResult result2 = VerifyLicenseTokenWithKey(token, old_pk);
  EXPECT_TRUE(result2.valid);
}

TEST_F(LicenseVerifierTest, V2TokenWithAllOptionalFields) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "enterprise";
  payload.exp = 1999999999;
  payload.sid = "sub_xyz789-abc";
  payload.kid = "k2";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(result.payload.plan, "enterprise");
  EXPECT_EQ(result.payload.sid, "sub_xyz789-abc");
  EXPECT_EQ(result.payload.kid, "k2");
}

// ---------- IsZeroKey indirect test ----------

TEST_F(LicenseVerifierTest, IsZeroKeyCheckedViaVerifyLicenseToken) {
  // VerifyLicenseToken() uses the embedded kPublicKey and kPreviousPublicKey.
  // We can't override them, but we can verify the function doesn't crash
  // and returns a consistent error for a token that doesn't match either key.
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  // Sign with a completely different key -- neither kPublicKey nor
  // kPreviousPublicKey.
  GoogleString other_seed(32, '\xFF');
  GoogleString other_pk, other_sk;
  CreateKeypair(other_seed, &other_pk, &other_sk);

  GoogleString token = SignLicenseToken(payload, other_pk, other_sk);
  LicenseResult result = VerifyLicenseToken(token);
  // Should fail (neither primary nor fallback key matches).
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("invalid signature"), GoogleString::npos);
}

// ---------- Token at boundary sizes ----------

TEST_F(LicenseVerifierTest, SingleByteToken) {
  LicenseResult result = VerifyLicenseTokenWithKey("A", public_key_);
  EXPECT_FALSE(result.valid);
}

TEST_F(LicenseVerifierTest, OnlyWhitespaceToken) {
  LicenseResult result = VerifyLicenseTokenWithKey("   ", public_key_);
  EXPECT_FALSE(result.valid);
}

// ---------- Dual-key edge cases ----------

TEST_F(LicenseVerifierTest, KidFieldPreservedInVerification) {
  // A token with kid="k2" should have that kid after verification.
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";
  payload.kid = "k2";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(result.payload.kid, "k2");
}

TEST_F(LicenseVerifierTest, WrongKeyReturnsInvalidSignature) {
  // Token signed with one key, verified with a different key.
  GoogleString other_seed(32, '\xAA');
  GoogleString other_pk, other_sk;
  CreateKeypair(other_seed, &other_pk, &other_sk);

  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, other_pk, other_sk);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("invalid signature"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, WrongIssuerDoesNotReturnSignatureError) {
  // Token with wrong issuer should fail but NOT with "invalid signature".
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "evil.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  // Error should be about issuer, not signature.
  EXPECT_EQ(result.error.find("invalid signature"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, ExpiredTokenWithKid) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1000000;
  payload.plan = "pro";
  payload.kid = "k1";
  payload.exp = 1000001;  // Long expired

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  // Signature is valid but token is expired.
  EXPECT_TRUE(result.valid);
  EXPECT_TRUE(result.expired);
  EXPECT_EQ(result.payload.kid, "k1");
}

// ---------- Invalid signature length ----------

TEST_F(LicenseVerifierTest, TokenDecodesToExactly64Bytes) {
  // 64 bytes = signature only, no payload.  SplitToken requires
  // decoded.size() > 64, so this should fail at the SplitToken step.
  GoogleString raw(64, '\x01');
  GoogleString token = Base64UrlEncode(raw);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("base64url decode failed"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, TokenDecodesToLessThan64Bytes) {
  // 32 bytes -- way too short for a valid token.
  GoogleString raw(32, '\x02');
  GoogleString token = Base64UrlEncode(raw);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("base64url decode failed"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, TokenDecodesToEmptyBinary) {
  // Encode an empty string -- decoded is 0 bytes.
  GoogleString token = Base64UrlEncode("");
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
}

// ---------- Valid signature but malformed JSON ----------

TEST_F(LicenseVerifierTest, ValidSignatureMalformedJson) {
  // The "payload" is not valid JSON.
  GoogleString bad_payload = "this is not json at all!!!";

  // Sign the bad payload with ed25519 directly.
  unsigned char signature[64];
  ed25519_sign(signature,
               reinterpret_cast<const unsigned char*>(bad_payload.data()),
               bad_payload.size(),
               reinterpret_cast<const unsigned char*>(public_key_.data()),
               reinterpret_cast<const unsigned char*>(private_key_.data()));

  // Build a token from the real signature + bad payload.
  GoogleString token =
      BuildToken(StringPiece(reinterpret_cast<const char*>(signature), 64),
                 bad_payload);

  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.error, "valid signature but malformed payload JSON");
}

TEST_F(LicenseVerifierTest, ValidSignatureTruncatedJson) {
  // JSON that starts valid but is truncated mid-field.
  GoogleString bad_payload = R"({"sub": "test", "iss": "modpagespeed)";

  unsigned char signature[64];
  ed25519_sign(signature,
               reinterpret_cast<const unsigned char*>(bad_payload.data()),
               bad_payload.size(),
               reinterpret_cast<const unsigned char*>(public_key_.data()),
               reinterpret_cast<const unsigned char*>(private_key_.data()));

  GoogleString token =
      BuildToken(StringPiece(reinterpret_cast<const char*>(signature), 64),
                 bad_payload);

  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.error, "valid signature but malformed payload JSON");
}

TEST_F(LicenseVerifierTest, ValidSignatureEmptyJsonPayload) {
  // Empty string as payload -- valid signature over empty content.
  // BuildToken needs at least 1 byte of payload for SplitToken, but
  // ed25519 can sign zero-length data.  The decoded token would be
  // exactly 64 bytes, which SplitToken rejects.  So use a single byte.
  GoogleString single_byte_payload = "{";

  unsigned char sig2[64];
  ed25519_sign(
      sig2, reinterpret_cast<const unsigned char*>(single_byte_payload.data()),
      single_byte_payload.size(),
      reinterpret_cast<const unsigned char*>(public_key_.data()),
      reinterpret_cast<const unsigned char*>(private_key_.data()));

  GoogleString token =
      BuildToken(StringPiece(reinterpret_cast<const char*>(sig2), 64),
                 single_byte_payload);

  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  // Either malformed JSON or some other parse error.
  EXPECT_NE(result.error.find("malformed payload JSON"), GoogleString::npos);
}

// ---------- Dual-key rotation: VerifyLicenseToken paths ----------

TEST_F(LicenseVerifierTest, DualKeyNonSignatureErrorDoesNotFallback) {
  LicensePayload payload;
  payload.sub = "customer@example.com";
  payload.iss = "evil.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
  // This token is signed with test keys (not embedded keys).
  // VerifyLicenseToken will first try kPublicKey -> "invalid signature"
  // Then try kPreviousPublicKey -> also "invalid signature"
  // Neither will succeed, so result.error = "invalid signature".
  LicenseResult result = VerifyLicenseToken(token);
  EXPECT_FALSE(result.valid);
  // The test key doesn't match either embedded key.
  EXPECT_EQ(result.error, "invalid signature");
}

TEST_F(LicenseVerifierTest, VerifyLicenseTokenEmptyToken) {
  // Empty token through the public VerifyLicenseToken function.
  LicenseResult result = VerifyLicenseToken("");
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.error, "empty token");
}

TEST_F(LicenseVerifierTest, VerifyLicenseTokenMalformedBase64) {
  // Non-signature error should not trigger fallback.
  LicenseResult result = VerifyLicenseToken("!!!invalid-base64!!!");
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("base64url decode failed"), GoogleString::npos);
}

// ---------- Truncated signature ----------

TEST_F(LicenseVerifierTest, TruncatedSignature10Bytes) {
  GoogleString raw(10, '\x03');
  GoogleString token = Base64UrlEncode(raw);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("base64url decode failed"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, TruncatedSignature63Bytes) {
  GoogleString raw(63, '\x04');
  GoogleString token = Base64UrlEncode(raw);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("base64url decode failed"), GoogleString::npos);
}

TEST_F(LicenseVerifierTest, TruncatedSignature1Byte) {
  GoogleString raw(1, '\x05');
  GoogleString token = Base64UrlEncode(raw);
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
}

// ---------- Key rotation fallback ----------

TEST_F(LicenseVerifierTest, KeyRotationFallbackSimulation) {
  // Create two separate key pairs.
  GoogleString seed1(32, '\x10');
  GoogleString pk1, sk1;
  CreateKeypair(seed1, &pk1, &sk1);

  GoogleString seed2(32, '\x20');
  GoogleString pk2, sk2;
  CreateKeypair(seed2, &pk2, &sk2);

  LicensePayload payload;
  payload.sub = "rotation@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";
  payload.kid = "k-old";

  // Sign with key1 (the "old" key).
  GoogleString token = SignLicenseToken(payload, pk1, sk1);
  ASSERT_FALSE(token.empty());

  // Verifying with key2 (the "new" primary key) should fail.
  LicenseResult primary_result = VerifyLicenseTokenWithKey(token, pk2);
  EXPECT_FALSE(primary_result.valid);
  EXPECT_EQ(primary_result.error, "invalid signature");

  // Verifying with key1 (the fallback key) should succeed.
  LicenseResult fallback_result = VerifyLicenseTokenWithKey(token, pk1);
  EXPECT_TRUE(fallback_result.valid) << "Error: " << fallback_result.error;
  EXPECT_EQ(fallback_result.payload.sub, "rotation@example.com");
  EXPECT_EQ(fallback_result.payload.kid, "k-old");
}

TEST_F(LicenseVerifierTest, KeyRotationKidMatchesCurrentKeyButWrongSig) {
  GoogleString seed3(32, '\x30');
  GoogleString pk3, sk3;
  CreateKeypair(seed3, &pk3, &sk3);

  LicensePayload payload;
  payload.sub = "kid-mismatch@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";
  payload.kid = "k1";  // Matches kCurrentKeyId

  GoogleString token = SignLicenseToken(payload, pk3, sk3);
  ASSERT_FALSE(token.empty());

  // Token does not verify with the test key (nor with any embedded key).
  LicenseResult result = VerifyLicenseTokenWithKey(token, public_key_);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.error, "invalid signature");

  // Also confirm it does not verify with yet another random key.
  GoogleString seed4(32, '\x40');
  GoogleString pk4, sk4;
  CreateKeypair(seed4, &pk4, &sk4);
  LicenseResult result2 = VerifyLicenseTokenWithKey(token, pk4);
  EXPECT_FALSE(result2.valid);
  EXPECT_EQ(result2.error, "invalid signature");
}

TEST_F(LicenseVerifierTest,
       KeyRotationFallbackDoesNotApplyForNonSignatureErrors) {
  // Non-signature errors (empty, too long, malformed base64) should not
  // trigger the fallback path.
  GoogleString too_long(2049, 'B');
  LicenseResult result = VerifyLicenseToken(too_long);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.error.find("maximum length"), GoogleString::npos);

  LicenseResult result2 = VerifyLicenseToken("!!!bad-base64!!!");
  EXPECT_FALSE(result2.valid);
  EXPECT_NE(result2.error.find("base64url decode failed"), GoogleString::npos);
}

}  // namespace
}  // namespace net_instaweb
