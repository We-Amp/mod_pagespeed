// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - PSOL Factory Worker
//
// Lightweight C++ daemon that handles HTML parsing and image optimization.
// Receives fire-and-forget notifications from the nginx proxy when new
// content is recorded in the cache.  Reads content from cache, processes
// it, and writes optimized variants back.

#ifndef PAGESPEED_SRC_WORKER_WORKER_H_
#define PAGESPEED_SRC_WORKER_WORKER_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/url_normalizer.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/config_persister.h"
#include "src/worker/html_mode.h"
#include "src/worker/image_transcoder.h"
#include "src/worker/serve_stats.h"
#include "src/worker/subresource_gate.h"
#include "src/worker/webbotauth_warmer.h"
#include "uv.h"

namespace pagespeed {

// One declined variant slot in the per-URL decline tombstone (#1382):
// the SSIMULACRA2 verify refused this (format x viewport x density x
// save-data) slot for the current source, so a later notification must not
// re-pay the full attempt ladder for it.
struct DeclineTombstoneRecord {
  // Low mask byte of the slot's sibling mask (MaskToAlternateId of the
  // encoded low byte) -- the same id FindMissingFormats computes.
  uint8_t slot = 0;
  // Final encoder quality of the declined arm (diagnostic evidence for
  // whoever inspects the blob; 0xFF = not recorded).
  uint8_t quality = 0xFF;
};

// Parse a decline-tombstone payload (kDeclineTombstone sentinel, format
// version kDeclineTombstoneFormatVersion) and return its records -- but
// ONLY when the blob is well-formed, was written by the current quality
// search (kVerifyQualitySearchVersion) and was written for exactly
// |source_hash|. Anything else reads as no tombstone: fail-open is the
// correct direction here, since a lost or unread tombstone only costs a
// re-run of the attempt ladder, never a wrong variant. Recording the
// search generation is what keeps a refusal from outliving the search that
// made it: a variant an older search could not reach is retried rather
// than suppressed forever.
std::vector<DeclineTombstoneRecord> ParseDeclineTombstone(
    std::string_view blob, const std::array<std::byte, 32>& source_hash);

// Serialize a tombstone payload for |source_hash|. Records are stored in
// the order given; the payload is [1B version][1B search generation][32B
// hash][1B count][2B per record: slot, quality].
std::string EncodeDeclineTombstone(
    const std::array<std::byte, 32>& source_hash,
    const std::vector<DeclineTombstoneRecord>& records);

// True when an original-format slot that produced nothing is a plain
// no-savings skip (the caller counts it under image_no_savings_skipped).
// A DECLINED original must not count as one: the verify decline already
// counted under ssimulacra2_declines, and one refusal wearing two labels
// makes both counters lie (#1382). Directly tested in worker_test: the
// declined-original state is not constructible end-to-end through the
// encoders (it needs a failed decode-back of our own JPEG), so the
// classification lives in a named function.
bool OriginalSlotCountsAsNoSavingsSkip(const MultiTranscodeResult& multi);

// SVG auto-vectorization mode:
//   kDetect  — evaluate candidacy and log, but don't vectorize or serve
//   kPreview — vectorize and store, but don't serve to clients
//   kAuto    — full pipeline: evaluate, vectorize, store, and serve
enum class SvgMode : uint8_t { kDetect, kPreview, kAuto };

class MessageHandler;
class HttpServer;
class WsManager;
class StaticFileCache;
class UrlRegistry;
class KnownSitesWriter;
struct ApiContext;
struct CacheApiContext;
struct CaptureContext;

struct HttpServerConfig;
struct WsConfig;

// Worker-level processing statistics (atomic for thread safety).
struct WorkerStats {
  std::atomic<uint64_t> notifications_received{0};
  std::atomic<uint64_t> notifications_skipped_dedup{0};
  // Dedup-set hits whose entry was ERASED because the variant family it
  // stood for no longer holds a single servable (non-sentinel) alternate in
  // the cache — an orphaned entry left behind by capacity eviction, which
  // has no callback into the worker.  The notification falls through to
  // normal processing instead of being skipped.  Not a skip and not an
  // error: it is the dedup set converging back to cache reality.
  std::atomic<uint64_t> notifications_dedup_healed{0};
  std::atomic<uint64_t> notifications_dedup_heal_rate_limited{0};
  // Processed-set hits whose entry was erased because the optimized copy
  // this process wrote for the key is no longer in the cache while other
  // copies of the URL are: the notification is processed again, which
  // normally writes the copy back.  Counted when that decision is taken,
  // not when the copy is written: a re-processing that ends without a
  // write (a write-failure cooldown, an unreadable source) is counted too.
  // Shares the window of the heal above, which is kept per URL and client
  // class (the key of the processed set).
  std::atomic<uint64_t> notifications_missing_copy_healed{0};
  std::atomic<uint64_t> notifications_skipped_inflight{0};
  // Notifications refused at the socket, split by WHY, because the three
  // causes have three different fixes and are indistinguishable from the
  // outside (in every case nothing gets optimized):
  //   _version   a peer speaking a different wire version — a deployment
  //              problem, and the counter that matters once notify peers ship
  //              on their own cadence.  Never counted as a parse error,
  //              because the frame was never parsed.
  //   _malformed a peer that agrees on the version but sent a truncated or
  //              corrupt frame — a transport or sender problem.
  //   _sentinel  a well-formed notification naming a reserved entry class
  //              this build does not accept as a notification trigger.
  //   _option_context
  //              a notification whose per-request option context FAILED
  //              VALIDATION at dispatch.  Three causes reach here, and they
  //              are the whole list: an unknown payload format, a malformed
  //              signature, and a signature that does not match its payload.
  //              An OVERSIZED payload does NOT land here — the frame reader
  //              bounds it with the same constant the validator uses, so it
  //              is refused a layer earlier and counted as _malformed (and
  //              the sending side refuses to emit one at all).  Nothing else
  //              lands here either: a context that validates is accepted
  //              whatever it names (see Worker::AcceptOptionContext), so this
  //              counter moving means a peer that genuinely needs attention,
  //              and the drift case (mismatch) needs it most.
  //
  //              NOTE the asymmetry with the two counters above, because it is
  //              visible on the stats surface and would otherwise read as an
  //              inconsistency: a version-skewed or malformed frame is refused
  //              BEFORE dispatch and never increments notifications_received,
  //              whereas an option-context refusal happens AT dispatch, so it
  //              does.  So received == refused + processed here, and received
  //              stays 0 for the other two.  That is deliberate -- "received"
  //              counts frames this build could parse -- but it means the
  //              three refusal counters are not interchangeable when reasoning
  //              about throughput.
  std::atomic<uint64_t> notifications_rejected_version{0};
  std::atomic<uint64_t> notifications_rejected_malformed{0};
  std::atomic<uint64_t> notifications_rejected_sentinel{0};
  std::atomic<uint64_t> notifications_rejected_option_context{0};
  // Site and sender (wire v6), checked at dispatch right after the option
  // context.  A notification from the module must name a site
  // (rejected_empty_site) and must not ask for page work: HTML, the llms.txt
  // build, an agent request, or a vouched host (rejected_module_render).
  std::atomic<uint64_t> notifications_rejected_empty_site{0};
  std::atomic<uint64_t> notifications_rejected_module_render{0};
  // Not a refusal: frames libpagespeed built for a caller whose parameters
  // predate the site fields (an old module).  Processed unsited.
  std::atomic<uint64_t> notifications_legacy_ex_caller{0};
  // Not a refusal: HTML or llms.txt notifications whose sender did not vouch
  // for the host.  The page is optimized; no agent render or llms.txt build
  // runs for it.
  std::atomic<uint64_t> notifications_unvouched_render_skipped{0};
  // Mirrors of the URL registry's known-sites state, refreshed with the cache
  // stats: sites seen past the cap since start, and whether a purge without
  // a site may miss a site (1) or not (0).
  std::atomic<uint64_t> known_sites_overflow{0};
  std::atomic<uint64_t> known_sites_incomplete{0};
  // NOTIFICATIONS carrying a valid context that is not the default one, and
  // admitted by the option-context gate.  Not a refusal and not an error: the
  // context costs the notification nothing, because optimized output on this
  // build does not depend on the sender's configuration, so whatever work the
  // notification goes on to do is done under the DEFAULT context.
  //
  // It counts NOTIFICATIONS ADMITTED, not optimizations performed.  The gate
  // runs before the shutdown, URL-length, in-flight and already-processed
  // checks, so two notifications for one URL under two different contexts
  // increment this twice and optimize once — which is the intended reading,
  // because what an operator cannot otherwise see is that peers are resolving
  // and sending contexts at all, and that their work lands on the default key
  // rather than on one of their own.  For work actually done, read the
  // per-content-type counters.
  std::atomic<uint64_t> notifications_accepted_non_default_option_context{0};
  std::atomic<uint64_t> variants_written{0};
  std::atomic<uint64_t> proactive_variants_written{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> errors_origin_misconfiguration{0};

  // CSS/JS minification parse failures: variant skipped, original served.
  std::atomic<uint64_t> text_minify_parse_failures{0};

  // Per content type
  std::atomic<uint64_t> html_processed{0};
  std::atomic<uint64_t> css_processed{0};
  std::atomic<uint64_t> js_processed{0};
  std::atomic<uint64_t> images_processed{0};

  // Entries whose optimization produced no smaller identity variant
  // ("already minimal" for CSS/JS, the no-savings skip for images), per
  // type: how many entries were judged, and their original byte sizes.
  // Counted ONCE PER PROCESSED ENTRY at the verdict, never per serve; the
  // window is this worker process's lifetime (they restart with it).  The
  // serve-side picture is GET /v1/stats serve_savings.*.by_encoding.
  std::atomic<uint64_t> css_already_optimal_count{0};
  std::atomic<uint64_t> css_already_optimal_bytes{0};
  std::atomic<uint64_t> js_already_optimal_count{0};
  std::atomic<uint64_t> js_already_optimal_bytes{0};
  std::atomic<uint64_t> image_already_optimal_count{0};
  std::atomic<uint64_t> image_already_optimal_bytes{0};

  // Per image output format
  std::atomic<uint64_t> webp_generated{0};
  std::atomic<uint64_t> avif_generated{0};
  std::atomic<uint64_t> jpeg_optimized{0};
  std::atomic<uint64_t> png_optimized{0};

  // Processing time in microseconds (cumulative)
  std::atomic<uint64_t> total_processing_time_us{0};
  std::atomic<uint64_t> image_processing_time_us{0};
  std::atomic<uint64_t> html_processing_time_us{0};
  std::atomic<uint64_t> css_processing_time_us{0};
  std::atomic<uint64_t> js_processing_time_us{0};

  // HTML assembly counters
  std::atomic<uint64_t> html_assembly_complete{0};
  std::atomic<uint64_t> html_assembly_skipped{0};
  std::atomic<uint64_t> critical_css_aborted{0};
  // Issue A: browser-profile critical CSS suppressed because the
  // coverage/byte budget judged it too close to the whole sheet to inline
  // (would bloat the body + double-ship the CSS via async re-download).
  std::atomic<uint64_t> critical_css_skipped_high_coverage{0};
  // Critical blocks dropped by the inline byte cap
  // (kInlineCriticalCssMaxBytes) with no validated deferral to justify them.
  std::atomic<uint64_t> critical_css_skipped_byte_cap{0};

  // Alternate write tracking
  std::atomic<uint64_t> alternate_writes{0};
  // Hard write failures only: genuine mmap/cache errors (NOT benign
  // purge-fences).  Surfaced as alternates.write_failures on the dashboard.
  std::atomic<uint64_t> alternate_write_failures{0};
  // Benign purge-fenced write skips (Issue E): a purge advanced the URL's
  // generation after this notification was dispatched (e.g. the content-hash
  // invalidation purge in the same notification), so the in-flight write is
  // correctly dropped.  Split out from alternate_write_failures + errors so the
  // dashboard no longer false-alarms (the live write_failures==content_hash_stale
  // ==errors lockstep) and real write failures stay visible.
  std::atomic<uint64_t> alternate_writes_fenced{0};

  // Compressed variant counters
  std::atomic<uint64_t> gzip_variants_written{0};
  std::atomic<uint64_t> brotli_variants_written{0};

  // Selector invocations (ReadBestAlternate calls)
  std::atomic<uint64_t> selector_invocations{0};

  // Optimization-source reads that the selector could not answer and the
  // durable-original class (0x0C, read by exact id) did.  Zero on a
  // deployment whose front end stores selectable alternates; equal to the
  // notification rate on one whose front end records durable originals
  // instead.  Its whole job is to make those two cases distinguishable: a
  // source-read miss on a URL that HAS a durable original used to look
  // exactly like a URL with no entry at all.
  std::atomic<uint64_t> source_reads_from_durable_original{0};

  // Cross-process cache read retry counters (cold-start mmap propagation)
  std::atomic<uint64_t> cache_read_retries{0};   // Succeeded after retry
  std::atomic<uint64_t> cache_read_failures{0};  // All retries exhausted
  std::atomic<uint64_t> cache_read_deferred_retries{
      0};  // Deferred re-notifications scheduled
  std::atomic<uint64_t> cache_read_deferred_successes{
      0};  // Deferred retries that succeeded
  // De-alias copy-verify (issue #934): a live-lease disk borrow
  // wrapped while its bytes were copied out, so the copy was discarded and
  // the read treated as a miss — the worker never persists torn output.
  std::atomic<uint64_t> read_borrow_wrap_discards{0};

  // Cache auto-heal counters
  std::atomic<uint64_t> cache_auto_heals{0};
  std::atomic<uint64_t> cache_auto_heal_exhausted{0};

  // Content analysis classification counters
  std::atomic<uint64_t> content_photo{0};
  std::atomic<uint64_t> content_screenshot{0};
  std::atomic<uint64_t> content_illustration{0};
  std::atomic<uint64_t> content_noisy{0};

  // SSIMULACRA2 quality verification counters
  std::atomic<uint64_t> ssimulacra2_checks{0};
  std::atomic<uint64_t> ssimulacra2_reencodes{0};
  // Variants refused by the verifier (below floor / no verdict, mpp #790).
  // A decline is a decision, not a failure: counted here like the size-gate
  // rejections (image_no_savings_skipped), never in errors, and its score
  // does not feed the average.
  std::atomic<uint64_t> ssimulacra2_declines{0};
  // Decline tombstones that stopped a variant recompute (#1382): a
  // notification asked for a slot already refused for this source and the
  // attempt ladder was skipped. Mirrors the declines counter's contract --
  // a decision, never an error.
  std::atomic<uint64_t> ssimulacra2_decline_tombstone_hits{0};
  // Accumulated score * 100 for average computation
  std::atomic<uint64_t> ssimulacra2_total_score_x100{0};

  // Learned quality prediction counters
  std::atomic<uint64_t> learned_quality_predictions{0};
  std::atomic<uint64_t> learned_quality_fallbacks{0};

  // SVG auto-vectorization counters
  std::atomic<uint64_t> svg_candidates_evaluated{0};
  std::atomic<uint64_t> svg_candidates_rejected{0};
  std::atomic<uint64_t> svg_vectorized{0};
  std::atomic<uint64_t> svg_fidelity_rejected{0};
  std::atomic<uint64_t> svg_timeout_exceeded{0};
  std::atomic<uint64_t> svg_size_rejected{0};
  std::atomic<uint64_t> svg_path_count_rejected{0};
  std::atomic<uint64_t> svg_written{0};
  std::atomic<uint64_t> svg_bytes_saved{0};
  std::atomic<uint64_t> svg_vectorize_time_us{0};  // Cumulative
  // svg.served lives in the cross-process ServeStats mmap
  // (svg_optimized_hits), written at serve time by both front-ends — see
  // serve_stats.h and the follow-up in #455. It is intentionally NOT a
  // WorkerStats field: no front-end can write a worker in-process counter.

  // Policy engine counters
  std::atomic<uint64_t> policy_computed{0};
  std::atomic<uint64_t> policy_async_css_enabled{0};
  std::atomic<uint64_t> policy_script_deferral_enabled{0};
  // FOUC sufficiency gate: incremented when async-CSS deferral is suppressed
  // because the critical CSS is too thin to bridge first paint (the sheet is
  // kept render-blocking instead).
  std::atomic<uint64_t> async_css_suppressed_low_coverage{0};
  // Empirical gate: incremented when the byte floor cleared but the page has
  // no validation record bound to the stylesheet being served — the page has
  // not been validated yet, or its stylesheet changed since it was.
  std::atomic<uint64_t> async_css_suppressed_unvalidated{0};
  // Empirical gate, upstream of the two above: incremented when a browser
  // profile WAS selected for the page but its DOM-matched derivation produced
  // no critical block, so its validation record was dropped rather than let
  // stand over whatever gets inlined instead (issue #1216).
  std::atomic<uint64_t> async_css_record_dropped_empty_derivation{0};

  // Quality baselining counters
  std::atomic<uint64_t> quality_capped_jpeg{0};    // JPEG quality capped
  std::atomic<uint64_t> quality_skip_reencode{0};  // JPEG re-encode skipped

  // Incomplete variant matrix counter (gap-fill mechanism)
  std::atomic<uint64_t> image_incomplete_matrices{0};

  // Image variants skipped because optimization produced no size savings
  std::atomic<uint64_t> image_no_savings_skipped{0};

  // Converted-format variants refused because the per-format fall-through
  // produced no conversion, i.e. the optimizer handed back the origin's own
  // bytes in the origin's own format (#1374).  Distinct from the counter
  // above: that one is "the optimizer ran and could not win", this one is
  // "there was nothing to convert to begin with".  Without it the two are
  // indistinguishable from outside, which is what let #1374 sit unnoticed.
  std::atomic<uint64_t> image_unconverted_fallthrough{0};

  // Cross-format size gate (#406): a WebP or AVIF that was not smaller than
  // the original-format image of its slot (refused), and those a smaller
  // encode at the same quality floor recovered (rescued). refused minus
  // rescued is what was dropped.
  std::atomic<uint64_t> image_size_refused_webp{0};
  std::atomic<uint64_t> image_size_refused_avif{0};
  std::atomic<uint64_t> image_budget_rescued_webp{0};
  std::atomic<uint64_t> image_budget_rescued_avif{0};

  // Save-Data variants refused because they were not smaller than the same
  // variant without Save-Data. Tells a deliberately identical pair (the
  // normal variant is served to both) apart from a missing variant.
  std::atomic<uint64_t> image_savedata_refused{0};

  // Intra-notification content dedup: writes skipped because identical
  // bytes were already written under a different AlternateId.
  std::atomic<uint64_t> dedup_writes_skipped{0};

  // Cross-notification content hash: origin unchanged, processing skipped.
  std::atomic<uint64_t> content_hash_hits{0};
  // Cross-notification content hash: origin changed, forced re-process.
  std::atomic<uint64_t> content_hash_stale{0};

  // Origin-refreshed sentinel (issue #652): nginx re-fetched expired origin
  // content; the worker purged the URL's stale variant set in response.
  std::atomic<uint64_t> origin_refresh_purges{0};
  // Origin-refreshed sentinel arrivals rate-limited away (per-URL interval).
  std::atomic<uint64_t> origin_refresh_rate_limited{0};
  // Origin-refreshed purges whose INLINE REBUILD was refused because the
  // only candidate source at the identity id was worker-processed (a stale
  // variant, not a genuine original — the dedup-heal work).
  // Rebuilding from it would re-publish superseded bytes with fresh stamps,
  // so the rebuild is left to the next record+notify instead (dedup was
  // cleared by the purge).  Not an error: it is the refresh convergence
  // taking the safe leg.
  std::atomic<uint64_t> origin_refresh_rebuild_refused{0};
  // A dedup-hit notification whose recorded original's origin state differs
  // from the variant set's (a front end re-recorded it): how many were
  // looked at, how many led to a targeted purge + rebuild, and how many
  // turned out to be the same bytes and were only restamped.
  std::atomic<uint64_t> notifications_origin_rechecked{0};
  std::atomic<uint64_t> origin_change_rebuilt{0};
  std::atomic<uint64_t> origin_unchanged_restamped{0};

  // Origin-refreshed sentinels where the pristine origin reference hashed
  // equal to the content-hash oracle: the origin did NOT change, so the
  // variant set was restamped in place from the fresh origin metadata —
  // no purge, no rebuild (issue #1503).
  std::atomic<uint64_t> origin_refresh_unchanged{0};
  // Origin-refreshed sentinels that arrived BEFORE the re-recorded origin
  // landed (the sender notifies ahead of the re-record) with the oracle
  // showing no change: no purge — change detection and convergence are
  // delegated to the record+notify that follows (issue #1503, images only).
  std::atomic<uint64_t> origin_refresh_deferred{0};

  // Durability (#19) — agent_markdown coverage.  The origin-refresh
  // sentinel fires on every TTL expiry / 200 revalidation, even when the
  // re-fetched origin body is byte-identical; D2 preserves the rendered
  // markdown variant in that case instead of needlessly purging + re-rendering.
  // preserved: a no-content-change refresh kept the variant alive (the win).
  std::atomic<uint64_t> agent_markdown_preserved{0};
  // purged_on_change: the origin content actually changed, so the rendered
  // markdown was superseded and dropped (correct purge — the variant rebuilds).
  std::atomic<uint64_t> agent_markdown_purged_on_change{0};
  // rebuild_forced: a rebuild was forced by the sticky per-URL intent flag (D3)
  // because the variant had already been lost in a prior churn — the
  // multi-churn demand-gating race that would otherwise leave it absent.
  std::atomic<uint64_t> agent_markdown_rebuild_forced{0};
};

// Purge fence baseline captured when a notification is dispatched
// (issue #652).  url_gen is the per-URL purge generation; all_gen is the
// worker-global generation bumped by full cache purges (ResetCache).  A
// variant write is dropped when either counter has advanced past the
// dispatch-time baseline — the write's source content was read before a
// purge and must not resurrect purged state.  Deliberately dispatch-order
// based (no wall-clock comparison): cache_inserted_at is Age-adjusted at
// insert time (RFC 9111 §4.2.3), so timestamp provenance comparisons would
// falsely reject legitimate post-purge stores that arrived through an
// Age-emitting upstream layer (CDN/Varnish).
struct PurgeDispatchGen {
  uint64_t url_gen = 0;
  uint64_t all_gen = 0;
};

// Configuration for the factory worker
struct WorkerConfig {
  // Compiled default matches the packaged layout (systemd RuntimeDirectory,
  // /run/pagespeed-optimizer, mode 0750 pagespeed:pagespeed).  Bare runs
  // without a RuntimeDirectory fail the bind loudly — pass --socket for
  // ad-hoc/dev use.
  std::string socket_path = "/run/pagespeed-optimizer/notify.sock";
  int num_threads = 0;  // 0 = auto (CPU budget clamped to [2, 128])
  size_t max_request_size = 10ULL * 1024 * 1024;  // 10MB

