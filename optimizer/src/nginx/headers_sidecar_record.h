// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_HEADERS_SIDECAR_RECORD_H_
#define PAGESPEED_SRC_NGINX_HEADERS_SIDECAR_RECORD_H_

// Store-side half of the response-header sidecar for the nginx front stage
// (issue #208).  Pure C++ (no nginx headers), so every decision the
// record arm makes about origin headers is unit-testable without an nginx
// build — the same pattern as revalidation_storability.h.
//
// THE RULE.  A response the front stage serves from cache must carry the same
// request-independent headers as the origin response it was recorded from,
// and a response whose headers cannot be reproduced must not be cached.  The
// library gate (lib/cache/headers_sidecar.h, ClassifyForHeadersSidecar)
// decides which is which; this file applies its verdict at the three places
// the front stage stores or keeps an entry:
//
//   1. the header filter's MISS block (refusal → uncacheable, and an entry
//      recorded earlier under different headers is evicted);
//   2. record_response (sidecar FIRST, then the entry, with two flag bits on
//      the entry recording the classification; the "stored entry exists, skip
//      the write" shortcut is bypassed when the stored classification is out
//      of date);
//   3. the 304 restamp path (RFC 9111 §4.3.4 merge of the stored block with
//      the 304's lines; rewrite, keep, or drop the URL).
//
// WHICH HEADERS.  Always the ORIGIN's lines, snapshotted at the top of the
// header filter before the module adds any of its own (X-PageSpeed,
// Accept-CH, x-verified-bot, its own Vary).  Classifying the live list at
// record time would refuse every response on the module's own names, or
// store the module's Vary as if the origin had sent it.
//
// TWO BITS ON THE ENTRY (lib/classify/alternate_metadata.h):
//   kFlagHeadersClassified       — recorded by a build that ran the gate;
//   kFlagHeadersSidecarRequired  — the gate stored a non-empty block for the
//                                  URL, so a serve must find it or MISS.
// Unmarked entries (recorded before this change) keep serving exactly as
// they did; nothing here refuses them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {

inline constexpr uint8_t kHeadersSidecarBits =
    AlternateMetadata::kFlagHeadersClassified |
    AlternateMetadata::kFlagHeadersSidecarRequired;

inline constexpr AlternateId kHeadersSidecarAlternateId =
    static_cast<AlternateId>(SentinelId::kHeadersSidecar);

// The first header name a fall-through verdict names, lowercased, for the
// operator log line ("origin header <name> cannot be reproduced on cache
// hits").  Empty when the classification carries no
// "unreconstructible-header:" reason (a malformed or oversized block); the
// caller then logs the first reason verbatim.
std::string_view FirstUnreproducibleHeader(
    const SidecarClassification& classification);

// ASCII case-insensitive field-name comparison.
bool HeaderNameEquals(std::string_view a, std::string_view b);

// What record_response knows about the identity entry already stored for the
// URL, from the one read it performs anyway.
struct StoredIdentityState {
  bool exists = false;
  uint8_t flags = 0;
};

// Why the stored identity is (or is not) rewritten.  Diagnostics and tests;
// the action follows from it.
enum class SidecarRecordReason : uint8_t {
  kFresh,           // nothing stored: record
  kForced,          // the caller already decided to overwrite (origin
                    // refreshed, Accept-marker disagreement)
  kUpgrade,         // stored entry predates the gate (no classified bit)
  kSelfHeal,        // stored entry requires a block and the block is gone
  kPayloadChanged,  // stored block differs from the fresh one
  kCarryFlip,       // stored carry state differs from the fresh one
  kCurrent,         // stored entry and block match: keep them (skip write)
};

// The observable outcome of RecordEntryWithHeadersSidecar.
enum class SidecarRecordOutcome : uint8_t {
  kSkipped,             // stored entry kept; caller proceeds to notify
  kWritten,             // (block +) entry written
  kRemovedAndWritten,   // carry flip: URL removed, then recorded fresh
  kNotSlotHolder,       // a rewrite was due, another request is doing it:
                        // nothing recorded, nothing to notify
  kSidecarWriteFailed,  // block not written, so the entry was not written
  kEntryWriteFailed,    // entry write failed (an orphan block is harmless)
  kNotEligible,         // the snapshot is not swap-eligible: nothing stored
};

