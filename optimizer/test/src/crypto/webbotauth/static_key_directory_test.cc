// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// test/pagespeed/kernel/webbotauth/static_key_directory_test.cc @ 8e15a19ce
// (2026-07-02). Keep logic in sync manually; the RFC 9421/8941 core is
// standards-frozen.
//
// Hermetic unit test for StaticKeyDirectory: in-memory JWKS lookup, no
// network, no filesystem. A 43-char base64url string of 'A's decodes to 32
// zero bytes, which is a valid OKP/Ed25519 `x` value (avoids needing an
// encoder here).

#include "src/crypto/webbotauth/static_key_directory.h"

#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "src/crypto/webbotauth/base64.h"
#include "src/crypto/webbotauth/key_directory.h"

namespace pagespeed {
namespace webbotauth {
namespace {

// base64url (no pad) of 32 zero bytes: 43 'A' characters.
const char kX[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

std::string MakeDoc(std::string_view kid, std::string_view x) {
  return absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"", kid,
      "\",\"x\":\"", x, "\"}]}");
}

TEST(StaticKeyDirectoryTest, FindsConfiguredKid) {
  std::string expected;
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
std::string MakeKidlessArrayDoc(std::string_view x) {
  return absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", x, "\"}]}");
}

// A single top-level JWK (shape 2) that omits "kid".
std::string MakeKidlessSingleDoc(std::string_view x) {
  return absl::StrCat("{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", x,
                      "\"}");
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

// A kid-less entry alongside a real keyed entry in an array must not shadow
// the keyed lookup: an unknown keyid stays kNotFound, and the keyed lookup
// resolves.
TEST(StaticKeyDirectoryTest, KidlessEntryDoesNotShadowKeyedLookupInArray) {
  std::string doc = absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"", kX,
      "\"},{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\",\"x\":\"", kX,
      "\"}]}");
  StaticKeyDirectory dir(doc);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            dir.GetKey("any.host", "unknown", 0).status);
  EXPECT_EQ(KeyLookupResult::kFound, dir.GetKey("any.host", "k1", 0).status);
}

// The documented single-JWK convenience is preserved: a lone top-level JWK
// that omits "kid" still resolves (single key, host already allowlisted).
// This is a regression guard -- it must stay green after the array-shape fix.
TEST(StaticKeyDirectoryTest, KidlessSingleJwkStillResolves) {
  std::string expected;
  ASSERT_TRUE(Base64UrlDecode(kX, &expected));
  StaticKeyDirectory dir(MakeKidlessSingleDoc(kX));
  KeyLookupResult r = dir.GetKey("any.host", "whatever-kid", 0);
  EXPECT_EQ(KeyLookupResult::kFound, r.status);
  EXPECT_EQ(expected, r.raw_key_32);
}

}  // namespace
}  // namespace webbotauth
}  // namespace pagespeed
