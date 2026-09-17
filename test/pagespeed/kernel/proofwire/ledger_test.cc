// Copyright 2026 We-Amp B.V.
//
// SPDX-License-Identifier: Apache-2.0

#include "pagespeed/kernel/proofwire/ledger.h"

#include <vector>

#include "ed25519.h"  // NOLINT(build/include_subdir) -- @ed25519 includes=["src"]
#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"  // StrCat, IntegerToString

namespace net_instaweb {
namespace proofwire {
namespace {

// Mints a deterministic Ed25519 keypair from a fixed 32-byte seed via @ed25519's
// own ed25519_create_keypair. This keeps the test fully offline: no fixture from
// another package, no network.
void MakeKeypair(StringPiece seed, GoogleString* public_key,
                 GoogleString* private_key) {
  unsigned char pub[kEd25519PublicKeyLen];
  unsigned char priv[kEd25519PrivateKeyLen];
  ed25519_create_keypair(pub, priv,
                         reinterpret_cast<const unsigned char*>(seed.data()));
  public_key->assign(reinterpret_cast<const char*>(pub), kEd25519PublicKeyLen);
  private_key->assign(reinterpret_cast<const char*>(priv),
                      kEd25519PrivateKeyLen);
}

// Lower-case hex of raw bytes, for known-answer comparisons.
GoogleString BytesToHex(StringPiece bytes) {
  static const char kHex[] = "0123456789abcdef";
  GoogleString out;
  out.reserve(bytes.size() * 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(bytes[i]);
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

// Test fixture: a deterministic Ed25519 keypair from a fixed seed and injected,
// non-wall-clock timestamps.
class LedgerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    MakeKeypair(GoogleString(32, '\0'), &public_key_, &private_key_);
    ASSERT_EQ(kEd25519PublicKeyLen, public_key_.size());
    ASSERT_EQ(kEd25519PrivateKeyLen, private_key_.size());
  }

  // Appends |n| records with deterministic, monotonically increasing injected
  // timestamps and distinct payloads.
  void AppendN(Ledger* ledger, int n) {
    for (int i = 0; i < n; ++i) {
      const int64 ts = 1000 + static_cast<int64>(i);
      EXPECT_TRUE(ledger->Append(StrCat("payload-", IntegerToString(i)), ts));
    }
  }

  GoogleString public_key_;
  GoogleString private_key_;
};

// (a) Append N records -> VerifyChain() == OK, and the chain re-verifies.
TEST_F(LedgerTest, AppendAndVerifyValidChain) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 5);
  ASSERT_EQ(5u, ledger.size());

  VerifyResult r = ledger.VerifyChain();
  EXPECT_TRUE(r.ok) << r.message << " at seq " << r.first_bad_seq;

  // A fresh valid chain re-verifies identically (idempotent, deterministic).
  VerifyResult r2 = ledger.VerifyChain();
  EXPECT_TRUE(r2.ok);
}

// Empty chain is valid, and genesis prev_hash is the all-zero 64-byte const.
TEST_F(LedgerTest, EmptyChainValidAndGenesisIsZero) {
  Ledger ledger(public_key_, private_key_);
  EXPECT_EQ(0u, ledger.size());
  EXPECT_TRUE(ledger.VerifyChain().ok);

  Ledger one(public_key_, private_key_);
  ASSERT_TRUE(one.Append("first", 42));
  EXPECT_EQ(GenesisPrevHash(), one.records()[0].prev_hash);
  EXPECT_EQ(GoogleString(kSha512DigestLen, '\0'), one.records()[0].prev_hash);
  // Each record's prev_hash links to the predecessor's record_hash.
  ASSERT_TRUE(one.Append("second", 43));
  EXPECT_EQ(one.records()[0].record_hash, one.records()[1].prev_hash);
}

// Determinism: identical Append sequences (key + timestamps) produce a
// byte-identical chain.
TEST_F(LedgerTest, DeterministicChain) {
  Ledger a(public_key_, private_key_);
  Ledger b(public_key_, private_key_);
  AppendN(&a, 4);
  AppendN(&b, 4);
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.records()[i].record_hash, b.records()[i].record_hash);
    EXPECT_EQ(a.records()[i].signature, b.records()[i].signature);
  }
}

