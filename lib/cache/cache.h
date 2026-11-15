// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — C++ cache API using Cyclone alternates.
//
// Replaces cache_wrapper.h. Uses Cyclone's native alternate selection
// instead of prefix-key based variant lookup.
//
// IMPORTANT: ReadHandles hold references to memory-mapped regions owned
// by the underlying Cyclone cache. All ReadHandles must be destroyed
// before the PageSpeedCache is destroyed.

#ifndef PAGESPEED_LIB_CACHE_CACHE_H_
#define PAGESPEED_LIB_CACHE_CACHE_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/handle.hpp"
#include "cyclone/key.hpp"
#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/hostname.h"
#include "lib/classify/pagespeed_selector.h"

namespace pagespeed {

class MessageHandler;

// Result of the intent-checked lease renewal a zero-copy embedder must call
// before each aliased send of a borrowed mmap region.  Re-exported
// from Cyclone so serve-path callers name it as pagespeed::LeaseRenewal
// without reaching into the cyclone namespace: kOk keep aliasing, kCopyNow /
// kLeasesOff de-alias by copying, kTorn abort the serve.
using LeaseRenewal = cyclone::LeaseRenewal;

// Configuration for PageSpeedCache.
struct PageSpeedCacheConfig {
  std::string volume_path;
  uint64_t volume_size = 1ULL * 1024 * 1024 * 1024;  // 1GB default
  bool enable_checksum = true;
  bool verify_checksum_on_read = true;

  // Multi-process sharing (per Step 1c: always enabled with
  // process_index=0, total_processes=1 for mmap directory sharing).
  cyclone::MultiProcessConfig multi_process = {
      .enabled = true,
      .process_index = 0,
      .total_processes = 1,
  };

  // RAM cache size (bytes). Set to 0 to disable.
  size_t ram_cache_size = 64 * 1024 * 1024;  // 64 MB

  // Maximum metadata blob size (bytes). Rejects writes with larger
  // metadata to prevent abuse. [F35]
  //
  // Derived, not chosen: see the prefix-budget block in
  // lib/classify/alternate_metadata.h.  The worst case the format can
  // produce with every member at its budget is 750 B; this default is that
  // plus 128 B of reserve.  A write over the ceiling is REFUSED whole (the
  // entry is not stored), so a ceiling below the format's worst case is a
  // cache that silently declines to keep entries with long headers.
  size_t max_metadata_size = AlternateMetadata::kRecommendedMaxMetadataSize;

  // Cap on the CONTENT length of a durable original-content entry, in bytes.
  // 0 selects kDefaultMaxOriginalContentLength (16 MiB) — deliberately, so
  // that a zero-initialised config gets the shipped policy rather than a cap
  // of nothing.  There is no "unlimited" setting; see the constant.
  //
  // Applies ONLY to WriteOriginalAlternate.  Optimized variants are bounded
  // by what the optimizer produced from an original that already passed this
  // cap; it is the ORIGINAL, whose size is whatever the origin sent, that
  // needs a ceiling.
  uint64_t max_original_content_length = 0;

  // Lease-based region pinning.  Every
  // disk-borrow read stamps a per-stripe lease of read_lease_duration (T);
  // writers defer circular write-buffer wraps while a lease is live, so
  // borrowed mmap bytes are never overwritten in place under a live
  // ReadHandle.  The re-stamp avoidance guard makes 3T/4 the guaranteed
  // protection floor: holders keeping a borrow longer MUST call
  // ReadResult::renew_lease() at a cadence <= 3T/4.  Wrap deferral is
  // bounded by lease_wrap_ceiling: past it the wrap is FORCED (counted in
  // cyclone stats wraps_forced_past_lease) and the borrow is unprotected
  // again.  read_lease_duration = 0 disables leases (the pre-lease
  // behavior); tests with tiny volumes and read-then-flood patterns may
  // need that to avoid wrap deferral.  Defaults mirror Cyclone's.
  std::chrono::milliseconds read_lease_duration{5000};
  std::chrono::milliseconds lease_wrap_ceiling{60000};

  // Optional diagnostic logger. When set, cache operations log through
  // this handler (read misses, writes, errors). Not owned.
  MessageHandler* handler = nullptr;
};

// Default cap on a durable original's content, in bytes.
//
// 16 MiB.  Above that size, keeping a copy of the origin's bytes costs more
// than the rebuild it saves.  The number is not invented here: it is the same
// ceiling the 1.x line has long used for cacheable responses
// (RewriteOptions::kDefaultMaxCacheableResponseContentLength), which matters
// because two writers into one volume that disagree about which objects are
// worth keeping produce a cache whose contents depend on who got there first.
// 2.0 exposes no directive of that name — this is a library constant, not a
// setting reachable from a config file.
//
// A response larger than this is not stored as a durable original; it is not
// an error, and nothing else about the response changes.
//
// CURRENT REACH, stated so the advisory argument below is not read as bigger
// than it is: no shipped path sets max_original_content_length, so today this
// IS the cap in every deployment and a mismatch cannot arise.  The advisory
// shape is what the cap has to be WHEN a second writer can set its own; it is
// not a description of a live divergence.
//
// WHY THE CAP IS ENFORCED WHILE STREAMING rather than measured afterwards:
// the storage layer accumulates a value's whole content in a process-heap
// buffer and commits only at close, so an uncapped original is an unbounded
// resident-memory spike, and a size measured after the fact has already cost
// what the cap exists to prevent.  Refusing to accumulate past the ceiling is
// the only form of the check that does anything.
//
// WHY IT IS ADVISORY: a peer configured with a different cap costs a SKIPPED
// STORE — an entry that is simply not durably kept — which is neither silent
// nor corrupting, and self-corrects the moment the caps agree.  It is
// therefore NOT a mirror to refuse to start over; that severity belongs to
// divergences that produce a split view of the same bytes.
inline constexpr uint64_t kDefaultMaxOriginalContentLength = 16 * 1024 * 1024;
static_assert(kDefaultMaxOriginalContentLength == 16777216);

// The default ceiling must cover what the serializer can actually emit.  This
// is the compile-time half of the prefix budget: the arithmetic lives with the
// format, and the config default is bound to it here so the two cannot drift.
static_assert(PageSpeedCacheConfig{}.max_metadata_size >=
                  AlternateMetadata::kMaxSerializedSizeAtBudget,
              "max_metadata_size default is below the metadata format's "
              "worst case — writes with long origin headers would be "
              "refused; re-derive the budget in alternate_metadata.h");

// Did a failed unlink of an alternate leave the entry STILL LINKED?
//
// The distinction matters wherever a writer unlinks before writing, because
// the two outcomes need opposite handling and only one of them is benign:
//
//   NOT LINKED — NotFound / AlternateNotFound.  There was nothing to remove.
//                Indistinguishable from "removed" for everything downstream,
//                and the ordinary case on the first store for a URL.
//   STILL LINKED — everything else.  `Busy` (repoint retries exhausted, which
//                a middle-of-chain node can genuinely hit), `ChainCorrupted`,
//                `NotOwned` (multi-process: this process does not own the
//                stripe), and the I/O-shaped failures.  The old node survives,
//                so the write that follows GROWS the chain.
//
// Written as a predicate over the error rather than as a branch at the call
// site because "which errors mean it is still there" is the part a reader has
// to get right, and it is worth being able to test it against every value the
// storage layer can return.
bool UnlinkLeftEntryLinked(cyclone::CacheError error);

// Result of a read operation — wraps Cyclone ReadHandle.
//
// Metadata (AlternateMetadata) is stored as a content prefix rather than
// via Cyclone's per-alternate set_header().  This keeps the wire format
// under our control (v3→v4 evolution without Cyclone changes) and
// guarantees atomicity: metadata + content are a single byte stream
// written in one close_sync() call.
//
// Nginx writes the original variant with default metadata on cache miss.
// The worker writes optimized variants with full metadata (content class,
// SSIMULACRA2 score, origin cache-control fields) after processing.
//
// The content() and content_length() accessors transparently strip the
// metadata prefix.
struct ReadResult {
  cyclone::ReadHandle handle;
  AlternateMetadata metadata;

  // Content bytes (excludes metadata prefix, memory-mapped).
  // When metadata_prefix_size_ is 0 (sentinel read or parse failure),
  // returns the full blob.  ReadBestAlternate guards against parse
  // failure by returning NotFound, so callers of content() always
  // see either sentinel data (no prefix) or valid content.
  std::span<const std::byte> content() const {
    auto full = handle.content();
    if (metadata_prefix_size_ > 0) {
      if (full.size() >= metadata_prefix_size_) {
        return full.subspan(metadata_prefix_size_);
      }
      // Corrupt: metadata claims more bytes than available — return empty.
      return {};
    }
    return full;
  }
  uint64_t content_length() const {
    auto raw = handle.content_length();
    return raw > metadata_prefix_size_ ? raw - metadata_prefix_size_ : 0;
  }
  bool is_valid() const { return handle.is_valid(); }

