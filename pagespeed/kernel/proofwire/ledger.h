// Copyright 2026 We-Amp B.V.
//
// SPDX-License-Identifier: Apache-2.0
//
// ProofWire served-evidence ledger (FREE LOCAL tier).
//
// An append-only, hash-chained, Ed25519-signed "flight recorder" for serving
// decisions. Each appended record is a signed OBSERVATION of what was observed
// or done while serving a request (which wedge fired, the verdict, the
// transforms applied). The chain is tamper-evident: any modification,
// reordering, or removal of records breaks the SHA-512 hash chain and/or an
// Ed25519 signature, and VerifyChain() reports exactly where.
//
// This is deliberately NOT a safety, correctness, or compliance oracle. It does
// not assert that what was done was correct or compliant -- it only records and
// signs WHAT WAS OBSERVED, so a holder can later detect tampering of that
// record. A relying party must form its own judgement about the observations
// themselves.
//
// Scope guardrails (FREE LOCAL tier only):
//   * No Merkle-root anchoring, no transparency-log / Rekor anchoring.
//   * No queryable API, no retention service, no entitlement / settlement.
//     Those belong to the PAID anchored tier and are intentionally out of
//     scope here.
//   * DEFAULT-OFF and NON-SHIPPING: this is a self-contained kernel primitive
//     plus its test. It is NOT wired into any request or serving path. Other
//     wedges may later emit into it; this pass ships only the verifiable
//     primitive.
//
// Crypto reuse: the external @ed25519 library supplies both the Ed25519
// signing/verification and the bundled SHA-512 used for the chain hash. No new
// crypto dependency and no parallel crypto path are introduced.

#ifndef PAGESPEED_KERNEL_PROOFWIRE_LEDGER_H_
#define PAGESPEED_KERNEL_PROOFWIRE_LEDGER_H_

#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace proofwire {

// Fixed sizes for the reused @ed25519 primitives.
constexpr size_t kEd25519PublicKeyLen = 32;
constexpr size_t kEd25519PrivateKeyLen = 64;
constexpr size_t kEd25519SignatureLen = 64;
constexpr size_t kSha512DigestLen = 64;

// The genesis predecessor hash is a fixed all-zero 64-byte value.
GoogleString GenesisPrevHash();

// One immutable entry in the ledger. All hashes/signatures are raw bytes stored
// in GoogleString (which is std::string and binary-safe).
struct LedgerRecord {
  uint64 seq = 0;            // 0-based, strictly monotonic by +1.
  GoogleString prev_hash;    // 64 bytes: record_hash of the predecessor, or
                             // GenesisPrevHash() for seq 0.
  int64 timestamp_us = 0;    // Injected; never read from a wall clock.
  GoogleString payload;      // Opaque observation (which wedge / verdict /
                             // transforms). Never a safety/compliance claim.
  GoogleString record_hash;  // 64 bytes: see Ledger canonical layout below.
  GoogleString signature;    // 64 bytes: Ed25519(record_hash).
};

// Result of a verification pass. |first_bad_seq| is the POSITION in the supplied
// record vector at which the first problem was detected (equal to the record's
// seq once the seq==index check has passed). It is only meaningful when
// ok==false; on success it is left at 0, which is indistinguishable from a
// "failure at position 0" except by the ok flag.
struct VerifyResult {
  bool ok = true;
  uint64 first_bad_seq = 0;
  GoogleString message;  // Human-readable reason; set only when !ok.
};

// Append-only, hash-chained, Ed25519-signed ledger.
//
// Determinism: both the keypair and every timestamp are injected, so an
// identical sequence of Append() calls yields a byte-identical chain. The class
// never reads a wall clock and never generates randomness.
//
// Canonical record-hash layout (one fixed byte layout, hashed with SHA-512):
//
//     record_hash = SHA512( prev_hash               // 64 bytes
//                        || LE64(seq)               //  8 bytes, little-endian
//                        || LE64(timestamp_us)      //  8 bytes, little-endian
//                        || LE64(payload.size())    //  8 bytes, little-endian
//                        || payload )               //  payload.size() bytes
//
// The length prefix on the payload makes the layout unambiguous (no field can
// be shifted into another), and binding prev_hash + seq into the hash is what
// makes the chain tamper-evident.
//
// The signature signs the bare record_hash digest. Domain separation (binding a
// versioned ProofWire tag into the SIGNED message so a key reused across sibling
// protocols cannot be confused) is DEFERRED to the serving-path wiring pass: it
// changes the signed-byte construction and must land -- with an updated
// known-answer test vector -- before the first real emitter persists or exports
// any chain. Zero chains are persisted today, so deferral is free.
class Ledger {
 public:
  // |public_key| must be 32 bytes and |private_key| 64 bytes (raw @ed25519
  // key material, e.g. from net_instaweb::CreateKeypair). The keys are copied.
  Ledger(StringPiece public_key, StringPiece private_key);
  // Scrubs the copied private key on destruction (best-effort secure wipe).
  ~Ledger();

  // Not copyable (the keypair / chain are owned state).
  Ledger(const Ledger&) = delete;
  Ledger& operator=(const Ledger&) = delete;

  // Appends a new signed record linking to the current tail (or genesis when
  // empty). |timestamp_us| is injected by the caller. Returns true on success;
  // returns false only if the configured keypair is malformed (wrong length),
  // in which case nothing is appended.
  bool Append(StringPiece payload, int64 timestamp_us);

  // Walks the entire chain and verifies, for every record in order:
  //   * seq == index (strictly monotonic, no gaps/reorder),
  //   * prev_hash links to the predecessor's record_hash (genesis for seq 0),
  //   * record_hash equals the recomputed canonical hash,
  //   * signature verifies against record_hash under the public key.
  // Returns ok=true for an empty or fully valid chain; otherwise ok=false with
  // first_bad_seq set to the lowest offending sequence number.
  VerifyResult VerifyChain() const;

  // Read-only access to the underlying records (e.g. for an out-of-band export
  // by a caller). No anchoring or transport is performed here.
  const std::vector<LedgerRecord>& records() const { return records_; }
  size_t size() const { return records_.size(); }

  // Verifies an arbitrary chain of records (e.g. one exported from a Ledger and
  // transported elsewhere) against |public_key|, applying the same four checks
  // as VerifyChain(). This lets a relying party re-verify a received chain
  // without a live Ledger instance, and is what VerifyChain() delegates to.
  // Obtaining the AUTHENTIC public key (out-of-band, or via the anchor in a
  // future paid tier) is the relying party's responsibility: Verify only proves
  // the chain is internally consistent and signed by WHATEVER key is supplied,
  // never that that key is the legitimate origin's.
  static VerifyResult Verify(const std::vector<LedgerRecord>& records,
                             StringPiece public_key);

  // Computes the canonical record hash for the given fields. Exposed so a
  // verifier can recompute independently; used internally by Append/Verify.
  static GoogleString ComputeRecordHash(StringPiece prev_hash, uint64 seq,
                                        int64 timestamp_us,
                                        StringPiece payload);

 private:
  GoogleString public_key_;   // 32 bytes.
  GoogleString private_key_;  // 64 bytes.
  std::vector<LedgerRecord> records_;
};

}  // namespace proofwire
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_PROOFWIRE_LEDGER_H_