  // Cache configuration
  std::string cache_path;
  uint64_t cache_size_bytes = 1ULL * 1024 * 1024 * 1024;  // 1GB
  size_t ram_cache_size = 64ULL * 1024 * 1024;            // 64 MB
  // Cyclone read-lease wrap gating (issue #934).  Every disk-hit
  // read stamps a per-stripe lease of read_lease_duration_ms; writers
  // defer circular write-buffer wraps while a lease is live, bounded by
  // lease_wrap_ceiling_ms (past it the wrap is forced).  0 disables
  // leases (the pre-lease behavior) — the operator/CI escape hatch for
  // read-heavy workloads at cache-full.  Defaults mirror Cyclone's (via
  // PageSpeedCacheConfig).  Restart to change (the cache opens once, at
  // Initialize).
  int read_lease_duration_ms = 5000;
  int lease_wrap_ceiling_ms = 60000;
  // Resource limits
  int max_connections = 128;                // Max simultaneous connections
  size_t max_buffer_size = 1024ULL * 1024;  // 1MB max per-client buffer
  int connection_timeout_ms = 30000;        // 30s idle connection timeout
  int shutdown_timeout_ms = 5000;           // 5s graceful shutdown timeout

  // Cache mode: "safe" (short TTLs, must-revalidate) or
  // "aggressive" (long TTLs, public).
  std::string cache_mode = "safe";