  // Re-stamp the read lease pinning this result's mmap borrow.
  // Any holder keeping this ReadResult alive longer than 3T/4 of
  // PageSpeedCacheConfig::read_lease_duration (default T=5s → 3.75s) MUST
  // call this at a cadence <= 3T/4, keep the total hold under
  // lease_wrap_ceiling (default 60s), or copy the bytes — past the
  // ceiling the writer wraps anyway and the borrow is unprotected.
  // Returns false when there is no lease to renew (RAM-cache hit, leases
  // disabled, invalid handle).  Inherits the existing lifetime contract:
  // the handle must be closed before the cache is stopped/destroyed.
  // The is_valid() guard is belt-and-suspenders: cyclone::ReadHandle
  // already no-ops on an empty impl (verified at pin be0bd82e), but
  // guarding here keeps the no-op contract at the optimizer boundary
  // independent of a future Cyclone pin.
  bool renew_lease() { return handle.is_valid() && handle.renew_lease(); }

  // Intent-checked renewal for the ALIASED zero-copy serve path
  // (nginx sendfile / mmap-alias HIT serves).  Unlike the epoch-only
  // renew_lease() above — sound only for the copy-then-verify path, whose
  // read is observable — this stamps a fresh lease AND Dekker-revalidates
  // the borrow (wrap intent loaded before epoch), the same protocol the
  // initial borrow uses, so an aliased send whose read is an unobservable
  // socket write is protected.  Callers MUST invoke it in the same call
  // stack as each aliased send burst and act on the verdict (kOk keep
  // aliasing; kCopyNow / kLeasesOff copy the bytes out; kTorn abort the
  // serve).  Returns kLeasesOff when there is no lease/handle to renew
  // (RAM-cache hit, leases disabled, released/invalid handle) — legacy
  // semantics, so a leases-off deploy copies but never aborts.
  LeaseRenewal renew_lease_strict() {
    return handle.is_valid() ? handle.renew_lease_strict()
                             : LeaseRenewal::kLeasesOff;
  }

  // Nanoseconds until a ceiling-forced write-buffer wrap could overwrite this
  // result's borrowed mmap region.  A ceiling-forced wrap
  // ignores the read lease, so a zero-copy embedder polls this and copies the
  // aliased bytes out BEFORE the deadline drops within its safety margin
  // (chosen > its strict-renew recheck cadence).  Returns kNoForcedWrap when
  // no wrap is currently deferred on the stripe or leases do not apply (RAM
  // hit, leases disabled, released/invalid handle).
  static constexpr uint64_t kNoForcedWrap = UINT64_MAX;
  uint64_t ns_until_forced_wrap() const {
    return handle.is_valid() ? handle.ns_until_forced_wrap() : kNoForcedWrap;
  }

  // Drop the underlying mmap borrow (issue #934 de-alias).  For holders
  // that copy content() out and keep working (e.g. the worker's image
  // transcode/variant-write phase): releasing the handle stops this reader
  // from pinning the stripe against write-buffer wraps — the
  // already-stamped lease is never renewed again and simply expires.
  // After release(), content() returns empty, renew_lease() returns
  // false, and is_valid() is false; metadata remains valid (owned by this
  // struct, not by the mmap).
  void release() { handle = cyclone::ReadHandle{}; }

  // File offset of content within the cache volume (for sendfile).
  // Returns kNoFileOffset when sendfile is not possible (RAM cache
  // hit or no persistent mapping).
  static constexpr uint64_t kNoFileOffset = cyclone::ReadHandle::kNoFileOffset;
  uint64_t content_file_offset() const {
    auto raw_offset = handle.content_file_offset();
    if (raw_offset == kNoFileOffset) return kNoFileOffset;
    return raw_offset + metadata_prefix_size_;
  }

 private:
  friend class PageSpeedCache;
  size_t metadata_prefix_size_ = 0;
};

// Write handle wrapper.
//
// Metadata is written as a content prefix by WriteAlternate() before
// the handle is returned.  The caller only writes actual content bytes.
struct WriteResult {
  // Content bytes this handle will accept before the store is abandoned, or
  // kNoContentCap when the entry class has no cap (every class but the
  // durable original).  Counts CONTENT only: the metadata prefix is written
  // by the cache before the cap is armed, so a caller's budget is exactly the
  // number it was given.
  static constexpr uint64_t kNoContentCap = UINT64_MAX;

  // Write content bytes.
  //
  // When a cap is armed and this write would take the total past it, the
  // store is ABANDONED here, mid-stream: the underlying handle is aborted, so
  // nothing is ever committed and no reader sees a partial entry, the skip is
  // counted, and this and every later call return an error.  The alternative
  // — accept the bytes and reject at close — would first buffer exactly the
  // memory the cap exists to bound.
  [[nodiscard]] std::expected<size_t, cyclone::CacheError> write_sync(
      std::span<const std::byte> data) {
    // Once abandoned, stay abandoned, and keep saying WHY.  A caller that
    // writes on regardless would otherwise get the underlying handle's
    // generic "invalid" for the second chunk and a different diagnosis for
    // the same event.
    if (cap_exceeded_) {
      return std::unexpected(cyclone::CacheError::NoSpace);
    }
    if (content_cap_ != kNoContentCap) {
      // Subtraction, not addition: content_written_ <= content_cap_ is an
      // invariant here, so this cannot overflow, whereas the sum can.
      if (data.size() > content_cap_ - content_written_) {
        AbandonOverCap();
        return std::unexpected(cyclone::CacheError::NoSpace);
      }
      content_written_ += data.size();
    }
    return handle_.write_sync(data);
  }
  [[nodiscard]] std::expected<void, cyclone::CacheError> close_sync() {
    return handle_.close_sync();
  }
  bool is_valid() const { return handle_.is_valid(); }

  // True once a capped write was cut off for exceeding the cap — as opposed to
  // any other reason a write can fail.  The distinction is the whole reason
  // the C surface can tell an embedder "this response is too large to keep"
  // instead of the substrate's generic out-of-space, which means something
  // else entirely and is worth retrying.
  bool over_content_cap() const { return cap_exceeded_; }
  uint64_t content_cap() const { return content_cap_; }

 private:
  friend class PageSpeedCache;

  // The raw handle is NOT public: it is the bypass around the cap above.
  // Nothing outside the cache needs it, and the one in-tree writer that
  // deliberately writes past the cap (the metadata prefix, written before the
  // cap is armed) is PageSpeedCache itself.
  cyclone::WriteHandle handle_;

  void AbandonOverCap() {
    cap_exceeded_ = true;
    handle_.abort();  // discards the buffered content; nothing commits
    if (over_cap_counter_ != nullptr) {
      over_cap_counter_->fetch_add(1, std::memory_order_relaxed);
    }
  }

  uint64_t content_cap_ = kNoContentCap;
  uint64_t content_written_ = 0;
  bool cap_exceeded_ = false;
  // Owned by the PageSpeedCache that produced this handle, which outlives it
  // by the same contract ReadHandles already have.
  std::atomic<uint64_t>* over_cap_counter_ = nullptr;
};

// C++ cache API using Cyclone's native alternate selection.
//
// All URL-based operations normalize the hostname before composing
// the Cyclone CacheKey via CacheKey::from_url().
//
// WRITE INVARIANT: For any given (URL, hostname) pair, nginx and the
// worker never write the same AlternateId concurrently.  Nginx writes the
// default-mask alternate (AlternateId derived from CapabilityMask() = 0x08);
// the worker writes the transcoded formats, viewport variants and sentinels.
// This invariant eliminates write contention on Cyclone's per-alternate
// lock and ensures torn-write detection never fires in practice.
//
// The ids are NOT disjoint between the two, and reading them as disjoint is
// the mistake to avoid: 0x08 holds origin bytes only until an optimized
// variant for that same mask is written over it, so whether it still holds
// the original is a function of which client warmed the URL.  The entry that
// durably means "the original" is the 0x0C class, which has exactly one
// writer (WriteOriginalAlternate) and is never overwritten by an optimizer.
//
// EVERY write path here is ALTERNATE-SCOPED, and that is load-bearing rather
// than incidental.  A plain whole-key write on a key that carries an
// alternate chain repoints the directory head at a document with no alternate
// id and no next-offset, which ORPHANS the entire chain: every variant and
// the durable original become unreachable at once, silently, looking exactly
// like a cold cache.  So no method on this class takes that path, and nothing
// else may reach one — the disjointness is asserted in
// test/lib/cache/durable_originals_test.cc rather than left as advice.
class PageSpeedCache {
 public:
  ~PageSpeedCache();

