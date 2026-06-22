// Copyright 2026 We-Amp B.V.
//
// Licensed under the We-Amp Business Source License 1.1 (the "License");
// you may not use this file except in compliance with the License.

#include "pagespeed/kernel/proofwire/ledger.h"

#include <cstring>

#include "ed25519.h"  // NOLINT(build/include_subdir) -- @ed25519 includes=["src"]
// sha512.h (bundled with @ed25519) is a C header WITHOUT an extern "C" guard,
// unlike ed25519.h. Wrap it so sha512() keeps C linkage and resolves against the
// compiled C object at link time (otherwise: undefined reference to a mangled
// C++ symbol).
extern "C" {
#include "sha512.h"  // NOLINT(build/include_subdir) -- bundled with @ed25519
}
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace proofwire {

namespace {

// Appends |value| as 8 little-endian bytes to |out|. Used for seq, timestamp,
// and payload length so the canonical layout is fixed-width and unambiguous.
void AppendLE64(uint64 value, GoogleString* out) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>(value & 0xff));
    value >>= 8;
  }
}

const unsigned char* UC(StringPiece s) {
  return reinterpret_cast<const unsigned char*>(s.data());
}

// Best-effort secure wipe whose volatile writes the optimizer may not elide.
// ProofWire depends only on //pagespeed/kernel/base and @ed25519 -- it has NO
// OpenSSL/BoringSSL dependency, so OPENSSL_cleanse / explicit_bzero are deliberately not used.
void SecureWipe(GoogleString* s) {
  if (s->empty()) {
    return;
  }
  volatile char* p = &(*s)[0];
  for (size_t i = 0; i < s->size(); ++i) {
    p[i] = 0;
  }
}

}  // namespace

GoogleString GenesisPrevHash() { return GoogleString(kSha512DigestLen, '\0'); }

GoogleString Ledger::ComputeRecordHash(StringPiece prev_hash, uint64 seq,
                                       int64 timestamp_us,
                                       StringPiece payload) {
  // Build the canonical pre-image:
  //   prev_hash || LE64(seq) || LE64(timestamp_us) || LE64(len(payload)) ||
  //   payload
  GoogleString pre_image;
  pre_image.reserve(prev_hash.size() + 24 + payload.size());
  pre_image.append(prev_hash.data(), prev_hash.size());
  AppendLE64(seq, &pre_image);
  // timestamp_us is signed but its two's-complement bit pattern hashes fine;
  // reinterpret as uint64 for a fixed-width encoding.
  AppendLE64(static_cast<uint64>(timestamp_us), &pre_image);
  AppendLE64(static_cast<uint64>(payload.size()), &pre_image);
  pre_image.append(payload.data(), payload.size());

  unsigned char digest[kSha512DigestLen];
  sha512(reinterpret_cast<const unsigned char*>(pre_image.data()),
         pre_image.size(), digest);
  return GoogleString(reinterpret_cast<const char*>(digest), kSha512DigestLen);
}

Ledger::Ledger(StringPiece public_key, StringPiece private_key)
    : public_key_(public_key.data(), public_key.size()),
      private_key_(private_key.data(), private_key.size()) {}

Ledger::~Ledger() {
  // Scrub the copied private key on destruction. Harmless in the shipped
  // unwired/default-off config (only a test key exists), but removes a footgun
  // before a wedge ever wires a real signing key into a long-lived process.
  SecureWipe(&private_key_);
}

bool Ledger::Append(StringPiece payload, int64 timestamp_us) {
  if (public_key_.size() != kEd25519PublicKeyLen ||
      private_key_.size() != kEd25519PrivateKeyLen) {
    return false;  // Malformed keypair -- refuse rather than emit garbage.
  }

  LedgerRecord record;
  record.seq = static_cast<uint64>(records_.size());
  record.prev_hash =
      records_.empty() ? GenesisPrevHash() : records_.back().record_hash;
  record.timestamp_us = timestamp_us;
  record.payload.assign(payload.data(), payload.size());
  record.record_hash = ComputeRecordHash(record.prev_hash, record.seq,
                                         record.timestamp_us, record.payload);

  unsigned char sig[kEd25519SignatureLen];
  ed25519_sign(sig, UC(record.record_hash), record.record_hash.size(),
               UC(public_key_), UC(private_key_));
  record.signature.assign(reinterpret_cast<const char*>(sig),
                          kEd25519SignatureLen);

  records_.push_back(record);
  return true;
}

VerifyResult Ledger::VerifyChain() const {
  return Verify(records_, public_key_);
}

VerifyResult Ledger::Verify(const std::vector<LedgerRecord>& records,
                            StringPiece public_key) {
  VerifyResult result;  // ok=true by default (empty chain is valid).

  GoogleString expected_prev = GenesisPrevHash();
  for (size_t i = 0; i < records.size(); ++i) {
    const LedgerRecord& r = records[i];

    // 1. Strict monotonic sequence: seq must equal its index. A swap or
    //    reorder makes the lower-indexed record's seq mismatch first.
    if (r.seq != static_cast<uint64>(i)) {
      result.ok = false;
      result.first_bad_seq = static_cast<uint64>(i);
      result.message = "sequence number does not match position";
      return result;
    }

    // 2. Chain linkage: prev_hash must equal the predecessor's record_hash
    //    (genesis for the first record). Truncation, reorder, or an altered
    //    predecessor breaks this.
    if (r.prev_hash != expected_prev) {
      result.ok = false;
      result.first_bad_seq = r.seq;
      result.message = "prev_hash does not link to predecessor";
      return result;
    }

    // 3. Recompute the canonical record hash and compare. Any change to
    //    payload/timestamp/seq/prev_hash changes the hash here. Check 2 above
    //    must precede this: it guarantees prev_hash is the canonical 64-byte
    //    predecessor digest before we re-hash it.
    GoogleString recomputed =
        ComputeRecordHash(r.prev_hash, r.seq, r.timestamp_us, r.payload);
    if (recomputed != r.record_hash) {
      result.ok = false;
      result.first_bad_seq = r.seq;
      result.message = "record_hash mismatch (payload or fields tampered)";
      return result;
    }

    // 4. Signature must verify against the record_hash under the public key.
    //    A forged or corrupted signature, or a key swap, fails here.
    if (r.signature.size() != kEd25519SignatureLen ||
        r.record_hash.size() != kSha512DigestLen ||
        public_key.size() != kEd25519PublicKeyLen) {
      result.ok = false;
      result.first_bad_seq = r.seq;
      result.message = "malformed signature, hash, or public key";
      return result;
    }
    if (ed25519_verify(UC(r.signature), UC(r.record_hash), r.record_hash.size(),
                       UC(public_key)) != 1) {
      result.ok = false;
      result.first_bad_seq = r.seq;
      result.message = "Ed25519 signature verification failed";
      return result;
    }

    expected_prev = r.record_hash;
  }

  return result;
}

}  // namespace proofwire
}  // namespace net_instaweb