  // HTML mode (src/worker/html_mode.h): off, markdown-static,
  // markdown-rendered or full.  Replaces the old disable_html boolean, which
  // every input surface still accepts (true = off, false = full) and every
  // writer still emits, derived, for readers that predate html_mode.
  HtmlMode html_mode = kDefaultHtmlMode;

  // Content-type toggles
  bool disable_css = false;
  bool disable_js = false;
  bool disable_image = false;

  // Proactive optimization: generate sibling format variants on
  // image notification (e.g., WebP+AVIF+optimized-original from
  // a single decode pass).  Off by default: each unique client
  // profile triggers its own single-variant optimization on demand.
  bool proactive_image_variants = false;

  // Proactive viewport siblings: when generating proactive format
  // variants, also generate them for other viewport classes
  // (Mobile/Tablet/Desktop).  Requires proactive_image_variants.
  bool proactive_viewport_variants = true;

  // Warmup: when nginx sends a warmup sentinel (0xFFFFFFFE), generate
  // all format variants for images and ensure minified versions exist
  // for CSS/JS.  Off by default.
  bool enable_warmup = false;

  // Proactive Save-Data siblings: generate lower-quality variants
  // for Save-Data=on in addition to normal quality.
  bool proactive_savedata_variants = true;

  // Proactive pixel density siblings: generate variants for both
  // 1x and 2x density from a single notification.
  bool proactive_density_variants = true;

  // Viewport-based image resizing target widths.
  // 0 = no resize for that viewport class.
  uint32_t mobile_width = 480;
  uint32_t tablet_width = 768;
  uint32_t desktop_width = 0;

  // Image quality settings (override ImageTranscoderConfig defaults)
  int jpeg_quality = 85;
  int webp_quality = 75;
  int avif_quality = 60;  // 0-100, higher = better
  int avif_speed = 6;     // 0 (slowest/best) - 10 (fastest)
  int savedata_jpeg_quality = 60;
  int savedata_webp_quality = 50;
  int savedata_avif_quality = 45;

  // Content-aware quality presets
  bool content_analysis = true;  // --no-content-analysis

  // SSIMULACRA2 perceptual quality verification (Phase 2b)
  bool quality_verify = true;          // --no-quality-verify
  float target_ssimulacra2 = 70.0f;    // --target-ssimulacra2
  float ssimulacra2_tolerance = 5.0f;  // --ssimulacra2-tolerance

  // Learned quality prediction
  bool learned_quality = true;             // --no-learned-quality
  bool learned_quality_jpeg = true;        // --no-learned-quality-jpeg
  bool learned_quality_webp = true;        // --no-learned-quality-webp
  bool learned_quality_avif = true;        // --no-learned-quality-avif
  float savedata_score_reduction = 15.0f;  // --savedata-score-reduction

  // Quality baselining.  A same-format JPEG re-encode is capped at the
  // detected source quality; --quality-cap-margin is still accepted but no
  // longer raises that cap (#1284).
  int quality_cap_margin = 10;  // --quality-cap-margin (deprecated)
  bool no_quality_cap = false;  // --no-quality-cap

  // C2PA / Content Credentials provenance preservation.
  // On by default; preserves APP11/JUMBF manifests through JPEG->JPEG
  // optimization. Opt out with --no-preserve-c2pa.
  bool preserve_c2pa = true;  // --no-preserve-c2pa

  // C2PA Level A PNG carry-through, OFF by default. When on, a
  // manifest-bearing PNG kept in PNG form is recompressed AND its caBX/iTXt chunks
  // are re-spliced (optimized + provenance), instead of the default skip-not-strip
  // (served unoptimized). PNG-only; requires preserve_c2pa. Demand-gated. Opt in
  // with --c2pa-carry.
  bool c2pa_carry = false;  // --c2pa-carry

  // SVG auto-vectorization settings
  SvgMode svg_mode = SvgMode::kDetect;  // --svg-mode
  int svg_candidacy_threshold = 50;     // --svg-candidacy-threshold
  int svg_max_pixels = 65536;           // --svg-max-pixels (256*256)
  int svg_preset = 1;              // --svg-preset (0=bw, 1=poster, 2=photo)
  int svg_color_precision = 0;     // --svg-color-precision (0=adaptive)
  int svg_filter_speckle = 4;      // --svg-filter-speckle
  int svg_max_paths = 500;         // --svg-max-paths
  int svg_max_svg_bytes = 262144;  // --svg-max-svg-bytes (256KB)
  float svg_fidelity_threshold = 55.0f;  // --svg-fidelity-threshold
  bool svg_exclude_lcp = true;           // --svg-exclude-lcp
  int svg_timeout_ms = 500;              // --svg-timeout-ms

  // Compression levels (0 = disabled, gzip: 1-9, brotli: 1-11)
  int gzip_level = 6;
  int brotli_level = 6;

  // HTML transformation toggles
  bool disable_lazy_load = false;             // --no-lazy-load-images
  bool disable_image_dimensions = false;      // --no-image-dimensions
  bool disable_lcp_preload = false;           // --no-lcp-preload
  bool disable_preconnect_injection = false;  // --no-preconnect-injection
  bool enable_speculation_rules = false;      // --enable-speculation-rules
  bool disable_async_css = false;             // --no-async-css
  // FOUC guard: the minimum critical/total CSS byte coverage required to DEFER
  // the full stylesheet (the async-CSS rel="preload" swap). Below this floor the
  // inlined critical block cannot bridge first paint (e.g. ~830 B of dark-color
  // overrides deferring a 110 KB layout/font sheet -> ~0.0075 coverage -> an
  // unstyled flash). Critical CSS is still INJECTED; only the deferral is
  // suppressed (the sheet stays render-blocking). 0.0 disables the gate (legacy
  // behavior). --async-css-min-coverage.
  float async_css_min_coverage = 0.10f;
  // Sheets smaller than this are always safe to defer (the FOUC window is
  // trivial), regardless of coverage. --async-css-min-deferred-bytes.
  size_t async_css_min_deferred_bytes = 15000;
  // Diagnostic-only: defer even when the sufficiency gate above refuses.
  // CLI-only (--unsafe-force-async-css) and deliberately not a config_file.cc
  // key, so it can never reach the RCU hot-reload path. See
  // unsafe_force_async_css.h.
  bool unsafe_force_async_css = false;
  bool disable_script_deferral = false;        // --no-script-deferral
  bool disable_css_import_flattening = false;  // --no-css-import-flattening

  // Security limits
  size_t max_url_length = 8192;
  size_t max_html_size = 5ULL * 1024 * 1024;    // 5MB
  size_t max_css_size = 2ULL * 1024 * 1024;     // 2MB
  size_t max_js_size = 2ULL * 1024 * 1024;      // 2MB
  size_t max_image_size = 10ULL * 1024 * 1024;  // 10MB

  // Cache key normalization (NON-reloadable — changes cache key semantics)
  std::vector<std::string> strip_query_extensions;  // e.g., {".jpg", ".png"}
  std::vector<std::string> strip_query_groups;  // e.g., {"images", "static"}
  std::vector<std::string>
      strip_query_params;  // e.g., {"utm_source", "fbclid"}
  std::vector<std::pair<std::string, std::string>>
      host_aliases;  // src→dst pairs

  // There is deliberately NO license / storefront / telemetry configuration
  // here.  Since 2.1 the daemon holds no license state and
  // makes no outbound request of its own accord; the only egress is what the
  // operator configures below (origin fetches, key directories).

  // Web Bot Auth (observe-only verifier — OFF by default).
  // When enabled, the worker periodically warm-fetches the configured key
  // directories (SSRF-guarded spawn-curl, see webbotauth_warmer.h) and
  // publishes the toggle + keys to nginx, which classifies requests.  The
  // verdict is a label + counter ONLY; request handling never changes.
  bool web_bot_auth = false;  // --web-bot-auth
  // Key-directory JWKS URLs (https), e.g.
  // "https://example.com/.well-known/http-message-signatures-directory".
  // --web-bot-auth-key-directory (repeatable).
  std::vector<std::string> web_bot_auth_key_directories;
  // Operator verified-bot registry, "keyid=name,keyid2=name2".
  // --web-bot-auth-verified-bots.
  std::string web_bot_auth_verified_bots;
  // Web Bot Auth opt-in counter mode (experimental — OFF by
  // default).  One of "off", "private", or "public"; gates the well-known
  // counter endpoint nginx exposes.  --web-bot-auth-public-counter.  The
  // gating bearer token is a SEPARATE secret carried via the
  // PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN env var (never in shared config).
  std::string web_bot_auth_public_counter = "off";

  // RSL-CAP enforcement (experimental — OFF by default).  The
  // operator-gated ENFORCEMENT sibling of the observe-only verifier above:
  // when enabled, the front-end validates an `Authorization: License <token>`
  // capability token against a required license/scope and returns 401/402/pass
  // (status codes ONLY — it never settles, meters, or handles payment).  Keys
  // for the token issuers are warm-fetched from the configured key directories
  // into a SEPARATE trust realm ("rsl") and published to nginx exactly like the
  // observe-only keys.  v1 scope: key-directory validation only (no live
  // token-introspection endpoint / remote authorization calls).
  bool rsl_cap_enforcement = false;  // --rsl-cap-enforcement
  // Issuer key-directory JWKS URLs (https); the worker warms these into realm
  // "rsl".  --rsl-cap-key-directory (repeatable).
  std::vector<std::string> rsl_cap_key_directories;
  // The license id and scope a token MUST grant to be authorized.  Empty
  // requested fields fail closed (kUnlicensed -> 402).
  std::string rsl_cap_requested_license;  // --rsl-cap-requested-license
  std::string rsl_cap_requested_scope;    // --rsl-cap-requested-scope
  // Optional issuer pin: when set, an otherwise-authorized token whose `iss`
  // does not match is rejected as an unknown issuer (401).  Empty = unbound.
  std::string rsl_cap_issuer;  // --rsl-cap-issuer