  // Create and open a PageSpeedCache.
  [[nodiscard]] static std::expected<std::unique_ptr<PageSpeedCache>,
                                     cyclone::CacheError>
  Create(const PageSpeedCacheConfig& config);

  // Replace the diagnostic logger (not owned; nullptr disables logging).
  // Not synchronised: call it before the cache is shared with other threads.
  // The C API uses it to drop a logger that only lives for ps_cache_open.
  void set_handler(MessageHandler* handler) {
    handler_ = handler;
    config_.handler = handler;
  }

  // The key namespace: the first bytes of every key this version composes.
  // Moving it makes every stored entry unreachable to every other version at
  // once, at the cost of a cold cache for every component.  It moved to
  // "ks3:" with the cookie rule (lib/cache/html_cache_policy.h) and to "ks4:"
  // when a response varying on a request header other than `Accept-Encoding`
  // stopped being stored (lib/cache/vary_storability.h), so that no entry
  // stored under an earlier rule is ever served.
  static constexpr std::string_view kKeyNamespace = "ks4:";

  // Compose a CacheKey from URL, pre-normalized hostname, and scheme.
  //
  // Every key is "ks4:" followed by the composed string ("scheme://host/url",
  // or the "oc1:" form below).  "ks4:" is the key namespace: keys composed
  // without it, as every earlier version did, never equal one composed with
  // it, so an earlier version cannot reach an entry this one stores and this
  // one cannot reach an earlier version's.  A test pins the exact strings.
  //
  // The hostname MUST already be normalized via NormalizeHostname().
  // The scheme MUST be "http" or "https" (validated by caller).
  // This avoids redundant normalization when the caller needs to
  // perform multiple cache operations on the same URL.
  static cyclone::CacheKey ComposeKeyPreNormalized(
      std::string_view url, std::string_view normalized_hostname,
      std::string_view scheme);

  // The same, for a request that resolved to a particular option context.
  //
  // NOT CALLED BY THE ENGINE, and that is the current design rather than an
  // omission: no optimized output varies with a resolved configuration today,
  // so the worker accepts a valid non-default context and stores the work
  // under the DEFAULT context (Worker::AcceptOptionContext).  What follows
  // describes the shape separation takes if that ever stops being true; the
  // overload and its tests are kept because the decision they record is the
  // expensive one to get wrong.  See lib/classify/option_context.h.
  //
  // THIS IS WHERE EXACT MATCH WOULD LIVE.  Two option contexts are two
  // namespaces:
  // a request under one cannot see, select, or be served anything stored under
  // the other, because it never looks at the same key.  There is no scoring
  // step to get wrong and no "close enough" to argue about — the discrimination
  // is the full 64-character signature or nothing.  lib/classify/option_context.h
  // records why it is keyed rather than filtered at selection time; the short
  // version is that a stored alternate is addressed by one byte, all eight bits
  // of it are spent, and a key holds at most 64 alternates against a published
  // budget that already accounts 44.
  //
  // THE DEFAULT CONTEXT PRODUCES THE DEFAULT KEY, BYTE FOR BYTE. An empty
  // signature, or the signature of the empty context, yields exactly what
  // ComposeKeyPreNormalized above yields — same string, same digest. So a
  // deployment that sends no option context is unaffected by contexts. A test
  // pins that equality rather than leaving it to inspection.
  //
  // Injectivity, since the two forms share one hash input space. Both carry
  // the same "ks4:" namespace prefix, so they are compared after it. A
  // default-context key is then the composed string "scheme://host/url",
  // whose scheme the caller has already validated as exactly "http" or
  // "https"; a context key always continues "oc1:". No validated scheme is a
  // prefix of "oc1:", so the two shapes are disjoint at their first byte
  // after the prefix, regardless of what the url or hostname contain — which
  // is the part that matters, because a url IS attacker-influenced. Within the context form the
  // signature is a fixed width followed by a separator, so its boundary is
  // unambiguous too, and a test drives a url crafted to imitate the context
  // form.
  //
  // SCOPE, stated because it is a real limit and not an oversight. The
  // signature scopes the key, so per-URL operations addressed through the
  // default-context key — purge, remove, alternate listing — act on the default
  // context. Nothing in the tree writes a non-default context yet (no in-tree
  // sender supplies one), so there is no reachable divergence today; a sender
  // that starts supplying contexts needs the purge surface to enumerate them,
  // and that is a prerequisite of THAT change, not of this one.
  static cyclone::CacheKey ComposeKeyPreNormalized(
      std::string_view url, std::string_view normalized_hostname,
      std::string_view scheme, std::string_view option_signature);

  // The same again, scoped to one site: the keyspace of one server block or
  // virtual host of the front end that recorded the entry.
  //
  // `site` is empty, or 64 lowercase hex characters (the SHA-256 of that
  // block's cache fragment); the caller has validated it. EMPTY IS THE OLD
  // KEY: the result is exactly the four-argument overload's, so an unsited
  // caller moves nothing. A non-empty site gives
  // "ks4:" + "st1:" + site + "@" + the string the four-argument overload
  // composes after "ks4:", its "oc1:" form included.
  //
  // Injectivity, alongside the "oc1:" argument above: the site is fixed width
  // and followed by "@", so its boundary is unambiguous, and after the "ks4:"
  // namespace the three shapes differ at their first byte — "s" for a sited
  // key, "h" for a validated scheme ("http"/"https"), "o" for "oc1:". No url
  // or hostname can move a key from one shape to another. The hostname inside stays the origin's, for fetches
  // and same-host tests; only the key is scoped.
  static cyclone::CacheKey ComposeSiteKeyPreNormalized(
      std::string_view url, std::string_view normalized_hostname,
      std::string_view scheme, std::string_view option_signature,
      std::string_view site);

  // SITE. Every url-based method below takes a trailing `site` and composes
  // its key through ComposeSiteKeyPreNormalized with the default option
  // context. Empty (the default) is the unsited keyspace, exactly as before;
  // a 64-hex site addresses that site's keyspace and nothing else. The
  // caller validates the site.
  //
  // Read the best-fit alternate for a client capability mask.
  // Uses PageSpeedSelector to score all stored alternates.
  // Returns CacheError::NotFound if no alternates exist or none match.
  // agent_request_entitled: when true, an entitled agent request
  // may select the kAgentMarkdown sentinel variant; default false = legacy
  // behavior (the sentinel stays unselectable). agent_optimize is OFF by default,
  // so all current callers leave this defaulted.
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadBestAlternate(std::string_view url, std::string_view hostname,
                    std::string_view scheme, const CapabilityMask& mask,
                    bool agent_request_entitled = false,
                    std::string_view site = {});

  // Read best alternate using a pre-composed CacheKey (avoids re-hashing).
  // This is the nginx serve hot path; per the port-parity discipline,
  // agent_request_entitled is NOT defaulted here so every serve site must pass
  // it consciously (compile-break = the audit).  The url-based ReadBestAlternate
  // keeps a fail-safe `=false` default for its ~150 non-serve callers.
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadBestAlternateByKey(const cyclone::CacheKey& key,
                         const CapabilityMask& mask,
                         bool agent_request_entitled);

  // URL-based convenience over ReadBestAlternateAgentByKey
  // (composes + normalizes the key, like ReadBestAlternate), for the native C
  // API / .NET serve surfaces.  agent_request_entitled is non-defaulted (the
  // serve-path audit-by-compile-break).
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadBestAlternateAgent(std::string_view url, std::string_view hostname,
                         std::string_view scheme, const CapabilityMask& mask,
                         bool agent_request_entitled,
                         std::string_view site = {});

  // The SINGLE audited agent serve gate, shared by every serve
  // surface (nginx + the native C API + the .NET middleware).  Selects the best
  // alternate (ReadBestAlternateByKey), and if it returns the kAgentMarkdown
  // variant, applies the §2.4 content-hash equality gate: the variant's stamped
  // origin_html_hash MUST equal the live kContentHash sentinel for the key, or
  // the markdown is REFUSED (returns NotFound) so the caller's miss path runs —
  // a superseded origin can never be served as fresh markdown.  Non-agent /
  // non-markdown reads pass through unchanged.  agent_request_entitled is
  // non-defaulted (the serve-path audit-by-compile-break).
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadBestAlternateAgentByKey(const cyclone::CacheKey& key,
                              const CapabilityMask& mask,
                              bool agent_request_entitled);