// (b) Flip one payload byte -> detected AT that seq.
TEST_F(LedgerTest, TamperedPayloadDetected) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 5);
  ASSERT_TRUE(ledger.VerifyChain().ok);

  // Copy the chain out and tamper the payload of record seq 2.
  std::vector<LedgerRecord> chain = ledger.records();
  ASSERT_FALSE(chain[2].payload.empty());
  chain[2].payload[0] ^= 0x01;

  VerifyResult r = Ledger::Verify(chain, public_key_);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(2u, r.first_bad_seq);
}

// (c) Corrupt one 64-byte signature -> detected at that seq.
TEST_F(LedgerTest, TamperedSignatureDetected) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 5);
  ASSERT_TRUE(ledger.VerifyChain().ok);

  std::vector<LedgerRecord> chain = ledger.records();
  ASSERT_EQ(kEd25519SignatureLen, chain[3].signature.size());
  chain[3].signature[10] ^= 0xff;  // flip a byte in the signature

  VerifyResult r = Ledger::Verify(chain, public_key_);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(3u, r.first_bad_seq);
}

// (d) Reorder/swap two records -> detected at the lower affected seq.
TEST_F(LedgerTest, ReorderDetected) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 5);
  ASSERT_TRUE(ledger.VerifyChain().ok);

  std::vector<LedgerRecord> chain = ledger.records();
  std::swap(chain[1], chain[2]);  // swap adjacent records

  VerifyResult r = Ledger::Verify(chain, public_key_);
  EXPECT_FALSE(r.ok);
  // The record now sitting at index 1 carries seq 2 -> seq != index fires
  // first, at the lower affected position.
  EXPECT_EQ(1u, r.first_bad_seq);
}

// (e) Truncate the chain (drop the tail) is by itself indistinguishable from a
// shorter-but-valid chain, so a stricter truncation check binds an expected
// length. Here we verify the inverse and the head-drop case, which the chain
// linkage DOES detect: dropping the genesis/head record breaks prev_hash.
TEST_F(LedgerTest, HeadDropDetected) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 5);
  ASSERT_TRUE(ledger.VerifyChain().ok);

  // Drop the first (genesis) record. The new head still carries seq 1 and a
  // prev_hash pointing at the dropped record -> both the seq check and the
  // genesis-linkage check fail at the lowest position.
  std::vector<LedgerRecord> chain(ledger.records().begin() + 1,
                                  ledger.records().end());
  VerifyResult r = Ledger::Verify(chain, public_key_);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(0u, r.first_bad_seq);  // detected at index 0 of the truncated chain
}

// (e) Tail truncation with an expected-length binding: a relying party that
// knows how many records to expect detects a dropped tail. The chain itself
// stays internally valid (that is the documented free-tier limit), so we assert
// both facts explicitly.
TEST_F(LedgerTest, TailTruncationLimitationDocumented) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 5);
  const size_t expected_len = ledger.size();

  std::vector<LedgerRecord> chain = ledger.records();
  chain.pop_back();  // drop the tail record

  // The truncated prefix is still internally consistent (free-tier limit)...
  EXPECT_TRUE(Ledger::Verify(chain, public_key_).ok);
  // ...but a length-aware relying party detects the missing tail.
  EXPECT_NE(expected_len, chain.size());
}

// A wrong public key (key swap) fails verification at seq 0.
TEST_F(LedgerTest, WrongPublicKeyDetected) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 3);
  ASSERT_TRUE(ledger.VerifyChain().ok);

  GoogleString other_pub, other_priv;
  MakeKeypair(GoogleString(32, '\x01'), &other_pub, &other_priv);
  ASSERT_NE(public_key_, other_pub);

  VerifyResult r = Ledger::Verify(ledger.records(), other_pub);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(0u, r.first_bad_seq);
}