  // HTTP Management API configuration.
  //
  // Management API invariant, enforced at config-parse time with a fatal
  // named refusal: REMOTE IS NEVER UNAUTHENTICATED, AND UNAUTHENTICATED IS
  // NEVER REMOTE.  The API is off unless a port or a socket is configured;
  // the unix socket is the default local transport (filesystem permission is
  // the credential); a non-loopback TCP bind needs BOTH a token and
  // --api-allow-remote; a tokenless bind needs --api-no-auth and can only
  // ever be loopback or the socket.
  int api_port = 0;               // 0 = disabled (use --api-port to enable)
  std::string api_bind_address;   // Override HttpServerConfig::bind_address
  std::string api_socket_path;    // HTTP over AF_UNIX (--api-socket)
  std::string api_token;          // Auth token (also PAGESPEED_API_TOKEN)
  bool api_read_open = false;     // The allow-listed GET routes and the
                                  // stats/events streams open even with a
                                  // token; the log stays token-only
                                  // (--api-read-open)
  bool api_allow_remote = false;  // Deliberate non-loopback bind
  bool api_no_auth = false;       // Deliberate tokenless local API
  bool security_headers =
      true;  // --no-security-headers / PAGESPEED_SECURITY_HEADERS=false
  std::string console_dir;  // Path to web console SPA files

  // Browser analysis configuration
  BrowserAnalysisConfig browser_analysis;

  // Allow capture endpoints to target private/loopback URLs.
  // For Docker/development environments only.
  bool allow_private_urls = false;

  // Max entries in the processed-set before overflow rotation.
  // Default 10000.  Lowerable in tests for dedup coverage.
  size_t max_processed_entries = 10000;
};

// Atomic shared_ptr wrapper for RCU hot-reload pattern.
// Provides .load() / .store() with the same semantics as C++20
// std::atomic<std::shared_ptr<T>>, but works on platforms where
// that specialization is not yet available (e.g., Apple libc++).
template <typename T>
class AtomicSharedPtr {
 public:
  AtomicSharedPtr() = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> p) : ptr_(std::move(p)) {}
  AtomicSharedPtr(const AtomicSharedPtr&) = delete;
  AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

  std::shared_ptr<T> load() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ptr_;
  }

  void store(std::shared_ptr<T> p) {
    std::lock_guard<std::mutex> lock(mu_);
    ptr_ = std::move(p);
  }

 private:
  mutable std::mutex mu_;
  std::shared_ptr<T> ptr_;
};

// Reason for an HTML processing cooldown entry.
enum class CooldownReason : uint8_t {
  kProcessing = 0,    // In-flight guard (60s, cleared on success)
  kWriteFailure = 1,  // Mmap reader contention (60s)
  kRevalidation = 2,  // CSS not yet cached (3s)
};

// Convert CooldownReason to a lowercase string for JSON serialization.
const char* CooldownReasonToString(CooldownReason r);

// Internal cooldown map entry (stores timing + reason).
struct CooldownEntry {
  std::chrono::steady_clock::time_point created_at;
  std::chrono::steady_clock::time_point expiry;
  CooldownReason reason;
};

// Public cooldown info returned by query methods.
struct CooldownInfo {
  CooldownReason reason;
  int remaining_seconds;
  int duration_seconds;
};

// Cooldown info for a specific URL (used by ListActiveCooldowns).
struct CooldownListEntry {
  std::string url;
  std::string hostname;
  std::string scheme;
  CooldownReason reason;
  int remaining_seconds;
  int duration_seconds;
};

// The PSOL Factory Worker daemon.
// Listens on a Unix socket for cache notifications from Nginx,
// reads content from cache, processes it, and writes optimized
// variants back.
class Worker {
 public:
  explicit Worker(const WorkerConfig& config, MessageHandler* handler);
  ~Worker();

  // Initialize the worker (create socket, open cache, etc.)
  bool Initialize();

  // Run the event loop (blocks until Shutdown is called)
  void Run();

  // Request graceful shutdown (safe to call from any thread).
  void Shutdown();

  // Async-signal-safe shutdown request.  Only calls uv_async_send()
  // which is documented as async-signal-safe by libuv.  Use this from
  // signal handlers instead of Shutdown().
  void RequestShutdown();

  // De-alias copy verification (issue #934).  After a caller copies
  // a ReadResult's content() into an owned buffer — and BEFORE it releases the
  // borrow — this proves the copy is untorn.  Only a live-lease mmap disk
  // borrow can be overwritten in place by a write-buffer wrap: a released
  // handle (already de-aliased upstream), a RAM-cache hit (handle-owned
  // buffer, detected via is_ram_cache_hit(), NOT content_file_offset() —
  // the offset also goes kNoFileOffset for empty docs and per-region-mapping
  // fallback, where the borrow still aliases the mmap), and a leases-disabled
  // config (read_lease_duration == 0) are all trivially safe.
  // For an mmap disk borrow under a live lease, renew_lease() == true proves
  // the stripe's epoch never moved while the bytes were copied (untorn);
  // false means a wrap raced the copy and the caller must discard it and
  // treat the read as a cache miss.  Static so the de-alias seam is
  // unit-testable in isolation (issue #934 regression).
  static bool BorrowCopyUntorn(ReadResult& rr, bool leases_enabled);

  // TEST-ONLY seam (issue #934 de-alias regression).  When set, invoked on
  // the worker-pool thread immediately after the HTML origin read succeeds
  // and BEFORE the de-alias copy + BorrowCopyUntorn verification, so a test
  // can force the stripe's wrap epoch to move inside the
  // read->copy->verify window (from outside that window is microseconds
  // wide and cannot be hit deterministically).  Never set in production;
  // the only cost there is one null std::function check per HTML
  // notification.
  std::function<void()> test_hook_after_html_origin_read_;

  // TEST-ONLY seam.  When set, invoked on the worker-pool thread before each
  // image variant write; returning true makes that write fail as a cache
  // write would, so a test can produce an incomplete variant matrix on
  // demand instead of waiting for a real write race.  Never set in
  // production; the cost there is one null std::function check per write.
  std::function<bool()> test_hook_fail_image_variant_write_;

  // Get the socket path
  const std::string& socket_path() const { return config_.socket_path; }

  // Get the health check socket path (pre-computed for signal safety).
  const std::string& health_socket_path() const { return health_socket_path_; }

  // Get the management socket path (pre-computed for signal safety).
  const std::string& mgmt_socket_path() const { return mgmt_socket_path_; }

  // Invalidate all cached variants for a URL (purge).
  // Returns the number of entries deleted.
  // When clear_heal_state is true (default, for manual purges), the per-URL
  // auto-heal counter is reset.  Auto-heal passes false to avoid loops.
  int InvalidateUrl(const std::string& url, const std::string& hostname = "",
                    const std::string& scheme = "https",
                    std::string_view site = {}, bool clear_heal_state = true);

  // Clear dedup set and cooldown for a URL without removing cached content.
  void ClearDedupAndCooldown(const std::string& url,
                             const std::string& hostname,
                             const std::string& scheme = "https",
                             std::string_view site = {});

  // Build the inline rebuild notification HandleOriginRefreshed dispatches
  // after purging a URL's stale variant set (Issue C).  Factored out as a pure
  // function so the agent_request-propagation contract is unit-testable without
  // a live cache or Chrome: the rebuild re-processes at the default capability
  // mask, and propagates agent_request when the URL had an agent markdown
  // variant before the purge (had_markdown) OR ever rendered one (has_intent —
  // durability #19/D3, heals a variant lost in a prior multi-churn
  // race) — so a re-entry rebuilds the markdown via the warm-template branch,
  // while pages that never rendered markdown keep agent_request=false
  // (demand-gate preserved).
  static CacheNotification MakeOriginRefreshRebuild(
      const CacheNotification& origin, bool had_markdown, bool has_intent);

  // Get the cache (for testing)
  PageSpeedCache* cache() const { return cache_.get(); }

  // Get the URL registry (for testing the cached alternate_count path).
  const UrlRegistry* url_registry() const { return url_registry_.get(); }

  // Get a snapshot of the current live configuration.
  // Thread-safe (uses AtomicSharedPtr).
  std::shared_ptr<const WorkerConfig> GetConfig() const {
    return live_config_.load();
  }

  // Replace the live configuration with a new snapshot.
  // Thread-safe (uses AtomicSharedPtr).
  void UpdateConfig(std::shared_ptr<const WorkerConfig> new_config);

  // Get the number of active connections (for testing/health)
  int active_connections() const {
    return active_connections_.load(std::memory_order_relaxed);
  }

  // Get the number of in-flight thread pool work items (for testing/health)
  int in_flight_work() const {
    return in_flight_work_.load(std::memory_order_relaxed);
  }

  // Number of optimized-copy records currently held (see
  // copy_written_current_).  For tests and diagnostics.
  size_t copy_record_count() const {
    std::lock_guard<std::mutex> lock(processed_set_mutex_);
    return copy_written_current_.size() + copy_written_previous_.size();
  }

  // Get processing statistics
  const WorkerStats& stats() const { return stats_; }

  // Test-only seam: reach the browser-analysis manager so a test can seed a
  // profile (BrowserAnalysisManager::TestStoreProfile) and exercise the
  // profile-driven critical-CSS path end-to-end without a live Chrome render.
  // Returns nullptr when browser analysis is disabled (the default).
  BrowserAnalysisManager* TestBrowserManager() {
    return browser_manager_.get();
  }

  // Get the shared mmap'd serve-time stats (nullptr until created).
  ServeStats* serve_stats() const { return serve_stats_; }

  // Get the event loop lag in microseconds.
  uint64_t event_loop_lag_us() const {
    return event_loop_lag_us_.load(std::memory_order_relaxed);
  }

  // Get the HTTP API server bound port (0 if disabled).
  int api_port() const;

  // The resolved headless-Chrome sandbox state
  // ("disabled" | "on" | "unavailable" | "off").
  BrowserSandboxState browser_sandbox_state() const {
    return browser_sandbox_state_;
  }

  // Get the WsManager (for event posting from external code).
  WsManager* ws_manager() const { return ws_manager_.get(); }

  // Query cooldown state for a specific URL.  Thread-safe.
  std::optional<CooldownInfo> GetCooldownForUrl(
      const std::string& url, const std::string& hostname,
      std::string_view scheme = "https", std::string_view site = {}) const;

  // List all active (non-expired) cooldowns.  Thread-safe.
  std::vector<CooldownListEntry> ListActiveCooldowns() const;

  // Check if a URL was purged after a notification was dispatched.
  // Returns true if the current per-URL purge generation OR the global
  // (full-purge) generation exceeds the baseline captured at dispatch
  // time, meaning any writes would re-populate stale content that was
  // explicitly purged.
  bool WasPurgedSinceDispatch(std::string_view url, std::string_view hostname,
                              const PurgeDispatchGen& dispatch_gen,
                              std::string_view scheme = "https",
                              std::string_view site = {}) const;