  // Read a specific alternate by ID.  A content alternate or the agent
  // Markdown variant whose metadata does not parse (a version this build
  // refuses, or damage) is a miss; other sentinels have no metadata prefix
  // and are returned as is.
  // Uses a trivial selector that matches only the requested ID.
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError> ReadAlternate(
      std::string_view url, std::string_view hostname, std::string_view scheme,
      AlternateId id, std::string_view site = {});

  // Read alternate by pre-composed CacheKey (avoids re-hashing).
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadAlternateByKey(const cyclone::CacheKey& key, AlternateId id);

  // Write a content alternate. Returns a WriteResult for streaming
  // data.  Sets the AlternateMetadata header automatically.
  // Rejects sentinel AlternateIds (use WriteSentinel instead). [F13]
  // Rejects metadata blobs > max_metadata_size. [F35]
  [[nodiscard]] std::expected<WriteResult, cyclone::CacheError> WriteAlternate(
      std::string_view url, std::string_view hostname, std::string_view scheme,
      AlternateId id, uint64_t content_length,
      const AlternateMetadata& metadata, std::string_view site = {});

  // The same, against a key the caller already has.
  //
  // The write-side twin of ReadBestAlternateByKey / ReadAlternateByKey, and it
  // exists for the reason those do: a caller holding a key composed from
  // something other than url+hostname+scheme — an option context, above — has
  // no way to express that through the url-based entry point. WriteAlternate is
  // now a thin wrapper over this, so the two cannot drift.
  //
  // Its only caller today is the budget proof in
  // test/lib/cache/option_context_key_test.cc, which is stated rather than
  // hidden: without a by-key write there is no way to demonstrate that
  // context-keyed alternates cost zero chain depth, and an unmeasured budget
  // claim is indistinguishable from a wrong one. The record and serve arms are
  // the production callers this is shaped for.
  //
  // `url_for_logs` is exactly that: it never reaches the key, only the
  // diagnostics, so that a message about a context-keyed write still names the
  // resource a human is looking for.
  [[nodiscard]] std::expected<WriteResult, cyclone::CacheError>
  WriteAlternateByKey(const cyclone::CacheKey& key, AlternateId id,
                      uint64_t content_length,
                      const AlternateMetadata& metadata,
                      std::string_view url_for_logs);

  // Write a sentinel alternate (Early Hints, warmup, content hash, etc.).
  //
  // RE-RECORDING REPLACES, with the same shape and the same limit the other
  // writers carry (stated in full on WriteOriginalAlternate):
  //
  //   * A READ ALWAYS SERVES THE NEWEST entry under this id, under any
  //     interleaving.
  //   * WITH ONE WRITER PER URL, a re-record REPLACES: the previous entry is
  //     unlinked before the new one is written, so the chain keeps one node
  //     under this id.
  //   * WITH CONCURRENT WRITERS for the same URL and id, superseded entries
  //     CAN accumulate: unlink and write are separate substrate operations
  //     with no atom across them, so the depth ratchets rather than settling.
  //     Bounded by the chain-traversal ceiling — whose exhaustion stops the
  //     KEY from accepting alternate writes at all — and logged, and counted
  //     process-internally: SentinelsSupersededObserved(),
  //     SentinelsUnlinkFailed() (C++-side accessors only; the log line is the
  //     field-observable, and it names the id).  Unconditional replacement
  //     arrives with the storage advance and needs no change here.
  //
  // REPLACEMENT WINDOW: like the durable original this is a STREAMING write —
  // the previous entry is unlinked when this method is called and the new one
  // exists only when the caller CLOSES the handle, so an abandoned stream, or
  // one the substrate refuses at close, leaves the URL with NO entry under
  // this id until the next store.  A reader in the window sees a miss — never
  // wrong bytes.  A caller that would rather keep a stale entry than risk
  // that gap should check for one first and decide.
  [[nodiscard]] std::expected<WriteResult, cyclone::CacheError> WriteSentinel(
      std::string_view url, std::string_view hostname, std::string_view scheme,
      SentinelId sentinel, uint64_t content_length, std::string_view site = {});

  // Write the agent_optimize markdown variant. It is stored under
  // the kAgentMarkdown=0x7C sentinel id but, UNLIKE WriteSentinel, WITH a
  // metadata prefix — because it is read back via the strict ReadBestAlternate
  // path (which rejects a missing prefix as NotFound). WriteAlternate cannot be
  // used (it rejects sentinel ids). Sets the AlternateMetadata header + prefix
  // automatically; the caller then write_sync()s the markdown bytes.
  //
  // RE-RECORDING REPLACES, exactly as WriteSentinel (which see for the full
  // contract): one writer per URL keeps one node under this id, concurrent
  // writers can accumulate superseded entries bounded by the chain ceiling —
  // counted via AgentMarkdownSupersededObserved() /
  // AgentMarkdownUnlinkFailed() — and the unlink happens when this method is
  // called while the replacement exists only at close, so an abandoned stream
  // leaves no entry under this id until the next store.  A reader in that
  // window misses — never wrong bytes.
  [[nodiscard]] std::expected<WriteResult, cyclone::CacheError>
  WriteAgentAlternate(std::string_view url, std::string_view hostname,
                      std::string_view scheme, uint64_t content_length,
                      const AlternateMetadata& metadata,
                      std::string_view site = {});

  // Write the durable ORIGINAL of a resource: the bytes the origin sent,
  // unoptimized, kept so that the optimized variants can be rebuilt without
  // going back to the origin.
  //
  // Stored under SentinelId::kOriginalContent (0x0C) and, like the agent
  // variant, WITH an AlternateMetadata prefix — a sentinel written with a
  // prefix.  The id is hard-coded: this entry point exists precisely so that
  // the class has exactly one writer, which is what makes "the original" mean
  // one thing.  (The default-mask alternate does NOT: it holds origin bytes
  // until an optimized variant for the same mask overwrites it, so whether it
  // still holds the original is a function of which client warmed the URL.)
  //
  // LIFETIME IS ORIGIN-FRESHNESS-BOUND.  The class has no TTL of its own: the
  // entry is fresh for exactly as long as the origin's own Cache-Control and
  // validators say the origin response is, evaluated by the same shared
  // freshness evaluator every other entry goes through
  // (FreshnessInputFromMetadata, lib/cache/freshness.h).  So `metadata` must
  // carry the origin state that governs it — cache_inserted_at, max-age /
  // s-maxage, the Cache-Control flags, the raw Cache-Control string, ETag and
  // Last-Modified.  A zero cache_inserted_at reads as epoch-old, i.e. always
  // revalidate: the failure direction is extra revalidation, never bytes
  // served as fresher than the origin allowed.
  //
  // The metadata's low mask byte MUST be kOriginalContent — the entry has to
  // self-describe as the class it is stored under, or a reader that finds it
  // by exact id cannot tell it from a variant that landed in the wrong slot.
  //
  // CONTENT CAP.  content_length above the configured cap
  // (max_original_content_length, default kDefaultMaxOriginalContentLength) is
  // refused here, and a caller that declares less and writes more is cut off
  // mid-stream by WriteResult::write_sync.  Both count as one skipped store
  // (OriginalsOverCapSkipped()); neither is an error the caller has to handle
  // beyond not storing the entry.
  //
  // RE-RECORDING REPLACES, as far as it can be made to from here — the same
  // shape, and the same limit, as the header sidecar's:
  //
  //   * A READ ALWAYS SERVES THE NEWEST original.  Unconditionally, under any
  //     interleaving.
  //   * WITH ONE WRITER PER URL, a re-record REPLACES: the previous original
  //     is unlinked before the new one is written, so the chain keeps one
  //     node of this class.
  //   * WITH CONCURRENT WRITERS for the same URL, superseded originals CAN
  //     accumulate.  Unlink and write are separate substrate operations with
  //     no atom across them, and unlink-one/write-one is a fixed point, so
  //     the depth ratchets rather than settling.  It cannot be closed from
  //     this side at this storage version; the substrate's own
  //     unlink-on-prepend closes it at the next pin advance and needs no
  //     change here.  Bounded by the chain-traversal ceiling — reaching it
  //     stops the KEY from accepting alternate writes at all — and logged,
  //     and counted process-internally: OriginalsSupersededObserved(),
  //     OriginalsUnlinkFailed() (C++-side accessors; not yet on any exported
  //     stats surface, so in the field the log line is the observable).
  //   * A WRITER OF ANOTHER CLASS RACING A RE-RECORD LOSES NOTHING.  The
  //     storage layer publishes an alternate only into the directory state
  //     the write resolved its chain against, and resolves again when
  //     another process published or removed the key in between.  Before
  //     that guarantee an optimized copy written while the original was
  //     being re-recorded could be dropped without any error; it is pinned
  //     by test/lib/cache/alternate_publish_race_test.cc.
  //
  // For THIS class the accumulation costs volume as well as depth: a
  // superseded node holds a whole response body, not a header block.
  //
  // WHY THE UNLINK CANNOT MOVE AFTER THE COMMIT, since that would close the
  // window below: the substrate PREPENDS, and remove_alternate_sync unlinks
  // the FIRST node matching the id — which, after a commit, is the node just
  // written.  A post-commit unlink would delete the new original and keep the
  // old one.
  //
  // REPLACEMENT WINDOW, wider here than for any other class and stated so a
  // caller can decide what to do about it: the previous original is unlinked
  // when this method is called, and the new one exists only when the caller
  // CLOSES the handle — so for the whole duration of the caller's streaming
  // there is no original for this URL, and a stream that is abandoned, fails,
  // or is cut off by the content cap leaves none.  A reader in that window
  // sees a miss, which is the correct reading of "no original is stored"; the
  // cost is a re-fetch, never wrong bytes.  A caller that would rather keep a
  // stale original than risk that gap should check for one first and decide,
  // rather than expect this method to keep it.
  [[nodiscard]] std::expected<WriteResult, cyclone::CacheError>
  WriteOriginalAlternate(std::string_view url, std::string_view hostname,
                         std::string_view scheme, uint64_t content_length,
                         const AlternateMetadata& metadata,
                         std::string_view site = {});

