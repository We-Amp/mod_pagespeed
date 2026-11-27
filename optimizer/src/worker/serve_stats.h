// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_WORKER_SERVE_STATS_H_
#define PAGESPEED_SRC_WORKER_SERVE_STATS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lib/classify/content_type.h"

namespace pagespeed {

class MessageHandler;

// Shared memory-mapped statistics file for tracking serve-time bandwidth
// savings and serve-class outcomes.  Written by a front-end (atomic increments
// on cache HIT) and, since v8, by the worker for the saturation group; read by
// the worker's stats API and by front-end consumers through the C API.
//
// INVARIANT (per FIELD GROUP, not per file).  Through v7 the rule was "exactly
// one writer process per cache" and every field obeyed it.  v8 breaks the
// per-file form — the worker now writes the saturation group — so the
// invariant is restated per group, which is the form that actually holds:
//
//   * bytes/hits, Web Bot Auth, RSL-CAP, zero-copy, bounded-stale
//       WRITER: the front end.  Historically nginx was the sole writer; the
//       in-process ASP.NET middleware now also writes via the shared
//       RecordServeHit() helper / C API, and the 1.16 module (mod_pagespeed
//       1.1) is a third front-end writer through the same C API.  The
//       front-end architectures are mutually exclusive by construction (a
//       deployment runs exactly one front end pointed at a given cache), so
//       exactly one process writes these.
//       By-encoding rows (v9) belong to this group: written by the front end
//       inside RecordServeHit().
//       Serve-by-host slots (v10) belong to this group too: claimed and
//       counted by the front end inside RecordServeHit().
//   * serve-class counters + notify_suppressed_total (v8)
//       WRITER: the front end, through RecordServeClass() / the C API.
//       LIVE where the 1.16 module fronts the cache: its module-side writer
//       has landed (ps_serve_stats_record_serve_class).  No IN-TREE front end
//       classifies serves yet, so under an nginx 2.0 / ASP.NET front end
//       these still read 0; see the field comments.
//   * saturation group + worker_pool_threads (v8)
//       WRITER: the WORKER, and only the worker.  No front end ever writes
//       them, so single-writer-per-field survives even though
//       single-writer-per-file does not.
//
// The worker remains the sole CREATOR.  Even a misconfigured violation is
// non-catastrophic: all writers use relaxed atomic fetch_add on aligned 64-bit
// counters, so the worst case is cosmetic double-counting, never corruption or
// a torn read.
//
// Layout: fixed struct at offset 0 of the mmap'd kFileSize file.
// Path: {cache_parent}/.pagespeed-serve-stats
//
// All counter fields are updated with relaxed atomic operations
// (__atomic_fetch_add / __atomic_load_n with __ATOMIC_RELAXED), which is
// safe for multi-process counters on all supported platforms (x86-64,
// aarch64) because aligned 64-bit loads/stores are naturally atomic.
struct ServeStats {
  static constexpr uint32_t kMagic = 0x50533032;  // "PS02"
  static constexpr uint32_t kVersion = 11;
  // 1024 (was 512 through v8, 256 through v5, 128 through v3): v6 adds the opt-in
  // counter apparatus (per-signer slots, overflow, verify-latency buckets, a
  // boot identity and a counting-since day) — 192 bytes of additions overflow
  // the 96-byte headroom left in 256, so the file grows to 512. Safe because
  // the version bump already forces a fresh file (OpenServeStats rejects the
  // old version AND the old size, the creator unlinks and recreates).
  // v7 adds the zero-copy serve-barrier + bounded-stale counters
  // (five u64, 40 bytes) — still well inside 512; the version bump forces a
  // fresh file exactly like the earlier bumps.
  // v8 adds the serve-class + saturation block (88 bytes).  HEADROOM AFTER v8
  // IS 32 BYTES (four u64): the next appender that needs more than that must
  // grow kFileSize on the same version bump, exactly as v6 did when 256 ran
  // out.  The exact-size assert below pins the layout so an accidental reorder
  // or a mid-block insertion cannot slip past review.
  // v9 adds the by-transfer-encoding serve counters (two [4][3] arrays,
  // 192 bytes), past v8's 32-byte headroom, so the file grows to 1024 on
  // the same version bump.
  // v10 adds 64 host slots and the other triple (10,776 bytes), past v9's
  // 352-byte headroom, so the file grows to 16384 on the same version bump.
  // v11 adds the six response-header sidecar counters (48 bytes), inside
  // v10's headroom; the version bump forces a fresh file as before.
  // HEADROOM AFTER v11 IS 4888 BYTES.
  static constexpr size_t kFileSize = 16384;

  // Web Bot Auth opt-in counter (experimental): the maximum
  // number of distinct verified signer identities we track by keyid; the
  // (N+1)-th and beyond fall into webbotauth_other_verified_bots.
  static constexpr size_t kMaxCountedBots = 8;

  // Serve savings by host (v10): slots claimed first-come; see the v10 block.
  static constexpr size_t kHostSlots = 64;
  // A host name and its terminating NUL.  NormalizeServeHost accepts at most
  // kHostNameCapacity - 1 bytes.
  static constexpr size_t kHostNameCapacity = 128;
  struct HostSlot {
    uint64_t key_hash;  // FNV-1a-64 of the normalised name; 0 == free
    uint64_t ready;     // 1 once `name` is written; read with acquire order
    uint64_t hits;
    uint64_t original_bytes;
    uint64_t optimized_bytes;
    char name[kHostNameCapacity];  // NUL-terminated, NUL-padded
  };

  uint32_t magic;
  uint32_t version;

  // Per-type cumulative counters: original bytes (what origin sent) and
  // optimized bytes (what we served from cache) for worker-processed HITs.
  uint64_t html_original_bytes;
  uint64_t html_optimized_bytes;
  uint64_t css_original_bytes;
  uint64_t css_optimized_bytes;
  uint64_t js_original_bytes;
  uint64_t js_optimized_bytes;
  uint64_t image_original_bytes;
  uint64_t image_optimized_bytes;

  // Per-type hit counts.
  uint64_t html_optimized_hits;
  uint64_t css_optimized_hits;
  uint64_t js_optimized_hits;
  uint64_t image_optimized_hits;

  // SVG variants serve as ContentType::kImage (there is no kSvg type); they are
  // also counted here so svg.served reflects real serve-time HITs across both
  // front-ends.  Added in kVersion 2 (follow-up #455); the v1→v2 bump
  // self-heals existing files (OpenServeStats rejects v1, CreateServeStats
  // memsets fresh).
  uint64_t svg_optimized_hits;

  // Web Bot Auth (observe-only): request-time verdicts for
  // requests that CARRIED an RFC 9421 signature.  "verified" = the signature
  // validated (signed-agent or verified-bot verdict); "invalid" = signature
  // material was present but the request fail-closed to unknown.  That
  // covers web-bot-auth signatures that did not validate (bad key / expired
  // / tampered / profile violations) AND anything that could not be parsed
  // at all: bare draft-cavage `Signature` headers (fediverse/ActivityPub
  // deliveries, webhook signers) and RFC-8941-valid-but-unsupported
  // serializations land here too, because an unparseable header cannot be
  // attributed to any scheme (fail-closed posture pinned in #863 / t/102).
  // Only PARSEABLE non-web-bot-auth material is split out into
  // webbotauth_other_signature below.  Human requests (no signature) are
  // not counted.  nginx increments at classify time; the worker exposes
  // these via /v1/metrics.  Added in kVersion 3 — new fields go AFTER
  // these; the version bump self-heals existing files exactly like v1→v2.
  uint64_t webbotauth_signed_verified;
  uint64_t webbotauth_signed_invalid;

  // Signature material that is NOT web-bot-auth (parseable Signature-Input
  // but no member tagged "web-bot-auth" — e.g. a CDN or other RFC 9421
  // signing scheme).  Such requests are classified as if unsigned (never
  // "invalid"); this counter keeps them visible instead of silently ignored.
  // Added in kVersion 4.
  uint64_t webbotauth_other_signature;

  // RSL-CAP enforcement (experimental): request-time verdicts for
  // requests evaluated by the enforcement handler while it is enabled.
  // "authorized" = a valid token granting the required license/scope (request
  // passed through); "denied_401" = no valid licensed identity (no/malformed/
  // unknown-issuer/bad-signature/expired token); "denied_402" = a
  // cryptographically valid identity whose license/scope was not granted.  The
  // front-end never counts requests that skip the handler (enforcement off).
  // nginx increments at enforcement time; the worker exposes these via
  // /v1/metrics.  Added in kVersion 5 — new fields go AFTER these; the version
  // bump self-heals existing files exactly like v3→v4.
  uint64_t rslcap_authorized;
  uint64_t rslcap_denied_401;
  uint64_t rslcap_denied_402;

  // Web Bot Auth opt-in counter (experimental).  Added in
  // kVersion 6.  All fields below are 8-byte aligned so every u64 can be
  // updated with std::atomic_ref across the shared MAP_SHARED mapping (nginx
  // workers + worker).  The version+size bump self-heals existing v5 files
  // exactly like the earlier bumps (OpenServeStats rejects them, the creator
  // recreates fresh).  New fields go AFTER these (before boot_id, which stays
  // last).
  //
  // One per-signer slot: a stable, REPRODUCIBLE non-crypto hash of the RFC-9421
  // keyid and the count of verified requests attributed to it.  The hash is
  // plain UNSALTED FNV-1a-64 of the raw keyid bytes (see HashWebBotAuthKeyid);
  // it is deliberately reproducible so a downstream consumer can recognise a
  // known keyid even where that keyid is not in the operator registry.
  // kid_hash == 0 means the slot is empty; the one input that would hash to 0
  // is remapped to a fixed non-zero sentinel so a real keyid never collides
  // with "empty" (this is a 0-remap, NOT a cryptographic salt — the hash stays
  // reproducible).  Slots are claimed lock-free (CAS on kid_hash) and are
  // cross-process-safe.
  struct SignerSlot {
    uint64_t kid_hash;  // unsalted FNV-1a-64 of the keyid; 0 == empty slot
    uint64_t count;     // verified requests attributed to this keyid
  };
  SignerSlot webbotauth_signers[kMaxCountedBots];

  // Verified requests whose signer keyid did not fit in the kMaxCountedBots
  // slots (all slots already claimed by other keyids).
  uint64_t webbotauth_other_verified_bots;