struct SidecarRecordResult {
  SidecarRecordOutcome outcome = SidecarRecordOutcome::kSkipped;
  SidecarRecordReason reason = SidecarRecordReason::kCurrent;
  // A header block was stored for the URL (set even when the entry write
  // that follows it fails).
  bool block_written = false;
};

// Record the identity entry for a classified response.  The rules:
//
//   * SIDECAR FIRST.  When the fresh response carries a block, the block is
//     written (through WriteHeadersSidecar, which is the gate) and only on a
//     stored block is the entry written, flagged classified + required.  A
//     failed block write stores no entry: a flagged entry without its block
//     is exactly the silent header drop this exists to prevent.
//   * NOTHING TO CARRY.  The entry is flagged classified only, written, and
//     any block left from an earlier response is removed after the commit.
//   * RE-RECORD.  The "stored exists, skip the write" shortcut is kept only
//     when the stored entry is classified, its carry state matches, and (when
//     it carries) its block is present and byte-identical to the fresh one.
//     A carry-state switch removes the URL (entry, variants and block — the
//     variants carry the old bits) before recording fresh.
//   * SINGLE FLIGHT.  The self-heal, payload-changed and carry-flip rewrites
//     run only for the request that wins `acquire_slot()`; the others record
//     nothing.  Not asked when `force_rewrite` is set: the caller already
//     overwrites unconditionally in that case.
//
// `meta` is the entry metadata as the caller built it; the two bits are set
// or cleared here.  A template over the cache type so a test can run it
// against a real cache on a temp volume (the restamp_apply_test seam);
// production instantiates it with PageSpeedCache.
template <typename Cache, typename Metadata, typename AcquireSlot>
SidecarRecordResult RecordEntryWithHeadersSidecar(
    Cache& cache, std::string_view url, std::string_view hostname,
    std::string_view scheme, AlternateId id, Metadata meta,
    std::span<const std::byte> body, std::span<const HeaderField> origin,
    const StoredIdentityState& stored, bool force_rewrite,
    AcquireSlot&& acquire_slot) {
  SidecarRecordResult result;
  const SidecarClassification fresh = ClassifyForHeadersSidecar(origin);
  if (!fresh.swap_eligible()) {
    result.outcome = SidecarRecordOutcome::kNotEligible;
    return result;
  }
  const bool carry = !fresh.payload.empty();

  // Decide whether the stored identity is current.
  bool single_flight = false;
  bool remove_first = false;
  if (!stored.exists) {
    result.reason = SidecarRecordReason::kFresh;
  } else if ((stored.flags & AlternateMetadata::kFlagHeadersClassified) == 0) {
    result.reason = SidecarRecordReason::kUpgrade;
  } else {
    const bool stored_required =
        (stored.flags & AlternateMetadata::kFlagHeadersSidecarRequired) != 0;
    if (stored_required != carry) {
      result.reason = SidecarRecordReason::kCarryFlip;
      single_flight = true;
      remove_first = true;
    } else if (carry) {
      auto block = cache.ReadHeadersSidecar(url, hostname, scheme);
      if (!block.has_value() || !block->is_valid()) {
        result.reason = SidecarRecordReason::kSelfHeal;
        single_flight = true;
      } else if (Cache::HeadersSidecarPayload(*block) != fresh.payload) {
        result.reason = SidecarRecordReason::kPayloadChanged;
        single_flight = true;
      } else {
        result.reason = SidecarRecordReason::kCurrent;
      }
    } else {
      result.reason = SidecarRecordReason::kCurrent;
    }
  }

  if (result.reason == SidecarRecordReason::kCurrent) {
    if (!force_rewrite) {
      result.outcome = SidecarRecordOutcome::kSkipped;
      return result;
    }
    result.reason = SidecarRecordReason::kForced;
  }
  if (single_flight && !force_rewrite && !acquire_slot()) {
    result.outcome = SidecarRecordOutcome::kNotSlotHolder;
    return result;
  }

  if (remove_first) {
    // Whole key: the variants the worker derived carry the old bits, so
    // rewriting the identity alone would leave them serving under the old
    // carry state.  NotFound is the end state asked for.
    (void)cache.Remove(url, hostname, scheme);
  }

  if (carry) {
    auto written = cache.WriteHeadersSidecar(url, hostname, scheme, origin);
    if (!written.has_value() || !written->stored) {
      result.outcome = SidecarRecordOutcome::kSidecarWriteFailed;
      return result;
    }
    result.block_written = true;
    meta.flags |= kHeadersSidecarBits;
  } else {
    meta.flags |= AlternateMetadata::kFlagHeadersClassified;
    meta.flags &=
        static_cast<uint8_t>(~AlternateMetadata::kFlagHeadersSidecarRequired);
  }

  auto wh = cache.WriteAlternate(url, hostname, scheme, id, body.size(), meta);
  if (!wh.has_value() || !wh->write_sync(body).has_value() ||
      !wh->close_sync().has_value()) {
    result.outcome = SidecarRecordOutcome::kEntryWriteFailed;
    return result;
  }
  // Same post-commit RAM eviction the legacy write performs:
  // the RAM tier is write-around, so a pre-write copy would keep answering.
  cache.EvictAlternateFromRamCache(url, hostname, scheme, id);

  if (!carry) {
    // A block left by an earlier response would contradict the entry just
    // written.  Nothing reads it without the required bit, but leaving it
    // invites a later reader to trust it.  NotFound is fine.
    (void)cache.RemoveAlternate(url, hostname, scheme,
                                kHeadersSidecarAlternateId);
  }
  result.outcome = remove_first ? SidecarRecordOutcome::kRemovedAndWritten
                                : SidecarRecordOutcome::kWritten;
  return result;
}

