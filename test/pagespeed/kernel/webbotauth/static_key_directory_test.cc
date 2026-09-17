// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Hermetic unit test for StaticKeyDirectory: in-memory JWKS lookup, no network,
// no filesystem. A 43-char base64url string of 'A's decodes to 32 zero bytes,
// which is a valid OKP/Ed25519 `x` value (avoids needing an encoder here).

#include "pagespeed/kernel/webbotauth/static_key_directory.h"

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/base64.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"

namespace net_instaweb {
namespace webbotauth {
namespace {

// base64url (no pad) of 32 zero bytes: 43 'A' characters.
const char kX[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

GoogleString MakeDoc(StringPiece kid, StringPiece x) {
  return StrCat("{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"",
                kid, "\",\"x\":\"", x, "\"}]}");
}

TEST(StaticKeyDirectoryTest, FindsConfiguredKid) {
  GoogleString expected;
  ASSERT_TRUE(Base64UrlDecode(kX, &expected));
  ASSERT_EQ(32u, expected.size());

  StaticKeyDirectory dir(MakeDoc("k1", kX));
  KeyLookupResult r = dir.GetKey("any.host", "k1", 0);
  EXPECT_EQ(KeyLookupResult::kFound, r.status);
  EXPECT_EQ(expected, r.raw_key_32);
}

TEST(StaticKeyDirectoryTest, UnknownKidIsNotFound) {
  StaticKeyDirectory dir(MakeDoc("k1", kX));
  KeyLookupResult r = dir.GetKey("any.host", "nope", 0);
  EXPECT_EQ(KeyLookupResult::kNotFound, r.status);
}

TEST(StaticKeyDirectoryTest, EmptyKidIsNotFound) {
  StaticKeyDirectory dir(MakeDoc("k1", kX));
  EXPECT_EQ(KeyLookupResult::kNotFound, dir.GetKey("h", "", 0).status);
}

TEST(StaticKeyDirectoryTest, MalformedDocumentIsNotFound) {
  StaticKeyDirectory dir("this is not json");
  EXPECT_EQ(KeyLookupResult::kNotFound, dir.GetKey("h", "k1", 0).status);
}

TEST(StaticKeyDirectoryTest, EmptyDocumentIsNotFound) {
  StaticKeyDirectory dir("");
  EXPECT_TRUE(dir.empty());
  EXPECT_EQ(KeyLookupResult::kNotFound, dir.GetKey("h", "k1", 0).status);
}

// A JWKS *array* whose entry omits "kid".
GoogleString MakeKidlessArrayDoc(StringPiece x) {
  return StrCat("{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", x,
                "\"}]}");
}

// A single top-level JWK (shape 2) that omits "kid".
GoogleString MakeKidlessSingleDoc(StringPiece x) {
  return StrCat("{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", x, "\"}");
}

// A kid-less key inside a JWKS *array* must NOT wildcard-match an arbitrary
// requested keyid. Accepting it lets the holder of a low-trust kid-less key
// impersonate any registered (e.g. verified-bot) keyid -- a key-directory
// fail-open the lookup is supposed to prevent. (bug 274-h1)
TEST(StaticKeyDirectoryTest, KidlessKeyInArrayDoesNotMatchArbitraryKid) {
  StaticKeyDirectory dir(MakeKidlessArrayDoc(kX));
  EXPECT_EQ(KeyLookupResult::kNotFound,
            dir.GetKey("any.host", "some-unknown-kid", 0).status);
}

// A kid-less entry alongside a real keyed entry in an array must not shadow the
// keyed lookup: an unknown keyid stays kNotFound, and the keyed lookup resolves.
TEST(StaticKeyDirectoryTest, KidlessEntryDoesNotShadowKeyedLookupInArray) {
  GoogleString doc =
      StrCat("{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", kX,
             "\"},{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\",\"x\":\"",
             kX, "\"}]}");
  StaticKeyDirectory dir(doc);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            dir.GetKey("any.host", "unknown", 0).status);
  EXPECT_EQ(KeyLookupResult::kFound, dir.GetKey("any.host", "k1", 0).status);
}

// The documented single-JWK convenience is preserved: a lone top-level JWK that
// omits "kid" still resolves (single key, host already allowlisted). This is a
// regression guard -- it must stay green after the array-shape fix.
TEST(StaticKeyDirectoryTest, KidlessSingleJwkStillResolves) {
  GoogleString expected;
  ASSERT_TRUE(Base64UrlDecode(kX, &expected));
  StaticKeyDirectory dir(MakeKidlessSingleDoc(kX));
  KeyLookupResult r = dir.GetKey("any.host", "whatever-kid", 0);
  EXPECT_EQ(KeyLookupResult::kFound, r.status);
  EXPECT_EQ(expected, r.raw_key_32);
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb
