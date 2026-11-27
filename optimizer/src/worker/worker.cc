// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - PSOL Factory Worker Implementation

#include "src/worker/worker.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "lib/base/message_handler.h"
#include "lib/base/string_util.h"
#include "lib/base/string_writer.h"
#include "lib/base/tee_message_handler.h"
#include "lib/base/url_util.h"
#include "lib/cache/html_withheld_marker.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"
#include "lib/classify/option_context.h"
#include "lib/classify/url_normalizer.h"
#include "lib/css/css_import_flattener.h"
#include "lib/css/css_minify.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_parse.h"
#include "lib/html/html_writer_filter.h"
#include "lib/html/markdown_extractor_filter.h"
#include "lib/image/content_analyzer.h"
#include "lib/image/image_util.h"
#include "lib/image/svg_vectorizer.h"
#include "lib/js/js_minify.h"
#include "lib/js/js_tokenizer.h"
#include "lib/net/fetch_policy.h"
#include "lib/net/upstream_pin.h"
#include "nlohmann/json.hpp"
#include "sha256.hpp"
#include "src/browser/agent_fetcher.h"
#include "src/browser/browser_css_extractor.h"
#include "src/browser/optimization_profile.h"
#include "src/browser/template_detector.h"
#include "src/product_version/version.h"
#include "src/worker/api_handlers.h"
#include "src/worker/browser_analysis_manager_internal.h"
#include "src/worker/cache_dir.h"
#include "src/worker/cache_handlers.h"
#include "src/worker/capture_handlers.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/host_aliases.h"
#include "src/worker/html_css_injector.h"
#include "src/worker/html_scanner.h"
#include "src/worker/html_transform_filter.h"
#include "src/worker/http_server.h"
#include "src/worker/known_sites.h"
#include "src/worker/layer_order_sources.h"
#include "src/worker/llms_txt_builder.h"
#include "src/worker/pipe_dacl.h"
#include "src/worker/posix_compat.h"
#include "src/worker/serve_stats.h"
#include "src/worker/shared_config.h"
#include "src/worker/static_file_handler.h"
#include "src/worker/syscall_filter.h"
#include "src/worker/text_compressor.h"
#include "src/worker/unsafe_force_async_css.h"
#include "src/worker/url_registry.h"
#include "src/worker/uv_helpers.h"
#include "src/worker/ws_handlers.h"

#ifdef _WIN32
#include <aclapi.h>  // SetSecurityInfo
#include <sddl.h>    // ConvertStringSecurityDescriptorToSecurityDescriptorA
#endif

namespace pagespeed {

const char* CooldownReasonToString(CooldownReason r) {
  switch (r) {
    case CooldownReason::kProcessing:
      return "processing";
    case CooldownReason::kWriteFailure:
      return "write_failure";
    case CooldownReason::kRevalidation:
      return "revalidation";
  }
  return "unknown";
}

namespace {

// True when two page URLs name the same page for the purpose of a template's
// validation record. By the time the serve path compares, notification.url is
// the normalized path-and-query form, and the record's analyzed_url is the
// notification URL the analysis was queued with (the same form), so both are
// compared as path and query: any scheme and authority are dropped, and so is
// a fragment. The profile carries no host, so two hosts sharing a template and
// a path compare equal; the per-template revalidation budget bounds that.
bool IsAnalyzedPage(std::string_view url, std::string_view analyzed_url) {
  auto path_and_query = [](std::string_view u) {
    size_t scheme_end = u.find("://");
    if (scheme_end != std::string_view::npos) {
      u.remove_prefix(scheme_end + 3);
      size_t path = u.find_first_of("/?");
      u = path == std::string_view::npos ? std::string_view("/")
                                         : u.substr(path);
    }
    size_t fragment = u.find('#');
    if (fragment != std::string_view::npos) u = u.substr(0, fragment);
    return u;
  };
  return !analyzed_url.empty() &&
         path_and_query(url) == path_and_query(analyzed_url);
}

// Minimal RAII scope-exit guard (the abseil cleanup utility is not vendored
// in this tree).  Runs the wrapped callable when the guard goes out of scope,
// on every return path.  Used by HandleNotification to refresh the cached
// alternate count regardless of which branch returns.
template <typename F>
class ScopeExit {
 public:
  explicit ScopeExit(F f) : f_(std::move(f)) {}
  ~ScopeExit() { f_(); }
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ScopeExit(ScopeExit&&) = delete;
  ScopeExit& operator=(ScopeExit&&) = delete;

 private:
  F f_;
};

#ifdef _WIN32
// The pipes' access rules (src/worker/pipe_dacl.h): notification and health
// pipes open to Authenticated Users for reading and writing only (matching
// the POSIX default: nginx workers run as a different user); the management
// pipe restricted to SYSTEM, Administrators and the worker's own account
// (matching POSIX chmod 0660: PURGE commands restricted).

// Apply a DACL to a libuv named pipe after bind, before listen. Uses
// uv_fileno() to retrieve the underlying Windows HANDLE. Returns false on
// failure (caller decides whether to treat as fatal).
static bool SetPipeDacl(uv_pipe_t* pipe, const std::string& sddl,
                        MessageHandler* handler, const char* label) {
  uv_os_fd_t fd;
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(pipe), &fd) != 0) {
    if (handler) handler->Warning("Failed to get handle for %s pipe", label);
    return false;
  }

  PSECURITY_DESCRIPTOR sd = nullptr;
  if (sddl.empty() || !ConvertStringSecurityDescriptorToSecurityDescriptorA(
                          sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
    if (handler) handler->Warning("Failed to parse SDDL for %s pipe", label);
    return false;
  }

  BOOL dacl_present = FALSE, dacl_defaulted = FALSE;
  PACL dacl = nullptr;
  if (!GetSecurityDescriptorDacl(sd, &dacl_present, &dacl, &dacl_defaulted)) {
    if (handler) handler->Warning("Failed to read DACL for %s pipe", label);
    LocalFree(sd);
    return false;
  }

  DWORD err = ERROR_INVALID_PARAMETER;
  if (dacl_present && dacl) {
    err = SetSecurityInfo(reinterpret_cast<HANDLE>(fd), SE_KERNEL_OBJECT,
                          DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
                          nullptr);
  }

  LocalFree(sd);
  if (err != ERROR_SUCCESS) {
    if (handler) {
      handler->Warning("Failed to set DACL on %s pipe (error %lu)", label,
                       static_cast<unsigned long>(err));
    }
    return false;
  }
  return true;
}
#endif  // _WIN32

// Callback for closing handles during cleanup
void CloseWalkCallback(uv_handle_t* handle, void* /*arg*/) {
  SafeClose(handle);
}

// Context for health check write requests
struct HealthWriteContext {
  uv_write_t req;
  uv_buf_t buf;
  uv_pipe_t* client;
};

// Context for management socket write requests
struct MgmtWriteContext {
  uv_write_t req;
  uv_buf_t buf;
  uv_pipe_t* client;
  bool keep_alive = false;  // Keep connection open after response (for AUTH)
};

// Context for thread pool work items (notification processing).
struct NotificationWorkContext {
  uv_work_t req;
  Worker* worker;
  CacheNotification notification;  // Owned copy
  std::string dedup_key;           // For in_flight_urls_ removal
  PurgeDispatchGen purge_gen;      // Purge-fence baseline at dispatch time
};

// Context for management socket connections
struct MgmtClientContext {
  Worker* worker;
  std::string buffer;
  bool closing = false;        // guard against double-close
  bool authenticated = false;  // AUTH token accepted on this connection
};

// Convert CapabilityMask to AlternateId (low byte of encoded mask).
AlternateId MaskToId(const CapabilityMask& mask) {
  return MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
}

// Normalize a notification's capability mask for dedup purposes: strip
// transfer-encoding and image-format bits (which don't affect content
// processing).  This matches the normalization in the dedup check at
// HandleNotification entry.
CapabilityMask NormalizeMaskForDedup(uint32_t encoded_mask) {
  CapabilityMask m = CapabilityMask::Decode(encoded_mask);
  m.set_transfer_encoding(CapabilityMask::TransferEncoding::kIdentity);
  m.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  return m;
}

// The dedup id an image notification is checked and recorded under: the
// full (format, viewport, density, save-data) slot, transfer-encoding
// stripped (image variants are always stored identity).  Unlike the text
// types above, an image's format IS a different piece of work, so it stays
// in the key: an entry for one format never answers "already processed"
// for a format no pass has decided.  A pass marks every format it decided
// for its slot (mark_image_slot_decided in HandleNotification), which is
// what keeps a notification for a format the pass refused (for instance a
// WebP not smaller than the JPEG at the same size) from re-running the
// pass.
CapabilityMask ImageDedupMask(uint32_t encoded_mask) {
  CapabilityMask m = CapabilityMask::Decode(encoded_mask);
  m.set_transfer_encoding(CapabilityMask::TransferEncoding::kIdentity);
  return m;
}

// The dedup mask of any notification: the image slot for an image, the
// format-stripped mask for every other type.
CapabilityMask DedupMaskFor(ContentType type, uint32_t encoded_mask) {
  return type == ContentType::kImage ? ImageDedupMask(encoded_mask)
                                     : NormalizeMaskForDedup(encoded_mask);
}

// The raster formats one image pass decides for its (viewport, density,
// save-data) slot: WriteImageVariants builds, refuses or finds present every
// one of them, and the passthrough branches answer all of them alike.
constexpr CapabilityMask::ImageFormat kImageSlotFormats[] = {
    CapabilityMask::ImageFormat::kOriginal,
    CapabilityMask::ImageFormat::kWebP,
    CapabilityMask::ImageFormat::kAvif,
};

// The flag bits a variant inherits from the entry it is derived from.  A hit
// reads the selected variant, never the identity, so the record-time state
// stamped on the source entry (kFlagHeadersClassified,
// kFlagHeadersSidecarRequired, kFlagAnonymousRecorded) has to ride every
// variant the worker builds.  Every variant-metadata site goes through here
// so a future inherited bit is added in one place.  Agent markdown is built
// from fresh metadata and does not inherit them.
constexpr uint8_t kVariantInheritedFlags =
    AlternateMetadata::kFlagHeadersClassified |
    AlternateMetadata::kFlagHeadersSidecarRequired |
    AlternateMetadata::kFlagAnonymousRecorded;

uint8_t InheritedVariantFlags(const AlternateMetadata& source) {
  return source.flags & kVariantInheritedFlags;
}

// Whether a refreshed origin reference may carry the existing variant set
// forward by restamping it in place.  It may not when the response forbids
// shared storage, and it may not when the response now negotiates on
// `Accept` (kFlagOriginVariesAccept): a marked origin is never optimized, so
// any variant set already stored was derived while the URL was an ordinary
// resource and would otherwise keep being served in place of the origin's own
// negotiated representation.  A restamp is the "same bytes, same identity"
// verdict; a flipped Vary set is a new identity, so the caller purges.
bool CanRestampVariantSet(const AlternateMetadata& fresh_origin_meta) {
  const bool storage_forbidden = (fresh_origin_meta.origin_cc_flags &
                                  (AlternateMetadata::kCCOriginNoStore |
                                   AlternateMetadata::kCCOriginPrivate)) != 0;
  const bool negotiates_by_accept =
      (fresh_origin_meta.flags & AlternateMetadata::kFlagOriginVariesAccept) !=
      0;
  return !storage_forbidden && !negotiates_by_accept;
}

// Build variant metadata from the original read result's metadata.
AlternateMetadata CreateWorkerMetadata(const AlternateMetadata& base,
                                       ContentType type, uint8_t flags,
                                       size_t origin_content_length) {
  AlternateMetadata m = base;
  m.content_type = type;
  // The score field does not carry over from the entry this one is built from:
  // for text it is the variant's own encoded size (set when the coded copies
  // are written), for images the score of the new variant.
  m.ssimulacra2_score_x100 = AlternateMetadata::kScoreNA;
  m.flags = flags | InheritedVariantFlags(base);
  m.origin_content_length = static_cast<uint32_t>(
      std::min(origin_content_length, size_t{UINT32_MAX}));
  return m;
}

// Stamp the SHA-256 of the RAW origin input onto a variant's metadata
// (issue #434): the tier-1 identity of the front ends' derived ETag.  The
// caller owns the gate: stamp only when the input entry's flags lack
// kFlagWorkerProcessed, i.e. the hashed bytes are the origin's own and not
// the worker's earlier output (the Issue-B trap: hashing optimized bytes
// here would re-bind every re-optimization and poison the agent-markdown
// equality gate).  A worker-processed input keeps the inherited stamp,
// which CreateWorkerMetadata and every `meta = origin_meta` copy already
// carry forward, so the binding stays pinned to the true origin revision
// across re-optimizations.
void StampSourceBinding(AlternateMetadata* m,
                        const std::array<std::byte, 32>& h) {
  m->origin_html_hash = h;
}

// Detect gzip-compressed content by checking the magic bytes (RFC 1952).
bool IsGzipCompressed(std::string_view content) {
  return content.size() >= 2 && static_cast<uint8_t>(content[0]) == 0x1f &&
         static_cast<uint8_t>(content[1]) == 0x8b;
}

// Compose internal key for dedup/purge/cooldown tracking.
// Includes scheme for cache-key consistency.
// Separator safety: pipe (|) is unambiguous here because hostnames
// cannot contain pipe (DNS label charset) and scheme is constrained
// to "http"/"https".  If any component admits user-controlled data
// with pipes, switch to a multi-byte or null-byte separator.
//
// A non-empty site (64 lowercase hex, validated on receipt) appends
// "|s=" + site, so each site's state is its own; an empty site gives the
// unsited key unchanged.  The prefix clears that build "url|host|scheme|" or
// "url|host|" by hand match every site's keys for the URL on purpose.
std::string ComposeInternalKey(std::string_view url, std::string_view hostname,
                               std::string_view scheme, std::string_view site) {
  if (site.empty()) return absl::StrCat(url, "|", hostname, "|", scheme);
  return absl::StrCat(url, "|", hostname, "|", scheme, "|s=", site);
}

// Compose internal key with alternate ID suffix (for processed-set).  The
// site, when there is one, comes before the id.
std::string ComposeInternalKeyWithId(std::string_view url,
                                     std::string_view hostname,
                                     std::string_view scheme,
                                     std::string_view site, AlternateId id) {
  return absl::StrCat(ComposeInternalKey(url, hostname, scheme, site), "|",
                      static_cast<int>(id));
}

// Retry policy for cross-process mmap propagation on cold start.
// Exponential backoff: 10ms, 50ms, 250ms (total worst-case ~310ms).
static constexpr int kCacheReadMaxRetries = 3;
static constexpr int kCacheReadInitialDelayMs = 10;
static constexpr int kCacheReadBackoffMultiplier = 5;

// One attempt at reading a URL's OPTIMIZATION SOURCE: the bytes the worker
// transforms.  Two reads, in a fixed order, and the order is the contract.
//
//   1. ReadBestAlternate — the selector path.  Unchanged, and still first:
//      whatever this build prefers as a source today it still prefers.
//   2. ReadOriginalAlternate — the durable original (0x0C) by EXACT ID, and
//      ONLY when (1) found nothing usable.
//
// WHY (2) HAS TO EXIST.  The durable-original class is deliberately never
// selectable: PageSpeedSelector::select skips it unconditionally, so that
// "the original" can never compete with the optimized variants at serve time
// (lib/classify/pagespeed_selector.cc).  That is right for SERVING and stays
// exactly as it is.  But the worker's source read went through the same
// selector — so an external writer using the published C ABI
// (ps_cache_write_original) that records the origin's bytes into that class
// and notifies got a cache that fills and an optimizer that never runs.  The
// notification was accepted, the source read missed, and the URL stayed
// unoptimized forever, with the miss indistinguishable in the log from a URL
// with no entry at all.
//
// WHY (2) DOES NOT WEAKEN THE SERVING GUARANTEE.  This is not a selection.
// An exact-id read is a caller naming the class it wants; the guarantee the
// skip provides is about what SCORING can reach, and scoring is not involved
// here.  The selector is byte-for-byte unchanged, no request of any shape can
// reach the class through it, and the worker never serves — it reads a source
// and writes variants under ordinary capability ids (WriteVariant stamps
// meta.full_mask = mask.Encode(), which is never a sentinel).
//
// FRESHNESS.  No freshness gate here, deliberately, and this matches the
// class contract rather than skipping it.  The class's lifetime is
// origin-freshness-bound, but that binding is enforced by whoever EVALUATES
// an entry against its own stamped origin state, not by the read: the
// deliberate-read API refuses exactly two things — a missing metadata prefix
// and a prefix that does not self-describe as this class — and returns a
// past-its-window original like any other (lib/cache/cache.cc,
// ReadOriginalAlternateByKey).  The worker does not need to re-decide it,
// because it cannot launder it: WriteVariant copies the source's origin state
// (cache_inserted_at, max-age, s-maxage, the Cache-Control flags) into every
// variant it writes, so a variant built from a past-its-window original is
// itself past its window and the serve-side evaluation reaches the same
// verdict it would have reached on the original.  Gating here would also be
// strictly stronger than the selector path this falls back FROM, which
// applies no freshness test either — and it would silently disable the
// fallback for any writer that leaves cache_inserted_at unstamped, since an
// unstamped entry reads as epoch-old by design.  Genuinely superseded
// originals are removed at the source: an origin refresh drops the original
// with the rest of the family (RemoveAlternatesExcept).
//
// SCOPE, stated because the boundary is not obvious and a reader will assume
// it is wider than it is.  This covers the read of the NOTIFIED resource: the
// four HandleNotification content-type branches and the warmup image branch.
// It does NOT cover the SUB-RESOURCE reads that optimizing a page performs —
// the combined-CSS gather and external-stylesheet read (BuildCombinedCss), the
// @import flatten lookups, the image-dimension read in the HTML transform
// filter, and the browser-analysis manager's HTML/CSS/image reads.  Those
// resolve a DIFFERENT URL than the one notified and remain selector-only, so
// on a front end that stores only durable originals a page's external
// stylesheet reads as absent until that stylesheet is itself notified.  That
// converges rather than sticking — the worker's own output IS selectable, so
// the first successful pass fixes the URL for good — but until it does, the
// HTML variant is written needing revalidation and gets re-processed on the
// cooldown.  Widening the fallback to those sites is a separate change with a
// different blast radius (the browser-analysis and dimension reads have their
// own correctness stakes) and is tracked separately; it is deliberately not
// bundled here.
auto ReadSourceOnce(PageSpeedCache* cache, const std::string& url,
                    const std::string& hostname, std::string_view scheme,
                    std::string_view site, WorkerStats& stats,
                    MessageHandler* handler)
    -> decltype(cache->ReadBestAlternate(url, hostname, scheme,
                                         CapabilityMask(),
                                         /*agent_request_entitled=*/false,
                                         site)) {
  auto selected =
      cache->ReadBestAlternate(url, hostname, scheme, CapabilityMask(),
                               /*agent_request_entitled=*/false, site);
  if (selected.has_value()) return selected;

  // The durable original is recorded by the front end, often another
  // process.  Cyclone's RAM tier is write-around and process-local, so an
  // earlier read here would keep answering with the superseded bytes.
  cache->EvictAlternateFromRamCache(
      url, hostname, scheme,
      static_cast<AlternateId>(SentinelId::kOriginalContent), site);
  auto original = cache->ReadOriginalAlternate(url, hostname, scheme, site);
  if (original.has_value()) {
    stats.source_reads_from_durable_original.fetch_add(
        1, std::memory_order_relaxed);
    if (handler != nullptr) {
      handler->Info(
          "No selectable alternate for %s; optimizing from the durable "
          "original read by exact id",
          url.c_str());
    }
    return original;
  }
  // Propagate the SELECTOR's error, not the fallback's: it is the one the
  // callers' miss messages are about, and the fallback's own miss is already
  // logged by ReadOriginalAlternate.
  return selected;
}

// Read the optimization source from cache with exponential-backoff retry.
// On cold start, cross-process mmap propagation may lag behind the
// notification delivery.  This retries with increasing delays to
// tolerate the worst case (first notification on a fresh cache).
//
// The durable-original fallback lives INSIDE the retry, i.e. inside
// ReadSourceOnce, rather than after the loop.  Both reads then get the same
// cold-start tolerance — the propagation lag the retry exists for is a
// property of the volume, not of one alternate id — and, more importantly, a
// cache whose entries are durable originals resolves on the FIRST attempt
// instead of paying the full ~310ms backoff on every single notification,
// which is what an after-the-loop fallback would cost the very deployment
// this fixes.
auto ReadOriginalWithRetry(PageSpeedCache* cache, const std::string& url,
                           const std::string& hostname, std::string_view scheme,
                           std::string_view site, WorkerStats& stats,
                           MessageHandler* handler)
    -> decltype(cache->ReadBestAlternate(url, hostname, scheme,
                                         CapabilityMask(),
                                         /*agent_request_entitled=*/false,
                                         site)) {
  stats.selector_invocations.fetch_add(1, std::memory_order_relaxed);
  auto result =
      ReadSourceOnce(cache, url, hostname, scheme, site, stats, handler);
  if (result.has_value()) return result;

  if (handler != nullptr) {
    handler->Warning("Cache read failed for %s (error=%s), retrying...",
                     url.c_str(),
                     make_error_code(result.error()).message().c_str());
  }

  int delay_ms = kCacheReadInitialDelayMs;
  for (int attempt = 0; attempt < kCacheReadMaxRetries; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    result = ReadSourceOnce(cache, url, hostname, scheme, site, stats, handler);
    if (result.has_value()) {
      stats.cache_read_retries.fetch_add(1, std::memory_order_relaxed);
      if (handler != nullptr) {
        handler->Info("Cache read succeeded on retry %d (%dms) for %s",
                      attempt + 1, delay_ms, url.c_str());
      }
      return result;
    }
    if (handler != nullptr) {
      handler->Info("Cache read retry %d (%dms) failed for %s: %s", attempt + 1,
                    delay_ms, url.c_str(),
                    make_error_code(result.error()).message().c_str());
    }
    delay_ms *= kCacheReadBackoffMultiplier;
  }
  stats.cache_read_failures.fetch_add(1, std::memory_order_relaxed);
  return result;
}

// Callback type for purge-generation check.  Returns true if the URL was
// purged after the notification was dispatched, meaning the write should be
// skipped to avoid re-populating stale content.
using PurgeCheck = std::function<bool()>;

// Write content to cache as an alternate. Returns true on success.
// The origin_meta carries cache-control fields (cache_inserted_at,
// origin_max_age, origin_s_maxage, origin_cc_flags) from the original
// alternate; full_mask is overridden from the supplied mask.
// If purge_check is provided and returns true, the write is skipped.
bool WriteVariant(PageSpeedCache* cache, std::string_view url,
                  std::string_view hostname, std::string_view scheme,
                  std::string_view site, const CapabilityMask& mask,
                  std::span<const char> data,
                  const AlternateMetadata& origin_meta,
                  const PurgeCheck& purge_check = nullptr,
                  Worker* purge_undo_worker = nullptr) {
  if (purge_check && purge_check()) {
    return false;  // URL was purged after dispatch — skip write
  }
  auto id = MaskToId(mask);
  AlternateMetadata meta = origin_meta;  // Copy to preserve cc fields
  meta.full_mask = mask.Encode();
  // content_type, flags, origin_content_type are inherited from origin_meta
  auto wh =
      cache->WriteAlternate(url, hostname, scheme, id, data.size(), meta, site);
  if (!wh) return false;
  auto written =
      wh->write_sync(std::as_bytes(std::span(data.data(), data.size())));
  if (!written) return false;
  if (!wh->close_sync().has_value()) return false;
  // Cyclone's RAM tier is write-around (reads populate it, writes do NOT
  // evict): any earlier read of this (key, id) in THIS process — typically
  // the read of the original this variant replaces — keeps serving the
  // stale pre-overwrite bytes indefinitely (issue #652 class).  Evict the
  // RAM copy of the slot just committed so same-process readers observe
  // the fresh bytes.  Cross-process RAM tiers are out of reach by design;
  // the origin-refresh handler covers the front-end overwrite case the
  // same way.  Must run post-commit: evicting earlier would let a
  // concurrent read repopulate the RAM tier with the old bytes before the
  // new document is published.
  cache->EvictAlternateFromRamCache(url, hostname, scheme, id, site);
  // Post-commit re-check (issue #652): the pre-write check is
  // check-then-act — a purge can land between it and close_sync, in which
  // case the just-committed alternate resurrects content the purge was
  // meant to delete.  Because InvalidateUrl bumps the generation and
  // performs its Remove atomically (under purge_gen_mutex_), a racing
  // purge is observable here exactly when our write could have landed
  // after its Remove.  The undo must be semantically a PURGE, not a bare
  // whole-key Remove (issue #652 review): a bare Remove can wipe a
  // concurrently-built FRESH variant set (e.g. a post-purge notification
  // that already re-inserted dedup marks) without clearing those marks —
  // leaving an empty cache plus "already processed" dedup entries, i.e.
  // optimization permanently disabled for the URL.  InvalidateUrl bumps
  // the generation AND clears dedup/cooldowns, so any concurrent fresh
  // writer is itself fenced at its next check/MarkVariantProcessed and
  // the next notification rebuilds cleanly.  Report failure so the caller
  // does not mark this variant processed.
  if (purge_check && purge_check()) {
    if (purge_undo_worker != nullptr) {
      (void)purge_undo_worker->InvalidateUrl(
          std::string(url), std::string(hostname), std::string(scheme), site,
          /*clear_heal_state=*/false);
    } else {
      (void)cache->Remove(url, hostname, scheme, site);
    }
    return false;
  }
  return true;
}

// Issue #507: when the worker leaves an input unchanged it writes no variant,
// so the raw entry the front end serves would stay without a source binding
// and an origin that sends no validators would get no ETag.  Rewrite that
// entry, in the slot it was read from, with identical bytes, unchanged flags
// and the stamp.  Same rule as every other stamp: only a raw read (flags
// lack kFlagWorkerProcessed) is hashed, and an entry that already carries
// the right stamp is left alone.  A read from a sentinel id (the durable
// original) cannot be rewritten through the alternate path: skipped, not
// a failure.
enum class StampResult : std::uint8_t { kSkipped, kWritten, kFailed };

StampResult StampRawEntry(PageSpeedCache* cache, std::string_view url,
                          std::string_view hostname, std::string_view scheme,
                          std::string_view site,
                          const AlternateMetadata& read_meta,
                          std::string_view bytes,
                          const std::array<std::byte, 32>& hash,
                          const PurgeCheck& purge_check,
                          Worker* purge_undo_worker) {
  if ((read_meta.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
    return StampResult::kSkipped;
  }
  const CapabilityMask read_mask = CapabilityMask::Decode(read_meta.full_mask);
  if (IsSentinel(MaskToId(read_mask))) return StampResult::kSkipped;
  if (read_meta.origin_html_hash == hash) return StampResult::kSkipped;
  AlternateMetadata raw_meta = read_meta;
  StampSourceBinding(&raw_meta, hash);
  return WriteVariant(cache, url, hostname, scheme, site, read_mask,
                      std::span<const char>(bytes.data(), bytes.size()),
                      raw_meta, purge_check, purge_undo_worker)
             ? StampResult::kWritten
             : StampResult::kFailed;
}

// Count only real writes and real failures: a skip is neither.
void CountStamp(WorkerStats& stats, StampResult r) {
  if (r == StampResult::kWritten) {
    stats.alternate_writes.fetch_add(1, std::memory_order_relaxed);
  } else if (r == StampResult::kFailed) {
    stats.alternate_writes.fetch_add(1, std::memory_order_relaxed);
    stats.alternate_write_failures.fetch_add(1, std::memory_order_relaxed);
  }
}

// Check if a variant exists for a mask.
bool VariantExistsForMask(PageSpeedCache* cache, std::string_view url,
                          std::string_view hostname, std::string_view scheme,
                          std::string_view site, const CapabilityMask& mask) {
  return cache->AlternateExists(url, hostname, scheme, MaskToId(mask), site);
}

// True when both views name the very same bytes.
bool SameBytes(std::string_view a, std::string_view b) {
  return a.data() == b.data() && a.size() == b.size();
}

// Write gzip and/or brotli compressed variants of text content.
// The identity_mask should be the mask used for the uncompressed
// variant.  This creates up to 2 additional alternates with
// TransferEncoding set to kGzip/kBrotli.  The origin_meta carries
// cache-control fields from the original alternate.
void WriteCompressedVariants(PageSpeedCache* cache, std::string_view url,
                             std::string_view hostname, std::string_view scheme,
                             std::string_view site,
                             const CapabilityMask& identity_mask,
                             std::string_view data, std::string_view origin_raw,
                             const AlternateMetadata& origin_meta,
                             int gzip_level, int brotli_level,
                             WorkerStats& stats,
                             const PurgeCheck& purge_check = nullptr,
                             Worker* purge_undo_worker = nullptr) {
  if (gzip_level > 0) {
    auto gz = GzipCompress(data, gzip_level);
    if (!gz.empty()) {
      CapabilityMask gz_mask = identity_mask;
      gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
      AlternateMetadata gz_meta = origin_meta;
      gz_meta.full_mask = gz_mask.Encode();
      gz_meta.ClearEncodedOriginLength();
      if (!origin_raw.empty()) {
        const size_t coded = SameBytes(origin_raw, data)
                                 ? gz.size()
                                 : GzipCompress(origin_raw, gzip_level).size();
        if (coded > 0) gz_meta.SetEncodedOriginLength(coded);
      }
      stats.alternate_writes.fetch_add(1, std::memory_order_relaxed);
      if (WriteVariant(cache, url, hostname, scheme, site, gz_mask,
                       std::span<const char>(gz.data(), gz.size()), gz_meta,
                       purge_check, purge_undo_worker)) {
        stats.variants_written.fetch_add(1, std::memory_order_relaxed);
        stats.gzip_variants_written.fetch_add(1, std::memory_order_relaxed);
      } else if (purge_check && purge_check()) {
        // Issue E: benign purge-fence, not a hard failure.
        stats.alternate_writes_fenced.fetch_add(1, std::memory_order_relaxed);
      } else {
        stats.alternate_write_failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  if (brotli_level > 0) {
    auto br = BrotliCompress(data, brotli_level);
    if (!br.empty()) {
      CapabilityMask br_mask = identity_mask;
      br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
      AlternateMetadata br_meta = origin_meta;
      br_meta.full_mask = br_mask.Encode();
      br_meta.ClearEncodedOriginLength();
      if (!origin_raw.empty()) {
        const size_t coded =
            SameBytes(origin_raw, data)
                ? br.size()
                : BrotliCompress(origin_raw, brotli_level).size();
        if (coded > 0) br_meta.SetEncodedOriginLength(coded);
      }
      stats.alternate_writes.fetch_add(1, std::memory_order_relaxed);
      if (WriteVariant(cache, url, hostname, scheme, site, br_mask,
                       std::span<const char>(br.data(), br.size()), br_meta,
                       purge_check, purge_undo_worker)) {
        stats.variants_written.fetch_add(1, std::memory_order_relaxed);
        stats.brotli_variants_written.fetch_add(1, std::memory_order_relaxed);
      } else if (purge_check && purge_check()) {
        // Issue E: benign purge-fence, not a hard failure.
        stats.alternate_writes_fenced.fetch_add(1, std::memory_order_relaxed);
      } else {
        stats.alternate_write_failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

// Bind the stored gzip/brotli copies to the bytes they were built from, so a
// later re-record of the original can tell "same bytes" from "new bytes"
// (RecheckRecordedOriginal).  Only a front end's own recording may seed the
// oracle (|genuine|), and only once every coded copy the configuration enables
// exists (a level of 0 disables that encoding, so its copy is never expected)
// and no purge has fenced the writes.  |hash| is the SHA-256 of the raw
// source, computed by the caller (hoisted once per run, issue #434: the same
// value also stamps the variant metadata's source binding).
void RecordSourceHash(PageSpeedCache* cache, std::string_view url,
                      std::string_view hostname, std::string_view scheme,
                      std::string_view site,
                      const CapabilityMask& identity_mask,
                      const std::array<std::byte, 32>& hash, bool genuine,
                      int gzip_level, int brotli_level,
                      const PurgeCheck& purge_check) {
  if (!genuine || (purge_check && purge_check())) return;
  CapabilityMask gz_mask = identity_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  CapabilityMask br_mask = identity_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  if (gzip_level > 0 &&
      !VariantExistsForMask(cache, url, hostname, scheme, site, gz_mask)) {
    return;
  }
  if (brotli_level > 0 &&
      !VariantExistsForMask(cache, url, hostname, scheme, site, br_mask)) {
    return;
  }
  auto wh = cache->WriteSentinel(url, hostname, scheme,
                                 SentinelId::kContentHash, 32, site);
  if (wh) {
    (void)wh->write_sync(std::as_bytes(std::span(hash.data(), hash.size())));
    (void)wh->close_sync();
    cache->EvictAlternateFromRamCache(
        url, hostname, scheme,
        static_cast<AlternateId>(SentinelId::kContentHash), site);
  }
}

// Map net_instaweb::ImageType (magic-byte detection) to the
// pagespeed::ImageType enum used by EvaluateSvgCandidacy().
ImageType ToSvgImageType(std::string_view image_data) {
  auto nit = net_instaweb::ComputeImageType(image_data);
  switch (nit) {
    case net_instaweb::IMAGE_JPEG:
      return ImageType::kJpeg;
    case net_instaweb::IMAGE_PNG:
      return ImageType::kPng;
    case net_instaweb::IMAGE_GIF:
      return ImageType::kGif;
    case net_instaweb::IMAGE_WEBP:
    case net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA:
    case net_instaweb::IMAGE_WEBP_ANIMATED:
      return ImageType::kWebP;
    default:
      return ImageType::kUnknown;
  }
}

// Check if an image URL is the LCP candidate for any cached page
// by scanning Early Hints sentinels.  Returns true if the URL
// appears as an "image:" prefixed line in any Early Hints data
// stored for the same hostname.  This is a best-effort heuristic
// because we only check the sentinel for the notification hostname.
bool IsLcpCandidate(PageSpeedCache* cache, std::string_view url,
                    std::string_view hostname) {
  // Read the Early Hints sentinel for all known pages is too expensive.
  // We can't feasibly check every HTML page's sentinel.  The LCP
  // exclusion is a safety net, not a hard requirement.  For now,
  // return false (don't exclude) — the operator can manually exclude
  // via URL patterns if needed.
  (void)cache;
  (void)url;
  (void)hostname;
  return false;
}

// Convert a MessageType enum to a log level string for WebSocket log streams.
const char* MessageTypeToLevel(MessageType type) {
  switch (type) {
    case MessageType::kInfo:
      return "info";
    case MessageType::kWarning:
      return "warning";
    case MessageType::kError:
    case MessageType::kFatal:
      return "error";
    default:
      return "debug";
  }
}

// The slot id for one (format x viewport x density x save-data)
// combination: the alternate id FindMissingFormats computes for the same
// combination, and therefore the decline tombstone's record key (#1382).
// All eight low-byte bits are set explicitly, so the starting mask does
// not matter.
uint8_t VariantSlotId(CapabilityMask::ImageFormat fmt,
                      CapabilityMask::Viewport vp,
                      CapabilityMask::PixelDensity den,
                      CapabilityMask::SaveData sd) {
  CapabilityMask m;
  m.set_image_format(fmt);
  m.set_viewport(vp);
  m.set_pixel_density(den);
  m.set_save_data(sd);
  m.set_transfer_encoding(CapabilityMask::TransferEncoding::kIdentity);
  return static_cast<uint8_t>(MaskToAlternateId(m.Encode() & 0xFF));
}

// Find image formats not yet cached for a given dimension combination.
std::vector<CapabilityMask::ImageFormat> FindMissingFormats(
    const CapabilityMask& target_mask, CapabilityMask::Viewport vp,
    CapabilityMask::PixelDensity den, CapabilityMask::SaveData sd,
    std::span<const CapabilityMask::ImageFormat> all_formats,
    const std::unordered_set<uint8_t>& existing_ids) {
  std::vector<CapabilityMask::ImageFormat> missing;
  for (auto fmt : all_formats) {
    auto sid = VariantSlotId(fmt, vp, den, sd);
    if (!existing_ids.count(sid)) {
      missing.push_back(fmt);
    }
  }
  return missing;
}

// Size gates shared by every image write path (the notification pass and
// the warmup matrix).  Each new variant is compared with the variant a client
// of its slot would be served without it, and refused when it is not
// smaller:
//
//   * a Save-Data variant against the variant of the same format and size
//     without Save-Data, or Save-Data costs the user bytes;
//   * a converted variant (WebP/AVIF) against the original-format variant of
//     the same slot -- built in this pass, else stored, else, for a
//     Save-Data slot, the one without Save-Data the selector falls back to.
//
// A refused variant is not stored; the selector then serves the smaller
// variant it was compared with.  The front end's unoptimized original at the
// identity slot is never a baseline: it is about to be replaced by the
// optimized encode, which is the variant the comparison is about.
class VariantSizeGates {
 public:
  using Log = std::function<void(const std::string&)>;

  VariantSizeGates(const std::vector<cyclone::AlternateInfo>* alts,
                   bool identity_raw, Log log)
      : log_(std::move(log)) {
    if (alts == nullptr) return;
    const auto identity = static_cast<uint8_t>(MaskToId(CapabilityMask()));
    for (const auto& alt : *alts) {
      const auto id = static_cast<uint8_t>(alt.id);
      if (IsSentinel(id) || (identity_raw && id == identity)) continue;
      stored_[id] = alt.content_length;
    }
  }

  // Applies the gates to one (viewport, density, save-data) combination's
  // results, then records what survives as this pass's variants.
  void Apply(MultiTranscodeResult& multi, CapabilityMask::Viewport vp,
             CapabilityMask::PixelDensity den, CapabilityMask::SaveData sd) {
    using Fmt = CapabilityMask::ImageFormat;
    constexpr auto kOff = CapabilityMask::SaveData::kOff;
    const std::pair<Fmt, TranscodeResult*> arms[] = {
        {Fmt::kOriginal, &multi.optimized_original},
        {Fmt::kWebP, &multi.webp},
        {Fmt::kAvif, &multi.avif}};
    // Save-Data first, so the converted gates below compare with what the
    // Save-Data slot will actually hold.
    if (sd == CapabilityMask::SaveData::kOn) {
      for (auto [fmt, tr] : arms) {
        const bool had = tr->success;
        RefuseUnlessSmaller(*tr, Baseline(VariantSlotId(fmt, vp, den, kOff)),
                            VariantSlotId(fmt, vp, den, sd),
                            "variant without Save-Data");
        if (had && !tr->success) ++multi.size_refused_savedata;
      }
    }
    size_t orig = multi.optimized_original.success
                      ? multi.optimized_original.output_data.size()
                      : Baseline(VariantSlotId(Fmt::kOriginal, vp, den, sd));
    if (orig == 0 && sd == CapabilityMask::SaveData::kOn) {
      orig = Baseline(VariantSlotId(Fmt::kOriginal, vp, den, kOff));
    }
    // A same-format origin (WebP) holds its own WebP in the original-format
    // slot; the transcoder decides that arm, not a cross-format gate.
    const bool orig_is_webp =
        multi.optimized_original.success &&
        multi.optimized_original.output_mime_type == "image/webp";
    // A refusal here (against a stored or this-pass baseline the transcoder
    // did not see) counts like one the transcoder made (#406).
    if (!orig_is_webp) {
      const bool had_webp = multi.webp.success;
      RefuseUnlessSmaller(multi.webp, orig,
                          VariantSlotId(Fmt::kWebP, vp, den, sd),
                          "original-format variant");
      if (had_webp && !multi.webp.success) multi.size_refused_webp = 1;
    }
    const bool had_avif = multi.avif.success;
    RefuseUnlessSmaller(multi.avif, orig,
                        VariantSlotId(Fmt::kAvif, vp, den, sd),
                        "original-format variant");
    if (had_avif && !multi.avif.success) multi.size_refused_avif = 1;
    // A rescue only counts when the variant survives every gate: the
    // backstop above can still drop an arm the transcoder recovered (the
    // Save-Data rule, or a smaller stored or non-Save-Data baseline), and
    // then nothing is stored. Keeps refused minus rescued equal to the
    // number dropped.
    if (!multi.webp.success) multi.size_rescued_webp = 0;
    if (!multi.avif.success) multi.size_rescued_avif = 0;
    for (auto [fmt, tr] : arms) {
      if (tr->success) {
        produced_[VariantSlotId(fmt, vp, den, sd)] = tr->output_data.size();
      }
    }
  }

  // The size a client of |slot| is served when this pass does not produce
  // |slot| itself: this pass's own variant, else the stored one, else 0
  // (nothing to compare with).
  size_t Baseline(uint8_t slot) const {
    if (auto it = produced_.find(slot); it != produced_.end()) {
      return it->second;
    }
    if (auto it = stored_.find(slot); it != stored_.end()) {
      return static_cast<size_t>(it->second);
    }
    return 0;
  }

 private:
  void RefuseUnlessSmaller(TranscodeResult& tr, size_t baseline, uint8_t slot,
                           const char* than) {
    if (!tr.success || baseline == 0 || tr.output_data.size() < baseline) {
      return;
    }
    if (log_) {
      log_(absl::StrFormat(
          "Refused image variant (slot 0x%02X, %s): %zu bytes is not smaller "
          "than the %s (%zu bytes)",
          static_cast<int>(slot), tr.output_mime_type, tr.output_data.size(),
          than, baseline));
    }
    tr = {false, {}, {}, absl::StrCat("not smaller than the ", than)};
  }

  Log log_;
  std::unordered_map<uint8_t, uint64_t> stored_;
  std::unordered_map<uint8_t, size_t> produced_;
};

// The result a multi-transcode holds for one format's slot.
const TranscodeResult& ResultForFormat(const MultiTranscodeResult& multi,
                                       CapabilityMask::ImageFormat fmt) {
  switch (fmt) {
    case CapabilityMask::ImageFormat::kWebP:
      return multi.webp;
    case CapabilityMask::ImageFormat::kAvif:
      return multi.avif;
    default:
      return multi.optimized_original;
  }
}

// Adds one multi-transcode's cross-format size gate outcomes to the worker
// stats (#406). Call after VariantSizeGates::Apply, which folds its own
// refusals into |multi|.
void RecordSizeGateStats(WorkerStats& stats,
                         const MultiTranscodeResult& multi) {
  stats.image_size_refused_webp.fetch_add(multi.size_refused_webp,
                                          std::memory_order_relaxed);
  stats.image_size_refused_avif.fetch_add(multi.size_refused_avif,
                                          std::memory_order_relaxed);
  stats.image_savedata_refused.fetch_add(multi.size_refused_savedata,
                                         std::memory_order_relaxed);
  stats.image_budget_rescued_webp.fetch_add(multi.size_rescued_webp,
                                            std::memory_order_relaxed);
  stats.image_budget_rescued_avif.fetch_add(multi.size_rescued_avif,
                                            std::memory_order_relaxed);
}

// True when the identity slot (0x08: desktop, 1x, Save-Data off, original
// format) holds the front end's own unoptimized recording rather than a
// worker variant.  The nginx front end records the origin's bytes there, and
// a check that only asks whether the slot exists would then never build the
// desktop original-format variant: desktop clients that negotiate no other
// format would be served the raw upload, and the size gates would compare
// with it.  Such a slot counts as missing for the original format.
bool IdentityHoldsRawOriginal(PageSpeedCache* cache, const std::string& url,
                              const std::string& hostname,
                              std::string_view scheme, std::string_view site,
                              const std::unordered_set<uint8_t>& existing_ids) {
  const AlternateId identity = MaskToId(CapabilityMask());
  if (existing_ids.count(static_cast<uint8_t>(identity)) == 0) return false;
  // Same RAM-tier reasoning as ReadPristineOrigin: observe the on-disk entry.
  cache->EvictAlternateFromRamCache(url, hostname, scheme, identity, site);
  auto rr = cache->ReadAlternate(url, hostname, scheme, identity, site);
  if (!rr.has_value() || !rr->is_valid()) return false;
  const bool raw =
      (rr->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) == 0;
  rr->release();
  return raw;
}

// Before the worker writes its optimized encode over a raw identity slot,
// keep the origin's bytes as the durable original (the class the change
// detection reads as the pristine reference, ReadPristineOrigin), unless a
// durable original at least as new is already stored.  Without it the next
// pass would hash the worker's own encode, read it as an origin change, and
// rebuild every variant from derived bytes.  Returns false when the bytes
// could not be preserved; the caller then leaves the identity slot alone.
bool PreserveIdentityAsDurableOriginal(
    PageSpeedCache* cache, const std::string& url, const std::string& hostname,
    std::string_view scheme, std::string_view site, bool leases_enabled) {
  const AlternateId identity = MaskToId(CapabilityMask());
  auto id_read = cache->ReadAlternate(url, hostname, scheme, identity, site);
  if (!id_read.has_value() || !id_read->is_valid()) return false;
  // Re-check on the entry actually read: a concurrent pass (a warmup racing
  // a notification) may have replaced the raw recording with its own encode
  // since IdentityHoldsRawOriginal looked.  A worker variant is never kept as
  // the origin's bytes.
  if ((id_read->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) !=
      0) {
    id_read->release();
    return false;
  }
  AlternateMetadata meta = id_read->metadata;
  auto span = id_read->content();
  std::string bytes(reinterpret_cast<const char*>(span.data()), span.size());
  if (!Worker::BorrowCopyUntorn(*id_read, leases_enabled)) return false;
  id_read->release();
  {
    auto durable = cache->ReadOriginalAlternate(url, hostname, scheme, site);
    if (durable.has_value() && durable->is_valid()) {
      const bool current =
          durable->metadata.cache_inserted_at >= meta.cache_inserted_at;
      durable->release();
      if (current) return true;
    }
  }
  meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
  auto wh = cache->WriteOriginalAlternate(url, hostname, scheme, bytes.size(),
                                          meta, site);
  if (!wh.has_value()) return false;
  if (!wh->write_sync(std::as_bytes(std::span(bytes.data(), bytes.size())))
           .has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

// Stamp the identity slot (0x08) in place when it still holds the front
// end's own recording: the source binding is the hash of those bytes, so an
// origin that sends no validators still yields a stable ETag and answers a
// conditional request with a 304.  Used by the image paths that write no
// optimized original-format variant (the bytes were already optimal, or the
// type is passed through), where nothing else would carry the stamp.  A
// worker-written entry, a missing entry and an entry that already carries
// the right stamp are left alone.
StampResult StampRawIdentityEntry(PageSpeedCache* cache, const std::string& url,
                                  const std::string& hostname,
                                  std::string_view scheme,
                                  std::string_view site, bool leases_enabled,
                                  const PurgeCheck& purge_check,
                                  Worker* purge_undo_worker) {
  const AlternateId identity = MaskToId(CapabilityMask());
  cache->EvictAlternateFromRamCache(url, hostname, scheme, identity, site);
  auto rr = cache->ReadAlternate(url, hostname, scheme, identity, site);
  if (!rr.has_value() || !rr->is_valid()) return StampResult::kSkipped;
  if ((rr->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
    rr->release();
    return StampResult::kSkipped;
  }
  // StampRawEntry rewrites the slot named by the entry's own mask; only the
  // identity slot is stamped here.
  if (MaskToId(CapabilityMask::Decode(rr->metadata.full_mask)) != identity) {
    rr->release();
    return StampResult::kSkipped;
  }
  const AlternateMetadata read_meta = rr->metadata;
  auto span = rr->content();
  const std::string bytes(reinterpret_cast<const char*>(span.data()),
                          span.size());
  if (!Worker::BorrowCopyUntorn(*rr, leases_enabled)) {
    return StampResult::kSkipped;
  }
  rr->release();
  const std::array<std::byte, 32> hash =
      cyclone::crypto::SHA256::hash(std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
  return StampRawEntry(cache, url, hostname, scheme, site, read_meta, bytes,
                       hash, purge_check, purge_undo_worker);
}

// ---------------------------------------------------------------------
// Issue #1503: the pristine origin reference.
//
// The identity/original-format slot (0x08) is NOT a trustworthy origin
// reference: the worker's own proactive output lands there too (the
// recompressed same-format image arm, minified text), flagged
// kFlagWorkerProcessed.  A reader that treats that slot as "the origin"
// compares the re-fetched origin against a DERIVED variant and concludes
// "changed" on every freshness cycle — the false-purge churn this issue
// reports.  The pristine reference is, in preference order:
//
//   1. the durable original (SentinelId::kOriginalContent, 0x0C), read by
//      exact id — the class external front ends (ps_cache_write_original)
//      record the origin's bytes into;
//   2. the identity slot, ONLY when its entry is a genuine original
//      (kFlagWorkerProcessed clear) — the nginx front end's re-record.
//
// When both exist the newer cache_inserted_at wins: it is the most recent
// origin observation.
struct PristineOriginRead {
  bool found = false;
  AlternateId id = static_cast<AlternateId>(0);
  AlternateMetadata meta;
  std::string bytes;  // owned copy (de-aliased from the mmap borrow)
  std::array<std::byte, 32> hash{};
};

// Read the pristine origin reference for a URL, if the cache holds one.
// Both candidate slots are evicted from this process's write-around RAM
// tier first, so the read observes the on-disk entry another process may
// just have re-recorded (same reason FreshOriginHash evicts).  A torn
// borrow is treated as "no reference" — hashing torn bytes could
// mis-conclude "changed", and the fail-safe direction for the callers is
// the legacy behaviour, not a verdict over suspect bytes.
PristineOriginRead ReadPristineOrigin(
    PageSpeedCache* cache, const std::string& url, const std::string& hostname,
    std::string_view scheme, std::string_view site, bool leases_enabled,
    WorkerStats& stats, MessageHandler* handler) {
  PristineOriginRead out;
  const AlternateId identity_id = MaskToId(CapabilityMask());
  const AlternateId durable_id =
      static_cast<AlternateId>(SentinelId::kOriginalContent);
  cache->EvictAlternateFromRamCache(url, hostname, scheme, durable_id, site);
  cache->EvictAlternateFromRamCache(url, hostname, scheme, identity_id, site);

  auto durable = cache->ReadOriginalAlternate(url, hostname, scheme, site);
  ReadResult* chosen = nullptr;
  AlternateId chosen_id = durable_id;
  auto identity =
      cache->ReadAlternate(url, hostname, scheme, identity_id, site);
  const bool identity_genuine =
      identity.has_value() && identity->is_valid() &&
      (identity->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) == 0;
  if (durable.has_value() && durable->is_valid()) {
    chosen = &*durable;
    // The identity wins only when it is a genuine original AND stamped
    // newer than the durable original (a front end that re-records the
    // identity after the durable original was written).
    if (identity_genuine && identity->metadata.cache_inserted_at >
                                durable->metadata.cache_inserted_at) {
      chosen = &*identity;
      chosen_id = identity_id;
    }
  } else if (identity_genuine) {
    chosen = &*identity;
    chosen_id = identity_id;
  }
  if (chosen == nullptr) {
    return out;  // No pristine reference: only worker-processed variants.
  }
  auto span = chosen->content();
  out.bytes.assign(reinterpret_cast<const char*>(span.data()), span.size());
  out.meta = chosen->metadata;
  if (!Worker::BorrowCopyUntorn(*chosen, leases_enabled)) {
    stats.read_borrow_wrap_discards.fetch_add(1, std::memory_order_relaxed);
    if (handler != nullptr) {
      handler->Warning(
          "Pristine origin borrow wrapped during de-alias copy for %s; "
          "treating the reference as unreadable",
          url.c_str());
    }
    out = PristineOriginRead{};
    return out;
  }
  chosen->release();
  out.found = true;
  out.id = chosen_id;
  out.hash = cyclone::crypto::SHA256::hash(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(out.bytes.data()), out.bytes.size()));
  return out;
}

// The newest cache_inserted_at across a URL's NON-SENTINEL alternates (the
// variant set), excluding |skip_id|.  0 when the set is empty.  Used to
// discriminate a re-recorded origin reference RELATIVELY rather than by wall
// clock: variants inherit the origin stamp they were built from (WriteVariant
// copies the source's origin state), so a reference stamped newer than the
// set is the refresh's re-record, while the 1.16 pre-record durable original
// IS the stamp the set inherited — never newer.
uint32_t NewestVariantStamp(PageSpeedCache* cache, std::string_view url,
                            std::string_view hostname, std::string_view scheme,
                            std::string_view site, AlternateId skip_id) {
  auto alts = cache->ListAlternates(url, hostname, scheme, site);
  if (!alts.has_value()) return 0;
  uint32_t newest = 0;
  for (const auto& alt : *alts) {
    const AlternateId pid = static_cast<uint8_t>(alt.id);
    if (pid == skip_id || IsSentinel(pid)) continue;
    auto entry = cache->ReadAlternate(url, hostname, scheme, pid, site);
    if (!entry.has_value() || !entry->is_valid()) continue;
    newest = std::max(newest, entry->metadata.cache_inserted_at);
  }
  return newest;
}

// Issue #1503: refresh the freshness stamp of every NON-SENTINEL alternate
// of a URL in place, adopting the origin-lifetime fields of a freshly
// re-fetched origin reference (cache_inserted_at, max-age/s-maxage, the
// Cache-Control flags, ETag, Last-Modified) while preserving the variant's
// own bytes and every other metadata field (mask, flags, content binding).
// This is the "unchanged origin" half of the refresh contract: the variants
// are still the correct derivation of the origin, so the cycle is a restamp,
// not a purge + re-optimization of identical bytes.
//
// There is no metadata-update primitive in the cache API, so a restamp is a
// same-id rewrite (the property the nginx 304 restamp path already relies
// on).  |skip_id| names the reference slot itself — the sender already
// re-recorded it fresh, and it is typically the largest entry (the raw
// origin).  The purge_check fences each write against a purge racing the
// restamp; a trip aborts the restamp (the purge won; whatever it left will
// re-converge through the ordinary miss/notify path).  Returns the number
// of alternates restamped.
size_t RestampVariantSetFreshness(PageSpeedCache* cache, std::string_view url,
                                  std::string_view hostname,
                                  std::string_view scheme,
                                  std::string_view site,
                                  const AlternateMetadata& fresh_origin_meta,
                                  AlternateId skip_id,
                                  const PurgeCheck& purge_check) {
  const AlternateId markdown_id =
      static_cast<AlternateId>(SentinelId::kAgentMarkdown);
  auto alts = cache->ListAlternates(url, hostname, scheme, site);
  if (!alts.has_value()) return 0;
  size_t restamped = 0;
  for (const auto& alt : *alts) {
    const AlternateId pid = static_cast<uint8_t>(alt.id);
    // Sentinels are skipped — EXCEPT the agent-markdown variant: a sentinel
    // id carrying a servable variant whose freshness stamp the serving path
    // evaluates.  D2 already preserves it across an unchanged refresh;
    // leaving its stamp stale would fall through on every agent request
    // anyway.  (WriteAlternate refuses sentinel ids, hence the dedicated
    // writer below.)
    if (pid == skip_id || (IsSentinel(pid) && pid != markdown_id)) continue;
    if (purge_check && purge_check()) break;  // Purged mid-restamp: stop.
    auto entry = cache->ReadAlternate(url, hostname, scheme, pid, site);
    if (!entry.has_value() || !entry->is_valid()) continue;
    auto span = entry->content();
    const std::string bytes(reinterpret_cast<const char*>(span.data()),
                            span.size());
    AlternateMetadata meta = entry->metadata;
    entry->release();
    meta.cache_inserted_at = fresh_origin_meta.cache_inserted_at;
    meta.origin_max_age = fresh_origin_meta.origin_max_age;
    meta.origin_s_maxage = fresh_origin_meta.origin_s_maxage;
    meta.origin_cc_flags = fresh_origin_meta.origin_cc_flags;
    meta.origin_etag = fresh_origin_meta.origin_etag;
    meta.origin_last_modified = fresh_origin_meta.origin_last_modified;
    // The re-recorded reference is the current word on the headers-sidecar
    // state, and a variant built before the bits existed (or before the
    // reference was classified) would otherwise be restamped unflagged on
    // every refresh.  Agent markdown never inherits them.
    if (pid != markdown_id) {
      meta.flags = (meta.flags & ~kVariantInheritedFlags) |
                   InheritedVariantFlags(fresh_origin_meta);
    }
    auto wh = pid == markdown_id
                  ? cache->WriteAgentAlternate(url, hostname, scheme,
                                               bytes.size(), meta, site)
                  : cache->WriteAlternate(url, hostname, scheme, pid,
                                          bytes.size(), meta, site);
    if (!wh) continue;
    if (!wh->write_sync(std::as_bytes(std::span(bytes.data(), bytes.size())))
             .has_value()) {
      continue;
    }
    if (!wh->close_sync().has_value()) continue;
    // Write-around RAM tier: the read above populated this process's RAM
    // copy and the overwrite does not evict it (see WriteVariant).
    cache->EvictAlternateFromRamCache(url, hostname, scheme, pid, site);
    ++restamped;
  }
  return restamped;
}

}  // namespace

// Client connection context (forward-declared in worker.h).
// The timeout timer is heap-allocated so it can be closed
// independently of the pipe handle.
struct Worker::ClientContext {
  Worker* worker;
  std::string buffer;
  uv_timer_t* timeout_timer;  // heap-allocated, freed on close
  uv_pipe_t* pipe;            // back-pointer for timeout callback
  bool closing = false;       // guard against double-close
};

// Helper to get ClientContext from uv_stream_t
Worker::ClientContext* Worker::GetClientContext(uv_stream_t* stream) {
  return static_cast<ClientContext*>(stream->data);
}

Worker::Worker(const WorkerConfig& config, MessageHandler* handler)
    : config_(config),
      handler_(handler),
      start_time_(std::chrono::steady_clock::now()),
      started_at_ms_(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count()) {}

int Worker::api_port() const {
  return http_server_ ? http_server_->bound_port() : 0;
}

Worker::~Worker() {
  Shutdown();

  // Idempotent: normally already done by BeginGracefulShutdown.  Must happen
  // before the loop is drained and closed below.
  if (config_persister_) {
    config_persister_->Shutdown();
  }

  CloseServeStats(serve_stats_);
  serve_stats_ = nullptr;

  // Clean up libuv resources
  if (loop_) {
    // Close all remaining handles
    uv_walk(loop_.get(), CloseWalkCallback, nullptr);

    // Run until all close callbacks complete
    while (uv_loop_alive(loop_.get())) {
      uv_run(loop_.get(), UV_RUN_ONCE);
    }

    // Now we can close the loop
    uv_loop_close(loop_.get());
  }
}

#ifndef _WIN32
namespace {

// Sets the process umask for as long as it is in scope and restores the
// previous value on the way out.  Used around socket binds, whose mode is
// applied by the kernel at create time and so cannot be made exact by a
// later chmod alone.
class UmaskGuard {
 public:
  explicit UmaskGuard(mode_t mask) : previous_(::umask(mask)) {}
  ~UmaskGuard() { ::umask(previous_); }
  UmaskGuard(const UmaskGuard&) = delete;
  UmaskGuard& operator=(const UmaskGuard&) = delete;

 private:
  mode_t previous_;
};

}  // namespace
#endif

bool Worker::Initialize() {
  // Gate reads that feed a shared copy (the agent render's document,
  // /llms.txt) ask with the shared-copy HEAD: the same refusals as the
  // render's own fetches.
  subresource_gate_->SetSharedCopyHead(MakeSharedCopyHeadFn(
      [this](const std::string& m) { LogWarning("%s", m.c_str()); }));
  // The ordinary HEAD logs its first refusal per origin with the reason.
  subresource_gate_->SetHead(MakeAnonymousHeadFn(
      nullptr, nullptr,
      [this](const std::string& m) { LogWarning("%s", m.c_str()); }));
#ifdef _WIN32
  // On Windows, libuv passes pipe names directly to CreateNamedPipeW without
  // prepending a prefix. Convert Unix-style socket paths to Windows Named Pipe
  // paths (\\.\pipe\<name>) so uv_pipe_bind succeeds.
  if (!config_.socket_path.empty() &&
      config_.socket_path.find("\\\\.\\pipe\\") != 0) {
    config_.socket_path = "\\\\.\\pipe\\" + config_.socket_path;
  }
#endif

  // Pre-compute socket paths for async-signal-safe Shutdown().
  health_socket_path_ = config_.socket_path + ".health";
  mgmt_socket_path_ = config_.socket_path + ".mgmt";

  // The daemon runs unprivileged and must own everything in
  // its cache directory.  Refuse to start — loudly, naming the cause and the
  // migration doc — when the directory is absent, unwritable, or holds
  // foreign-owned content.  This runs BEFORE anything (host aliases,
  // instance id, shared config, the volume itself) writes into the
  // directory, and it never chowns, never falls back to another location,
  // never continues cache-off silently.
  if (!config_.cache_path.empty()) {
    std::filesystem::path cache_path(config_.cache_path);
    std::filesystem::path cache_dir = cache_path.parent_path();
    // Only the daemon's own artifacts are subject to the ownership rule —
    // the volume files and the generation file are named after this stem.
    // stem(), not filename(): the cache layer builds volume names by
    // inserting -<format>-<geometry> BEFORE any extension, so an
    // extensioned --cache-path (cache.vol) yields cache-6-<hex>.vol, which
    // filename() ("cache.vol") would not prefix-match — the daemon's own
    // volume would then slip past the ownership check.
    const std::string volume_stem = cache_path.stem().string();
    std::string detail;
    CacheDirStatus status =
        ValidateCacheDir(cache_dir.string(), volume_stem, &detail);
    if (status != CacheDirStatus::kOk) {
      return RefuseCacheDir(status, cache_dir.string(), detail, handler_);
    }
  }

  // Publish initial config for RCU hot-reload.
  live_config_.store(std::make_shared<const WorkerConfig>(config_));

  // Build URL normalization config.
  {
    for (const auto& ext : config_.strip_query_extensions) {
      url_norm_config_.strip_query_extensions.insert(ext);
    }
    // Expand group names to extensions.
    for (const auto& group : config_.strip_query_groups) {
      for (const auto& ext : pagespeed::ExpandExtensionGroup(group)) {
        url_norm_config_.strip_query_extensions.insert(ext);
      }
    }
    for (const auto& param : config_.strip_query_params) {
      url_norm_config_.strip_query_params.insert(param);
    }
    for (const auto& [src, dst] : config_.host_aliases) {
      url_norm_config_.host_aliases[src] = dst;
    }
  }

  // Write host aliases file for nginx (always write, even if empty, to clear
  // stale aliases from a previous run).
  if (!config_.cache_path.empty()) {
    std::string path = pagespeed::HostAliasesFilePath(config_.cache_path);
    if (!path.empty()) {
      pagespeed::WriteHostAliasesFile(path, config_.host_aliases);
    }
  }

  // Write shared config file for nginx.
  if (!config_.cache_path.empty()) {
    SharedConfig shared;
    shared.socket_path = config_.socket_path;
    // Publish the size this worker opens its cache volume with — the same
    // value handed to PageSpeedCache below, which is what Cyclone derives the
    // volume's on-disk filename from.  A peer that has to open the same
    // volume can then inherit the number instead of assuming one, which is
    // the difference between sharing the cache and silently creating a
    // second, permanently cold one next to it.
    //
    // The REQUESTED size, deliberately, not anything measured off the opened
    // file: the request is what the filename is derived from, so it is what a
    // peer has to match to land on the same file.
    shared.volume_size = config_.cache_size_bytes;
    // Publish the cache-directory generation this build was
    // compiled with, so a peer built against a different N fails the
    // handshake loudly instead of cold-starting in a silent split-brain.
    shared.cache_dir_generation = kCacheDirGeneration;
    shared.html_mode = config_.html_mode;
    // agent_optimize: the serve-side
    // toggle nginx reads to gate the markdown variant is simply the operator's
    // opt-in flag.  The wire key keeps its historical name.
    shared.agent_optimize_entitled = config_.browser_analysis.agent_optimize;
    // /llms.txt serve toggle = both operator flags.
    shared.agent_optimize_llms_txt_enabled =
        shared.agent_optimize_entitled &&
        config_.browser_analysis.agent_optimize_llms_txt;
    // Web Bot Auth: publish the observe-only classify toggle,
    // the operator verified-bot registry, and the directory hosts nginx
    // resolves keyids under.  All default off/empty — nginx does no
    // signature work unless the operator opted in.
    shared.web_bot_auth = config_.web_bot_auth;
    shared.web_bot_auth_verified_bots = config_.web_bot_auth_verified_bots;
    {
      std::string hosts;
      for (const auto& host :
           WebBotAuthDirectoryHosts(config_.web_bot_auth_key_directories)) {
        if (!hosts.empty()) hosts += ',';
        hosts += host;
      }
      shared.web_bot_auth_directory_hosts = std::move(hosts);
    }
    // Web Bot Auth opt-in counter (experimental): publish the
    // non-secret mode (off/private/public) nginx reads to expose the
    // well-known counter endpoint.  Default off — the endpoint is invisible
    // and no counting is published unless the operator opted in.  The gating
    // bearer token is NOT published here (it rides an env var read by nginx);
    // pagespeed-shared.conf is group-readable by the daemon's peers by design.
    shared.web_bot_auth_public_counter = config_.web_bot_auth_public_counter;
    // RSL-CAP enforcement (experimental): publish the toggle, the
    // required license/scope, the optional issuer pin, and the RSL issuer
    // directory hosts (realm "rsl").  All default off/empty — nginx does no
    // token work unless the operator opted in.
    shared.rsl_cap_enforcement = config_.rsl_cap_enforcement;
    shared.rsl_cap_requested_license = config_.rsl_cap_requested_license;
    shared.rsl_cap_requested_scope = config_.rsl_cap_requested_scope;
    shared.rsl_cap_issuer = config_.rsl_cap_issuer;
    {
      std::string hosts;
      for (const auto& host :
           WebBotAuthDirectoryHosts(config_.rsl_cap_key_directories)) {
        if (!hosts.empty()) hosts += ',';
        hosts += host;
      }
      shared.rsl_cap_directory_hosts = std::move(hosts);
    }
    shared.cache_mode = config_.cache_mode;
    // Build comma-separated extensions (including expanded groups).
    {
      std::string exts;
      for (const auto& ext : url_norm_config_.strip_query_extensions) {
        if (!exts.empty()) exts += ',';
        exts += ext;
      }
      shared.strip_query_extensions = std::move(exts);
    }
    {
      std::string params;
      for (const auto& param : url_norm_config_.strip_query_params) {
        if (!params.empty()) params += ',';
        params += param;
      }
      shared.strip_query_params = std::move(params);
    }
    std::string shared_path = SharedConfigFilePath(config_.cache_path);
    if (!shared_path.empty()) {
      WriteSharedConfigFile(shared_path, shared);
    }
  }

  // Read PURGE authentication token from environment.
  const char* purge_env = getenv("PAGESPEED_PURGE_TOKEN");
  if (purge_env != nullptr && purge_env[0] != '\0') {
    purge_token_ = purge_env;
    LogInfo("PURGE authentication enabled (token configured)");
  }

  // Open cache if cache_path is configured
  if (!config_.cache_path.empty()) {
    // Create a TeeMessageHandler for cache diagnostic logging.
    // Delegates to NullMessageHandler (avoids double-printing to stderr)
    // and posts to WsManager with source="cache" when available.
    cache_log_handler_ = std::make_unique<TeeMessageHandler>(
        &null_handler_, [this](MessageType type, std::string msg) {
          if (ws_manager_) {
            ws_manager_->PostLog(MessageTypeToLevel(type), "cache", "cache",
                                 std::move(msg));
          }
        });

    PageSpeedCacheConfig cache_config;
    cache_config.volume_path = config_.cache_path;
    cache_config.volume_size = config_.cache_size_bytes;
    cache_config.handler = cache_log_handler_.get();

    cache_config.ram_cache_size = config_.ram_cache_size;

    // Read-lease wrap gating (issue #934): forward the operator
    // knobs; 0 disables leases entirely.
    cache_config.read_lease_duration =
        std::chrono::milliseconds(config_.read_lease_duration_ms);
    cache_config.lease_wrap_ceiling =
        std::chrono::milliseconds(config_.lease_wrap_ceiling_ms);

    auto cache_result = PageSpeedCache::Create(cache_config);
    if (!cache_result.has_value()) {
      const cyclone::CacheError open_error = cache_result.error();
      if (PageSpeedCache::OpenFailureIsDamagedVolume(open_error)) {
        // Auto-recovery: delete the damaged volume file(s) and retry once.
        // The file on disk is not the configured path but its
        // structural-fingerprint sibling, so unlinking the configured path
        // removed nothing and the retry reopened the same damaged volume
        // (#428).  RemoveVolumeFiles removes the real files.  At the
        // pinned storage layer a damaged volume with no live peer is
        // reset in place inside Create() and never reaches this branch;
        // it is the backstop for a build that reports the damage
        // directly.
        LogWarning(
            "Cache open failed at %s (%s), attempting recovery "
            "(delete and recreate)",
            config_.cache_path.c_str(),
            make_error_code(open_error).message().c_str());
        PageSpeedCache::RemoveVolumeFiles(config_.cache_path);
        cache_result = PageSpeedCache::Create(cache_config);
        if (!cache_result.has_value()) {
          LogError(
              "Cache unavailable at %s even after recreation (%s). "
              "Starting in degraded mode.",
              config_.cache_path.c_str(),
              make_error_code(cache_result.error()).message().c_str());
          cache_degraded_ = true;
          // cache_ stays nullptr. Continue with event loop setup so
          // health/mgmt sockets are available and the process isn't dead.
        } else {
          LogInfo("Cache recovered successfully at %s",
                  config_.cache_path.c_str());
        }
      } else if (open_error == cyclone::CacheError::ResetRefusedLivePeer) {
        // Another live process (normally the web server module) holds the
        // volume and the storage layer refused the reset on purpose.
        // Deleting the files here would split the two processes onto
        // different volumes with no reset signal: the peer would keep
        // writing an inode this process never reads, losing its writes
        // until it restarts.  Degrade and name the operator's cure.
        LogError(
            "Cache open failed at %s (%s): another process holds the cache "
            "volume; starting degraded. Restart the web server so the cache "
            "can be reset.",
            config_.cache_path.c_str(),
            make_error_code(open_error).message().c_str());
        cache_degraded_ = true;
        // cache_ stays nullptr, as above.
      } else {
        // A refusal (ownership, permission, symlink, I/O) or a
        // configuration or environment failure: deleting the volume cannot
        // fix it, and a file this process was refused is not this
        // process's to remove.  Log the error and degrade without the
        // delete.
        LogError(
            "Cache open failed at %s (%s), not a damaged volume; skipping "
            "the delete-and-recreate recovery. Starting in degraded mode.%s",
            config_.cache_path.c_str(),
            make_error_code(open_error).message().c_str(),
            open_error == cyclone::CacheError::VolumeFileRefused
                ? " Remove the volume file named in the preceding \"cyclone: "
                  "not "
                  "using volume\" line and let the daemon recreate it."
                : "");
        cache_degraded_ = true;
        // cache_ stays nullptr, as above.
      }
    }
    if (cache_result.has_value()) {
      cache_ = std::move(cache_result.value());

      // Config accessor lambda: reads live_config_ at transcode time
      // so hot-reloaded quality/viewport settings take effect.
      auto config_accessor = [this]() -> ImageTranscoderConfig {
        auto cfg = live_config_.load();
        ImageTranscoderConfig tc;
        tc.viewport_widths.mobile = cfg->mobile_width;
        tc.viewport_widths.tablet = cfg->tablet_width;
        tc.viewport_widths.desktop = cfg->desktop_width;
        tc.jpeg_quality = cfg->jpeg_quality;
        tc.webp_quality = cfg->webp_quality;
        tc.avif_quality = cfg->avif_quality;
        tc.avif_speed = cfg->avif_speed;
        tc.savedata_jpeg_quality = cfg->savedata_jpeg_quality;
        tc.savedata_webp_quality = cfg->savedata_webp_quality;
        tc.savedata_avif_quality = cfg->savedata_avif_quality;
        tc.content_analysis = cfg->content_analysis;
        tc.quality_verify = cfg->quality_verify;
        tc.target_ssimulacra2 = cfg->target_ssimulacra2;
        tc.ssimulacra2_tolerance = cfg->ssimulacra2_tolerance;
        tc.learned_quality = cfg->learned_quality;
        tc.learned_quality_jpeg = cfg->learned_quality_jpeg;
        tc.learned_quality_webp = cfg->learned_quality_webp;
        tc.learned_quality_avif = cfg->learned_quality_avif;
        tc.savedata_score_reduction = cfg->savedata_score_reduction;
        // Opt-out for the same-format JPEG quality cap.  It has to reach the
        // transcoder for the switch to mean anything (#1284).
        tc.no_quality_cap = cfg->no_quality_cap;
        tc.preserve_c2pa = cfg->preserve_c2pa;
        tc.c2pa_carry = cfg->c2pa_carry;
        return tc;
      };
      image_transcoder_ =
          std::make_unique<ImageTranscoder>(config_accessor, handler_);

      LogInfo("Cache opened at %s (%" PRIu64 " bytes)",
              config_.cache_path.c_str(), config_.cache_size_bytes);

      // Create shared mmap for serve-time bandwidth counters.
      std::string stats_path = ServeStatsPath(config_.cache_path);
      serve_stats_ = CreateServeStats(stats_path, handler_);
      // The worker's own Busy cache reads, counted on the same shared surface
      // as the front end's.  Installed before any worker thread reads.
      cache_->SetBusyReadObserver([this](bool retry_hit) {
        RecordCacheBusyRead(serve_stats_, retry_hit);
      });
      if (serve_stats_ != nullptr) {
        // Configuration, not a counter: re-stamped on every create-or-reuse so
        // it tracks a pool-width change across a restart, and non-zero while
        // the worker runs, which is how a reader tells "no saturation samples
        // yet" from "nothing writes this block".
        SetWorkerPoolThreads(serve_stats_,
                             config_.num_threads > 0
                                 ? static_cast<uint32_t>(config_.num_threads)
                                 : 0u);
        LogInfo("Serve stats mmap created at %s", stats_path.c_str());
      } else {
        LogWarning("Failed to create serve stats mmap at %s",
                   stats_path.c_str());
      }
    }
  }

  // Create event loop
  loop_ = std::make_unique<uv_loop_t>();
  if (uv_loop_init(loop_.get()) != 0) {
    LogError("Failed to initialize libuv loop");
    return false;
  }

  // Create async handle for thread-safe shutdown
  shutdown_async_ = std::make_unique<uv_async_t>();
  if (uv_async_init(loop_.get(), shutdown_async_.get(), OnShutdownAsync) != 0) {
    LogError("Failed to initialize async handle");
    return false;
  }
  shutdown_async_->data = this;

  // Create pipe server
  server_ = std::make_unique<uv_pipe_t>();
  if (uv_pipe_init(loop_.get(), server_.get(), 0) != 0) {
    LogError("Failed to initialize pipe");
    return false;
  }

  // Store 'this' pointer for callbacks
  server_->data = this;

  // Declared out here because they outlive the umask-guarded block below.
  std::string health_path;
  std::string mgmt_path;

  {
#ifndef _WIN32
    // The three sockets below are each chmod'ed to 0660 right after their
    // bind, but bind() itself applies the process umask -- leaving a window
    // in which the socket exists at a wider mode than it will end up with.
    // Hold a umask of 0117 across all three binds so the transient mode can
    // never be wider than the final one either.  This is belt to the chmod's
    // braces: the chmod is what makes the mode exact, this is what makes it
    // exact at every instant.
    //
    // The block is TIGHT on purpose.  This umask is inherited by everything
    // spawned later in Initialize() -- notably the headless browser, which
    // creates its own profile directory and would be handed 0117 instead of
    // its own mode.  The guard must be gone before anything else runs.
    UmaskGuard socket_umask(0117);
#endif

    // Remove existing socket file
#ifndef _WIN32
    unlink(config_.socket_path.c_str());
#endif

    // Bind to socket path
    if (uv_pipe_bind(server_.get(), config_.socket_path.c_str()) != 0) {
      LogError("Failed to bind to %s", config_.socket_path.c_str());
      return false;
    }

#ifndef _WIN32
    // Restrict the notification socket to owner+group (the daemon and the
    // web-server peers in group `pagespeed`).  Explicit chmod after bind —
    // never umask-derived (H3: this socket was world-writable under the old
    // UMask=0000 unit).  Failure is fatal: a socket we cannot scope is a
    // socket any local user can feed notifications to.
    if (chmod(config_.socket_path.c_str(), 0660) != 0) {
      LogError("Failed to chmod 0660 notification socket %s: %s",
               config_.socket_path.c_str(), strerror(errno));
      return false;
    }
#endif

#ifdef _WIN32
    // Set DACL before listen to prevent a TOCTOU window where the pipe
    // accepts connections with default (permissive) security.
    // Non-fatal: notification pipe carries optimization responses, not secrets.
    if (!SetPipeDacl(server_.get(), OpenPipeSddl(), handler_, "notification")) {
      LogError(
          "Notification pipe DACL failed — proceeding "
          "with default security");
    }
#endif

    // Start listening
    if (uv_listen(reinterpret_cast<uv_stream_t*>(server_.get()), 128,
                  OnNewConnection) != 0) {
      LogError("Failed to listen on socket");
      return false;
    }

    // Set up health check socket
    health_path = health_socket_path();
#ifndef _WIN32
    unlink(health_path.c_str());
#endif

    health_server_ = std::make_unique<uv_pipe_t>();
    if (uv_pipe_init(loop_.get(), health_server_.get(), 0) != 0) {
      LogError("Failed to initialize health pipe");
      return false;
    }
    health_server_->data = this;

    if (uv_pipe_bind(health_server_.get(), health_path.c_str()) != 0) {
      LogError("Failed to bind health socket to %s", health_path.c_str());
      return false;
    }

#ifndef _WIN32
    // Same scoping as the notification socket: owner+group only, explicit,
    // never umask-derived.  Fatal for the same reason.
    if (chmod(health_path.c_str(), 0660) != 0) {
      LogError("Failed to chmod 0660 health socket %s: %s", health_path.c_str(),
               strerror(errno));
      return false;
    }
#endif

#ifdef _WIN32
    // Non-fatal: health pipe exposes operational state, not secrets.
    if (!SetPipeDacl(health_server_.get(), OpenPipeSddl(), handler_,
                     "health")) {
      LogError(
          "Health pipe DACL failed — proceeding "
          "with default security");
    }
#endif

    if (uv_listen(reinterpret_cast<uv_stream_t*>(health_server_.get()), 8,
                  OnHealthConnection) != 0) {
      LogError("Failed to listen on health socket");
      return false;
    }

    // Set up management socket
    mgmt_path = mgmt_socket_path();
#ifndef _WIN32
    unlink(mgmt_path.c_str());
#endif

    mgmt_server_ = std::make_unique<uv_pipe_t>();
    if (uv_pipe_init(loop_.get(), mgmt_server_.get(), 0) != 0) {
      LogError("Failed to initialize management pipe");
      return false;
    }
    mgmt_server_->data = this;

    if (uv_pipe_bind(mgmt_server_.get(), mgmt_path.c_str()) != 0) {
      LogError("Failed to bind management socket to %s", mgmt_path.c_str());
      return false;
    }

    // Restrict management socket to owner+group only (PURGE commands).
    // The notification and health sockets carry the same 0660 scoping (set
    // right after their binds above); after the H1 privilege drop the group
    // is `pagespeed`, so group membership IS the peer boundary for all three.
    // Fatal on failure: this is the socket that accepts PURGE, so a mode we
    // could not set is a command surface we cannot vouch for.
#ifndef _WIN32
    if (chmod(mgmt_path.c_str(), 0660) != 0) {
      LogError("Failed to chmod 0660 management socket %s: %s",
               mgmt_path.c_str(), strerror(errno));
      return false;
    }
#else
    // Windows equivalent of chmod 0660: restrict to SYSTEM + Administrators.
    // Applied before listen to prevent a TOCTOU window. Failure is fatal
    // for the management pipe since it accepts PURGE commands.
    if (!SetPipeDacl(mgmt_server_.get(), RestrictedPipeSddl(), handler_,
                     "management")) {
      LogError(
          "Management pipe DACL failed — refusing to start "
          "with permissive default security");
      return false;
    }
#endif

    if (uv_listen(reinterpret_cast<uv_stream_t*>(mgmt_server_.get()), 8,
                  OnMgmtConnection) != 0) {
      LogError("Failed to listen on management socket");
      return false;
    }
  }  // socket umask guard ends here -- nothing spawned later inherits 0117.

#ifndef _WIN32
  // Cheap, unconditional proof that the guard really did unwind before the
  // rest of Initialize() (the headless browser is spawned further down and
  // must not inherit the socket umask).
  {
    const mode_t restored = ::umask(0);
    ::umask(restored);
    if (restored == 0117) {
      LogError(
          "Internal error: the socket umask guard did not unwind before "
          "subprocess setup; refusing to start rather than spawning "
          "children with a socket-scoped umask.");
      return false;
    }
  }
#endif

  // Set up periodic cache stats refresh (every 2s) to avoid calling
  // cache_->Stats() on the event loop where it can contend with
  // Cyclone locks held by thread pool workers.
  if (cache_) {
    stats_refresh_timer_ = std::make_unique<uv_timer_t>();
    uv_timer_init(loop_.get(), stats_refresh_timer_.get());
    stats_refresh_timer_->data = this;
    // Fire immediately (repeat=2000ms).
    uv_timer_start(stats_refresh_timer_.get(), OnStatsRefresh, 0, 2000);
  }

  // Initialize browser analysis manager if enabled.
  if (config_.browser_analysis.enabled && cache_) {
    BrowserAnalysisConfig browser_config = config_.browser_analysis;
    // The analysis resource map must normalize its cache lookups exactly as
    // the worker normalizes store keys (a query-versioned script src would
    // otherwise miss permanently).  url_norm_config_ is built once in
    // Initialize() and never mutated afterwards, so this copy cannot go
    // stale.
    browser_config.url_normalization = url_norm_config_;

    // Resolve the headless-Chrome sandbox BEFORE the first
    // Chrome spawn.  Three outcomes, no silent fallback and no `auto`:
    //   require + probe ok  -> Chrome runs sandboxed (no --no-sandbox)
    //   require + probe bad -> browser analysis REFUSES, daemon serves on
    //   off                 -> --no-sandbox is re-added, loudly
    const char* runtime_dir_env = std::getenv("RUNTIME_DIRECTORY");
    if (browser_config.chrome_user_data_dir.empty()) {
      std::string cache_parent;
      if (!config_.cache_path.empty()) {
        const size_t slash = config_.cache_path.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
          cache_parent = config_.cache_path.substr(0, slash);
        }
      }
      browser_config.chrome_user_data_dir = ResolveChromeUserDataDir(
          runtime_dir_env != nullptr ? runtime_dir_env : "",
          config_.socket_path, cache_parent);
    }

    if (browser_config.sandbox_mode == BrowserSandboxMode::kOff) {
      browser_sandbox_state_ = BrowserSandboxState::kOff;
      browser_config.chrome_no_sandbox = true;
      LogWarning(
          "SECURITY: browser sandbox DISABLED by --browser-sandbox=off. "
          "Headless Chrome will run with --no-sandbox, so untrusted page "
          "content is parsed with no kernel-enforced isolation and the only "
          "boundary left is whatever contains this process. This is reported "
          "as browser_sandbox=\"off\" on GET /v1/health.");
    } else {
      const BrowserSandboxProbe probe = ProbeBrowserSandbox();
      if (probe.available) {
        browser_sandbox_state_ = BrowserSandboxState::kOn;
        LogInfo("Browser sandbox available (%s)", probe.diagnostics.c_str());
      } else {
        browser_sandbox_state_ = BrowserSandboxState::kUnavailable;
        // LOUD, NAMED, and non-fatal: browser analysis is optional and
        // default-off, so killing a healthy optimizer over it would invert
        // the availability posture.  What must never happen is a silent
        // fallback to an unsandboxed browser -- so we start no Chrome at all.
        LogError(
            "REFUSING to start browser analysis: the headless-Chrome sandbox "
            "is unavailable (%s). Observed: %s. The daemon keeps serving; "
            "browser analysis is OFF and analysis requests are not queued. To "
            "fix, either make unprivileged user namespaces available to this "
            "service (on Ubuntu 24.04 and derivatives see "
            "kernel.apparmor_restrict_unprivileged_userns; in a container, run "
            "with a Chrome-compatible seccomp profile and a non-root user), or "
            "accept an unsandboxed browser deliberately with "
            "--browser-sandbox=off (PAGESPEED_BROWSER_SANDBOX=off). This is "
            "reported as browser_sandbox=\"unavailable\" on GET /v1/health.",
            probe.reason.c_str(), probe.diagnostics.c_str());
      }
    }

    if (browser_sandbox_state_ == BrowserSandboxState::kUnavailable) {
      // No manager is constructed, so there is no queue to reject from: every
      // EnqueueAnalysis call site is already guarded on browser_manager_.
      browser_manager_.reset();
    } else {
      browser_manager_ = std::make_unique<BrowserAnalysisManager>(
          loop_.get(), std::move(browser_config), cache_.get(), handler_);
      if (!browser_manager_->Initialize()) {
        LogWarning("Browser analysis init failed, using heuristics only");
        browser_manager_.reset();
      } else {
        browser_manager_->set_subresource_gate(subresource_gate_);
        // The critical-CSS validation needs the exact stylesheet bytes the SERVE
        // path will hash, so it calls the serve path's own assembly rather than
        // owning a second one.  Reassembling them independently would drift (the
        // contract next to CombinedCssValidationHash lists seven ways) and every
        // record produced would silently fail to match at serve time — the
        // feature would be on and permanently inert.
        //
        // Runs on the event loop rather than the notification thread pool: it
        // reads the cache (thread-safe), the atomic stats, and url_norm_config_,
        // which is fixed after Initialize().
        //
        // MEASURED, because "it should be fine" is not an argument about a
        // shared event loop. On the frozen modpagespeed.com capture (287 KB page,
        // 118 KB sheet): scan 5.2-5.4 ms plus this assembly 0.3 ms = ~5.7 ms,
        // once per analysed page. The baseline it joins is
        // InlineCachedStylesheets at ~10.2 ms, which already runs synchronously
        // on this same loop for every analysed page today. Reproduce with
        // //tools/async-css-probe:measure_validation_threshold.
        browser_manager_->set_combined_css_builder(
            [this](const HtmlScanResult& scan_result, const std::string& url,
                   const std::string& hostname, const std::string& scheme,
                   const std::string& site, bool host_vouched,
                   bool from_module) {
              CacheNotification n;
              n.url = url;
              n.hostname = hostname;
              n.scheme = scheme;
              // The analysed item's keyspace and sender bits.
              n.site = site;
              n.host_vouched = host_vouched;
              n.from_module = from_module;
              const auto cfg = live_config_.load();
              // count_lookups=false: this page's sheet is assembled again on the
              // serve path, and counting both would double-report the optimizer's
              // cache probes.
              // gate_may_fetch=false: this runs on the event loop, which
              // must not wait on a request; the serve path asked first.
              CombinedCssResult built =
                  BuildCombinedCss(scan_result, n, *cfg,
                                   /*count_lookups=*/false,
                                   /*gate_may_fetch=*/false);
              return BrowserAnalysisManager::CombinedCss{
                  std::move(built.css), built.external_css_missing,
                  built.revalidatable_css_missing,
                  std::move(built.layer_order)};
            });
      }
    }
  }

  // Initialize event loop lag gauge (fires every 1s).
  lag_timer_ = std::make_unique<uv_timer_t>();
  uv_timer_init(loop_.get(), lag_timer_.get());
  lag_timer_->data = this;
  // Issue #165: store as atomic ticks (see worker.h).
  lag_scheduled_ticks_.store(
      std::chrono::steady_clock::now().time_since_epoch().count(),
      std::memory_order_relaxed);
  uv_timer_start(lag_timer_.get(), OnLagTimer, 1000, 1000);

  // Web Bot Auth misconfiguration surfacing: the refresh timer
  // silently does nothing for unusable config, so tell the operator up front.
  if (config_.web_bot_auth) {
    for (const auto& url : config_.web_bot_auth_key_directories) {
      if (WebBotAuthDirectoryHosts({url}).empty()) {
        LogWarning(
            "Web Bot Auth key-directory URL is not a valid https URL and "
            "will be ignored: %s",
            url.c_str());
      }
    }
    if (WebBotAuthDirectoryHosts(config_.web_bot_auth_key_directories)
            .empty()) {
      LogWarning(
          "Web Bot Auth is enabled but no usable key-directory URL is "
          "configured — signed requests will classify as unknown "
          "(use --web-bot-auth-key-directory)");
    }
  }

  // Initialize the Web Bot Auth key-directory refresh timer.
  // Only started when the feature is enabled AND directories are configured
  // — default off means no timer, no startup fetch, zero behavior change.
  if (config_.web_bot_auth && !config_.web_bot_auth_key_directories.empty()) {
    webbotauth_refresh_timer_ = std::make_unique<uv_timer_t>();
    uv_timer_init(loop_.get(), webbotauth_refresh_timer_.get());
    webbotauth_refresh_timer_->data = this;
    // Cadence constants (incl. the total-failure backoff schedule) live in
    // worker.h next to webbotauth_backoff_stage_.
    uv_timer_start(webbotauth_refresh_timer_.get(), OnWebBotAuthRefresh,
                   kWebBotAuthInitialDelayMs, kWebBotAuthRefreshIntervalMs);
    LogInfo(
        "Web Bot Auth key-directory refresh timer started "
        "(%zu director%s configured)",
        config_.web_bot_auth_key_directories.size(),
        config_.web_bot_auth_key_directories.size() == 1 ? "y" : "ies");
  }

  // RSL-CAP misconfiguration surfacing (experimental): mirror the
  // observe-only surfacing above so unusable enforcement config is not silent.
  if (config_.rsl_cap_enforcement) {
    for (const auto& url : config_.rsl_cap_key_directories) {
      if (WebBotAuthDirectoryHosts({url}).empty()) {
        LogWarning(
            "RSL-CAP key-directory URL is not a valid https URL and will be "
            "ignored: %s",
            url.c_str());
      }
    }
    if (WebBotAuthDirectoryHosts(config_.rsl_cap_key_directories).empty()) {
      LogWarning(
          "RSL-CAP enforcement is enabled but no usable key-directory URL is "
          "configured — every token will be rejected as an unknown issuer "
          "(use --rsl-cap-key-directory)");
    }
    if (config_.rsl_cap_requested_license.empty() ||
        config_.rsl_cap_requested_scope.empty()) {
      LogWarning(
          "RSL-CAP enforcement is enabled but --rsl-cap-requested-license "
          "and/or --rsl-cap-requested-scope is empty — every valid token will "
          "be denied with 402 (fail-closed)");
    }
  }

  // Initialize the RSL-CAP key-directory refresh timer, a second
  // independent warmer warming realm "rsl" into pagespeed-rslcap-keys.conf.
  // Only started when enforcement is enabled AND directories are configured.
  if (config_.rsl_cap_enforcement && !config_.rsl_cap_key_directories.empty()) {
    rslcap_refresh_timer_ = std::make_unique<uv_timer_t>();
    uv_timer_init(loop_.get(), rslcap_refresh_timer_.get());
    rslcap_refresh_timer_->data = this;
    uv_timer_start(rslcap_refresh_timer_.get(), OnRslCapRefresh,
                   kWebBotAuthInitialDelayMs, kWebBotAuthRefreshIntervalMs);
    LogInfo(
        "RSL-CAP key-directory refresh timer started "
        "(%zu director%s configured)",
        config_.rsl_cap_key_directories.size(),
        config_.rsl_cap_key_directories.size() == 1 ? "y" : "ies");
  }

  // Initialize URL registry for cache URL enumeration.
  url_registry_ = std::make_unique<UrlRegistry>();

  // Known sites: every site this worker has stored work for, kept in a file
  // beside the cache so a purge after a restart still reaches each site's
  // entries.  Loaded here; written off the receive thread when the set
  // changes, at most once a second.
  if (const std::string sites_path = KnownSitesFilePath(config_.cache_path);
      !sites_path.empty()) {
    bool readable = true;
    size_t bad_lines = 0;
    std::vector<std::string> sites =
        ReadKnownSitesFile(sites_path, &readable, &bad_lines);
    known_sites_unreadable_at_start_ = !readable;
    url_registry_->LoadKnownSites(sites);
    if (!readable) {
      LogWarning(
          "Known-sites file %s could not be read or has another version; a "
          "purge without a site may miss entries stored before this start",
          sites_path.c_str());
    }
    if (bad_lines > 0) {
      LogWarning("Known-sites file %s: skipped %zu line(s) that are not sites",
                 sites_path.c_str(), bad_lines);
    }
    known_sites_writer_ = std::make_unique<KnownSitesWriter>(
        sites_path,
        [this] { return url_registry_->TakeKnownSitesIfChanged(); });
    known_sites_writer_->Start();
  }

  // The async agent-markdown write (BrowserAnalysisManager::OnAgentRenderDone)
  // adds a kAgentMarkdown alternate to a page URL OUTSIDE HandleNotification,
  // so the notification-time scope guard never sees it.  Wire a callback so the
  // worker refreshes that URL's cached alternate count after the markdown is
  // written, keeping /v1/cache/urls counts accurate for agent-optimized pages.
  if (browser_manager_) {
    browser_manager_->set_agent_alternate_written_callback(
        [this](const std::string& url, const std::string& hostname,
               const std::string& scheme, const std::string& site) {
          RefreshAlternateCount(url, hostname, scheme, site);
        });
  }

  // Initialize HTTP Management API server if a transport is configured.
  // api_socket_path: HTTP/1.1 over a group-scoped unix socket (the default
  // local transport).  api_port > 0: bind that TCP port.
  // api_port < 0: OS picks (for tests).
  if (config_.api_port != 0 || !config_.api_socket_path.empty()) {
    HttpServerConfig http_cfg;
    http_cfg.socket_path = config_.api_socket_path;
    http_cfg.port = config_.api_port > 0 ? config_.api_port : 0;
    if (!config_.api_bind_address.empty()) {
      http_cfg.bind_address = config_.api_bind_address;
    }
    http_cfg.auth_token = config_.api_token;
    // An unset token no longer opens the API.  It is legal
    // only when the operator asked for it, and main() has already refused to
    // start on any combination the invariant forbids.
    http_cfg.allow_unauthenticated = config_.api_no_auth;
    // DECOUPLED from the token (was `|| config_.api_token.empty()`): "reads
    // are open" is a choice an operator makes, not a side effect of a
    // credential they forgot.  That coupling was what force-opened the cached
    // URL inventory, the config and the stats on every tokenless API.
    http_cfg.read_open = config_.api_read_open;

    config_persister_ =
        std::make_unique<ConfigPersister>(loop_.get(), handler_);
    http_server_ =
        std::make_unique<HttpServer>(loop_.get(), http_cfg, handler_);

    // Wire up operational API context.  Constructed in place on the heap
    // (designated initializers) so the route lambdas below can capture a
    // stable address.
    api_ctx_ = std::unique_ptr<ApiContext>(new ApiContext{
        .stats = stats_,
        .get_config = [this]() { return live_config_.load(); },
        .update_config =
            [this](std::shared_ptr<const WorkerConfig> c) {
              UpdateConfig(std::move(c));
            },
        // The connections block describes the management API listener itself:
        // its live active count and the cap it actually enforces
        // (HttpServerConfig::max_connections), not the notification
        // listener's separate WorkerConfig::max_connections.  http_server_ is
        // created just above and stable for the worker's lifetime.
        .http_active_connections =
            [this]() { return http_server_->active_connections(); },
        .in_flight_work = [this]() { return in_flight_work(); },
        .cache_entries =
            [this]() {
              return cached_cache_entries_.load(std::memory_order_relaxed);
            },
        .cache_bytes =
            [this]() {
              return cached_cache_bytes_.load(std::memory_order_relaxed);
            },
        .cache_degraded = [this]() { return cache_degraded_; },
        .num_threads = config_.num_threads,
        .max_connections = http_cfg.max_connections,
        .start_time = start_time_,
        .started_at_ms = started_at_ms_,
        .serve_stats = serve_stats_,
        // Live read: browser_manager_ is created (or reset on init failure)
        // before the HTTP server starts and is stable afterwards.
        .browser_manager = [this]() -> const BrowserAnalysisManager* {
          return browser_manager_.get();
        },
        // Resolved once during Initialize(), before the HTTP
        // server starts, and never written again.
        .browser_sandbox_state = [this]() -> std::string {
          return BrowserSandboxStateName(browser_sandbox_state_);
        },
        // Read live from /proc/self/status on each call --
        // the daemon does not install the filter, so it must not cache an
        // answer it does not own.  One small /proc read; not a hot path.
        .syscall_filter_state = []() -> std::string {
          return SyscallFilterStateName(DetectSyscallFilter());
        },
        // The log ring read, wired to the WsManager constructed further down
        // in this block.  The lambda runs at request time, on the loop
        // thread; the HTTP server only starts accepting at the end of this
        // block (http_server_->Start()), after ws_manager_ exists.
        .read_logs = [this](uint64_t since, bool has_since,
                            size_t limit) -> nlohmann::json {
          return ws_manager_->BuildLogsResponse(since, has_since, limit);
        },
        .config_persister = config_persister_.get(),
    });
    RegisterOperationalRoutes(*http_server_, *api_ctx_);

    // Wire up cache API context if cache is available.
    if (cache_) {
      cache_api_ctx_ = std::make_unique<CacheApiContext>(CacheApiContext{
          .cache = cache_.get(),
          .url_registry = *url_registry_,
          .invalidate_url =
              [this](const std::string& url, const std::string& hostname,
                     const std::string& scheme, const std::string& site) {
                return InvalidateUrl(url, hostname, scheme, site);
              },
          .clear_dedup =
              [this](const std::string& url, const std::string& hostname,
                     const std::string& scheme, const std::string& site) {
                ClearDedupAndCooldown(url, hostname, scheme, site);
              },
          .enqueue_reprocess =
              [this](const std::string& url, const std::string& hostname,
                     const std::string& scheme, const std::string& site,
                     ContentType content_type) {
                // An operator request: it names a site but vouches for no
                // host and is not the module.
                CacheNotification notif;
                notif.url = url;
                notif.hostname = hostname;
                notif.scheme = scheme;
                notif.site = site;
                notif.host_vouched = false;
                notif.from_module = false;
                notif.content_type = content_type;
                notif.capability_mask = CapabilityMask().Encode();
                DispatchNotification(notif);
              },
          .reset_cache = [this]() -> std::string { return ResetCache(); },
          .url_norm_config = &url_norm_config_,
          .get_cooldown =
              [this](const std::string& url, const std::string& hostname,
                     const std::string& scheme, const std::string& site)
              -> std::optional<CacheApiContext::CooldownEntry> {
            auto info = GetCooldownForUrl(url, hostname, scheme, site);
            if (!info) return std::nullopt;
            return CacheApiContext::CooldownEntry{
                .url = url,
                .hostname = hostname,
                .scheme = scheme,
                .reason = CooldownReasonToString(info->reason),
                .remaining_seconds = info->remaining_seconds,
                .duration_seconds = info->duration_seconds,
            };
          },
          .list_cooldowns =
              [this]() -> std::vector<CacheApiContext::CooldownEntry> {
            auto entries = ListActiveCooldowns();
            std::vector<CacheApiContext::CooldownEntry> result;
            result.reserve(entries.size());
            for (auto& e : entries) {
              result.push_back(CacheApiContext::CooldownEntry{
                  .url = std::move(e.url),
                  .hostname = std::move(e.hostname),
                  .scheme = std::move(e.scheme),
                  .reason = CooldownReasonToString(e.reason),
                  .remaining_seconds = e.remaining_seconds,
                  .duration_seconds = e.duration_seconds,
              });
            }
            return result;
          },
          .known_sites_incomplete = [this] { return KnownSitesIncomplete(); },
      });
      RegisterCacheRoutes(*http_server_, *cache_api_ctx_);
    }

    // Wire up capture API context (browser screenshots + waterfall).
    capture_ctx_ = std::make_unique<CaptureContext>(CaptureContext{
        .browser_manager = browser_manager_.get(),
        .navigation_timeout_ms = 30000,
        .allow_private_urls = config_.allow_private_urls,
    });
    RegisterCaptureRoutes(*http_server_, *capture_ctx_);

    // Wire up WebSocket handlers.
    WsConfig ws_cfg;
    // On the unix socket the transport authenticates
    // the peer, so the WS layer carries no token there either -- matching
    // HttpServer::CheckAuth, which the handshake gate defers to.
    const bool api_over_socket = !config_.api_socket_path.empty();
    ws_cfg.auth_token = api_over_socket ? std::string() : config_.api_token;
    ws_cfg.read_open = config_.api_read_open;
    ws_cfg.allow_unauthenticated = config_.api_no_auth || api_over_socket;
    ws_manager_ = std::make_unique<WsManager>(loop_.get(), ws_cfg, handler_);

    // Stats provider produces the JSON snapshot for WebSocket /v1/ws/stats.
    // It calls the SAME BuildStatsJson() used by GET /v1/stats (via api_ctx_),
    // so the WS snapshot/delta payload can never drift from the REST contract
    // that the dashboard's TypeScript types and Metrics page depend on.  These
    // were previously two separate hand-maintained serializers; the WS one
    // omitted the svg/policy/quality_baselining groups and emitted `errors` as
    // a scalar, so the live (WS-fed) dashboard perpetually showed 0 for those
    // fields — e.g. SVG "Candidates Evaluated" — while the REST-fed Metrics
    // page was correct.
    ws_manager_->SetStatsProvider(
        [this]() -> nlohmann::json { return BuildStatsJson(*api_ctx_); });
    ws_manager_->Start();

    // Wrap handler_ with TeeMessageHandler so log output is also
    // pushed to /v1/ws/logs subscribers.
    {
      auto* ws = ws_manager_.get();
      tee_handler_ = std::make_unique<TeeMessageHandler>(
          handler_, [ws](MessageType type, std::string msg) {
            ws->PostLog(MessageTypeToLevel(type), "worker", "worker",
                        std::move(msg));
          });
      handler_ = tee_handler_.get();
    }

    // Update handlers that were created before the tee wrapper so
    // their warnings reach the WebSocket log stream.
    if (image_transcoder_) {
      image_transcoder_->set_handler(handler_);
    }
    if (browser_manager_) {
      // Browser analysis gets a dedicated handler that tags logs as
      // "chrome" source, matching the frontend's LogSource type.
      browser_log_handler_ = std::make_unique<TeeMessageHandler>(
          handler_, [this](MessageType type, std::string msg) {
            if (ws_manager_) {
              ws_manager_->PostLog(MessageTypeToLevel(type), "chrome",
                                   "browser", std::move(msg));
            }
          });
      browser_manager_->set_handler(browser_log_handler_.get());
    }

    // Wire WebSocket upgrade: HTTP server detects Upgrade headers and
    // hands the detached TCP connection to WsManager.
    http_server_->SetUpgradeHandler([this](HttpRequest request,
                                           uv_stream_t* handle) {
      // Extract endpoint from path: /v1/ws/stats, /v1/ws/events,
      // or /v1/ws/logs.
      std::string_view path = request.path;
      std::string endpoint;
      if (path == "/v1/ws/stats") {
        endpoint = "stats";
      } else if (path == "/v1/ws/events") {
        endpoint = "events";
      } else if (path == "/v1/ws/logs") {
        endpoint = "logs";
      } else {
        // Unknown WS endpoint — close the handle.
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
          delete reinterpret_cast<uv_any_handle*>(h);
        });
        return;
      }

      std::string_view ws_key = request.Header("sec-websocket-key");
      int interval_ms = 0;
      std::string_view interval_param = request.QueryParam("interval");
      if (!interval_param.empty()) {
        try {
          interval_ms = std::stoi(std::string(interval_param));
        } catch (...) {  // NOLINT(bugprone-empty-catch)
          // Ignore invalid interval — use default.
        }
      }

      ws_manager_->AcceptUpgrade(handle, endpoint, ws_key, interval_ms);
    });

    // Load web console static files if configured.
    if (!config_.console_dir.empty()) {
      console_cache_ = std::make_unique<StaticFileCache>();
      if (console_cache_->Load(config_.console_dir, 50ULL * 1024 * 1024,
                               handler_)) {
        RegisterConsoleRoute(*http_server_, *console_cache_,
                             config_.security_headers);
      } else {
        LogWarning("Failed to load console from %s",
                   config_.console_dir.c_str());
        console_cache_.reset();
      }
    }

    if (!http_server_->Start()) {
      if (!config_.api_socket_path.empty()) {
        LogError("Failed to start HTTP API server on socket %s",
                 config_.api_socket_path.c_str());
      } else {
        LogError("Failed to start HTTP API server on port %d",
                 config_.api_port);
      }
      // Clean up WsManager before returning — it started above.
      ws_manager_->Stop();
      return false;
    }

    if (!config_.api_socket_path.empty()) {
      LogInfo("HTTP API server listening on unix socket %s",
              config_.api_socket_path.c_str());
    } else {
      LogInfo("HTTP API server listening on port %d",
              http_server_->bound_port());
    }
  }

  LogInfo("Worker listening on %s (health: %s, mgmt: %s)",
          config_.socket_path.c_str(), health_path.c_str(), mgmt_path.c_str());

  return true;
}

void Worker::UpdateConfig(std::shared_ptr<const WorkerConfig> new_config) {
  live_config_.store(std::move(new_config));
}

bool Worker::AgentMarkdownNeedsBuild(
    const std::string& url, const std::string& hostname,
    const std::string& scheme, std::string_view site,
    const std::array<std::byte, 32>& origin_hash) const {
  if (!cache_) return false;
  auto md = cache_->ReadAlternate(
      url, hostname, scheme,
      static_cast<AlternateId>(SentinelId::kAgentMarkdown), site);
  if (!md.has_value()) return true;  // never built -> build it
  // Built — rebuild only if its binding no longer matches the current origin.
  const auto& stamp = md->metadata.origin_html_hash;
  return std::memcmp(stamp.data(), origin_hash.data(),
                     AlternateMetadata::kHashSize) != 0;
}

void Worker::Run() {
  running_ = true;
  uv_run(loop_.get(), UV_RUN_DEFAULT);
}

void Worker::RequestShutdown() {
  // Async-signal-safe: only touches atomics and uv_async_send.
  running_.store(false, std::memory_order_relaxed);
  if (shutdown_async_) {
    uv_async_send(shutdown_async_.get());
  }
}

void Worker::Shutdown() {
  if (!running_) {
    return;
  }

  running_ = false;

  // Signal shutdown via async handle (thread-safe)
  if (shutdown_async_) {
    uv_async_send(shutdown_async_.get());
  }
}

void Worker::OnShutdownAsync(uv_async_t* handle) {
  // Called on the loop thread - safe to begin graceful shutdown
  auto* worker = static_cast<Worker*>(handle->data);
  if (worker && worker->loop_) {
    worker->BeginGracefulShutdown();
  }
}

void Worker::BeginGracefulShutdown() {
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;

  LogInfo("Worker shutting down");

  // Clean up socket files (Named Pipes on Windows are auto-cleaned).
#ifndef _WIN32
  unlink(config_.socket_path.c_str());
  unlink(health_socket_path().c_str());
  unlink(mgmt_socket_path().c_str());
#endif

  // Stop accepting new connections on both sockets
  if (server_) SafeClose(server_.get());
  if (health_server_) SafeClose(health_server_.get());
  if (mgmt_server_) SafeClose(mgmt_server_.get());
  if (stats_refresh_timer_) SafeTimerClose(stats_refresh_timer_.get());
  if (lag_timer_) SafeTimerClose(lag_timer_.get());
  if (webbotauth_refresh_timer_)
    SafeTimerClose(webbotauth_refresh_timer_.get());
  // Issue #164: cancel a queued Web Bot Auth refresh so shutdown never waits
  // out its bounded fetches.  If the work hasn't started on the thread pool
  // yet, uv_cancel succeeds and the done-callback fires with UV_ECANCELED
  // (freeing req+work); if it has already started, the after-callback runs
  // normally on completion.  Either way the destructor's drain loop
  // guarantees the after-callback runs before uv_loop_close, so the
  // allocations are always reclaimed.
  if (webbotauth_req_ != nullptr) {
    uv_cancel(reinterpret_cast<uv_req_t*>(webbotauth_req_));
  }
  if (rslcap_refresh_timer_) SafeTimerClose(rslcap_refresh_timer_.get());
  if (rslcap_req_ != nullptr) {
    uv_cancel(reinterpret_cast<uv_req_t*>(rslcap_req_));
  }
  if (browser_manager_) {
    browser_manager_->Shutdown();
  }

  // Stop HTTP API server and WebSocket manager.
  if (ws_manager_) {
    ws_manager_->Stop();
  }
  if (http_server_) {
    http_server_->Stop();
  }
  // After the server stops: a configuration save already handed to the
  // persister still lands, then its thread is joined and its handle closed
  // while the loop can still run the close callback.
  if (config_persister_) {
    config_persister_->Shutdown();
  }

  LogInfo("Graceful shutdown: %d active connections",
          active_connections_.load());

  // If no active connections or in-flight work, stop immediately
  if (active_connections_.load() == 0 && in_flight_work_.load() == 0) {
    uv_stop(loop_.get());
    return;
  }

  // Start shutdown timer
  shutdown_timer_ = std::make_unique<uv_timer_t>();
  uv_timer_init(loop_.get(), shutdown_timer_.get());
  shutdown_timer_->data = this;
  uv_timer_start(shutdown_timer_.get(), OnShutdownTimeout,
                 config_.shutdown_timeout_ms, 0);
}

void Worker::OnShutdownTimeout(uv_timer_t* timer) {
  auto* worker = static_cast<Worker*>(timer->data);
  if (worker->handler_ != nullptr) {
    worker->handler_->Warning(
        "Shutdown timeout: force-closing %d remaining connections",
        worker->active_connections_.load());
  }

  // Force close all remaining handles and stop the loop
  uv_walk(worker->loop_.get(), CloseWalkCallback, nullptr);
  uv_stop(worker->loop_.get());
}

void Worker::CheckShutdownComplete() {
  if (shutting_down_ && active_connections_.load() == 0 &&
      in_flight_work_.load() == 0) {
    LogInfo("All connections drained, shutting down");
    // Stop the shutdown timer if it is still running
    if (shutdown_timer_) SafeTimerClose(shutdown_timer_.get());
    uv_stop(loop_.get());
  }
}

// =================================================================
// Periodic cache stats refresh
// =================================================================

void Worker::OnStatsRefresh(uv_timer_t* timer) {
  auto* worker = static_cast<Worker*>(timer->data);
  if (worker->url_registry_) {
    worker->stats_.known_sites_overflow.store(
        worker->url_registry_->known_sites_overflow(),
        std::memory_order_relaxed);
    worker->stats_.known_sites_incomplete.store(
        worker->KnownSitesIncomplete() ? 1 : 0, std::memory_order_relaxed);
  }
  if (worker->cache_) {
    auto cs = worker->cache_->Stats();
    worker->cached_cache_entries_.store(cs.current_entries,
                                        std::memory_order_relaxed);
    worker->cached_cache_bytes_.store(cs.current_bytes,
                                      std::memory_order_relaxed);
    worker->cached_hit_flush_ok_.store(cs.hit_flush_successes,
                                       std::memory_order_relaxed);
    worker->cached_hit_flush_fail_.store(cs.hit_flush_failures,
                                         std::memory_order_relaxed);
    worker->cached_hit_flush_delta_.store(cs.hit_flush_total_delta,
                                          std::memory_order_relaxed);
    worker->cached_hit_flush_fsyncs_.store(cs.hit_flush_fsyncs,
                                           std::memory_order_relaxed);
  }
}

// =================================================================
// Event loop lag gauge
// =================================================================

void Worker::OnLagTimer(uv_timer_t* timer) {
  auto* worker = static_cast<Worker*>(timer->data);
  auto now = std::chrono::steady_clock::now();
  // Issue #165: load the previous schedule time from the atomic ticks.
  std::chrono::steady_clock::time_point scheduled{
      std::chrono::steady_clock::duration{
          worker->lag_scheduled_ticks_.load(std::memory_order_relaxed)}};
  auto elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(now - scheduled)
          .count();
  // Subtract expected 1000ms interval to get pure lag.
  int64_t lag = elapsed_us - 1000000;
  if (lag < 0) lag = 0;
  worker->event_loop_lag_us_.store(static_cast<uint64_t>(lag),
                                   std::memory_order_relaxed);
  worker->lag_scheduled_ticks_.store(now.time_since_epoch().count(),
                                     std::memory_order_relaxed);

  // Mirror the in-flight backlog into the shared counting surface as an
  // integral (sum + sample count + high-water mark), so a front-end-side reader
  // can compute a mean backlog over any interval by differencing two reads
  // instead of depending on how often it polls.  Observation only: nothing
  // reads these back to make a decision, and the dispatch path is untouched.
  //
  // Riding this timer is deliberate — it is the loop-health tick, so the same
  // starvation that would make these samples sparse is already measured by the
  // lag gauge published beside them, and a reader can tell an under-sampled
  // window from a quiet one.  The cadence is therefore this timer's: one
  // sample per second, stated in the header so a reader can check the sample
  // count against a wall-clock delta.
  const int in_flight = worker->in_flight_work_.load(std::memory_order_relaxed);
  RecordSaturationSample(worker->serve_stats_,
                         in_flight > 0 ? static_cast<uint32_t>(in_flight) : 0u);
}

// =================================================================
// Health check socket
// =================================================================

void Worker::OnHealthConnection(uv_stream_t* server, int status) {
  auto* worker = static_cast<Worker*>(server->data);

  if (status < 0) {
    return;
  }

  auto* client = new uv_pipe_t();
  uv_pipe_init(worker->loop_.get(), client, 0);

  if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client)) != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(client),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
    return;
  }

  // Format health response with stats (uses cached values to avoid
  // contending with Cyclone locks on the event loop).
  uint64_t cache_entries =
      worker->cached_cache_entries_.load(std::memory_order_relaxed);
  const auto& s = worker->stats_;
  const char* status_word = worker->cache_degraded_ ? "DEGRADED" : "OK";
  std::string response = absl::StrCat(
      status_word, " ", worker->active_connections_.load(), "/",
      worker->config_.max_connections,
      " notifs=", s.notifications_received.load(std::memory_order_relaxed),
      " variants=", s.variants_written.load(std::memory_order_relaxed),
      " proactive=",
      s.proactive_variants_written.load(std::memory_order_relaxed),
      " errors=", s.errors.load(std::memory_order_relaxed),
      " cache_entries=", cache_entries,
      " inflight=", worker->in_flight_work_.load(std::memory_order_relaxed));
  if (worker->cache_degraded_) {
    absl::StrAppend(&response, " cache=unavailable");
  }
  absl::StrAppend(&response, "\n");

  // Allocate write context with response data.
  auto* ctx = new HealthWriteContext();
  ctx->buf.base = new char[response.size()];
  memcpy(ctx->buf.base, response.data(), response.size());
  ctx->buf.len = response.size();
  ctx->client = client;
  ctx->req.data = ctx;

  int r = uv_write(&ctx->req, reinterpret_cast<uv_stream_t*>(client), &ctx->buf,
                   1, OnHealthWriteDone);
  if (r != 0) {
    // Write failed synchronously — callback won't fire.  Clean up.
    delete[] ctx->buf.base;
    delete ctx;
    uv_close(reinterpret_cast<uv_handle_t*>(client),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
  }
}

void Worker::OnHealthWriteDone(uv_write_t* req, int /*status*/) {
  auto* ctx = static_cast<HealthWriteContext*>(req->data);
  delete[] ctx->buf.base;

  uv_close(reinterpret_cast<uv_handle_t*>(ctx->client),
           [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
  delete ctx;
}

// =================================================================
// Client connection management
// =================================================================

// Close callback for a client pipe.  Frees the context (including
// the already-closed timeout timer) and the pipe handle, then
// decrements the active connection count.
void Worker::OnClientClose(uv_handle_t* handle) {
  auto* ctx = GetClientContext(reinterpret_cast<uv_stream_t*>(handle));
  Worker* worker = ctx->worker;

  // The timeout timer was already closed before the pipe close was
  // initiated, so it is safe to delete here.
  delete ctx->timeout_timer;
  delete ctx;
  delete reinterpret_cast<uv_pipe_t*>(handle);

  // Decrement connection count and check shutdown
  worker->active_connections_--;
  worker->CheckShutdownComplete();
}

void Worker::OnConnectionTimeout(uv_timer_t* timer) {
  auto* ctx = static_cast<ClientContext*>(timer->data);
  if (ctx->closing) return;  // Already being closed by another path
  ctx->closing = true;

  // Only warn if there was a partial message in the buffer — idle
  // persistent connections timing out are normal (nginx keeps a
  // long-lived socket and we reclaim it after the idle period).
  if (ctx->worker->handler_ && !ctx->buffer.empty()) {
    ctx->worker->handler_->Warning(
        "Connection timeout with partial data (%zu bytes), disconnecting",
        ctx->buffer.size());
  }

  // Stop timer, close it, then close the pipe in the timer's
  // close callback.
  uv_timer_stop(timer);
  uv_close(reinterpret_cast<uv_handle_t*>(timer), OnTimerClosedThenPipe);
}

// Timer close callback: close the client pipe next.  Guards against
// force shutdown (uv_walk) having already closed the pipe.
void Worker::OnTimerClosedThenPipe(uv_handle_t* h) {
  auto* c = static_cast<ClientContext*>(reinterpret_cast<uv_timer_t*>(h)->data);
  if (IsClosing(c->pipe)) return;
  SafeClose(c->pipe, OnClientClose);
}

// Helper: initiate close sequence for a client.  First closes the
// timeout timer, then closes the pipe in the timer's close callback.
void Worker::CloseClientConnection(ClientContext* ctx) {
  if (ctx->closing) return;  // Already being closed by another path
  ctx->closing = true;

  uv_timer_stop(ctx->timeout_timer);
  uv_close(reinterpret_cast<uv_handle_t*>(ctx->timeout_timer),
           OnTimerClosedThenPipe);
}

void Worker::OnNewConnection(uv_stream_t* server, int status) {
  auto* worker = static_cast<Worker*>(server->data);

  if (status < 0) {
    if (worker->handler_ != nullptr) {
      worker->handler_->Warning("Connection error: %s", uv_strerror(status));
    }
    return;
  }

  // Check max connections limit
  if (worker->active_connections_.load() >= worker->config_.max_connections) {
    if (worker->handler_ != nullptr) {
      worker->handler_->Warning("Max connections reached (%d), rejecting",
                                worker->config_.max_connections);
    }
    // Must accept and immediately close to drain the kernel queue
    uv_pipe_t* reject = new uv_pipe_t();
    uv_pipe_init(worker->loop_.get(), reject, 0);
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(reject)) == 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(reject),
               [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
    } else {
      uv_close(reinterpret_cast<uv_handle_t*>(reject),
               [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
    }
    return;
  }

  // Create client pipe
  uv_pipe_t* client = new uv_pipe_t();
  uv_pipe_init(worker->loop_.get(), client, 0);

  // Set up client context
  auto* context = new ClientContext();
  context->worker = worker;
  context->pipe = client;
  client->data = context;

  // Accept connection
  if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client)) == 0) {
    worker->active_connections_++;

    // Create and start connection timeout timer
    context->timeout_timer = new uv_timer_t();
    uv_timer_init(worker->loop_.get(), context->timeout_timer);
    context->timeout_timer->data = context;
    uv_timer_start(context->timeout_timer, OnConnectionTimeout,
                   worker->config_.connection_timeout_ms, 0);

    uv_read_start(reinterpret_cast<uv_stream_t*>(client), OnAlloc,
                  OnClientRead);
  } else {
    delete context;
    uv_close(reinterpret_cast<uv_handle_t*>(client), [](uv_handle_t* handle) {
      delete reinterpret_cast<uv_pipe_t*>(handle);
    });
  }
}

void Worker::OnAlloc(uv_handle_t* /*handle*/, size_t suggested_size,
                     uv_buf_t* buf) {
  buf->base = new char[suggested_size];
  buf->len = suggested_size;
}

void Worker::OnClientRead(uv_stream_t* client, ssize_t nread,
                          const uv_buf_t* buf) {
  auto* context = GetClientContext(client);
  Worker* worker = context->worker;

  // If the connection is already being torn down (e.g. OnConnectionTimeout
  // fired, set closing=true, and scheduled uv_close on timeout_timer), do not
  // touch the context or restart the timer below — restarting a timer that is
  // mid-close is a use-after-free. Mirrors the OnMgmtRead guard. delete[] on a
  // null base is a no-op.
  if (context->closing) {
    delete[] buf->base;
    return;
  }

  if (nread < 0) {
    if (nread != UV_EOF && worker->handler_) {
      worker->handler_->Warning("Read error: %s", uv_strerror(nread));
    }

    delete[] buf->base;

    // Close connection via timer-then-pipe sequence
    CloseClientConnection(context);
    return;
  }

  if (nread > 0) {
    // Check buffer size limit before appending
    if (context->buffer.size() + static_cast<size_t>(nread) >
        worker->config_.max_buffer_size) {
      if (worker->handler_ != nullptr) {
        worker->handler_->Warning(
            "Client buffer exceeded max size (%zu), disconnecting",
            worker->config_.max_buffer_size);
      }
      delete[] buf->base;
      CloseClientConnection(context);
      return;
    }

    // Reset connection timeout on activity
    uv_timer_start(context->timeout_timer, OnConnectionTimeout,
                   worker->config_.connection_timeout_ms, 0);

    // Append to buffer
    context->buffer.append(buf->base, nread);

    // Process all complete notifications in the buffer.  Multiple
    // messages can arrive in a single read when the sender is fast
    // or when TCP coalesces small writes.
    while (context->buffer.size() >= 4) {
      // Read length header (big-endian uint32)
      uint32_t msg_len =
          (static_cast<uint32_t>(static_cast<unsigned char>(context->buffer[0]))
           << 24) |
          (static_cast<uint32_t>(static_cast<unsigned char>(context->buffer[1]))
           << 16) |
          (static_cast<uint32_t>(static_cast<unsigned char>(context->buffer[2]))
           << 8) |
          static_cast<uint32_t>(static_cast<unsigned char>(context->buffer[3]));

      // Reject messages larger than max_request_size
      if (msg_len > worker->config_.max_request_size) {
        if (worker->handler_ != nullptr) {
          worker->handler_->Warning(
              "Message too large (%u > %zu), disconnecting", msg_len,
              worker->config_.max_request_size);
        }
        delete[] buf->base;
        CloseClientConnection(context);
        return;
      }

      size_t total_len = 4 + static_cast<size_t>(msg_len);
      if (context->buffer.size() < total_len) {
        break;  // Incomplete message, wait for more data
      }

      // Parse notification from the first total_len bytes.
      //
      // A refusal is never a best-effort parse: Deserialize stops at the
      // version byte when the peer speaks another version, and the two
      // refusal classes are counted and reported separately here because a
      // version-skewed peer and a corrupt frame look identical from the
      // outside (nothing is optimized) and have nothing in common as fixes.
      CacheNotification notification;
      IpcRejection rejection;
      std::string_view msg_view(context->buffer.data(), total_len);
      if (CacheNotification::Deserialize(msg_view, &notification, &rejection)) {
        // Dispatch to thread pool (fire-and-forget, no response)
        worker->DispatchNotification(notification);
      } else if (rejection.reason == IpcRejectReason::kVersionMismatch) {
        worker->stats_.notifications_rejected_version.fetch_add(
            1, std::memory_order_relaxed);
        worker->WarnIpcVersionMismatch(rejection.peer_version);
      } else {
        worker->stats_.notifications_rejected_malformed.fetch_add(
            1, std::memory_order_relaxed);
        if (worker->handler_ != nullptr) {
          worker->handler_->Warning("Failed to deserialize notification");
        }
      }

      // Remove processed bytes, keep any remaining data
      context->buffer.erase(0, total_len);
    }
  }

  delete[] buf->base;
}

// =================================================================
// Thread pool dispatch
// =================================================================

void Worker::WarnIpcVersionMismatch(uint8_t peer_version) {
  const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

  // Each distinct peer version logs the first time it is seen — a second,
  // differently skewed peer is a fact of its own and must not be hidden by the
  // timer the first one armed.  Because the set is remembered (not just the
  // last version), that immediate path is bounded at one line per version
  // value for the life of the process, so a sender flipping version bytes
  // cannot use it to flood the log.
  const uint64_t bit = uint64_t{1} << (peer_version & 63U);
  const uint64_t seen = ipc_peer_versions_seen_[peer_version >> 6].fetch_or(
      bit, std::memory_order_relaxed);
  bool should_log = (seen & bit) == 0;
  if (should_log) {
    last_ipc_version_warn_us_.store(now_us, std::memory_order_relaxed);
  } else {
    // Repeat of a version already reported: at most one line per minute.  The
    // condition stays permanently readable on the stats surface, so the log
    // only has to make it discoverable, not narrate every message.
    int64_t prev_us = last_ipc_version_warn_us_.load(std::memory_order_relaxed);
    constexpr int64_t kWarnIntervalUs = int64_t{60} * 1'000'000;  // 1 minute
    should_log = (now_us - prev_us >= kWarnIntervalUs) &&
                 last_ipc_version_warn_us_.compare_exchange_strong(
                     prev_us, now_us, std::memory_order_relaxed);
  }
  if (!should_log) {
    return;
  }
  LogWarning(
      "Notification rejected: peer sent wire version %u, this build speaks "
      "version %u. The notification was dropped unparsed and the resource it "
      "named is served without optimization; nothing is cached from a message "
      "this build cannot read. Align the versions of the components sending "
      "notifications (each version logs once, then at most 1/min)",
      static_cast<unsigned>(peer_version), static_cast<unsigned>(kIpcVersion));
}

// The /llms.txt build and the browser analysis with its agent render fetch
// from the notification's hostname.  Only the senders that check that
// hostname against the names the site serves may ask for them: the nginx
// module and the ASP.NET package, which send no options context.  A
// notification that carries one comes from a front end that asks for none of
// this work, so it never starts a fetch from its hostname.
bool Worker::MayRequestHostFetches(const CacheNotification& notification) {
  return notification.option_context.empty();
}

bool Worker::AcceptOptionContext(const CacheNotification& notification) {
  const OptionContextStatus status = ValidateOptionContext(
      notification.option_context, notification.option_signature);

  // A VALID context is accepted, whatever it names, and the work is done and
  // stored under the DEFAULT context -- the same key this engine has always
  // used.  Why that is correct rather than merely convenient:
  //
  // Nothing derived from the sender's resolved configuration reaches the
  // optimizer.  Take the inputs that shape an optimized byte one at a time:
  // the capability mask and the content type are read off the request and the
  // response being optimized; the agent-request bit is the request's own
  // declared intent gated by an operator flag THIS side publishes into shared
  // config, so it too carries nothing the sender resolved for itself; and
  // every rewriter parameter (the quality targets, the size ceilings, the
  // filter enables) is read from this daemon's own WorkerConfig, which no
  // notification can reach.  The payload itself is opaque here by
  // construction: nothing in this engine reads an option out of it.  So two
  // requests that resolved to two different option
  // contexts cannot produce two different optimized artifacts on this surface,
  // and one stored artifact is the right answer for both.  Sharing output
  // between contexts would be wrong if the output could depend on the context;
  // here it cannot.
  //
  // The signature is therefore validated ADMISSION METADATA, not a namespace
  // selector at this build.  Re-deriving it from the payload is how two
  // implementations of the context format learn on the very first message that
  // they have drifted, so that check stays exactly as strict -- the four
  // invalid statuses below are still refused, and nothing they carry is acted
  // on.  (The cache does carry a context-keyed key composition --
  // PageSpeedCache::ComposeKeyPreNormalized's four-argument overload -- and it
  // stays unused until something in the optimizer actually varies with a
  // context.  Keying by a signature that separates nothing would only split
  // one warm cache into several cold ones.)
  if (status == OptionContextStatus::kOk ||
      status == OptionContextStatus::kEmpty) {
    if (status == OptionContextStatus::kOk &&
        !IsDefaultOptionContext(notification.option_signature)) {
      stats_.notifications_accepted_non_default_option_context.fetch_add(
          1, std::memory_order_relaxed);

      // Said ONCE for the life of the process, and no more: a sender supplying
      // contexts is a fact worth stating (where its work lands is not obvious
      // from the sender's side), but it is normal operation, not a problem, so
      // it gets one informational line and the counter carries the continuous
      // signal.  Deliberately does NOT arm last_option_context_warn_us_ --
      // acceptance must never be able to delay or suppress a refusal warning.
      constexpr uint8_t kAcceptedNonDefaultBit = 0x80U;
      const uint8_t accepted_seen = option_context_conditions_seen_.fetch_or(
          kAcceptedNonDefaultBit, std::memory_order_relaxed);
      if ((accepted_seen & kAcceptedNonDefaultBit) == 0) {
        LogInfo(
            "Notification carries a non-default per-request options context "
            "(%.16s...): accepted, and the work is stored under the default "
            "context. Optimized output on this build is a function of the "
            "resource and the request, never of the sender's configuration, so "
            "one stored artifact is correct for every context; the signature "
            "is still validated byte-exactly, which is what catches two "
            "implementations of the context format drifting apart (logged "
            "once; the notifications_accepted_non_default_option_context "
            "counter is continuous)",
            notification.option_signature.c_str());
      }
    }
    return true;
  }

  stats_.notifications_rejected_option_context.fetch_add(
      1, std::memory_order_relaxed);

  const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

  // Each distinct CONDITION logs the first time it is seen, exactly as each
  // distinct peer version does above, and for the same reason: a second,
  // differently broken peer is a fact of its own and must not be hidden by the
  // timer the first one armed.  Without this, a steady stream of oversized or
  // unknown-format payloads would suppress the signature-mismatch line
  // indefinitely -- and that is the one condition here that actually needs a
  // human, because it means the two implementations of the context format have
  // drifted.  The set is remembered rather than the last value, so the
  // immediate path is bounded at one line per condition for the life of the
  // process and cannot be used to flood the log.
  const auto condition_bit =
      static_cast<uint8_t>(1U << static_cast<unsigned>(status));
  const uint8_t seen = option_context_conditions_seen_.fetch_or(
      condition_bit, std::memory_order_relaxed);
  bool should_log = (seen & condition_bit) == 0;
  if (should_log) {
    last_option_context_warn_us_.store(now_us, std::memory_order_relaxed);
  } else {
    // Repeat of a condition already reported: at most one line per minute.  The
    // counter stays continuous on the stats surface, so the log only has to
    // make the condition discoverable, not narrate every message.
    int64_t prev_us =
        last_option_context_warn_us_.load(std::memory_order_relaxed);
    constexpr int64_t kWarnIntervalUs = int64_t{60} * 1'000'000;  // 1 minute
    should_log = (now_us - prev_us >= kWarnIntervalUs) &&
                 last_option_context_warn_us_.compare_exchange_strong(
                     prev_us, now_us, std::memory_order_relaxed);
  }
  if (should_log) {
    LogWarning(
        "Notification rejected: its per-request options context was refused "
        "(%.*s). The notification was dropped and the resource it named is "
        "served without optimization. A signature-mismatch here means the "
        "sending and receiving implementations of the context format have "
        "diverged and need aligning; a size or format refusal means the "
        "sender is from a different release (at most 1 line/min; the "
        "notifications_rejected_option_context counter is continuous)",
        static_cast<int>(OptionContextStatusName(status).size()),
        OptionContextStatusName(status).data());
  }
  return false;
}

bool Worker::AcceptSiteAndSender(const CacheNotification& notification) {
  if (notification.legacy_ex_caller) {
    // An old module: its frames are processed unsited, as before.
    stats_.notifications_legacy_ex_caller.fetch_add(1,
                                                    std::memory_order_relaxed);
  }
  if (!notification.from_module) return true;
  // The module keys every entry it records by its site, so a notification
  // from it without one names no entry it wrote.
  if (notification.site.empty()) {
    stats_.notifications_rejected_empty_site.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }
  // HTML from the module: only a page its HTML store wrote, for a host it
  // vouched for, and only while this worker derives Markdown copies.  The
  // stored entry itself is checked on the thread pool, before any byte of it
  // is copied (HandleModuleHtmlNotification): no cache read happens here.
  if (notification.content_type == ContentType::kHtml) {
    if (ModuleHtmlFrameAccepted(notification, *live_config_.load())) {
      return true;
    }
    stats_.notifications_rejected_module_render.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }
  // Anything else from the module asks for no page work.  Work that fetches
  // from the notification's host never starts from a module notification.
  if (notification.capability_mask == kLlmsTxtSentinel ||
      notification.agent_request || notification.host_vouched) {
    stats_.notifications_rejected_module_render.fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool Worker::ModuleHtmlFrameAccepted(const CacheNotification& notification,
                                     const WorkerConfig& cfg) {
  // Every flag of the stored-page shape, not just html_stored: the wire
  // check (HtmlStoredShapeValid) already refuses html_stored without the
  // others, and this does not rely on it.
  // A plain capability mask only: a sentinel-shaped one (viewport bits 3:
  // llms.txt, warmup, origin-refresh and every reserved class) names work
  // this path does not do, and a warmup mask would also bypass the in-flight
  // guard that keeps one pass per page.
  const bool plain_mask = ((notification.capability_mask >> 2) & 0x03U) != 3;
  return notification.from_module &&
         notification.content_type == ContentType::kHtml &&
         notification.html_stored && notification.host_vouched &&
         !notification.site.empty() && plain_mask &&
         HtmlModeIsMarkdown(cfg.html_mode) &&
         cfg.browser_analysis.agent_optimize;
}

bool Worker::KnownSitesIncomplete() const {
  return known_sites_unreadable_at_start_ ||
         (url_registry_ != nullptr &&
          url_registry_->known_sites_overflow() > 0);
}

void Worker::DispatchNotification(const CacheNotification& notification) {
  stats_.notifications_received.fetch_add(1, std::memory_order_relaxed);

  // Before anything else touches this notification: if it declares an option
  // context that fails validation, it does not get optimized at all. The
  // ordering is the point — a context that cannot be trusted must be refused
  // before any source is read, not after work has already been done on it. A
  // context that validates is accepted here whatever it names; see
  // AcceptOptionContext for why that is safe on this surface.
  if (!AcceptOptionContext(notification)) {
    return;
  }
  // Then the site and the sender bits, still before any read.
  if (!AcceptSiteAndSender(notification)) {
    return;
  }

  // Reject new work during shutdown to prevent the race where a queued
  // notification callback dispatches work after BeginGracefulShutdown
  // has already checked in_flight_work_ == 0.
  if (shutting_down_) {
    return;
  }

  // URL length validation (fast, stays on event loop)
  if (notification.url.size() > config_.max_url_length) {
    LogWarning("URL too long (%zu > %zu), skipping: %.100s...",
               notification.url.size(), config_.max_url_length,
               notification.url.c_str());
    stats_.errors.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Normalize URL and hostname for cache key consistency (defense-in-depth).
  CacheNotification normalized = notification;
  normalized.url = NormalizeCacheUrl(notification.url, url_norm_config_);
  normalized.hostname =
      NormalizeCacheHostname(notification.hostname, url_norm_config_);

  // In-flight dedup: skip if this URL+hostname is already being
  // processed in the thread pool.  Both DispatchNotification and
  // OnNotificationDone run on the event loop, so no locking needed.
  // Warmup sentinels are excluded — they use a different code path
  // and the same URL may need both normal and warmup processing.
  std::string dedup_key;
  bool tracked = false;
  if (normalized.capability_mask != kWarmupSentinel) {
    dedup_key = ComposeInternalKey(normalized.url, normalized.hostname,
                                   normalized.scheme, normalized.site);
    auto [it, inserted] = in_flight_urls_.insert(dedup_key);
    if (!inserted) {
      stats_.notifications_skipped_inflight.fetch_add(
          1, std::memory_order_relaxed);
      // An origin-refreshed sentinel carries information that no later
      // request is guaranteed to resend (a forced reload that revalidated
      // the origin sends it once).  Hold one per URL and dispatch it when
      // the running item finishes, rather than letting it vanish.  It is
      // still not allowed to run beside the item in flight.
      if (normalized.capability_mask == kOriginRefreshedSentinel) {
        pending_origin_refresh_[dedup_key] = std::move(normalized);
      } else if (normalized.from_module &&
                 normalized.content_type == ContentType::kHtml) {
        // Held, not dropped: see pending_module_html_.  A held agent request
        // is not given up for a later store frame.
        auto [held, fresh] =
            pending_module_html_.try_emplace(dedup_key, normalized);
        if (!fresh) {
          const bool agent = held->second.agent_request;
          held->second = std::move(normalized);
          held->second.agent_request = held->second.agent_request || agent;
        }
      }
      return;
    }
    tracked = true;
  }

  // Capture the purge-fence baseline (per-URL + global generation) at
  // dispatch time for stale-notification detection.  Taking
  // purge_gen_mutex_ here orders this capture against
  // PurgeUrlAndBumpGeneration: observing a post-purge generation implies
  // the purge's Remove() has already completed.
  PurgeDispatchGen current_purge_gen = CapturePurgeGen(
      normalized.url, normalized.hostname, normalized.scheme, normalized.site);

  auto* ctx = new NotificationWorkContext();
  ctx->worker = this;
  ctx->notification = normalized;  // Copy normalized
  ctx->purge_gen = current_purge_gen;
  if (tracked) {
    ctx->dedup_key = std::move(dedup_key);
  }
  ctx->req.data = ctx;

  in_flight_work_.fetch_add(1, std::memory_order_relaxed);

  int rc = uv_queue_work(loop_.get(), &ctx->req, OnNotificationWork,
                         OnNotificationDone);
  if (rc != 0) {
    in_flight_work_.fetch_sub(1, std::memory_order_relaxed);
    if (!ctx->dedup_key.empty()) {
      in_flight_urls_.erase(ctx->dedup_key);
    }
    LogWarning("Failed to queue work: %s", uv_strerror(rc));
    delete ctx;
  }
}

void Worker::OnNotificationWork(uv_work_t* req) {
  auto* ctx = static_cast<NotificationWorkContext*>(req->data);
  ctx->worker->HandleNotification(ctx->notification, ctx->purge_gen);
}

void Worker::OnNotificationDone(uv_work_t* req, int status) {
  auto* ctx = static_cast<NotificationWorkContext*>(req->data);

  if (status != 0) {
    // Work was cancelled (shutdown).  The Worker's atomic counter is
    // still safe to touch (its memory is live during the destructor's
    // uv_run cleanup loop), but we must not call CheckShutdownComplete
    // or access non-atomic fields (Worker may be partially destroyed).
    ctx->worker->in_flight_work_.fetch_sub(1, std::memory_order_relaxed);
    delete ctx;
    return;
  }

  Worker* worker = ctx->worker;
  std::optional<CacheNotification> held_refresh;
  std::optional<CacheNotification> held_module_html;
  if (!ctx->dedup_key.empty()) {
    worker->in_flight_urls_.erase(ctx->dedup_key);
    auto held = worker->pending_origin_refresh_.find(ctx->dedup_key);
    if (held != worker->pending_origin_refresh_.end()) {
      held_refresh = std::move(held->second);
      worker->pending_origin_refresh_.erase(held);
    }
    auto module_held = worker->pending_module_html_.find(ctx->dedup_key);
    if (module_held != worker->pending_module_html_.end()) {
      held_module_html = std::move(module_held->second);
      worker->pending_module_html_.erase(module_held);
    }
  }
  delete ctx;

  // Dispatch before releasing this item's in-flight count so the worker is
  // never observed idle between the two.
  if (held_refresh) {
    worker->DispatchNotification(*held_refresh);
  }
  if (held_module_html) {
    worker->DispatchNotification(*held_module_html);
  }

  worker->in_flight_work_.fetch_sub(1, std::memory_order_relaxed);
  if (worker->shutting_down_) {
    worker->CheckShutdownComplete();
  }
}

// =================================================================
// Notification handling
// =================================================================

// Mark a variant as processed in the in-memory dedup set.
// Called after successful variant writes.  Does NOT mark variants
// written with kFlagNeedsRevalidation (they must be re-processed).
void Worker::MarkVariantProcessed(const std::string& url,
                                  const std::string& hostname,
                                  std::string_view scheme,
                                  std::string_view site, AlternateId id,
                                  const PurgeDispatchGen& purge_gen) {
  // Check if a purge occurred after this notification was dispatched.
  // If so, skip insertion to avoid re-adding stale dedup entries.
  // Always check (even for purge_gen==0) to catch first-ever purges.
  if (WasPurgedSinceDispatch(url, hostname, purge_gen, scheme, site)) {
    return;
  }

  std::string key = ComposeInternalKeyWithId(url, hostname, scheme, site, id);
  std::lock_guard<std::mutex> lock(processed_set_mutex_);
  // A new verdict for this key starts without a copy record: the caller
  // adds one (MarkCopyWritten) only when it wrote the copy this time.
  copy_written_current_.erase(key);
  copy_written_previous_.erase(key);
  processed_current_.insert(std::move(key));
  // Generational overflow: rotate current → previous instead of clearing
  // everything.  Recently-added entries survive in the new current set.
  // The copy records rotate in the SAME step, so a key is never a
  // processed-set hit whose record has already been dropped: the two sets
  // age together and a record lives exactly as long as its mark.
  if (processed_current_.size() > config_.max_processed_entries) {
    processed_previous_ = std::move(processed_current_);
    processed_current_.clear();
    copy_written_previous_ = std::move(copy_written_current_);
    copy_written_current_.clear();
  }
}

void Worker::MarkCopyWritten(const std::string& url,
                             const std::string& hostname,
                             std::string_view scheme, std::string_view site,
                             AlternateId id,
                             const PurgeDispatchGen& purge_gen) {
  // The same test MarkVariantProcessed applies: a URL purged after this
  // notification was dispatched keeps no record of a copy the purge removed.
  if (WasPurgedSinceDispatch(url, hostname, purge_gen, scheme, site)) {
    return;
  }
  std::string key = ComposeInternalKeyWithId(url, hostname, scheme, site, id);
  std::lock_guard<std::mutex> lock(processed_set_mutex_);
  // The record goes into the generation that holds the key's mark, and is
  // not written at all when there is no mark (MarkVariantProcessed refused
  // it, or a purge of the URL erased it in between).  A record therefore
  // never exists without its mark, and the record sets stay within the
  // processed sets' bound.  No rotation here: the records rotate with the
  // processed set, in MarkVariantProcessed.
  if (processed_current_.contains(key)) {
    copy_written_current_.insert(std::move(key));
  } else if (processed_previous_.contains(key)) {
    copy_written_previous_.insert(std::move(key));
  }
}

void Worker::EraseUrlMarksLocked(const std::string& prefix) {
  const auto matches = [&prefix](const std::string& key) {
    return key.starts_with(prefix);
  };
  std::erase_if(processed_current_, matches);
  std::erase_if(processed_previous_, matches);
  std::erase_if(copy_written_current_, matches);
  std::erase_if(copy_written_previous_, matches);
}

void Worker::SetWriteFailureCooldown(const std::string& url,
                                     const std::string& hostname,
                                     std::string_view scheme,
                                     std::string_view site) {
  std::string ck = ComposeInternalKey(url, hostname, scheme, site);
  std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
  auto now = std::chrono::steady_clock::now();
  text_cooldown_expiry_[ck] =
      CooldownEntry{now, now + std::chrono::seconds(kWriteFailureCooldownSecs),
                    CooldownReason::kWriteFailure};
  // Prune when map grows too large: remove expired, then evict
  // soonest-expiring if still over limit.
  if (text_cooldown_expiry_.size() > kMaxCooldownEntries) {
    for (auto it = text_cooldown_expiry_.begin();
         it != text_cooldown_expiry_.end();) {
      if (now >= it->second.expiry) {
        it = text_cooldown_expiry_.erase(it);
      } else {
        ++it;
      }
    }
    while (text_cooldown_expiry_.size() > kMaxCooldownEntries) {
      auto earliest = text_cooldown_expiry_.begin();
      for (auto it = text_cooldown_expiry_.begin();
           it != text_cooldown_expiry_.end(); ++it) {
        if (it->second.expiry < earliest->second.expiry) earliest = it;
      }
      text_cooldown_expiry_.erase(earliest);
    }
  }
}

void Worker::RegisterIntegrityPinnedUrls(
    const HtmlScanResult& scan_result, const CacheNotification& notification) {
  const std::vector<std::string>& raw_urls = scan_result.integrity_pinned_urls;
  if (raw_urls.empty()) return;
  const DocumentBase document_base =
      DocumentBaseOf(notification.url, scan_result, notification.hostname,
                     notification.scheme);
  for (const auto& raw : raw_urls) {
    if (raw.empty()) continue;
    // Same resolution as the critical-CSS stylesheet lookup: resolve
    // against the document base (the page, or its <base href>), normalize,
    // and key by the subresource's own hostname (falling back to the page
    // hostname for relative URLs).
    std::string resolved = ResolveAgainstBase(document_base, raw);
    // Protocol-relative references ("//host/path") inherit the page's
    // scheme (ResolveAgainstBase gives them the document's; this is the
    // fallback for a page URL without one); without it
    // NormalizeCacheUrl/UrlHostname can't split hostname from path and the
    // pin key would never match.
    if (resolved.starts_with("//")) {
      resolved = absl::StrCat(notification.scheme, ":", resolved);
    }
    std::string norm_url = NormalizeCacheUrl(resolved, url_norm_config_);
    std::string_view res_host = UrlHostname(resolved);
    std::string norm_host =
        res_host.empty() ? notification.hostname
                         : NormalizeCacheHostname(res_host, url_norm_config_);
    std::string key = ComposeInternalKey(
        norm_url, norm_host, notification.scheme, notification.site);
    bool inserted = false;
    {
      std::lock_guard<std::mutex> lock(integrity_pinned_mutex_);
      if (integrity_pinned_urls_.size() >= kMaxIntegrityPinnedEntries &&
          !integrity_pinned_urls_.contains(key)) {
        continue;  // Bounded; new pins beyond the cap are dropped.
      }
      inserted = integrity_pinned_urls_.insert(key).second;
    }
    if (inserted) {
      // A minified variant may already exist (the subresource notification
      // can be processed before the first HTML scan registers the pin).
      // Drop the whole cache key so nginx re-caches the original on the
      // next request, and clear dedup so that path isn't blocked.
      if (cache_) {
        (void)cache_->Remove(norm_url, norm_host, notification.scheme,
                             notification.site);
        // Remove changed this subresource's alternate set; refresh its cached
        // count so /v1/cache/urls does not report a stale-high value.  The
        // HandleNotification count guard only refreshes notification.url, not
        // this resolved subresource key, and a now-pinned subresource may not
        // re-notify promptly.  No-op if the subresource is not tracked.
        RefreshAlternateCount(norm_url, norm_host, notification.scheme,
                              notification.site);
      }
      ClearDedupAndCooldown(norm_url, norm_host, notification.scheme,
                            notification.site);
      LogInfo(
          "SRI: excluding %s from text optimization (referenced with "
          "integrity attribute)",
          norm_url.c_str());
    }
  }
}

bool Worker::IsIntegrityPinned(const CacheNotification& notification) const {
  // notification.url/hostname were normalized at dispatch time.
  std::string key = ComposeInternalKey(notification.url, notification.hostname,
                                       notification.scheme, notification.site);
  std::lock_guard<std::mutex> lock(integrity_pinned_mutex_);
  return integrity_pinned_urls_.contains(key);
}

bool Worker::WasPurgedSinceDispatch(std::string_view url,
                                    std::string_view hostname,
                                    const PurgeDispatchGen& dispatch_gen,
                                    std::string_view scheme,
                                    std::string_view site) const {
  // Normalize for consistency with InvalidateUrl (which also normalizes).
  std::string norm_url = NormalizeCacheUrl(url, url_norm_config_);
  std::string norm_host = NormalizeCacheHostname(hostname, url_norm_config_);
  std::string gen_key = ComposeInternalKey(norm_url, norm_host, scheme, site);
  std::lock_guard<std::mutex> lock(purge_gen_mutex_);
  uint64_t current_gen = 0;
  auto it = purge_generation_.find(gen_key);
  if (it != purge_generation_.end()) {
    current_gen = it->second;
  } else {
    auto pit = purge_generation_previous_.find(gen_key);
    if (pit != purge_generation_previous_.end()) {
      current_gen = pit->second;
    }
  }
  return current_gen > dispatch_gen.url_gen ||
         purge_all_generation_ > dispatch_gen.all_gen;
}

PurgeDispatchGen Worker::CapturePurgeGen(std::string_view url,
                                         std::string_view hostname,
                                         std::string_view scheme,
                                         std::string_view site) const {
  // Normalize for consistency with InvalidateUrl (idempotent when the
  // caller already normalized).
  std::string norm_url = NormalizeCacheUrl(url, url_norm_config_);
  std::string norm_host = NormalizeCacheHostname(hostname, url_norm_config_);
  std::string gen_key = ComposeInternalKey(norm_url, norm_host, scheme, site);
  PurgeDispatchGen gen;
  std::lock_guard<std::mutex> lock(purge_gen_mutex_);
  auto it = purge_generation_.find(gen_key);
  if (it != purge_generation_.end()) {
    gen.url_gen = it->second;
  } else {
    auto pit = purge_generation_previous_.find(gen_key);
    if (pit != purge_generation_previous_.end()) {
      gen.url_gen = pit->second;
    }
  }
  gen.all_gen = purge_all_generation_;
  return gen;
}

bool OriginalSlotCountsAsNoSavingsSkip(const MultiTranscodeResult& multi) {
  // A declined original is not a no-savings skip: the verify already
  // counted it under ssimulacra2_declines, and a skip label beside the
  // decline label double-counts one refusal (#1382).
  return !multi.optimized_original.success && !multi.ssimulacra2_declined;
}

std::vector<DeclineTombstoneRecord> ParseDeclineTombstone(
    std::string_view blob, const std::array<std::byte, 32>& source_hash) {
  // [1B version][1B search generation][32B hash][1B count]
  // [count x (1B slot, 1B quality)].
  // Malformed input, an unknown future version, a refusal recorded by a
  // different quality search, or a different source hash all read as "no
  // tombstone": the reader re-runs the ladder rather than guessing
  // (fail-open costs recompute, never correctness).
  constexpr size_t kHeaderSize = 1 + 1 + 32 + 1;
  if (blob.size() < kHeaderSize) return {};
  const auto* bytes = reinterpret_cast<const uint8_t*>(blob.data());
  if (bytes[0] != kDeclineTombstoneFormatVersion) return {};
  // A refusal is only as good as the search that produced it: a variant an
  // older search could not reach may be reachable now, so its refusals
  // must not keep suppressing the slot.
  if (bytes[1] != kVerifyQualitySearchVersion) return {};
  if (std::memcmp(bytes + 2, source_hash.data(), 32) != 0) return {};
  const size_t count = bytes[34];
  if (blob.size() != kHeaderSize + count * 2) return {};
  std::vector<DeclineTombstoneRecord> records;
  records.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    records.push_back(
        {bytes[kHeaderSize + i * 2], bytes[kHeaderSize + i * 2 + 1]});
  }
  return records;
}

std::string EncodeDeclineTombstone(
    const std::array<std::byte, 32>& source_hash,
    const std::vector<DeclineTombstoneRecord>& records) {
  std::string blob;
  blob.reserve(1 + 1 + 32 + 1 + records.size() * 2);
  blob.push_back(static_cast<char>(kDeclineTombstoneFormatVersion));
  blob.push_back(static_cast<char>(kVerifyQualitySearchVersion));
  blob.append(reinterpret_cast<const char*>(source_hash.data()), 32);
  // The slot space is 3 formats x 3 viewports x 2 densities x 2 save-data
  // = 36 records, far under the uint8 count ceiling; clamp defensively.
  blob.push_back(static_cast<char>(std::min<size_t>(records.size(), 255)));
  for (const DeclineTombstoneRecord& r : records) {
    blob.push_back(static_cast<char>(r.slot));
    blob.push_back(static_cast<char>(r.quality));
  }
  return blob;
}

Worker::ImageVariantResult Worker::WriteImageVariants(
    const CacheNotification& notification, CapabilityMask target_mask,
    std::string_view image_data, const AlternateMetadata& base_meta,
    const std::function<bool()>& purge_check,
    const std::shared_ptr<const WorkerConfig>& cfg,
    const std::array<std::byte, 32>& origin_hash) {
  ImageVariantResult result;
  result.smallest_raster_size = image_data.size();

  // Intra-notification content dedup: track (viewport, size, hash) of
  // written variant bytes to skip writing identical content under multiple
  // AlternateIds of the same viewport (e.g., Desktop ignores density -> 1x
  // and 2x identical).  The viewport is part of the key because the
  // selector ranks a viewport match above every other dimension: a
  // duplicate skipped at another viewport is never served in its place, so
  // an image narrower than the tablet width (whose tablet and desktop
  // encodes are byte-identical) would leave the desktop slot holding only
  // the front end's raw recording, served to every desktop client.
  struct WrittenVariant {
    CapabilityMask::Viewport viewport;
    size_t content_size;
    size_t content_hash;  // std::hash<std::string_view>
  };
  std::vector<WrittenVariant> written_variants;
  written_variants.reserve(36);

  // Generate missing format variants across enabled dimensions:
  // save-data, density, viewport, format.  In proactive mode all
  // combinations are produced; otherwise only the requested one.
  const CapabilityMask::ImageFormat all_formats[] = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };
  const CapabilityMask::Viewport all_viewports[] = {
      CapabilityMask::Viewport::kMobile,
      CapabilityMask::Viewport::kTablet,
      CapabilityMask::Viewport::kDesktop,
  };
  const CapabilityMask::SaveData all_savedata[] = {
      CapabilityMask::SaveData::kOff,
      CapabilityMask::SaveData::kOn,
  };
  const CapabilityMask::PixelDensity all_densities[] = {
      CapabilityMask::PixelDensity::k1x,
      CapabilityMask::PixelDensity::k2xPlus,
  };

  // Detect the source JPEG quality once: it is a property of the input bytes,
  // not of a variant.  A JPEG re-encode is capped at this quality (#1284);
  // -1 for non-JPEG input or when it cannot be determined.
  const int source_jpeg_quality =
      ImageTranscoder::DetectSourceJpegQuality(image_data, handler_);

  // Build the base variant metadata from the original image's metadata.
  AlternateMetadata img_variant_meta = base_meta;
  img_variant_meta.content_type = ContentType::kImage;
  img_variant_meta.flags = AlternateMetadata::kFlagWorkerProcessed |
                           InheritedVariantFlags(img_variant_meta);
  img_variant_meta.origin_content_length =
      static_cast<uint32_t>(std::min(image_data.size(), size_t{UINT32_MAX}));
  // origin_content_type inherited from base_meta

  // Helper lambda to write a single format result.
  auto write_format = [&](const TranscodeResult& tr,
                          CapabilityMask::ImageFormat fmt,
                          CapabilityMask::Viewport vp,
                          CapabilityMask::PixelDensity den,
                          CapabilityMask::SaveData sd, bool is_proactive) {
    if (!tr.success) return;
    ++result.expected_writes;
    CapabilityMask write_mask = target_mask;
    write_mask.set_image_format(fmt);
    write_mask.set_viewport(vp);
    write_mask.set_pixel_density(den);
    write_mask.set_save_data(sd);
    // Images are already format-compressed; strip encoding bits
    // to kIdentity so nginx does not set Content-Encoding on
    // the raw image bytes.
    write_mask.set_transfer_encoding(
        CapabilityMask::TransferEncoding::kIdentity);
    // Content dedup: skip if identical bytes already written for
    // this URL under a different AlternateId of the same viewport (e.g.,
    // Desktop ignores density so 1x and 2x produce identical output).
    size_t csz = tr.output_data.size();
    size_t chash = std::hash<std::string_view>{}(
        std::string_view(tr.output_data.data(), csz));
    bool is_dup = false;
    for (const auto& w : written_variants) {
      if (w.viewport == vp && w.content_size == csz &&
          w.content_hash == chash) {
        is_dup = true;
        break;
      }
    }
    if (is_dup) {
      ++result.successful_writes;  // Not a failure -- content exists
      LogInfo(
          "Image variant not written for %s (mask 0x%02X): identical bytes "
          "already written for this viewport",
          notification.url.c_str(),
          static_cast<int>(write_mask.Encode() & 0xFF));
      stats_.dedup_writes_skipped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    stats_.alternate_writes.fetch_add(1, std::memory_order_relaxed);
    bool wr =
        !(test_hook_fail_image_variant_write_ &&
          test_hook_fail_image_variant_write_()) &&
        WriteVariant(
            cache_.get(), notification.url, notification.hostname,
            notification.scheme, notification.site, write_mask,
            std::span<const char>(tr.output_data.data(), tr.output_data.size()),
            img_variant_meta, purge_check, this);
    if (wr) {
      written_variants.push_back({vp, csz, chash});
      ++result.successful_writes;
      stats_.variants_written.fetch_add(1, std::memory_order_relaxed);
      stats_.images_processed.fetch_add(1, std::memory_order_relaxed);
      if (is_proactive) {
        stats_.proactive_variants_written.fetch_add(1,
                                                    std::memory_order_relaxed);
      }
      if (tr.output_mime_type == "image/webp") {
        stats_.webp_generated.fetch_add(1, std::memory_order_relaxed);
      } else if (tr.output_mime_type == "image/avif") {
        stats_.avif_generated.fetch_add(1, std::memory_order_relaxed);
      } else if (tr.output_mime_type == "image/jpeg") {
        stats_.jpeg_optimized.fetch_add(1, std::memory_order_relaxed);
      } else if (tr.output_mime_type == "image/png") {
        stats_.png_optimized.fetch_add(1, std::memory_order_relaxed);
      }
      // Track smallest raster for SVG size gate.
      if (tr.output_data.size() < result.smallest_raster_size) {
        result.smallest_raster_size = tr.output_data.size();
      }
      LogInfo("Wrote %simage variant for %s (%s, %zu -> %zu bytes)",
              is_proactive ? "proactive " : "", notification.url.c_str(),
              tr.output_mime_type.c_str(), image_data.size(),
              tr.output_data.size());
    } else {
      // Check if the alternate was written by a concurrent
      // thread (dedup race).  If so, this is harmless.
      if (VariantExistsForMask(cache_.get(), notification.url,
                               notification.hostname, notification.scheme,
                               notification.site, write_mask)) {
        LogInfo(
            "Skipped %simage variant %s (mask 0x%02X, "
            "already written by concurrent thread)",
            is_proactive ? "proactive " : "", notification.url.c_str(),
            static_cast<int>(write_mask.Encode() & 0xFF));
      } else if (purge_check()) {
        // Issue E: benign purge-fence (purged after dispatch), not a hard
        // failure — count distinctly and do not touch errors.
        stats_.alternate_writes_fenced.fetch_add(1, std::memory_order_relaxed);
        LogInfo(
            "Fenced %simage variant %s (mask 0x%02X, purged after dispatch)",
            is_proactive ? "proactive " : "", notification.url.c_str(),
            static_cast<int>(write_mask.Encode() & 0xFF));
      } else {
        stats_.errors.fetch_add(1, std::memory_order_relaxed);
        stats_.alternate_write_failures.fetch_add(1, std::memory_order_relaxed);
        LogWarning(
            "Write failed for %simage variant %s (mask "
            "0x%02X, %s, %zu bytes)",
            is_proactive ? "proactive " : "", notification.url.c_str(),
            static_cast<int>(write_mask.Encode() & 0xFF),
            tr.output_mime_type.c_str(), tr.output_data.size());
      }
    }
  };

  auto requested_fmt = target_mask.image_format();
  auto requested_vp = target_mask.viewport();
  auto requested_den = target_mask.pixel_density();
  auto requested_sd = target_mask.save_data();
  bool any_missing = false;
  bool content_class_counted = false;

  // Fetch all existing alternate IDs in a single disk traversal
  // instead of calling AlternateExists() per format/viewport/etc.
  std::unordered_set<uint8_t> existing_ids;
  std::optional<std::vector<cyclone::AlternateInfo>> listed;
  {
    auto alts = cache_->ListAlternates(notification.url, notification.hostname,
                                       notification.scheme, notification.site);
    if (alts.has_value()) {
      for (const auto& alt : *alts) {
        existing_ids.insert(static_cast<uint8_t>(alt.id));
      }
      listed = std::move(*alts);
    }
  }
  // A raw identity slot is rebuilt as the optimized desktop original-format
  // variant once its bytes are kept as the durable original.
  bool identity_raw =
      IdentityHoldsRawOriginal(cache_.get(), notification.url,
                               notification.hostname, notification.scheme,
                               notification.site, existing_ids) &&
      PreserveIdentityAsDurableOriginal(cache_.get(), notification.url,
                                        notification.hostname,
                                        notification.scheme, notification.site,
                                        cfg->read_lease_duration_ms > 0);
  if (identity_raw) {
    existing_ids.erase(static_cast<uint8_t>(MaskToId(CapabilityMask())));
  }
  VariantSizeGates gates(listed ? &*listed : nullptr, identity_raw,
                         [this, &notification](const std::string& msg) {
                           LogInfo("%s for %s", msg.c_str(),
                                   notification.url.c_str());
                         });

  // Decline tombstone (#1382): slots this source's verify already refused.
  // Consulted once, before the loops; records are only honored when the
  // stored source hash matches (a changed source retries its slots), and a
  // hit skips the transcode for that slot -- the full attempt ladder for
  // one AVIF arm costs seconds, and a declined variant is never stored, so
  // without the tombstone every proactive notification re-pays it.
  // Gated on the verifier toggle: a tombstone stored while verification
  // was on must not keep suppressing its slot after verification is
  // turned off -- that slot would now fill, so the consult is skipped
  // and the ladder re-runs, which makes the tombstone self-invalidating
  // for this config axis. The stored quality-search generation is the
  // same idea along the other axis: a refusal recorded by a search with a
  // different reach is not evidence about this one, so it reads as no
  // tombstone and the slot is tried again.
  std::vector<DeclineTombstoneRecord> tombstoned =
      cfg->quality_verify
          ? LoadDeclineTombstone(notification, existing_ids, origin_hash)
          : std::vector<DeclineTombstoneRecord>{};
  std::vector<DeclineTombstoneRecord> new_tombstone_records;

  for (auto sd : all_savedata) {
    if (sd != requested_sd &&
        !(cfg->proactive_image_variants && cfg->proactive_savedata_variants)) {
      continue;
    }

    // Reset per save-data group: save-data ON uses different base
    // quality, so the learned quality from kOff doesn't apply.
    int learned_jpeg_quality = -1;

    for (auto den : all_densities) {
      if (den != requested_den &&
          !(cfg->proactive_image_variants && cfg->proactive_density_variants)) {
        continue;
      }

      for (auto vp : all_viewports) {
        if (vp != requested_vp && !(cfg->proactive_image_variants &&
                                    cfg->proactive_viewport_variants)) {
          continue;
        }

        // Build list of missing formats at this combination.
        auto missing_formats = FindMissingFormats(target_mask, vp, den, sd,
                                                  all_formats, existing_ids);

        // Tombstoned slots do not re-enter the ladder (#1382). Counted per
        // skipped slot so the suppression is observable next to the
        // declines it mirrors.
        if (!tombstoned.empty()) {
          std::erase_if(missing_formats, [&](auto fmt) {
            const uint8_t slot = VariantSlotId(fmt, vp, den, sd);
            for (const DeclineTombstoneRecord& rec : tombstoned) {
              if (rec.slot == slot) {
                stats_.ssimulacra2_decline_tombstone_hits.fetch_add(
                    1, std::memory_order_relaxed);
                LogInfo(
                    "SSIMULACRA2 decline tombstone hit for %s (slot 0x%02X): "
                    "variant already declined for this source, skipping "
                    "recompute",
                    notification.url.c_str(), static_cast<int>(slot));
                return true;
              }
            }
            return false;
          });
        }

        if (missing_formats.empty()) continue;
        any_missing = true;

        auto multi = image_transcoder_->TranscodeMultiResized(
            image_data, missing_formats, vp, den, sd,
            {.carried_hint = learned_jpeg_quality,
             .source_quality = source_jpeg_quality});
        if (multi.final_jpeg_quality > 0) {
          learned_jpeg_quality = multi.final_jpeg_quality;
        }

        // Count content class once per notification (skip kUnknown
        // from fast-path returns so a later iteration can count).
        if (!content_class_counted &&
            multi.applied_preset.content_class != ContentClass::kUnknown) {
          content_class_counted = true;
          switch (multi.applied_preset.content_class) {
            case ContentClass::kPhoto:
              stats_.content_photo.fetch_add(1, std::memory_order_relaxed);
              break;
            case ContentClass::kScreenshot:
              stats_.content_screenshot.fetch_add(1, std::memory_order_relaxed);
              break;
            case ContentClass::kIllustration:
              stats_.content_illustration.fetch_add(1,
                                                    std::memory_order_relaxed);
              break;
            case ContentClass::kNoisy:
              stats_.content_noisy.fetch_add(1, std::memory_order_relaxed);
              break;
            case ContentClass::kUnknown:
              break;
          }
        }

        // Track learned quality prediction stats.
        if (multi.used_learned_quality) {
          stats_.learned_quality_predictions.fetch_add(
              1, std::memory_order_relaxed);
        }
        if (multi.learned_quality_fallbacks > 0) {
          stats_.learned_quality_fallbacks.fetch_add(
              multi.learned_quality_fallbacks, std::memory_order_relaxed);
        }

        // Track SSIMULACRA2 quality verification stats (all formats).
        // A declined variant counts its own counter (like the size-gate
        // rejections), never errors, and does not drag the score average.
        auto count_ssim = [this](float score, bool reencoded, bool declined) {
          if (declined) {
            stats_.ssimulacra2_declines.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          if (score >= 0.0f) {
            stats_.ssimulacra2_checks.fetch_add(1, std::memory_order_relaxed);
            stats_.ssimulacra2_total_score_x100.fetch_add(
                static_cast<uint64_t>(score * 100), std::memory_order_relaxed);
            if (reencoded) {
              stats_.ssimulacra2_reencodes.fetch_add(1,
                                                     std::memory_order_relaxed);
            }
          }
        };
        count_ssim(multi.ssimulacra2_score, multi.ssimulacra2_reencoded,
                   multi.ssimulacra2_declined);
        count_ssim(multi.webp_ssimulacra2_score,
                   multi.webp_ssimulacra2_reencoded,
                   multi.webp_ssimulacra2_declined);
        count_ssim(multi.avif_ssimulacra2_score,
                   multi.avif_ssimulacra2_reencoded,
                   multi.avif_ssimulacra2_declined);

        // Tombstone each declined slot for THIS source (#1382), so the
        // next notification for the same slot stops at the consult above
        // instead of re-paying the attempt ladder. The quality is evidence
        // for whoever inspects the blob ("declined at quality X, source
        // hash H").
        auto record_decline = [&](CapabilityMask::ImageFormat fmt,
                                  bool declined, int quality) {
          if (!declined) return;
          DeclineTombstoneRecord rec;
          rec.slot = VariantSlotId(fmt, vp, den, sd);
          rec.quality = (quality > 0 && quality <= 100)
                            ? static_cast<uint8_t>(quality)
                            : 0xFF;
          new_tombstone_records.push_back(rec);
        };
        record_decline(CapabilityMask::ImageFormat::kWebP,
                       multi.webp_ssimulacra2_declined,
                       multi.final_webp_quality);
        record_decline(CapabilityMask::ImageFormat::kAvif,
                       multi.avif_ssimulacra2_declined,
                       multi.final_avif_quality);
        record_decline(CapabilityMask::ImageFormat::kOriginal,
                       multi.ssimulacra2_declined, multi.final_jpeg_quality);

        // Set content class metadata from transcode result.
        img_variant_meta.content_class =
            static_cast<uint8_t>(multi.applied_preset.content_class);

        // Helper to set per-format SSIMULACRA2 score in metadata.
        auto set_ssim_meta = [&](float score) {
          if (score >= 0.0f) {
            img_variant_meta.SetQualityScoreX100(
                static_cast<uint16_t>(std::min(score * 100.0f, 10000.0f)));
          } else {
            img_variant_meta.SetQualityScoreX100(std::nullopt);  // N/A
          }
        };

        // Size gates (VariantSizeGates): never store a variant larger than
        // the one its clients are served without it.
        gates.Apply(multi, vp, den, sd);
        RecordSizeGateStats(stats_, multi);

        // A variant is "not proactive" only when it matches
        // the exact requested combination.
        bool exact_match =
            (vp == requested_vp && den == requested_den && sd == requested_sd);
        set_ssim_meta(multi.webp_ssimulacra2_score);
        write_format(multi.webp, CapabilityMask::ImageFormat::kWebP, vp, den,
                     sd,
                     !(exact_match &&
                       requested_fmt == CapabilityMask::ImageFormat::kWebP));
        set_ssim_meta(multi.avif_ssimulacra2_score);
        write_format(multi.avif, CapabilityMask::ImageFormat::kAvif, vp, den,
                     sd,
                     !(exact_match &&
                       requested_fmt == CapabilityMask::ImageFormat::kAvif));
        set_ssim_meta(multi.ssimulacra2_score);
        write_format(
            multi.optimized_original, CapabilityMask::ImageFormat::kOriginal,
            vp, den, sd,
            !(exact_match &&
              requested_fmt == CapabilityMask::ImageFormat::kOriginal));

        // One line per requested slot this pass left unwritten, with the
        // reason, so a missing variant is never silent. A slot the content
        // dedup skipped logs its own line in write_format.
        for (auto fmt : missing_formats) {
          const TranscodeResult& tr = ResultForFormat(multi, fmt);
          if (tr.success) continue;
          LogInfo("Image variant not written for %s (slot 0x%02X): %s",
                  notification.url.c_str(),
                  static_cast<int>(VariantSlotId(fmt, vp, den, sd)),
                  tr.error_message.empty() ? "no result from the transcoder"
                                           : tr.error_message.c_str());
        }

        // Count converted-format slots the fall-through refused: the
        // optimizer answered with the origin's own bytes in the origin's own
        // format, so there was nothing to convert (#1374).  Counted here
        // rather than in write_format, which cannot see the difference -- it
        // returns early on any unsuccessful result.
        if (multi.unconverted_fallthrough > 0) {
          stats_.image_unconverted_fallthrough.fetch_add(
              multi.unconverted_fallthrough, std::memory_order_relaxed);
        }

        // Count original-format skips due to no size savings. A declined
        // original does not count (OriginalSlotCountsAsNoSavingsSkip): its
        // refusal already counted under ssimulacra2_declines (#1382).
        if (OriginalSlotCountsAsNoSavingsSkip(multi) &&
            std::find(missing_formats.begin(), missing_formats.end(),
                      CapabilityMask::ImageFormat::kOriginal) !=
                missing_formats.end()) {
          stats_.image_no_savings_skipped.fetch_add(1,
                                                    std::memory_order_relaxed);
          stats_.image_already_optimal_count.fetch_add(
              1, std::memory_order_relaxed);
          stats_.image_already_optimal_bytes.fetch_add(
              image_data.size(), std::memory_order_relaxed);
        }
      }
    }
  }

  if (!any_missing) {
    LogInfo("All siblings exist for %s, skipping", notification.url.c_str());
  }

  // Persist the tombstone once per notification, after the loops: records
  // for every declined slot across every (vp, den, sd) combination land in
  // the same per-URL blob (#1382).
  if (!new_tombstone_records.empty()) {
    StoreDeclineTombstone(notification, origin_hash, tombstoned,
                          std::move(new_tombstone_records));
  }

  return result;
}

std::vector<DeclineTombstoneRecord> Worker::LoadDeclineTombstone(
    const CacheNotification& notification,
    const std::unordered_set<uint8_t>& existing_ids,
    const std::array<std::byte, 32>& origin_hash) {
  if (existing_ids.count(
          static_cast<AlternateId>(SentinelId::kDeclineTombstone)) == 0) {
    return {};
  }
  auto stored = cache_->ReadAlternate(
      notification.url, notification.hostname, notification.scheme,
      static_cast<AlternateId>(SentinelId::kDeclineTombstone),
      notification.site);
  if (!stored.has_value()) return {};
  auto content = stored->content();
  return ParseDeclineTombstone(
      std::string_view(reinterpret_cast<const char*>(content.data()),
                       content.size()),
      origin_hash);
}

void Worker::StoreDeclineTombstone(
    const CacheNotification& notification,
    const std::array<std::byte, 32>& origin_hash,
    const std::vector<DeclineTombstoneRecord>& kept,
    std::vector<DeclineTombstoneRecord> added) {
  // Merge: keep this source's earlier records (they are still missing --
  // a declined variant is never stored), add the new ones, and let a
  // re-declined slot's newer quality win. Records for a DIFFERENT source
  // never reach here (LoadDeclineTombstone filtered them by hash) and the
  // source-change path purges the whole key, tombstone included.
  std::vector<DeclineTombstoneRecord> records = kept;
  for (DeclineTombstoneRecord& rec : added) {
    auto it = std::find_if(
        records.begin(), records.end(),
        [&](const DeclineTombstoneRecord& r) { return r.slot == rec.slot; });
    if (it != records.end()) {
      *it = rec;
    } else {
      records.push_back(rec);
    }
  }

  std::string blob = EncodeDeclineTombstone(origin_hash, records);
  auto wh = cache_->WriteSentinel(
      notification.url, notification.hostname, notification.scheme,
      SentinelId::kDeclineTombstone, blob.size(), notification.site);
  if (!wh.has_value()) {
    // A failed tombstone write costs a re-run of the ladder on the next
    // notification -- bounded, and the correct direction to fail in.
    LogWarning("Decline tombstone write failed for %s",
               notification.url.c_str());
    return;
  }
  (void)wh->write_sync(std::as_bytes(std::span(blob.data(), blob.size())));
  (void)wh->close_sync();
  // Write-around RAM tier (issue #1126, same reason as kContentHash): the
  // consult reads this sentinel, so a stale RAM copy must not outlive the
  // overwrite.
  cache_->EvictAlternateFromRamCache(
      notification.url, notification.hostname, notification.scheme,
      static_cast<AlternateId>(SentinelId::kDeclineTombstone),
      notification.site);
}

void Worker::WriteTextVariant(
    const CacheNotification& notification, ReadResult& read_result,
    std::string_view original_input, std::string_view minified,
    ContentType content_type, const char* type_name,
    std::atomic<uint64_t>& processed_stat,
    std::atomic<uint64_t>& already_optimal_count,
    std::atomic<uint64_t>& already_optimal_bytes,
    const std::function<bool()>& purge_check, const PurgeDispatchGen& purge_gen,
    const std::shared_ptr<const WorkerConfig>& cfg, bool record_source_hash) {
  // Only a front end's own recording may seed the change oracle, and not for
  // a sheet that embeds other files (see the CSS caller).
  const bool source_genuine =
      record_source_hash && (read_result.metadata.flags &
                             AlternateMetadata::kFlagWorkerProcessed) == 0;
  // Issue #434: SHA-256 of the raw origin input, computed once here over the
  // owned, de-aliased copy the callers pass in, and reused below for both
  // the variant stamps and the change oracle: RecordSourceHash takes it
  // rather than re-hashing.  Zero (unbound) when the input is not genuine;
  // a worker-processed input then keeps the inherited stamp.
  const std::array<std::byte, 32> source_hash =
      source_genuine
          ? cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(original_input.data()),
                original_input.size()))
          : std::array<std::byte, 32>{};
  if (minified.size() < original_input.size()) {
    CapabilityMask mask = NormalizeMaskForDedup(notification.capability_mask);
    AlternateMetadata variant_meta = CreateWorkerMetadata(
        read_result.metadata, content_type,
        AlternateMetadata::kFlagWorkerProcessed, original_input.size());
    if (source_genuine) {
      StampSourceBinding(&variant_meta, source_hash);
    }

    // Release the read handle — minified data owns the output and
    // metadata has been copied.  See HTML path comment.
    read_result.handle = cyclone::ReadHandle{};

    stats_.alternate_writes.fetch_add(1, std::memory_order_relaxed);
    bool write_ok =
        WriteVariant(cache_.get(), notification.url, notification.hostname,
                     notification.scheme, notification.site, mask,
                     std::span<const char>(minified.data(), minified.size()),
                     variant_meta, purge_check, this);

    if (write_ok) {
      // SRI (issue #656): close the register-vs-write race.  The pin may
      // have been registered (insert, then cache Remove) between the
      // handler's IsIntegrityPinned check and this write — in that
      // interleaving the registration's Remove can precede the write and
      // the minified variant would persist.  Registration inserts the pin
      // before removing, so re-checking here after the write catches it.
      if (IsIntegrityPinned(notification)) {
        (void)cache_->Remove(notification.url, notification.hostname,
                             notification.scheme, notification.site);
        LogInfo(
            "SRI: dropped just-written %s variant for %s "
            "(pinned concurrently)",
            type_name, notification.url.c_str());
        return;
      }
      // Populate dedup set before incrementing stats so that any
      // observer polling the processed counter sees the entry present.
      MarkVariantProcessed(notification.url, notification.hostname,
                           notification.scheme, notification.site,
                           MaskToId(mask), purge_gen);
      // The optimized copy is in the cache: remember that, so a later
      // notification notices if it has gone missing.
      MarkCopyWritten(notification.url, notification.hostname,
                      notification.scheme, notification.site, MaskToId(mask),
                      purge_gen);
      stats_.variants_written.fetch_add(1, std::memory_order_relaxed);
      processed_stat.fetch_add(1, std::memory_order_relaxed);
      LogInfo(
          "Wrote minified %s variant for %s "
          "(%zu -> %zu bytes)",
          type_name, notification.url.c_str(), original_input.size(),
          minified.size());
      WriteCompressedVariants(cache_.get(), notification.url,
                              notification.hostname, notification.scheme,
                              notification.site, mask, minified, original_input,
                              variant_meta, cfg->gzip_level, cfg->brotli_level,
                              stats_, purge_check, this);
      RecordSourceHash(cache_.get(), notification.url, notification.hostname,
                       notification.scheme, notification.site, mask,
                       source_hash, source_genuine, cfg->gzip_level,
                       cfg->brotli_level, purge_check);
      // Clear tentative cooldown on success (matches HTML path).
      {
        std::string ck =
            ComposeInternalKey(notification.url, notification.hostname,
                               notification.scheme, notification.site);
        std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
        text_cooldown_expiry_.erase(ck);
      }
    } else {
      stats_.errors.fetch_add(1, std::memory_order_relaxed);
      stats_.alternate_write_failures.fetch_add(1, std::memory_order_relaxed);
      LogWarning("Failed to write %s variant for %s", type_name,
                 notification.url.c_str());
      SetWriteFailureCooldown(notification.url, notification.hostname,
                              notification.scheme, notification.site);
    }
  } else {
    // Already minimal — still write compressed variants of the
    // original content (gzip/brotli still reduces transfer size).
    // Counted before the de-alias early return below: the verdict is about
    // the optimization outcome, not the write's success.
    already_optimal_count.fetch_add(1, std::memory_order_relaxed);
    already_optimal_bytes.fetch_add(original_input.size(),
                                    std::memory_order_relaxed);
    CapabilityMask mask = NormalizeMaskForDedup(notification.capability_mask);
    AlternateMetadata orig_meta = CreateWorkerMetadata(
        read_result.metadata, content_type,
        AlternateMetadata::kFlagWorkerProcessed, original_input.size());
    // Same source binding as the minified branch: these compressed copies
    // are built from the raw input too (issue #434).
    if (source_genuine) {
      StampSourceBinding(&orig_meta, source_hash);
    }
    // De-alias the origin borrow (issue #934): own the bytes and drop the
    // read handle before compressing.  gzip+brotli over the live borrow pin
    // this stripe's read lease across the compress+write, so at cache-full
    // the worker's own compressed-variant writes into the same stripe are
    // deferred by its own lease (self-starvation).  Matches the minified and
    // image-transcode branches, which already copy-and-drop.
    const std::string owned_input(original_input);
    // Verify the copy is untorn before releasing (issue #934).
    // Belt-and-suspenders: the CSS/JS callers already de-alias + release the
    // borrow before minify, so read_result is typically already released
    // here (a no-op verify); this keeps WriteTextVariant self-protecting if a
    // future caller passes a live borrow.  On a wrap-torn copy, skip the
    // compressed-variant write rather than persist torn bytes.
    if (!BorrowCopyUntorn(read_result, cfg->read_lease_duration_ms > 0)) {
      stats_.read_borrow_wrap_discards.fetch_add(1, std::memory_order_relaxed);
      LogWarning("Origin %s borrow wrapped during de-alias copy, skipping %s",
                 type_name, notification.url.c_str());
      return;
    }
    read_result.handle = cyclone::ReadHandle{};
    // The identity entry stays the front end's own bytes; give it the same
    // source binding as the compressed copies (issue #507).
    if (source_genuine) {
      const StampResult stamped = StampRawEntry(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site, read_result.metadata,
          owned_input, source_hash, purge_check, this);
      CountStamp(stats_, stamped);
      if (stamped == StampResult::kFailed) {
        LogWarning("Failed to stamp unchanged %s for %s", type_name,
                   notification.url.c_str());
      }
    }
    MarkVariantProcessed(notification.url, notification.hostname,
                         notification.scheme, notification.site, MaskToId(mask),
                         purge_gen);
    WriteCompressedVariants(cache_.get(), notification.url,
                            notification.hostname, notification.scheme,
                            notification.site, mask, owned_input, owned_input,
                            orig_meta, cfg->gzip_level, cfg->brotli_level,
                            stats_, purge_check, this);
    RecordSourceHash(cache_.get(), notification.url, notification.hostname,
                     notification.scheme, notification.site, mask, source_hash,
                     source_genuine, cfg->gzip_level, cfg->brotli_level,
                     purge_check);
    // Clear tentative cooldown on success (matches HTML path).
    {
      std::string ck =
          ComposeInternalKey(notification.url, notification.hostname,
                             notification.scheme, notification.site);
      std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
      text_cooldown_expiry_.erase(ck);
    }
    LogInfo("%s already minimal for %s (%zu bytes)", type_name,
            notification.url.c_str(), owned_input.size());
  }
}

void Worker::RefreshAlternateCount(const std::string& url,
                                   const std::string& hostname,
                                   const std::string& scheme,
                                   std::string_view site) {
  if (url_registry_ == nullptr || cache_ == nullptr) return;
  auto alts = cache_->ListAlternates(url, hostname, scheme, site);
  url_registry_->SetAlternateCount(
      url, hostname, scheme,
      alts ? static_cast<uint32_t>(CountUniqueAlternates(*alts)) : 0, site);
}

// static
bool Worker::BorrowCopyUntorn(ReadResult& rr, bool leases_enabled) {
  // Released handle: the borrow was already copied + verified + released
  // upstream (the CSS/JS paths de-alias before WriteTextVariant runs), so
  // nothing aliases the mmap anymore and the caller's owned bytes are safe.
  if (!rr.is_valid()) return true;
  // A RAM-cache hit is served from a handle-owned buffer — no mmap alias
  // exists, so a write-buffer wrap cannot tear it; renew_lease() would
  // return false (nothing to renew), which must not be mistaken for a torn
  // read.  NOTE: is_ram_cache_hit() — NOT content_file_offset() — is the
  // aliasing test: the offset also reports kNoFileOffset for empty-content
  // docs and when Cyclone's whole-file map fell back to per-region
  // mappings (a sendfile-eligibility signal), and in that fallback the
  // borrow is still a tear-able MAP_SHARED alias that MUST be verified
  // (renew_lease() works there: the stripe is stamped on every disk-hit
  // path regardless of mapping mode or content size).
  if (rr.handle.is_ram_cache_hit()) return true;
  // Leases disabled (read_lease_duration == 0, the operator escape
  // hatch): no wrap deferral is configured and renew_lease() always
  // returns false — legacy semantics, proceed with the copy.  Gating on
  // the caller's RCU config snapshot is safe ONLY because
  // read_lease_duration_ms is IsNonReloadable (config_file.cc): it cannot
  // diverge from the lease mode the cache was constructed with.
  if (!leases_enabled) return true;
  // Epoch-only proof: true iff the stripe never wrapped during the copy
  // (an empty-content disk borrow takes this path too — a wrap then yields
  // a spurious-but-fail-safe miss on a zero-byte doc).  The single renew
  // coincides with the read-time stamp, so it does not extend the lease
  // beyond that stamp (no write starvation; cf. issue #934, which was
  // caused by renewing REPEATEDLY across the write phase).
  return rr.renew_lease();
}

// Assembles the stylesheet bytes a page is optimized and judged against.
//
// Extracted from HandleNotification unchanged: the byte sequence this produces
// is the input to CombinedCssValidationHash, so any drift here silently
// invalidates every stored validation record. The contract is written out next
// to that function in src/browser/optimization_profile.h; read it before
// touching this.
bool Worker::SubresourceAdmitted(const CacheNotification& notification,
                                 std::string_view read_url,
                                 std::string_view read_host, bool may_fetch,
                                 const AlternateMetadata* entry,
                                 bool shared_copy) {
  if (subresource_gate_ == nullptr) return false;
  SubresourceRead read;
  std::vector<pagespeed::PinnedUpstream> private_origins;
  if (shared_copy) {
    const auto cfg = live_config_.load();
    private_origins = pagespeed::ParseOperatorOrigins(
                          cfg->browser_analysis.agent_render_private_origins)
                          .value_or(std::vector<pagespeed::PinnedUpstream>{});
    read.shared_copy = true;
    read.private_address_origins = &private_origins;
  }
  read.site = notification.site;
  read.scheme = notification.scheme;
  read.host = read_host;
  read.url = read_url;
  read.page_host = notification.hostname;
  read.host_fetch_allowed =
      MayRequestHostFetches(notification) && notification.host_vouched;
  // Every entry handed in here was read under the notification's site.
  if (entry != nullptr) {
    read.entry_read = true;
    read.entry_flags = entry->flags;
    read.entry_cc_flags = entry->origin_cc_flags;
    read.entry_site = notification.site;
  }
  const SubresourceVerdict verdict = subresource_gate_->Check(read, may_fetch);
  if (verdict == SubresourceVerdict::kAdmitted) return true;
  LogInfo("Sub-resource %.*s not used for %s: %s",
          static_cast<int>(read_url.size()), read_url.data(),
          notification.url.c_str(),
          verdict == SubresourceVerdict::kRefused
              ? "not served to anonymous clients"
              : "not confirmed as served to anonymous clients");
  return false;
}

std::optional<ReadResult> Worker::ReadAdmittedSubresource(
    const CacheNotification& notification, std::string_view read_url,
    std::string_view read_host, bool may_fetch, bool* refused) {
  if (refused != nullptr) *refused = false;
  auto read = [&] {
    return cache_->ReadBestAlternate(
        read_url, read_host, notification.scheme, CapabilityMask(),
        /*agent_request_entitled=*/false, notification.site);
  };
  // Under a host the worker may ask, every entry is asked about as before.
  if (!(MayRequestHostFetches(notification) && notification.host_vouched)) {
    auto entry = read();
    // Recorded from an anonymous request: admitted on the entry itself, with
    // no request, so the entry is used as read.
    if (entry.has_value() &&
        (entry->metadata.flags & AlternateMetadata::kFlagAnonymousRecorded) !=
            0 &&
        SubresourceAdmitted(notification, read_url, read_host, may_fetch,
                            &entry->metadata)) {
      return std::optional<ReadResult>(std::move(*entry));
    }
  }
  // Anything else is asked about first, as it was before the read: the read
  // above is not held across the request.
  if (!SubresourceAdmitted(notification, read_url, read_host, may_fetch)) {
    if (refused != nullptr) *refused = true;
    return std::nullopt;
  }
  auto entry = read();
  if (!entry.has_value()) return std::nullopt;
  // The entry read after the answer may be another than the one asked
  // about: its own recorded Cache-Control decides too.
  if (!CcFlagsAllowShared(entry->metadata.origin_cc_flags)) {
    if (refused != nullptr) *refused = true;
    return std::nullopt;
  }
  return std::optional<ReadResult>(std::move(*entry));
}

Worker::CombinedCssResult Worker::BuildCombinedCss(
    const HtmlScanResult& scan_result, const CacheNotification& notification,
    const WorkerConfig& cfg, bool count_lookups, bool gate_may_fetch) {
  CombinedCssResult out;
  auto count_lookup = [this, count_lookups] {
    if (count_lookups) {
      stats_.selector_invocations.fetch_add(1, std::memory_order_relaxed);
    }
  };
  // Gather CSS: start with inline CSS from <style> tags.
  std::string& combined_css = out.css;
  combined_css = scan_result.inline_css;
  // What every reference in the page resolves against: the page URL, or its
  // <base href> when it has one (DocumentBaseOf, the rule the scanner's sheet
  // identities and the analysis-side CSS inliner share). A
  // relative @import inside an inline <style> resolves against its URL.
  const DocumentBase document_base =
      DocumentBaseOf(notification.url, scan_result, notification.hostname,
                     notification.scheme);
  const std::string& inline_base_url = document_base.url;
  static constexpr size_t kMaxCombinedCssBytes =
      static_cast<size_t>(10 * 1024 * 1024);

  // Shared lookup for @import flattening — resolves import URLs from
  // cache.  Uses UrlHostname on the resolved import URL to handle
  // cross-origin @imports correctly (e.g. CDN-hosted CSS), falling
  // back to the page hostname for path-only URLs.
  css::CssLookupFn import_lookup;
  if (!cfg.disable_css_import_flattening) {
    import_lookup = [&](std::string_view url) -> std::optional<std::string> {
      count_lookup();
      std::string norm_url = NormalizeCacheUrl(url, url_norm_config_);
      std::string_view host = UrlHostname(url);
      if (host.empty()) host = notification.hostname;
      auto result =
          ReadAdmittedSubresource(notification, norm_url, host, gate_may_fetch);
      if (!result.has_value()) return std::nullopt;
      auto span = result->content();
      return std::string(reinterpret_cast<const char*>(span.data()),
                         span.size());
    };

    // Flatten @imports in inline CSS against the document's base URL: the page
    // URL, or its <base href> when it has one.
    if (!combined_css.empty()) {
      auto flat =
          css::FlattenImports(combined_css, inline_base_url, import_lookup);
      // imports_resolved > 0 IS the all-or-nothing use-original
      // path: any skip yields 0.  Do not "simplify" this gate away.
      if (flat.imports_resolved > 0) {
        combined_css = std::move(flat.css);
      }
    }
  }

  // Try to gather external stylesheet CSS from cache.
  // Track whether any external stylesheets are missing (not yet
  // cached).  If so, the HTML variant is marked for revalidation
  // so nginx re-notifies the worker once CSS becomes available.
  bool& external_css_missing = out.external_css_missing;
  // Only same-origin (proxied) stylesheets can ever enter the worker's
  // cache, so only their absence can self-heal on revalidation. A
  // cross-origin sheet (e.g. a Google Fonts <link>) never caches here, so
  // marking the page for revalidation on its account would re-process the
  // HTML on every request forever without ever resolving. external_css_missing
  // still drives the async-CSS fail-safe (any unmeasured sheet keeps the
  // stylesheet render-blocking); this narrower flag drives only the
  // self-heal re-notify.
  bool& revalidatable_css_missing = out.revalidatable_css_missing;
  // Each gathered sheet as read (before @import flattening) and its resolved
  // URL, by index in scan_result.stylesheets, for the cascade-layer order
  // below. A sheet the loop does not read stays nullopt.
  std::vector<std::optional<GatheredSheet>> gathered_sheets(
      scan_result.stylesheets.size());
  size_t next_sheet = 0;
  for (const auto& stylesheet : scan_result.stylesheets) {
    const size_t sheet_index = next_sheet++;
    if (stylesheet.href.empty()) continue;
    // The sheet's rules join the combined sheet under its media, so a print
    // sheet's rules stay print rules and never reach a screen block as
    // unconditional ones. A media value that cannot be written safely as an
    // @media prelude leaves the sheet out of the combined sheet and counts it
    // as missing, so it stays render-blocking.
    const std::optional<std::string_view> media_scope =
        css::CombinedSheetMediaScope(CombinedSheetMediaOf(stylesheet));
    if (!media_scope.has_value()) {
      external_css_missing = true;
      continue;
    }
    // Resolved against the document base, so under <base href="/sub/"> a
    // relative href is the sheet in /sub/, and under a cross-host <base> a
    // root-relative href is the base host's sheet, as the browser fetches it.
    // A hostless result is the page's own path.
    std::string resolved_href =
        ResolveAgainstBase(document_base, stylesheet.href);
    // Normalize for cache key consistency.
    std::string norm_href = NormalizeCacheUrl(resolved_href, url_norm_config_);
    count_lookup();
    // Use the CSS URL's hostname for lookup, falling back to the page
    // hostname for relative URLs.  Nginx stores CSS keyed by the Host
    // header of the CSS request, which is the CSS URL's hostname.
    std::string_view css_host = UrlHostname(resolved_href);
    // "Revalidatable" = the sheet can plausibly enter the worker's cache, so
    // its absence self-heals on a re-notify: a relative (host-less) href, or
    // an absolute href whose host matches the page's served host
    // (notification.hostname, set by nginx). A cross-origin sheet (e.g. a
    // Google Fonts <link>) never caches here. When the page host is unknown
    // (notification.hostname empty) we default to revalidatable — the
    // self-heal direction; nginx always sets it in production, so
    // cross-origin churn is still avoided there.
    const bool css_revalidatable = css_host.empty() ||
                                   notification.hostname.empty() ||
                                   css_host == notification.hostname;
    if (css_host.empty()) css_host = notification.hostname;
    // A sheet the server does not serve to any client is absent, and its
    // absence does not heal on a re-notify: it is not revalidatable.
    bool css_refused = false;
    auto css_read = ReadAdmittedSubresource(notification, norm_href, css_host,
                                            gate_may_fetch, &css_refused);
    if (css_refused) {
      external_css_missing = true;
      continue;
    }
    if (css_read.has_value()) {
      auto css_span = css_read->content();
      if (!css_span.empty()) {
        // Cap combined CSS to prevent excessive memory use.
        if (combined_css.size() +
                css::MediaScopedCssSize(*media_scope, css_span.size()) >
            kMaxCombinedCssBytes) {
          LogWarning("Combined CSS exceeds %zu bytes, truncating for %s",
                     kMaxCombinedCssBytes, notification.url.c_str());
          // Remaining sheets are unmeasured — keep the stylesheet
          // render-blocking (fail-safe) rather than async-deferring a sheet
          // we did not gather. Matches the .NET ps_html_process size cap.
          external_css_missing = true;
          break;
        }
        gathered_sheets[sheet_index].emplace(GatheredSheet{
            std::string(reinterpret_cast<const char*>(css_span.data()),
                        css_span.size()),
            resolved_href});
        if (import_lookup) {
          // Flatten @imports before combining — each stylesheet uses its
          // own URL as base so relative @import URLs resolve correctly.
          std::string css_content(
              reinterpret_cast<const char*>(css_span.data()), css_span.size());
          auto flat =
              css::FlattenImports(css_content, resolved_href, import_lookup);
          // imports_resolved > 0 IS the all-or-nothing use-original
          // path: any skip yields 0 and falls to the raw append
          // below.  Do not "simplify" this gate away.
          if (flat.imports_resolved > 0) {
            // Re-check size after flattening expanded the content.
            if (combined_css.size() +
                    css::MediaScopedCssSize(*media_scope, flat.css.size()) >
                kMaxCombinedCssBytes) {
              if (handler_) {
                handler_->Warning(
                    "Flattened CSS exceeds %zu bytes, truncating for %s",
                    kMaxCombinedCssBytes, notification.url.c_str());
              }
              // Remaining sheets are unmeasured — fail safe (see above).
              external_css_missing = true;
              break;
            }
            const size_t before = combined_css.size();
            if (!combined_css.empty()) combined_css.append("\n");
            if (!css::AppendMediaScopedCss(*media_scope, flat.css,
                                           &combined_css)) {
              // Cannot be wrapped in its media safely: counts as missing.
              combined_css.resize(before);
              external_css_missing = true;
              continue;
            }
            if (handler_) {
              handler_->Info(
                  "Gathered external CSS from %s (%zu bytes, %zu after "
                  "@import flattening)",
                  resolved_href.c_str(), css_span.size(), flat.css.size());
            }
            continue;
          }
        }
        // No @imports resolved (or flattening disabled) — append raw CSS.
        const size_t before = combined_css.size();
        if (!combined_css.empty()) {
          combined_css.append("\n");
        }
        if (!css::AppendMediaScopedCss(
                *media_scope,
                std::string_view(reinterpret_cast<const char*>(css_span.data()),
                                 css_span.size()),
                &combined_css)) {
          // Cannot be wrapped in its media safely: counts as missing.
          combined_css.resize(before);
          external_css_missing = true;
          continue;
        }
        LogInfo("Gathered external CSS from %s (%zu bytes)",
                resolved_href.c_str(), css_span.size());
      } else {
        // Cached and empty: known, and it declares no layer.
        gathered_sheets[sheet_index].emplace(GatheredSheet{{}, resolved_href});
      }
    } else {
      external_css_missing = true;
      if (css_revalidatable) revalidatable_css_missing = true;
    }
  }

  // The page's cascade-layer order over every stylesheet source in document
  // order (cascade_layer_order.h). Not part of the byte contract
  // above: it only decides where a layered critical block may go.
  std::vector<LayerOrderSource> layer_sources = BuildLayerOrderSources(
      scan_result.stylesheet_sources, inline_base_url, gathered_sheets);
  LayerOrderImportLookup layer_import_lookup =
      [&](std::string_view base_url,
          std::string_view href) -> std::optional<LayerOrderImport> {
    std::string resolved = ResolvePath(UrlDirectory(base_url), href);
    std::string norm_url = NormalizeCacheUrl(resolved, url_norm_config_);
    std::string_view host = UrlHostname(resolved);
    if (host.empty()) host = notification.hostname;
    auto result =
        ReadAdmittedSubresource(notification, norm_url, host, gate_may_fetch);
    if (!result.has_value()) return std::nullopt;
    auto span = result->content();
    return LayerOrderImport{
        std::string(reinterpret_cast<const char*>(span.data()), span.size()),
        std::move(resolved)};
  };
  out.layer_order =
      ComputeCascadeLayerOrder(layer_sources, layer_import_lookup);
  return out;
}

bool Worker::MarkdownModePassIsCurrent(
    const CacheNotification& notification,
    const std::array<std::byte, 32>& origin_hash) {
  const std::string key =
      ComposeInternalKey(notification.url, notification.hostname,
                         notification.scheme, notification.site);
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(markdown_pass_mutex_);
  auto it = markdown_pass_last_.find(key);
  return it != markdown_pass_last_.end() &&
         it->second.origin_hash == origin_hash &&
         now - it->second.at < std::chrono::seconds(kMarkdownPassRetrySecs);
}

void Worker::RecordMarkdownModePass(
    const CacheNotification& notification,
    const std::array<std::byte, 32>& origin_hash) {
  const std::string key =
      ComposeInternalKey(notification.url, notification.hostname,
                         notification.scheme, notification.site);
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(markdown_pass_mutex_);
  if (markdown_pass_last_.size() >= kMaxMarkdownPassEntries &&
      !markdown_pass_last_.contains(key)) {
    // Sweep only expired records.  When every record is live the pass is
    // simply not recorded: the next request processes again, which is the
    // behaviour without this bound, never a skip it cannot justify.
    std::erase_if(markdown_pass_last_, [now](const auto& kv) {
      return now - kv.second.at >= std::chrono::seconds(kMarkdownPassRetrySecs);
    });
    if (markdown_pass_last_.size() >= kMaxMarkdownPassEntries) return;
  }
  markdown_pass_last_[key] = MarkdownPass{origin_hash, now};
}

Worker::OriginRecheck Worker::RecheckRecordedOriginal(
    const CacheNotification& notification) {
  const auto cfg = live_config_.load();
  const AlternateId durable_id =
      static_cast<AlternateId>(SentinelId::kOriginalContent);
  // Metadata-only look first (RAM evicted, no byte copy, no hash): a
  // notification whose original was not re-recorded costs one small read.
  cache_->EvictAlternateFromRamCache(notification.url, notification.hostname,
                                     notification.scheme, durable_id,
                                     notification.site);
  AlternateMetadata recorded;
  {
    auto durable =
        cache_->ReadOriginalAlternate(notification.url, notification.hostname,
                                      notification.scheme, notification.site);
    if (!durable.has_value() || !durable->is_valid()) {
      // No recorded original: nothing to compare.
      return OriginRecheck::kUnchanged;
    }
    recorded = durable->metadata;
    durable->release();
  }
  if (recorded.cache_inserted_at == 0) return OriginRecheck::kUnchanged;
  // Covered content: images, CSS and JS, the types that record a content
  // hash of the bytes they were built from.  Anything else would have no way
  // to tell "same bytes" from "new bytes" and would rebuild on every
  // re-record.
  if (notification.content_type != ContentType::kImage &&
      notification.content_type != ContentType::kCss &&
      notification.content_type != ContentType::kJs) {
    return OriginRecheck::kUnchanged;
  }

  // What the variant set inherited: the newest stamp and, from the variant
  // that carries it, the validators.
  uint32_t newest = 0;
  AlternateMetadata built_from;
  {
    auto alts = cache_->ListAlternates(notification.url, notification.hostname,
                                       notification.scheme, notification.site);
    if (!alts.has_value()) return OriginRecheck::kUnchanged;
    for (const auto& alt : *alts) {
      const AlternateId pid = static_cast<uint8_t>(alt.id);
      if (IsSentinel(pid)) continue;
      auto entry =
          cache_->ReadAlternate(notification.url, notification.hostname,
                                notification.scheme, pid, notification.site);
      if (!entry.has_value() || !entry->is_valid()) continue;
      if (entry->metadata.cache_inserted_at >= newest) {
        newest = entry->metadata.cache_inserted_at;
        built_from = entry->metadata;
      }
      entry->release();
    }
  }
  if (newest == 0) return OriginRecheck::kUnchanged;
  const bool newer = recorded.cache_inserted_at > newest;
  const bool etag_differs = !recorded.origin_etag.empty() &&
                            !built_from.origin_etag.empty() &&
                            recorded.origin_etag != built_from.origin_etag;
  const bool lm_differs =
      recorded.origin_last_modified != 0 &&
      built_from.origin_last_modified != 0 &&
      recorded.origin_last_modified != built_from.origin_last_modified;
  const bool length_differs =
      recorded.origin_content_length != 0 &&
      built_from.origin_content_length != 0 &&
      recorded.origin_content_length != built_from.origin_content_length;
  if (!newer && !etag_differs && !lm_differs && !length_differs) {
    return OriginRecheck::kUnchanged;
  }

  // Rate limit, per URL.  Inside the window the hash is still compared when
  // a record exists (a genuine change is never deferred); only an unchanged
  // or unprovable verdict is dropped, so no copy is rewritten.
  bool limited = false;
  {
    const std::string key =
        ComposeInternalKey(notification.url, notification.hostname,
                           notification.scheme, notification.site);
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(dedup_heal_mutex_);
    auto it = origin_recheck_last_.find(key);
    if (it != origin_recheck_last_.end() &&
        now - it->second < std::chrono::seconds(kDedupHealMinIntervalSecs)) {
      limited = true;
    } else {
      if (origin_recheck_last_.size() >= kMaxDedupHealEntries) {
        std::erase_if(origin_recheck_last_, [now](const auto& kv) {
          return now - kv.second >=
                 std::chrono::seconds(kDedupHealMinIntervalSecs);
        });
      }
      if (origin_recheck_last_.size() < kMaxDedupHealEntries) {
        origin_recheck_last_[key] = now;
      }
    }
  }
  stats_.notifications_origin_rechecked.fetch_add(1, std::memory_order_relaxed);

  // The origin state moved: decide on the bytes.
  PristineOriginRead pristine =
      ReadPristineOrigin(cache_.get(), notification.url, notification.hostname,
                         notification.scheme, notification.site,
                         cfg->read_lease_duration_ms > 0, stats_, handler_);
  if (!pristine.found || pristine.id != durable_id) {
    return OriginRecheck::kUnchanged;
  }
  bool hash_matches = false;
  bool have_oracle = false;
  {
    auto stored = cache_->ReadAlternate(
        notification.url, notification.hostname, notification.scheme,
        static_cast<AlternateId>(SentinelId::kContentHash), notification.site);
    if (stored.has_value()) {
      auto sc = stored->content();
      have_oracle = sc.size() == 32;
      hash_matches =
          have_oracle && std::memcmp(sc.data(), pristine.hash.data(), 32) == 0;
    }
  }
  // A re-recorded response that forbids shared storage must not extend the
  // copies' life: purge instead of restamping (as HandleOriginRefreshed does).
  const bool may_restamp = CanRestampVariantSet(pristine.meta);
  if (limited && !(have_oracle && !hash_matches)) {
    // Inside the window and not provably changed: leave the copies alone.
    return OriginRecheck::kRestamped;
  }
  if (hash_matches && may_restamp) {
    PurgeDispatchGen gen =
        CapturePurgeGen(notification.url, notification.hostname,
                        notification.scheme, notification.site);
    PurgeCheck fence =
        [this, url = notification.url, hostname = notification.hostname,
         scheme = notification.scheme, site = notification.site, gen]() {
          return WasPurgedSinceDispatch(url, hostname, gen, scheme, site);
        };
    RestampVariantSetFreshness(cache_.get(), notification.url,
                               notification.hostname, notification.scheme,
                               notification.site, pristine.meta, pristine.id,
                               fence);
    stats_.origin_unchanged_restamped.fetch_add(1, std::memory_order_relaxed);
    return OriginRecheck::kRestamped;
  }

  // Changed (or no oracle to prove otherwise): drop everything derived from
  // the old bytes, keep the new original, and rebuild.  The bump and the
  // removal happen under the purge-generation lock so a concurrent writer
  // from the old set is fenced; the caller re-reads the generation after
  // this returns, or the rebuild would be fenced by this very bump.
  {
    std::string gen_key =
        ComposeInternalKey(notification.url, notification.hostname,
                           notification.scheme, notification.site);
    std::lock_guard<std::mutex> lock(purge_gen_mutex_);
    BumpPurgeGenerationLocked(gen_key);
    auto purged =
        cache_->RemoveDerivedAlternates(notification.url, notification.hostname,
                                        notification.scheme, notification.site);
    if (!purged.has_value()) {
      LogWarning(
          "Targeted purge for %s failed (%s): stale copies may remain until "
          "the next notification",
          notification.url.c_str(),
          make_error_code(purged.error()).message().c_str());
      return OriginRecheck::kUnchanged;
    }
  }
  ClearUrlProcessingState(notification.url, notification.hostname,
                          notification.scheme, notification.site);
  // Evict again: the rebuild's source read must observe the on-disk original.
  cache_->EvictAlternateFromRamCache(notification.url, notification.hostname,
                                     notification.scheme, durable_id,
                                     notification.site);
  stats_.origin_change_rebuilt.fetch_add(1, std::memory_order_relaxed);
  LogInfo("Recorded original for %s changed: derived copies purged, rebuilding",
          notification.url.c_str());
  return OriginRecheck::kPurgedRebuild;
}

namespace {

// The stored page's declared media type is HTML: "text/html", alone or with
// parameters.  The same test the library applies when it reads the entry for
// a serve (ps_html_stored_facts_from_reads), so the deriver never derives a
// copy for an entry the serve side would not call a stored page.
bool DeclaresHtml(std::string_view content_type) {
  constexpr std::string_view kHtml = "text/html";
  if (content_type.size() < kHtml.size()) return false;
  for (size_t i = 0; i < kHtml.size(); ++i) {
    char c = content_type[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
    if (c != kHtml[i]) return false;
  }
  if (content_type.size() == kHtml.size()) return true;
  const char next = content_type[kHtml.size()];
  return next == ';' || next == ' ' || next == '\t';
}

// The page's own URL, which the extractor resolves relative links against.
// It is never fetched.
std::string ModulePageUrl(const CacheNotification& n) {
  if (!n.url.starts_with('/')) return n.url;
  return absl::StrCat(n.scheme.empty() ? "http" : n.scheme, "://", n.hostname,
                      n.url);
}

std::span<const uint8_t> AsBytes(const std::array<std::byte, 32>& h) {
  return {reinterpret_cast<const uint8_t*>(h.data()), h.size()};
}

bool AllZeroHash(const std::array<std::byte, 32>& h) {
  return std::all_of(h.begin(), h.end(),
                     [](std::byte b) { return b == std::byte{0}; });
}

// Why a stored entry may not have a Markdown copy derived from it, or
// nullptr when it may: it must be an HTML page the module's HTML store
// wrote, allow transformation and carry its content hash.  Asked before any
// byte is copied and again just before anything is written for it.
const char* ModuleStoredEntryRefusal(const AlternateMetadata& meta) {
  if (meta.content_type != ContentType::kHtml ||
      !DeclaresHtml(meta.origin_content_type)) {
    return "the stored entry is not an HTML page";
  }
  if ((meta.flags & AlternateMetadata::kFlagHeadersClassified) == 0) {
    return "the stored entry was not written by the module's HTML store";
  }
  if ((meta.origin_cc_flags & AlternateMetadata::kCCOriginNoTransform) != 0) {
    return "the origin forbids transformation (no-transform)";
  }
  if (AllZeroHash(meta.origin_html_hash)) {
    return "the stored page carries no content hash, so no copy could bind";
  }
  return nullptr;
}

}  // namespace

void Worker::HandleModuleHtmlNotification(const CacheNotification& notification,
                                          const PurgeDispatchGen& purge_gen,
                                          const WorkerConfig& cfg) {
  const std::string& url = notification.url;
  const std::string& host = notification.hostname;
  const std::string& scheme = notification.scheme;
  const std::string& site = notification.site;
  constexpr auto kWithheldId =
      static_cast<AlternateId>(SentinelId::kAgentMarkdownWithheld);
  constexpr auto kMarkdownId =
      static_cast<AlternateId>(SentinelId::kAgentMarkdown);
  constexpr auto kContentHashId =
      static_cast<AlternateId>(SentinelId::kContentHash);
  constexpr auto kOriginalId =
      static_cast<AlternateId>(SentinelId::kOriginalContent);

  // This path never holds the page off with a cooldown: a lockout left by
  // anything else must not hold off the next frame either.
  ScopeExit clear_cooldown([this, &url, &host, &scheme, &site] {
    const std::string key = ComposeInternalKey(url, host, scheme, site);
    std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
    text_cooldown_expiry_.erase(key);
  });
  const auto refuse = [this, &url](const char* why) {
    stats_.notifications_rejected_module_render.fetch_add(
        1, std::memory_order_relaxed);
    LogInfo("Module HTML for %s refused: %s", url.c_str(), why);
  };

  // Checked again against this pass's own configuration: the mode or
  // agent_optimize may have changed since the event loop accepted the frame.
  if (!ModuleHtmlFrameAccepted(notification, cfg)) {
    refuse("not a stored page this worker derives a copy for");
    return;
  }
  if (cfg.html_mode == HtmlMode::kMarkdownRendered &&
      !module_rendered_mode_logged_.exchange(true, std::memory_order_relaxed)) {
    LogInfo(
        "html_mode markdown-rendered: pages stored by the module are derived "
        "without a browser, as in markdown-static (logged once)");
  }
  if (!cache_) {
    LogWarning("Cache not initialized, skipping module HTML for %s",
               url.c_str());
    return;
  }

  // The stored page, by EXACT id and only by exact id: a selection read could
  // return something other than the entry the module's HTML store wrote.
  // The module writes from another process, and this process's RAM tier is
  // write-around, so every read below evicts first.  A miss is retried with
  // the same backoff as every other source read, for the cross-process
  // propagation delay on a cold volume.
  std::optional<ReadResult> stored;
  for (int attempt = 0, delay_ms = kCacheReadInitialDelayMs;; ++attempt) {
    cache_->EvictAlternateFromRamCache(url, host, scheme, kOriginalId, site);
    auto read = cache_->ReadOriginalAlternate(url, host, scheme, site);
    if (read.has_value()) {
      stored = std::move(*read);
      break;
    }
    if (attempt >= kCacheReadMaxRetries ||
        (read.error() != cyclone::CacheError::NotFound &&
         read.error() != cyclone::CacheError::AlternateNotFound)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    delay_ms *= kCacheReadBackoffMultiplier;
  }
  if (!stored.has_value()) {
    refuse("no stored page");
    return;
  }

  // Refused before a byte of the page is copied: what the entry says it is.
  if (const char* why = ModuleStoredEntryRefusal(stored->metadata)) {
    refuse(why);
    return;
  }
  const std::array<std::byte, 32> stamp = stored->metadata.origin_html_hash;
  const size_t stored_bytes = stored->content().size();

  // A refusal already recorded for this content needs no second derivation.
  cache_->EvictAlternateFromRamCache(url, host, scheme, kWithheldId, site);
  bool marker_present = false;
  if (auto marker = cache_->ReadAlternate(url, host, scheme, kWithheldId, site);
      marker.has_value()) {
    marker_present = true;
    const auto payload = marker->content();
    if (HtmlWithheldMarkerBlocksDerivation(
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size()),
            AsBytes(stamp), cfg.max_html_size)) {
      stats_.module_markdown_withheld_skips.fetch_add(
          1, std::memory_order_relaxed);
      LogInfo("Markdown copy of %s is withheld for this content; not derived",
              url.c_str());
      return;
    }
  }

  // The purge fence for every write below.  Re-captured after this pass's
  // own purge of the derived copies, so the pass is not fenced by its own
  // bump, while a purge from anywhere else still fences it.
  PurgeDispatchGen gen = purge_gen;
  const auto purged = [this, &url, &host, &scheme, &site, &gen] {
    return WasPurgedSinceDispatch(url, host, gen, scheme, site);
  };
  // True while the stored page still carries the content hash this pass
  // read and still passes every entry check: a page stored again meanwhile,
  // even with the same bytes but now refusing transformation, is another
  // page's business.
  const auto still_bound = [this, &url, &host, &scheme, &site, &stamp] {
    cache_->EvictAlternateFromRamCache(url, host, scheme, kOriginalId, site);
    auto now = cache_->ReadOriginalAlternate(url, host, scheme, site);
    return now.has_value() &&
           ModuleStoredEntryRefusal(now->metadata) == nullptr &&
           now->metadata.origin_html_hash == stamp;
  };
  // A permanent refusal for this content: recorded, so the serve side
  // answers "withheld" and asks for no copy again until the content, the
  // deriver or (for a size refusal) the size cap changes.
  const auto withhold = [&](HtmlWithheldReason reason) {
    if (purged() || !still_bound()) return;
    HtmlWithheldMarker marker;
    marker.reason = reason;
    marker.deriver_version = kHtmlMarkdownDeriverVersion;
    std::memcpy(marker.parent_hash.data(), stamp.data(), stamp.size());
    if (reason == HtmlWithheldReason::kTooLarge) {
      marker.size_cap = cfg.max_html_size;
    }
    const std::string payload = EncodeHtmlWithheldMarker(marker);
    auto wh = cache_->WriteSentinel(url, host, scheme,
                                    SentinelId::kAgentMarkdownWithheld,
                                    payload.size(), site);
    const bool written =
        wh.has_value() &&
        wh->write_sync(std::as_bytes(std::span(payload.data(), payload.size())))
            .has_value() &&
        wh->close_sync().has_value();
    cache_->EvictAlternateFromRamCache(url, host, scheme, kWithheldId, site);
    const std::string_view name = HtmlWithheldReasonName(reason);
    if (!written) {
      stats_.errors.fetch_add(1, std::memory_order_relaxed);
      LogWarning("Withheld marker write failed for %s (%.*s)", url.c_str(),
                 static_cast<int>(name.size()), name.data());
      return;
    }
    stats_.module_markdown_withheld.fetch_add(1, std::memory_order_relaxed);
    LogInfo("Markdown copy of %s withheld: %.*s", url.c_str(),
            static_cast<int>(name.size()), name.data());
  };

  if (stored_bytes > cfg.max_html_size) {
    LogWarning("HTML too large (%zu > %zu), no Markdown copy for %s",
               stored_bytes, cfg.max_html_size, url.c_str());
    withhold(HtmlWithheldReason::kTooLarge);
    return;
  }

  // An owned copy, proven untorn, and the borrow released before anything
  // else runs against the bytes (the same discipline as the HTML pipeline).
  const auto borrowed = stored->content();
  std::string html(reinterpret_cast<const char*>(borrowed.data()),
                   borrowed.size());
  if (!BorrowCopyUntorn(*stored, cfg.read_lease_duration_ms > 0)) {
    stats_.read_borrow_wrap_discards.fetch_add(1, std::memory_order_relaxed);
    LogWarning("Stored HTML borrow wrapped during the copy, skipping %s",
               url.c_str());
    return;
  }
  stored.reset();
  if (test_hook_module_html_after_copy_) test_hook_module_html_after_copy_();

  if (IsGzipCompressed(html)) {
    stats_.errors_origin_misconfiguration.fetch_add(1,
                                                    std::memory_order_relaxed);
    LogWarning("Stored HTML for %s is compressed; no Markdown copy",
               url.c_str());
    withhold(HtmlWithheldReason::kEncoded);
    return;
  }
  const std::array<std::byte, 32> h =
      cyclone::crypto::SHA256::hash(std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(html.data()), html.size()));
  if (h != stamp) {
    LogWarning(
        "Stored HTML for %s does not match its content hash; no Markdown copy",
        url.c_str());
    withhold(HtmlWithheldReason::kHashMismatch);
    return;
  }

  // What is stored beside the page now.  Read from disk: a removal by the
  // module, in another process, leaves this process's RAM copies behind.
  cache_->EvictAlternateFromRamCache(url, host, scheme, kContentHashId, site);
  cache_->EvictAlternateFromRamCache(url, host, scheme, kMarkdownId, site);
  bool content_changed = true;
  if (auto recorded =
          cache_->ReadAlternate(url, host, scheme, kContentHashId, site);
      recorded.has_value()) {
    const auto c = recorded->content();
    content_changed =
        c.size() != h.size() || std::memcmp(c.data(), h.data(), h.size()) != 0;
  }
  bool copy_present = false;
  bool copy_bound = false;
  if (auto copy = cache_->ReadAlternate(url, host, scheme, kMarkdownId, site);
      copy.has_value()) {
    copy_present = true;
    copy_bound = copy->metadata.origin_html_hash == h;
  }

  if (content_changed) {
    // A purge from anywhere else since dispatch: this pass is fenced, and
    // its own purge below must not re-capture the fence past it.
    if (purged()) {
      LogInfo("Module HTML for %s not processed: purged meanwhile",
              url.c_str());
      return;
    }
    // New content, a first pass, or a removal in between.  Drop the copies
    // derived from earlier content; the stored page and its header block are
    // the module's and stay.  Never the whole key.  A copy already bound to
    // exactly these bytes is kept: only the record went missing.
    if (!copy_bound) {
      if (copy_present) {
        stats_.agent_markdown_purged_on_change.fetch_add(
            1, std::memory_order_relaxed);
      }
      PurgeDerivedCopiesAndBumpGeneration(url, host, scheme, site);
      gen = CapturePurgeGen(url, host, scheme, site);
      marker_present = false;  // the purge removed it with the other copies
    }
    auto wh = cache_->WriteSentinel(url, host, scheme, SentinelId::kContentHash,
                                    h.size(), site);
    const bool recorded =
        wh.has_value() &&
        wh->write_sync(std::as_bytes(std::span(h.data(), h.size())))
            .has_value() &&
        wh->close_sync().has_value();
    cache_->EvictAlternateFromRamCache(url, host, scheme, kContentHashId, site);
    if (!recorded) {
      // Without the record the next frame cannot tell this content from new
      // content.  Leave the copy for that frame rather than build it now.
      stats_.errors.fetch_add(1, std::memory_order_relaxed);
      LogWarning("Content hash write failed for %s; no Markdown copy yet",
                 url.c_str());
      return;
    }
  }

  // Demand: an agent asked for the copy and none is bound to this content,
  // or a copy that was in use belongs to earlier content.
  const bool demanded = notification.agent_request && !copy_bound;
  const bool stale_copy = copy_present && !copy_bound;
  if (!demanded && !stale_copy) {
    LogInfo("Module HTML for %s recorded; no Markdown copy requested",
            url.c_str());
    return;
  }

  // Extraction from the stored bytes alone: no request leaves this process.
  std::string markdown;
  try {
    markdown =
        net_instaweb::ExtractAgentMarkdown(html, ModulePageUrl(notification));
  } catch (const std::bad_alloc&) {
    // Memory pressure says nothing about the page: no marker, a later
    // request tries again.
    stats_.errors.fetch_add(1, std::memory_order_relaxed);
    LogWarning("Markdown extraction for %s ran out of memory", url.c_str());
    return;
  } catch (const std::exception& e) {
    LogWarning("Markdown extraction failed for %s: %s", url.c_str(), e.what());
    withhold(HtmlWithheldReason::kExtractFailed);
    return;
  }
  if (MarkdownBelowFidelityFloor(markdown)) {
    stats_.agent_markdown_below_floor.fetch_add(1, std::memory_order_relaxed);
    withhold(HtmlWithheldReason::kBelowFloor);
    return;
  }

  if (purged()) {
    stats_.alternate_writes_fenced.fetch_add(1, std::memory_order_relaxed);
    LogInfo("Markdown copy of %s not written: purged meanwhile", url.c_str());
    return;
  }
  if (!still_bound()) {
    LogInfo("Markdown copy of %s not written: the page was stored again",
            url.c_str());
    return;
  }
  AlternateMetadata copy_meta;
  copy_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  copy_meta.content_type = ContentType::kOther;
  copy_meta.origin_content_type = "text/markdown";
  copy_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  copy_meta.origin_html_hash = h;
  copy_meta.render_source_hash = cyclone::crypto::SHA256::hash(
      std::as_bytes(std::span(markdown.data(), markdown.size())));
  auto wh = cache_->WriteAgentAlternate(url, host, scheme, markdown.size(),
                                        copy_meta, site);
  const bool written =
      wh.has_value() &&
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value() &&
      wh->close_sync().has_value();
  cache_->EvictAlternateFromRamCache(url, host, scheme, kMarkdownId, site);
  if (!written) {
    stats_.alternate_write_failures.fetch_add(1, std::memory_order_relaxed);
    LogWarning("Markdown copy write failed for %s", url.c_str());
    return;
  }
  if (purged()) {
    // A purge landed between the check and the commit: take the copy back.
    if (!cache_->RemoveAlternate(url, host, scheme, kMarkdownId, site)
             .has_value()) {
      LogWarning("Markdown copy of %s written across a purge was not removed",
                 url.c_str());
    }
    stats_.alternate_writes_fenced.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (marker_present) {
    // A marker this pass did not obey (an older deriver, or a size cap since
    // raised) must not keep the serve side withholding the copy just built.
    if (!cache_->RemoveAlternate(url, host, scheme, kWithheldId, site)
             .has_value()) {
      // The copy stays unserved until the next store clears the marker.
      LogWarning(
          "Withheld marker for %s was not removed after its copy was "
          "written",
          url.c_str());
    }
  }
  stats_.module_markdown_derived.fetch_add(1, std::memory_order_relaxed);
  LogInfo("Wrote %zu bytes of Markdown for %s from the stored page",
          markdown.size(), url.c_str());
}

void Worker::HandleNotification(const CacheNotification& notification,
                                const PurgeDispatchGen& purge_gen) {
  // Snapshot live config for this notification (RCU read).
  const auto cfg = live_config_.load();

  // Purge guard: if this URL was purged after the notification was dispatched,
  // all variant writes must be skipped to avoid re-populating stale content.
  // This callback is passed to WriteVariant/WriteCompressedVariants.
  // Always created (even for purge_gen==0) to catch purges that arrive after
  // dispatch for URLs that were never previously purged.
  // Captures URL/hostname/scheme by value to avoid dangling reference if the
  // PurgeCheck outlives the notification parameter.
  PurgeCheck purge_check =
      [this, url = notification.url, hostname = notification.hostname,
       scheme = notification.scheme, site = notification.site, purge_gen]() {
        return WasPurgedSinceDispatch(url, hostname, purge_gen, scheme, site);
      };

  LogInfo("Processing notification for %s (type=%d, mask=0x%08x)",
          notification.url.c_str(), static_cast<int>(notification.content_type),
          notification.capability_mask);

  // Record URL in the registry for /v1/cache/urls enumeration.
  // Done early: nginx already wrote the default alternate, so the URL is cached.
  if (url_registry_) {
    url_registry_->Record(notification.url, notification.hostname,
                          notification.scheme, notification.site);
  }

  // Refresh the cached alternate count for this URL on EVERY exit path below.
  // The /v1/cache/urls listing reads this cached value instead of doing a
  // per-URL cache chain traversal on the event-loop thread (which caused the
  // /console/urls timeout and blocked /v1/health).  This guard runs at
  // function exit — after all variant writes/removes for this notification —
  // so it captures the post-write count for EVERY content type (including the
  // HTML WriteVariant path) and every sentinel sub-handler (warmup, llms.txt,
  // origin-refresh), which all dispatch after Record() above.  It executes on
  // the worker-pool thread (HandleNotification runs via uv_queue_work), so the
  // single extra ListAlternates stays off the event loop.  SetAlternateCount
  // is a no-op if the entry was LRU-evicted between Record() and here.
  ScopeExit count_refresh_guard(
      [this, url = notification.url, hostname = notification.hostname,
       scheme = notification.scheme, site = notification.site] {
        RefreshAlternateCount(url, hostname, scheme, site);
      });

  // Reject notifications with viewport=3 in the mask (sentinel range),
  // unless it is one of the whole-mask specials handled below.  A
  // notification with viewport=3 would write to a sentinel AlternateId,
  // potentially overwriting reserved data (Early Hints, etc.).
  //
  // The DECISION here is viewport-bits-shaped and stays that way: it does not
  // consult the sentinel registry, so a class this build knows and a class it
  // has never heard of are dropped identically.  That is deliberate — the set
  // of entry classes a notification may legitimately trigger is the three
  // whole-mask specials, and nothing else, whether or not this build happens
  // to recognise the id.  What the registry contributes is the DIAGNOSTIC: the
  // log names the class where it can, so an operator can tell "a peer named a
  // class this build does not have" (unknown_sentinel — a namespace-skewed
  // peer) apart from "a peer used a known class as a notification trigger"
  // (a sender bug).  Those have different fixes and were previously
  // indistinguishable in the log.
  {
    uint8_t viewport_bits = (notification.capability_mask >> 2) & 0x03;
    if (viewport_bits == 3 && notification.capability_mask != kWarmupSentinel &&
        notification.capability_mask != kLlmsTxtSentinel &&
        notification.capability_mask != kOriginRefreshedSentinel) {
      const auto sentinel_id =
          static_cast<AlternateId>(notification.capability_mask & 0xFF);
      const std::string_view sentinel_name = SentinelName(sentinel_id);
      LogWarning(
          "Notification naming sentinel entry class %.*s (id=0x%02X, "
          "mask=0x%08x) rejected: this build accepts no notification for that "
          "class, so it was dropped and nothing was written (url=%s)",
          static_cast<int>(sentinel_name.size()), sentinel_name.data(),
          static_cast<unsigned>(sentinel_id), notification.capability_mask,
          notification.url.c_str());
      stats_.notifications_rejected_sentinel.fetch_add(
          1, std::memory_order_relaxed);
      stats_.errors.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }

  // A stored HTML page from the module has a handler of its own, ahead of
  // the dedup, the cooldown and the front end's repeat-pass skip below: none
  // of them may swallow the agent request that follows a store frame, and
  // nothing of the HTML pipeline after them (the scan, the stylesheet reads,
  // browser analysis, an HTML variant) applies to it.
  if (notification.from_module &&
      notification.content_type == ContentType::kHtml) {
    HandleModuleHtmlNotification(notification, purge_gen, *cfg);
    return;
  }

  // Warmup sentinel: nginx sends mask=0xFFFFFFFE when a URL crosses
  // the hot threshold.  Dispatch to dedicated warmup handler.
  if (notification.capability_mask == kWarmupSentinel) {
    if (cfg->enable_warmup) {
      HandleWarmupRequest(notification, purge_gen);
    } else {
      LogInfo("Warmup disabled, ignoring sentinel for %s",
              notification.url.c_str());
    }
    return;
  }

  // llms.txt sentinel: nginx requests a lazy /llms.txt (re)build.
  // Dispatch to the dedicated handler, bypassing the variant dedup/cooldown
  // below exactly as warmup does.
  if (notification.capability_mask == kLlmsTxtSentinel) {
    if (!MayRequestHostFetches(notification)) {
      LogInfo(
          "llms.txt: not built for a notification with an options "
          "context (%s)",
          notification.url.c_str());
      return;
    }
    if (cfg->browser_analysis.agent_optimize &&
        cfg->browser_analysis.agent_optimize_llms_txt &&
        !notification.host_vouched) {
      // The build fetches from the notification's host: only for a sender
      // that vouched for it.
      stats_.notifications_unvouched_render_skipped.fetch_add(
          1, std::memory_order_relaxed);
      LogInfo("llms.txt: not built for an unvouched host (%s)",
              notification.url.c_str());
    } else if (cfg->browser_analysis.agent_optimize &&
               cfg->browser_analysis.agent_optimize_llms_txt) {
      HandleLlmsTxtBuild(notification, purge_gen);
    } else {
      LogInfo("llms.txt build disabled, ignoring sentinel for %s",
              notification.url.c_str());
    }
    return;
  }

  // Origin-refreshed sentinel (issue #652): a freshness-driven re-fetch in
  // nginx obtained refreshed origin content (re-recorded at the identity
  // id) while stale optimized variants still exist for the URL.  Purge the
  // stale non-identity variants (bumping the purge generation and clearing
  // dedup), then rebuild the variant set inline from the preserved fresh
  // identity.  Bypasses the dedup/cooldown machinery below exactly as the
  // other sentinels do.
  if (notification.capability_mask == kOriginRefreshedSentinel) {
    HandleOriginRefreshed(notification);
    return;
  }

  // The origin negotiates on Accept itself: never derive variants for this
  // URL, whatever asked us to.
  //
  // This is the authoritative enforcement point, and it lives here rather
  // than only in the front stage because the invariant is per-URL while the
  // marker is per-ALTERNATE — it is stamped on the identity original, which
  // is the only alternate the store path writes.  A front stage that decides
  // from whichever alternate it happened to select reads an unmarked variant
  // whenever one still exists (a URL optimized BEFORE its origin started
  // sending `Vary: Accept`), and asks for exactly the optimization the marker
  // forbids.  Reading the identity here answers the question about the URL.
  //
  // It also decides the origin-refresh rebuild, which dispatches back through
  // this function: the purge above drops the stale variant set and this
  // refuses to rebuild it, so the refresh becomes purge-only for a marked URL
  // instead of needing a second, purge-only sentinel.
  //
  // Failure-open on a read miss is deliberate: no identity means nothing to
  // optimize from, and the paths below handle that case as they always have.
  //
  // Counted on the existing skipped counter, which the cooldown path below
  // already shares: it means "notification dropped without processing", not
  // "dedup hit specifically".  The log line carries the reason, so an
  // operator asking why a URL is not being optimized gets the answer there
  // rather than from an undifferentiated total.
  if (OriginNegotiatesByAccept(notification)) {
    LogInfo(
        "Skipping %s: origin negotiates on Accept (stored as-is, never "
        "optimized)",
        notification.url.c_str());
    stats_.notifications_skipped_dedup.fetch_add(1, std::memory_order_relaxed);
    // Skipping alone would leave a variant set derived before the flip in
    // place with nothing left to remove it (an image's origin-refresh sentinel
    // defers to this notification).  Drop the derived copies; the marked
    // original is kept.  Under the purge-generation lock, as the recheck does.
    PurgeDerivedCopiesAndBumpGeneration(notification.url, notification.hostname,
                                        notification.scheme, notification.site);
    ClearUrlProcessingState(notification.url, notification.hostname,
                            notification.scheme, notification.site);
    return;
  }

  // Notification deduplication: skip if the worker has already processed
  // a variant for this (url, hostname, alternateId) combination.
  //
  // Uses an in-memory set instead of cache reads to avoid polluting
  // Cyclone's write-around RAM cache (reads populate RAM, but writes
  // don't evict, causing stale-data issues in single-process tests
  // and potentially in production).
  //
  // The set is populated on successful variant writes and cleared
  // on URL invalidation.  Not persisted across restarts — the worker
  // will reprocess once after restart, which is the desired behavior.
  {
    // Strip encoding and image-format bits for dedup: text content
    // types (HTML/CSS/JS) don't vary by image format, and write paths
    // always use encoding=kIdentity.  Without this, AVIF notifications
    // (mask 0x4A) would check AlternateId 0x0A instead of 0x08,
    // missing the existing variant and causing unbounded reprocessing.
    // Images keep the format bits: their key is the full slot (see
    // ImageDedupMask).
    AlternateId target_id = MaskToId(
        DedupMaskFor(notification.content_type, notification.capability_mask));
    std::string dedup_key = ComposeInternalKeyWithId(
        notification.url, notification.hostname, notification.scheme,
        notification.site, target_id);
    bool dedup_hit = false;
    bool copy_expected = false;
    {
      std::lock_guard<std::mutex> lock(processed_set_mutex_);
      dedup_hit = processed_current_.contains(dedup_key) ||
                  processed_previous_.contains(dedup_key);
      copy_expected = copy_written_current_.contains(dedup_key) ||
                      copy_written_previous_.contains(dedup_key);
    }
    if (dedup_hit && cache_ != nullptr) {
      // The dedup key is the URL plus a variant id, never the content, so a
      // front end that re-recorded the durable original with NEW bytes after
      // this set was built would be skipped here and keep serving the old
      // derived copies.  Compare the recorded original's origin state with
      // the one the variant set inherited; on a difference, hash it and
      // either restamp (same bytes) or purge the derived copies and rebuild.
      const auto verdict = RecheckRecordedOriginal(notification);
      if (verdict == OriginRecheck::kRestamped) {
        stats_.notifications_skipped_dedup.fetch_add(1,
                                                     std::memory_order_relaxed);
        return;
      }
      if (verdict == OriginRecheck::kPurgedRebuild) {
        // The processed-set entry is gone and the purge generation was
        // re-read AFTER the purge, so this pass is not fenced by its own bump.
        HandleNotification(
            notification,
            CapturePurgeGen(notification.url, notification.hostname,
                            notification.scheme, notification.site));
        return;
      }
    }
    if (dedup_hit) {
      // Orphaned-entry heal.  A dedup
      // entry can outlive the variant it stands for: entries leave this set
      // only via purge/invalidation, full cache reset, or generational
      // overflow, while Cyclone capacity eviction has no callback into the
      // worker.  Answering "skip" for an entry whose variant family is gone
      // disables optimization for the URL forever, so before skipping,
      // validate that the family still holds at least one SERVABLE
      // alternate.
      //
      // Servable here means NOT in sentinel space (IsSentinel: viewport
      // bits == 3 — the durable original 0x0C, the content-hash and
      // decline-tombstone records, Early Hints, kAgentMarkdown).  Those are
      // metadata a front end never serves as content.  The identity
      // original (0x00, and the default-mask 0x08 nginx writes) is NOT
      // sentinel space and counts as family-present, deliberately: on the
      // nginx substrate an identity-only key is healed by the
      // origin-refresh sentinel path (a freshness re-fetch re-records the
      // identity, purges stale variants, and clears dedup), so an
      // identity-only key is not a state this heal is responsible for.
      // What remains — a family with nothing servable at all — is exactly
      // the state no other path re-processes.
      //
      // EMPTY BY DECISION vs ORPHANED.  A sentinel-only family is not
      // necessarily orphaned.  At least five paths mark an entry processed
      // while writing nothing servable: every format quality-declined (a
      // decline tombstone), a deterministic CSS or JS minify failure, an
      // integrity-pinned URL, and an image over max_image_size.  Two of
      // them say outright that retrying "would loop forever".  This check
      // cannot tell those apart from a family the cache evicted, and
      // re-processing them never yields a variant, so an unconditional
      // heal turns each of them into a per-notification re-pay — and with
      // nothing servable the front end falls through, and re-notifies, on
      // every request.
      //
      // Enumerating the decided states by name is not sound: the list is
      // open-ended, and naming one (the tombstone) leaves the rest looping
      // while asserting a safety property the id check cannot deliver — it
      // matches on the alternate id and never reads the payload's source
      // hash, so a "decided" skip would also stand after the source
      // changed.  The heal is therefore BOUNDED rather than forbidden: at
      // most one re-process per URL per kDedupHealMinIntervalSecs.  A
      // genuinely orphaned family still converges on its next
      // notification; an empty-by-decision family costs one bounded re-pay
      // per window instead of one per request, and — unlike a permanent
      // skip — still notices when the source changes.
      //
      // RAM tier: ListAlternates walks the directory/chain through the mmap
      // directly and never reads document content, so it neither consults
      // nor populates Cyclone's write-around RAM cache — this check cannot
      // introduce stale-RAM serving, and no EvictAlternateFromRamCache is
      // needed.  It is also not a new cost class: the RefreshAlternateCount
      // ScopeExit guard above already pays the same traversal per
      // notification.  Cache I/O runs outside processed_set_mutex_; the
      // erase below re-takes it, and a mark/erase racing in between is
      // benign in both directions (at worst one extra re-process).
      // Heal rate limit, per URL.  Mirrors origin_refresh_last_purge_ /
      // kOriginRefreshMinIntervalSecs, which exists for the same shape: a
      // URL that re-enters this path on every request.  Checked BEFORE the
      // ListAlternates traversal, so a rate-limited notification costs the
      // same O(1) as the skip it stands in for.
      {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(dedup_heal_mutex_);
        auto it = dedup_heal_last_.find(dedup_key);
        if (it != dedup_heal_last_.end() &&
            now - it->second <
                std::chrono::seconds(kDedupHealMinIntervalSecs)) {
          stats_.notifications_dedup_heal_rate_limited.fetch_add(
              1, std::memory_order_relaxed);
          stats_.notifications_skipped_dedup.fetch_add(
              1, std::memory_order_relaxed);
          return;
        }
      }
      bool variant_family_present = true;  // no cache: keep today's skip
      // MISSING COPY.  A family can be present and still lack the one copy
      // this entry stands for: the optimized copy of a stylesheet or script
      // is one alternate among several (its gzip and brotli siblings, the
      // stored original), and it can go missing on its own -- the siblings
      // then keep the family "present" for the test above while every
      // request is answered with the original.  That is only a loss when
      // this process actually wrote the copy (copy_expected): the verdicts
      // that write none -- already minimal, a minify failure, an
      // integrity-pinned URL -- leave no such record, so none of them can
      // be mistaken for it.  Bounded by the same per-URL window as the heal
      // below, which was checked above before any cache read.
      bool expected_copy_missing = false;
      if (cache_ != nullptr) {
        auto alts =
            cache_->ListAlternates(notification.url, notification.hostname,
                                   notification.scheme, notification.site);
        if (!alts.has_value()) {
          // A cache ERROR is not evidence the family is gone.  Keep the
          // skip, matching the no-cache case above: reading an error as
          // "orphaned" would turn a persistent ListAlternates failure into
          // a reprocess on every notification.  Same uncertainty, same
          // fail direction.
          variant_family_present = true;
        } else {
          variant_family_present = false;
          bool target_present = false;
          for (const auto& alt : *alts) {
            const auto alt_id = static_cast<AlternateId>(alt.id);
            if (!IsSentinel(alt_id)) {
              variant_family_present = true;
            }
            if (alt_id == target_id) {
              target_present = true;
            }
          }
          expected_copy_missing =
              copy_expected && variant_family_present && !target_present;
        }
      }
      if (variant_family_present && !expected_copy_missing) {
        LogInfo(
            "Variant already processed for %s (mask=0x%08x), "
            "skipping",
            notification.url.c_str(), notification.capability_mask);
        stats_.notifications_skipped_dedup.fetch_add(1,
                                                     std::memory_order_relaxed);
        return;
      }
      {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(dedup_heal_mutex_);
        if (dedup_heal_last_.size() >= kMaxDedupHealEntries) {
          // Overflow: sweep only EXPIRED windows, never live ones.  The
          // origin-refresh limiter learned this the hard way — a wholesale
          // clear() degraded it to a no-op under exactly the churn it was
          // added to bound.
          std::erase_if(dedup_heal_last_, [now](const auto& kv) {
            return now - kv.second >=
                   std::chrono::seconds(kDedupHealMinIntervalSecs);
          });
          if (dedup_heal_last_.size() >= kMaxDedupHealEntries) {
            // Every tracked URL is in-window.  Defer rather than heal
            // untracked: an untracked heal is an unbounded one.
            stats_.notifications_dedup_heal_rate_limited.fetch_add(
                1, std::memory_order_relaxed);
            stats_.notifications_skipped_dedup.fetch_add(
                1, std::memory_order_relaxed);
            return;
          }
        }
        dedup_heal_last_[dedup_key] = now;
      }
      {
        std::lock_guard<std::mutex> lock(processed_set_mutex_);
        processed_current_.erase(dedup_key);
        processed_previous_.erase(dedup_key);
        // The record is spent with the heal: it is written again only if
        // the re-processing below writes the copy again.
        copy_written_current_.erase(dedup_key);
        copy_written_previous_.erase(dedup_key);
      }
      if (expected_copy_missing) {
        stats_.notifications_missing_copy_healed.fetch_add(
            1, std::memory_order_relaxed);
        LogInfo(
            "Optimized copy for %s (mask=0x%08x) is missing from the cache "
            "while other copies of the URL remain; processing the URL again",
            notification.url.c_str(), notification.capability_mask);
      } else {
        stats_.notifications_dedup_healed.fetch_add(1,
                                                    std::memory_order_relaxed);
        LogInfo(
            "Dedup entry for %s (mask=0x%08x) orphaned: no servable variant "
            "remains in cache, healing (re-processing)",
            notification.url.c_str(), notification.capability_mask);
      }
      // Fall through to normal processing.
    }
  }

  // Text processing cooldown: skip if this URL+hostname has an active
  // cooldown.  Prevents tight re-processing loops from:
  //  - Write failures (reader contention on mmap handles): 60s cooldown
  //  - Revalidation writes (CSS not yet cached): 3s cooldown
  //  - Concurrent processing (tentative in-flight guard, HTML only): 60s cooldown
  // CSS/JS only need the check (to honor write-failure cooldowns set by
  // SetWriteFailureCooldown); the tentative entry is HTML-only.
  if (notification.content_type == ContentType::kHtml ||
      notification.content_type == ContentType::kCss ||
      notification.content_type == ContentType::kJs) {
    std::string cooldown_key =
        ComposeInternalKey(notification.url, notification.hostname,
                           notification.scheme, notification.site);
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
    auto it = text_cooldown_expiry_.find(cooldown_key);
    if (it != text_cooldown_expiry_.end()) {
      if (now < it->second.expiry) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                             it->second.expiry - now)
                             .count();
        LogInfo("Skipping %s (cooldown, %llds remaining)",
                notification.url.c_str(), static_cast<long long>(remaining));
        stats_.notifications_skipped_dedup.fetch_add(1,
                                                     std::memory_order_relaxed);
        return;
      }
      // Expired — remove entry.
      text_cooldown_expiry_.erase(it);
    }
    // Set tentative cooldown for HTML only.  HTML has complex
    // multi-phase processing (scan, transform, write, revalidation)
    // that benefits from concurrent-processing protection.  CSS/JS
    // only need the cooldown check above (to honor write-failure
    // cooldowns), not a tentative entry.
    if (notification.content_type == ContentType::kHtml) {
      text_cooldown_expiry_[cooldown_key] = CooldownEntry{
          now, now + std::chrono::seconds(kWriteFailureCooldownSecs),
          CooldownReason::kProcessing};
    }
  }

  auto notif_start = std::chrono::steady_clock::now();

  switch (notification.content_type) {
    case ContentType::kHtml: {
      // Helper: clear the tentative kProcessing cooldown set above.
      // Must be called on every early-return path so the URL isn't
      // locked out for 60s after a transient failure.
      auto clear_tentative_cooldown = [&]() {
        std::string ck =
            ComposeInternalKey(notification.url, notification.hostname,
                               notification.scheme, notification.site);
        std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
        text_cooldown_expiry_.erase(ck);
      };

      if (cfg->html_mode == HtmlMode::kOff) {
        LogInfo("HTML processing disabled (html_mode off), skipping %s",
                notification.url.c_str());
        clear_tentative_cooldown();
        return;
      }

      if (!cache_) {
        LogWarning("Cache not initialized, skipping HTML processing");
        clear_tentative_cooldown();
        return;
      }

      // Drop what an earlier version stored for this page and this build no
      // longer serves.  The pass below overwrites what it rebuilds, but not
      // what it does not (a page left unchanged, a compression level now
      // off), and selection would otherwise keep choosing those, as a miss.
      if (auto dropped = cache_->RemoveRetiredAlternates(
              notification.url, notification.hostname, notification.scheme,
              notification.site);
          dropped.has_value() && *dropped > 0) {
        LogInfo("Dropped %zu entries stored by an earlier version for %s",
                *dropped, notification.url.c_str());
      }

      // Read original HTML from cache (stored with default mask).
      // Uses exponential-backoff retry for cross-process mmap propagation
      // delay on cold start (10ms, 50ms, 250ms).
      auto html_read_result = ReadOriginalWithRetry(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site, stats_, handler_);

      if (!html_read_result.has_value()) {
        LogWarning("Original HTML not found in cache for %s",
                   notification.url.c_str());
        clear_tentative_cooldown();
        return;
      }

      // TEST-ONLY (issue #934): lets a test move the stripe's wrap epoch
      // inside the read->copy->verify window (see worker.h).  Null in
      // production.
      if (test_hook_after_html_origin_read_) {
        test_hook_after_html_origin_read_();
      }

      auto html_content_span = html_read_result->content();
      std::string_view html_content(
          reinterpret_cast<const char*>(html_content_span.data()),
          html_content_span.size());

      // Guard: reject gzip-compressed content.  PageSpeed caches raw
      // (uncompressed) bytes.  If compressed content leaked into cache
      // (e.g. origin sent Content-Encoding: gzip), parsing it as HTML
      // produces corrupt output.  Skip processing entirely.
      // Note: this magic-byte check is defense-in-depth for gzip only;
      // the primary protection is nginx's Content-Encoding header check
      // which strips encoding for all formats before caching.
      if (IsGzipCompressed(html_content)) {
        LogError(
            "Cached content appears gzip-compressed, skipping "
            "HTML processing for %s. Ensure origin sends "
            "uncompressed responses (proxy_set_header "
            "Accept-Encoding \"\").",
            notification.url.c_str());
        stats_.errors.fetch_add(1, std::memory_order_relaxed);
        stats_.errors_origin_misconfiguration.fetch_add(
            1, std::memory_order_relaxed);
        clear_tentative_cooldown();
        return;
      }

      if (html_content.size() > cfg->max_html_size) {
        LogWarning("HTML too large (%zu > %zu), skipping %s",
                   html_content.size(), cfg->max_html_size,
                   notification.url.c_str());
        clear_tentative_cooldown();
        return;
      }

      // De-alias the origin borrow before the (unbounded) HTML pipeline
      // (issue #934).  Copy the size-capped, verified-non-gzip
      // origin bytes into an owned buffer, prove the copy is untorn, then
      // RELEASE the borrow so every downstream step — SHA-256, sentinel
      // writes, purge/Remove, CSS sub-reads, the full transform parse and the
      // variant write — runs against owned memory.  Without this, at
      // cache-full a write-buffer wrap could overwrite the mmap under the
      // alias mid-parse and the worker would persist a torn transform with a
      // fresh checksum (durable corruption).  One bounded memcpy
      // (<= max_html_size) per disk-hit HTML read; mirrors the image path.
      std::string owned_html(html_content);
      if (!BorrowCopyUntorn(*html_read_result,
                            cfg->read_lease_duration_ms > 0)) {
        // A write-buffer wrap raced the copy — the bytes may be torn.  Treat
        // as a cache miss (as if the original were not found); a later
        // notification re-reads a settled slot.
        stats_.read_borrow_wrap_discards.fetch_add(1,
                                                   std::memory_order_relaxed);
        LogWarning(
            "Origin HTML borrow wrapped during de-alias copy, "
            "skipping %s",
            notification.url.c_str());
        clear_tentative_cooldown();
        return;
      }
      html_read_result->release();
      html_content = owned_html;

      // -----------------------------------------------------------------
      // Content-hash unit invalidation for the
      // HTML key.  The kContentHash sentinel computed over the RAW origin
      // HTML binds the whole variant set (human HTML + the agent markdown
      // variant) so an origin change invalidates them as a unit.  It is the
      // oracle the serve-time equality gate (Gap#2) and the store-time
      // re-validation (Gap#3) read.  Gated behind agent_optimize so disabling it
      // leaves this block's machinery out of the path (no ReadAlternate, no
      // Remove, no WriteSentinel).  The sentinel's SHA-256 is not the only
      // hash left, though: the variant stamp at the optimized-identity write
      // (issue #434) hashes a raw read once per run with agent_optimize off
      // too.  NOTE: the v7 cache WIRE
      // FORMAT (P2.1) always carries the 64-byte (zero-when-unbound) hash suffix
      // on every variant regardless of the flag — that is the intended,
      // test-covered format cost, NOT part of the processing-path OFF claim.
      // The mmap backing html_content stays valid across Remove via the
      // ReadResult anchor (same lifetime guarantee the image path relies on).
      // The computed hash is also carried to the agent render (P2.3) so its
      // store-time re-validation (Gap#3) can prove the binding is still live.
      // -----------------------------------------------------------------
      // The identity metadata carries kFlagWorkerProcessed exactly when the
      // read is the worker's own output (alternate_metadata.h:69).  Computed
      // once here because two consumers need it: the agent_optimize block
      // below (the raw-vs-derived oracle question) and the variant stamp at
      // the optimized-identity write (the source-binding rule, issue #434).
      const bool worker_processed_identity =
          (html_read_result->metadata.flags &
           AlternateMetadata::kFlagWorkerProcessed) != 0;

      // Issue #507: a page the transform leaves unchanged writes no variant,
      // so the raw entry the front end serves would stay without a source
      // binding and a validator-less origin would get no ETag.  Rewrite that
      // entry with identical bytes and the stamp, under the same rule as the
      // variant stamps: only a raw read (not the worker's own output) is
      // hashed, and a stamp that is already correct is not rewritten.
      auto stamp_unchanged_html = [&]() {
        if (worker_processed_identity) return;
        const StampResult stamped = StampRawEntry(
            cache_.get(), notification.url, notification.hostname,
            notification.scheme, notification.site, html_read_result->metadata,
            html_content,
            cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(html_content.data()),
                html_content.size())),
            purge_check, this);
        CountStamp(stats_, stamped);
        if (stamped == StampResult::kFailed) {
          LogWarning("Failed to stamp unchanged HTML for %s",
                     notification.url.c_str());
        }
      };

      std::optional<std::array<std::byte, 32>> agent_origin_html_hash;
      // Whether this pass found the content-hash record missing or different
      // (an origin change, a first pass, or a purge in between).
      bool agent_content_changed = true;
      if (cfg->browser_analysis.agent_optimize) {
        // Issue B: the read slot (0x08) FLIPS between the raw origin body
        // (written by nginx) and the worker's OWN optimized output (written
        // below at the identity mask).  Re-hashing the read slot to derive the
        // content-binding oracle is only valid when the slot still holds RAW
        // bytes.  When it holds the worker's optimized output, SHA-256 of those
        // bytes is meaningless against a raw-origin sentinel — it falsely fires
        // content_changed, self-purges the whole key (incl. the agent markdown
        // variant), and the in-flight HTML write is then fenced by its now-stale
        // dispatch baseline, double-counting write_failures+errors.
        //
        // In that case we MUST NOT re-hash; instead we reuse the DURABLE
        // raw-origin hash: persisted onto the identity's
        // metadata.origin_html_hash when raw was last read (see the
        // optimized-identity write below), with a fallback to the live
        // kContentHash sentinel for legacy/unplumbed blobs.

        // The live sentinel as written over RAW on a prior raw-read run.
        auto stored = cache_->ReadAlternate(
            notification.url, notification.hostname, notification.scheme,
            static_cast<AlternateId>(SentinelId::kContentHash),
            notification.site);

        std::array<std::byte, 32> origin_html_hash{};
        bool have_raw_hash = false;
        if (!worker_processed_identity) {
          // Read slot holds RAW origin bytes — hash them directly.
          origin_html_hash =
              cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                  reinterpret_cast<const std::byte*>(html_content.data()),
                  html_content.size()));
          have_raw_hash = true;
        } else {
          // Read slot holds the worker's OWN optimized output — reuse the
          // durable raw-origin hash rather than hashing optimized bytes.
          static constexpr std::array<std::byte, 32> kZeroHash{};
          if (html_read_result->metadata.origin_html_hash != kZeroHash) {
            origin_html_hash = html_read_result->metadata.origin_html_hash;
            have_raw_hash = true;
          } else if (stored.has_value() && stored->content().size() == 32) {
            // Legacy identity (no persisted hash): fall back to the live
            // sentinel, itself written over raw on the prior raw-read run.
            std::memcpy(origin_html_hash.data(), stored->content().data(), 32);
            have_raw_hash = true;
          }
        }

        if (have_raw_hash) {
          agent_origin_html_hash = origin_html_hash;
        }

        bool content_changed = true;
        if (have_raw_hash && stored.has_value()) {
          auto sc = stored->content();
          if (sc.size() == 32 &&
              std::memcmp(sc.data(), origin_html_hash.data(), 32) == 0) {
            content_changed = false;
          }
        }
        agent_content_changed = content_changed;
        // Only purge on a GENUINE origin change, which can only be observed
        // when the read slot is raw (worker-processed reads never carry a new
        // origin).  Gating on !worker_processed_identity is the belt-and-braces
        // guard mandated by the Issue-B fix: even if the durable hash were
        // somehow absent, a worker-processed read must never self-purge.
        if (content_changed && stored.has_value() && have_raw_hash &&
            !worker_processed_identity) {
          // Origin advanced since the bound variant set was produced — drop
          // the stale set (whole-key) so a superseded agent markdown variant
          // can never be served, and bump the purge generation so an in-flight
          // render's store-time re-check (Gap#3) is rejected.  Bump + Remove
          // are atomic under purge_gen_mutex_ (issue #652).
          stats_.content_hash_stale.fetch_add(1, std::memory_order_relaxed);
          (void)PurgeUrlAndBumpGeneration(
              notification.url, notification.hostname, notification.scheme,
              notification.site);
          {
            std::lock_guard<std::mutex> lock(processed_set_mutex_);
            std::string prefix =
                absl::StrCat(notification.url, "|", notification.hostname, "|",
                             notification.scheme, "|");
            EraseUrlMarksLocked(prefix);
          }
        }
        // Refresh the content-hash binding ONLY when we have the true raw
        // hash AND it actually changed (raw-read run).  On a worker-processed
        // reprocess the sentinel already holds the correct raw hash, so we
        // leave it untouched — rewriting it with anything derived from the
        // optimized slot would poison the agent markdown serve-gate binding.
        // FAIL-CLOSED (unlike the image path's (void)-swallow): if the
        // kContentHash write fails the agent variant cannot be bound, so leave
        // the sentinel absent — the serve gate (Gap#2) treats an absent/all-zero
        // binding as refuse-to-serve.
        if (have_raw_hash && content_changed) {
          auto ch_wh = cache_->WriteSentinel(
              notification.url, notification.hostname, notification.scheme,
              SentinelId::kContentHash, 32, notification.site);
          if (ch_wh.has_value()) {
            (void)ch_wh->write_sync(std::as_bytes(
                std::span(origin_html_hash.data(), origin_html_hash.size())));
            (void)ch_wh->close_sync();
            // Write-around RAM tier (issue #1126): the `stored` read above
            // populated this process's RAM copy of the sentinel.  On the
            // no-purge path (worker_processed_identity with a missing or
            // mismatched sentinel) this overwrite would leave that copy
            // stale — the next notification's `stored` read would re-derive
            // content_changed and rewrite on every run.  Evict post-commit.
            // On the purge path this is a no-op: the whole-key Remove
            // already cleared the RAM tier.
            cache_->EvictAlternateFromRamCache(
                notification.url, notification.hostname, notification.scheme,
                static_cast<AlternateId>(SentinelId::kContentHash),
                notification.site);
          } else {
            stats_.errors.fetch_add(1, std::memory_order_relaxed);
            LogWarning(
                "agent_optimize: kContentHash write failed for %s; agent "
                "variant will be unbound (serve gate refuses unbound variants)",
                notification.url.c_str());
          }
        }
      }

      // A markdown-* mode writes no HTML variant, so the processed-variant
      // dedup above never marks the page and every agent request that misses
      // the Markdown copy would repeat the scan, the stylesheet reads and the
      // analysis lookup below.  Skip a page this worker already passed with
      // the same origin hash recently.  The skip needs the content-hash
      // record to be present and equal to this read's hash, so an origin
      // change, a purge or a refresh (all of which leave the record missing
      // or different) is always processed.
      if (!HtmlModeWritesHtmlVariant(cfg->html_mode) &&
          agent_origin_html_hash.has_value() && !agent_content_changed &&
          MarkdownModePassIsCurrent(notification, *agent_origin_html_hash)) {
        LogInfo("html_mode %s: %s unchanged since its last pass, skipping",
                std::string(HtmlModeName(cfg->html_mode)).c_str(),
                notification.url.c_str());
        stats_.notifications_skipped_dedup.fetch_add(1,
                                                     std::memory_order_relaxed);
        clear_tentative_cooldown();
        return;
      }

      // Scan HTML to collect elements, stylesheet links, inline CSS.
      HtmlScanner scanner;
      HtmlScanResult scan_result = scanner.Scan(notification.url, html_content);

      if (!scan_result.success) {
        LogWarning("HTML scanning failed for %s: %s", notification.url.c_str(),
                   scan_result.error_message.c_str());
        clear_tentative_cooldown();
        return;
      }

      // SRI (issue #656): subresources referenced with an integrity
      // attribute must keep their original bytes — register them before
      // any further processing so CSS/JS notifications skip them.
      RegisterIntegrityPinnedUrls(scan_result, notification);

      // Gather the combined stylesheet this page is judged against. The exact
      // bytes are a contract, not an implementation detail — see
      // CombinedCssValidationHash in src/browser/optimization_profile.h.
      // Gathered before the page is queued for browser analysis: the
      // analysis assembles the same bytes on the event loop, where the
      // sub-resource gate answers from what this call has just asked.
      CombinedCssResult combined =
          BuildCombinedCss(scan_result, notification, *cfg);

      // Browser analysis: compute template hash and look up profile.
      // A page is queued for analysis (and the agent render that follows it)
      // only for a sender that may ask for fetches; see
      // MayRequestHostFetches.
      uint64_t template_hash = 0;
      std::optional<OptimizationProfile> browser_profile;
      if (browser_manager_) {
        template_hash = TemplateDetector::HashStructure(scan_result);
        browser_profile =
            browser_manager_->LookupProfile(template_hash, notification.site);
        // Both rules must allow a page to be queued: the sender may ask for
        // fetches (MayRequestHostFetches), and the agent render that can
        // follow an analysis fetches from the notification's host, so it runs
        // only for a vouched notification (BrowserAnalysisManager::
        // FinishAnalysis checks the item's bit).  The analysis itself, and
        // HTML optimization, proceed for an unvouched one.
        const bool may_analyse = MayRequestHostFetches(notification);
        const bool render_withheld =
            cfg->browser_analysis.agent_optimize && !notification.host_vouched;
        if (!browser_profile) {
          if (may_analyse) {
            browser_manager_->EnqueueAnalysis(
                notification.url, notification.hostname, notification.scheme,
                notification.site, notification.host_vouched,
                notification.from_module, notification.capability_mask,
                template_hash,
                /*original_html=*/html_content, agent_origin_html_hash);
            if (render_withheld) {
              stats_.notifications_unvouched_render_skipped.fetch_add(
                  1, std::memory_order_relaxed);
            }
          }
        } else if (render_withheld && notification.agent_request) {
          stats_.notifications_unvouched_render_skipped.fetch_add(
              1, std::memory_order_relaxed);
        } else if (may_analyse && cfg->browser_analysis.agent_optimize &&
                   notification.agent_request &&
                   agent_origin_html_hash.has_value() &&
                   !browser_manager_->AgentCopyBelowFloor(
                       notification.url, notification.hostname,
                       notification.scheme, notification.site,
                       *agent_origin_html_hash) &&
                   AgentMarkdownNeedsBuild(
                       notification.url, notification.hostname,
                       notification.scheme, notification.site,
                       *agent_origin_html_hash)) {
          // The template's perf profile already exists
          // (LookupProfile hit), so the perf-driven enqueue above is skipped —
          // but THIS url's agent markdown was never built (or its origin
          // advanced).  Force a per-URL agent render (skips the redundant perf
          // analysis; the render's store-time re-validation still guards staleness).
          browser_manager_->EnqueueAnalysis(
              notification.url, notification.hostname, notification.scheme,
              notification.site, notification.host_vouched,
              notification.from_module, notification.capability_mask,
              template_hash,
              /*original_html=*/html_content, agent_origin_html_hash,
              /*force_agent_render=*/true);
        }
      }

      // A markdown-* mode writes no optimized HTML variant, for any sender:
      // what runs above (the content hash the Markdown copy binds to, and the
      // analysis that renders it) is all this notification is for.  The raw
      // entry still gets its source-binding stamp (identical bytes, no
      // worker-processed flag), as an unchanged page does in full, so a
      // validator-less origin keeps its derived ETag.
      if (!HtmlModeWritesHtmlVariant(cfg->html_mode)) {
        stamp_unchanged_html();
        if (agent_origin_html_hash.has_value()) {
          RecordMarkdownModePass(notification, *agent_origin_html_hash);
        }
        LogInfo("html_mode %s: no HTML variant for %s",
                std::string(HtmlModeName(cfg->html_mode)).c_str(),
                notification.url.c_str());
        clear_tentative_cooldown();
        return;
      }

      std::string& combined_css = combined.css;
      const bool external_css_missing = combined.external_css_missing;
      const bool revalidatable_css_missing = combined.revalidatable_css_missing;

      // Extract critical CSS: prefer browser profile, fall back to
      // heuristic.
      std::string critical_css;
      int critical_rules = 0;
      int total_rules = 0;
      // Critical/total CSS byte coverage, used by BOTH coverage gates below.
      // <0 means "unmeasured" (the heuristic path derives it from byte sizes).
      // Unified 3-band coverage policy (low band = FOUC gate, high band
      // = Issue A budget): coverage < async_css_min_coverage -> keep external
      // <link> render-blocking, no inline block (FOUC); mid -> inline + async
      // defer; coverage >= ShouldInlineCriticalCss cap -> skip BOTH (below).
      float critical_css_coverage = -1.0f;
      // CriticalCssResult::anonymous_layers_dropped of whichever derivation
      // below produced the block; salts the record's binding
      // (ValidationBindingFor), as the validator's does.
      bool anonymous_layers_dropped = false;
      // Issue A (high band): the budget decision is computed at profile-selection
      // time (where the viewport's coverage/byte stats are in scope) but APPLIED
      // just before has_critical_css below — NOT by clearing critical_css here,
      // which sits before the heuristic fallback and would silently re-trigger
      // the heuristic extractor on the full sheet.
      bool skip_critical_css = false;
      // The viewport profile the critical block being inlined was actually
      // derived from. nullptr on the heuristic path, and nullptr again when a
      // profile was selected but derived nothing for this page (#1216) — the
      // record must only ever travel with its own block. Carries the empirical
      // validation record the deferral gate below requires.
      const ViewportProfile* validated_source_vp = nullptr;
      // Set when a profile WAS selected but its derivation produced no block,
      // so the record was dropped (see AsyncCssRecordForDerivedBlock). Only
      // steers the refusal message below: "there is no profile" and "the
      // profile's confirmation is not about these bytes" are different things
      // to go and look at.
      bool profile_derived_no_block = false;
      if (browser_profile &&
          cfg->browser_analysis.enable_browser_critical_css) {
        CapabilityMask m = CapabilityMask::Decode(notification.capability_mask);
        const ViewportProfile* vp = nullptr;
        switch (m.viewport()) {
          case CapabilityMask::Viewport::kMobile:
            vp = &browser_profile->mobile;
            break;
          case CapabilityMask::Viewport::kTablet:
            vp = &browser_profile->tablet;
            break;
          case CapabilityMask::Viewport::kDesktop:
            vp = &browser_profile->desktop;
            break;
        }
        if ((vp != nullptr) && !vp->critical_css.empty()) {
          // Issue #1056: the browser profile derives "critical" purely from
          // Chrome's FCP rule-usage over a static, JS-off, light-mode,
          // template-shared render, so it silently drops rules the fold DOES
          // use — state-conditional Tailwind v4 variants (`.dark\:X:where(...)`)
          // that never render under that sample, and classes that differ on a
          // structurally-similar sibling served the same shared profile. The
          // result is a nav/layout-collapse FOUC. Rather than inline that byte
          // range verbatim, ALWAYS re-derive the critical block by matching the
          // full combined sheet against THIS page's above-the-fold DOM, using
          // the profile's covered selectors as a force-include augmentation for
          // the matcher's own blind spots. One clean layered block replaces the
          // raw browser block; the profile's pre-extraction coverage no longer
          // describes what we inline.
          if (!combined_css.empty()) {
            // The SAME derivation the browser-side validation renders, so the
            // record it produced is about these exact bytes (see
            // DeriveDomMatchedCriticalCss).
            // The measured fold is ADDED to the extractor's "first N body
            // elements" estimate for this viewport, not substituted for it:
            // the extractor ORs the two, so the derived block is a superset of
            // what the estimate alone admits. A validation record is keyed on
            // the sheet's hash only, so a record written under a smaller
            // estimate keeps authorising deferral of this larger block until
            // the page is re-validated.
            CriticalCssResult css_result = DeriveDomMatchedCriticalCss(
                scan_result.elements, combined_css, vp->critical_css,
                m.viewport(), vp->above_fold_selectors, combined.layer_order);
            anonymous_layers_dropped = css_result.anonymous_layers_dropped;
            critical_css = std::move(css_result.critical_css);
            critical_rules = css_result.critical_rules;
            total_rules = css_result.total_rules;
          } else {
            // No combined sheet to match against (inline-only / nothing
            // gathered): fall back to the browser-extracted block verbatim.
            // The record is not about this raw blob either — the validator
            // never rendered it — and the binding below still passes it
            // through; harmless only because an empty sheet hashes to nothing
            // matchable, so deferral is refused by that mechanism instead (see
            // the #1216 review, finding 2).
            critical_css = vp->critical_css;
          }
          // Issue #1216: bind the validation record to the block it was
          // produced about, BEFORE anything else can take that block's place.
          // An empty derivation means the fold of THIS page matched nothing the
          // profile can speak for; the heuristic fallback further down then
          // substitutes its own block (or nothing is inlined at all), and the
          // record — validated bit set, stylesheet hash still matching, neither
          // of them about the served block — would otherwise authorize
          // deferring the sheet anyway.
          validated_source_vp = AsyncCssRecordForDerivedBlock(vp, critical_css);
          if (validated_source_vp == nullptr) {
            profile_derived_no_block = true;
            stats_.async_css_record_dropped_empty_derivation.fetch_add(
                1, std::memory_order_relaxed);
            LogInfo(
                "Dropping the browser profile's async-CSS validation record "
                "for %s: the DOM-matched derivation produced no critical block "
                "for this page, so the record does not describe what would be "
                "inlined",
                notification.url.c_str());
          }
          // Recompute coverage from the bytes we will ACTUALLY inline, BEFORE
          // the sufficiency gate. The profile's rule-level coverage was measured
          // pre-extraction against a different render and must not veto here.
          critical_css_coverage =
              combined_css.empty()
                  ? -1.0f
                  : static_cast<float>(critical_css.size()) /
                        static_cast<float>(combined_css.size());
          LogInfo(
              "Re-derived DOM-matched critical CSS for %s from browser profile "
              "(%zu bytes, profile coverage=%.2f)",
              notification.url.c_str(), critical_css.size(),
              vp->css_coverage_ratio);
          // Issue A budget (unchanged): a high-coverage / near-whole-sheet
          // profile must be neither inlined nor async-deferred — keep the
          // original render-blocking external <link>. Judged on the profile's
          // measured coverage, exactly as before.
          if (!browser_internal::ShouldInlineCriticalCss(
                  vp->css_coverage_ratio)) {
            skip_critical_css = true;
            stats_.critical_css_skipped_high_coverage.fetch_add(
                1, std::memory_order_relaxed);
            LogInfo(
                "Skipping browser-profile critical CSS for %s "
                "(coverage=%.2f, %zu/%zu bytes over budget) — keeping external "
                "render-blocking <link>",
                notification.url.c_str(), vp->css_coverage_ratio,
                vp->critical_css.size(), vp->total_css_bytes);
          }
        }
      }
      if (critical_css.empty() && !combined_css.empty()) {
        CapabilityMask notif_mask =
            CapabilityMask::Decode(notification.capability_mask);
        // The small-sheet threshold below which the coverage gate is not
        // applied, so rules kept for wide replaced elements are
        // bounded by the same gate this block is judged by.
        CriticalCssConfig css_config;
        css_config.inline_limit_min_sheet_bytes =
            cfg->async_css_min_deferred_bytes;
        CriticalCssExtractor extractor(std::move(css_config));
        // The page's layer order decides where the block goes, and so which
        // @media blocks it keeps, as on the profile path.
        CriticalCssResult css_result = extractor.Extract(
            scan_result.elements, combined_css, notif_mask.viewport(),
            /*force_include=*/nullptr, &combined.layer_order);
        anonymous_layers_dropped = css_result.anonymous_layers_dropped;
        if (css_result.success && !css_result.critical_css.empty()) {
          // The same coverage budget the profile path applies, on the bytes
          // actually derived: a block that is most of the sheet is not
          // inlined and the original render-blocking <link> is kept. The
          // estimate is document-order, so a long page with a small sheet can
          // otherwise derive nearly the whole sheet. (The byte cap is applied
          // further down, once the deferral decision is known.)
          //
          // Cold-cache pass, external_css_missing: combined_css is then the
          // page's INLINE CSS alone, and the block derived from it is close to
          // all of it. If that inline CSS is itself at least
          // async_css_min_deferred_bytes the ratio clears 0.60 and nothing is
          // inlined on this pass — the variant is marked for revalidation
          // once the sheet caches, and the real decision is made then.
          if (!browser_internal::ShouldInlineHeuristicCriticalCss(
                  css_result.critical_css.size(), combined_css.size(),
                  cfg->async_css_min_deferred_bytes)) {
            stats_.critical_css_skipped_high_coverage.fetch_add(
                1, std::memory_order_relaxed);
            LogInfo(
                "Skipping heuristic critical CSS for %s (%zu of %zu bytes over "
                "the inline budget) — keeping external render-blocking <link>",
                notification.url.c_str(), css_result.critical_css.size(),
                combined_css.size());
          } else {
            critical_css = std::move(css_result.critical_css);
            critical_rules = css_result.critical_rules;
            total_rules = css_result.total_rules;
          }
        }
      }

      // Issue A: apply the budget AFTER the heuristic fallback above,
      // so suppressing a near-whole-sheet browser profile does NOT re-trigger
      // the heuristic extractor (which would re-inline the full sheet).  The
      // flag is only set for the profile path; a heuristic (rule-counted)
      // result is left untouched.  has_critical_css then gates BOTH the inline
      // (HtmlTransformFilter::InjectCriticalCss) AND the async re-download
      // (has_async_css below), so clearing it here drops both — leaving the
      // original render-blocking external <link>.
      if (skip_critical_css) {
        critical_css.clear();
      }

      // Determine if any HTML transformations will apply.
      bool has_critical_css = !critical_css.empty();
      bool has_lazy_load = !cfg->disable_lazy_load;
      bool has_image_dims = !cfg->disable_image_dimensions;
      bool has_lcp_preload = !cfg->disable_lcp_preload;
      bool has_preconnect = !cfg->disable_preconnect_injection &&
                            !scan_result.third_party_origins.empty();
      // FOUC sufficiency gate: only DEFER the full stylesheet (async-CSS
      // rel="preload" swap) when the inlined critical CSS is substantial enough
      // to bridge first paint. A thin critical block (e.g. ~830 B of dark-color
      // overrides) deferring a large layout/font sheet renders the page unstyled
      // until the sheet loads. When insufficient we keep the stylesheet
      // render-blocking (no FOUC) and, below, drop the critical block: inlined
      // next to a render-blocking sheet it only duplicates bytes.
      AsyncCssSufficiencyConfig async_css_suff{
          cfg->async_css_min_coverage, cfg->async_css_min_deferred_bytes};
      // external_css_missing is the cold-cache fail-safe: when a declared
      // external <link> was not yet in cache, combined_css.size() (the deferred
      // byte denominator) is inline-only and the critical CSS was derived
      // without the sheet, so CriticalCssIsSufficient must refuse to defer.
      const bool async_css_bytes_sufficient = CriticalCssIsSufficient(
          critical_css_coverage, critical_css.size(), combined_css.size(),
          external_css_missing, async_css_suff);
      // Empirical gate: the byte floor above is a proxy and can clear while the
      // fold is still left unstyled, so it is necessary but not sufficient.
      const bool async_css_validated = AsyncCssValidatedForServedSheet(
          validated_source_vp, combined_css,
          ValidationBindingFor(
              combined.layer_order.ValidationBinding(combined_css),
              scan_result.noscript_affects_render, combined_css,
              CapabilityMask::Decode(notification.capability_mask).viewport(),
              anonymous_layers_dropped));
      // A validation record made against other stylesheet bytes (a redeploy,
      // or a change in how the bytes are assembled), or against
      // another cascade-layer order on a layered page (the binding above),
      // would keep
      // deferral off until the profile's TTL ran out. Ask for the template to
      // be validated again now instead (rate-limited per template). Not while
      // a declared sheet is missing from cache: those bytes are incomplete,
      // and a revalidation run on them would bind to the wrong sheet. Nor
      // while an @import the layer order needs is missing: the order would
      // bind as unproven and change once the import is cached.
      //
      // Only the page the record was made on may ask. The record's hash is of
      // THAT page's combined sheet, which includes its own inline <style>; a
      // sibling page of the same template with different inline CSS always
      // mismatches, and letting it ask would move the record from page to
      // page, burning the template's budget and taking deferral away from
      // the page that had it.
      if (browser_manager_ && browser_profile.has_value() &&
          IsAnalyzedPage(notification.url, browser_profile->analyzed_url) &&
          validated_source_vp != nullptr &&
          validated_source_vp->critical_css_validated &&
          !validated_source_vp->validated_combined_css_hash.empty() &&
          !async_css_validated && !external_css_missing &&
          !combined.layer_order.import_missing && !combined_css.empty()) {
        browser_manager_->RequestRevalidation(
            notification.url, notification.hostname, notification.scheme,
            notification.site, notification.host_vouched,
            notification.from_module, notification.capability_mask,
            template_hash,
            /*original_html=*/html_content, agent_origin_html_hash);
      }
      // Byte cap on the inlined block — only while the sheet stays
      // render-blocking. With a validated deferral the block REPLACES the
      // render-blocking fetch and the validator rendered exactly these bytes;
      // vetoing it would take away both the inline and the deferral a site
      // had (kInlineCriticalCssMaxBytes for the rationale).
      if (has_critical_css && !async_css_validated &&
          critical_css.size() > browser_internal::kInlineCriticalCssMaxBytes) {
        stats_.critical_css_skipped_byte_cap.fetch_add(
            1, std::memory_order_relaxed);
        LogInfo(
            "Skipping critical CSS for %s (%zu bytes over the %zu-byte inline "
            "cap with no validated deferral) — keeping external "
            "render-blocking <link>",
            notification.url.c_str(), critical_css.size(),
            browser_internal::kInlineCriticalCssMaxBytes);
        critical_css.clear();
        has_critical_css = false;
      }
      const bool async_css_sufficient =
          async_css_bytes_sufficient && async_css_validated;
      // Diagnostic override (--unsafe-force-async-css, CLI-only): defer even on
      // a refusal, so the probe harness can exercise the deferred markup while
      // the gate is correctly refusing. Scoped to the deferral decision alone.
      const bool async_css_deferral_allowed = AsyncCssDeferralAllowed(
          async_css_sufficient, cfg->unsafe_force_async_css);
      bool has_async_css = !cfg->disable_async_css && has_critical_css &&
                           async_css_deferral_allowed;
      if (has_critical_css && !cfg->disable_async_css &&
          !async_css_deferral_allowed) {
        if (external_css_missing) {
          // Cold-cache fail-safe path: NOT thin-critical low coverage. The
          // coverage ratio here would be computed off the inline-only
          // denominator and is meaningless, so do not pollute the
          // low-coverage counter/log with it.
          LogInfo(
              "Async-CSS suppressed for %s: an external stylesheet is not yet "
              "cached - keeping it render-blocking (no FOUC) until it caches"
              "%s",
              notification.url.c_str(),
              revalidatable_css_missing
                  ? "; the critical CSS block stays while that sheet is missing"
                  : "");
        } else if (!async_css_bytes_sufficient) {
          stats_.async_css_suppressed_low_coverage.fetch_add(
              1, std::memory_order_relaxed);
          // Log the ratio the gate actually used: min(byte ratio, profile
          // coverage when known). Logging the raw profile coverage here could
          // claim a value ABOVE the floor while the byte ratio vetoed (#879).
          float logged_ratio =
              combined_css.empty()
                  ? 0.0f
                  : static_cast<float>(critical_css.size()) /
                        static_cast<float>(combined_css.size());
          if (critical_css_coverage >= 0.0f &&
              critical_css_coverage < logged_ratio) {
            logged_ratio = critical_css_coverage;
          }
          LogInfo(
              "Async-CSS suppressed for %s: critical %zu B / sheet %zu B "
              "(effective coverage=%.4f < floor %.2f, profile coverage=%.4f) "
              "- keeping stylesheet render-blocking to avoid FOUC",
              notification.url.c_str(), critical_css.size(),
              combined_css.size(), logged_ratio, cfg->async_css_min_coverage,
              critical_css_coverage);
        } else {
          // The byte floor cleared; the empirical record did not. Distinct
          // counter and message from the low-coverage refusal, because the
          // operator action is different: this one is "the page has not been
          // validated (yet), or its stylesheet changed since it was", not
          // "the critical block is too thin".
          stats_.async_css_suppressed_unvalidated.fetch_add(
              1, std::memory_order_relaxed);
          const char* reason = "no browser profile for this page";
          if (profile_derived_no_block) {
            // There IS a profile, and it may well carry a healthy record — it
            // just is not about the block being inlined here (#1216).
            reason =
                "no critical block could be derived for this page, so its "
                "browser profile's confirmation is not about what is inlined";
          } else if (validated_source_vp != nullptr) {
            if (!validated_source_vp->critical_css_validated) {
              reason = "its browser profile carries no validation";
            } else if (validated_source_vp->validated_combined_css_hash
                           .empty()) {
              // Validated bit set with no stylesheet bound to it. Not a
              // redeploy — a record that was written wrong, which is a
              // different thing to go and look at.
              reason = "its validation record is incomplete";
            } else {
              reason = "the stylesheet changed since it was validated";
            }
          }
          LogInfo(
              "Async-CSS suppressed for %s: %s — keeping the stylesheet "
              "render-blocking until the above-the-fold appearance has been "
              "confirmed",
              notification.url.c_str(), reason);
        }
      }
      // The block is only worth its bytes when it replaces the render-blocking
      // fetch. While the full stylesheet stays render-blocking (no validated
      // record, or the sandbox is off) it would sit next to that fetch and
      // duplicate bytes with no paint benefit, so it is dropped and the page
      // keeps its original <link>. When the page is later validated, the next
      // variant write inlines the block and defers the sheet together, from the
      // same decision.
      //
      // Not while a same-origin sheet is still missing from cache: such a pass
      // is only written as a variant (flagged for revalidation, so it is
      // decided again once the sheet caches) if the transform changes the page,
      // and for a page whose only change is this block, dropping it would drop
      // the revalidation with it. The window ends when the sheet caches.
      //
      // Nor when a gathered sheet is applied by script or stands after body
      // content: first paint does not wait for it, so the block may be the only
      // styling the fold has.
      //
      // A same-origin sheet that never caches keeps the block for as long as it
      // is missing: the page stays flagged for revalidation and each pass
      // decides again.
      bool dropped_block = false;
      bool sheet_may_not_block_paint = false;
      for (const auto& sheet : scan_result.stylesheets) {
        if (sheet.may_not_block_paint) sheet_may_not_block_paint = true;
      }
      if (has_critical_css && !has_async_css && !revalidatable_css_missing &&
          !sheet_may_not_block_paint) {
        LogInfo(
            "Critical CSS not inlined for %s: the stylesheet stays "
            "render-blocking",
            notification.url.c_str());
        critical_css.clear();
        has_critical_css = false;
        dropped_block = true;
      }
      // A pass that dropped the block and changed nothing else is held for a
      // short while, so a front end that re-notifies on every hit does not
      // rerun the whole pipeline for a page that has nothing to change. The
      // hold is short, so a record that arrives later is picked up.
      auto finish_unchanged = [&]() {
        stamp_unchanged_html();
        if (!dropped_block) {
          clear_tentative_cooldown();
          return;
        }
        std::string ck =
            ComposeInternalKey(notification.url, notification.hostname,
                               notification.scheme, notification.site);
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
        text_cooldown_expiry_[ck] = CooldownEntry{
            now, now + std::chrono::seconds(kDroppedBlockCooldownSecs),
            CooldownReason::kRevalidation};
      };

      // Store preload hints for future Early Hints (103) responses.
      // These are stored at mask=0xFFFFFFFF (reserved, impossible in
      // normal use) so nginx can look them up on cache MISS.
      //
      // A deferred stylesheet IS preload-hinted.  The deferral primitive is
      // itself `rel=preload as=style`, so a 103 hint announces exactly what
      // the transform announces -- the same request, started earlier.  (This
      // is the inverse of the rule that stood while the primitive was a
      // low-priority media trick, where a hint would have undone the
      // transform's own demotion.)  Print sheets are still never hinted: they
      // are not render-blocking for screen, so the priority would be spent for
      // nothing.
      //
      // Accepted cost: at `as=style` the sheet competes with the LCP image
      // hint below for early bandwidth.  That is the trade -- the full
      // stylesheet arriving sooner, against an LCP image that may share the
      // link -- and it is the point of the change, not an oversight.
      {
        // Sanitize hint URLs: strip CR/LF to prevent header injection.
        // Early Hints are newline-delimited, so embedded newlines would
        // create spurious entries.
        auto sanitize_hint_url = [](std::string_view url) -> std::string {
          std::string result;
          result.reserve(url.size());
          for (char c : url) {
            if (c != '\r' && c != '\n') result += c;
          }
          return result;
        };

        std::string hints;
        size_t hint_count = 0;
        // Resolved as the combined-stylesheet gather resolves them (against
        // the document base), so the hint names the sheet the
        // browser fetches.
        const DocumentBase hints_base =
            DocumentBaseOf(notification.url, scan_result, notification.hostname,
                           notification.scheme);
        for (const auto& stylesheet : scan_result.stylesheets) {
          if (stylesheet.href.empty()) continue;
          // A print sheet is never render-blocking for screen — hinting it
          // would only burn bandwidth priority.
          if (StylesheetMediaIsPrint(stylesheet.media)) continue;
          if (!hints.empty()) hints += '\n';
          hints += sanitize_hint_url(
              ResolveAgainstBase(hints_base, stylesheet.href));
          ++hint_count;
        }
        // Add LCP image preload hint if detected.  An <img> inside <picture>
        // is skipped: a <source> sibling may win selection and the preload
        // would double-download (mirrors InjectLcpPreload).
        if (!scan_result.lcp_candidate.src.empty() &&
            !scan_result.lcp_candidate.in_picture) {
          // Validate URL scheme (same as InjectLcpPreload).
          std::string_view src = scan_result.lcp_candidate.src;
          if (src.starts_with("https://") || src.starts_with("http://") ||
              src.starts_with("//") || src.starts_with("/")) {
            if (!hints.empty()) hints += '\n';
            hints += "image:";
            hints += sanitize_hint_url(scan_result.lcp_candidate.src);
            ++hint_count;
          }
        }
        // Add preconnect origins for third-party resources.  CORS-mode
        // origins carry a distinct prefix so the 103 path emits the SAME
        // crossorigin-ness as the injected <link rel="preconnect"> — the two
        // paths must warm the same connection pool.
        if (!cfg->disable_preconnect_injection) {
          for (const auto& entry : scan_result.third_party_origins) {
            if (!hints.empty()) hints += '\n';
            hints += entry.crossorigin ? "preconnect-cors:" : "preconnect:";
            hints += sanitize_hint_url(entry.origin);
            ++hint_count;
          }
        }
        if (!hints.empty()) {
          auto wh = cache_->WriteSentinel(
              notification.url, notification.hostname, notification.scheme,
              SentinelId::kEarlyHints, hints.size(), notification.site);
          if (wh) {
            // Early hints are best-effort; write failure is non-fatal.
            (void)wh->write_sync(
                std::as_bytes(std::span(hints.data(), hints.size())));
            (void)wh->close_sync();
          }
          LogInfo("Stored %zu preload hints for %s", hint_count,
                  notification.url.c_str());
        } else {
          // Reprocessing can legitimately drop every hint (the page's only
          // stylesheet becomes print-only, its LCP image goes away, a
          // third-party origin is dropped).  A sentinel from an earlier pass
          // would otherwise survive and keep the 103 path promoting resources
          // the page no longer wants, so remove it — a missing sentinel reads
          // as "no hints".  Best-effort like the write: failure (including
          // "never existed") is non-fatal.
          (void)cache_->RemoveAlternate(
              notification.url, notification.hostname, notification.scheme,
              static_cast<AlternateId>(SentinelId::kEarlyHints),
              notification.site);
        }
      }

      // Script deferral: use browser analysis to identify safe-to-defer scripts.
      std::vector<std::string> defer_scripts;
      if (browser_profile) {
        defer_scripts = browser_profile->defer_safe_scripts;
      }
      bool has_script_deferral =
          !cfg->disable_script_deferral && !defer_scripts.empty();

      // Speculation rules: get hot URLs for this hostname.
      std::vector<std::string> speculation_urls;
      bool has_speculation = false;
      if (cfg->enable_speculation_rules) {
        // Extract hostname from the notification URL for filtering.
        speculation_urls = GetHotUrls(notification.hostname, 10);
        has_speculation = !speculation_urls.empty();
      }

      if (!has_critical_css && !has_lazy_load && !has_image_dims &&
          !has_lcp_preload && !has_preconnect && !has_speculation &&
          !has_async_css && !has_script_deferral &&
          !worker_processed_identity) {
        stats_.html_assembly_skipped.fetch_add(1, std::memory_order_relaxed);
        LogInfo("No HTML transformations applicable for %s%s",
                notification.url.c_str(),
                dropped_block ? " (the critical CSS block was dropped)" : "");
        finish_unchanged();
        return;
      }

      // Issue E: wire the policy.* dashboard counters at the REAL decision
      // site.  Semantics (documented in src/worker/CLAUDE.md): policy_computed
      // counts "a policy decision was made", i.e. a transform pass is about to
      // run (past the no-transformation early-return above), independent of
      // whether the pass nets a change at modified() below.  The two sub-policy
      // counters count when their feature was selected for the transform.  This
      // is observability only — the actual gate is the transform_config flags,
      // not OptimizationPolicy::Compute (telemetry-only; not revived).
      stats_.policy_computed.fetch_add(1, std::memory_order_relaxed);
      if (has_async_css) {
        stats_.policy_async_css_enabled.fetch_add(1, std::memory_order_relaxed);
      }
      if (has_script_deferral) {
        stats_.policy_script_deferral_enabled.fetch_add(
            1, std::memory_order_relaxed);
      }

      // Second HtmlParse pass: apply all transformations via
      // HtmlTransformFilter + HtmlWriterFilter.
      net_instaweb::HtmlKeywords::Init();
      net_instaweb::NullMessageHandler message_handler;
      net_instaweb::HtmlParse transform_parser(&message_handler);

      HtmlTransformConfig transform_config;
      transform_config.enable_critical_css = has_critical_css;
      transform_config.enable_lazy_load = has_lazy_load;
      transform_config.enable_image_dimensions = has_image_dims;
      transform_config.enable_lcp_preload = has_lcp_preload;
      transform_config.enable_preconnect_injection = has_preconnect;
      transform_config.enable_speculation_rules = has_speculation;
      transform_config.enable_async_css = has_async_css;
      transform_config.enable_script_deferral = has_script_deferral;

      HtmlTransformFilter transform_filter(
          &transform_parser, transform_config, critical_css, cache_.get(),
          notification.hostname, notification.scheme, scan_result.lcp_candidate,
          scan_result.third_party_origins, speculation_urls, defer_scripts);
      transform_filter.set_cascade_layer_order(combined.layer_order);
      // The page is text/html only when the origin said so: an XHTML page
      // (application/xhtml+xml, also filed under kHtml) or one stored without
      // a Content-Type gets no inline critical CSS.
      transform_filter.set_document_is_text_html(
          IsTextHtmlMediaType(html_read_result->metadata.origin_content_type));
      transform_filter.set_site(notification.site);
      transform_filter.set_subresource_gate(
          subresource_gate_.get(),
          MayRequestHostFetches(notification) && notification.host_vouched);
      // The origin's header CSP (headers sidecar) gates inline injections
      // like a <meta> CSP does. No sidecar (older entries) -> no change.
      transform_filter.set_header_csps(HtmlTransformFilter::ReadHeaderCsps(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site));
      transform_parser.AddFilter(&transform_filter);

      std::string output_html;
      net_instaweb::StringWriter string_writer(&output_html);
      net_instaweb::HtmlWriterFilter writer_filter(&transform_parser);
      writer_filter.set_writer(&string_writer);
      transform_parser.AddFilter(&writer_filter);

      if (!transform_parser.StartParse(notification.url)) {
        stats_.critical_css_aborted.fetch_add(1, std::memory_order_relaxed);
        LogWarning("HTML transform parse failed for %s",
                   notification.url.c_str());
        clear_tentative_cooldown();
        return;
      }

      transform_parser.ParseText(html_content);
      transform_parser.FinishParse();

      if (!transform_filter.modified()) {
        stats_.html_assembly_skipped.fetch_add(1, std::memory_order_relaxed);
        LogInfo("HTML transform produced no changes for %s%s",
                notification.url.c_str(),
                dropped_block ? " (the critical CSS block was dropped)" : "");
        finish_unchanged();
        return;
      }

      // Write the modified HTML as a variant at the notification
      // mask.  If external CSS was missing, mark the variant for
      // revalidation so nginx re-notifies once CSS is cached.
      CapabilityMask html_mask =
          CapabilityMask::Decode(notification.capability_mask);
      // Strip encoding and image-format bits: HTML doesn't vary by
      // image format, and the identity variant needs a distinct
      // AlternateId from gzip/brotli compressed variants.
      html_mask.set_transfer_encoding(
          CapabilityMask::TransferEncoding::kIdentity);
      html_mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
      uint8_t html_flags = AlternateMetadata::kFlagWorkerProcessed;
      // Revalidate when a SAME-ORIGIN external sheet was missing. The critical
      // CSS was derived from INCOMPLETE input (the cold sheet was not gathered)
      // and the async-CSS decision was forced render-blocking by the cold-cache
      // fail-safe above — both must be recomputed once the sheet caches. This
      // previously gated on !critical_css_injected(), but a degenerate inline
      // echo (e.g. a dark-mode override block) DOES inject critical CSS, which
      // wrongly suppressed revalidation and made the cold, unstyled result
      // sticky. We use the same-origin-scoped flag (not external_css_missing)
      // so a cross-origin sheet that can never cache (e.g. Google Fonts) does
      // not re-notify forever; async-CSS stays suppressed for it either way.
      // Re-notify is request-driven and stops once the sheet caches, so this
      // converges.
      if (revalidatable_css_missing) {
        html_flags |= AlternateMetadata::kFlagNeedsRevalidation;
      }
      AlternateMetadata html_variant_meta =
          CreateWorkerMetadata(html_read_result->metadata, ContentType::kHtml,
                               html_flags, html_content.size());
      // Issue B (durable raw-origin hash): persist the TRUE raw-origin hash on
      // the optimized identity so that the NEXT notification — which reads this
      // worker-processed slot, not raw — reuses it instead of re-hashing
      // optimized bytes.  Without this the identity inherits the nginx-written
      // raw original's all-zero hash and the binding oracle is lost after the
      // first optimize.
      //
      // Source binding (issue #434): the agent branch stamps the hash it
      // derived (raw read or durable reuse).  With agent_optimize off the
      // raw read is hashed here instead, so every HTML variant carries a
      // tier-1 identity for the derived ETag.  A worker-processed read
      // (agent or not) keeps the inherited stamp: never hash the worker's
      // own output into the binding.
      if (agent_origin_html_hash.has_value()) {
        html_variant_meta.origin_html_hash = *agent_origin_html_hash;
      } else if (!worker_processed_identity) {
        StampSourceBinding(
            &html_variant_meta,
            cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(html_content.data()),
                html_content.size())));
      }

      // The origin borrow was already de-aliased (copied + released) right
      // after the size cap above, so the target AlternateId (0x08 for Desktop
      // notifications, the same slot that held the original) is no longer
      // pinned by an open mmap read handle and Cyclone can overwrite it.
      stats_.alternate_writes.fetch_add(1, std::memory_order_relaxed);
      bool html_write_ok = WriteVariant(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site, html_mask,
          std::span<const char>(output_html.data(), output_html.size()),
          html_variant_meta, purge_check, this);

      if (html_write_ok) {
        stats_.variants_written.fetch_add(1, std::memory_order_relaxed);
        stats_.html_processed.fetch_add(1, std::memory_order_relaxed);
        stats_.html_assembly_complete.fetch_add(1, std::memory_order_relaxed);
        LogInfo(
            "Wrote HTML variant for %s "
            "(%zu -> %zu bytes, %d/%d critical CSS rules)",
            notification.url.c_str(), html_content.size(), output_html.size(),
            critical_rules, total_rules);
        // Write pre-compressed (gzip/brotli) variants of the HTML.
        WriteCompressedVariants(
            cache_.get(), notification.url, notification.hostname,
            notification.scheme, notification.site, html_mask, output_html,
            html_content, html_variant_meta, cfg->gzip_level, cfg->brotli_level,
            stats_, purge_check, this);
        // Mark as processed for dedup (skip if revalidation-flagged).
        // MarkVariantProcessed acquires processed_set_mutex_, so call
        // it OUTSIDE write_cooldown_mutex_ to avoid nested locking.
        if ((html_flags & AlternateMetadata::kFlagNeedsRevalidation) == 0) {
          MarkVariantProcessed(notification.url, notification.hostname,
                               notification.scheme, notification.site,
                               MaskToId(html_mask), purge_gen);
        }
        {
          std::string ck =
              ComposeInternalKey(notification.url, notification.hostname,
                                 notification.scheme, notification.site);
          std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
          if ((html_flags & AlternateMetadata::kFlagNeedsRevalidation) == 0) {
            // Clear cooldown: variant is fully processed.
            text_cooldown_expiry_.erase(ck);
          } else {
            // Revalidation write: set a SHORT cooldown (3s) to throttle
            // the re-notification loop while allowing CSS convergence.
            auto reval_now = std::chrono::steady_clock::now();
            text_cooldown_expiry_[ck] = CooldownEntry{
                reval_now,
                reval_now + std::chrono::seconds(kRevalidationCooldownSecs),
                CooldownReason::kRevalidation};
          }
        }
      } else if (purge_check()) {
        // Issue E: BENIGN purge-fence — a purge advanced this URL's generation
        // after the notification was dispatched (e.g. the content-hash
        // invalidation purge earlier in THIS same notification), so the write
        // was correctly dropped.  This is NOT a hard failure; counting it as
        // alternate_write_failures + errors produced the live dashboard lockstep
        // (write_failures==content_hash_stale==errors) that masked real
        // failures.  Count it distinctly and do NOT touch errors.
        stats_.alternate_writes_fenced.fetch_add(1, std::memory_order_relaxed);
        LogInfo("HTML variant write fenced for %s (purged after dispatch)",
                notification.url.c_str());
        SetWriteFailureCooldown(notification.url, notification.hostname,
                                notification.scheme, notification.site);
      } else {
        // Distinguish reader-contention (variant exists but can't be
        // overwritten) from actual write failures.
        if (VariantExistsForMask(cache_.get(), notification.url,
                                 notification.hostname, notification.scheme,
                                 notification.site, html_mask)) {
          LogInfo("HTML variant write skipped for %s (concurrent reader)",
                  notification.url.c_str());
        } else {
          stats_.errors.fetch_add(1, std::memory_order_relaxed);
          stats_.alternate_write_failures.fetch_add(1,
                                                    std::memory_order_relaxed);
          LogWarning("Failed to write HTML variant for %s",
                     notification.url.c_str());
        }
        // Record cooldown to avoid retrying on every notification.
        SetWriteFailureCooldown(notification.url, notification.hostname,
                                notification.scheme, notification.site);
      }
      break;
    }

    case ContentType::kImage: {
      if (cfg->disable_image) {
        LogInfo("Image processing disabled, skipping %s",
                notification.url.c_str());
        return;
      }

      if (!cache_ || !image_transcoder_) {
        LogWarning(
            "Cache or transcoder not initialized, skipping image "
            "processing");
        return;
      }

      // Record every format of a slot as decided by this pass: the image
      // dedup key carries the format (ImageDedupMask), and each exit below
      // has decided all of kImageSlotFormats for the slot it marks.
      auto mark_image_slot_decided = [&](CapabilityMask slot) {
        slot.set_transfer_encoding(CapabilityMask::TransferEncoding::kIdentity);
        for (auto fmt : kImageSlotFormats) {
          slot.set_image_format(fmt);
          MarkVariantProcessed(notification.url, notification.hostname,
                               notification.scheme, notification.site,
                               MaskToId(slot), purge_gen);
        }
      };

      // Read original image from cache (with exponential-backoff retry).
      auto img_read_result = ReadOriginalWithRetry(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site, stats_, handler_);

      if (!img_read_result.has_value()) {
        LogWarning(
            "Original image not found in cache for %s (error=%s, "
            "notif_mask=0x%08x)",
            notification.url.c_str(),
            make_error_code(img_read_result.error()).message().c_str(),
            notification.capability_mask);
        return;
      }

      // Skip raster processing for origin SVG files — they're already
      // vector format. Still write gzip/brotli compressed variants.
      std::string_view origin_ct =
          img_read_result->metadata.origin_content_type;
      if (origin_ct.starts_with("image/svg+xml")) {
        LogInfo(
            "Skipping native SVG (already vector), writing "
            "compressed variants: %s",
            notification.url.c_str());
        auto svg_span = img_read_result->content();
        // De-alias the origin borrow (issue #934): own the bytes and release
        // the handle before compressing, so gzip+brotli don't pin the stripe
        // we then write the compressed variants into (self-starvation at
        // cache-full).  Matches the image-transcode de-alias below.
        const std::string svg_data(
            reinterpret_cast<const char*>(svg_span.data()), svg_span.size());
        AlternateMetadata svg_origin_meta = img_read_result->metadata;
        const AlternateMetadata svg_read_meta = svg_origin_meta;
        // Only a front end's own recording may seed the change oracle.
        const bool svg_source_genuine =
            (svg_origin_meta.flags & AlternateMetadata::kFlagWorkerProcessed) ==
            0;
        // Verify the copy is untorn before releasing (issue #934):
        // a wrap during the copy would feed torn SVG bytes into the compressed
        // variants.  On failure, skip — a later notification re-reads a
        // settled slot.
        if (!BorrowCopyUntorn(*img_read_result,
                              cfg->read_lease_duration_ms > 0)) {
          stats_.read_borrow_wrap_discards.fetch_add(1,
                                                     std::memory_order_relaxed);
          LogWarning(
              "Origin SVG borrow wrapped during de-alias copy, skipping %s",
              notification.url.c_str());
          return;
        }
        img_read_result->release();
        // Source binding (issue #434): SHA-256 of the raw SVG bytes stamps
        // every compressed variant written below; a worker-processed input
        // keeps the inherited stamp.  The same hash seeds the change oracle.
        const std::array<std::byte, 32> svg_hash =
            svg_source_genuine
                ? cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                      reinterpret_cast<const std::byte*>(svg_data.data()),
                      svg_data.size()))
                : std::array<std::byte, 32>{};
        if (svg_source_genuine) {
          StampSourceBinding(&svg_origin_meta, svg_hash);
        }
        // The identity entry (the front end's own bytes) gets the same
        // binding, so a request without compression is validated too.
        if (svg_source_genuine) {
          CountStamp(stats_,
                     StampRawEntry(cache_.get(), notification.url,
                                   notification.hostname, notification.scheme,
                                   notification.site, svg_read_meta, svg_data,
                                   svg_hash, purge_check, this));
        }
        svg_origin_meta.content_type = ContentType::kImage;
        svg_origin_meta.flags = AlternateMetadata::kFlagWorkerProcessed |
                                InheritedVariantFlags(svg_origin_meta);
        svg_origin_meta.origin_content_length = static_cast<uint32_t>(
            std::min(svg_data.size(), size_t{UINT32_MAX}));
        // origin_content_type inherited from img_read_result->metadata
        WriteCompressedVariants(cache_.get(), notification.url,
                                notification.hostname, notification.scheme,
                                notification.site, CapabilityMask(), svg_data,
                                svg_data, svg_origin_meta, cfg->gzip_level,
                                cfg->brotli_level, stats_, purge_check, this);
        RecordSourceHash(cache_.get(), notification.url, notification.hostname,
                         notification.scheme, notification.site,
                         CapabilityMask(), svg_hash, svg_source_genuine,
                         cfg->gzip_level, cfg->brotli_level, purge_check);
        mark_image_slot_decided(
            CapabilityMask::Decode(notification.capability_mask));
        return;
      }

      // Skip image types that our decoders don't support (ICO, BMP, TIFF,
      // etc.).  Only JPEG, PNG, GIF, and WebP can be decoded for
      // transcoding/SVG candidacy.  Write compressed variants for the
      // original content so nginx can serve pre-compressed responses.
      if (!origin_ct.empty() && !origin_ct.starts_with("image/jpeg") &&
          !origin_ct.starts_with("image/png") &&
          !origin_ct.starts_with("image/gif") &&
          !origin_ct.starts_with("image/webp")) {
        // Check if the compressed variants already exist (dedup for
        // unsupported types like favicon.ico that can't be optimized).  Only
        // the encodings the configuration enables are expected (a level of 0
        // disables that encoding, so its copy never exists), the same rule
        // RecordSourceHash applies.
        CapabilityMask gz_check;
        gz_check.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
        CapabilityMask br_check;
        br_check.set_transfer_encoding(
            CapabilityMask::TransferEncoding::kBrotli);
        const bool coded_copies_stored =
            (cfg->gzip_level > 0 || cfg->brotli_level > 0) &&
            (cfg->gzip_level <= 0 ||
             VariantExistsForMask(cache_.get(), notification.url,
                                  notification.hostname, notification.scheme,
                                  notification.site, gz_check)) &&
            (cfg->brotli_level <= 0 ||
             VariantExistsForMask(cache_.get(), notification.url,
                                  notification.hostname, notification.scheme,
                                  notification.site, br_check));
        if (coded_copies_stored) {
          // The identity stamp must not depend on the compressed copies
          // being absent: a passthrough image whose copies were written
          // before the stamp existed still needs its content identity.
          img_read_result->release();
          CountStamp(stats_,
                     StampRawIdentityEntry(
                         cache_.get(), notification.url, notification.hostname,
                         notification.scheme, notification.site,
                         cfg->read_lease_duration_ms > 0, purge_check, this));
          LogInfo(
              "Compressed variants already exist for unsupported "
              "type %.*s, skipping: %s",
              static_cast<int>(origin_ct.size()), origin_ct.data(),
              notification.url.c_str());
          mark_image_slot_decided(
              CapabilityMask::Decode(notification.capability_mask));
          return;
        }
        LogInfo(
            "Skipping unsupported image type %.*s, writing "
            "compressed variants: %s",
            static_cast<int>(origin_ct.size()), origin_ct.data(),
            notification.url.c_str());
        auto span = img_read_result->content();
        // De-alias the origin borrow (issue #934): own the bytes and release
        // the handle before compressing (see the SVG branch above).
        const std::string data(reinterpret_cast<const char*>(span.data()),
                               span.size());
        AlternateMetadata meta = img_read_result->metadata;
        const AlternateMetadata read_meta = meta;
        const bool source_genuine =
            (meta.flags & AlternateMetadata::kFlagWorkerProcessed) == 0;
        // Verify the copy is untorn before releasing (issue #934);
        // see the SVG branch above.  On failure, skip.
        if (!BorrowCopyUntorn(*img_read_result,
                              cfg->read_lease_duration_ms > 0)) {
          stats_.read_borrow_wrap_discards.fetch_add(1,
                                                     std::memory_order_relaxed);
          LogWarning(
              "Origin image (unsupported type) borrow wrapped during de-alias "
              "copy, skipping %s",
              notification.url.c_str());
          return;
        }
        img_read_result->release();
        // Source binding (issue #434): same rule as the SVG branch: stamp
        // the compressed variants with the SHA-256 of the raw bytes when the
        // input is genuine, keep the inherited stamp otherwise.
        const std::array<std::byte, 32> passthrough_hash =
            source_genuine
                ? cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                      reinterpret_cast<const std::byte*>(data.data()),
                      data.size()))
                : std::array<std::byte, 32>{};
        if (source_genuine) {
          StampSourceBinding(&meta, passthrough_hash);
        }
        if (source_genuine) {
          CountStamp(stats_,
                     StampRawEntry(cache_.get(), notification.url,
                                   notification.hostname, notification.scheme,
                                   notification.site, read_meta, data,
                                   passthrough_hash, purge_check, this));
        }
        meta.content_type = ContentType::kImage;
        meta.flags = AlternateMetadata::kFlagWorkerProcessed |
                     InheritedVariantFlags(meta);
        meta.origin_content_length =
            static_cast<uint32_t>(std::min(data.size(), size_t{UINT32_MAX}));
        WriteCompressedVariants(cache_.get(), notification.url,
                                notification.hostname, notification.scheme,
                                notification.site, CapabilityMask(), data, data,
                                meta, cfg->gzip_level, cfg->brotli_level,
                                stats_, purge_check, this);
        RecordSourceHash(cache_.get(), notification.url, notification.hostname,
                         notification.scheme, notification.site,
                         CapabilityMask(), passthrough_hash, source_genuine,
                         cfg->gzip_level, cfg->brotli_level, purge_check);
        mark_image_slot_decided(
            CapabilityMask::Decode(notification.capability_mask));
        return;
      }

      auto img_content_span = img_read_result->content();
      std::string_view image_data(
          reinterpret_cast<const char*>(img_content_span.data()),
          img_content_span.size());

      if (image_data.size() > cfg->max_image_size) {
        LogWarning("Image too large (%zu > %zu), skipping %s",
                   image_data.size(), cfg->max_image_size,
                   notification.url.c_str());
        mark_image_slot_decided(
            CapabilityMask::Decode(notification.capability_mask));
        return;
      }

      // De-alias the origin borrow (issue #934): copy the origin bytes out
      // of the cache-backed mmap and release the ReadHandle BEFORE the
      // decode/transcode/write phase.  Holding (and renewing) the borrow
      // across the variant matrix kept this stripe's read lease
      // permanently fresh, so at cache-full the worker's own variant
      // writes into the same stripe were deferred by its own lease
      // (self-starvation; the lease has no release path, and each re-stamp
      // pushed expiry out by T).  One bounded copy (<= max_image_size)
      // trades transient memory for never pinning the stripe being
      // written to; the already-stamped read lease simply expires.
      std::string origin_bytes(image_data);
      AlternateMetadata origin_meta = img_read_result->metadata;
      // Verify the copy is untorn before releasing (issue #934): a
      // wrap during the copy would feed torn bytes into decode/transcode and
      // the worker would persist a corrupt variant.  On failure, skip — a
      // later notification re-reads a settled slot.
      if (!BorrowCopyUntorn(*img_read_result,
                            cfg->read_lease_duration_ms > 0)) {
        stats_.read_borrow_wrap_discards.fetch_add(1,
                                                   std::memory_order_relaxed);
        LogWarning(
            "Origin image borrow wrapped during de-alias copy, skipping %s",
            notification.url.c_str());
        return;
      }
      img_read_result->release();
      image_data = origin_bytes;

      CapabilityMask target_mask =
          CapabilityMask::Decode(notification.capability_mask);

      // ---------------------------------------------------------------
      // Cross-notification content hash: compute SHA-256 of origin
      // content and compare with stored kContentHash sentinel.  If
      // unchanged and non-sentinel alternates exist, skip all decode +
      // transcode work.  If changed (stale), clear processed set entries
      // so the full variant matrix is regenerated.
      //
      // Issue #1503: the hash MUST be taken over the pristine origin
      // reference, not over the selector's source slot.  After the first
      // pass that slot (the identity/original-format slot) holds the
      // worker's OWN recompressed variant (kFlagWorkerProcessed), so
      // hashing it both false-fires "changed" against an oracle stamped
      // over raw origin bytes and — on a genuine origin change observed
      // through a re-recorded durable original — would rebuild the matrix
      // from the superseded derived bytes, never propagating the change.
      // ReadPristineOrigin answers with the durable original (exact id)
      // or a genuine flag-clear identity, or nothing (the legacy
      // selector-source hash is the fallback then, unchanged).
      // ---------------------------------------------------------------
      PristineOriginRead pristine = ReadPristineOrigin(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site,
          cfg->read_lease_duration_ms > 0, stats_, handler_);
      // An oversize reference is refused as a processing source below, so
      // it must not drive detection either (its hash would bind the oracle
      // to bytes this branch never processes).
      const bool have_pristine =
          pristine.found && pristine.bytes.size() <= cfg->max_image_size;
      auto origin_hash =
          have_pristine
              ? pristine.hash
              : cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(image_data.data()),
                    image_data.size()));
      {
        bool content_changed = true;
        auto stored = cache_->ReadAlternate(
            notification.url, notification.hostname, notification.scheme,
            static_cast<AlternateId>(SentinelId::kContentHash),
            notification.site);
        if (stored.has_value()) {
          auto sc = stored->content();
          if (sc.size() == 32 &&
              std::memcmp(sc.data(), origin_hash.data(), 32) == 0) {
            content_changed = false;
          }
        }

        if (!content_changed) {
          // Origin unchanged — check if the requested viewport/density/
          // save-data combination already has all format variants.
          // Don't short-circuit when only other combinations exist
          // (e.g., Desktop variants exist but Mobile was requested).
          auto alts =
              cache_->ListAlternates(notification.url, notification.hostname,
                                     notification.scheme, notification.site);
          std::unordered_set<uint8_t> existing_ids_dedup;
          if (alts.has_value()) {
            for (const auto& alt : *alts) {
              existing_ids_dedup.insert(static_cast<uint8_t>(alt.id));
            }
          }

          // A raw identity slot is not the desktop original-format variant
          // (IdentityHoldsRawOriginal): it still has to be built.
          if (IdentityHoldsRawOriginal(
                  cache_.get(), notification.url, notification.hostname,
                  notification.scheme, notification.site, existing_ids_dedup)) {
            existing_ids_dedup.erase(
                static_cast<uint8_t>(MaskToId(CapabilityMask())));
          }

          // Check if all formats for this viewport/density/save-data exist.
          bool all_requested_exist = true;
          const CapabilityMask::ImageFormat check_formats[] = {
              CapabilityMask::ImageFormat::kWebP,
              CapabilityMask::ImageFormat::kAvif,
              CapabilityMask::ImageFormat::kOriginal,
          };
          for (auto fmt : check_formats) {
            CapabilityMask check_mask = target_mask;
            check_mask.set_image_format(fmt);
            check_mask.set_transfer_encoding(
                CapabilityMask::TransferEncoding::kIdentity);
            auto check_id = static_cast<uint8_t>(
                MaskToAlternateId(check_mask.Encode() & 0xFF));
            if (!existing_ids_dedup.count(check_id)) {
              all_requested_exist = false;
              break;
            }
          }

          if (all_requested_exist) {
            stats_.content_hash_hits.fetch_add(1, std::memory_order_relaxed);
            // Issue #1503: the content is unchanged, but the variant set's
            // freshness stamps may be stale — a front end that never
            // serves stale re-records the durable original on every
            // age-expired fall-through, and this notification is that
            // re-record's.  Adopt the fresh reference's origin state into
            // every variant, so the set serves fresh for another origin
            // lifetime instead of paying a re-fetch per request.  Skipped
            // when the reference forbids shared storage — a restamp must
            // never extend the life of content the origin recalled.
            if (have_pristine && CanRestampVariantSet(pristine.meta)) {
              PurgeCheck restamp_fence = [this, url = notification.url,
                                          hostname = notification.hostname,
                                          scheme = notification.scheme,
                                          site = notification.site,
                                          purge_gen]() {
                return WasPurgedSinceDispatch(url, hostname, purge_gen, scheme,
                                              site);
              };
              const size_t restamped = RestampVariantSetFreshness(
                  cache_.get(), notification.url, notification.hostname,
                  notification.scheme, notification.site, pristine.meta,
                  pristine.id, restamp_fence);
              if (restamped > 0) {
                LogInfo(
                    "Content hash unchanged for %s; restamped %zu "
                    "variant(s) from the fresh origin reference",
                    notification.url.c_str(), restamped);
              }
            } else if (have_pristine) {
              // The fresh reference forbids shared storage or now negotiates
              // on Accept: the set built before that must not be carried
              // forward, so drop the derived copies instead of restamping.
              PurgeDerivedCopiesAndBumpGeneration(
                  notification.url, notification.hostname, notification.scheme,
                  notification.site);
              ClearUrlProcessingState(notification.url, notification.hostname,
                                      notification.scheme, notification.site);
              LogInfo(
                  "Content hash unchanged for %s but the fresh origin "
                  "reference may not be restamped; derived copies purged",
                  notification.url.c_str());
              return;
            }
            LogInfo(
                "Content hash unchanged, all requested variants exist, "
                "skipping: %s",
                notification.url.c_str());
            mark_image_slot_decided(target_mask);
            return;
          }
        }

        if (content_changed && stored.has_value()) {
          // Origin content changed since last processing — existing
          // alternates are stale.  Remove them from cache and clear
          // the processed_set so the full variant matrix is regenerated.
          stats_.content_hash_stale.fetch_add(1, std::memory_order_relaxed);
          LogInfo("Content hash changed (stale), forcing re-process: %s",
                  notification.url.c_str());
          // Remove stale alternates and bump the purge generation
          // atomically so in-flight dedup entries and variant writes are
          // rejected (issue #652).
          (void)PurgeUrlAndBumpGeneration(
              notification.url, notification.hostname, notification.scheme,
              notification.site);
          {
            std::lock_guard<std::mutex> lock(processed_set_mutex_);
            std::string prefix =
                absl::StrCat(notification.url, "|", notification.hostname, "|",
                             notification.scheme, "|");
            EraseUrlMarksLocked(prefix);
          }
        }
      }

      // Issue #1503: process from the pristine origin reference when one
      // was identified — the selector's source slot may hold the worker's
      // own recompressed variant, which is a generation-losing source on
      // an unchanged origin and a STALE one after a genuine change (the
      // purge above ran precisely because the reference diverged).
      if (have_pristine) {
        origin_bytes = std::move(pristine.bytes);
        origin_meta = pristine.meta;
        image_data = origin_bytes;
      }
      // Source binding (issue #434): every variant of this pass (the raster
      // matrix, the SVG vectorization) derives from origin_meta, so one
      // stamp here binds them all.  origin_hash is SHA-256 of the pristine
      // reference when one was found (origin_meta is then the reference's
      // flag-clear metadata) and of the read slot otherwise: stamp only in
      // the raw case; a worker-processed source keeps the inherited stamp
      // (never re-hash the worker's own output into the binding).
      if ((origin_meta.flags & AlternateMetadata::kFlagWorkerProcessed) == 0) {
        StampSourceBinding(&origin_meta, origin_hash);
      }

      // ---------------------------------------------------------------
      // SVG auto-vectorization: evaluate candidacy and optionally
      // vectorize.  Runs alongside (not instead of) the raster
      // transcode loop.  The SVG variant uses a single AlternateId
      // (kSvg/Desktop/1x/off/Identity) regardless of viewport/density.
      // ---------------------------------------------------------------
      SvgVectorizeResult svg_result;
      bool svg_candidate_passed = false;
      bool svg_vectorized_ok = false;

      {
        // Always evaluate candidacy (all modes including kDetect — for
        // logging and workbench display).

        // Check LCP exclusion first (cheapest gate).
        bool skip_svg_lcp = false;
        if (cfg->svg_exclude_lcp) {
          skip_svg_lcp = IsLcpCandidate(cache_.get(), notification.url,
                                        notification.hostname);
          if (skip_svg_lcp) {
            LogInfo("SVG: skipping LCP candidate %s", notification.url.c_str());
          }
        }

        if (!skip_svg_lcp) {
          // image_data points at the de-aliased origin copy (issue #934),
          // so the potentially multi-second decode below runs without
          // pinning the origin's stripe.
          // Decode image to pixels for candidacy evaluation.
          // We use the transcoder's decode which enforces memory limits.
          auto decoded = image_transcoder_->DecodeToPixels(image_data);

          if (!decoded.pixel_buffer.empty()) {
            // Run content analysis first (needed by EvaluateSvgCandidacy).
            QualityPreset preset;
            if (cfg->content_analysis) {
              auto analysis = AnalyzeContent(
                  reinterpret_cast<const uint8_t*>(decoded.pixel_buffer.data()),
                  decoded.pixel_buffer.size(), decoded.width, decoded.height,
                  decoded.bytes_per_pixel, handler_);
              preset = analysis.quality_preset;
            }

            // Build candidacy config from worker config.
            SvgCandidacyConfig svg_cand_cfg;
            svg_cand_cfg.max_pixels = cfg->svg_max_pixels;
            svg_cand_cfg.candidacy_threshold = cfg->svg_candidacy_threshold;

            // Determine source format for format bonuses.
            ImageType src_format = ToSvgImageType(image_data);

            // Evaluate SVG candidacy.
            auto candidacy = EvaluateSvgCandidacy(
                svg_cand_cfg, preset,
                reinterpret_cast<const uint8_t*>(decoded.pixel_buffer.data()),
                decoded.pixel_buffer.size(), decoded.width, decoded.height,
                decoded.bytes_per_pixel, src_format,
                /*png_meta=*/nullptr, handler_);

            stats_.svg_candidates_evaluated.fetch_add(
                1, std::memory_order_relaxed);

            if (candidacy.result == SvgCandidacy::kReject) {
              stats_.svg_candidates_rejected.fetch_add(
                  1, std::memory_order_relaxed);
              LogInfo("SVG: rejected %s (score=%d, reason=%s)",
                      notification.url.c_str(), candidacy.score,
                      candidacy.reason.c_str());
            } else {
              svg_candidate_passed = true;
              LogInfo(
                  "SVG: candidate %s passed (score=%d, "
                  "unique_colors=%zu, flat_ratio=%.2f)",
                  notification.url.c_str(), candidacy.score,
                  candidacy.quantized_unique_colors,
                  candidacy.flat_region_ratio);
            }

            // Broadcast SVG candidacy event for workbench.
            if (ws_manager_) {
              nlohmann::json ev;
              ev["url"] = notification.url;
              ev["score"] = candidacy.score;
              ev["result"] = (candidacy.result == SvgCandidacy::kAttempt)
                                 ? "attempt"
                                 : "reject";
              ev["reason"] = candidacy.reason;
              ev["unique_colors"] = candidacy.quantized_unique_colors;
              ev["flat_ratio"] = candidacy.flat_region_ratio;
              ev["alpha_coverage"] = candidacy.alpha_coverage;
              ws_manager_->PostEvent("svg_candidacy", std::move(ev));
            }

            // If candidate passed and mode >= kPreview, vectorize.
            if (svg_candidate_passed && cfg->svg_mode >= SvgMode::kPreview) {
              // Build vectorizer config from worker config.
              SvgVectorizerConfig vec_cfg;
              vec_cfg.filter_speckle = cfg->svg_filter_speckle;
              vec_cfg.max_paths = cfg->svg_max_paths;
              vec_cfg.max_svg_bytes =
                  static_cast<size_t>(cfg->svg_max_svg_bytes);

              // Adaptive color precision: 0 = auto.
              if (cfg->svg_color_precision == 0) {
                // Adapt based on unique color count:
                //   <= 8 colors → precision 3 (tight quantization)
                //   <= 32 colors → precision 5
                //   else → precision 6
                if (candidacy.quantized_unique_colors <= 8) {
                  vec_cfg.color_precision = 3;
                } else if (candidacy.quantized_unique_colors <= 32) {
                  vec_cfg.color_precision = 5;
                } else {
                  vec_cfg.color_precision = 6;
                }
              } else {
                vec_cfg.color_precision = cfg->svg_color_precision;
              }

              auto svg_start = std::chrono::steady_clock::now();

              svg_result = VectorizeImage(
                  reinterpret_cast<const uint8_t*>(decoded.pixel_buffer.data()),
                  decoded.width, decoded.height, decoded.bytes_per_pixel,
                  vec_cfg);

              auto svg_elapsed_us =
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - svg_start)
                      .count();
              stats_.svg_vectorize_time_us.fetch_add(
                  static_cast<uint64_t>(svg_elapsed_us),
                  std::memory_order_relaxed);

              if (svg_result.success) {
                svg_vectorized_ok = true;
                stats_.svg_vectorized.fetch_add(1, std::memory_order_relaxed);
                LogInfo(
                    "SVG: vectorized %s (%zu paths, %zu bytes, "
                    "%d colors, %" PRId64 " us)",
                    notification.url.c_str(), svg_result.path_count,
                    svg_result.svg_data.size(), svg_result.actual_colors,
                    svg_elapsed_us);
              } else {
                LogInfo("SVG: vectorization failed for %s: %s",
                        notification.url.c_str(),
                        svg_result.error_message.c_str());
                // Check if it was a path count rejection.
                if (svg_result.path_count >
                    static_cast<size_t>(cfg->svg_max_paths)) {
                  stats_.svg_path_count_rejected.fetch_add(
                      1, std::memory_order_relaxed);
                }
              }

              // Broadcast SVG vectorization event for workbench.
              if (ws_manager_) {
                nlohmann::json ev;
                ev["url"] = notification.url;
                ev["success"] = svg_result.success;
                ev["path_count"] = svg_result.path_count;
                ev["svg_bytes"] = svg_result.svg_data.size();
                ev["actual_colors"] = svg_result.actual_colors;
                if (!svg_result.success) {
                  ev["error"] = svg_result.error_message;
                }
                ws_manager_->PostEvent("svg_vectorized", std::move(ev));
              }
            }
          } else {
            LogInfo("SVG: decode failed for candidacy eval on %s",
                    notification.url.c_str());
          }
        }
      }

      auto variant_result =
          WriteImageVariants(notification, target_mask, image_data, origin_meta,
                             purge_check, cfg, origin_hash);
      // Kept as-is: when no optimized original-format variant replaced the
      // identity slot (the re-encode was not smaller), it still holds the
      // front end's own bytes; give it the stamp so an origin without
      // validators gets an ETag and a 304 for them.
      CountStamp(stats_,
                 StampRawIdentityEntry(
                     cache_.get(), notification.url, notification.hostname,
                     notification.scheme, notification.site,
                     cfg->read_lease_duration_ms > 0, purge_check, this));
      size_t smallest_raster_size = variant_result.smallest_raster_size;
      size_t expected_writes = variant_result.expected_writes;
      size_t successful_writes = variant_result.successful_writes;

      // ---------------------------------------------------------------
      // SVG size gate + cache write (after raster loop completes).
      // Compare SVG output against the smallest raster variant.
      // ---------------------------------------------------------------
      if (svg_vectorized_ok && cfg->svg_mode == SvgMode::kAuto) {
        // Size gate: SVG must be smaller than the smallest raster.
        if (svg_result.svg_data.size() >= smallest_raster_size) {
          stats_.svg_size_rejected.fetch_add(1, std::memory_order_relaxed);
          LogInfo(
              "SVG: size gate rejected %s (svg=%zu >= "
              "smallest_raster=%zu)",
              notification.url.c_str(), svg_result.svg_data.size(),
              smallest_raster_size);
        } else {
          // All gates passed — write SVG variant to cache.
          // SVG uses a single AlternateId: kSvg format, Desktop
          // viewport, 1x density, no save-data, identity encoding.
          // Resolution-independent: one variant serves all viewports.
          CapabilityMask svg_mask(CapabilityMask::ImageFormat::kSvg,
                                  CapabilityMask::Viewport::kDesktop,
                                  CapabilityMask::PixelDensity::k1x,
                                  CapabilityMask::SaveData::kOff,
                                  CapabilityMask::TransferEncoding::kIdentity);
          AlternateMetadata svg_vec_meta = origin_meta;
          svg_vec_meta.content_type = ContentType::kImage;
          svg_vec_meta.flags = AlternateMetadata::kFlagWorkerProcessed |
                               InheritedVariantFlags(svg_vec_meta);
          svg_vec_meta.origin_content_length = static_cast<uint32_t>(
              std::min(image_data.size(), size_t{UINT32_MAX}));
          svg_vec_meta.origin_content_type = "image/svg+xml";
          stats_.alternate_writes.fetch_add(1, std::memory_order_relaxed);
          bool svg_written = WriteVariant(
              cache_.get(), notification.url, notification.hostname,
              notification.scheme, notification.site, svg_mask,
              std::span<const char>(svg_result.svg_data.data(),
                                    svg_result.svg_data.size()),
              svg_vec_meta, purge_check, this);

          if (svg_written) {
            stats_.svg_written.fetch_add(1, std::memory_order_relaxed);
            stats_.variants_written.fetch_add(1, std::memory_order_relaxed);

            // Track bytes saved (original - svg).
            if (image_data.size() > svg_result.svg_data.size()) {
              stats_.svg_bytes_saved.fetch_add(
                  image_data.size() - svg_result.svg_data.size(),
                  std::memory_order_relaxed);
            }

            LogInfo(
                "SVG: wrote variant for %s (%zu bytes, "
                "saved %zu bytes vs original)",
                notification.url.c_str(), svg_result.svg_data.size(),
                image_data.size() > svg_result.svg_data.size()
                    ? image_data.size() - svg_result.svg_data.size()
                    : 0);

            // Write pre-compressed SVG variants (gzip/brotli).
            // SVGs are text and compress very well.
            WriteCompressedVariants(
                cache_.get(), notification.url, notification.hostname,
                notification.scheme, notification.site, svg_mask,
                std::string_view(svg_result.svg_data.data(),
                                 svg_result.svg_data.size()),
                std::string_view(), svg_vec_meta, cfg->gzip_level,
                cfg->brotli_level, stats_, purge_check, this);

            // Broadcast SVG write event for workbench.
            if (ws_manager_) {
              nlohmann::json ev;
              ev["url"] = notification.url;
              ev["svg_bytes"] = svg_result.svg_data.size();
              ev["original_bytes"] = image_data.size();
              ev["smallest_raster_bytes"] = smallest_raster_size;
              ev["path_count"] = svg_result.path_count;
              ws_manager_->PostEvent("svg_written", std::move(ev));
            }
          } else {
            stats_.alternate_write_failures.fetch_add(
                1, std::memory_order_relaxed);
            LogWarning("SVG: failed to write variant for %s",
                       notification.url.c_str());
          }
        }
      } else if (svg_vectorized_ok && cfg->svg_mode == SvgMode::kPreview) {
        // Preview mode: log result but don't write to cache for serving.
        // The workbench can display the SVG preview via the events already
        // broadcast above.
        LogInfo(
            "SVG: preview mode — vectorized %s (%zu bytes, "
            "%zu paths) but not writing to cache",
            notification.url.c_str(), svg_result.svg_data.size(),
            svg_result.path_count);
      }

      // Persist origin content hash for future change detection (reuses
      // the hash computed at the start of the image processing pipeline).
      {
        auto wh = cache_->WriteSentinel(
            notification.url, notification.hostname, notification.scheme,
            SentinelId::kContentHash, 32, notification.site);
        if (wh) {
          (void)wh->write_sync(
              std::as_bytes(std::span(origin_hash.data(), origin_hash.size())));
          (void)wh->close_sync();
          // Write-around RAM tier (issue #1126): the change-detection read
          // at the top of this pipeline populated this process's RAM copy
          // of the sentinel; a concurrent same-URL notification can admit
          // it via CLFUS before this overwrite commits.  Without a
          // post-commit eviction the stale hash keeps comparing "changed"
          // and the full decode/transcode matrix is regenerated on every
          // subsequent notification, forever.
          cache_->EvictAlternateFromRamCache(
              notification.url, notification.hostname, notification.scheme,
              static_cast<AlternateId>(SentinelId::kContentHash),
              notification.site);
        }
      }

      {
        const CapabilityMask img_slot =
            CapabilityMask::Decode(notification.capability_mask);

        // Incomplete-matrix retry tracking: if not all expected variants
        // were written (e.g., due to cache write failures from race
        // conditions), allow re-notification to fill gaps.
        if (expected_writes > 0 && successful_writes < expected_writes) {
          std::string retry_key =
              ComposeInternalKey(notification.url, notification.hostname,
                                 notification.scheme, notification.site);
          std::lock_guard<std::mutex> lock(incomplete_retries_mutex_);
          int& count = image_incomplete_retries_[retry_key];
          if (++count >= kMaxIncompleteRetries) {
            // Exhausted retries — mark as processed to stop retrying.
            LogWarning(
                "Image incomplete matrix for %s: %zu/%zu written, "
                "retries exhausted (%d)",
                notification.url.c_str(), successful_writes, expected_writes,
                count);
            image_incomplete_retries_.erase(retry_key);
            // Populate dedup set so that any observer polling stats
            // sees the entry already present.
            mark_image_slot_decided(img_slot);
          } else {
            // Don't mark as processed — allow re-notification to fill gaps.
            stats_.image_incomplete_matrices.fetch_add(
                1, std::memory_order_relaxed);
            LogInfo(
                "Image incomplete matrix for %s: %zu/%zu written, "
                "retry %d/%d",
                notification.url.c_str(), successful_writes, expected_writes,
                count, kMaxIncompleteRetries);
          }
          if (image_incomplete_retries_.size() > kMaxIncompleteRetryEntries) {
            image_incomplete_retries_.clear();
          }
        } else {
          // All expected writes succeeded — populate dedup set before
          // stats become observable to prevent dedup race.
          mark_image_slot_decided(img_slot);
        }
      }
      break;
    }

    case ContentType::kCss: {
      if (cfg->disable_css) {
        LogInfo("CSS processing disabled, skipping %s",
                notification.url.c_str());
        return;
      }

      // SRI (issue #656): never write a variant for an integrity-pinned
      // URL — the optimized bytes would fail the browser's hash check.
      if (IsIntegrityPinned(notification)) {
        LogInfo("Skipping CSS optimization for %s (integrity-pinned)",
                notification.url.c_str());
        MarkVariantProcessed(
            notification.url, notification.hostname, notification.scheme,
            notification.site,
            MaskToId(NormalizeMaskForDedup(notification.capability_mask)),
            purge_gen);
        return;
      }

      if (!cache_) {
        LogWarning("Cache not initialized, skipping CSS processing");
        return;
      }

      // Read original CSS from cache (with exponential-backoff retry).
      auto css_read_result = ReadOriginalWithRetry(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site, stats_, handler_);

      if (!css_read_result.has_value()) {
        LogWarning("Original CSS not found in cache for %s",
                   notification.url.c_str());
        return;
      }

      auto css_content_span = css_read_result->content();
      std::string_view css_input(
          reinterpret_cast<const char*>(css_content_span.data()),
          css_content_span.size());

      // Guard: reject gzip-compressed content (see HTML guard above).
      // Defense-in-depth for gzip only; nginx's Content-Encoding check
      // is the primary protection for all encodings.
      if (IsGzipCompressed(css_input)) {
        LogError(
            "Cached content appears gzip-compressed, skipping "
            "CSS processing for %s. Ensure origin sends "
            "uncompressed responses (proxy_set_header "
            "Accept-Encoding \"\").",
            notification.url.c_str());
        stats_.errors.fetch_add(1, std::memory_order_relaxed);
        stats_.errors_origin_misconfiguration.fetch_add(
            1, std::memory_order_relaxed);
        return;
      }

      if (css_input.size() > cfg->max_css_size) {
        LogWarning("CSS too large (%zu > %zu), skipping %s", css_input.size(),
                   cfg->max_css_size, notification.url.c_str());
        return;
      }

      // De-alias the origin borrow before @import flattening + minify
      // (issue #934).  Copy the size-capped CSS out, prove it is
      // untorn, then release the borrow so FlattenImports/MinifyCss run over
      // owned memory — at cache-full a wrap can never overwrite the input
      // mid-minify.  One bounded copy per disk-hit CSS read.
      std::string owned_css(css_input);
      if (!BorrowCopyUntorn(*css_read_result,
                            cfg->read_lease_duration_ms > 0)) {
        stats_.read_borrow_wrap_discards.fetch_add(1,
                                                   std::memory_order_relaxed);
        LogWarning(
            "Origin CSS borrow wrapped during de-alias copy, skipping %s",
            notification.url.c_str());
        return;
      }
      css_read_result->release();
      css_input = owned_css;

      // Flatten @import statements before minification.
      std::string css_to_minify;
      // A sheet whose output depends on other files (inlined imports, or an
      // import that could not be inlined) is not bound to its own bytes: the
      // hash of this file cannot show that an imported child changed, so
      // such sheets keep healing at TTL expiry or purge.
      bool css_embeds_imports = false;
      if (!cfg->disable_css_import_flattening) {
        auto lookup = [&](std::string_view url) -> std::optional<std::string> {
          stats_.selector_invocations.fetch_add(1, std::memory_order_relaxed);
          // Normalize the import URL for cache key consistency — @import
          // directives may contain absolute URLs that need scheme+authority
          // stripping to match how nginx stores cache entries.
          std::string norm_url = NormalizeCacheUrl(url, url_norm_config_);
          auto result = ReadAdmittedSubresource(notification, norm_url,
                                                notification.hostname);
          if (!result.has_value()) return std::nullopt;
          auto span = result->content();
          return std::string(reinterpret_cast<const char*>(span.data()),
                             span.size());
        };
        auto flatten_result =
            css::FlattenImports(css_input, notification.url, lookup);
        css_embeds_imports = flatten_result.imports_resolved > 0 ||
                             flatten_result.skipped_unresolved_import;
        if (flatten_result.imports_resolved > 0) {
          // Flattening is all-or-nothing: success implies 0 unresolved.
          css_to_minify = std::move(flatten_result.css);
          LogInfo("Flattened %d @imports for %s",
                  flatten_result.imports_resolved, notification.url.c_str());
        } else {
          if (flatten_result.skipped_unresolved_import) {
            LogInfo(
                "@import flatten skipped for %s (unresolved import); "
                "original served, heals at TTL",
                notification.url.c_str());
          }
          css_to_minify = std::string(css_input);
        }
      } else {
        css_to_minify = std::string(css_input);
      }

      std::string minified_css;
      if (!css::MinifyCss(css_to_minify, &minified_css)) {
        LogWarning("CSS minification failed for %s", notification.url.c_str());
        // Mark processed: minification failure is deterministic for a given
        // input, so retrying on the next notification would loop forever.
        stats_.text_minify_parse_failures.fetch_add(1,
                                                    std::memory_order_relaxed);
        MarkVariantProcessed(
            notification.url, notification.hostname, notification.scheme,
            notification.site,
            MaskToId(NormalizeMaskForDedup(notification.capability_mask)),
            purge_gen);
        return;
      }

      WriteTextVariant(notification, *css_read_result, css_input, minified_css,
                       ContentType::kCss, "CSS", stats_.css_processed,
                       stats_.css_already_optimal_count,
                       stats_.css_already_optimal_bytes, purge_check, purge_gen,
                       cfg, /*record_source_hash=*/!css_embeds_imports);
      break;
    }

    case ContentType::kJs: {
      if (cfg->disable_js) {
        LogInfo("JS processing disabled, skipping %s",
                notification.url.c_str());
        return;
      }

      // SRI (issue #656): never write a variant for an integrity-pinned
      // URL — the optimized bytes would fail the browser's hash check.
      if (IsIntegrityPinned(notification)) {
        LogInfo("Skipping JS optimization for %s (integrity-pinned)",
                notification.url.c_str());
        MarkVariantProcessed(
            notification.url, notification.hostname, notification.scheme,
            notification.site,
            MaskToId(NormalizeMaskForDedup(notification.capability_mask)),
            purge_gen);
        return;
      }

      if (!cache_) {
        LogWarning("Cache not initialized, skipping JS processing");
        return;
      }

      // Read original JS from cache (with exponential-backoff retry).
      auto js_read_result = ReadOriginalWithRetry(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site, stats_, handler_);

      if (!js_read_result.has_value()) {
        LogWarning("Original JS not found in cache for %s",
                   notification.url.c_str());
        return;
      }

      auto js_content_span = js_read_result->content();
      std::string_view js_input(
          reinterpret_cast<const char*>(js_content_span.data()),
          js_content_span.size());

      // Guard: reject gzip-compressed content (see HTML guard above).
      // Defense-in-depth for gzip only; nginx's Content-Encoding check
      // is the primary protection for all encodings.
      if (IsGzipCompressed(js_input)) {
        LogError(
            "Cached content appears gzip-compressed, skipping "
            "JS processing for %s. Ensure origin sends "
            "uncompressed responses (proxy_set_header "
            "Accept-Encoding \"\").",
            notification.url.c_str());
        stats_.errors.fetch_add(1, std::memory_order_relaxed);
        stats_.errors_origin_misconfiguration.fetch_add(
            1, std::memory_order_relaxed);
        return;
      }

      if (js_input.size() > cfg->max_js_size) {
        LogWarning("JS too large (%zu > %zu), skipping %s", js_input.size(),
                   cfg->max_js_size, notification.url.c_str());
        return;
      }

      // De-alias the origin borrow before minify (issue #934).
      // Copy the size-capped JS out, prove it is untorn, then release the
      // borrow so MinifyUtf8Js runs over owned memory — at cache-full a wrap
      // can never overwrite the input mid-minify.  One bounded copy per
      // disk-hit JS read.
      std::string owned_js(js_input);
      if (!BorrowCopyUntorn(*js_read_result, cfg->read_lease_duration_ms > 0)) {
        stats_.read_borrow_wrap_discards.fetch_add(1,
                                                   std::memory_order_relaxed);
        LogWarning("Origin JS borrow wrapped during de-alias copy, skipping %s",
                   notification.url.c_str());
        return;
      }
      js_read_result->release();
      js_input = owned_js;

      std::string minified_js;

      // JsTokenizerPatterns compiles RE2 patterns; create once.
      static const js::JsTokenizerPatterns js_patterns;

      bool js_ok = js::MinifyUtf8Js(&js_patterns, js_input, &minified_js);
      if (!js_ok) {
        // Log once per resource per worker lifetime (issue #666): the parse
        // failure is deterministic, so every cache refresh would otherwise
        // re-log this WARNING for a permanently-unparseable file.
        if (ShouldLogJsParseError(notification.url, notification.hostname,
                                  notification.scheme, notification.site)) {
          LogWarning(
              "JS minification had parse errors for %s, "
              "serving original (variant not written)",
              notification.url.c_str());
        }
        // The tokenizer's error path emits the unlexable remainder raw,
        // so the output may be half-minified — never ship it.  Mark
        // processed: parse failure is deterministic for a given input,
        // so retrying on the next notification would loop forever.
        stats_.text_minify_parse_failures.fetch_add(1,
                                                    std::memory_order_relaxed);
        MarkVariantProcessed(
            notification.url, notification.hostname, notification.scheme,
            notification.site,
            MaskToId(NormalizeMaskForDedup(notification.capability_mask)),
            purge_gen);
        return;
      }

      WriteTextVariant(notification, *js_read_result, js_input, minified_js,
                       ContentType::kJs, "JS", stats_.js_processed,
                       stats_.js_already_optimal_count,
                       stats_.js_already_optimal_bytes, purge_check, purge_gen,
                       cfg);
      break;
    }

    case ContentType::kOther:
    default:
      LogWarning("Unknown content type for %s", notification.url.c_str());
      break;
  }

  // Accumulate timing
  auto notif_end = std::chrono::steady_clock::now();
  auto elapsed_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(notif_end -
                                                            notif_start)
          .count());
  stats_.total_processing_time_us.fetch_add(elapsed_us,
                                            std::memory_order_relaxed);
  switch (notification.content_type) {
    case ContentType::kHtml:
      stats_.html_processing_time_us.fetch_add(elapsed_us,
                                               std::memory_order_relaxed);
      break;
    case ContentType::kImage:
      stats_.image_processing_time_us.fetch_add(elapsed_us,
                                                std::memory_order_relaxed);
      break;
    case ContentType::kCss:
      stats_.css_processing_time_us.fetch_add(elapsed_us,
                                              std::memory_order_relaxed);
      break;
    case ContentType::kJs:
      stats_.js_processing_time_us.fetch_add(elapsed_us,
                                             std::memory_order_relaxed);
      break;
    default:
      break;
  }
}

// =================================================================
// llms.txt builder handler (sentinel mask kLlmsTxtSentinel)
// =================================================================

bool Worker::LlmsTxtNeedsBuild(
    const std::string& hostname, const std::string& scheme,
    std::string_view site, const std::array<std::byte, 32>& live_sitemap_hash,
    int64_t now_sec) const {
  if (!cache_) return false;
  auto meta = cache_->ReadAlternate(
      "/llms.txt", hostname, scheme,
      static_cast<AlternateId>(SentinelId::kLlmsTxtMeta), site);
  if (!meta.has_value()) return true;  // never built
  auto bytes = meta->content();
  if (bytes.size() < AlternateMetadata::kHashSize + 8)
    return true;  // malformed
  if (std::memcmp(bytes.data(), live_sitemap_hash.data(),
                  AlternateMetadata::kHashSize) != 0) {
    return true;  // sitemap content changed
  }
  int64_t next_refresh = 0;
  for (int i = 0; i < 8; ++i) {
    next_refresh =
        (next_refresh << 8) |
        static_cast<unsigned char>(bytes[AlternateMetadata::kHashSize + i]);
  }
  return now_sec >= next_refresh;  // TTL ceiling reached
}

void Worker::HandleLlmsTxtBuild(const CacheNotification& notification,
                                const PurgeDispatchGen& /*purge_gen*/) {
  const auto cfg = live_config_.load();
  if (!cache_) {
    LogWarning("llms.txt: cache not initialized, skipping build");
    return;
  }
  // Full gate: both operator flags (there is no token entitlement behind
  // this any more).
  if (!cfg->browser_analysis.agent_optimize ||
      !cfg->browser_analysis.agent_optimize_llms_txt) {
    LogInfo("llms.txt: disabled, skipping build for %s",
            notification.hostname.c_str());
    return;
  }

  const std::string scheme =
      notification.scheme.empty() ? "https" : notification.scheme;
  const std::string& host = notification.hostname;
  if (host.empty()) {
    LogWarning("llms.txt: empty hostname, skipping build");
    return;
  }

  // G1 own-origin pin built from the notification's OWN origin (NEVER page- or
  // sitemap-supplied). allow_hosts is intentionally empty — /llms.txt synthesis
  // fetches own-origin only (sitemap, robots.txt, page HTML).
  auto policy = std::make_shared<pagespeed::FetchPolicy>();
  if (auto up = pagespeed::ParseUpstream(scheme + "://" + host);
      up.has_value()) {
    policy->pinned_upstreams.push_back(*up);
  } else {
    LogWarning("llms.txt: could not pin own origin %s://%s, skipping",
               scheme.c_str(), host.c_str());
    return;
  }
  pagespeed::AgentFetcherDeps deps;
  deps.spawn = pagespeed::RealCurlSpawn();
  deps.resolve = pagespeed::RealHostResolver();
  // The answers go into /llms.txt, served to other clients.
  deps.shared_copy = true;
  deps.private_address_origins =
      pagespeed::ParseOperatorOrigins(
          cfg->browser_analysis.agent_render_private_origins)
          .value_or(std::vector<pagespeed::PinnedUpstream>{});
  deps.on_private_address_refusal = [this](const std::string& m) {
    LogWarning("%s", m.c_str());
  };

  LlmsTxtFetchFn fetch = [policy, deps](const std::string& url) {
    pagespeed::AgentFetchOutcome o = pagespeed::FetchSubresource(
        url, pagespeed::ResourceClass::kDocument, *policy, deps);
    LlmsTxtFetchResult r;
    r.status = o.status;
    r.ok = o.fulfilled && o.status == 200;
    if (r.ok) r.body = std::move(o.body);
    for (const pagespeed::HttpHeader& h : o.headers) {
      if (net_instaweb::StringCaseEqual(h.name, "X-Robots-Tag")) {
        r.x_robots_tag = h.value;
      } else if (net_instaweb::StringCaseEqual(h.name, "Google-Extended")) {
        r.google_extended = h.value;
      }
    }
    return r;
  };

  const std::string origin = scheme + "://" + host;
  LlmsTxtVariantReaderFn variant_reader =
      [this, host, scheme, origin, site = notification.site,
       &notification](const std::string& url) -> std::optional<std::string> {
    std::string path = (url.size() >= origin.size() &&
                        url.compare(0, origin.size(), origin) == 0)
                           ? url.substr(origin.size())
                           : std::string("/");
    if (path.empty()) path = "/";
    // Another page's markdown goes into /llms.txt only when the server serves
    // that page to any client.
    if (!SubresourceAdmitted(notification, path, host, /*may_fetch=*/true,
                             /*entry=*/nullptr, /*shared_copy=*/true)) {
      return std::nullopt;
    }
    auto md = cache_->ReadAlternate(
        path, host, scheme,
        static_cast<AlternateId>(SentinelId::kAgentMarkdown), site);
    if (!md.has_value()) return std::nullopt;
    auto c = md->content();
    return std::string(reinterpret_cast<const char*>(c.data()), c.size());
  };

  LlmsTxtBuilder builder(std::move(fetch), std::move(variant_reader));

  LlmsTxtBuildOptions opts;
  opts.scheme = scheme;
  opts.host = host;
  opts.sitemap_path = cfg->browser_analysis.agent_optimize_sitemap_url;
  opts.allow_paths = cfg->browser_analysis.agent_optimize_paths;
  opts.respect_ai_directives =
      cfg->browser_analysis.agent_optimize_respect_ai_directives;
  opts.summary_fetch_cap =
      cfg->browser_analysis.agent_optimize_llms_summary_fetch_cap;

  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  int64_t ttl = cfg->browser_analysis.agent_optimize_cache_ttl_seconds;
  if (ttl <= 0) ttl = 86400;

  // Writes use the SAME normalized (host, scheme) as the reads above so the
  // worker-write and nginx-read keys are identical (host == notification.hostname;
  // scheme is the empty->"https" normalized form).
  auto write_sentinel = [this, &notification, &host, &scheme](
                            SentinelId id, std::string_view bytes) {
    auto wh = cache_->WriteSentinel(notification.url, host, scheme, id,
                                    bytes.size(), notification.site);
    if (wh.has_value()) {
      (void)wh->write_sync(
          std::as_bytes(std::span(bytes.data(), bytes.size())));
      (void)wh->close_sync();
      // Cyclone's RAM tier is write-around (reads populate it, writes do
      // NOT evict): LlmsTxtNeedsBuild and the existing-index check read
      // these sentinel slots in-process before the build overwrites them,
      // so a stale RAM copy of kLlmsTxtMeta would keep reading
      // expired/changed and re-trigger the FULL per-page rebuild on every
      // subsequent build check, forever (issue #1126).  Evict the
      // just-committed slot post-commit so the next check observes the
      // fresh record.  Covers both kLlmsTxt and kLlmsTxtMeta writes.
      cache_->EvictAlternateFromRamCache(notification.url, host, scheme,
                                         static_cast<AlternateId>(id),
                                         notification.site);
    } else {
      stats_.errors.fetch_add(1, std::memory_order_relaxed);
      LogWarning("llms.txt: sentinel write failed for %s",
                 notification.url.c_str());
    }
  };
  // kLlmsTxtMeta freshness record: 32-byte sitemap hash ++ big-endian
  // next-refresh epoch. nginx throttles its per-request build-notify on this
  // epoch, so EVERY attempt (success OR failure) must stamp one — otherwise a
  // stale/cold site with an unreachable sitemap re-notifies on every request.
  auto write_meta = [&](const std::array<std::byte, 32>& hash,
                        int64_t next_refresh) {
    std::string rec(AlternateMetadata::kHashSize + 8, '\0');
    std::memcpy(rec.data(), hash.data(), AlternateMetadata::kHashSize);
    for (int i = 0; i < 8; ++i) {
      rec[AlternateMetadata::kHashSize + i] =
          static_cast<char>((next_refresh >> (8 * (7 - i))) & 0xFF);
    }
    write_sentinel(SentinelId::kLlmsTxtMeta, rec);
  };
  // On a failed/blocked build, advance next-refresh by a short backoff so the
  // per-request notify storm degrades to ~one attempt per backoff per site.
  const int64_t backoff_refresh = now + (ttl < 900 ? ttl : 900);
  auto stamp_backoff = [&]() {
    write_meta(std::array<std::byte, 32>{}, backoff_refresh);
  };

  const bool existing =
      cache_
          ->ReadAlternate(notification.url, host, scheme,
                          static_cast<AlternateId>(SentinelId::kLlmsTxt))
          .has_value();

  // Phase 1 — cheap sitemap-only build: yields the sitemap content-hash + (for a
  // cold miss) a servable stub. No per-page fetches.
  LlmsTxtBuildOptions stub_opts = opts;
  stub_opts.stub_only = true;
  LlmsTxtBuildResult stub = builder.Build(stub_opts);

  if (stub.status == LlmsTxtBuildStatus::kAiBlocked) {
    // The site now disallows AI crawling — stop serving any prior index, then
    // stamp a backoff so we re-check robots.txt only periodically (not per req).
    (void)cache_->Remove(notification.url, host, scheme, notification.site);
    stamp_backoff();
    LogInfo("llms.txt: AI-blocked by robots.txt, removed index for %s",
            host.c_str());
    return;
  }
  if (stub.status != LlmsTxtBuildStatus::kOk) {
    // No reachable sitemap / no pages — keep any existing index (do not destroy a
    // previously-good file on a transient failure), but advance next-refresh so
    // nginx stops re-notifying on every request (abuse control).
    stamp_backoff();
    LogInfo("llms.txt: no source for %s, leaving existing index unchanged",
            host.c_str());
    return;
  }

  if (existing && !LlmsTxtNeedsBuild(host, scheme, notification.site,
                                     stub.sitemap_hash, now)) {
    return;  // within TTL and the sitemap is unchanged — nothing to do
  }

  if (!existing) {
    // Cold miss: publish the sitemap-only stub immediately so a request during
    // the (slower) full build gets a valid index rather than a miss.
    write_sentinel(SentinelId::kLlmsTxt, stub.llms_txt);
  }

  // Phase 2 — full build with the capped per-page summary fetches.
  LlmsTxtBuildResult full = builder.Build(opts);
  if (full.status == LlmsTxtBuildStatus::kAiBlocked) {
    // robots.txt flipped to AI-blocked between the stub and full passes (TOCTOU):
    // honor it — remove the index (incl. any stub just written) and back off.
    (void)cache_->Remove(notification.url, host, scheme, notification.site);
    stamp_backoff();
    LogInfo("llms.txt: AI-blocked during full build, removed index for %s",
            host.c_str());
    return;
  }
  const bool full_ok = full.status == LlmsTxtBuildStatus::kOk;
  std::string_view body = full_ok ? full.llms_txt : stub.llms_txt;
  const std::array<std::byte, 32>& hash =
      full_ok ? full.sitemap_hash : stub.sitemap_hash;

  write_sentinel(SentinelId::kLlmsTxt, body);
  write_meta(hash, now + ttl);  // success: full TTL ceiling

  LogInfo("llms.txt: wrote %zu-byte index for %s (full=%d)", body.size(),
          host.c_str(), full_ok ? 1 : 0);
}

// =================================================================
// Warmup handler (sentinel mask 0xFFFFFFFE)
// =================================================================

std::optional<std::array<std::byte, 32>> Worker::FreshOriginHash(
    const std::string& url, const std::string& hostname,
    const std::string& scheme, std::string_view site) {
  if (!cache_) return std::nullopt;
  const AlternateId identity_id = MaskToId(CapabilityMask());
  // At origin-refresh time nginx has re-recorded the fresh re-fetched body at
  // the identity slot BEFORE sending the sentinel.  But Cyclone's RAM tier is
  // WRITE-AROUND: that overwrite does NOT evict this process's stale RAM copy
  // from an earlier read, so a plain read here could return the PRE-refresh body
  // and hash it — wrongly preserving a markdown variant whose origin actually
  // changed (a stale-serve / D8 violation).  Evict first so the hash is taken
  // over the FRESH on-disk body — the same reason HandleOriginRefreshed evicts
  // the identity before the inline rebuild reads it.
  cache_->EvictAlternateFromRamCache(url, hostname, scheme, identity_id, site);
  // The slot holds RAW origin bytes — hash them directly to get the live
  // content-binding oracle.
  auto id = cache_->ReadAlternate(url, hostname, scheme, identity_id, site);
  if (!id.has_value()) return std::nullopt;
  // FAIL-SAFE: when the slot already holds the worker's OWN optimized output
  // (kFlagWorkerProcessed — a concurrent front-end re-optimized first), its
  // bytes are not the raw origin and the stamped durable hash could be stale,
  // so we refuse to derive a fresh hash.  The caller then purges the markdown
  // variant exactly as before (preservation is a pure optimization; never
  // preserve on a serving-path decision we cannot directly verify).
  if ((id->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
    return std::nullopt;
  }
  return cyclone::crypto::SHA256::hash(id->content());
}

void Worker::RecordAgentMarkdownIntent(const std::string& url,
                                       const std::string& hostname,
                                       const std::string& scheme,
                                       std::string_view site) {
  std::string key = ComposeInternalKey(url, hostname, scheme, site);
  std::lock_guard<std::mutex> lock(agent_markdown_intent_mutex_);
  if (agent_markdown_intent_urls_.size() >= kMaxAgentMarkdownIntentEntries &&
      !agent_markdown_intent_urls_.contains(key)) {
    return;  // fail-open at capacity: revert to had_markdown gating for this URL
  }
  agent_markdown_intent_urls_.insert(std::move(key));
}

bool Worker::HasAgentMarkdownIntent(const std::string& url,
                                    const std::string& hostname,
                                    const std::string& scheme,
                                    std::string_view site) const {
  std::string key = ComposeInternalKey(url, hostname, scheme, site);
  std::lock_guard<std::mutex> lock(agent_markdown_intent_mutex_);
  return agent_markdown_intent_urls_.contains(key);
}

bool Worker::ShouldLogJsParseError(const std::string& url,
                                   const std::string& hostname,
                                   const std::string& scheme,
                                   std::string_view site) {
  std::string key = ComposeInternalKey(url, hostname, scheme, site);
  std::lock_guard<std::mutex> lock(js_parse_error_logged_mutex_);
  if (js_parse_error_logged_urls_.contains(key)) {
    return false;  // already warned once for this resource — suppress repeats
  }
  if (js_parse_error_logged_urls_.size() >= kMaxJsParseErrorLoggedEntries) {
    // Fail open at capacity: log the warning but do not insert, so memory
    // stays bounded and a real parser gap is never silently swallowed.
    return true;
  }
  js_parse_error_logged_urls_.insert(std::move(key));
  return true;
}

void Worker::HandleOriginRefreshed(const CacheNotification& notification) {
  if (!cache_) {
    LogWarning("Origin-refreshed: cache not initialized, skipping %s",
               notification.url.c_str());
    return;
  }

  // Rate limit per URL: a born-stale upstream (e.g. a CDN emitting
  // Age > max-age) makes every request take the revalidate → re-fetch
  // path, each sending this sentinel.  Without the limit that would purge
  // and re-optimize the URL on every request pair.  Inside the window the
  // sentinel is dropped — serving correctness is unaffected (the stale
  // verdict already forces a re-fetch per request); only re-optimization
  // is deferred.
  {
    std::string key =
        ComposeInternalKey(notification.url, notification.hostname,
                           notification.scheme, notification.site);
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(purge_gen_mutex_);
    auto it = origin_refresh_last_purge_.find(key);
    if (it != origin_refresh_last_purge_.end() &&
        now - it->second <
            std::chrono::seconds(kOriginRefreshMinIntervalSecs)) {
      stats_.origin_refresh_rate_limited.fetch_add(1,
                                                   std::memory_order_relaxed);
      return;
    }
    if (origin_refresh_last_purge_.size() >= kMaxOriginRefreshEntries) {
      // Overflow: sweep only EXPIRED entries (issue #652 review — a
      // wholesale clear() erased the timestamps of URLs still inside
      // their window, so under sustained churn with >1024 distinct stale
      // URLs the limiter degraded to a no-op exactly in the scenario it
      // was added to bound).  Expired entries are dead weight by
      // definition; live windows always survive.
      std::erase_if(origin_refresh_last_purge_, [now](const auto& kv) {
        return now - kv.second >=
               std::chrono::seconds(kOriginRefreshMinIntervalSecs);
      });
      if (origin_refresh_last_purge_.size() >= kMaxOriginRefreshEntries) {
        // Every tracked URL is in-window: we cannot track this one
        // without dropping a live window.  Treat the sentinel as
        // rate-limited (purge deferred) instead of clearing.
        stats_.origin_refresh_rate_limited.fetch_add(1,
                                                     std::memory_order_relaxed);
        return;
      }
    }
    origin_refresh_last_purge_[key] = now;
  }

  // -------------------------------------------------------------------
  // Issue #1503: the sentinel means "the origin was re-fetched", NOT "the
  // origin changed".  Both senders fire it unconditionally on an
  // age-expired re-fetch (the nginx full re-fetch path; the 1.16 serve
  // arm's age-expired fall-through), so purging on arrival re-optimized
  // unchanged origins every freshness cycle — and with the identity slot
  // poisoned by the worker's own proactive variant, the inline rebuild
  // was refused and the URL paid a purge + deferred rebuild of identical
  // bytes per cycle (on a front end that never serves stale, a dead
  // window in which nothing optimized is servable at all).
  //
  // So decide changed-vs-unchanged FIRST, over the pristine origin
  // reference (the durable original by exact id, else a genuine
  // flag-clear identity — never the worker-processed slot, which is a
  // derived variant), compared against the kContentHash oracle the
  // processing path stamped over the origin bytes the variant set was
  // built from:
  //
  //   * match + a FRESH reference (the sender re-recorded before
  //     sending, the nginx shape): unchanged — restamp the variant set
  //     from the fresh origin metadata in place, no purge, no rebuild.
  //     "Fresh" is discriminated RELATIVELY (newer than the variant set's
  //     own stamps) precisely because nginx Age-backdates the re-record's
  //     cache_inserted_at — a wall-clock test would defer a CDN-fronted
  //     origin forever on a shape where no notify ever follows.
  //   * match + a STALE reference (the 1.16 shape: the sentinel precedes
  //     the re-record it precedes): defer — no purge; clear dedup so the
  //     imminent record+notify runs the content-hash check, which
  //     restamps on a match and purges + rebuilds on a genuine change.
  //     SUPPORTED FOR IMAGES ONLY: the image branch is the one notify
  //     path that re-decides over the pristine reference and restamps on
  //     an unchanged verdict; for text the purge below remains the
  //     change-propagation mechanism (a deferred text URL would keep its
  //     stale stamps and fall through on every request instead).
  //   * no readable reference: purge, as before (fail-safe — the D8
  //     posture is that an unverifiable origin reference is purged
  //     conservatively, and on the nginx shape no notify follows the
  //     sentinel to re-decide).
  //   * mismatch: genuinely changed — purge + rebuild, as before.
  //   * no oracle: indeterminate — purge + rebuild, as before (fail-safe:
  //     the purge is also what makes the next source read reach the
  //     re-recorded origin past a poisoned identity slot).
  // -------------------------------------------------------------------
  const auto refresh_cfg = live_config_.load();
  const bool defer_supported = notification.content_type == ContentType::kImage;
  bool have_oracle = false;
  std::array<std::byte, 32> oracle_hash{};
  {
    auto stored = cache_->ReadAlternate(
        notification.url, notification.hostname, notification.scheme,
        static_cast<AlternateId>(SentinelId::kContentHash), notification.site);
    if (stored.has_value()) {
      auto sc = stored->content();
      if (sc.size() == 32) {
        std::memcpy(oracle_hash.data(), sc.data(), 32);
        have_oracle = true;
      }
    }
  }
  if (have_oracle) {
    PristineOriginRead pristine = ReadPristineOrigin(
        cache_.get(), notification.url, notification.hostname,
        notification.scheme, notification.site,
        refresh_cfg->read_lease_duration_ms > 0, stats_, handler_);
    const bool hash_matches = pristine.found && pristine.hash == oracle_hash;
    const bool unchanged =
        hash_matches &&
        // A never stamped (epoch-0) reference carries no freshness
        // information; treat it as stale regardless of the hash match.
        pristine.meta.cache_inserted_at != 0;
    if (unchanged) {
      const int64_t now_sec = static_cast<int64_t>(time(nullptr));
      // Discriminate RELATIVELY, not by wall clock: the reference is the
      // refresh's re-record when it postdates the variant set's own stamps
      // (variants inherit the origin stamp they were built from; the 1.16
      // pre-record durable original IS that stamp — never newer).  A
      // wall-clock-only test misclassifies an Age-backdated re-record as
      // stale: nginx stamps cache_inserted_at minus the upstream Age
      // header, and on the nginx shape NO notify follows the sentinel, so
      // deferring there is a permanent re-fetch loop that never optimizes.
      // The 10s wall-clock leg stays as a secondary admission.
      const uint32_t newest_variant = NewestVariantStamp(
          cache_.get(), notification.url, notification.hostname,
          notification.scheme, notification.site, pristine.id);
      const bool reference_fresh =
          pristine.meta.cache_inserted_at > newest_variant ||
          now_sec - static_cast<int64_t>(pristine.meta.cache_inserted_at) <=
              kOriginRefreshMinIntervalSecs ||
          static_cast<int64_t>(pristine.meta.cache_inserted_at) > now_sec;
      // A re-fetched origin response that forbids shared storage must not
      // extend the variant set's life — purge instead (fail-safe).
      const bool may_restamp = CanRestampVariantSet(pristine.meta);
      if (reference_fresh && may_restamp) {
        PurgeDispatchGen restamp_gen =
            CapturePurgeGen(notification.url, notification.hostname,
                            notification.scheme, notification.site);
        PurgeCheck restamp_fence = [this, url = notification.url,
                                    hostname = notification.hostname,
                                    scheme = notification.scheme,
                                    site = notification.site, restamp_gen]() {
          return WasPurgedSinceDispatch(url, hostname, restamp_gen, scheme,
                                        site);
        };
        const size_t restamped = RestampVariantSetFreshness(
            cache_.get(), notification.url, notification.hostname,
            notification.scheme, notification.site, pristine.meta, pristine.id,
            restamp_fence);
        stats_.origin_refresh_unchanged.fetch_add(1, std::memory_order_relaxed);
        LogInfo(
            "Origin refresh for %s: content unchanged (hash match), "
            "restamped %zu variant(s) in place — no purge",
            notification.url.c_str(), restamped);
        return;
      }
      if (!reference_fresh && defer_supported) {
        // Defer (see the header comment): the re-recorded origin has not
        // landed yet (the 1.16 serve arm sends the sentinel BEFORE the
        // re-record), and the variant set shows no evidence of change.  Do
        // NOT purge; clear dedup so the record + notify that follows runs
        // the content-hash check over the fresh bytes — restamping when
        // they match, purging + rebuilding when they genuinely differ.
        ClearUrlProcessingState(notification.url, notification.hostname,
                                notification.scheme, notification.site);
        stats_.origin_refresh_deferred.fetch_add(1, std::memory_order_relaxed);
        LogInfo(
            "Origin refresh for %s: pristine reference unchanged but not "
            "yet re-recorded — purge deferred to the record+notify "
            "convergence",
            notification.url.c_str());
        return;
      }
      if (!reference_fresh) {
        // A deferrable verdict on a non-deferrable content type: purge as
        // before (the purge is the text/change-propagation mechanism).
        LogInfo(
            "Origin refresh for %s: content unchanged but the reference is "
            "stale and this content type cannot defer — purging",
            notification.url.c_str());
      } else {
        // reference_fresh && !may_restamp: fall through to the purge (the
        // refreshed origin forbids shared storage, or now negotiates on
        // Accept so the variant set built before that is stale).
        LogInfo(
            "Origin refresh for %s: unchanged content but the refreshed "
            "origin forbids shared storage or now varies on Accept, purging",
            notification.url.c_str());
      }
    } else if (!pristine.found) {
      if (defer_supported) {
        // No readable pristine reference, but an oracle exists: defer —
        // skipping the purge here is safe because the image notify path
        // re-decides over the re-recorded durable original (the 1.16
        // sender's re-record + notify always follows).
        ClearUrlProcessingState(notification.url, notification.hostname,
                                notification.scheme, notification.site);
        stats_.origin_refresh_deferred.fetch_add(1, std::memory_order_relaxed);
        LogInfo(
            "Origin refresh for %s: no readable pristine origin reference — "
            "purge deferred to the record+notify convergence",
            notification.url.c_str());
        return;
      }
      // No pristine reference on a non-defer path: purge conservatively
      // (the D8 posture — an unverifiable origin reference is never
      // preserved on).
      LogInfo(
          "Origin refresh for %s: no readable pristine origin reference — "
          "purging conservatively",
          notification.url.c_str());
    } else {
      // pristine.found && !unchanged (the !pristine.found case is handled
      // above): either the reference diverged from the oracle (genuinely
      // changed — fresh evidence) or it matched but carries no insertion
      // stamp (indeterminate freshness).  Both purge; name them apart in
      // the log.
      if (hash_matches) {
        LogInfo(
            "Origin refresh for %s: pristine origin matches the "
            "content-hash oracle but carries no insertion stamp — "
            "indeterminate, purging",
            notification.url.c_str());
      } else {
        LogInfo(
            "Origin refresh for %s: pristine origin hash diverged from the "
            "content-hash oracle — purging stale variant set",
            notification.url.c_str());
      }
    }
  } else {
    LogInfo("Origin refreshed for %s — purging stale variant set",
            notification.url.c_str());
  }

  // Issue C: capture whether this URL had an agent markdown variant BEFORE the
  // purge loop drops it.  The purge below removes the kAgentMarkdown(124)
  // variant along with the rest of the non-identity set; the inline rebuild
  // then re-processes the URL at the default mask.  Unless we propagate
  // agent_request, the warm-template branch (which rebuilds markdown on a
  // profile-present page) never fires, so 124 stays absent and the page serves
  // HTML to AI crawlers until the next cold MISS.  Demand-gated: only rebuild
  // markdown for URLs that had it (never render it for pages that never did).
  const bool had_markdown = cache_->AlternateExists(
      notification.url, notification.hostname, notification.scheme,
      static_cast<AlternateId>(SentinelId::kAgentMarkdown), notification.site);

  // Durability (#19) — D2: the origin-refresh sentinel fires on every
  // TTL expiry / 200 revalidation, INCLUDING when the re-fetched origin body is
  // byte-identical (origins without conditional-GET support re-send the same
  // bytes).  Purging the rendered markdown variant in that case needlessly
  // drops a still-valid rendering and leaves a coverage hole until the async
  // rebuild completes — the measured 62%-coverage churn.  Preserve the markdown
  // variant when the FRESH origin still hashes to the value the variant was
  // bound to; otherwise drop it as before (the content was superseded).  The
  // decision is made HERE, at purge time (never deferred to the rebuild), so a
  // genuine content change can never leave a stale-bound variant servable in
  // the window before the rebuild — the fail-closed binding
  // invariant is preserved.
  bool keep_markdown = false;
  if (had_markdown) {
    auto md = cache_->ReadAlternate(
        notification.url, notification.hostname, notification.scheme,
        static_cast<AlternateId>(SentinelId::kAgentMarkdown),
        notification.site);
    auto fresh = FreshOriginHash(notification.url, notification.hostname,
                                 notification.scheme, notification.site);
    if (md.has_value() && fresh.has_value() &&
        md->metadata.origin_html_hash == *fresh) {
      keep_markdown = true;
    }
    // D3: record the sticky intent NOW, while the variant is provably present,
    // so a later churn that loses it mid-rebuild (had_markdown reads false)
    // still forces the rebuild instead of stranding the URL on HTML.
    RecordAgentMarkdownIntent(notification.url, notification.hostname,
                              notification.scheme, notification.site);
  }
  const bool has_intent =
      HasAgentMarkdownIntent(notification.url, notification.hostname,
                             notification.scheme, notification.site);
  if (keep_markdown) {
    stats_.agent_markdown_preserved.fetch_add(1, std::memory_order_relaxed);
  } else if (had_markdown) {
    stats_.agent_markdown_purged_on_change.fetch_add(1,
                                                     std::memory_order_relaxed);
  }

  // Purge the STALE optimized variants but PRESERVE the identity original
  // (issue #652 review): nginx re-records the fresh re-fetched body at the
  // identity id BEFORE sending this sentinel, so a whole-key Remove would
  // delete the just-written fresh original — forcing a second origin
  // fetch and an unoptimized MISS on every TTL expiry.  Bump the per-URL
  // purge generation atomically with the per-alternate removes (fencing
  // in-flight stale variant writers), then clear dedup/cooldowns so the
  // rebuild below re-processes the URL.
  //
  // The preserve list is what this event is: everything else goes, including
  // the durable original, which belongs to the origin response that was just
  // replaced.  Keeping it would leave the rebuild reconstructing the NEW
  // variant set from the OLD bytes — an original that outlived its origin,
  // which is indistinguishable from a working cache from the outside.  The
  // cache refuses a preserve list naming it, so this cannot be loosened here
  // by accident.
  //
  // The identity id itself joins the preserve list ONLY when the entry
  // there is a genuine original.  The
  // worker's own output lands at the identity id too — the minified text
  // variant, and the original-format arm of an image matrix — and such an
  // entry is a STALE VARIANT at purge time, not the refreshed origin: the
  // sender recorded the refresh at the durable-original id, or has not
  // re-recorded yet.  Preserving it would keep superseded bytes servable
  // AND pass the inline-rebuild gate below on the very source this event
  // exists to replace.  The discriminator is the entry's own
  // kFlagWorkerProcessed metadata flag, not the sender: nginx's
  // pre-sentinel re-record has the flag clear and is preserved exactly as
  // before, on every substrate.  An absent or unreadable slot preserves as
  // today (the list is a filter, not an assertion that the entry exists).
  //
  // The flag must be read from the ON-DISK entry, not from this process's
  // write-around RAM tier: the sender's pre-sentinel re-record does not
  // evict our RAM copy, and any earlier read of the slot (the worker's own
  // source read, a serve, a test's verify) would otherwise answer this
  // check with the SUPERSEDED entry's flag — misjudging a genuine fresh
  // original as a stale variant on every refresh.  Evict first, exactly
  // like FreshOriginHash does before hashing and the post-purge eviction
  // below does before the rebuild's source read.  The read then populates
  // RAM with the on-disk entry; both follow-up paths clean that — a purged
  // slot is RAM-evicted by RemoveAlternatesExcept, a preserved one by the
  // post-purge EvictAlternateFromRamCache below.
  AlternateId identity_id = MaskToId(CapabilityMask());
  bool identity_was_worker_processed = false;
  // Outcome of the removal below, read after the lock is released.
  std::expected<size_t, cyclone::CacheError> purged = 0;
  {
    cache_->EvictAlternateFromRamCache(notification.url, notification.hostname,
                                       notification.scheme, identity_id,
                                       notification.site);
    auto identity = cache_->ReadAlternate(
        notification.url, notification.hostname, notification.scheme,
        identity_id, notification.site);
    if (identity.has_value() && identity->is_valid()) {
      identity_was_worker_processed =
          (identity->metadata.flags &
           AlternateMetadata::kFlagWorkerProcessed) != 0;
    }
  }
  {
    std::string gen_key =
        ComposeInternalKey(notification.url, notification.hostname,
                           notification.scheme, notification.site);
    std::lock_guard<std::mutex> lock(purge_gen_mutex_);
    BumpPurgeGenerationLocked(gen_key);
    // D2: the markdown variant is still bound to the live (unchanged) origin —
    // preserve it across the purge instead of dropping a valid rendering.
    // (When the content changed, keep_markdown is false and it is removed here
    // exactly as before.)
    std::vector<AlternateId> preserve;
    if (!identity_was_worker_processed) {
      preserve.push_back(identity_id);
    }
    if (keep_markdown) {
      preserve.push_back(static_cast<AlternateId>(SentinelId::kAgentMarkdown));
    }
    // The URL's headers sidecar (0x6C) lives and dies with the identity: the
    // refreshing record wrote the sidecar and the flagged identity just before
    // sending this sentinel, so a preserved identity needs its record kept.
    // When the identity is not preserved (the worker's own output sits there)
    // the sidecar goes with the rest of the family.
    if (!identity_was_worker_processed) {
      preserve.push_back(static_cast<AlternateId>(SentinelId::kHeadersSidecar));
    }
    purged = cache_->RemoveAlternatesExcept(
        notification.url, notification.hostname, notification.scheme, preserve,
        notification.site);
    if (!purged.has_value()) {
      // Issue #1566: a purge whose outcome is never looked at is a purge that
      // can silently do nothing — and a variant set that survives it is
      // selected, declined on freshness and re-purged on the next refresh.
      // Name the refusal, and do not count it below as a purge that happened.
      LogWarning(
          "Origin-refreshed purge for %s failed (%s): stale variants may "
          "remain servable until the next refresh",
          notification.url.c_str(),
          make_error_code(purged.error()).message().c_str());
    }
  }
  // Dedup is cleared whether or not the removal succeeded: the next
  // record+notify must be processed either way, and on a failed purge it is
  // what converges the URL.
  ClearUrlProcessingState(notification.url, notification.hostname,
                          notification.scheme, notification.site);
  if (purged.has_value()) {
    stats_.origin_refresh_purges.fetch_add(1, std::memory_order_relaxed);
  }

  // Evict THIS process's RAM-tier copy of the identity: Cyclone's RAM
  // tier is write-around (reads populate it, writes do not evict), so the
  // worker's earlier read of the now-overwritten identity would otherwise
  // keep serving the PRE-refresh bytes to the rebuild below — rebuilding
  // stale variants from a stale original (in production nginx overwrites
  // the identity from another process and the same stale-RAM read
  // happens).
  cache_->EvictAlternateFromRamCache(notification.url, notification.hostname,
                                     notification.scheme, identity_id,
                                     notification.site);

  // Rebuild the variant set immediately from the preserved fresh identity
  // (issue #652 review): without this, the URL would serve the unoptimized
  // original until the NEXT MISS-fill re-notified the worker.  Process
  // inline as a normal notification with the default capability mask —
  // dedup was just cleared and the baseline is captured AFTER the bump, so
  // the write fence admits these writes.  The in-flight URL dedup entry
  // held by this sentinel's dispatch prevents a concurrent normal
  // notification for the same URL from racing the rebuild.
  //
  // Source gate: rebuild only from a
  // GENUINE original at the identity id — never from worker-processed
  // content, which is a stale variant at this point and would re-publish
  // superseded bytes with fresh stamps.  After the preserve rule above, no
  // worker-processed entry can SURVIVE the purge at a selectable id: the
  // preserve list holds only a flag-clear identity and (when still
  // content-bound) the markdown sentinel, which the rebuild's source read
  // never selects (ReadBestAlternate defaults agent_request_entitled to
  // false), and the durable-original fallback reads by exact id a class the
  // purge always removes — only a post-purge substrate re-record can
  // recreate it, fresh by construction.  The refusal is therefore pinned
  // here at the handler level; the source read needs no flag check of its
  // own.  With no genuine source the inline rebuild is skipped and left to
  // the sender's post-commit record+notify, which rebuilds from genuinely
  // fresh bytes (dedup was cleared above) — a bounded handoff, since the
  // next store for the URL re-notifies as a matter of course.
  bool source_is_genuine = false;
  bool source_is_worker_processed = false;
  {
    auto source = cache_->ReadAlternate(notification.url, notification.hostname,
                                        notification.scheme, identity_id,
                                        notification.site);
    if (source.has_value() && source->is_valid()) {
      source_is_worker_processed =
          (source->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) !=
          0;
      source_is_genuine = !source_is_worker_processed;
    }
  }
  if (source_is_genuine) {
    // D3: a rebuild driven purely by the sticky intent flag (the variant was
    // already lost in a prior churn, so had_markdown is false) is the
    // multi-churn-race heal — count it so the lift is observable.
    if (!had_markdown && has_intent) {
      stats_.agent_markdown_rebuild_forced.fetch_add(1,
                                                     std::memory_order_relaxed);
    }
    CacheNotification rebuild =
        MakeOriginRefreshRebuild(notification, had_markdown, has_intent);
    PurgeDispatchGen rebuild_gen =
        CapturePurgeGen(notification.url, notification.hostname,
                        notification.scheme, notification.site);
    HandleNotification(rebuild, rebuild_gen);
  } else if (identity_was_worker_processed || source_is_worker_processed) {
    // The trap this event must close: the identity slot held (or still
    // holds) a worker-processed variant, so there is no genuine refreshed
    // origin to rebuild from.  Count and log the refusal; the next
    // record+notify converges the URL from fresh bytes.
    stats_.origin_refresh_rebuild_refused.fetch_add(1,
                                                    std::memory_order_relaxed);
    LogInfo(
        "Origin-refreshed inline rebuild for %s refused: identity slot held "
        "a worker-processed variant (stale), not the refreshed origin — "
        "rebuild deferred to the next record+notify",
        notification.url.c_str());
  }
}

CacheNotification Worker::MakeOriginRefreshRebuild(
    const CacheNotification& origin, bool had_markdown, bool has_intent) {
  CacheNotification rebuild = origin;
  rebuild.capability_mask = CapabilityMask().Encode();
  // Issue C + durability (#19/D3): propagate agent_request so the
  // warm-template rebuild branch (HandleNotification) re-renders the agent
  // markdown the purge dropped.  Demand-gated: fire when the URL had a markdown
  // variant before the purge (had_markdown) OR ever rendered one (has_intent —
  // heals a variant lost in a prior multi-churn race).  A page that never
  // rendered markdown keeps agent_request=false (no wasted/unwanted render).
  // The actual render stays gated by AgentMarkdownNeedsBuild, so a preserved,
  // still-valid variant is never re-rendered even when agent_request is true.
  rebuild.agent_request = had_markdown || has_intent;
  return rebuild;
}

bool Worker::OriginNegotiatesByAccept(
    const CacheNotification& notification) const {
  if (cache_ == nullptr) return false;
  // Observe the on-disk entries, not this process's RAM copies: the
  // origin-refresh sentinel's pristine read can leave the old unmarked
  // original in RAM, and a marked re-record from another process does not
  // evict it.  Same eviction as ReadPristineOrigin.
  cache_->EvictAlternateFromRamCache(
      notification.url, notification.hostname, notification.scheme,
      static_cast<AlternateId>(SentinelId::kOriginalContent),
      notification.site);
  cache_->EvictAlternateFromRamCache(
      notification.url, notification.hostname, notification.scheme,
      MaskToId(CapabilityMask()), notification.site);
  auto identity = cache_->ReadAlternate(
      notification.url, notification.hostname, notification.scheme,
      MaskToId(CapabilityMask()), notification.site);
  // Only a genuine original carries the origin's marker.  The worker's own
  // variant in this slot (kFlagWorkerProcessed: the recompressed same-format
  // image, minified text) is derived and never inherits it, so answering from
  // it would report "not negotiating" for a URL whose durable original was
  // re-recorded as marked.  Such a slot falls through to the durable original,
  // the same rule ReadPristineOrigin applies.
  if (identity.has_value() && identity->is_valid() &&
      (identity->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) ==
          0) {
    return (identity->metadata.flags &
            AlternateMetadata::kFlagOriginVariesAccept) != 0;
  }

  // No usable identity original.  Failing open here used to cost nothing —
  // "no identity means nothing to optimize from", so the notification
  // produced nothing whatever this answered — and that premise is exactly
  // what the durable-original source fallback removes: a URL with no identity
  // alternate but a durable original IS optimizable now.  Failing open would
  // then derive a variant set for an origin that negotiates on Accept, which
  // is the outcome this guard exists to prevent.  So ask the same question of
  // the entry the source read would actually use.  Still failure-open when
  // there is no original either: nothing to optimize from, as before.
  auto original =
      cache_->ReadOriginalAlternate(notification.url, notification.hostname,
                                    notification.scheme, notification.site);
  return original.has_value() && original->is_valid() &&
         (original->metadata.flags &
          AlternateMetadata::kFlagOriginVariesAccept) != 0;
}

void Worker::HandleWarmupRequest(const CacheNotification& notification,
                                 const PurgeDispatchGen& purge_gen) {
  // A warmup is a request to optimize, so a URL whose origin negotiates on
  // Accept is refused here too.  Repeated rather than hoisted: the warmup
  // sentinel is dispatched BEFORE the general guard, and the image branch
  // below transcodes inline, so it would otherwise derive exactly the variant
  // set the marker forbids.  Hoisting the general guard above the sentinel
  // dispatches is NOT the fix — the origin-refresh sentinel must keep
  // reaching its handler so the purge still runs.
  if (OriginNegotiatesByAccept(notification)) {
    LogInfo("Skipping warmup for %s: origin negotiates on Accept",
            notification.url.c_str());
    stats_.notifications_skipped_dedup.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Snapshot live config for this warmup request (RCU read).
  const auto cfg = live_config_.load();

  // Purge guard for warmup writes (same as HandleNotification).
  PurgeCheck purge_check =
      [this, url = notification.url, hostname = notification.hostname,
       scheme = notification.scheme, site = notification.site, purge_gen]() {
        return WasPurgedSinceDispatch(url, hostname, purge_gen, scheme, site);
      };

  LogInfo("Warmup request for %s (type=%d)", notification.url.c_str(),
          static_cast<int>(notification.content_type));

  // Track hot URL for speculation rules (Phase 1d).
  // Reconstruct full URL from hostname + normalized path so GetHotUrls
  // can filter by hostname (it parses scheme://host from the URL).
  if (cfg->enable_speculation_rules && !notification.hostname.empty()) {
    RecordHotUrl(notification.scheme + "://" + notification.hostname +
                 notification.url);
  }

  if (!cache_) {
    LogWarning("Cache not initialized, skipping warmup");
    return;
  }

  switch (notification.content_type) {
    case ContentType::kImage: {
      if (cfg->disable_image || !image_transcoder_) return;

      // Skip warmup for images already processed by the proactive handler
      // to prevent race-condition overwrites that lose metadata (content
      // class, SSIMULACRA2 scores).  Check the Desktop/Original/Identity
      // variant key — always the first written by the proactive handler.
      {
        CapabilityMask dedup_mask;  // Desktop/Identity default
        dedup_mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
        AlternateId dedup_id = MaskToAlternateId(dedup_mask.Encode() & 0xFF);
        std::string dedup_key = ComposeInternalKeyWithId(
            notification.url, notification.hostname, notification.scheme,
            notification.site, dedup_id);
        std::lock_guard<std::mutex> lock(processed_set_mutex_);
        if (processed_current_.contains(dedup_key) ||
            processed_previous_.contains(dedup_key)) {
          LogInfo(
              "Warmup: skipping image %s (already processed by "
              "proactive handler)",
              notification.url.c_str());
          return;
        }
      }

      // Read original image.  Same source read as the notification branches —
      // selector first, durable original by exact id when it finds nothing —
      // because this is the same question about the same URL.  The bare
      // (non-retrying) form is kept on purpose: a warmup is speculative, and
      // sleeping out the cold-start backoff for one was never its behaviour.
      stats_.selector_invocations.fetch_add(1, std::memory_order_relaxed);
      auto img_read = ReadSourceOnce(cache_.get(), notification.url,
                                     notification.hostname, notification.scheme,
                                     notification.site, stats_, handler_);
      if (!img_read.has_value()) return;

      // Skip unsupported image types in warmup path too.
      std::string_view warmup_ct = img_read->metadata.origin_content_type;
      if (!warmup_ct.empty() && !warmup_ct.starts_with("image/jpeg") &&
          !warmup_ct.starts_with("image/png") &&
          !warmup_ct.starts_with("image/gif") &&
          !warmup_ct.starts_with("image/webp")) {
        return;
      }

      auto img_span = img_read->content();
      std::string_view image_data(
          reinterpret_cast<const char*>(img_span.data()), img_span.size());
      if (image_data.size() > cfg->max_image_size) return;

      // De-alias the origin borrow (issue #934) — same contract as the
      // proactive path: copy once, release the handle, and transcode from
      // the copy so the warmup matrix never defers its own writes behind
      // the worker's own read lease at cache-full.
      const std::string origin_bytes(image_data);
      AlternateMetadata origin_meta = img_read->metadata;
      // Verify the copy is untorn before releasing (issue #934):
      // same contract as the proactive image path.  On a wrap-torn copy,
      // skip this warmup URL — a later notification re-reads a settled slot.
      if (!BorrowCopyUntorn(*img_read, cfg->read_lease_duration_ms > 0)) {
        stats_.read_borrow_wrap_discards.fetch_add(1,
                                                   std::memory_order_relaxed);
        LogWarning(
            "Origin image borrow (warmup) wrapped during de-alias copy, "
            "skipping %s",
            notification.url.c_str());
        return;
      }
      img_read->release();
      image_data = origin_bytes;
      // Source binding (issue #434), same rule as the notification pass:
      // stamp the warmup matrix with the SHA-256 of the raw input; a
      // worker-processed source keeps the inherited stamp.
      if ((origin_meta.flags & AlternateMetadata::kFlagWorkerProcessed) == 0) {
        StampSourceBinding(
            &origin_meta,
            cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(origin_bytes.data()),
                origin_bytes.size())));
      }

      const CapabilityMask::ImageFormat all_fmts[] = {
          CapabilityMask::ImageFormat::kWebP,
          CapabilityMask::ImageFormat::kAvif,
          CapabilityMask::ImageFormat::kOriginal,
      };
      const CapabilityMask::Viewport warmup_viewports[] = {
          CapabilityMask::Viewport::kMobile,
          CapabilityMask::Viewport::kTablet,
          CapabilityMask::Viewport::kDesktop,
      };
      const CapabilityMask::SaveData warmup_savedata[] = {
          CapabilityMask::SaveData::kOff,
          CapabilityMask::SaveData::kOn,
      };
      const CapabilityMask::PixelDensity warmup_densities[] = {
          CapabilityMask::PixelDensity::k1x,
          CapabilityMask::PixelDensity::k2xPlus,
      };

      // Detect the source JPEG quality once for all warmup variants; a JPEG
      // re-encode is capped at it (#1284).  -1 when it is not a JPEG or the
      // quality cannot be determined.
      const int warmup_source_jpeg_quality =
          ImageTranscoder::DetectSourceJpegQuality(image_data, handler_);

      // Build warmup variant metadata from the original image's metadata.
      AlternateMetadata warmup_img_meta = origin_meta;
      warmup_img_meta.content_type = ContentType::kImage;
      warmup_img_meta.flags = AlternateMetadata::kFlagWorkerProcessed |
                              InheritedVariantFlags(warmup_img_meta);
      warmup_img_meta.origin_content_length = static_cast<uint32_t>(
          std::min(image_data.size(), size_t{UINT32_MAX}));
      // origin_content_type inherited from img_read->metadata

      // Fetch all existing alternate IDs in a single disk traversal.
      std::unordered_set<uint8_t> existing_ids;
      std::optional<std::vector<cyclone::AlternateInfo>> warmup_listed;
      {
        auto alts =
            cache_->ListAlternates(notification.url, notification.hostname,
                                   notification.scheme, notification.site);
        if (alts.has_value()) {
          for (const auto& alt : *alts) {
            existing_ids.insert(static_cast<uint8_t>(alt.id));
          }
          warmup_listed = std::move(*alts);
        }
      }
      // Same identity-slot and size-gate rules as the notification pass
      // (WriteImageVariants).
      const bool warmup_identity_raw =
          IdentityHoldsRawOriginal(cache_.get(), notification.url,
                                   notification.hostname, notification.scheme,
                                   notification.site, existing_ids) &&
          PreserveIdentityAsDurableOriginal(
              cache_.get(), notification.url, notification.hostname,
              notification.scheme, notification.site,
              cfg->read_lease_duration_ms > 0);
      if (warmup_identity_raw) {
        existing_ids.erase(static_cast<uint8_t>(MaskToId(CapabilityMask())));
      }
      VariantSizeGates warmup_gates(
          warmup_listed ? &*warmup_listed : nullptr, warmup_identity_raw,
          [this, &notification](const std::string& msg) {
            LogInfo("Warmup: %s for %s", msg.c_str(), notification.url.c_str());
          });

      auto write_warmup = [&](const TranscodeResult& tr,
                              CapabilityMask::ImageFormat fmt,
                              CapabilityMask::Viewport vp,
                              CapabilityMask::PixelDensity den,
                              CapabilityMask::SaveData sd) {
        if (!tr.success) return;
        CapabilityMask wm;  // Desktop/Identity base
        wm.set_image_format(fmt);
        wm.set_viewport(vp);
        wm.set_pixel_density(den);
        wm.set_save_data(sd);
        // Skip if the proactive handler already wrote this variant
        // (avoid overwriting metadata set by the proactive path).  A raw
        // identity slot is not such a variant: it is replaced.
        const bool replaces_raw_identity =
            warmup_identity_raw && MaskToId(wm) == MaskToId(CapabilityMask());
        if (!replaces_raw_identity &&
            VariantExistsForMask(cache_.get(), notification.url,
                                 notification.hostname, notification.scheme,
                                 notification.site, wm)) {
          return;
        }
        stats_.alternate_writes.fetch_add(1, std::memory_order_relaxed);
        bool wr = WriteVariant(
            cache_.get(), notification.url, notification.hostname,
            notification.scheme, notification.site, wm,
            std::span<const char>(tr.output_data.data(), tr.output_data.size()),
            warmup_img_meta, purge_check, this);
        if (wr) {
          stats_.variants_written.fetch_add(1, std::memory_order_relaxed);
          stats_.proactive_variants_written.fetch_add(
              1, std::memory_order_relaxed);
          stats_.images_processed.fetch_add(1, std::memory_order_relaxed);
          if (tr.output_mime_type == "image/webp") {
            stats_.webp_generated.fetch_add(1, std::memory_order_relaxed);
          } else if (tr.output_mime_type == "image/avif") {
            stats_.avif_generated.fetch_add(1, std::memory_order_relaxed);
          } else if (tr.output_mime_type == "image/jpeg") {
            stats_.jpeg_optimized.fetch_add(1, std::memory_order_relaxed);
          } else if (tr.output_mime_type == "image/png") {
            stats_.png_optimized.fetch_add(1, std::memory_order_relaxed);
          }
          LogInfo("Warmup: wrote %s variant for %s (%zu bytes)",
                  tr.output_mime_type.c_str(), notification.url.c_str(),
                  tr.output_data.size());
        } else {
          if (VariantExistsForMask(cache_.get(), notification.url,
                                   notification.hostname, notification.scheme,
                                   notification.site, wm)) {
            LogInfo(
                "Warmup: skipped image variant %s (mask 0x%02X, "
                "already written by concurrent thread)",
                notification.url.c_str(), static_cast<int>(wm.Encode() & 0xFF));
          } else {
            stats_.errors.fetch_add(1, std::memory_order_relaxed);
            stats_.alternate_write_failures.fetch_add(
                1, std::memory_order_relaxed);
            LogWarning(
                "Warmup: write failed for image variant %s (mask "
                "0x%02X, %s, %zu bytes)",
                notification.url.c_str(), static_cast<int>(wm.Encode() & 0xFF),
                tr.output_mime_type.c_str(), tr.output_data.size());
          }
        }
      };

      // Decline tombstone (#1382), warmup leg: the warmup matrix must not
      // re-pay the attempt ladder for slots the verify already refused for
      // this source either. Same consult/record contract as the proactive
      // handler above, including the verifier-toggle gate. The cheap
      // existence check runs first, so a URL with no tombstone (the
      // common case) never pays the full-image SHA-256.
      const bool warmup_may_consult =
          cfg->quality_verify && existing_ids.count(static_cast<AlternateId>(
                                     SentinelId::kDeclineTombstone)) != 0;
      std::array<std::byte, 32> warmup_origin_hash{};
      std::vector<DeclineTombstoneRecord> warmup_tombstoned;
      if (warmup_may_consult) {
        warmup_origin_hash =
            cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(image_data.data()),
                image_data.size()));
        warmup_tombstoned = LoadDeclineTombstone(notification, existing_ids,
                                                 warmup_origin_hash);
      }
      std::vector<DeclineTombstoneRecord> warmup_new_records;

      bool any_missing = false;
      for (auto sd : warmup_savedata) {
        if (sd != CapabilityMask::SaveData::kOff &&
            !cfg->proactive_savedata_variants) {
          continue;
        }
        int learned_jpeg_quality = -1;
        for (auto den : warmup_densities) {
          if (den != CapabilityMask::PixelDensity::k1x &&
              !cfg->proactive_density_variants) {
            continue;
          }
          for (auto vp : warmup_viewports) {
            if (vp != CapabilityMask::Viewport::kDesktop &&
                !cfg->proactive_viewport_variants) {
              continue;
            }

            CapabilityMask base_mask;
            base_mask.set_viewport(vp);
            base_mask.set_pixel_density(den);
            base_mask.set_save_data(sd);
            auto missing = FindMissingFormats(base_mask, vp, den, sd, all_fmts,
                                              existing_ids);

            if (!warmup_tombstoned.empty()) {
              std::erase_if(missing, [&](auto fmt) {
                const uint8_t slot = VariantSlotId(fmt, vp, den, sd);
                for (const DeclineTombstoneRecord& rec : warmup_tombstoned) {
                  if (rec.slot == slot) {
                    stats_.ssimulacra2_decline_tombstone_hits.fetch_add(
                        1, std::memory_order_relaxed);
                    LogInfo(
                        "SSIMULACRA2 decline tombstone hit for %s (slot "
                        "0x%02X): variant already declined for this source, "
                        "skipping recompute",
                        notification.url.c_str(), static_cast<int>(slot));
                    return true;
                  }
                }
                return false;
              });
            }

            if (missing.empty()) continue;
            any_missing = true;

            auto multi = image_transcoder_->TranscodeMultiResized(
                image_data, missing, vp, den, sd,
                {.carried_hint = learned_jpeg_quality,
                 .source_quality = warmup_source_jpeg_quality});
            if (multi.final_jpeg_quality > 0) {
              learned_jpeg_quality = multi.final_jpeg_quality;
            }

            // Propagate content class from transcode result so warmup
            // writes carry the same metadata as the proactive handler.
            warmup_img_meta.content_class =
                static_cast<uint8_t>(multi.applied_preset.content_class);

            // Helper to set per-format SSIMULACRA2 score in metadata.
            auto set_warmup_ssim = [&](float score) {
              if (score >= 0.0f) {
                warmup_img_meta.SetQualityScoreX100(
                    static_cast<uint16_t>(std::min(score * 100.0f, 10000.0f)));
              } else {
                warmup_img_meta.SetQualityScoreX100(std::nullopt);  // N/A
              }
            };

            warmup_gates.Apply(multi, vp, den, sd);
            RecordSizeGateStats(stats_, multi);

            // One line per requested slot left unwritten, with the reason.
            for (auto fmt : missing) {
              const TranscodeResult& tr = ResultForFormat(multi, fmt);
              if (tr.success) continue;
              LogInfo(
                  "Warmup: image variant not written for %s (slot 0x%02X): %s",
                  notification.url.c_str(),
                  static_cast<int>(VariantSlotId(fmt, vp, den, sd)),
                  tr.error_message.empty() ? "no result from the transcoder"
                                           : tr.error_message.c_str());
            }

            set_warmup_ssim(multi.webp_ssimulacra2_score);
            write_warmup(multi.webp, CapabilityMask::ImageFormat::kWebP, vp,
                         den, sd);
            set_warmup_ssim(multi.avif_ssimulacra2_score);
            write_warmup(multi.avif, CapabilityMask::ImageFormat::kAvif, vp,
                         den, sd);
            set_warmup_ssim(multi.ssimulacra2_score);
            write_warmup(multi.optimized_original,
                         CapabilityMask::ImageFormat::kOriginal, vp, den, sd);

            // Tombstone declined slots, same contract as the proactive
            // path (#1382).
            auto record_warmup_decline = [&](CapabilityMask::ImageFormat fmt,
                                             bool declined, int quality) {
              if (!declined) return;
              DeclineTombstoneRecord rec;
              rec.slot = VariantSlotId(fmt, vp, den, sd);
              rec.quality = (quality > 0 && quality <= 100)
                                ? static_cast<uint8_t>(quality)
                                : 0xFF;
              warmup_new_records.push_back(rec);
            };
            record_warmup_decline(CapabilityMask::ImageFormat::kWebP,
                                  multi.webp_ssimulacra2_declined,
                                  multi.final_webp_quality);
            record_warmup_decline(CapabilityMask::ImageFormat::kAvif,
                                  multi.avif_ssimulacra2_declined,
                                  multi.final_avif_quality);
            record_warmup_decline(CapabilityMask::ImageFormat::kOriginal,
                                  multi.ssimulacra2_declined,
                                  multi.final_jpeg_quality);
          }
        }
      }

      if (!any_missing) {
        LogInfo("Warmup: all variants exist for %s", notification.url.c_str());
      }

      if (!warmup_new_records.empty()) {
        if (!warmup_may_consult) {
          // Declines without a prior consult: the hash was not needed
          // above, but the store keys on it.
          warmup_origin_hash =
              cyclone::crypto::SHA256::hash(std::span<const std::byte>(
                  reinterpret_cast<const std::byte*>(image_data.data()),
                  image_data.size()));
        }
        StoreDeclineTombstone(notification, warmup_origin_hash,
                              warmup_tombstoned, std::move(warmup_new_records));
      }
      break;
    }

    case ContentType::kCss: {
      if (cfg->disable_css) return;

      // Re-dispatch as a normal CSS notification with default mask.
      // CSS/JS minification overwrites the original at the default
      // mask, and the normal handler's "only write if smaller" check
      // prevents redundant writes.
      CacheNotification css_notif = notification;
      CapabilityMask css_mask;
      css_notif.capability_mask = css_mask.Encode();
      HandleNotification(css_notif, purge_gen);
      break;
    }

    case ContentType::kJs: {
      if (cfg->disable_js) return;

      CacheNotification js_notif = notification;
      CapabilityMask js_mask;
      js_notif.capability_mask = js_mask.Encode();
      HandleNotification(js_notif, purge_gen);
      break;
    }

    default:
      break;
  }
}

// =================================================================
// Management socket
// =================================================================

void Worker::OnMgmtConnection(uv_stream_t* server, int status) {
  auto* worker = static_cast<Worker*>(server->data);

  if (status < 0) {
    return;
  }

  // Enforce management connection limit.
  static constexpr int kMaxMgmtConnections = 16;
  int current =
      worker->mgmt_active_connections_.fetch_add(1, std::memory_order_relaxed);
  if (current >= kMaxMgmtConnections) {
    worker->mgmt_active_connections_.fetch_sub(1, std::memory_order_relaxed);
    // Accept and immediately close to drain the kernel backlog.
    auto* reject = new uv_pipe_t();
    uv_pipe_init(worker->loop_.get(), reject, 0);
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(reject)) == 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(reject),
               [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
    } else {
      uv_close(reinterpret_cast<uv_handle_t*>(reject),
               [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
    }
    return;
  }

  auto* client = new uv_pipe_t();
  uv_pipe_init(worker->loop_.get(), client, 0);

  if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client)) != 0) {
    worker->mgmt_active_connections_.fetch_sub(1, std::memory_order_relaxed);
    uv_close(reinterpret_cast<uv_handle_t*>(client),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
    return;
  }

  auto* ctx = new MgmtClientContext();
  ctx->worker = worker;
  client->data = ctx;

  uv_read_start(reinterpret_cast<uv_stream_t*>(client), OnMgmtAlloc,
                OnMgmtRead);
}

void Worker::OnMgmtAlloc(uv_handle_t* /*handle*/, size_t suggested_size,
                         uv_buf_t* buf) {
  buf->base = new char[suggested_size];
  buf->len = suggested_size;
}

void Worker::OnMgmtRead(uv_stream_t* client, ssize_t nread,
                        const uv_buf_t* buf) {
  auto* ctx = static_cast<MgmtClientContext*>(client->data);

  if (nread < 0) {
    delete[] buf->base;
    if (ctx->closing) return;  // Already being closed by OnMgmtWriteDone
    ctx->closing = true;
    uv_close(reinterpret_cast<uv_handle_t*>(client), [](uv_handle_t* h) {
      auto* c = static_cast<MgmtClientContext*>(
          reinterpret_cast<uv_pipe_t*>(h)->data);
      c->worker->mgmt_active_connections_.fetch_sub(1,
                                                    std::memory_order_relaxed);
      delete c;
      delete reinterpret_cast<uv_pipe_t*>(h);
    });
    return;
  }

  if (nread > 0) {
    ctx->buffer.append(buf->base, nread);

    // Process complete lines
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
      std::string command = ctx->buffer.substr(0, pos);
      ctx->buffer.erase(0, pos + 1);
      ctx->worker->HandleMgmtCommand(client, command);
      if (ctx->closing) break;  // HandleMgmtCommand may trigger close on
                                // synchronous write failure — stop processing.
    }

    // Safety: disconnect if buffer grows too large without a newline
    if (ctx->buffer.size() > 16384) {
      if (ctx->closing) return;
      ctx->closing = true;
      uv_close(reinterpret_cast<uv_handle_t*>(client), [](uv_handle_t* h) {
        auto* c = static_cast<MgmtClientContext*>(
            reinterpret_cast<uv_pipe_t*>(h)->data);
        c->worker->mgmt_active_connections_.fetch_sub(
            1, std::memory_order_relaxed);
        delete c;
        delete reinterpret_cast<uv_pipe_t*>(h);
      });
    }
  }

  delete[] buf->base;
}

void Worker::OnMgmtWriteDone(uv_write_t* req, int /*status*/) {
  auto* ctx = static_cast<MgmtWriteContext*>(req->data);
  delete[] ctx->buf.base;

  // AUTH responses keep the connection alive so the client can send
  // a follow-up PURGE on the same authenticated connection.
  if (ctx->keep_alive) {
    delete ctx;
    return;
  }

  // Close after response (one-shot for non-AUTH commands)
  auto* mgmt_ctx = static_cast<MgmtClientContext*>(ctx->client->data);
  if (mgmt_ctx->closing) {
    // Already being closed by OnMgmtRead (EOF/error)
    delete ctx;
    return;
  }
  mgmt_ctx->closing = true;
  uv_close(reinterpret_cast<uv_handle_t*>(ctx->client), [](uv_handle_t* h) {
    auto* c =
        static_cast<MgmtClientContext*>(reinterpret_cast<uv_pipe_t*>(h)->data);
    c->worker->mgmt_active_connections_.fetch_sub(1, std::memory_order_relaxed);
    delete c;
    delete reinterpret_cast<uv_pipe_t*>(h);
  });
  delete ctx;
}

void Worker::HandleMgmtCommand(uv_stream_t* client,
                               const std::string& command) {
  std::string response;
  bool keep_alive = false;  // Keep connection open after this response
  auto* ctx = static_cast<MgmtClientContext*>(
      reinterpret_cast<uv_pipe_t*>(client)->data);

  // AUTH <token> — authenticate this connection for PURGE commands.
  // The connection stays open after AUTH so the client can follow up
  // with PURGE on the same authenticated connection.
  if (command.starts_with("AUTH ")) {
    if (purge_token_.empty()) {
      response = "ERR no token configured\n";
    } else {
      std::string_view provided = std::string_view(command).substr(5);
      if (net_instaweb::ConstantTimeCompare(provided, purge_token_)) {
        ctx->authenticated = true;
        response = "OK\n";
        keep_alive = true;  // Stay open for follow-up PURGE
      } else {
        response = "ERR invalid token\n";
      }
    }
  } else if (command.starts_with("PURGE ")) {
    // PURGE requires authentication (fail-closed).  When no token is
    // configured, PURGE is unavailable — admin must set
    // PAGESPEED_PURGE_TOKEN to enable cache invalidation.
    if (!ctx->authenticated) {
      response = purge_token_.empty() ? "ERR no purge token configured\n"
                                      : "ERR authentication required\n";
    } else {
      // Wire format: PURGE <hostname> <url>
      std::string_view args = std::string_view(command).substr(6);
      auto space = args.find(' ');
      if (space == std::string_view::npos || space == 0 ||
          space + 1 >= args.size()) {
        response = "ERR usage: PURGE <hostname> <url>\n";
      } else {
        std::string hostname(args.substr(0, space));
        std::string url(args.substr(space + 1));
        if (url.size() > config_.max_url_length) {
          response = "ERR URL too long\n";
        } else {
          // Every keyspace the URL can live in: each known site, and the
          // unsited one.
          int deleted = 0;
          std::vector<std::string> sites = url_registry_->KnownSites();
          sites.emplace_back();
          for (const std::string& site : sites) {
            deleted += InvalidateUrl(url, hostname, "https", site);
            deleted += InvalidateUrl(url, hostname, "http", site);
          }
          response = "OK " + std::to_string(deleted) + " entries deleted";
          if (deleted == 0) {
            response +=
                " (URL not found — may have been evicted or never cached)";
          }
          response += "\n";
        }
      }
    }
  } else if (command == "STATS") {
    // Build JSON stats response (uses cached values to avoid
    // contending with Cyclone locks on the event loop).
    uint64_t cache_entries =
        cached_cache_entries_.load(std::memory_order_relaxed);
    uint64_t cache_size = cached_cache_bytes_.load(std::memory_order_relaxed);
    const auto& s = stats_;
    response = absl::StrCat(
        "{\"status\":\"ok\","
        "\"connections\":{\"active\":",
        active_connections_.load(), ",\"max\":", config_.max_connections,
        "},\"notifications\":{\"received\":",
        s.notifications_received.load(std::memory_order_relaxed),
        ",\"skipped_dedup\":",
        s.notifications_skipped_dedup.load(std::memory_order_relaxed),
        ",\"skipped_inflight\":",
        s.notifications_skipped_inflight.load(std::memory_order_relaxed),
        ",\"dedup_healed\":",
        s.notifications_dedup_healed.load(std::memory_order_relaxed),
        ",\"missing_copy_healed\":",
        s.notifications_missing_copy_healed.load(std::memory_order_relaxed),
        "},\"variants\":{\"written\":",
        s.variants_written.load(std::memory_order_relaxed), ",\"proactive\":",
        s.proactive_variants_written.load(std::memory_order_relaxed),
        ",\"gzip\":", s.gzip_variants_written.load(std::memory_order_relaxed),
        ",\"brotli\":",
        s.brotli_variants_written.load(std::memory_order_relaxed),
        "},\"errors\":", s.errors.load(std::memory_order_relaxed));
    absl::StrAppend(
        &response, ",\"cache\":{\"entries\":", cache_entries,
        ",\"size\":", cache_size, "},\"by_type\":{\"html\":{\"n\":",
        s.html_processed.load(std::memory_order_relaxed),
        ",\"us\":", s.html_processing_time_us.load(std::memory_order_relaxed),
        "},\"css\":{\"n\":", s.css_processed.load(std::memory_order_relaxed),
        ",\"us\":", s.css_processing_time_us.load(std::memory_order_relaxed),
        "},\"js\":{\"n\":", s.js_processed.load(std::memory_order_relaxed),
        ",\"us\":", s.js_processing_time_us.load(std::memory_order_relaxed),
        "},\"images\":{\"n\":",
        s.images_processed.load(std::memory_order_relaxed),
        ",\"us\":", s.image_processing_time_us.load(std::memory_order_relaxed),
        "}}");
    absl::StrAppend(
        &response, ",\"by_format\":{\"webp\":",
        s.webp_generated.load(std::memory_order_relaxed),
        ",\"avif\":", s.avif_generated.load(std::memory_order_relaxed),
        ",\"jpeg\":", s.jpeg_optimized.load(std::memory_order_relaxed),
        ",\"png\":", s.png_optimized.load(std::memory_order_relaxed),
        "},\"timing_us\":{\"total\":",
        s.total_processing_time_us.load(std::memory_order_relaxed),
        "},\"html_assembly\":{\"complete\":",
        s.html_assembly_complete.load(std::memory_order_relaxed),
        ",\"skipped\":",
        s.html_assembly_skipped.load(std::memory_order_relaxed),
        ",\"css_aborted\":",
        s.critical_css_aborted.load(std::memory_order_relaxed), "}");
    absl::StrAppend(
        &response, ",\"alternates\":{\"writes\":",
        s.alternate_writes.load(std::memory_order_relaxed),
        ",\"write_failures\":",
        s.alternate_write_failures.load(std::memory_order_relaxed),
        "},\"origin_refresh\":{\"purges\":",
        s.origin_refresh_purges.load(std::memory_order_relaxed),
        ",\"rate_limited\":",
        s.origin_refresh_rate_limited.load(std::memory_order_relaxed),
        ",\"rebuild_refused\":",
        s.origin_refresh_rebuild_refused.load(std::memory_order_relaxed),
        "},\"selector_invocations\":",
        s.selector_invocations.load(std::memory_order_relaxed),
        ",\"source_reads_from_durable_original\":",
        s.source_reads_from_durable_original.load(std::memory_order_relaxed),
        ",\"cache_read_retries\":",
        s.cache_read_retries.load(std::memory_order_relaxed),
        ",\"cache_read_failures\":",
        s.cache_read_failures.load(std::memory_order_relaxed),
        ",\"content_analysis\":{\"photo\":",
        s.content_photo.load(std::memory_order_relaxed), ",\"screenshot\":",
        s.content_screenshot.load(std::memory_order_relaxed),
        ",\"illustration\":",
        s.content_illustration.load(std::memory_order_relaxed),
        ",\"noisy\":", s.content_noisy.load(std::memory_order_relaxed),
        ",\"denoised\":0", "}");
    absl::StrAppend(
        &response, ",\"ssimulacra2\":{\"checks\":",
        s.ssimulacra2_checks.load(std::memory_order_relaxed), ",\"reencodes\":",
        s.ssimulacra2_reencodes.load(std::memory_order_relaxed),
        ",\"declines\":",
        s.ssimulacra2_declines.load(std::memory_order_relaxed),
        ",\"tombstone_hits\":",
        s.ssimulacra2_decline_tombstone_hits.load(std::memory_order_relaxed),
        ",\"avg_score_x100\":",
        s.ssimulacra2_total_score_x100.load(std::memory_order_relaxed), "}");
    absl::StrAppend(
        &response, ",\"svg\":{\"candidates_evaluated\":",
        s.svg_candidates_evaluated.load(std::memory_order_relaxed),
        ",\"candidates_rejected\":",
        s.svg_candidates_rejected.load(std::memory_order_relaxed),
        ",\"vectorized\":", s.svg_vectorized.load(std::memory_order_relaxed),
        ",\"fidelity_rejected\":",
        s.svg_fidelity_rejected.load(std::memory_order_relaxed),
        ",\"size_rejected\":",
        s.svg_size_rejected.load(std::memory_order_relaxed),
        ",\"path_count_rejected\":",
        s.svg_path_count_rejected.load(std::memory_order_relaxed),
        ",\"written\":", s.svg_written.load(std::memory_order_relaxed),
        ",\"bytes_saved\":", s.svg_bytes_saved.load(std::memory_order_relaxed),
        ",\"vectorize_time_us\":",
        s.svg_vectorize_time_us.load(std::memory_order_relaxed),
        // served = serve-time SVG HITs from the cross-process ServeStats mmap
        // (the WorkerStats counter was unwritable / always 0 — the
        // follow-up in #455). 0 when the serve-stats file isn't mapped yet.
        ",\"served\":",
        serve_stats_ != nullptr ? *reinterpret_cast<const volatile uint64_t*>(
                                      &serve_stats_->svg_optimized_hits)
                                : uint64_t{0},
        "}");
    absl::StrAppend(
        &response, ",\"image_incomplete_matrices\":",
        s.image_incomplete_matrices.load(std::memory_order_relaxed),
        ",\"image_no_savings_skipped\":",
        s.image_no_savings_skipped.load(std::memory_order_relaxed),
        ",\"image_unconverted_fallthrough\":",
        s.image_unconverted_fallthrough.load(std::memory_order_relaxed),
        ",\"image_size_refused\":{\"webp\":",
        s.image_size_refused_webp.load(std::memory_order_relaxed),
        ",\"avif\":", s.image_size_refused_avif.load(std::memory_order_relaxed),
        "},\"image_savedata_refused\":",
        s.image_savedata_refused.load(std::memory_order_relaxed),
        ",\"image_budget_rescued\":{\"webp\":",
        s.image_budget_rescued_webp.load(std::memory_order_relaxed),
        ",\"avif\":",
        s.image_budget_rescued_avif.load(std::memory_order_relaxed), "}");
    absl::StrAppend(&response, ",\"dedup\":{\"writes_skipped\":",
                    s.dedup_writes_skipped.load(std::memory_order_relaxed),
                    ",\"content_hash_hits\":",
                    s.content_hash_hits.load(std::memory_order_relaxed),
                    ",\"content_hash_stale\":",
                    s.content_hash_stale.load(std::memory_order_relaxed), "}");
    absl::StrAppend(&response, ",\"thread_pool\":{\"inflight\":",
                    in_flight_work_.load(std::memory_order_relaxed),
                    ",\"size\":", config_.num_threads, "}");
    if (browser_manager_) {
      const auto& bs = browser_manager_->stats();
      absl::StrAppend(
          &response, ",\"browser\":{\"enabled\":true,\"chrome_running\":",
          browser_manager_->chrome_running() ? "true" : "false",
          ",\"profiles_generated\":",
          bs.profiles_generated.load(std::memory_order_relaxed),
          ",\"profiles_used\":",
          bs.profiles_used.load(std::memory_order_relaxed),
          ",\"analysis_errors\":",
          bs.analysis_errors.load(std::memory_order_relaxed),
          ",\"chrome_crashes\":",
          bs.chrome_crashes.load(std::memory_order_relaxed),
          ",\"chrome_consecutive_failures\":",
          bs.chrome_consecutive_failures.load(std::memory_order_relaxed),
          ",\"chrome_restart_delay_ms\":",
          bs.chrome_restart_delay_ms.load(std::memory_order_relaxed),
          ",\"queue_depth\":", browser_manager_->queue_depth(),
          ",\"css_inlining_attempted\":",
          bs.css_inlining_attempted.load(std::memory_order_relaxed),
          ",\"css_inlining_stylesheets_found\":",
          bs.css_inlining_stylesheets_found.load(std::memory_order_relaxed),
          ",\"css_inlining_stylesheets_cached\":",
          bs.css_inlining_stylesheets_cached.load(std::memory_order_relaxed),
          ",\"css_inlining_bytes_inlined\":",
          bs.css_inlining_bytes_inlined.load(std::memory_order_relaxed),
          ",\"reanalyses_scheduled\":",
          bs.reanalyses_scheduled.load(std::memory_order_relaxed), "}");
    }
    if (serve_stats_ != nullptr) {
      auto* ss = serve_stats_;
      auto ld = [](const uint64_t& f) -> uint64_t {
        return *reinterpret_cast<const volatile uint64_t*>(&f);
      };
      absl::StrAppend(
          &response, ",\"serve_savings\":{\"html\":{\"original_bytes\":",
          ld(ss->html_original_bytes),
          ",\"optimized_bytes\":", ld(ss->html_optimized_bytes),
          ",\"hits\":", ld(ss->html_optimized_hits),
          "},\"css\":{\"original_bytes\":", ld(ss->css_original_bytes),
          ",\"optimized_bytes\":", ld(ss->css_optimized_bytes),
          ",\"hits\":", ld(ss->css_optimized_hits),
          "},\"js\":{\"original_bytes\":", ld(ss->js_original_bytes),
          ",\"optimized_bytes\":", ld(ss->js_optimized_bytes),
          ",\"hits\":", ld(ss->js_optimized_hits),
          "},\"image\":{\"original_bytes\":", ld(ss->image_original_bytes),
          ",\"optimized_bytes\":", ld(ss->image_optimized_bytes),
          ",\"hits\":", ld(ss->image_optimized_hits), "}}");
    }
    absl::StrAppend(&response, "}\n");
  } else if (command == "METRICS") {
    BuildPrometheusMetrics(response);
  } else if (command == "BROWSER-STATUS") {
    response = browser_manager_ ? browser_manager_->StatusJson() + "\n"
                                : "{\"enabled\":false}\n";
  } else {
    response = "ERR unknown command\n";
  }

  // Send response
  auto* wctx = new MgmtWriteContext();
  wctx->buf.base = new char[response.size()];
  memcpy(wctx->buf.base, response.data(), response.size());
  wctx->buf.len = response.size();
  wctx->client = reinterpret_cast<uv_pipe_t*>(client);
  wctx->keep_alive = keep_alive;
  wctx->req.data = wctx;

  int r = uv_write(&wctx->req, client, &wctx->buf, 1, OnMgmtWriteDone);
  if (r != 0) {
    // Write failed synchronously — callback won't fire.  Clean up.
    delete[] wctx->buf.base;
    auto* mgmt_ctx = static_cast<MgmtClientContext*>(wctx->client->data);
    if (!mgmt_ctx->closing) {
      mgmt_ctx->closing = true;
      uv_close(reinterpret_cast<uv_handle_t*>(wctx->client),
               [](uv_handle_t* h) {
                 auto* c = static_cast<MgmtClientContext*>(
                     reinterpret_cast<uv_pipe_t*>(h)->data);
                 c->worker->mgmt_active_connections_.fetch_sub(
                     1, std::memory_order_relaxed);
                 delete c;
                 delete reinterpret_cast<uv_pipe_t*>(h);
               });
    }
    delete wctx;
  }
}

void Worker::PurgeDerivedCopiesAndBumpGeneration(const std::string& norm_url,
                                                 const std::string& norm_host,
                                                 const std::string& scheme,
                                                 std::string_view site) {
  std::string gen_key = ComposeInternalKey(norm_url, norm_host, scheme, site);
  std::lock_guard<std::mutex> lock(purge_gen_mutex_);
  BumpPurgeGenerationLocked(gen_key);
  auto purged =
      cache_->RemoveDerivedAlternates(norm_url, norm_host, scheme, site);
  if (!purged.has_value()) {
    LogWarning(
        "Purge of derived copies for %s failed (%s): stale copies may remain "
        "until the next notification",
        norm_url.c_str(), make_error_code(purged.error()).message().c_str());
  }
}

int Worker::PurgeUrlAndBumpGeneration(const std::string& norm_url,
                                      const std::string& norm_host,
                                      const std::string& scheme,
                                      std::string_view site) {
  if (!cache_) return 0;
  std::string gen_key = ComposeInternalKey(norm_url, norm_host, scheme, site);
  // Hold purge_gen_mutex_ ACROSS the generation bump and the Remove
  // (issue #652).  Dispatch-time baseline capture takes the same mutex,
  // so any notification that observes the new generation is guaranteed to
  // read its source content only after the Remove completed; notifications
  // holding the old generation are rejected at the write fence.  Without
  // the spanning, a notification dispatched between the bump and the
  // Remove could read pre-purge content, pass every generation check, and
  // resurrect it.  Deadlock-safe: Remove() only takes Cyclone's SHARED
  // reset lock and no code path acquires purge_gen_mutex_ while holding a
  // Cyclone lock (see lock-ordering comment in worker.h).
  std::lock_guard<std::mutex> lock(purge_gen_mutex_);
  BumpPurgeGenerationLocked(gen_key);
  auto result = cache_->Remove(norm_url, norm_host, scheme, site);
  return result.has_value() ? 1 : 0;
}

void Worker::BumpPurgeGenerationLocked(const std::string& gen_key) {
  // Seed from the previous map when the key is absent from the current one
  // (issue #652 review): after a rotation, a key with generation N lives
  // only in `previous`; a bare `++purge_generation_[gen_key]` would insert
  // current[key]=1, and a notification holding baseline N would evaluate
  // `1 > N` == false — the write fence, the post-commit re-check AND the
  // MarkVariantProcessed guard all silently bypassed until N more purges
  // occur.  Generations must stay monotonic across rotations.
  auto [it, inserted] = purge_generation_.try_emplace(gen_key, 0);
  if (inserted) {
    if (auto pit = purge_generation_previous_.find(gen_key);
        pit != purge_generation_previous_.end()) {
      it->second = pit->second;
      purge_generation_previous_.erase(pit);
    }
  }
  ++it->second;
  // Generational overflow: rotate current → previous instead of
  // clearing everything.  Recent entries survive in the new set.
  if (purge_generation_.size() > kMaxPurgeGenEntries) {
    purge_generation_previous_ = std::move(purge_generation_);
    purge_generation_.clear();
  }
}

int Worker::InvalidateUrl(const std::string& url, const std::string& hostname,
                          const std::string& scheme, std::string_view site,
                          bool clear_heal_state) {
  (void)clear_heal_state;
  if (!cache_) return 0;

  // Normalize for cache key consistency.
  std::string norm_url = NormalizeCacheUrl(url, url_norm_config_);
  std::string norm_host = NormalizeCacheHostname(hostname, url_norm_config_);

  // Bump the purge generation and Remove atomically so in-flight workers
  // can neither re-add stale dedup entries nor re-insert purged content
  // after this purge completes (issue #652).
  int deleted = PurgeUrlAndBumpGeneration(norm_url, norm_host, scheme, site);

  // Clear dedup entries, retry tracking and cooldowns so the next
  // notification triggers re-processing.
  ClearUrlProcessingState(norm_url, norm_host, scheme, site);

  LogInfo("Purged variants for %s (%s)", norm_url.c_str(),
          (deleted != 0) ? "found" : "not found");

  return deleted;
}

void Worker::ClearUrlProcessingState(const std::string& norm_url,
                                     const std::string& norm_host,
                                     const std::string& scheme,
                                     std::string_view site) {
  // Remove all dedup entries for this URL so the next notification
  // triggers re-processing.  Deliberately does NOT touch the purge
  // generation maps (unlike ClearDedupAndCooldown): callers rely on the
  // just-bumped generation staying in place to fence in-flight writers.
  {
    std::string prefix =
        absl::StrCat(norm_url, "|", norm_host, "|", scheme, "|");
    std::lock_guard<std::mutex> lock(processed_set_mutex_);
    EraseUrlMarksLocked(prefix);
  }

  // Clear incomplete retry tracking for this URL.
  {
    std::string retry_key =
        ComposeInternalKey(norm_url, norm_host, scheme, site);
    std::lock_guard<std::mutex> lock(incomplete_retries_mutex_);
    image_incomplete_retries_.erase(retry_key);
  }

  // Also clear any write-failure cooldown so the purged URL can be
  // reprocessed immediately (not blocked for up to 60 seconds).
  {
    std::string ck_prefix = absl::StrCat(norm_url, "|", norm_host, "|");
    std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
    // Erase cooldowns for all schemes (purge is scheme-aware via cache,
    // but cooldowns keyed by url|hostname|scheme need prefix erasure).
    std::erase_if(text_cooldown_expiry_, [&ck_prefix](const auto& kv) {
      return kv.first.starts_with(ck_prefix);
    });
  }
}

std::string Worker::ResetCache() {
  if (!cache_) return "cache not initialized";

  // Fence in-flight variant writes across the volume reset (issue #652):
  // bump the global purge generation and run ResetVolume under
  // purge_gen_mutex_, mirroring PurgeUrlAndBumpGeneration.  Any
  // notification dispatched before the bump is rejected at the write
  // fence (its write could otherwise commit pre-reset content into the
  // fresh volume); any notification that captures the new generation can
  // only start after ResetVolume returned, so it reads post-reset state.
  // Clearing BOTH per-URL maps under the same lock also prevents the
  // inverse failure: stale high generations surviving in
  // purge_generation_previous_ would make WasPurgedSinceDispatch flag
  // every post-reset notification (which captures the cleared-map
  // baseline 0) as purged, silently dropping legitimate writes until the
  // URL's next purge.
  //
  // Deadlock-safety: the lock order is always purge_gen_mutex_ →
  // Cyclone reset lock (here exclusive; shared in
  // PurgeUrlAndBumpGeneration).  PageSpeedCache takes its reset lock
  // per-call (not for handle lifetimes) and never calls back into
  // Worker, so no thread holds a Cyclone lock while waiting on
  // purge_gen_mutex_.
  {
    // Separate scope: dedup_heal_mutex_ is always taken alone, never
    // nested inside purge_gen_mutex_.
    std::lock_guard<std::mutex> lock(dedup_heal_mutex_);
    dedup_heal_last_.clear();
    origin_recheck_last_.clear();
  }
  std::expected<void, cyclone::CacheError> result;
  std::string reset_detail;
  {
    std::lock_guard<std::mutex> lock(purge_gen_mutex_);
    ++purge_all_generation_;
    purge_generation_.clear();
    purge_generation_previous_.clear();
    origin_refresh_last_purge_.clear();
    result = cache_->ResetVolume(&reset_detail);
  }
  if (!result) {
    // The generation bump above stays in effect; in-flight writes are
    // still (conservatively) fenced.  That is the safe direction.
    if (!reset_detail.empty()) {
      return reset_detail;
    }
    return make_error_code(result.error()).message();
  }

  // Clear dedup sets and all cooldowns — full purge is a clean slate.
  {
    std::lock_guard<std::mutex> lock(processed_set_mutex_);
    processed_current_.clear();
    processed_previous_.clear();
    copy_written_current_.clear();
    copy_written_previous_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(incomplete_retries_mutex_);
    image_incomplete_retries_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
    text_cooldown_expiry_.clear();
  }
  return {};
}

void Worker::ClearDedupAndCooldown(const std::string& url,
                                   const std::string& hostname,
                                   const std::string& scheme,
                                   std::string_view site) {
  std::string norm_url = NormalizeCacheUrl(url, url_norm_config_);
  std::string norm_host = NormalizeCacheHostname(hostname, url_norm_config_);
  {
    std::string prefix =
        absl::StrCat(norm_url, "|", norm_host, "|", scheme, "|");
    std::lock_guard<std::mutex> lock(processed_set_mutex_);
    EraseUrlMarksLocked(prefix);
  }
  {
    std::string gen_key = ComposeInternalKey(norm_url, norm_host, scheme, site);
    std::lock_guard<std::mutex> lock(purge_gen_mutex_);
    purge_generation_.erase(gen_key);
    purge_generation_previous_.erase(gen_key);
  }
  {
    std::string retry_key =
        ComposeInternalKey(norm_url, norm_host, scheme, site);
    std::lock_guard<std::mutex> lock(incomplete_retries_mutex_);
    image_incomplete_retries_.erase(retry_key);
  }
  {
    std::string ck = ComposeInternalKey(norm_url, norm_host, scheme, site);
    std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
    text_cooldown_expiry_.erase(ck);
  }
}

// =================================================================
// Cooldown query methods (thread-safe, for HTTP API)
// =================================================================

std::optional<CooldownInfo> Worker::GetCooldownForUrl(
    const std::string& url, const std::string& hostname,
    std::string_view scheme, std::string_view site) const {
  std::string key = ComposeInternalKey(url, hostname, scheme, site);
  std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
  auto it = text_cooldown_expiry_.find(key);
  if (it == text_cooldown_expiry_.end()) return std::nullopt;
  auto now = std::chrono::steady_clock::now();
  if (now >= it->second.expiry) return std::nullopt;
  int remaining = static_cast<int>(
      std::chrono::duration_cast<std::chrono::seconds>(it->second.expiry - now)
          .count());
  int duration =
      static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                           it->second.expiry - it->second.created_at)
                           .count());
  return CooldownInfo{it->second.reason, remaining, duration};
}

std::vector<CooldownListEntry> Worker::ListActiveCooldowns() const {
  std::vector<CooldownListEntry> result;
  std::lock_guard<std::mutex> lock(write_cooldown_mutex_);
  auto now = std::chrono::steady_clock::now();
  for (const auto& [full_key, entry] : text_cooldown_expiry_) {
    if (now >= entry.expiry) continue;
    // Key format: "url|hostname|scheme", followed by "|s=<site>" for a sited
    // key (see ComposeInternalKey) — drop the site, then split on the last
    // two '|' separators (hostnames and schemes never contain '|').
    std::string_view key = full_key;
    constexpr size_t kSiteSuffix = 3 + kSiteIdChars;  // "|s=" + site
    if (key.size() > kSiteSuffix &&
        key.substr(key.size() - kSiteSuffix, 3) == "|s=") {
      key.remove_suffix(kSiteSuffix);
    }
    auto scheme_sep = key.rfind('|');
    if (scheme_sep == std::string::npos) continue;
    auto host_sep = key.rfind('|', scheme_sep - 1);
    if (host_sep == std::string::npos) continue;
    int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(entry.expiry - now)
            .count());
    int duration =
        static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                             entry.expiry - entry.created_at)
                             .count());
    result.push_back(CooldownListEntry{
        .url = std::string(key.substr(0, host_sep)),
        .hostname =
            std::string(key.substr(host_sep + 1, scheme_sep - host_sep - 1)),
        .scheme = std::string(key.substr(scheme_sep + 1)),
        .reason = entry.reason,
        .remaining_seconds = remaining,
        .duration_seconds = duration,
    });
  }
  return result;
}

// =================================================================
// Hot URL tracking for Speculation Rules
// =================================================================

void Worker::RecordHotUrl(const std::string& url) {
  std::lock_guard<std::mutex> lock(hot_urls_mutex_);
  // If already in the set, move to front (LRU refresh).
  if (hot_set_.count(url)) {
    auto it = std::find(hot_urls_.begin(), hot_urls_.end(), url);
    if (it != hot_urls_.end()) {
      hot_urls_.erase(it);
    }
    hot_urls_.push_front(url);
    return;
  }
  // Evict oldest if at capacity.
  if (hot_urls_.size() >= kMaxHotUrls) {
    hot_set_.erase(hot_urls_.back());
    hot_urls_.pop_back();
  }
  hot_urls_.push_front(url);
  hot_set_.insert(url);
}

std::vector<std::string> Worker::GetHotUrls(std::string_view hostname,
                                            size_t max_count) const {
  std::lock_guard<std::mutex> lock(hot_urls_mutex_);
  std::vector<std::string> result;
  result.reserve(max_count);

  // URL exclusion patterns (M8 security finding).
  static constexpr std::string_view kExcludePatterns[] = {
      "/api/", "/logout", "/cart", "/checkout", "/admin", "/wp-admin",
  };

  for (const auto& url : hot_urls_) {
    if (result.size() >= max_count) break;

    // Only same-hostname URLs: extract hostname from URL.
    // URL format: http(s)://hostname/path
    std::string_view url_view = url;
    size_t scheme_end = url_view.find("://");
    if (scheme_end == std::string_view::npos) continue;
    size_t host_start = scheme_end + 3;
    size_t host_end = url_view.find('/', host_start);
    std::string_view url_host;
    if (host_end != std::string_view::npos) {
      url_host = url_view.substr(host_start, host_end - host_start);
    } else {
      url_host = url_view.substr(host_start);
    }

    if (url_host != hostname) continue;

    // Exclude sensitive patterns.
    bool excluded = false;
    for (const auto& pattern : kExcludePatterns) {
      if (url.find(pattern) != std::string::npos) {
        excluded = true;
        break;
      }
    }
    if (excluded) continue;

    result.push_back(url);
  }
  return result;
}

// =================================================================
// Web Bot Auth key-directory refresh
// =================================================================

namespace {

// Context for one background key-directory refresh cycle.  Carries a COPY of
// the warmer state so the work-pool thread never touches loop-thread members;
// the done-callback swaps the refreshed copy back on the loop thread.
struct WebBotAuthRefreshWork {
  Worker* worker;
  WebBotAuthWarmerConfig config;
  WebBotAuthWarmerState state;
  int keys_written = 0;
  WebBotAuthRefreshFetchStats fetch_stats;
};

}  // namespace

// static
void Worker::OnWebBotAuthRefresh(uv_timer_t* timer) {
  auto* worker = static_cast<Worker*>(timer->data);

  // Guard against overlapping refresh cycles.
  if (worker->webbotauth_refresh_in_progress_) return;

  auto cfg = worker->live_config_.load();
  if (!cfg->web_bot_auth || cfg->web_bot_auth_key_directories.empty()) return;

  worker->webbotauth_refresh_in_progress_ = true;

  auto* work = new WebBotAuthRefreshWork{};
  work->worker = worker;
  work->config.directory_urls = cfg->web_bot_auth_key_directories;
  work->config.keys_file_path = WebBotAuthKeysFilePath(cfg->cache_path);
  work->state = worker->webbotauth_state_;

  auto* req = new uv_work_t;
  req->data = work;
  worker->webbotauth_req_ = req;
  uv_queue_work(worker->loop_.get(), req, DoWebBotAuthRefresh,
                OnWebBotAuthRefreshDone);
}

// static — runs on thread pool
void Worker::DoWebBotAuthRefresh(uv_work_t* req) {
  auto* work = static_cast<WebBotAuthRefreshWork*>(req->data);
  int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  JwksFetchFn fetch = MakeWebBotAuthJwksFetcher(work->config);
  work->keys_written = RefreshWebBotAuthKeys(work->config, fetch, now_s,
                                             &work->state, &work->fetch_stats);
}

// static — runs on event loop
void Worker::OnWebBotAuthRefreshDone(uv_work_t* req, int status) {
  auto* work = static_cast<WebBotAuthRefreshWork*>(req->data);
  Worker* worker = work->worker;

  // Lifecycle discipline: this callback always runs (the destructor's drain
  // loop keeps the loop alive), so clearing the in-flight pointer here
  // guarantees req+work are freed exactly once.
  worker->webbotauth_req_ = nullptr;

  if (status != 0) {
    // Cancelled at shutdown — free only, no other worker state.
    delete work;
    delete req;
    return;
  }

  worker->webbotauth_refresh_in_progress_ = false;
  worker->webbotauth_state_ = std::move(work->state);

  if (worker->handler_ != nullptr) {
    worker->handler_->Info(
        "Web Bot Auth key refresh: %d key(s) warmed from %zu director%s",
        work->keys_written, work->config.directory_urls.size(),
        work->config.directory_urls.size() == 1 ? "y" : "ies");
  }

  // Total-failure backoff (#876): when every directory fetch failed (and at
  // least one was attempted — distinct from a reachable directory serving an
  // empty key set), retry on the short schedule instead of waiting out the
  // full refresh interval.  uv_timer_start on the active timer restarts it:
  // the retry fires after the backoff delay and the normal cadence resumes
  // from there.  Any cycle that reaches a directory resets the schedule.
  const bool total_failure =
      work->fetch_stats.attempted > 0 && work->fetch_stats.fetched_ok == 0;
  if (!total_failure) {
    worker->webbotauth_backoff_stage_ = 0;
  } else if (!worker->shutting_down_ && worker->webbotauth_refresh_timer_ &&
             !uv_is_closing(reinterpret_cast<uv_handle_t*>(
                 worker->webbotauth_refresh_timer_.get())) &&
             worker->webbotauth_backoff_stage_ <
                 std::size(kWebBotAuthTotalFailureBackoffMs)) {
    const uint64_t delay_ms =
        kWebBotAuthTotalFailureBackoffMs[worker->webbotauth_backoff_stage_++];
    uv_timer_start(worker->webbotauth_refresh_timer_.get(), OnWebBotAuthRefresh,
                   delay_ms, kWebBotAuthRefreshIntervalMs);
    worker->LogWarning(
        "Web Bot Auth key refresh: all %d director%s unreachable — "
        "retrying in %llus (signed requests classify as unknown until keys "
        "warm)",
        work->fetch_stats.attempted,
        work->fetch_stats.attempted == 1 ? "y" : "ies",
        static_cast<unsigned long long>(delay_ms / 1000));
  }

  delete work;
  delete req;
}

// =================================================================
// RSL-CAP key-directory refresh (experimental)
// =================================================================
//
// A second, independent instance of the observe-only warmer above, warming the
// RSL-CAP issuer directories into realm "rsl" and publishing them to a separate
// keys file.  Kept parallel (not merged) so the enforcement trust domain is
// isolated end to end and the A1 path is untouched.

namespace {

struct RslCapRefreshWork {
  Worker* worker = nullptr;
  WebBotAuthWarmerConfig config;
  WebBotAuthWarmerState state;
  int keys_written = 0;
  WebBotAuthRefreshFetchStats fetch_stats;
};

}  // namespace

// static
void Worker::OnRslCapRefresh(uv_timer_t* timer) {
  auto* worker = static_cast<Worker*>(timer->data);

  if (worker->rslcap_refresh_in_progress_) return;

  auto cfg = worker->live_config_.load();
  if (!cfg->rsl_cap_enforcement || cfg->rsl_cap_key_directories.empty()) return;

  worker->rslcap_refresh_in_progress_ = true;

  auto* work = new RslCapRefreshWork{};
  work->worker = worker;
  work->config.realm = "rsl";
  work->config.directory_urls = cfg->rsl_cap_key_directories;
  work->config.keys_file_path = RslCapKeysFilePath(cfg->cache_path);
  work->state = worker->rslcap_state_;

  auto* req = new uv_work_t;
  req->data = work;
  worker->rslcap_req_ = req;
  uv_queue_work(worker->loop_.get(), req, DoRslCapRefresh, OnRslCapRefreshDone);
}

// static — runs on thread pool
void Worker::DoRslCapRefresh(uv_work_t* req) {
  auto* work = static_cast<RslCapRefreshWork*>(req->data);
  int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  JwksFetchFn fetch = MakeWebBotAuthJwksFetcher(work->config);
  work->keys_written = RefreshWebBotAuthKeys(work->config, fetch, now_s,
                                             &work->state, &work->fetch_stats);
}

// static — runs on event loop
void Worker::OnRslCapRefreshDone(uv_work_t* req, int status) {
  auto* work = static_cast<RslCapRefreshWork*>(req->data);
  Worker* worker = work->worker;

  worker->rslcap_req_ = nullptr;

  if (status != 0) {
    delete work;
    delete req;
    return;
  }

  worker->rslcap_refresh_in_progress_ = false;
  worker->rslcap_state_ = std::move(work->state);

  if (worker->handler_ != nullptr) {
    worker->handler_->Info(
        "RSL-CAP key refresh: %d key(s) warmed from %zu director%s",
        work->keys_written, work->config.directory_urls.size(),
        work->config.directory_urls.size() == 1 ? "y" : "ies");
  }

  const bool total_failure =
      work->fetch_stats.attempted > 0 && work->fetch_stats.fetched_ok == 0;
  if (!total_failure) {
    worker->rslcap_backoff_stage_ = 0;
  } else if (!worker->shutting_down_ && worker->rslcap_refresh_timer_ &&
             !uv_is_closing(reinterpret_cast<uv_handle_t*>(
                 worker->rslcap_refresh_timer_.get())) &&
             worker->rslcap_backoff_stage_ <
                 std::size(kWebBotAuthTotalFailureBackoffMs)) {
    const uint64_t delay_ms =
        kWebBotAuthTotalFailureBackoffMs[worker->rslcap_backoff_stage_++];
    uv_timer_start(worker->rslcap_refresh_timer_.get(), OnRslCapRefresh,
                   delay_ms, kWebBotAuthRefreshIntervalMs);
    worker->LogWarning(
        "RSL-CAP key refresh: all %d director%s unreachable — retrying in "
        "%llus (tokens rejected as unknown issuer until keys warm)",
        work->fetch_stats.attempted,
        work->fetch_stats.attempted == 1 ? "y" : "ies",
        static_cast<unsigned long long>(delay_ms / 1000));
  }

  delete work;
  delete req;
}

void Worker::BuildPrometheusMetrics(std::string& response) const {
  // Delegates to the shared builder in api_handlers.cc (single source of
  // truth with GET /v1/metrics — see BuildPrometheusMetricsText, #877).
  // Uses cached cache-size values to avoid contending with Cyclone locks on
  // the event loop.
  PrometheusMetricsInputs in{
      .stats = stats_,
      .cache_entries = cached_cache_entries_.load(std::memory_order_relaxed),
      .cache_bytes = cached_cache_bytes_.load(std::memory_order_relaxed),
      .active_connections = active_connections_.load(),
      .max_connections = config_.max_connections,
      .in_flight_work = in_flight_work_.load(std::memory_order_relaxed),
      .browser_manager = browser_manager_.get(),
      .serve_stats = serve_stats_,
      .web_bot_auth_verified_bots = config_.web_bot_auth_verified_bots,
  };
  response = BuildPrometheusMetricsText(in);
}
}  // namespace pagespeed