  // Read the durable original by EXACT ID.
  //
  // This is the whole serve-side story for the class, and it is why the class
  // is safe: selection scores alternates, an exact-id read does not, so a
  // caller that has decided it wants the original asks for it by name and no
  // amount of scoring can produce it for anyone who did not.
  //
  // Two things make an entry at this id a MISS rather than a result, and both
  // are about entries this build did not write: a missing or unparseable
  // metadata prefix (the class always carries one), and a prefix whose low
  // mask byte is not this class (an entry that does not say it is what it is
  // stored as — a misplaced variant, or a peer's idea of the slot). The
  // caller gets the class or it gets nothing.
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadOriginalAlternate(std::string_view url, std::string_view hostname,
                        std::string_view scheme, std::string_view site = {});
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadOriginalAlternateByKey(const cyclone::CacheKey& key);

  // Offer a response's headers to the response-header sidecar
  // (SentinelId::kHeadersSidecar, 0x6C): the curated, verbatim block of
  // request-independent headers an optimized entry has to reproduce.
  //
  // THIS ENTRY POINT IS THE ADMISSION GATE, which is why the class has
  // exactly one writer and why the generic sentinel surface refuses the id.
  // The caller hands over the response's complete header set — one entry per
  // received line — and the gate decides; a caller cannot hand over a block
  // it curated itself, because "which headers may be stored" is precisely the
  // decision that must not be re-made per call site.  See
  // lib/cache/headers_sidecar.h for the allowlist and the nonce rule.
  //
  // A REFUSAL IS A NORMAL OUTCOME, NOT AN ERROR.  Most responses carry
  // something the entry cannot reproduce; those are served plain (correctly,
  // just not optimized in place).  So the refusal comes back as a successful
  // call whose result says `stored == false` with the verdict that explains
  // it, and `std::unexpected` means the cache itself failed.
  //
  // ONE ENTRY PER URL — and here is exactly how far that holds today, because
  // the honest version of this promise is narrower than the obvious one.
  //
  //   * A READ ALWAYS SERVES THE NEWEST BLOCK.  Unconditionally, under any
  //     interleaving.  This is the property callers actually depend on.
  //   * WITH ONE WRITER PER URL, a re-store REPLACES: the old entry is
  //     unlinked before the new one is written, so the chain stays at one
  //     node and nothing accumulates.
  //   * WITH CONCURRENT WRITERS for the same URL, superseded nodes CAN
  //     accumulate.  The unlink and the write are separate operations on the
  //     substrate with no atom across them, so two calls can interleave
  //     unlink/unlink/write/write and leave two nodes — and unlink-one,
  //     write-one is a fixed point at two, so the depth ratchets and does not
  //     come back down on its own.  It cannot be closed at this storage
  //     version from this side; what closes it is the substrate's own
  //     unlink-on-prepend, which arrives with the next storage-layer advance
  //     and makes replacement unconditional.  Until then the cost is bounded
  //     by the substrate's chain-traversal ceiling, and reaching it stops the
  //     KEY (not just this class) from accepting alternate writes — so the
  //     accumulation is observable rather than silent: see
  //     HeadersSidecarSupersededObserved().
  //
  // The process-local RAM copy is evicted as part of the write, because that
  // tier is write-around and would otherwise keep answering reads with the
  // block this call just replaced.
  //
  // AVAILABILITY GAP, stated because the serve path has to inherit a rule
  // from it: between the unlink and the commit there is a window in which a
  // reader finds NO block, and if the write then fails the previous block is
  // gone with nothing in its place.  That is the price of not accumulating,
  // and it is the right trade only as long as a missing block means "this
  // response is not optimizable" and never "optimized, with no headers".  A
  // serve path that treats a sidecar miss as the latter turns this window
  // into precisely the bug the class exists to prevent.
  //
  // NOT WRITTEN when the block would be empty: a response whose headers are
  // all reproduced by the entry metadata is swap-eligible with nothing to
  // carry, and an empty sidecar entry would assert a class membership that
  // means nothing.
  struct HeadersSidecarWrite {
    SidecarVerdict verdict = SidecarVerdict::kSwapEligible;
    bool stored = false;
    size_t fields = 0;         // header lines in the stored block
    size_t payload_bytes = 0;  // stored block size
  };
  [[nodiscard]] std::expected<HeadersSidecarWrite, cyclone::CacheError>
  WriteHeadersSidecar(std::string_view url, std::string_view hostname,
                      std::string_view scheme,
                      std::span<const HeaderField> headers,
                      std::string_view site = {});

  // Read the header sidecar by EXACT ID.  Like the durable original, it is
  // never selectable — a block of headers is not a representation and must
  // never be handed to a client as one.
  //
  // A MISS, rather than a result, for anything that is not this class as this
  // build knows it: no entry, an empty blob, a payload format version this
  // build does not recognise, or a payload that does not parse.  A reader
  // that finds a version byte it does not know STOPS; the caller's miss path
  // then runs, which is the same thing it would do on a cold cache.
  //
  // The returned ReadResult holds the RAW blob: this class carries a payload
  // format version, NOT an entry-metadata prefix, so the parsed `metadata` of
  // the result is meaningless and `content()` is not the accessor to use.
  // Pass ReadHeadersSidecarPayload(result) to ParseHeadersSidecar.
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadHeadersSidecar(std::string_view url, std::string_view hostname,
                     std::string_view scheme, std::string_view site = {});
  [[nodiscard]] std::expected<ReadResult, cyclone::CacheError>
  ReadHeadersSidecarByKey(const cyclone::CacheKey& key);

  // The stored sidecar payload of a ReadHeadersSidecar result, as bytes.
  // Named rather than left to the caller so that "use the raw blob, not
  // content()" is a thing you call instead of a thing you remember.
  static std::string_view HeadersSidecarPayload(const ReadResult& result);

  // Responses offered to the sidecar and not carried, counted since this
  // cache was opened.  The two classes are DISJOINT, and that is the whole
  // reason there are two: a fall-through is a gap later work can close, and
  // the never-optimized class cannot be closed by anything — a per-response
  // nonce is not storable in principle.  A single counter over both would
  // report the permanent class as closable, which is exactly the conflation
  // the third verdict was spent to prevent.  Total refusals is their sum, and
  // a caller that wants it adds them rather than finding one silently
  // including the other.
  //
  // OBSERVABILITY ONLY.  Nothing in this library reads any of these counters
  // to make a decision, and nothing may start: they exist to show how large
  // each class actually is, and a counter that acquires a consumer stops
  // being a measurement of what it was measuring.  Monotonic; a non-zero
  // value is an observation, not an error.
  uint64_t HeadersSidecarFallThrough() const {
    return headers_sidecar_fall_through_.load(std::memory_order_relaxed);
  }
  uint64_t HeadersSidecarNeverOptimized() const {
    return headers_sidecar_never_optimized_.load(std::memory_order_relaxed);
  }