// A malformed keypair causes Append to refuse rather than emit garbage.
TEST_F(LedgerTest, MalformedKeypairRefusesAppend) {
  Ledger bad("too-short", "also-too-short");
  EXPECT_FALSE(bad.Append("x", 1));
  EXPECT_EQ(0u, bad.size());
}

// Known-answer vector freezing the canonical record-hash pre-image layout
// (prev_hash || LE64(seq) || LE64(timestamp_us) || LE64(len) || payload) and the
// SHA-512 over it. Every other test recomputes the hash via the same
// ComputeRecordHash it validates, so a self-consistent change to field order,
// endianness, the length prefix, or the genesis constant would round-trip
// cleanly through all of them while silently breaking interop with an
// independent verifier -- defeating the "a relying party can re-verify" thesis.
// This vector is the only test that fails on such a drift. The digest was
// computed out-of-band (Python hashlib.sha512) over the exact 91-byte pre-image
// for prev_hash=GenesisPrevHash(), seq=0, timestamp_us=42, payload="abc".
//
// NB: this pins the CURRENT free-tier construction, not a frozen public wire
// contract -- the record is designed to gain fields (content fingerprint,
// transforms bitmap, client class) and a domain-separation tag when a real
// emitter is wired in; update this vector in that same change.
TEST(LedgerHashVectorTest, ComputeRecordHashKnownAnswer) {
  const GoogleString hash = Ledger::ComputeRecordHash(
      GenesisPrevHash(), /*seq=*/0, /*timestamp_us=*/42, "abc");
  ASSERT_EQ(kSha512DigestLen, hash.size());
  EXPECT_EQ(
      "e45f74dd56b324733f21487c1c545b451ccc94025a397bac529c03ed6f384b13"
      "9b9358e3eb7930e7cbd1a0168b9b33b9a99cecc7c7be239e08f6247124872988",
      BytesToHex(hash));
}

// Verify's malformed-length guard (signature size) fires before the C-ABI
// ed25519 pointer read. Distinct from TamperedSignatureDetected, which flips a
// byte of a correctly-SIZED signature (caught at ed25519_verify instead).
TEST_F(LedgerTest, MalformedSignatureLengthDetected) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 3);
  ASSERT_TRUE(ledger.VerifyChain().ok);

  std::vector<LedgerRecord> chain = ledger.records();
  chain[1].signature.resize(10);  // wrong length, not a wrong value

  VerifyResult r = Ledger::Verify(chain, public_key_);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(1u, r.first_bad_seq);
  // Prove the malformed-length guard fired, not ed25519_verify (the two are
  // otherwise indistinguishable by (ok, first_bad_seq) at the same seq).
  EXPECT_NE(GoogleString::npos, r.message.find("malformed"));
}

// A wrong-SIZE public key trips the same guard at seq 0. Distinct from
// WrongPublicKeyDetected, which passes a valid 32-byte key that fails later at
// ed25519_verify.
TEST_F(LedgerTest, WrongSizePublicKeyDetected) {
  Ledger ledger(public_key_, private_key_);
  AppendN(&ledger, 2);
  ASSERT_TRUE(ledger.VerifyChain().ok);

  VerifyResult r = Ledger::Verify(ledger.records(), GoogleString(31, '\0'));
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(0u, r.first_bad_seq);
  EXPECT_NE(GoogleString::npos, r.message.find("malformed"));
}

// An empty payload is a valid observation: Append succeeds and the chain
// verifies, with the length prefix keeping an empty payload unambiguous next to
// a non-empty neighbour.
TEST_F(LedgerTest, EmptyPayloadAppendsAndVerifies) {
  Ledger ledger(public_key_, private_key_);
  EXPECT_TRUE(ledger.Append("", 1000));
  EXPECT_TRUE(ledger.Append("nonempty", 1001));
  EXPECT_TRUE(ledger.Append("", 1002));
  ASSERT_EQ(3u, ledger.size());
  EXPECT_TRUE(ledger.records()[0].payload.empty());
  EXPECT_TRUE(ledger.VerifyChain().ok);
}

}  // namespace
}  // namespace proofwire
}  // namespace net_instaweb