  // Convenience overload checking the per-URL generation only (all_gen
  // baseline 0).  Equivalent to the struct overload for workers that
  // never executed a full cache purge.
  bool WasPurgedSinceDispatch(std::string_view url, std::string_view hostname,
                              uint64_t dispatch_purge_gen,
                              std::string_view scheme = "https",
                              std::string_view site = {}) const {
    return WasPurgedSinceDispatch(
        url, hostname, PurgeDispatchGen{dispatch_purge_gen, 0}, scheme, site);
  }

  // Capture the current purge-fence baseline for a URL.  Called at
  // notification dispatch time; the result is compared by
  // WasPurgedSinceDispatch before AND after every variant write.
  PurgeDispatchGen CapturePurgeGen(std::string_view url,
                                   std::string_view hostname,
                                   std::string_view scheme = "https",
                                   std::string_view site = {}) const;

  // True when the known sites may not name every site in the volume: sites
  // were seen past the cap since start, or the known-sites file existed at
  // start and could not be read (CacheApiContext::known_sites_incomplete).
  bool KnownSitesIncomplete() const;

  // Full cache purge: resets the Cyclone volume, bumps the global purge
  // generation (fencing all in-flight variant writes), and clears the
  // dedup/cooldown/purge-generation maps.  Returns an empty string on
  // success, an error message on failure.  Exposed publicly for the
  // HTTP cache API handler and tests.
  std::string ResetCache();

 private:
  // True if THIS url has no kAgentMarkdown variant, or
  // one stamped with a different origin_html_hash than `origin_hash` (stale).
  // Used to force a per-URL agent render when the template's perf profile
  // already exists (so the perf-driven enqueue is skipped).
  bool AgentMarkdownNeedsBuild(
      const std::string& url, const std::string& hostname,
      const std::string& scheme, std::string_view site,
      const std::array<std::byte, 32>& origin_hash) const;

  // True if the /llms.txt index for (hostname, scheme) must be
  // (re)built — absent kLlmsTxtMeta, a changed sitemap content-hash, or past the
  // regen TTL ceiling. Reads the kLlmsTxtMeta sentinel.
  bool LlmsTxtNeedsBuild(const std::string& hostname, const std::string& scheme,
                         std::string_view site,
                         const std::array<std::byte, 32>& live_sitemap_hash,
                         int64_t now_sec) const;

  // Recompute and cache the unique alternate count for `url` in the URL
  // registry, off the event-loop thread.  Shared by the HandleNotification
  // scope guard and the async agent-markdown write path so both use identical
  // counting semantics (CountUniqueAlternates).  No-op if the registry/cache
  // are unset or the URL was LRU-evicted since it was recorded.
  void RefreshAlternateCount(const std::string& url,
                             const std::string& hostname,
                             const std::string& scheme, std::string_view site);

  // Client connection context
  struct ClientContext;

  // libuv callbacks
  static void OnNewConnection(uv_stream_t* server, int status);
  static void OnClientRead(uv_stream_t* client, ssize_t nread,
                           const uv_buf_t* buf);
  static void OnAlloc(uv_handle_t* handle, size_t suggested_size,
                      uv_buf_t* buf);
  static void OnClientClose(uv_handle_t* handle);
  static void OnTimerClosedThenPipe(uv_handle_t* handle);
  static void OnConnectionTimeout(uv_timer_t* timer);
  static ClientContext* GetClientContext(uv_stream_t* stream);
  static void CloseClientConnection(ClientContext* ctx);

  // Health check callbacks
  static void OnHealthConnection(uv_stream_t* server, int status);
  static void OnHealthWriteDone(uv_write_t* req, int status);

  // Management socket callbacks
  static void OnMgmtConnection(uv_stream_t* server, int status);
  static void OnMgmtRead(uv_stream_t* client, ssize_t nread,
                         const uv_buf_t* buf);
  static void OnMgmtAlloc(uv_handle_t* handle, size_t suggested_size,
                          uv_buf_t* buf);
  static void OnMgmtWriteDone(uv_write_t* req, int status);
  void HandleMgmtCommand(uv_stream_t* client, const std::string& command);

  // Build Prometheus-format metrics string from current stats.
  void BuildPrometheusMetrics(std::string& response) const;

  // Graceful shutdown helpers
  static void OnShutdownTimeout(uv_timer_t* timer);
  void BeginGracefulShutdown();
  void CheckShutdownComplete();

  // Handle a cache notification (fire-and-forget, no response).
  // Called from thread pool worker threads.
  void HandleNotification(const CacheNotification& notification,
                          const PurgeDispatchGen& purge_gen = {});

  // The stylesheet bytes a page is optimized and judged against, plus the two
  // "a declared sheet was not gathered" flags the callers gate on.
  struct CombinedCssResult {
    std::string css;
    // ANY declared external sheet was not gathered (cross-origin included).
    // Drives the async-CSS cold-cache fail-safe.
    bool external_css_missing = false;
    // A sheet that could plausibly enter this cache was not gathered. Narrower;
    // drives only the self-heal re-notify.
    bool revalidatable_css_missing = false;
    // The page's cascade-layer order over every stylesheet source in document
    // order, or why it is not known. Decides whether a critical
    // block that names a layer can go before the sheets, with an `@layer`
    // statement in front (DecideCriticalCssLayerPlacement). Not part of the
    // byte contract above.
    CascadeLayerOrder layer_order{};
  };

  // Assemble the combined stylesheet: the page's inline CSS, then each declared
  // external sheet resolved from cache, newline-joined in document order.
  //
  // THE BYTE SEQUENCE IS A CONTRACT, not an implementation detail: it is the
  // input to CombinedCssValidationHash, which binds a stored validation record
  // to the stylesheet it was made against. Any caller needing those bytes MUST
  // call this rather than reassemble them. The full contract, and its known
  // instabilities, are written out beside that function in
  // src/browser/optimization_profile.h.
  // `count_lookups` reports each cache probe into the serve-path telemetry
  // (selector_invocations).  The browser-analysis caller passes false: it
  // assembles the same bytes for a page that is ALSO about to be assembled on
  // the serve path, and counting both would report the optimizer doing twice
  // the lookups it does.  It does not touch the byte sequence — the contract
  // above is about bytes, and telemetry is not one of them.
  // `gate_may_fetch` false (the browser-analysis caller, on the event loop)
  // answers the sub-resource gate from its memo only; see
  // SubresourceAdmitted.
  CombinedCssResult BuildCombinedCss(const HtmlScanResult& scan_result,
                                     const CacheNotification& notification,
                                     const WorkerConfig& cfg,
                                     bool count_lookups = true,
                                     bool gate_may_fetch = true);

  // Whether the cached bytes of `read_url` (a cache URL, keyed under
  // `read_host`) may go into an output built for `notification`: the server
  // serves that URL to an anonymous client (SubresourceGate).  Not admitted
  // reads as absent.  `may_fetch` false answers from the gate's memo only,
  // for callers that must not block.
  // `shared_copy`: the bytes go into a copy served to other clients
  // (/llms.txt), so the gate asks with its shared-copy HEAD.
  bool SubresourceAdmitted(const CacheNotification& notification,
                           std::string_view read_url,
                           std::string_view read_host, bool may_fetch = true,
                           const AlternateMetadata* entry = nullptr,
                           bool shared_copy = false);

  // The best cached entry for `read_url` (keyed under `read_host` and the
  // notification's site) when it may go into an output built for
  // `notification`, else nullopt.  When the worker may not ask the page's
  // host, a stylesheet or image the front end recorded from an anonymous
  // request is admitted as read, without asking the server; any other entry,
  // and every entry under a host the worker may ask, goes through
  // SubresourceAdmitted first, exactly as before the read.  `refused`, when
  // given, is set when the gate did not admit the URL
  // (as opposed to the cache holding nothing for it).
  std::optional<ReadResult> ReadAdmittedSubresource(
      const CacheNotification& notification, std::string_view read_url,
      std::string_view read_host, bool may_fetch = true,
      bool* refused = nullptr);

  // Write a minified text variant (CSS or JS) to cache, or compressed-only
  // when the minified output is not smaller than the original.
  // Shared implementation for CSS and JS paths in HandleNotification.
  void WriteTextVariant(const CacheNotification& notification,
                        ReadResult& read_result,
                        std::string_view original_input,
                        std::string_view minified, ContentType content_type,
                        const char* type_name,
                        std::atomic<uint64_t>& processed_stat,
                        std::atomic<uint64_t>& already_optimal_count,
                        std::atomic<uint64_t>& already_optimal_bytes,
                        const std::function<bool()>& purge_check,
                        const PurgeDispatchGen& purge_gen,
                        const std::shared_ptr<const WorkerConfig>& cfg,
                        bool record_source_hash = true);

  // Result of WriteImageVariants -- outputs consumed by SVG gate and
  // incomplete-matrix retry tracking.
  struct ImageVariantResult {
    size_t expected_writes = 0;
    size_t successful_writes = 0;
    size_t smallest_raster_size = 0;
  };

  // Generate and write missing raster image variants across all enabled
  // dimension combinations (save-data x density x viewport x format).
  // Extracted from HandleNotification to flatten nesting.  cfg is the
  // RCU config snapshot taken by the caller (not re-loaded here).
  // image_data MUST NOT alias cache-backed mmap bytes (issue #934): the
  // caller copies the origin content out of its ReadResult borrow and
  // releases the handle before calling.  The matrix here can run for
  // many seconds (up to 2x2x3 combinations, AVIF encodes taking seconds
  // each on large inputs); holding — and renewing — a read lease on the
  // very stripe being written to deferred the worker's own variant
  // writes behind its own lease at cache-full (self-starvation).
  ImageVariantResult WriteImageVariants(
      const CacheNotification& notification,
      CapabilityMask target_mask,  // by value: lambda copies into write_mask
      std::string_view image_data, const AlternateMetadata& base_meta,
      const std::function<bool()>& purge_check,
      const std::shared_ptr<const WorkerConfig>& cfg,
      // SHA-256 of image_data, computed by the caller (the image pipeline
      // already has it); keys the decline tombstone so a changed source
      // retries the declined slots.
      const std::array<std::byte, 32>& origin_hash);

  // Decline tombstone (#1382): read the per-URL kDeclineTombstone sentinel
  // and return its records when they were written for |origin_hash| (an
  // empty vector otherwise -- including when the sentinel holds a blob for
  // a different source, which is inert and must not stop a recompute).
  std::vector<DeclineTombstoneRecord> LoadDeclineTombstone(
      const CacheNotification& notification,
      const std::unordered_set<uint8_t>& existing_ids,
      const std::array<std::byte, 32>& origin_hash);

  // Merge |added| (slots declined by the run that just finished) into the
  // tombstone and store it under the kDeclineTombstone sentinel. |kept| is
  // the record set LoadDeclineTombstone returned for this source.
  void StoreDeclineTombstone(const CacheNotification& notification,
                             const std::array<std::byte, 32>& origin_hash,
                             const std::vector<DeclineTombstoneRecord>& kept,
                             std::vector<DeclineTombstoneRecord> added);

  // Mark a variant as processed in the in-memory dedup set.
  // If a newer purge (per-URL or global) has occurred since purge_gen was
  // captured, the insertion is skipped (stale in-flight notification).
  void MarkVariantProcessed(const std::string& url, const std::string& hostname,
                            std::string_view scheme, std::string_view site,
                            AlternateId id,
                            const PurgeDispatchGen& purge_gen = {});