// Refusal eviction: the fresh origin response cannot be reproduced on a
// hit, so an entry recorded earlier for the URL must stop serving.  Runs in
// the header filter, because an uncacheable response never reaches
// record_response.  Probes the identity first so a URL that is refused on
// every request (and therefore never cached) does not pay a whole-key remove
// each time.  Returns true when an entry was found and removed.
template <typename Cache>
bool EvictOnSidecarRefusal(Cache& cache, std::string_view url,
                           std::string_view hostname, std::string_view scheme,
                           AlternateId identity_id) {
  if (!cache.AlternateExists(url, hostname, scheme, identity_id)) {
    return false;
  }
  auto removed = cache.Remove(url, hostname, scheme);
  return removed.has_value();
}

// --- 304 restamp -------------------------------------------------------

enum class RestampSidecarAction : uint8_t {
  // The stash predates the gate: restamp exactly as before.
  kLegacy,
  // The merged set carries the same block (or still nothing): restamp only.
  kRestampOnly,
  // Same carry state, different block: write the merged block first, then
  // restamp; a failed block write skips the restamp.
  kRewriteThenRestamp,
  // Refusal, carry-state switch, or a required block that cannot be read:
  // remove the URL and serve this response; the next request re-records.
  kRemove,
};

struct RestampSidecarPlan {
  RestampSidecarAction action = RestampSidecarAction::kLegacy;
  // The merged set M (RFC 9111 §4.3.4): stored fields whose name the 304
  // does not carry, then every 304 line.  Views into the stored payload and
  // the 304 snapshot; both must outlive the plan.
  std::vector<HeaderField> merged;
  // The stored fields whose names the 304 did not carry — what the rewritten
  // 200 adds to the 304's own lines.  Views, as above.
  std::vector<HeaderField> stored_not_overridden;
  // The classification of `merged` (meaningless for kLegacy).
  SidecarClassification classification;
};

// Plan the 304 path for a stashed entry.  `stored_flags` is the stash's
// flags byte; `stored_payload` is the URL's stored block, or nullopt when it
// could not be read (only consulted when the stash requires one).
RestampSidecarPlan PlanRestampSidecar(
    uint8_t stored_flags, std::optional<std::string_view> stored_payload,
    std::span<const HeaderField> response_304);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_HEADERS_SIDECAR_RECORD_H_
