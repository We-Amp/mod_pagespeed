// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// Human-readable descriptions for worker metrics.
// Used by the Metrics page to show a Description column.
// ---------------------------------------------------------------------------

/** Map of dotted metric key → terse description. */
export const METRIC_DESCRIPTIONS: Record<string, string> = {
  // -- Notifications --
  'notifications.received': 'Total proxy notifications from nginx',
  'notifications.skipped_dedup': 'Notifications skipped (URL already processing)',
  'notifications.skipped_inflight':
    'Notifications skipped (same URL already in-flight in thread pool)',
  'notifications.rejected_version':
    'Notifications rejected (sender runs a different component version — a moving counter means a mismatched component set)',
  'notifications.rejected_malformed':
    'Notifications rejected (could not be parsed)',
  'notifications.rejected_sentinel':
    'Notifications rejected (reserved sentinel name)',

  // -- Variants --
  'variants.written': 'Total optimized variants written to cache',
  'variants.proactive': 'Proactively generated variants (viewport/density/save-data)',
  'variants.gzip': 'Gzip-compressed variants written',
  'variants.brotli': 'Brotli-compressed variants written',

  // -- Errors --
  'errors.total': 'Total processing errors',
  'errors.origin_misconfiguration':
    'Origin sent compressed response (add proxy_set_header Accept-Encoding "")',

  // -- Cache --
  'cache.entries': 'Number of entries in cache',
  'cache.size_bytes': 'Total cache size on disk',

  // -- By type --
  'by_type.html.count': 'HTML documents processed',
  'by_type.html.time_us': 'Total HTML processing time',
  'by_type.css.count': 'CSS stylesheets processed',
  'by_type.css.time_us': 'Total CSS processing time',
  'by_type.js.count': 'JavaScript files processed',
  'by_type.js.time_us': 'Total JS processing time',
  'by_type.image.count': 'Images processed',
  'by_type.image.time_us': 'Total image processing time',

  // -- By format --
  'by_format.webp': 'WebP variants generated',
  'by_format.avif': 'AVIF variants generated',
  'by_format.jpeg': 'JPEG variants generated',
  'by_format.png': 'PNG variants generated',
  'by_format.svg': 'SVG vectorizations generated',

  // -- SVG --
  'svg.candidates_evaluated': 'Images evaluated for SVG vectorization candidacy',
  'svg.candidates_rejected': 'SVG candidates rejected by content analysis score',
  'svg.vectorized': 'Images successfully vectorized to SVG',
  'svg.fidelity_rejected': 'SVG candidates rejected (fidelity too low)',
  'svg.timeout_exceeded': 'SVG vectorizations that exceeded the timeout',
  'svg.size_rejected': 'SVG variants rejected (larger than raster original)',
  'svg.path_count_rejected': 'SVG variants rejected (too many vector paths)',
  'svg.written': 'SVG variants written to cache',
  'svg.bytes_saved': 'Cumulative bytes saved by SVG vectorization',
  'svg.vectorize_time_us': 'Total SVG vectorization time (microseconds)',
  'svg.served': 'SVG variants served to clients',

  // -- Content analysis --
  'content_analysis.photo': 'Images classified as photographs',
  'content_analysis.screenshot': 'Images classified as screenshots',
  'content_analysis.illustration': 'Images classified as illustrations',
  'content_analysis.noisy': 'Images classified as noisy',
  'content_analysis.denoised': 'Deprecated; always 0',

  // -- SSIMULACRA2 --
  'ssimulacra2.checks': 'Quality verification checks performed',
  'ssimulacra2.reencodes': 'Re-encodes triggered by low quality score',
  'ssimulacra2.declines':
    'Variants refused because the shipped candidate scored below the acceptance floor or could not be measured — a decision, not an error',
  'ssimulacra2.tombstone_hits':
    'Variant recomputes skipped because the slot was already declined for this source',
  'ssimulacra2.avg_score_x100': 'Average quality score (x100)',

  // -- Learned quality --
  'learned_quality.predictions':
    'Number of images where the ML quality predictor was used',
  'learned_quality.fallbacks':
    'Number of images where the ML predictor returned an invalid result and fell back to heuristic quality',

  // -- HTML assembly --
  'html_assembly.complete': 'HTML pages fully assembled with optimized resources',
  'html_assembly.skipped': 'HTML pages skipped (no optimization needed)',
  'html_assembly.css_aborted': 'HTML assembly aborted (CSS fetch failed)',

  // -- Alternates --
  'alternates.writes': 'Cache alternate write operations',
  'alternates.write_failures': 'Cache alternate write failures',

  // -- Selector --
  selector_invocations: 'Cache variant selector invocations',

  // -- Cache reliability --
  cache_read_retries: 'Cache reads that required retry (cross-process propagation delay)',
  cache_read_failures: 'Cache reads where all retries were exhausted',
  cache_read_deferred_retries: 'Cache reads deferred and retried due to transient contention',
  cache_read_deferred_successes: 'Deferred cache reads that succeeded on retry',
  cache_auto_heals: 'Cache entries automatically repaired after detecting corruption',
  cache_auto_heal_exhausted: 'Auto-heal attempts that exhausted all retries without success',
  image_incomplete_matrices: 'Image variant matrices incomplete after processing',
  image_no_savings_skipped:
    'Image variants skipped because optimization produced no size reduction',
  image_unconverted_fallthrough:
    'Converted-format variants refused because there was nothing to convert — the optimizer returned the original bytes in the original format',

  // -- Content deduplication --
  'dedup.writes_skipped':
    'Cache writes skipped because identical content was already stored under a different alternate ID',
  'dedup.content_hash_hits': 'Content hash matches found during deduplication checks',
  'dedup.content_hash_stale': 'Stale content hash entries invalidated during dedup checks',

  // -- Quality baselining --
  'quality_baselining.capped_jpeg': 'JPEG quality capped to source quality level',
  'quality_baselining.skip_reencode': 'JPEG re-encodes skipped (source already optimal)',

  // -- Thread pool --
  'thread_pool.inflight': 'Currently active worker threads',
  'thread_pool.size': 'Thread pool capacity',

  // -- Connections --
  'connections.active': 'Active IPC connections',
  'connections.max': 'Maximum IPC connections',

  // -- Browser analysis --
  'browser.enabled': 'Browser analysis feature enabled',
  'browser.chrome_running': 'Chrome process is running',
  'browser.profiles_generated': 'Browser profiles generated',
  'browser.profiles_used': 'Browser profiles applied to HTML',
  'browser.analysis_errors': 'Browser analysis errors',
  'browser.chrome_crashes': 'Chrome process crashes',
  'browser.queue_depth': 'Pending browser analysis jobs',
  'browser.scripts_analyzed': 'Scripts evaluated by browser analysis',
  'browser.scripts_deferrable': 'Scripts identified as safe to defer',

  // -- Policy --
  'policy.computed': 'Optimization policies computed',
  'policy.async_css_enabled': 'Times async CSS was enabled by policy',
  'policy.script_deferral_enabled': 'Times script deferral was enabled by policy',

  // -- Serve-time bandwidth savings --
  'serve_savings.html.original_bytes':
    'Total original HTML bytes served from cache (before optimization)',
  'serve_savings.html.optimized_bytes': 'Total optimized HTML bytes served from cache',
  'serve_savings.html.hits': 'Number of optimized HTML responses served from cache',
  'serve_savings.css.original_bytes':
    'Total original CSS bytes served from cache (before optimization)',
  'serve_savings.css.optimized_bytes': 'Total optimized CSS bytes served from cache',
  'serve_savings.css.hits': 'Number of optimized CSS responses served from cache',
  'serve_savings.js.original_bytes':
    'Total original JS bytes served from cache (before optimization)',
  'serve_savings.js.optimized_bytes': 'Total optimized JS bytes served from cache',
  'serve_savings.js.hits': 'Number of optimized JS responses served from cache',
  'serve_savings.image.original_bytes':
    'Total original image bytes served from cache (before optimization)',
  'serve_savings.image.optimized_bytes': 'Total optimized image bytes served from cache',
  'serve_savings.image.hits': 'Number of optimized image responses served from cache',
};