  // Record that this process wrote the optimized copy `id` of a URL, so a
  // later notification can tell a copy that has gone missing from a copy
  // that was never due.  Applies the same purge test as
  // MarkVariantProcessed and is called right after it: that function clears
  // any earlier record for the key, so only the latest verdict counts.  The
  // record is kept only while the key's processed mark exists.
  void MarkCopyWritten(const std::string& url, const std::string& hostname,
                       std::string_view scheme, std::string_view site,
                       AlternateId id, const PurgeDispatchGen& purge_gen = {});

  // Erase every processed mark of one URL -- all of its ids -- together
  // with the copy records that belong to them.  `prefix` is the key up to
  // and including the separator before the id.  The caller holds
  // processed_set_mutex_.
  void EraseUrlMarksLocked(const std::string& prefix);

  // Set a write-failure cooldown for a URL to prevent unbounded
  // re-processing loops when cache writes persistently fail.
  void SetWriteFailureCooldown(const std::string& url,
                               const std::string& hostname,
                               std::string_view scheme, std::string_view site);

  // SRI (issue #656): register subresource URLs referenced from HTML with
  // an integrity attribute.  Optimized variants at the same URL would fail
  // the browser's hash check.  The scan's integrity_pinned_urls are
  // unresolved href/src values, resolved against the document base (the page
  // URL or its <base href>, DocumentBaseOf); notification is the HTML page's
  // notification (provides the page URL, hostname, scheme).  On first
  // registration of a URL, any already-written variant is dropped (whole
  // cache key) so nginx re-caches the original.
  void RegisterIntegrityPinnedUrls(const HtmlScanResult& scan_result,
                                   const CacheNotification& notification);

  // True if the notification's URL was registered as integrity-pinned.
  bool IsIntegrityPinned(const CacheNotification& notification) const;

  // Dispatch a notification to the libuv thread pool for processing.
  // Called on the event loop thread; validates URL then queues work.
  void DispatchNotification(const CacheNotification& notification);

  // Emit the rate-limited WARNING for a notification refused because the
  // sending peer speaks another wire version.  The refusal itself has already
  // happened and is already counted; this is only the operator-facing half.
  // Called on the event loop thread.
  void WarnIpcVersionMismatch(uint8_t peer_version);

  // Decide whether a notification's per-request option context is usable,
  // counting and (rate-limited) logging when it is not.  A context that
  // validates is accepted whatever it names — including a non-default one,
  // whose work is then processed and stored under the default context — and
  // only the four validation failures are refused.  Returns true when
  // processing may continue. Called on the event loop thread.
  bool AcceptOptionContext(const CacheNotification& notification);

  // Whether `notification` may start fetches from its hostname (the
  // /llms.txt build, browser analysis and the agent render).
  static bool MayRequestHostFetches(const CacheNotification& notification);

  // Checks the site and sender bits (wire v6): a notification from the module
  // must carry a site and must not ask for page work (HTML, the llms.txt
  // build, an agent request, a vouched host).  Returns false, counted, when
  // the notification is refused.  Called on the event loop thread.
  bool AcceptSiteAndSender(const CacheNotification& notification);

  // Thread pool callbacks for uv_queue_work.
  static void OnNotificationWork(uv_work_t* req);
  static void OnNotificationDone(uv_work_t* req, int status);

  // True when this URL's identity original records that the ORIGIN does its
  // own `Accept` negotiation (AlternateMetadata::kFlagOriginVariesAccept).
  // Such a URL is stored as the origin sent it and never optimized: deriving
  // variants from the one representation we captured would layer a second
  // negotiation on the origin's own.
  //
  // Asks about the URL, not about a representation of it — the marker lives
  // on the identity original, which is the only alternate the store path
  // writes, so a caller that inspected whichever alternate a request selected
  // would read an unmarked variant whenever a pre-marker one still exists.
  // False when the cache is unavailable or the original is absent: there is
  // nothing to optimize from either way.
  bool OriginNegotiatesByAccept(const CacheNotification& notification) const;

  // Handle a warmup request (sentinel mask 0xFFFFFFFE): generate
  // full format variant matrix for images, ensure minified versions
  // for CSS/JS.
  void HandleWarmupRequest(const CacheNotification& notification,
                           const PurgeDispatchGen& purge_gen = {});

  // Handle a lazy /llms.txt (re)build (sentinel mask kLlmsTxtSentinel).
  // Off the request path: fetches the sitemap + (capped) per-page summaries
  // via the G1 own-origin pin, assembles the index, and writes the kLlmsTxt +
  // kLlmsTxtMeta sentinels. Gated on the operator flags agent_optimize &&
  // agent_optimize_llms_txt only. Deduped to one in-flight build per site key.
  void HandleLlmsTxtBuild(const CacheNotification& notification,
                          const PurgeDispatchGen& purge_gen = {});

  // Async callback for thread-safe shutdown
  static void OnShutdownAsync(uv_async_t* handle);

  // Null-safe logging helpers — avoid repeating `if (handler_)` 107 times.
  template <typename... Args>
  void LogInfo(const char* format, Args... args) {
    if (handler_) handler_->Info(format, args...);
  }
  template <typename... Args>
  void LogWarning(const char* format, Args... args) {
    if (handler_) handler_->Warning(format, args...);
  }
  template <typename... Args>
  void LogError(const char* format, Args... args) {
    if (handler_) handler_->Error(format, args...);
  }

  // Startup configuration.  Infrastructure fields (socket_path,
  // max_connections, max_buffer_size, connection_timeout_ms,
  // shutdown_timeout_ms, num_threads, cache_path, cache_size_bytes)
  // are read from config_ and never hot-reloaded — changing these
  // at runtime is nonsensical.
  WorkerConfig config_;

  // Pre-computed socket paths for async-signal-safe access in Shutdown().
  // Computed once during Initialize() to avoid heap allocation in signal
  // handler context.
  std::string health_socket_path_;
  std::string mgmt_socket_path_;

  // URL normalization config, built from WorkerConfig at startup.
  UrlNormalizationConfig url_norm_config_;

  // Live configuration for RCU hot-reload.  Initialized from
  // config_ at startup; runtime reads go through live_config_.
  // Hot-reloadable fields: quality settings (jpeg_quality, webp_quality,
  // avif_quality, savedata_*), viewport widths, content toggles
  // (html_mode, disable_css, etc.), SSIMULACRA2 thresholds.
  // Uses AtomicSharedPtr for portable RCU semantics.
  AtomicSharedPtr<const WorkerConfig> live_config_;

  MessageHandler* handler_;
  std::unique_ptr<uv_loop_s> loop_;
  std::unique_ptr<uv_pipe_s> server_;
  std::unique_ptr<uv_async_s> shutdown_async_;
  std::atomic<bool> running_ = false;

  // Resource limit tracking
  std::atomic<int> active_connections_ = 0;
  std::atomic<int> mgmt_active_connections_ = 0;
  std::atomic<int> in_flight_work_{0};
  bool shutting_down_ = false;

  // In-flight URL dedup: tracks URLs currently being processed in the
  // thread pool.  Accessed only from the event loop (DispatchNotification
  // and OnNotificationDone), so no locking is needed.
  std::unordered_set<std::string> in_flight_urls_;

  // Origin-refreshed sentinels turned away by the in-flight guard, one per
  // in-flight key, dispatched from OnNotificationDone.  Event loop only.
  std::unordered_map<std::string, CacheNotification> pending_origin_refresh_;

  // Health check socket
  std::unique_ptr<uv_pipe_s> health_server_;

  // Management socket
  std::unique_ptr<uv_pipe_s> mgmt_server_;

  // Shutdown timer for graceful shutdown timeout
  std::unique_ptr<uv_timer_s> shutdown_timer_;

  // Periodic stats refresh timer (avoids calling cache_->Stats() on
  // the event loop, which can contend with Cyclone locks held by
  // thread pool workers).
  std::unique_ptr<uv_timer_s> stats_refresh_timer_;
  static void OnStatsRefresh(uv_timer_t* timer);

  // Cached cache stats (refreshed by stats_refresh_timer_).
  std::atomic<uint64_t> cached_cache_entries_{0};
  std::atomic<uint64_t> cached_cache_bytes_{0};
  std::atomic<uint64_t> cached_hit_flush_ok_{0};
  std::atomic<uint64_t> cached_hit_flush_fail_{0};
  std::atomic<uint64_t> cached_hit_flush_delta_{0};
  std::atomic<uint64_t> cached_hit_flush_fsyncs_{0};

  // Asks the server, once per memo period, whether a cached sub-resource is
  // served to anonymous clients before another URL's output uses it.
  std::shared_ptr<SubresourceGate> subresource_gate_ =
      std::make_shared<SubresourceGate>(MakeAnonymousHeadFn(),
                                        MonotonicNowMs());

  // Cache and image transcoder
  std::unique_ptr<PageSpeedCache> cache_;
  bool cache_degraded_ = false;  // true when cache was configured but failed
  std::unique_ptr<ImageTranscoder> image_transcoder_;

  // Processing statistics
  WorkerStats stats_;

  // Shared mmap'd serve-time bandwidth stats (written by nginx, read here).
  ServeStats* serve_stats_ = nullptr;

  // Rate-limit state for the "notify peer speaks another wire version"
  // WARNING.  A skewed peer does not send one bad message, it sends every
  // message, so the line is rate-limited to one per minute.
  //
  // The refinement over a plain timer: each DISTINCT peer version logs the
  // first time it is seen, because a second, differently skewed peer appearing
  // is exactly the event an operator needs, and a timer armed by the first
  // peer would hide it.  Tracked as a 256-bit seen-set rather than
  // "last version", so a sender alternating version bytes cannot re-arm the
  // immediate path on every message — the unconditional lines are bounded at
  // one per version value for the life of the process, and everything after
  // that falls back to the 1/min floor.
  //
  // Only the LOG is rate-limited; the counter is unconditional.
  std::atomic<int64_t> last_ipc_version_warn_us_{0};
  // Same one-line-per-minute floor as the version warning above, and separate
  // from it so that a steady stream of one condition cannot hide the other.
  std::atomic<int64_t> last_option_context_warn_us_{0};
  // One bit per option-context condition already reported: the three
  // OptionContextStatus values that can actually reach the refusal path
  // (kUnknownFormat, kMalformedSignature, kSignatureMismatch -- kOk and
  // kEmpty are accepted and return earlier, and kTooLarge cannot get this far
  // because the frame reader bounds the payload with the same constant, so it
  // is refused as a malformed frame), plus 0x80 for the once-per-process INFO
  // on the first accepted non-default context.  The bit is derived from the
  // status value rather than from that list, so kTooLarge would still get a
  // bit of its own if a future frame format let one through.  Same
  // shape as ipc_peer_versions_seen_ below, and load-bearing for the same
  // reason -- the conditions have different fixes, so the frequent one must
  // not be able to suppress the rare one.  The 0x80 arm shares only this
  // seen-set, never last_option_context_warn_us_: an accepted context is not
  // a problem and must not be able to hold off a refusal warning.
  std::atomic<uint8_t> option_context_conditions_seen_{0};
  std::atomic<uint64_t> ipc_peer_versions_seen_[4] = {};

