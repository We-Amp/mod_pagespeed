// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Web Bot Auth opt-in counter store.
//
// A small memory-mapped, cross-process counter file that records verified
// AI-crawl volume per signer identity plus a coarse verify-latency histogram,
// for the opt-in /.well-known/webbotauth-counter endpoint. It is the mod_pagespeed
// 1.15 equivalent of ModPageSpeed 2.0's serve-stats v6 apparatus; the on-disk
// binary layout is engine-private, but the JSON document the nginx endpoint
// derives from it is BYTE-COMPATIBLE with 2.0's (the same corp aggregator
// consumes both engines' documents), which is why the per-signer keyid hash is
// the identical UNSALTED FNV-1a-64 recipe (see HashWebBotAuthKeyid).
//
// Why a dedicated mmap file instead of the engine's shared-memory Statistics:
// the Statistics/Variable substrate is a table of FIXED named scalar counters,
// frozen after Init and offering only Add/Set/Get -- it cannot express a
// lock-free CAS find-or-claim over a per-signer slot table, nor a boot identity
// minted once and preserved across worker restarts. This file mirrors 2.0's
// approach exactly: a fixed struct at offset 0 of a MAP_SHARED file, every
// counter an 8-byte-aligned uint64 updated with std::atomic_ref, so all nginx
// worker processes (which fork from a common master that maps the file first)
// increment the same physical counters race-free.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_WEBBOTAUTH_COUNTER_STORE_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_WEBBOTAUTH_COUNTER_STORE_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace net_instaweb {
namespace webbotauth {

// Fixed struct at offset 0 of the mmap'd kFileSize file. All counter fields are
// 8-byte aligned so every uint64 can be updated with std::atomic_ref across the
// shared mapping (all nginx workers + the master). Naturally-aligned 64-bit
// loads/stores are atomic on all supported platforms (x86-64, aarch64), so
// relaxed atomic RMW on these counters is safe for multi-process use: the worst
// case under a misconfiguration is cosmetic double-counting, never a torn read.
struct WebBotAuthCounterStats {
  static constexpr uint32_t kMagic = 0x57424143;  // 'W''B''A''C'
  static constexpr uint32_t kVersion = 1;
  static constexpr size_t kFileSize = 512;

  // Maximum number of distinct verified signer identities tracked by keyid; the
  // (N+1)-th and beyond fall into other_verified_bots.
  static constexpr size_t kMaxCountedBots = 8;

  uint32_t magic;
  uint32_t version;

  // Cumulative verdict totals for requests that carried signature material.
  // verified = the signature validated (signed-agent or verified-bot);
  // invalid  = signature material present but fail-closed to unknown;
  // other    = parseable RFC 9421 material NOT tagged web-bot-auth (treated as
  //            unsigned, never "invalid"). Human (no material) is counted here
  //            nowhere.
  uint64_t verified_total;
  uint64_t invalid_total;
  uint64_t other_total;

  // One per-signer slot: a stable, REPRODUCIBLE non-crypto hash of the RFC-9421
  // keyid (unsalted FNV-1a-64, see HashWebBotAuthKeyid) and the count of
  // verified requests attributed to it. kid_hash == 0 means the slot is empty;
  // the one input that would hash to 0 is remapped to a fixed non-zero sentinel
  // so a real keyid never collides with "empty". Slots are claimed lock-free
  // (CAS on kid_hash) and are cross-process-safe.
  struct SignerSlot {
    uint64_t kid_hash;  // unsalted FNV-1a-64 of the keyid; 0 == empty slot
    uint64_t count;     // verified requests attributed to this keyid
  };
  SignerSlot signers[kMaxCountedBots];

  // Verified requests whose signer keyid did not fit in the kMaxCountedBots
  // slots (all slots already claimed by other keyids).
  uint64_t other_verified_bots;

  // Coarse verify-latency histogram for verified requests. Buckets are
  // disjoint (each request lands in exactly one).
  uint64_t verify_latency_lt100us;
  uint64_t verify_latency_lt1ms;
  uint64_t verify_latency_lt10ms;
  uint64_t verify_latency_ge10ms;

  // Days since the Unix epoch when this counting file was first created fresh
  // ("since" date). Minted in the create-fresh path only; PRESERVED across
  // reopen (NOT a counter).
  uint64_t counting_since_unix_day;