  // Stores after which the URL's chain still carried more than one node under
  // this class's id — i.e. the accumulation the single-writer path prevents
  // and concurrent writers can still produce.  It is the instrument that
  // keeps that limitation from being invisible: the depth is measured after
  // the pass rather than assumed from the code path, and a rising value is
  // the signal that a deployment has more than one writer per URL.
  uint64_t HeadersSidecarSupersededObserved() const {
    return headers_sidecar_superseded_observed_.load(std::memory_order_relaxed);
  }

  // Stores whose unlink of the previous block reported an error that means it
  // may still be LINKED (see UnlinkLeftEntryLinked).  The store goes ahead
  // regardless — availability first, a URL with fresh headers beats a URL
  // with none — so this counter plus the log line is the only trace that the
  // chain grew for that reason.
  uint64_t HeadersSidecarUnlinkFailed() const {
    return headers_sidecar_unlink_failed_.load(std::memory_order_relaxed);
  }

  // The content cap durable originals are held to, with a zero-initialised
  // config resolved to the default.  Exposed so a caller that has to explain
  // a skipped store can name the number it was held to rather than assume
  // the default.
  uint64_t MaxOriginalContentLength() const {
    return EffectiveMaxOriginalContentLength();
  }

  // Durable originals not stored because they exceeded the content cap,
  // counted since this cache was opened (declared-too-large and
  // cut-off-mid-stream both count once).  Monotonic; a non-zero value is a
  // sizing observation, not an error.
  uint64_t OriginalsOverCapSkipped() const {
    return originals_over_cap_skipped_.load(std::memory_order_relaxed);
  }

  // Re-records that ARRIVED to find more than one node already carrying this
  // class's id — i.e. the accumulation a single writer prevents and
  // concurrent writers can still produce.
  //
  // Measured at arrival rather than after the commit, and the difference is
  // deliberate: this is a streaming class whose entry commits when the CALLER
  // closes the handle, so the writer has no post-commit moment of its own.
  // Reaching one would mean carrying a pointer back into this cache on the
  // write handle and walking the chain outside the reset lock — immediacy
  // bought with a lifetime hazard.  Nothing that matters is lost: the store
  // that grows the chain DETERMINISTICALLY is counted when it happens, by
  // OriginalsUnlinkFailed(); what this catches is the concurrent
  // interleaving, which needs a later observer either way.  The pair OVERLAP
  // by one event: a failed unlink leaves depth 2, so the NEXT store also
  // observes it here.  Read them as overlapping signals, not addends.
  //
  // The consequence to read it with: a store that leaves a superseded node is
  // reported by the NEXT store for that URL, and if there is no next store it
  // is not reported at all.  The counter is a floor on accumulation, never an
  // exact count of it.  (The headers sidecar's peer counter measures after
  // the commit, because that writer owns the whole write; the two are not
  // interchangeable and are deliberately not merged into one.)
  uint64_t OriginalsSupersededObserved() const {
    return originals_superseded_observed_.load(std::memory_order_relaxed);
  }

  // Re-records whose unlink of the previous original reported an error
  // meaning it may still be LINKED (see UnlinkLeftEntryLinked).  The store
  // goes ahead regardless — availability first — so this counter plus the log
  // line is the only trace that the chain grew for that reason.
  uint64_t OriginalsUnlinkFailed() const {
    return originals_unlink_failed_.load(std::memory_order_relaxed);
  }

  // Sentinel re-records (WriteSentinel, any sentinel id) that ARRIVED to find
  // more than one node already under the id being written — the accumulation
  // a single writer prevents and concurrent writers can still produce.
  //
  // Same measurement shape as OriginalsSupersededObserved(), and for the same
  // forced reason: WriteSentinel is a streaming writer whose commit belongs
  // to the caller, so the depth is read at arrival rather than after the
  // commit.  The pair OVERLAP the same way: a failed unlink leaves depth 2,
  // so the next store re-observes it here.  Read them as overlapping signals,
  // not addends, and as a floor on accumulation rather than a count of it.
  //
  // Per WRITER rather than per sentinel id: the counter's job is to make the
  // accumulation visible, and attribution already happens in the log line,
  // which names the id.  A per-id dimension would buy a map lookup where
  // there is now a load.
  uint64_t SentinelsSupersededObserved() const {
    return sentinels_superseded_observed_.load(std::memory_order_relaxed);
  }

  // Sentinel re-records whose unlink of the previous entry reported an error
  // meaning it may still be LINKED (see UnlinkLeftEntryLinked).  The store
  // goes ahead regardless — availability first — so this counter plus the log
  // line (which names the id) is the only trace that the chain grew for that
  // reason.
  uint64_t SentinelsUnlinkFailed() const {
    return sentinels_unlink_failed_.load(std::memory_order_relaxed);
  }

  // The agent-markdown variant's pair of the same signals — kept separate
  // from the sentinel pair for the reason the originals and sidecar pairs are
  // separate: the classes re-record on different triggers, so a shared
  // counter would conflate signals that mean different things operationally.
  uint64_t AgentMarkdownSupersededObserved() const {
    return agent_markdown_superseded_observed_.load(std::memory_order_relaxed);
  }
  uint64_t AgentMarkdownUnlinkFailed() const {
    return agent_markdown_unlink_failed_.load(std::memory_order_relaxed);
  }

  // Remove all alternates for a URL.
  [[nodiscard]] std::expected<void, cyclone::CacheError> Remove(
      std::string_view url, std::string_view hostname, std::string_view scheme,
      std::string_view site = {});

  // Remove a single alternate for a URL, leaving the rest of the chain
  // intact (issue #652: lets the worker's origin-refreshed purge drop the
  // stale optimized variants while preserving the fresh identity original
  // that nginx just re-recorded).  Also evicts the (key, id) entry from
  // this process's RAM tier, mirroring whole-key Remove semantics.
  [[nodiscard]] std::expected<void, cyclone::CacheError> RemoveAlternate(
      std::string_view url, std::string_view hostname, std::string_view scheme,
      AlternateId id, std::string_view site = {});

  // Drop every alternate of a URL except the ids in `preserve` — the removal
  // the origin-refresh purge performs, decided from listings of the URL's
  // alternate chain rather than by a sequence of independent call-site
  // decisions.
  //
  // The durable original (0x0C) is NOT preservable, and that is the point of
  // this entry point rather than a loop at the call site: the original belongs
  // to the origin response the refresh just replaced, so it must die in the
  // SAME removal event as the variants derived from it.  Preserve it and the
  // rebuild reconstructs the new variant set from the old bytes — an original
  // that outlived its origin, which looks like a working cache.  Passing it in
  // `preserve` is a programming error: the whole call is refused
  // (InvalidArgument) and nothing is removed, rather than honouring half of a
  // request that cannot be honoured.
  //
  // NOT ONE SNAPSHOT, and not a transaction — Cyclone has no multi-alternate
  // atom.  A listing is a CAPPED traversal of the chain, so one listing need
  // not show every alternate: the pass repeats (bounded) while non-preserved
  // entries are still listed and the listing is still changing.  Cyclone's
  // write-time unlink of superseded same-id documents keeps an ordinary chain
  // far below that cap, and it is best-effort by contract, so the repeat is
  // defensive rather than routine.  What a caller
  // may assume is therefore about the WINDOW, not about a snapshot: an
  // alternate written concurrently during the purge is removed if a later pass
  // lists it, and survives only if it lands after the last pass — the callers
  // fence concurrent writers with their own purge generation, as before, and
  // that fence is what makes the window safe rather than any atomicity here.
  //
  // Escalation.  When removals report SUCCESS and the chain comes back
  // unchanged — a chain whose nodes link in a cycle, which the storage layer
  // also refuses every further write to, and whose write-time single-id reset
  // does not cover a key holding more than one alternate id — no number of
  // passes can empty it,
  // and the whole key is dropped instead (confirmed first by a positively
  // enumerated non-preserved entry).  That drop takes a preserved entry with
  // it: on such a key the preserve contract cannot be honoured, and one
  // re-record beats a purge-and-rebuild on every refresh forever.  A removal
  // that FAILED, or a listing that could not be READ, is never treated as
  // that signature: both are maybe-transient storage verdicts, so the call
  // simply stops and the next refresh retries, exactly as it did before this
  // function grew passes.
  //
  // Returns the number of alternates removed (a whole-key drop counts as one).
  // A key that no longer exists is not an error: the result is 0.
  [[nodiscard]] std::expected<size_t, cyclone::CacheError>
  RemoveAlternatesExcept(std::string_view url, std::string_view hostname,
                         std::string_view scheme,
                         std::span<const AlternateId> preserve,
                         std::string_view site = {});