  // Coarse verify-latency histogram for verified Web Bot Auth requests.
  // Buckets are cumulative-disjoint (each request lands in exactly one).
  uint64_t webbotauth_verify_latency_lt100us;
  uint64_t webbotauth_verify_latency_lt1ms;
  uint64_t webbotauth_verify_latency_lt10ms;
  uint64_t webbotauth_verify_latency_ge10ms;

  // Zero-copy serve-barrier verdicts, incremented by the front-end
  // (nginx) at emit/timer time on an aliased cache HIT.  Added in kVersion 7;
  // new fields go AFTER these (before counting_since_unix_day / boot_id).
  //   torn_aborts        — a strict renew saw kTorn (a write-buffer wrap
  //                        overwrote the borrowed region): the serve was
  //                        failed closed so a client never gets foreign bytes.
  //   proactive_copyouts — the aliased tail was de-aliased into request-owned
  //                        memory ahead of a wrap (kCopyNow / leases-off / a
  //                        forced-wrap deadline within the safety margin).
  //   copy_then_verify_discards — a wrap raced the de-alias copy and the
  //                        post-copy re-check saw kTorn: the copy was
  //                        discarded and the serve failed closed.
  uint64_t zerocopy_torn_aborts;
  uint64_t zerocopy_proactive_copyouts;
  uint64_t zerocopy_copy_then_verify_discards;

  // Bounded stale-while-revalidate telemetry (issue #652), incremented by the
  // front-end.  swr_coalesced_serves = a refresh coalesced onto another
  // request's in-flight re-fetch; stale_if_error_serves = an upstream >= 500
  // answered with the stashed stale entry.
  uint64_t swr_coalesced_serves;
  uint64_t stale_if_error_serves;

  // ---- v8: serve-class + saturation block ----
  //
  // SERVE-CLASS COUNTERS (front end; live where the 1.16 module fronts the
  // cache — no in-tree writer yet).
  // For every response the front end classifies as optimizable, EXACTLY ONE of
  // the five below increments; responses it does not classify increment none.
  // That partition is what lets a reader compute
  //   original_served_fraction = (cold + pending + declined + skew) / all five
  // and defend the number.  Write them only through RecordServeClass(), whose
  // single-class parameter is what enforces the partition.
  //
  // These five and notify_suppressed_total are written by the 1.16 module
  // (mod_pagespeed 1.1), which classifies serves through the C API
  // (ps_serve_stats_record_serve_class); no IN-TREE front end classifies
  // serves yet, so under an nginx 2.0 / ASP.NET front end they still read 0.
  // A reader must keep treating all-zero as "not instrumented", not as
  // "nothing happened".  The saturation group below IS live and
  // disambiguates the two: a non-zero worker_pool_threads proves the worker
  // half of this block is being written.
  //
  //   serve_optimized_total       — served from a worker-produced alternate.
  //                                 Deliberately redundant with the per-type
  //                                 *_optimized_hits above: a reader needs a
  //                                 numerator sharing a writer and a clock
  //                                 with the four denominators, and the
  //                                 comparison against the legacy hits is what
  //                                 detects a front end that is mapped but not
  //                                 instrumented.  CAVEAT under the 1.16
  //                                 module writer: a 304 revalidation of an
  //                                 optimized variant counts the class but
  //                                 records no per-type hit (no bytes served),
  //                                 so there serve_optimized_total >=
  //                                 sum(per-type *_optimized_hits) — the two
  //                                 drift apart by exactly the 304 rate.
  //                                 Equality holds only where the writer has
  //                                 no client-facing 304 hit path (nginx).
  //   serve_original_cold_total   — served origin bytes; no entry at all for
  //                                 the key (never asked for, or forgotten
  //                                 across a restart).
  //   serve_original_pending_total— served origin bytes; an entry exists but no
  //                                 acceptable alternate yet.  THIS is the
  //                                 load-sensitive one.
  //   serve_original_declined_total — served origin bytes; a negative verdict
  //                                 is recorded for the key.  No negative
  //                                 verdict is representable in the entry
  //                                 schema yet, so this one stays 0 even once
  //                                 the front end is instrumented, and its
  //                                 events land in pending.  Said plainly
  //                                 rather than left for a reader to discover.
  //   serve_original_skew_total   — served origin bytes because a version /
  //                                 handshake fail-safe fired.  Split out
  //                                 because skew is an incident, not load, and
  //                                 must never be laundered into a
  //                                 back-pressure numerator.
  uint64_t serve_optimized_total;
  uint64_t serve_original_cold_total;
  uint64_t serve_original_pending_total;
  uint64_t serve_original_declined_total;
  uint64_t serve_original_skew_total;

  // A serve that would have asked the worker to optimize but did not, because
  // the front end's own cooldown/dedup suppressed the request.  Distinguishes
  // "the worker is behind" from "we stopped asking".  ORTHOGONAL to the five
  // classes above — a suppressed notify still has exactly one serve class.
  // Same caveat as the block above: written by the 1.16 module, no in-tree
  // writer yet.
  uint64_t notify_suppressed_total;

  // What RecordServeClass could NOT interpret.  Dropping an unintelligible
  // write is right; dropping it without a trace is not — the five counters
  // above are a partition, and a partition that quietly loses members reads
  // exactly like a healthy one.  These two make that visible from inside the
  // surface, without a log line on the serve path.
  //
  // They are TWO counters because they mean opposite things and merging them
  // would make the first non-zero reading unattributable:
  //
  //   serve_class_unrecognized_total — a write was DROPPED: its class was not
  //     one of the five (a garbled value, two classes combined, or a class a
  //     newer peer knows and this build does not).  The partition is now
  //     INCOMPLETE: the five no longer sum to the responses the front end
  //     classified, and any fraction computed from them is understated by this
  //     count.  Non-zero is an anomaly and a reader should say so.
  //   serve_flags_unrecognized_total — a write was ACCEPTED with its class
  //     counted, and only unknown FLAG bits were ignored.  The partition is
  //     intact; the reader is simply older than the writer.  This is the
  //     designed forward-compatibility path, an expected event in a mixed
  //     fleet, not a fault.
  //
  // One write increments at most one of them: a dropped write is counted only
  // as a drop, because its flags were never interpreted either and reporting
  // both would double-report a single bad call.
  //
  // Same caveat as the counters above: RecordServeClass is their only writer,
  // reached through the C API by the 1.16 module; no in-tree front end calls
  // it yet.  They cost 16 of the block's bytes, leaving 32 (four u64) of
  // headroom in kFileSize — see the note at
  // kFileSize before appending anything else.
  uint64_t serve_class_unrecognized_total;
  uint64_t serve_flags_unrecognized_total;

  // SATURATION GROUP (worker; LIVE).  The worker samples its in-flight work
  // count on an existing periodic timer and accumulates it here, so a reader
  // can compute a true MEAN backlog over any interval by differencing two
  // reads — independent of how often the reader itself polls.  A bare
  // instantaneous gauge would be only as good as the reader's sampling rate,
  // which is why the integral is what lives here.
  //
  // READ ORDER MATTERS: read saturation_sample_count FIRST, then
  // saturation_sample_accum.  The two are independent u64s with no
  // cross-field atomicity; taking count first bounds the error at one sample
  // and biases the resulting mean HIGH, which is the safe direction for a
  // saturation reading.  A seqlock would be overkill for a telemetry mean.
  //
  // The sample interval is the worker's event-loop lag timer: ONE SAMPLE PER
  // SECOND.  Stated so a reader can sanity-check the sample count against a
  // wall-clock delta: a starved loop thread under-samples, and under-sampling
  // UNDERSTATES saturation — the dangerous direction.  The lag gauge published
  // on /v1/metrics measures exactly that starvation, so the two cross-check.
  uint64_t saturation_sample_accum;
  uint64_t saturation_sample_count;

  // Pool width the worker is running with, so a reader can normalise
  // (accum/count) into a saturation ratio without a second API call.  A THIRD
  // field category, distinct from both "counter" and "minted-once identity":
  // CONFIGURATION.  Preserved by ZeroCounters like an identity field, but
  // RE-STAMPED on every reuse unlike one, because the pool width can change
  // across a restart with a changed config.  Never legitimately 0 while the
  // worker is running, which is what makes it the liveness probe for this
  // whole block.
  uint32_t worker_pool_threads;

  // High-water mark of the sampled in-flight count since this file was created
  // (a max, not a last value).  Zeroed by ZeroCounters with the counters.
  uint32_t saturation_hwm;
  // ---- end v8 block (88 bytes) ----

  // ---- v9: serves by transfer encoding ----
  // The same serves the per-type counters above total, split by the
  // transfer-encoding axis of the served variant's capability mask (bits
  // 6-7: identity, gzip, brotli), so a front end that never serves a stored
  // compressed variant shows an all-identity row instead of an unexplained
  // saving gap.  Index [type][encoding]: type is the ContentType value
  // (html=0..image=3), encoding is CapabilityMask::TransferEncoding
  // (identity=0, gzip=1, brotli=2).  A mask whose encoding field is
  // kReserved (3) writes no row here -- not a servable encoding -- while
  // the per-type totals still count the serve.  WRITER: the front end, with
  // the bytes/hits group above, through RecordServeHit().
  uint64_t serve_hits_by_encoding[4][3];
  uint64_t serve_bytes_by_encoding[4][3];
  // ---- end v9 block (192 bytes) ----

  // ---- v10: serve savings by host ----
  // The same serves the per-type counters above total, attributed to the host
  // the front end served them for.  A slot is claimed by a compare-and-swap
  // of key_hash from 0, probing linearly from key_hash % kHostSlots, exactly
  // like the signer slots; the claimer writes `name` and then stores
  // ready = 1 with release order, and a reader trusts the name only after
  // reading ready == 1 with acquire order, re-validating it, and checking
  // that the hash of the stored name equals the stored key_hash -- so a name
  // torn by a ZeroCounters racing a claim is never reported.  A serve with
  // no host, a host NormalizeServeHost refuses, or a host that finds every
  // slot taken counts in the other triple, so the slots plus the other triple
  // always add up to the per-type totals over HTML, CSS, JS and images.
  // WRITER: the front end, through RecordServeHit().  Slots are released only
  // by ZeroCounters (a worker restart), so they fill first-come from the
  // first 64 distinct hosts seen since then.  A ZeroCounters racing a claim
  // or a counter add can leave one slot unnamed (its serves then read as
  // other), or leave a few counts in a freed slot that the next claimer
  // inherits -- cosmetic until the next restart, never corruption.  A
  // ZeroCounters racing a probe can leave one host in two slots; the reader
  // merges slots with the same name, so the host is still reported once.
  HostSlot serve_hosts[kHostSlots];
  uint64_t serve_hosts_other_hits;
  uint64_t serve_hosts_other_original_bytes;
  uint64_t serve_hosts_other_optimized_bytes;
  // ---- end v10 block (10,776 bytes) ----

  // ---- v11: response-header sidecar (issue #208) ----
  // WRITER: the nginx front stage.  The gate is ClassifyForHeadersSidecar
  // (lib/cache/headers_sidecar.h), run on the origin's header lines when a
  // 200 is about to be recorded.
  //   headers_sidecar_fall_through   — responses not cached because a header
  //                                    could not be reproduced on a hit;
  //   headers_sidecar_never_optimized — responses not cached because their
  //                                    Content-Security-Policy carries a
  //                                    nonce (disjoint from the above);
  //   headers_sidecar_replayed       — cache hits that replayed at least one
  //                                    stored header line;
  //   headers_sidecar_missing        — cache hits refused (served as a miss)
  //                                    because the entry requires a stored
  //                                    header block that could not be read;
  //   headers_sidecar_write_failed   — records abandoned because the header
  //                                    block could not be written (the entry
  //                                    is then not written either);
  //   headers_sidecar_stored         — header blocks stored with an entry
  //                                    (records and 304 revalidations).
  uint64_t headers_sidecar_fall_through;
  uint64_t headers_sidecar_never_optimized;
  uint64_t headers_sidecar_replayed;
  uint64_t headers_sidecar_missing;
  uint64_t headers_sidecar_write_failed;
  uint64_t headers_sidecar_stored;
  // ---- end v11 block (48 bytes) ----

  // Days since the Unix epoch when this counting file was first created fresh.
  // Day-granular ("since" date).  Minted in the create-fresh path only;
  // PRESERVED across same-version reuse/restart (NOT a counter).
  uint64_t counting_since_unix_day;

  // Random 16-byte INSTANCE identity for this counting file.  Despite the
  // "boot" name it is NOT a per-process boot id: it is minted only on a fresh
  // file create and is STABLE across ordinary same-version worker restarts
  // (which reuse the file and reset counters but leave this untouched).  It
  // changes only when the file is recreated: a version bump, an explicit
  // clear, or file deletion.  This is intended — distinct ids across concurrent
  // origin responses indicate multiple instances / a load balancer, while a
  // single restarting instance keeps one id.  A counter RESET is therefore
  // detectable via a cumulative-total DECREASE, not via this id changing.
  // Identity, NOT a counter — PRESERVED across reuse and ZeroCounters.  Kept
  // LAST in the struct.
  uint8_t boot_id[16];

  // ---- split block: the saving divided into encoding and rewrite ----
  // Appended AFTER boot_id, inside the same kFileSize and WITHOUT a version
  // bump: no existing field moves, so a peer built before it keeps reading and
  // writing every field it knew, and a file created by such a peer simply has
  // this block all zero.  (The identity/epoch pair used to be the last two
  // fields as a convention; nothing depends on that -- readers address
  // boot_id by member -- and the assert below keeps both ahead of this block.)
  //
  // For the serves of each counted type whose encoded-origin size was known
  // (RecordServeHitSplit), the sum of their origin bytes, of those bytes
  // encoded the way the serve was encoded, and of the bytes served:
  //   encoding saved = original - compressed_original
  //   rewrite saved  = compressed_original - optimized
  // The two add up to original - optimized for exactly those serves.  A serve
  // recorded without the encoded size counts in the per-type totals above but
  // not here, so split_original / original is the share the split covers; 0
  // means the writer predates this block or no entry carried the size.
  // WRITER: the front end, with the bytes/hits group, through
  // RecordServeHitSplit().
  struct SplitCounters {
    uint64_t original_bytes;
    uint64_t compressed_original_bytes;
    uint64_t optimized_bytes;
  };
  SplitCounters split_html;
  SplitCounters split_css;
  SplitCounters split_js;
  SplitCounters split_image;
  // ---- end split block (96 bytes) ----

  // ---- busy-read block ----
  // Appended after the split block the same way: no version bump, no field
  // moves, and a file created by a peer built before it has the block zero.
  // WRITER: the front end and the worker, through RecordCacheBusyRead(), from
  // the cache's Busy-read observer (PageSpeedCache::SetBusyReadObserver).
  //   cache_busy_reads      — cache reads that found the entry locked by a
  //                           concurrent write for the whole read budget, and
  //                           so were retried once;
  //   cache_busy_retry_hits — those retries that then found the entry (the
  //                           rest were served as a miss, as before).
  uint64_t cache_busy_reads;
  uint64_t cache_busy_retry_hits;
  // ---- end busy-read block (16 bytes) ----
};
static_assert(sizeof(ServeStats) <= ServeStats::kFileSize);
// Pin the EXACT size (v11 plus the split and busy-read blocks).  The <= assert above only catches an overflow; it
// would happily accept a field silently dropped or re-typed.  Anything that
// changes the layout must change this number and the version together.
static_assert(sizeof(ServeStats) == 11608,
              "ServeStats layout changed: bump kVersion and update this pin");
static_assert(alignof(ServeStats) == 8);
// The v8 block must stay 8-byte aligned so every u64 in it can be updated with
// std::atomic_ref across the shared mapping.
static_assert(offsetof(ServeStats, serve_optimized_total) % 8 == 0);
static_assert(offsetof(ServeStats, serve_class_unrecognized_total) % 8 == 0);
static_assert(offsetof(ServeStats, saturation_sample_accum) % 8 == 0);
static_assert(offsetof(ServeStats, worker_pool_threads) % 4 == 0);
// The ordering rule, made executable: the identity/epoch pair stays last.
static_assert(offsetof(ServeStats, saturation_hwm) <
              offsetof(ServeStats, counting_since_unix_day));
static_assert(offsetof(ServeStats, counting_since_unix_day) <
              offsetof(ServeStats, boot_id));
// The split block follows the identity pair, so every offset a pre-split peer
// knows is unchanged.
static_assert(offsetof(ServeStats, split_html) >=
              offsetof(ServeStats, boot_id) + sizeof(ServeStats::boot_id));
static_assert(offsetof(ServeStats, split_html) % 8 == 0);
static_assert(offsetof(ServeStats, cache_busy_reads) >=
              offsetof(ServeStats, split_image) +
                  sizeof(ServeStats::SplitCounters));
static_assert(offsetof(ServeStats, cache_busy_reads) % 8 == 0);
static_assert(offsetof(ServeStats, serve_hits_by_encoding) % 8 == 0);
static_assert(offsetof(ServeStats, serve_bytes_by_encoding) % 8 == 0);
static_assert(offsetof(ServeStats, serve_bytes_by_encoding) <
              offsetof(ServeStats, counting_since_unix_day));
static_assert(sizeof(ServeStats::HostSlot) == 168);
static_assert(offsetof(ServeStats, serve_hosts) % 8 == 0);
static_assert(offsetof(ServeStats, serve_bytes_by_encoding) <
              offsetof(ServeStats, serve_hosts));
static_assert(offsetof(ServeStats, serve_hosts_other_optimized_bytes) <
              offsetof(ServeStats, counting_since_unix_day));

// One serve-class outcome for a response the front end judged optimizable.
// Values are DISJOINT SINGLE BITS on purpose: a caller that accidentally
// combines two classes produces a value that is not a class at all, so
// RecordServeClass drops it instead of attributing the serve to a third class
// (which is what consecutive small integers would have done).  A serve has
// exactly one class; the type carries no way to express two.
// The underlying type is deliberately wider than the value set: a class that
// arrives unrecognised (from the C boundary, say) must stay unrecognised, and a
// narrower type would truncate it onto a valid class — the mis-attribution the
// disjoint bits exist to prevent.
// NOLINTNEXTLINE(performance-enum-size)
enum class ServeClass : uint32_t {
  kOptimized = 1u << 0,
  kOriginalCold = 1u << 1,
  kOriginalPending = 1u << 2,
  kOriginalDeclined = 1u << 3,
  kOriginalSkew = 1u << 4,
};

// Flags orthogonal to the serve class, OR-ed together.  Unknown bits are
// ignored so a newer front end can pass a flag an older library does not know.
inline constexpr uint32_t kServeFlagNone = 0;
inline constexpr uint32_t kServeFlagNotifySuppressed = 1u << 0;
// Every flag bit this build understands.  Anything outside it is ignored and
// counted in serve_flags_unrecognized_total; keep it in step when adding a flag.
inline constexpr uint32_t kServeFlagKnownMask = kServeFlagNotifySuppressed;

// Derive the serve-stats file path from the cache path.
// Returns "{parent_of(cache_path)}/.pagespeed-serve-stats".
std::string ServeStatsPath(const std::string& cache_path);

// Create or reset the serve-stats file and return a pointer to the mmap'd
// region.  If a valid file already exists, reuses the same inode (zeroing
// only counters) so that nginx's existing mmap stays valid across worker
// restarts.  Creates a new file only when missing or invalid.
// Returns nullptr on failure.  Caller must call CloseServeStats() to unmap.
//
// When `handler` is supplied and an existing file is discarded because it
// carries a different version, logs ONE warning naming both versions.  A
// version bump is otherwise an invisible event with two visible-only-later
// consequences: the file's counters restart from zero, and any peer built
// against the old version stops recording (its open fails closed and every
// Record* call becomes a no-op).  Both show up downstream as flat lines with
// no error, so the skew window is announced here instead.
ServeStats* CreateServeStats(const std::string& path,
                             MessageHandler* handler = nullptr);

// Open an existing serve-stats file (read-write) and validate magic+version.
// Returns nullptr if the file doesn't exist, is too small, or has wrong
// magic/version.  Caller must call CloseServeStats() to unmap.
ServeStats* OpenServeStats(const std::string& path);

// Unmap a previously mapped serve-stats region.
void CloseServeStats(ServeStats* stats);

// Atomically record one worker-processed serve HIT for content_type.
// Single source of truth for nginx and the C API.  No gate here (callers
// gate on worker-processed && original_bytes > 0).  kOther / unknown
// content_type is a no-op.  Uses __atomic_fetch_add with __ATOMIC_RELAXED,
// matching the existing nginx increments.  stats == nullptr is a no-op.
//
// `mask` is the served variant's full capability mask.  For kImage hits whose
// image-format bits == kSvg (0x03) the SVG-served counter is also bumped
// (svg.served).  Defaults to 0 so legacy call sites that do
// not thread a mask keep compiling (and simply never tick svg_optimized_hits).
void RecordServeHit(ServeStats* stats, ContentType content_type,
                    uint64_t original_bytes, uint64_t optimized_bytes,
                    uint32_t mask = 0);

// RecordServeHit() with the host, plus the split of the saving: the serve's
// origin bytes encoded the way it was served (see the split block).  Counts
// exactly what the host form counts and, for the four counted content types,
// adds the serve to that type's split counters.  A front end that does not
// know the encoded size calls the host form instead.
void RecordServeHitSplit(ServeStats* stats, ContentType content_type,
                         uint64_t original_bytes,
                         uint64_t compressed_original_bytes,
                         uint64_t optimized_bytes, uint32_t mask,
                         std::string_view host);

// One counted type's split counters as a reader sees them.  Zero for any
// other type and for stats == nullptr.
ServeStats::SplitCounters ReadServeSplit(const ServeStats* stats,
                                         ContentType content_type);

// One host's share of the serve savings, as ReadServeSavingsByHost reports it.
struct ServeHostRow {
  std::string host;  // empty on the "other" row
  uint64_t hits = 0;
  uint64_t original_bytes = 0;
  uint64_t optimized_bytes = 0;
};

// The by-host block as a reader sees it.  `hosts` holds at most the requested
// number of rows, most hits first (ties by name); `other` holds every other
// serve: the remaining slots, the serves that found no free slot, and the
// serves recorded without an acceptable host.
struct ServeSavingsByHost {
  std::vector<ServeHostRow> hosts;
  ServeHostRow other;
};

// How many hosts GET /v1/stats and /v1/metrics name; the rest are "other".
inline constexpr size_t kServeHostReportLimit = 32;

// THE HOST-NAME RULE for serve savings per host -- its one statement in the
// code; the HTTP API reference (serve_savings_by_host) and the /v1/metrics
// help text repeat it in words.  NormalizeServeHost turns the host a front
// end names into the name a row is reported under:
//   - ASCII letters are lowercased; no other byte is changed;
//   - a ":port" of 1-5 digits is dropped -- after the closing "]" of a
//     bracketed IPv6 literal;
//   - exactly one trailing "." is dropped.
// What is left is a host name when it is 1..kHostNameCapacity-1 (127)
// bytes with at least one letter or digit, and either
//   - only letters, digits, ".", "-" and "_" (a DNS name, or an IPv4
//     address), or
//   - "[", then hexadecimal digits, ":" and ".", then "]" (an IPv6 literal;
//     an IPv6 address without brackets is not a host name).
// Anything else -- empty, longer, any other byte (space, quote, "*", "/",
// non-ASCII), a malformed bracket, a name still ending in "." -- is not a
// host name: NormalizeServeHost returns false (`out` is then unspecified),
// the serve counts under "other", and the value is never stored or
// reported.  A name this returns is a fixed point of it, which the reader
// relies on (ReadServeSavingsByHost re-validates every stored name); every
// reported name is therefore safe as a JSON string and a Prometheus label
// value.
bool NormalizeServeHost(std::string_view host, std::string* out);

// Atomically attributes ONE serve's bytes to `host` (see the v10 block): a
// refused host, or no free slot, counts in the other triple.  Same
// relaxed-atomic, cross-process discipline as the other Record* helpers;
// stats == nullptr is a no-op.  RecordServeHit() calls it; public for the
// tests and the golden capture.
void RecordServeHostHit(ServeStats* stats, std::string_view host,
                        uint64_t original_bytes, uint64_t optimized_bytes);

// RecordServeHit() with the host the front end served the response for:
// counts exactly what the form above counts and, for the four counted content
// types, attributes the serve to `host`.  The form above attributes the
// serve to "other".
void RecordServeHit(ServeStats* stats, ContentType content_type,
                    uint64_t original_bytes, uint64_t optimized_bytes,
                    uint32_t mask, std::string_view host);

// Reads the by-host block: every claimed, ready slot whose name re-validates
// and hashes to the slot's key_hash, sorted by hits (most first, ties by
// name), at most `limit` of them; the rest folded into `other`.
// stats == nullptr reads as empty.
ServeSavingsByHost ReadServeSavingsByHost(const ServeStats* stats,
                                          size_t limit);

// Atomically record one Web Bot Auth classification of a SIGNED request
// (observe-only telemetry).  `verified` == the signature
// validated.  Same relaxed-atomic discipline as RecordServeHit; stats ==
// nullptr is a no-op.  Callers never invoke this for signature-less (human)
// requests, nor for non-web-bot-auth signature material (see
// RecordWebBotAuthOtherSignature).
void RecordWebBotAuthSigned(ServeStats* stats, bool verified);

// Atomically record one request that carried signature material with NO
// web-bot-auth-tagged signature (classified as if unsigned).  Same
// relaxed-atomic discipline; stats == nullptr is a no-op.
void RecordWebBotAuthOtherSignature(ServeStats* stats);

// Stable, REPRODUCIBLE non-crypto hash of an RFC-9421 keyid string used as the
// per-signer slot key.  Plain UNSALTED FNV-1a-64 over the raw
// keyid bytes (offset basis 0xcbf29ce484222325, prime 0x100000001b3), with the
// single input that would hash to 0 remapped to a fixed non-zero sentinel so a
// real keyid never collides with the "empty slot" value 0.  It is intentionally
// reproducible by anyone (no secret salt): it is the only handle a downstream
// consumer has to identify a specific signer at an origin where that keyid is
// not in the operator registry.  Exposed for the reader side (nginx endpoint)
// to join registry names against slots by hash.
uint64_t HashWebBotAuthKeyid(std::string_view keyid);

// Atomically record one VERIFIED Web Bot Auth request against its signer
// (opt-in counter, experimental).  Finds-or-claims the slot for
// `keyid` lock-free (CAS on kid_hash) and increments its count; if all
// kMaxCountedBots slots are taken by other keyids, increments
// webbotauth_other_verified_bots instead.  Also buckets `latency_us` into the
// coarse verify-latency histogram.  Same relaxed-atomic, cross-process
// discipline as the other Record* helpers; stats == nullptr is a no-op.  This
// is ADDITIONAL to RecordWebBotAuthSigned(stats, true) — callers invoke both on
// a verified verdict.  Records ALL verified keyids (including our own probe
// key); probe-kid / first-party exclusion is an aggregator-side concern.
void RecordWebBotAuthVerifiedSigner(ServeStats* stats, std::string_view keyid,
                                    uint64_t latency_us);

// Atomically record one zero-copy serve-barrier verdict, incremented
// by the front-end on an aliased cache HIT.  Same relaxed-atomic, cross-process
// discipline as the other Record* helpers; stats == nullptr is a no-op.
void RecordZerocopyTornAbort(ServeStats* stats);
void RecordZerocopyProactiveCopyout(ServeStats* stats);
void RecordZerocopyCopyThenVerifyDiscard(ServeStats* stats);

// Atomically record one bounded stale-serve event (issue #652): a refresh
// coalesced onto an in-flight re-fetch, or a stale entry served on upstream
// error.  Same discipline; stats == nullptr is a no-op.
void RecordSwrCoalescedServe(ServeStats* stats);
void RecordStaleIfErrorServe(ServeStats* stats);

// Atomically record one response-header sidecar event (v11, issue #208),
// incremented by the nginx front stage.  Same discipline; stats == nullptr is
// a no-op.  See the v11 block for what each one counts.
void RecordHeadersSidecarFallThrough(ServeStats* stats);
void RecordHeadersSidecarNeverOptimized(ServeStats* stats);
void RecordHeadersSidecarReplayed(ServeStats* stats);
void RecordHeadersSidecarMissing(ServeStats* stats);
void RecordHeadersSidecarWriteFailed(ServeStats* stats);
void RecordHeadersSidecarStored(ServeStats* stats);

// Atomically record one cache read that came back Busy and was retried once
// (the busy-read block): cache_busy_reads always, cache_busy_retry_hits too
// when `retry_hit`.  Same discipline; stats == nullptr is a no-op.
void RecordCacheBusyRead(ServeStats* stats, bool retry_hit);

// Atomically record ONE classified serve: exactly one serve-class counter is
// incremented, plus notify_suppressed_total when `flags` carries
// kServeFlagNotifySuppressed.  Same relaxed-atomic, cross-process discipline as
// the other Record* helpers; stats == nullptr is a no-op.
//
// A `cls` that is not one of the five enumerated classes — including a value
// produced by combining two of them — records no serve class, and instead
// increments serve_class_unrecognized_total so the dropped write is visible.
// Unknown flag bits do not cost the serve its class; they increment
// serve_flags_unrecognized_total.  Neither case logs: this is the serve path,
// and the counter is the trace.
//
// One call per classified serve.  Callers must not call it twice for one
// response: the partition is a contract about responses, and this function can
// only enforce the half it can see (one class per call).
void RecordServeClass(ServeStats* stats, ServeClass cls,
                      uint32_t flags = kServeFlagNone);

// Record ONE saturation sample: adds `in_flight` to the accumulator, advances
// the sample count, and raises the high-water mark if this sample exceeds it.
// Called by the WORKER only, from its periodic timer.  The high-water mark is
// published with a relaxed compare-exchange loop; a lost race costs at most one
// sample's contribution to the mark.  stats == nullptr is a no-op.
void RecordSaturationSample(ServeStats* stats, uint32_t in_flight);

// Stamp the worker's pool width (configuration, not a counter — see the field
// comment).  Called by the WORKER after create-or-reuse, so the value tracks a
// config change across a restart.  stats == nullptr is a no-op.
void SetWorkerPoolThreads(ServeStats* stats, uint32_t threads);

// Atomically record one RSL-CAP enforcement verdict.  `http_status`
// is the handler's outcome: 0 = authorized (passed through), 401 = no valid
// licensed identity, 402 = valid identity but license/scope ungranted.  Same
// relaxed-atomic discipline; stats == nullptr is a no-op.  Any other status is
// a no-op (the handler only ever emits 0/401/402).
void RecordRslCapVerdict(ServeStats* stats, int http_status);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_SERVE_STATS_H_