  // Random 16-byte INSTANCE identity for this counting file. Despite the "boot"
  // name it is NOT a per-process boot id: it is minted only on a fresh file
  // create and is STABLE across ordinary worker restarts / config reloads
  // (which reuse the file). It changes only when the file is recreated: a
  // version bump or file deletion. Distinct ids across concurrent origin
  // responses indicate multiple instances / a load balancer; a single
  // restarting instance keeps one id. A counter RESET is therefore detectable
  // via a cumulative-total DECREASE, not via this id changing. Identity, NOT a
  // counter. Kept LAST in the struct.
  uint8_t boot_id[16];
};
static_assert(sizeof(WebBotAuthCounterStats) <=
              WebBotAuthCounterStats::kFileSize);

// Derive the counter file path from a writable directory (e.g. the configured
// FileCachePath). Returns "{dir}/.pagespeed-webbotauth-counter".
std::string WebBotAuthCounterPath(const std::string& dir);

// Open an existing counter file (read-write) and validate magic+version.
// Returns nullptr if the file doesn't exist, is too small, or has the wrong
// magic/version. Caller must call CloseWebBotAuthCounter() to unmap.
WebBotAuthCounterStats* OpenWebBotAuthCounter(const std::string& path);

// Open a valid existing counter file, else create a fresh one (minting a random
// boot identity + the counting-since day, all counters zero). PRESERVES the
// identity and existing counters of a valid existing file -- counters persist
// across worker restart / config reload, resetting only when the file is
// deleted or the version changes. Returns nullptr on failure. Caller must call
// CloseWebBotAuthCounter() to unmap.
WebBotAuthCounterStats* OpenOrCreateWebBotAuthCounter(const std::string& path);

// Unmap a previously mapped region.
void CloseWebBotAuthCounter(WebBotAuthCounterStats* stats);

// Stable, REPRODUCIBLE non-crypto hash of an RFC-9421 keyid string used as the
// per-signer slot key. Plain UNSALTED FNV-1a-64 over the raw keyid bytes
// (offset basis 0xcbf29ce484222325, prime 0x100000001b3), with the single input
// that would hash to 0 remapped to a fixed non-zero sentinel so a real keyid
// never collides with the "empty slot" value 0. Intentionally reproducible by
// anyone (no secret salt): it is the only handle a downstream consumer has to
// identify a signer at an origin where that keyid is not in the operator
// registry. IDENTICAL recipe to ModPageSpeed 2.0 so hashes are cross-engine
// compatible (e.g. the probe kid "GBxqWscB-GOkwNXEhmWQA4SY2gWRH2Hz4dAvzRcK1gw"
// hashes to d7a140a27d88bea6 on both engines).
uint64_t HashWebBotAuthKeyid(std::string_view keyid);

// Atomically record one classification of a request that carried WEB-BOT-AUTH
// signature material. `verified` == the signature validated. stats == nullptr
// is a no-op. Callers never invoke this for signature-less (human) requests nor
// for non-web-bot-auth signature material (see RecordWebBotAuthOtherSignature).
void RecordWebBotAuthSigned(WebBotAuthCounterStats* stats, bool verified);

// Atomically record one request that carried parseable signature material with
// NO web-bot-auth-tagged member (classified as if unsigned). stats == nullptr
// is a no-op.
void RecordWebBotAuthOtherSignature(WebBotAuthCounterStats* stats);

// Atomically record one VERIFIED request against its signer. Finds-or-claims the
// slot for `keyid` lock-free (CAS on kid_hash) and increments its count; if all
// kMaxCountedBots slots are taken by other keyids, increments other_verified_bots
// instead. Also buckets `latency_us` into the coarse verify-latency histogram.
// stats == nullptr is a no-op. This is ADDITIONAL to
// RecordWebBotAuthSigned(stats, true) -- callers invoke both on a verified
// verdict. Records ALL verified keyids (including our own probe key);
// probe-kid / first-party exclusion is an aggregator-side concern.
void RecordWebBotAuthVerifiedSigner(WebBotAuthCounterStats* stats,
                                    std::string_view keyid,
                                    uint64_t latency_us);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_WEBBOTAUTH_COUNTER_STORE_H_