  // Drop everything the worker DERIVED for a URL and keep what the front end
  // recorded: the mirror image of RemoveAlternatesExcept.
  //
  // RemoveAlternatesExcept serves the origin-refresh event, where the durable
  // original (0x0C) is the SUPERSEDED response and must die with the variants
  // built from it, so it refuses to preserve it.  This entry point serves the
  // opposite event: the front end has just recorded a NEW original, and the
  // variants (optimized, gzip/brotli copies, decline tombstone, content-hash
  // oracle, agent markdown, ...) were built from the old one.  The original
  // must survive so the rebuild starts from the new bytes.  The two are
  // deliberately separate; do not fold one into the other.
  //
  // Preserved: the durable original (0x0C), the headers sidecar (0x6C), and
  // the identity slot when it is a front end's genuine recording
  // (kFlagWorkerProcessed clear, read from disk).  A worker-processed
  // identity is derived and is removed.  Everything else is removed, and
  // each removed id is evicted from this process's RAM tier.
  //
  // Same non-transactional, bounded-pass semantics as RemoveAlternatesExcept,
  // including its escalation: a chain that cannot be unlinked is dropped as a
  // whole key, original included (the front end re-records on the next
  // request; a stale variant that cannot be removed is the worse outcome).
  //
  // The identity slot's flag is read BEFORE the removal takes reset_mutex_.
  // A front end that records a genuine identity in that gap has it removed
  // as if derived; the next request re-records it.
  //
  // A transient removal failure (e.g. Busy) stops the passes but the call
  // still returns success with a partial count; the caller's next
  // notification retries.  Do not read a success as "nothing derived remains".
  // Only the unlinkable-chain escalation above drops the whole key, original
  // included.
  //
  // Purge-generation fence: this function does not touch the worker's
  // generation counters.  A caller that fences concurrent writers must bump
  // its generation around this call AND re-read it afterwards, before the
  // rebuild's writes, or the rebuild is fenced by its own bump.
  //
  // Returns the number of alternates removed.  A missing key is 0, not an
  // error.
  [[nodiscard]] std::expected<size_t, cyclone::CacheError>
  RemoveDerivedAlternates(std::string_view url, std::string_view hostname,
                          std::string_view scheme, std::string_view site = {});

  // Drop the entries of a URL that an earlier version wrote and this build no
  // longer serves (AlternateMetadata::IsRetired: every HTML entry below
  // version 9, and the worker's Markdown versions of pages).  Reads already
  // treat them as a MISS; removing them matters when a rebuild does not
  // overwrite every one, since selection works on the stored mask and would
  // keep choosing such an entry.  Only the newest node of each id is judged:
  // it is the node a read returns and the node a removal unlinks.  Best
  // effort, one pass; returns the number removed.  A missing key is 0, not an
  // error.  Acts on `site`'s keyspace only.
  [[nodiscard]] std::expected<size_t, cyclone::CacheError>
  RemoveRetiredAlternates(std::string_view url, std::string_view hostname,
                          std::string_view scheme, std::string_view site = {});

  // Evict a single (key, alternate) entry from THIS process's RAM tier
  // without touching the on-disk document.  Needed because Cyclone's RAM
  // tier is write-around (reads populate it, writes do NOT evict): after
  // another process — or even this process — overwrites an alternate on
  // disk, a stale RAM copy keeps winning reads (issue #652: the worker
  // must evict its RAM copy of the identity before rebuilding from the
  // freshly re-recorded body).  No-op when the RAM tier is disabled.
  void EvictAlternateFromRamCache(std::string_view url,
                                  std::string_view hostname,
                                  std::string_view scheme, AlternateId id,
                                  std::string_view site = {});

  // Check if a specific alternate exists.
  bool AlternateExists(std::string_view url, std::string_view hostname,
                       std::string_view scheme, AlternateId id,
                       std::string_view site = {});

  // List all alternates for a URL.
  [[nodiscard]] std::expected<std::vector<cyclone::AlternateInfo>,
                              cyclone::CacheError>
  ListAlternates(std::string_view url, std::string_view hostname,
                 std::string_view scheme, std::string_view site = {});

  // Cache statistics.
  cyclone::CacheStats Stats() const;

  // Stop the cache, delete the volume file, and recreate from scratch.
  // Thread-safe: acquires an exclusive lock, blocking until all
  // in-flight read/write operations complete.
  // Increments the generation file ({volume_path}.gen) on success so
  // that other processes (e.g., nginx) can detect the reset.
  //
  // Fails, without changing the generation, when the volume files could not
  // be removed (see RemoveVolumeFiles): the volume is reattached so this
  // object keeps serving its old entries, CacheError::Busy is returned, and
  // `detail`, when given, receives an operator-facing explanation.  If no
  // cache could be reopened at all, the object answers NotInitialized (or a
  // miss) from every method until a later reset succeeds.
  [[nodiscard]] std::expected<void, cyclone::CacheError> ResetVolume(
      std::string* detail = nullptr);

  // Read the generation counter from a .gen file.
  // Returns 0 if the file is missing or unreadable.
  static uint64_t ReadGenerationFile(const std::string& path);

  // Actual on-disk path of the (single, default-tier) Cyclone volume file.
  // Cyclone's structural-fingerprint filenames mean this generally differs
  // from the configured volume_path; anything that touches the file itself
  // (a sendfile FD, permissions, deletion) must use this.  Returns a copy
  // taken under the reset lock — ResetVolume() replaces the underlying file.
  [[nodiscard]] std::string VolumeFilePath() const;

  // Delete the on-disk files Cyclone could open for volume_path IN THE FORMAT
  // THIS BUILD RUNS: the raw legacy path, its ".small" sibling, and any
  // structural-fingerprint-shaped sibling of this format
  // ("<stem>-<current format major>-<16 hex><ext>", both shapes) in
  // volume_path's directory.  Clean-slate semantics for reset/recovery, within
  // that boundary: a fingerprint-named volume written in ANOTHER format is not
  // this build's data and survives, so a purge or a recovery cannot destroy the
  // file a rolled-back build would reopen.  Deliberately NARROWER than Cyclone's
  // own GC matcher, which accepts every major — do not substitute that matcher
  // (or its gc_superseded_on_start) for this one without deciding the
  // cross-format question again.  Like it, this assumes the directory is owned
  // exclusively by this cache path.  Never touches the ".gen" sidecar.
  //
  // Consequence: nothing in this process reclaims another format's volume.
  // Deleting it is an operator step, and the upgrade guide says so.
  //
  // Returns false when a file that exists could not be removed (on Windows,
  // typically because another process has it open); the first such path is
  // stored in `failed_path` when given, and the operating system's reason in
  // `failure_message`.  A file that is already absent counts
  // as removed.  The remaining files are still attempted.
  static bool RemoveVolumeFiles(const std::string& volume_path,
                                std::string* failed_path = nullptr,
                                std::string* failure_message = nullptr);

  // Classify a Create() failure: true iff the on-disk volume itself is
  // damaged, so a delete-and-recreate retry (RemoveVolumeFiles + Create)
  // has a chance to succeed.  Only Corrupted qualifies: a file too small
  // to hold a volume header, or a header that fails validation.  At the
  // pinned storage layer such a volume is reset in place inside Create()
  // when no other process holds it, so the error does not surface today;
  // the classification exists so a build that reports the damage directly
  // still recovers.
  //
  // False covers everything a delete cannot fix and must not be used to
  // fix.  ResetRefusedLivePeer and IncompatibleVersion both mean another
  // process may hold the volume open: the storage layer refuses to reset
  // under a live peer on purpose, and its remaining IncompatibleVersion
  // path on open fires only when a peer raced this open in.  Deleting the
  // files in either case would split the two processes onto different
  // volumes with no reset signal, silently losing the peer's writes until
  // it restarts, so the caller logs the reason and degrades instead.
  // IoError is excluded as a whole because the ownership, permission and
  // symlink refusals share it with genuine I/O failures
  // (CaptureVolumePathsAndChmod reports its refusals as IoError, and so does
  // the storage layer's own open), so the code alone cannot tell them apart
  // and a refusal must never be "fixed" by deleting the file it refuses.
  // The rest are configuration, geometry and contention errors.
  static bool OpenFailureIsDamagedVolume(cyclone::CacheError error);