  // PURGE authentication token (from PAGESPEED_PURGE_TOKEN env var).
  // When non-empty, PURGE commands require AUTH on the connection.
  std::string purge_token_;

  // Browser analysis manager (null when disabled).
  std::unique_ptr<BrowserAnalysisManager> browser_manager_;
  // What the startup sandbox probe resolved.  Reported by
  // GET /v1/health and /v1/stats so "is the browser sandboxed?" is answerable
  // without reading the journal.  Written once during Initialize().
  BrowserSandboxState browser_sandbox_state_ = BrowserSandboxState::kDisabled;

  // Hot URL tracking for speculation rules (Phase 1d).
  // Bounded LRU: most-recently-warmed URLs at front, max 50 entries.
  // Thread-safe: accessed from worker threads and event loop.
  static constexpr size_t kMaxHotUrls = 50;
  mutable std::mutex hot_urls_mutex_;
  std::deque<std::string> hot_urls_;         // LRU order, front = newest
  std::unordered_set<std::string> hot_set_;  // For O(1) dedup

  // Record a URL as hot (from warmup notification). Thread-safe.
  void RecordHotUrl(const std::string& url);

  // Get top N same-hostname hot URLs. Thread-safe.
  std::vector<std::string> GetHotUrls(std::string_view hostname,
                                      size_t max_count) const;

  // Notification deduplication: tracks (url|hostname|alternateId) keys that
  // the worker has already processed.  Avoids reading from cache (which would
  // pollute Cyclone's write-around RAM cache) by keeping state in memory.
  // Uses generational double-buffer: when the current set overflows, it
  // becomes the previous set (still consulted for lookups) instead of being
  // discarded entirely.  This preserves recently-added entries on overflow.
  // Cleared on URL invalidation.
  //
  // Lock ordering (must always acquire in this order to prevent deadlocks):
  //   purge_gen_mutex_ < processed_set_mutex_ < write_cooldown_mutex_
  // Cyclone's reset lock (PageSpeedCache per-call shared/exclusive lock)
  // nests INSIDE purge_gen_mutex_ (PurgeUrlAndBumpGeneration, ResetCache).
  // Never acquire purge_gen_mutex_ while holding a Cyclone lock.
  mutable std::mutex processed_set_mutex_;
  std::unordered_set<std::string> processed_current_;
  std::unordered_set<std::string> processed_previous_;
  // Keys, in the processed set's form, whose optimized copy THIS process
  // wrote (stylesheets and scripts).  A processed-set hit for such a key is
  // honoured only while that copy is still listed in the cache; when it is
  // gone and other copies of the URL remain, the URL is processed again.
  // Every verdict that writes no copy leaves no record here, which is what
  // keeps "nothing was due" from ever being read as "something was lost".
  // A record never exists without its mark: it is written into the
  // generation that holds the mark, rotated with the processed set in the
  // same step, and erased wherever the mark is erased (a new verdict, a
  // heal, a purge of the URL or of everything, an integrity-pin
  // registration, a content change).  Guarded by processed_set_mutex_.
  //
  // What the rule does not see: a copy this process did not write since
  // the key's latest verdict.  When an already optimized URL is processed
  // again while its copy is present -- after a restart, after its mark aged
  // out, or for another client class -- the source read finds the optimized
  // copy, nothing is written and no record is left; if that copy goes
  // missing later, the notification is skipped as before and the URL is
  // optimized again when its stored original is next recorded.
  std::unordered_set<std::string> copy_written_current_;
  std::unordered_set<std::string> copy_written_previous_;

  // Purge generation tracking: prevents in-flight workers from re-adding
  // stale dedup entries — or re-inserting purged content (issue #652) —
  // after a purge.  Each purge increments the generation for the URL;
  // variant writes and MarkVariantProcessed are rejected when the current
  // generation exceeds the baseline captured at dispatch time.
  // Guarded by purge_gen_mutex_ (accessed from thread pool workers and
  // event loop in InvalidateUrl).
  //
  // CRITICAL ORDERING (issue #652): PurgeUrlAndBumpGeneration holds
  // purge_gen_mutex_ ACROSS both the generation bump and the cache
  // Remove().  Dispatch-time baseline capture takes the same mutex, so a
  // notification that captures the post-purge generation is guaranteed to
  // start (and therefore read its source content) only after the Remove
  // completed — it can never resurrect pre-purge content.  Notifications
  // holding the pre-purge generation are rejected at the write fence
  // (before the write, and again after close_sync with an undo Remove for
  // the TOCTOU window).  This is deadlock-safe: Remove() only takes
  // Cyclone's SHARED reset lock, and no code path acquires
  // purge_gen_mutex_ while holding a Cyclone lock.
  static constexpr size_t kMaxPurgeGenEntries = 10000;
  mutable std::mutex purge_gen_mutex_;
  std::unordered_map<std::string, uint64_t> purge_generation_;
  std::unordered_map<std::string, uint64_t> purge_generation_previous_;
  // Worker-global purge generation, bumped by ResetCache (full purge).
  // Fences in-flight writes across ResetVolume the same way the per-URL
  // generations fence single-URL purges.  Guarded by purge_gen_mutex_.
  uint64_t purge_all_generation_ = 0;

  // Per-URL rate limit for origin-refreshed sentinel purges: a born-stale
  // upstream (e.g. a CDN serving Age > max-age) would otherwise trigger a
  // purge + full re-optimization cycle per request pair.  Guarded by
  // purge_gen_mutex_.  Bounded; on overflow only EXPIRED entries are
  // swept — live windows are never dropped (issue #652 review: a
  // wholesale clear defeated the limiter precisely under the >1024-URL
  // churn it was added to bound).  When all entries are in-window the
  // incoming sentinel is treated as rate-limited instead.
  static constexpr int kOriginRefreshMinIntervalSecs = 10;
  static constexpr size_t kMaxOriginRefreshEntries = 1024;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      origin_refresh_last_purge_;

  // Per-URL rate limit on the orphaned-entry heal
  //.  A sentinel-only family is not
  // always orphaned -- several paths mark an entry processed while writing
  // nothing servable (a decline tombstone, a deterministic minify failure,
  // an integrity-pinned URL, an oversized image) -- and re-processing those
  // never produces a variant.  Rather than enumerate them (open-ended, and
  // an id-only check cannot tell a still-valid decision from a stale one),
  // the heal is bounded to one re-process per URL per window.  Bounded like
  // origin_refresh_last_purge_: on overflow only EXPIRED windows are swept,
  // and when every window is live the heal is deferred rather than run
  // untracked -- an untracked heal is an unbounded one.  Guarded by
  // dedup_heal_mutex_, which is always taken ALONE, so it stays outside the
  // purge_gen_mutex_ < processed_set_mutex_ < write_cooldown_mutex_ order.
  static constexpr int kDedupHealMinIntervalSecs = 10;
  static constexpr size_t kMaxDedupHealEntries = 1024;
  mutable std::mutex dedup_heal_mutex_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      dedup_heal_last_;
  // Per-URL window for the recorded-original recheck (RecheckRecordedOriginal):
  // at most one restamp-or-decide per URL per kDedupHealMinIntervalSecs, so a
  // front end that re-records on every request cannot make every request a
  // rewrite.  Same sweep-expired-only overflow rule; same mutex.
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      origin_recheck_last_;

  // Durability (#19) — D2: SHA-256 of the FRESH origin body for a URL,
  // used at origin-refresh time to decide whether the rendered markdown variant
  // is still bound to live content (preserve) or has been superseded (purge).
  // Mirrors the normal HTML path's hash derivation (Issue B): hashes the raw
  // identity body, but when the identity slot already holds the worker's OWN
  // optimized output it reuses the durable raw-origin hash stamped on it, with a
  // final fallback to the live kContentHash sentinel for legacy blobs.  Returns
  // std::nullopt when no origin hash can be derived — the caller then purges the
  // variant as before (fail-safe: never preserve on an uncertain binding).
  std::optional<std::array<std::byte, 32>> FreshOriginHash(
      const std::string& url, const std::string& hostname,
      const std::string& scheme, std::string_view site);

  // Durability (#19) — D3: sticky set of ComposeInternalKey()s for URLs
  // that EVER held a markdown variant.  When a URL renders markdown then loses
  // the variant mid-rebuild across successive churns, had_markdown reads false
  // on the next sentinel and the demand-gate would never rebuild it.  The flag
  // makes the rebuild reliable (agent_request = had_markdown || has_intent)
  // without rendering markdown for pages that never had it.  In-process only
  // (resets on restart — a cold MISS re-establishes it); bounded + fail-open at
  // capacity (an over-cap URL simply reverts to had_markdown gating, never a
  // correctness issue).  Guarded by agent_markdown_intent_mutex_.
  static constexpr size_t kMaxAgentMarkdownIntentEntries = 100000;
  mutable std::mutex agent_markdown_intent_mutex_;
  std::unordered_set<std::string> agent_markdown_intent_urls_;
  void RecordAgentMarkdownIntent(const std::string& url,
                                 const std::string& hostname,
                                 const std::string& scheme,
                                 std::string_view site);
  bool HasAgentMarkdownIntent(const std::string& url,
                              const std::string& hostname,
                              const std::string& scheme,
                              std::string_view site) const;

  // Bump the purge generation for a URL and Remove() all its cached
  // alternates as one atomic step under purge_gen_mutex_ (see the
  // CRITICAL ORDERING comment above).  Inputs must be pre-normalized.
  // Returns 1 when the Remove deleted an entry, 0 otherwise.
  int PurgeUrlAndBumpGeneration(const std::string& norm_url,
                                const std::string& norm_host,
                                const std::string& scheme,
                                std::string_view site);

  // Bump the purge generation for a URL and remove every derived alternate
  // as one step under purge_gen_mutex_, keeping the origin's own recording
  // (the marked original of a URL that now negotiates on Accept).  Inputs must
  // be pre-normalized.  Failure is logged; stale copies then remain until the
  // next notification.
  void PurgeDerivedCopiesAndBumpGeneration(const std::string& norm_url,
                                           const std::string& norm_host,
                                           const std::string& scheme,
                                           std::string_view site);

  // Increment the per-URL purge generation, seeding from
  // purge_generation_previous_ when the key only survives there (rotation
  // monotonicity — issue #652 review), then rotate the maps on overflow.
  // REQUIRES purge_gen_mutex_ held by the caller.
  void BumpPurgeGenerationLocked(const std::string& gen_key);

  // html_mode markdown-*: the last full pass per URL (internal key), with the
  // origin hash it saw.  A repeat notification for the same hash within
  // kMarkdownPassRetrySecs skips the scan and analysis lookup (see the HTML
  // branch); a different hash, or a missing content-hash record, always
  // processes.  The window lets a copy that failed to build be retried.
  // markdown_pass_mutex_ is always taken alone.
  struct MarkdownPass {
    std::array<std::byte, 32> origin_hash;
    std::chrono::steady_clock::time_point at;
  };
  static constexpr int kMarkdownPassRetrySecs = 600;
  static constexpr size_t kMaxMarkdownPassEntries = 4096;
  bool MarkdownModePassIsCurrent(const CacheNotification& notification,
                                 const std::array<std::byte, 32>& origin_hash);
  void RecordMarkdownModePass(const CacheNotification& notification,
                              const std::array<std::byte, 32>& origin_hash);
  mutable std::mutex markdown_pass_mutex_;
  std::unordered_map<std::string, MarkdownPass> markdown_pass_last_;

