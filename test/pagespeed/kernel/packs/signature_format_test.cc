// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/signature_format.h"

#include <string>

#include "gtest/gtest.h"

namespace pagespeed::packs {
namespace {

const std::string kSig = "l1EnhYBLwTAT9hPlKPPo0IaMFLFp6ZZcZVRUR2Pji9euhgACVosN2cfGmhntfZxuIBXdBsA7M2c43cEDy0UqAw==";
const std::string kLine = "ed25519 example-publisher " + kSig;

TEST(SignatureFormatTest, KeyIds) {
  EXPECT_TRUE(IsValidKeyId("a"));
  EXPECT_TRUE(IsValidKeyId("example-publisher"));
  EXPECT_TRUE(IsValidKeyId(std::string(64, 'a')));
  EXPECT_FALSE(IsValidKeyId(""));
  EXPECT_FALSE(IsValidKeyId(std::string(65, 'a')));
  EXPECT_FALSE(IsValidKeyId("A"));
  EXPECT_FALSE(IsValidKeyId("a_b"));
  EXPECT_FALSE(IsValidKeyId("a b"));
  EXPECT_FALSE(IsValidKeyId("\xc3\xa9"));
}

TEST(SignatureFormatTest, SidecarAccepted) {
  auto with_nl = ParsePackSidecar(kLine + "\n");
  ASSERT_TRUE(with_nl.has_value());
  EXPECT_EQ("example-publisher", with_nl->key_id);
  EXPECT_EQ(kEd25519SignatureBytes, with_nl->signature.size());
  auto without = ParsePackSidecar(kLine);
  ASSERT_TRUE(without.has_value());
  EXPECT_EQ(with_nl->signature, without->signature);
}

TEST(SignatureFormatTest, SidecarRejected) {
  EXPECT_FALSE(ParsePackSidecar(kLine + "\r\n"));
  EXPECT_FALSE(ParsePackSidecar(kLine + "\n\n"));
  EXPECT_FALSE(ParsePackSidecar(" " + kLine));
  EXPECT_FALSE(ParsePackSidecar("ed25519  example-publisher " + kSig));
  EXPECT_FALSE(ParsePackSidecar("ed25519\texample-publisher " + kSig));
  EXPECT_FALSE(ParsePackSidecar("ED25519 example-publisher " + kSig));
  EXPECT_FALSE(ParsePackSidecar("rsa example-publisher " + kSig));
  EXPECT_FALSE(ParsePackSidecar("ed25519 Bad_Id " + kSig));
  EXPECT_FALSE(ParsePackSidecar("ed25519 example-publisher " + kSig + "A"));
  EXPECT_FALSE(ParsePackSidecar(
      "ed25519 example-publisher " + kSig.substr(0, 86) + "A="));  // 87 chars
  EXPECT_FALSE(ParsePackSidecar("ed25519 example-publisher " +
                                kSig.substr(0, 86)));  // no padding
  std::string dash = kSig;
  dash[3] = '-';
  EXPECT_FALSE(ParsePackSidecar("ed25519 example-publisher " + dash));
  EXPECT_FALSE(ParsePackSidecar(kLine + "\n" + kLine + "\n"));
  EXPECT_FALSE(ParsePackSidecar(std::string(kMaxSidecarBytes + 1, 'a')));
  EXPECT_FALSE(ParsePackSidecar(""));
}

TEST(SignatureFormatTest, TrustKeys) {
  const std::string key = "fuRqJ3FnXonzbp8KuLy7yWR8oZU8Yue62CUrstW39Z4=";
  auto decoded = DecodeTrustKey(key);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(kEd25519PublicKeyBytes, decoded->size());
  EXPECT_FALSE(DecodeTrustKey(key.substr(0, 43)));        // no padding
  EXPECT_FALSE(DecodeTrustKey(key + "A"));                // 45
  std::string bad = key;
  bad[5] = '*';
  EXPECT_FALSE(DecodeTrustKey(bad));
  EXPECT_FALSE(DecodeTrustKey(""));
}

TEST(SignatureFormatTest, TrustFindAndFingerprint) {
  PackTrust a;
  a.keys = {TrustKey{"alpha", std::string(32, 'a')},
            TrustKey{"beta", std::string(32, 'b')}};
  ASSERT_NE(nullptr, a.Find("beta"));
  EXPECT_EQ(std::string(32, 'b'), a.Find("beta")->public_key);
  EXPECT_EQ(nullptr, a.Find("gamma"));
  EXPECT_EQ(nullptr, PackTrust().Find("alpha"));

  // Options keep the keys sorted by id, so the same set gives the same text.
  PackTrust b = a;
  EXPECT_EQ(a.Fingerprint(), b.Fingerprint());
  std::string hex_a, hex_b;
  for (int i = 0; i < 32; ++i) {
    hex_a += "61";
    hex_b += "62";
  }
  EXPECT_EQ("0|alpha=" + hex_a + ";beta=" + hex_b + ";", a.Fingerprint());
  b.require_signed = true;
  EXPECT_NE(a.Fingerprint(), b.Fingerprint());
  EXPECT_EQ('1', b.Fingerprint()[0]);
  b.keys[0].public_key = std::string(32, 'c');
  b.require_signed = false;
  EXPECT_NE(a.Fingerprint(), b.Fingerprint());
}

TEST(SignatureFormatTest, StatusPhrases) {
  EXPECT_EQ("verified", SignatureStatusPhrase(SignatureStatus::kVerified));
  EXPECT_EQ("unsigned", SignatureStatusPhrase(SignatureStatus::kUnsigned));
  EXPECT_EQ("signature unreadable",
            SignatureStatusPhrase(SignatureStatus::kUnreadable));
  EXPECT_EQ("publisher mismatch",
            SignatureStatusPhrase(SignatureStatus::kPublisherMismatch));
  EXPECT_EQ("unknown key",
            SignatureStatusPhrase(SignatureStatus::kUnknownKey));
  EXPECT_EQ("bad signature",
            SignatureStatusPhrase(SignatureStatus::kBadSignature));
}

}  // namespace
}  // namespace pagespeed::packs
