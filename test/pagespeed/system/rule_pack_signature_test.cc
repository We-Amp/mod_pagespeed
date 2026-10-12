// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/rule_pack_signature.h"

#include <cstdint>
#include <string>

#include "absl/strings/escaping.h"
#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/signature_format.h"

#include <openssl/curve25519.h>

namespace net_instaweb {
namespace {

using pagespeed::packs::PackTrust;
using pagespeed::packs::SignatureStatus;
using pagespeed::packs::TrustKey;

constexpr char kPack[] = R"({"pack":{"id":"p","version":"1"},"rules":[]})";

struct Keys {
  Keys() {
    uint8_t pub[32];
    ED25519_keypair(pub, priv);
    public_key.assign(reinterpret_cast<const char*>(pub), sizeof(pub));
  }
  std::string Sign(const std::string& message) const {
    uint8_t sig[64];
    ED25519_sign(sig, reinterpret_cast<const uint8_t*>(message.data()),
                 message.size(), priv);
    return std::string(reinterpret_cast<const char*>(sig), sizeof(sig));
  }
  uint8_t priv[64];
  std::string public_key;
};

SidecarState Sidecar(const std::string& key_id, const std::string& signature) {
  SidecarState s;
  s.present = true;
  s.read_ok = true;
  s.text = "ed25519 " + key_id + " " + absl::Base64Escape(signature) + "\n";
  return s;
}

PackTrust TrustOf(const std::string& key_id, const Keys& keys) {
  PackTrust trust;
  trust.keys.push_back(TrustKey{key_id, keys.public_key});
  return trust;
}

class CheckPackSignatureTest : public testing::Test {
 protected:
  SignatureStatus Check(const std::string& pack, const std::string& publisher,
                        const SidecarState& sidecar, const PackTrust& trust) {
    key_id_.clear();
    return CheckPackSignature(pack, publisher, sidecar, trust, &key_id_);
  }
  Keys keys_;
  std::string key_id_;
};

TEST_F(CheckPackSignatureTest, InTheOrderOfTheFormat) {
  const std::string pack = kPack;
  const PackTrust trust = TrustOf("example-publisher", keys_);
  const SidecarState good = Sidecar("example-publisher", keys_.Sign(pack));

  // 1. no sidecar
  EXPECT_EQ(SignatureStatus::kUnsigned,
            Check(pack, "example-publisher", SidecarState(), trust));
  // 2. present but not read, and present but garbage
  SidecarState unread;
  unread.present = true;
  EXPECT_EQ(SignatureStatus::kUnreadable,
            Check(pack, "example-publisher", unread, trust));
  SidecarState garbage;
  garbage.present = garbage.read_ok = true;
  garbage.text = "hello\n";
  EXPECT_EQ(SignatureStatus::kUnreadable,
            Check(pack, "example-publisher", garbage, trust));
  // 3. the publisher is not the sidecar's key id (checked before the trust
  // list: this key id is not listed either)
  EXPECT_EQ(SignatureStatus::kPublisherMismatch,
            Check(pack, "local", Sidecar("example-publisher", keys_.Sign(pack)),
                  PackTrust()));
  // 4. the key id is not trusted
  EXPECT_EQ(SignatureStatus::kUnknownKey,
            Check(pack, "example-publisher", good, PackTrust()));
  EXPECT_EQ(SignatureStatus::kUnknownKey,
            Check(pack, "example-publisher", good,
                  TrustOf("other-publisher", keys_)));
  // 5. the signature does not verify: a changed pack byte, a changed
  // signature byte, a different key
  EXPECT_EQ(SignatureStatus::kBadSignature,
            Check(std::string(kPack) + " ", "example-publisher", good, trust));
  std::string sig = keys_.Sign(pack);
  sig[10] ^= 1;
  EXPECT_EQ(SignatureStatus::kBadSignature,
            Check(pack, "example-publisher",
                  Sidecar("example-publisher", sig), trust));
  Keys other;
  EXPECT_EQ(SignatureStatus::kBadSignature,
            Check(pack, "example-publisher", good,
                  TrustOf("example-publisher", other)));
  // verified
  EXPECT_EQ(SignatureStatus::kVerified,
            Check(pack, "example-publisher", good, trust));
  EXPECT_EQ("example-publisher", key_id_);
}

TEST(VerifyEd25519Test, RefusesWrongSizes) {
  Keys keys;
  const std::string sig = keys.Sign(kPack);
  EXPECT_TRUE(VerifyEd25519(kPack, sig, keys.public_key));
  EXPECT_FALSE(VerifyEd25519(kPack, sig.substr(1), keys.public_key));
  EXPECT_FALSE(VerifyEd25519(kPack, sig, keys.public_key.substr(1)));
  EXPECT_FALSE(VerifyEd25519(kPack, "", ""));
}

// A signature made by sign-pack.sh (OpenSSL) verifies with BoringSSL. The
// signing key was thrown away; only the public half is here.
TEST(OpenSslInteropTest, ASignaturePackSignMadeVerifies) {
  const std::string pack =
      R"({"pack":{"id":"vector","version":"0.0.1","publisher":"vector-key"},"rules":[]})";
  const std::string sidecar_line =
      "ed25519 vector-key l1EnhYBLwTAT9hPlKPPo0IaMFLFp6ZZcZVRUR2Pji9euhgACVosN2cfGmhntfZxuIBXdBsA7M2c43cEDy0UqAw==\n";
  const auto key = pagespeed::packs::DecodeTrustKey("fuRqJ3FnXonzbp8KuLy7yWR8oZU8Yue62CUrstW39Z4=");
  ASSERT_TRUE(key.has_value());
  PackTrust trust;
  trust.keys.push_back(TrustKey{"vector-key", *key});
  SidecarState sidecar;
  sidecar.present = sidecar.read_ok = true;
  sidecar.text = sidecar_line;
  std::string key_id;
  EXPECT_EQ(SignatureStatus::kVerified,
            CheckPackSignature(pack, "vector-key", sidecar, trust, &key_id));
  EXPECT_EQ("vector-key", key_id);
  std::string changed = pack;
  changed[12] = 'X';
  EXPECT_EQ(SignatureStatus::kBadSignature,
            CheckPackSignature(changed, "vector-key", sidecar, trust, &key_id));
}

}  // namespace
}  // namespace net_instaweb
