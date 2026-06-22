// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for license token serialization: base64url encode/decode, payload
// serialize/parse, token build/split.

#include "pagespeed/kernel/license_v2/license_token.h"

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace {

TEST(Base64UrlTest, EncodeEmpty) { EXPECT_EQ(Base64UrlEncode(""), ""); }

TEST(Base64UrlTest, EncodeHello) {
  // "Hello" -> "SGVsbG8"
  EXPECT_EQ(Base64UrlEncode("Hello"), "SGVsbG8");
}

TEST(Base64UrlTest, RoundTrip) {
  GoogleString input = "test data with special chars: +/=";
  GoogleString encoded = Base64UrlEncode(input);
  GoogleString decoded;
  ASSERT_TRUE(Base64UrlDecode(encoded, &decoded));
  EXPECT_EQ(decoded, input);
}

TEST(Base64UrlTest, RoundTripBinary) {
  // Binary data with all byte values 0-255
  GoogleString input;
  for (int i = 0; i < 256; ++i) {
    input.push_back(static_cast<char>(i));
  }
  GoogleString encoded = Base64UrlEncode(input);
  GoogleString decoded;
  ASSERT_TRUE(Base64UrlDecode(encoded, &decoded));
  EXPECT_EQ(decoded, input);
}

TEST(Base64UrlTest, DecodeInvalid) {
  GoogleString output;
  // Invalid base64url character
  EXPECT_FALSE(Base64UrlDecode("abc!def", &output));
}

TEST(Base64UrlTest, DecodePadding) {
  // With padding (should still work)
  GoogleString output;
  ASSERT_TRUE(Base64UrlDecode("SGVsbG8=", &output));
  EXPECT_EQ(output, "Hello");
}

TEST(PayloadTest, SerializeAndParse) {
  LicensePayload payload;
  payload.sub = "user@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString json = SerializePayload(payload);
  EXPECT_NE(json.find("\"sub\":\"user@example.com\""), GoogleString::npos);
  EXPECT_NE(json.find("\"iss\":\"modpagespeed.com\""), GoogleString::npos);
  EXPECT_NE(json.find("\"iat\":1706745600"), GoogleString::npos);
  EXPECT_NE(json.find("\"plan\":\"pro\""), GoogleString::npos);

  LicensePayload parsed;
  ASSERT_TRUE(ParsePayload(json, &parsed));
  EXPECT_EQ(parsed.sub, "user@example.com");
  EXPECT_EQ(parsed.iss, "modpagespeed.com");
  EXPECT_EQ(parsed.iat, 1706745600);
  EXPECT_EQ(parsed.plan, "pro");
}

TEST(PayloadTest, SerializeAndParseV2) {
  LicensePayload payload;
  payload.sub = "user@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";
  payload.exp = 1709424000;
  payload.sid = "sub_abc123";
  payload.kid = "k1";

  GoogleString json = SerializePayload(payload);
  EXPECT_NE(json.find("\"exp\":1709424000"), GoogleString::npos);
  EXPECT_NE(json.find("\"sid\":\"sub_abc123\""), GoogleString::npos);
  EXPECT_NE(json.find("\"kid\":\"k1\""), GoogleString::npos);

  LicensePayload parsed;
  ASSERT_TRUE(ParsePayload(json, &parsed));
  EXPECT_EQ(parsed.exp, 1709424000);
  EXPECT_EQ(parsed.sid, "sub_abc123");
  EXPECT_EQ(parsed.kid, "k1");
}

TEST(PayloadTest, V1PayloadOmitsOptionalFields) {
  // v1: exp=0, sid="", kid="" -> these should NOT appear in JSON.
  LicensePayload payload;
  payload.sub = "user@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString json = SerializePayload(payload);
  EXPECT_EQ(json.find("\"exp\""), GoogleString::npos);
  EXPECT_EQ(json.find("\"sid\""), GoogleString::npos);
  EXPECT_EQ(json.find("\"kid\""), GoogleString::npos);
}

TEST(PayloadTest, ParseV1PayloadDefaultsOptionalFields) {
  // Parsing a v1 JSON should leave exp=0, sid="", kid="".
  GoogleString v1_json =
      "{\"sub\":\"u@e.com\",\"iss\":\"modpagespeed.com\","
      "\"iat\":1706745600,\"plan\":\"pro\"}";
  LicensePayload parsed;
  ASSERT_TRUE(ParsePayload(v1_json, &parsed));
  EXPECT_EQ(parsed.exp, 0);
  EXPECT_TRUE(parsed.sid.empty());
  EXPECT_TRUE(parsed.kid.empty());
}

TEST(PayloadTest, ParseMalformed) {
  LicensePayload parsed;
  EXPECT_FALSE(ParsePayload("not json", &parsed));
  EXPECT_FALSE(ParsePayload("{}", &parsed));
  EXPECT_FALSE(ParsePayload("{\"sub\":\"a\"}", &parsed));
}

TEST(TokenTest, BuildAndSplit) {
  // Create a fake 64-byte signature
  GoogleString sig(64, 'S');
  GoogleString payload = "{\"sub\":\"test\"}";

  GoogleString token = BuildToken(sig, payload);
  EXPECT_FALSE(token.empty());

  GoogleString out_sig;
  GoogleString out_payload;
  ASSERT_TRUE(SplitToken(token, &out_sig, &out_payload));
  EXPECT_EQ(out_sig, sig);
  EXPECT_EQ(out_payload, payload);
}

TEST(TokenTest, SplitEmpty) {
  GoogleString sig;
  GoogleString payload;
  EXPECT_FALSE(SplitToken("", &sig, &payload));
}

TEST(TokenTest, SplitTooShort) {
  // Token that decodes to less than 64 bytes
  GoogleString sig;
  GoogleString payload;
  EXPECT_FALSE(SplitToken("AAAA", &sig, &payload));
}

TEST(TokenTest, SplitInvalidBase64) {
  GoogleString sig;
  GoogleString payload;
  EXPECT_FALSE(SplitToken("invalid!!!base64", &sig, &payload));
}

// ---------- Escaped-quote regression tests ----------

TEST(PayloadTest, EscapedQuotesInSubRoundTrip) {
  LicensePayload payload;
  payload.sub = "O'Brien said \"hello\"";
  payload.iss = "modpagespeed.com";
  payload.iat = 1700000000;
  payload.plan = "pro";

  GoogleString json = SerializePayload(payload);
  LicensePayload parsed;
  ASSERT_TRUE(ParsePayload(json, &parsed));
  EXPECT_EQ(parsed.sub, payload.sub);
}

TEST(PayloadTest, EscapedBackslashInSubRoundTrip) {
  LicensePayload payload;
  payload.sub = "path\\to\\file";
  payload.iss = "modpagespeed.com";
  payload.iat = 1700000000;
  payload.plan = "pro";

  GoogleString json = SerializePayload(payload);
  LicensePayload parsed;
  ASSERT_TRUE(ParsePayload(json, &parsed));
  EXPECT_EQ(parsed.sub, payload.sub);
}

// ---------- Edge cases ----------

TEST(Base64UrlTest, SingleByte) {
  GoogleString input(1, '\x42');
  GoogleString encoded = Base64UrlEncode(input);
  GoogleString decoded;
  ASSERT_TRUE(Base64UrlDecode(encoded, &decoded));
  EXPECT_EQ(decoded, input);
}

TEST(Base64UrlTest, TwoBytes) {
  GoogleString input(2, '\xAB');
  GoogleString encoded = Base64UrlEncode(input);
  GoogleString decoded;
  ASSERT_TRUE(Base64UrlDecode(encoded, &decoded));
  EXPECT_EQ(decoded, input);
}

TEST(PayloadTest, SpecialCharsInEmail) {
  LicensePayload payload;
  payload.sub = "user+tag@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";

  GoogleString json = SerializePayload(payload);
  LicensePayload parsed;
  ASSERT_TRUE(ParsePayload(json, &parsed));
  EXPECT_EQ(parsed.sub, "user+tag@example.com");
}

TEST(PayloadTest, EmptyOptionalFieldsNotSerialized) {
  LicensePayload payload;
  payload.sub = "user@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1706745600;
  payload.plan = "pro";
  payload.exp = 0;
  payload.sid = "";
  payload.kid = "";

  GoogleString json = SerializePayload(payload);
  // None of the optional fields should appear.
  EXPECT_EQ(json.find("exp"), GoogleString::npos);
  EXPECT_EQ(json.find("sid"), GoogleString::npos);
  EXPECT_EQ(json.find("kid"), GoogleString::npos);
}

}  // namespace
}  // namespace net_instaweb