  // Outcome of RecheckRecordedOriginal.
  enum class OriginRecheck : uint8_t {
    kUnchanged,      // Nothing to do; the caller's dedup skip stands.
    kRestamped,      // Same bytes re-recorded: freshness adopted, no rebuild.
    kPurgedRebuild,  // New bytes: derived copies purged; caller rebuilds.
  };
  // Compares the durable original a front end recorded with the origin state
  // the URL's variant set was built from.  See the call site in
  // HandleNotification.
  OriginRecheck RecheckRecordedOriginal(const CacheNotification& notification);

  // Clear the per-URL dedup entries, incomplete-retry tracking and
  // write-failure cooldowns — everything InvalidateUrl clears EXCEPT the
  // purge generations, which must stay bumped to fence in-flight writers
  // (ClearDedupAndCooldown, by contrast, erases the generations too).
  // Inputs must be pre-normalized.
  void ClearUrlProcessingState(const std::string& norm_url,
                               const std::string& norm_host,
                               const std::string& scheme,
                               std::string_view site);

  // Handle the kOriginRefreshedSentinel notification (issue #652): purge
  // the URL's STALE optimized variants while PRESERVING the identity
  // original (nginx re-records the fresh body at the identity id before
  // sending the sentinel), clear dedup, then rebuild the variant set
  // inline from the preserved fresh identity.  Rate-limited per URL.
  void HandleOriginRefreshed(const CacheNotification& notification);

  // Image incomplete-matrix retry tracking: allows re-processing when not all
  // variants were written, with a cap to prevent infinite retries.
  // Guarded by incomplete_retries_mutex_ (accessed from thread pool workers in
  // HandleNotification and from event loop in InvalidateUrl/ClearDedupAndCooldown).
  static constexpr int kMaxIncompleteRetries = 3;
  static constexpr size_t kMaxIncompleteRetryEntries = 1000;
  mutable std::mutex incomplete_retries_mutex_;
  std::unordered_map<std::string, int> image_incomplete_retries_;

  // Auto-heal: purge URLs after persistent cache read failures so the next
  // nginx MISS re-populates.  Per-URL attempt counter prevents infinite loops.
  // Guarded by auto_heal_mutex_ (accessed from thread pool workers).
  static constexpr int kMaxAutoHealsPerUrl = 3;
  static constexpr size_t kMaxAutoHealEntries = 1024;
  mutable std::mutex auto_heal_mutex_;
  std::unordered_map<std::string, int> auto_heal_counts_;

  // Try to auto-heal a URL that persistently fails cache reads.
  // Returns true if purge was performed, false if rate-limited.
  bool TryAutoHealCorruption(const std::string& url,
                             const std::string& hostname,
                             const char* error_type);

  // SRI integrity-pinned URLs (issue #656): "url|hostname|scheme" keys of
  // subresources referenced from HTML with an integrity attribute.  CSS/JS
  // notifications for these URLs are skipped (serve-original).  Pins are
  // conservative: a URL referenced WITH integrity anywhere stays pinned
  // even if other references omit it, and pins persist for the worker's
  // lifetime (a restart clears them; the next HTML scan re-registers).
  // Bounded; beyond the cap new pins are dropped (fail-open: those URLs
  // may still be optimized).
  static constexpr size_t kMaxIntegrityPinnedEntries = 100000;
  mutable std::mutex integrity_pinned_mutex_;
  std::unordered_set<std::string> integrity_pinned_urls_;

  // Text processing cooldown: prevents tight re-processing loops for
  // HTML, CSS, and JS.  Map stores expiry time (not start time), enabling
  // different durations:
  //  - Write failure (reader contention): 60s backoff
  //  - Revalidation (CSS/imports not yet cached): 3s backoff (convergence)
  //  - Tentative (in-flight guard): 60s (cleared on non-revalidation success)
  static constexpr int kWriteFailureCooldownSecs = 60;
  static constexpr int kRevalidationCooldownSecs = 3;
  // Hold after a pass that dropped the critical block and changed nothing else.
  static constexpr int kDroppedBlockCooldownSecs = 5;
  static constexpr size_t kMaxCooldownEntries = 256;
  mutable std::mutex write_cooldown_mutex_;
  std::unordered_map<std::string, CooldownEntry> text_cooldown_expiry_;

  // JS parse-error log dedup (issue #666): the JS-minify fail-safe serves the
  // original and marks the variant processed, but a cache refresh re-attempts
  // minification and re-logs the parse-error WARNING — a steady drip for a
  // permanently-unparseable file.  Suppress repeats: the first sighting of a
  // resource logs WARNING (a real parser gap is worth knowing about); later
  // sightings of the SAME resource are silenced for the worker's lifetime
  // (a restart re-arms the warning).  In release builds there is no DEBUG sink
  // (PS_DLOG_* compile out), so repeats are dropped rather than demoted.
  // Keyed on ComposeInternalKey(url, hostname, scheme) — same URL on a
  // different host/scheme is a distinct resource, matching every other dedup
  // set here.  Bounded; at capacity an unseen key fails OPEN (logs but is not
  // inserted), so memory never grows past the cap and a real gap is never
  // silently swallowed.  Guarded by js_parse_error_logged_mutex_ (accessed
  // from thread pool workers in HandleNotification).
  static constexpr size_t kMaxJsParseErrorLoggedEntries = 1024;
  mutable std::mutex js_parse_error_logged_mutex_;
  std::unordered_set<std::string> js_parse_error_logged_urls_;
  // Returns true the first time this resource hits a JS parse error (caller
  // should log WARNING), false on subsequent sightings (caller suppresses).
  bool ShouldLogJsParseError(const std::string& url,
                             const std::string& hostname,
                             const std::string& scheme, std::string_view site);

  // HTTP Management API server (null when api_port == 0).
  // Declaration order matters: contexts must outlive http_server_ because
  // route handler lambdas capture references to them. url_registry_ must
  // outlive cache_api_ctx_ (which holds a reference to it).
  std::unique_ptr<UrlRegistry> url_registry_;
  // Writes the registry's known sites to the known-sites file.  Declared
  // after url_registry_ so it stops (and flushes) first.
  std::unique_ptr<KnownSitesWriter> known_sites_writer_;
  // The known-sites file existed at start but could not be read.
  bool known_sites_unreadable_at_start_ = false;
  // Saves PATCH /v1/config changes off the event loop.  Declared before
  // http_server_ so it outlives the routes that hold a pointer to it.
  std::unique_ptr<ConfigPersister> config_persister_;
  std::unique_ptr<HttpServer> http_server_;
  std::unique_ptr<ApiContext> api_ctx_;
  std::unique_ptr<CacheApiContext> cache_api_ctx_;
  std::unique_ptr<CaptureContext> capture_ctx_;
  std::unique_ptr<WsManager> ws_manager_;
  std::unique_ptr<MessageHandler> tee_handler_;
  NullMessageHandler null_handler_;  // Delegate for cache log handler.
  std::unique_ptr<MessageHandler> cache_log_handler_;
  std::unique_ptr<MessageHandler> browser_log_handler_;
  std::unique_ptr<StaticFileCache> console_cache_;

  // Event loop lag gauge: measured by scheduling a 1s repeating timer
  // and checking actual fire time vs expected.
  std::unique_ptr<uv_timer_s> lag_timer_;
  static void OnLagTimer(uv_timer_t* timer);
  std::atomic<uint64_t> event_loop_lag_us_{0};
  // Issue #165: written at init (loop thread) and read/written in OnLagTimer
  // (loop thread).  Single-threaded in practice, but stored as an atomic to
  // make the formal access race-free.  steady_clock::time_point is not
  // trivially atomic, so we hold the raw tick count (steady_clock::rep) and
  // reconstruct the time_point at the read/write sites.
  std::atomic<std::chrono::steady_clock::rep> lag_scheduled_ticks_{0};

  // Start time for uptime computation.
  std::chrono::steady_clock::time_point start_time_;

  // Wall-clock epoch milliseconds at construction, for GET /v1/stats.
  // start_time_ (steady) measures durations but is meaningless across
  // processes; this answers "since when" for humans.
  int64_t started_at_ms_;

  // Web Bot Auth key-directory warmer: periodic timer that
  // queues one SSRF-guarded JWKS refresh cycle onto the work pool
  // (webbotauth_warmer.h) and publishes the warmed keys for nginx.  The work
  // struct carries a COPY of the state, so the work pool never touches
  // loop-thread members; the done-callback swaps the result back on the loop
  // thread.  This timer fetches ONLY the operator-configured key directories
  // — the daemon has no other periodic egress.
  std::unique_ptr<uv_timer_s> webbotauth_refresh_timer_;
  static void OnWebBotAuthRefresh(uv_timer_t* timer);
  static void DoWebBotAuthRefresh(uv_work_t* req);
  static void OnWebBotAuthRefreshDone(uv_work_t* req, int status);
  // Warm cadence: first warm 5s after start (let startup settle), then every
  // 15 minutes — with the default 1h positive TTL that allows several retries
  // per key lifetime before strict expiry lets entries lapse.  A cycle where
  // EVERY directory fetch fails (characteristic of boot ordering: a same-host
  // directory served by an nginx that starts after this worker) retries on
  // the short backoff schedule below before falling back to the normal
  // cadence, instead of leaving signed requests fail-closed to "unknown" for
  // a full 15-minute window (#876).
  static constexpr uint64_t kWebBotAuthInitialDelayMs = 5ULL * 1000;
  static constexpr uint64_t kWebBotAuthRefreshIntervalMs = 15ULL * 60 * 1000;
  static constexpr uint64_t kWebBotAuthTotalFailureBackoffMs[] = {30ULL * 1000,
                                                                  60ULL * 1000};
  // Index of the NEXT backoff delay to use; reset to 0 by any cycle that
  // fetches at least one directory successfully.  Loop thread only.
  size_t webbotauth_backoff_stage_ = 0;
  bool webbotauth_refresh_in_progress_ = false;
  // In-flight refresh work request, tracked so shutdown can uv_cancel it;
  // the done-callback frees both the uv_work_t and its payload.  Loop thread
  // only (queue site and BeginGracefulShutdown).
  uv_work_t* webbotauth_req_ = nullptr;
  // Warmed key store + per-directory kid sets.  Loop thread only.
  WebBotAuthWarmerState webbotauth_state_;

  // RSL-CAP key-directory warmer (experimental).  A SECOND,
  // independent instance of the same warmer machinery above, warming the
  // RSL-CAP issuer directories into realm "rsl" and publishing them to a
  // SEPARATE keys file (pagespeed-rslcap-keys.conf) — a file-level trust-domain
  // split on top of the store-key realm split, so the observe-only "wba" keys
  // and the enforcement "rsl" keys can never cross-contaminate.  Shares the
  // warm-cadence / backoff constants above.  Loop thread only.
  std::unique_ptr<uv_timer_s> rslcap_refresh_timer_;
  static void OnRslCapRefresh(uv_timer_t* timer);
  static void DoRslCapRefresh(uv_work_t* req);
  static void OnRslCapRefreshDone(uv_work_t* req, int status);
  size_t rslcap_backoff_stage_ = 0;
  bool rslcap_refresh_in_progress_ = false;
  uv_work_t* rslcap_req_ = nullptr;
  WebBotAuthWarmerState rslcap_state_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_WORKER_H_