  // Create(), then, only when the open failed because the volume is damaged
  // (OpenFailureIsDamagedVolume), RemoveVolumeFiles + one more Create().
  // This is the recovery the serving module runs: any other failure (a volume
  // another live process holds, a refusal, an I/O or configuration error) is
  // returned as it is, with every file left in place, so the caller can log
  // the reason and run without the cache.  `deleted_damaged_volume`, when
  // given, is set to whether the files were removed.  `first_error`, when
  // given, receives the error of the first open on failure.
  [[nodiscard]] static std::expected<std::unique_ptr<PageSpeedCache>,
                                     cyclone::CacheError>
  CreateRecreatingDamagedVolume(const PageSpeedCacheConfig& config,
                                bool* deleted_damaged_volume = nullptr,
                                cyclone::CacheError* first_error = nullptr);

  // Total capacity of the backing volume(s), in bytes.
  //
  // This is deliberately a value and not the cache: it is the whole of what
  // production ever needed from the substrate directly, and handing out
  // `cyclone::Cache&` to get it published the one operation that breaks this
  // class's central invariant (a whole-key write orphans the alternate chain).
  // A narrow accessor makes the disjointness above a property of the type
  // rather than of everybody's good behaviour — there is no longer a public
  // path to the raw handle to audit.  Tests that need one go through
  // PageSpeedCacheTestPeer (test/test_util/cache_test_peer.h), which is the
  // point: reaching past the class is now something you have to link a
  // test-only target to do.
  uint64_t VolumeCapacityBytes() const;

 private:
  PageSpeedCache() = default;

  // Raw substrate access, for tests only — see VolumeCapacityBytes().
  friend class PageSpeedCacheTestPeer;
  cyclone::Cache& RawCycloneCacheForTesting() { return *cache_; }

  // Shared removal passes for RemoveAlternatesExcept and
  // RemoveDerivedAlternates.  The caller holds reset_mutex_ (shared) and has
  // validated its own preserve rules; `is_preserved` decides per alternate id.
  [[nodiscard]] std::expected<size_t, cyclone::CacheError>
  RemoveAlternatesWhereLocked(
      std::string_view url, std::string_view hostname, std::string_view scheme,
      std::string_view site,
      const std::function<bool(AlternateId)>& is_preserved, const char* label);

  // Compose a CacheKey from URL, hostname, and scheme (normalizes hostname).
  cyclone::CacheKey ComposeKey(std::string_view url, std::string_view hostname,
                               std::string_view scheme, std::string_view site);

  // Drop the node currently carrying `id` on this key's chain, and this
  // process's RAM copy of it, so that the write about to follow REPLACES
  // rather than adds.
  //
  // Shared implementation for classes whose entry means "the current one for
  // this URL" — carried by the two dedicated writers (0x0C durable originals,
  // 0x6C headers sidecar) and by the generic ones (WriteSentinel,
  // WriteAgentAlternate), wired through here by #1312.  The reason is the
  // substrate's and not any class's: at the pinned
  // storage version a write to an id that already exists LINKS ANOTHER node
  // under the same id.  Reads still find the newest, so nothing looks wrong —
  // while the chain grows by a node per re-store, against a traversal ceiling
  // whose exhaustion stops the whole KEY from accepting alternate writes.
  //
  // An unlink that fails with anything but not-found leaves the old node
  // linked, so the write that follows grows the chain.  That is counted
  // (`unlink_failed`) and logged, and the caller proceeds anyway:
  // availability first — an entry that is current beats no entry — and the
  // alternative converts a bounded depth cost into losing the class for that
  // URL.  What is not acceptable is doing it silently.
  //
  // PRECONDITION: the caller holds `reset_mutex_` (every public writer does).
  // `class_name` names the class in the log line; `unlink_failed` is the
  // per-class counter to bump.
  void UnlinkSupersededAlternate(const cyclone::CacheKey& key, AlternateId id,
                                 std::string_view url,
                                 std::string_view class_name,
                                 std::atomic<uint64_t>* unlink_failed);

  // How many nodes on this key's chain carry `id`.  Returns 0 for a key
  // with no alternates yet — the first store for a URL lands there by
  // design, and its depth is knowably zero.  nullopt when the listing
  // itself FAILED, which is not the same as zero and must not be counted as
  // "no accumulation" — that failure is logged here (with `url` and
  // `class_name`), so callers may treat nullopt as "unmeasured this time"
  // without adding their own arm.
  //
  // This is the measurement behind the superseded-node counters: the depth is
  // READ rather than inferred from the code path, because an unmeasured
  // limitation is indistinguishable from an absent one.  Costs one chain
  // walk.  PRECONDITION: the caller holds `reset_mutex_`.
  std::optional<size_t> CountChainNodes(const cyclone::CacheKey& key,
                                        AlternateId id, std::string_view url,
                                        std::string_view class_name);

  // The configured durable-original cap, with 0 resolved to the default.
  uint64_t EffectiveMaxOriginalContentLength() const {
    return config_.max_original_content_length != 0
               ? config_.max_original_content_length
               : kDefaultMaxOriginalContentLength;
  }

  // Guards cache_ against concurrent access during ResetVolume().
  // All public methods acquire a shared lock; ResetVolume() acquires
  // an exclusive lock, ensuring no operations are in-flight when the
  // underlying Cyclone cache is destroyed and recreated.
  mutable std::shared_mutex reset_mutex_;

  // Write the current generation_ to {volume_path}.gen atomically.
  // Returns true on success, false if the file could not be written.
  bool WriteGenerationFile();

  // Record the actual volume file path(s) from cache_ and chmod them 0660
  // (multi-process: the daemon and its cache-sharing peers in group
  // `pagespeed` need read/write access; the world needs none).
  //
  // Returns false if any volume file's mode could not be set.  That is FATAL
  // to the caller, not a warning: the mode is applied through a descriptor
  // opened O_NOFOLLOW, so the most likely failure is ELOOP — the path is a
  // symlink, which means the file the cache is about to use is not the file
  // whose name it was given.  Continuing would hand the cache an
  // attacker-chosen path with a mode nobody vouched for.
  [[nodiscard]] bool CaptureVolumePathsAndChmod();

  std::unique_ptr<cyclone::Cache> cache_;
  // Actual on-disk volume file path(s), captured after every (re)create —
  // see VolumeFilePath().
  std::vector<std::string> volume_file_paths_;
  PageSpeedCacheConfig config_;
  PageSpeedSelector selector_;
  MessageHandler* handler_ = nullptr;  // Diagnostic logger (not owned).
  // Keep the Cyclone config alive for the lifetime of the cache.
  // Cyclone may hold a reference to the config passed to create().
  cyclone::CacheConfig cyclone_config_;
  uint64_t generation_ = 0;
  // Bumped by WriteOriginalAlternate (declared over the cap) and by the
  // WriteResult it hands out (cut off mid-stream).  Address is handed to
  // those write handles, so it must not move for the cache's lifetime.
  std::atomic<uint64_t> originals_over_cap_skipped_{0};
  // Bumped by WriteOriginalAlternate: the first when a re-record arrives to
  // find a superseded node still on the chain, the second when the unlink of
  // the previous original reported an error meaning it may still be linked.
  // Read by nothing but their accessors — see the discipline stated there.
  std::atomic<uint64_t> originals_superseded_observed_{0};
  std::atomic<uint64_t> originals_unlink_failed_{0};
  // Bumped by WriteHeadersSidecar: the first two when the offered response is
  // not carried (disjoint classes), the third when the chain still carried a
  // superseded node after the store, the fourth when the unlink reported an
  // error that means the old node may still be linked.  Read by nothing but
  // the accessors above — see the discipline stated there.
  std::atomic<uint64_t> headers_sidecar_fall_through_{0};
  std::atomic<uint64_t> headers_sidecar_never_optimized_{0};
  std::atomic<uint64_t> headers_sidecar_superseded_observed_{0};
  std::atomic<uint64_t> headers_sidecar_unlink_failed_{0};
  // Bumped by WriteSentinel and WriteAgentAlternate respectively: the first of
  // each pair when a re-record arrives to find a superseded node still on the
  // chain, the second when the unlink reported an error meaning it may still
  // be linked.  Same signals as the originals pair, per generic writer rather
  // than per sentinel id — the log line names the id.  Read by nothing but
  // their accessors — see the discipline stated there.
  std::atomic<uint64_t> sentinels_superseded_observed_{0};
  std::atomic<uint64_t> sentinels_unlink_failed_{0};
  std::atomic<uint64_t> agent_markdown_superseded_observed_{0};
  std::atomic<uint64_t> agent_markdown_unlink_failed_{0};
};

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_CACHE_H_