/** Category display names for metric group headers. */
export const METRIC_CATEGORIES: Record<string, string> = {
  notifications: 'Notifications',
  variants: 'Variants',
  errors: 'Errors',
  cache: 'Cache',
  by_type: 'Processing by Type',
  by_format: 'Output Formats',
  svg: 'SVG Vectorization',
  content_analysis: 'Content Analysis',
  ssimulacra2: 'Quality Verification',
  learned_quality: 'Learned Quality Prediction',
  html_assembly: 'HTML Assembly',
  alternates: 'Cache Alternates',
  selector_invocations: 'Cache',
  cache_read_retries: 'Cache Reliability',
  cache_read_failures: 'Cache Reliability',
  cache_auto_heals: 'Cache Reliability',
  cache_auto_heal_exhausted: 'Cache Reliability',
  cache_read_deferred_retries: 'Cache Reliability',
  cache_read_deferred_successes: 'Cache Reliability',
  image_incomplete_matrices: 'Image Processing',
  image_no_savings_skipped: 'Image Processing',
  image_unconverted_fallthrough: 'Image Processing',
  quality_baselining: 'Quality Baselining',
  thread_pool: 'Thread Pool',
  connections: 'Connections',
  browser: 'Browser Analysis',
  policy: 'Policy',
  serve_savings: 'Serve-Time Bandwidth Savings',
  dedup: 'Content Deduplication',
};

/**
 * Extract the category prefix from a dotted metric key.
 * e.g. "notifications.received" → "notifications"
 *      "errors" → "errors"
 */
export function metricCategory(key: string): string {
  // Top-level keys without dots are their own category
  const dot = key.indexOf('.');
  return dot === -1 ? key : key.substring(0, dot);
}
