// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { Leaf, MockRequest, Reply, Responder } from "./mock-admin";

export const ok = (body: unknown): Reply => ({ status: 200, body });

const GLOBAL_VARS = {
  css_filter_total_bytes_saved: 499345,
  css_filter_total_original_bytes: 1063346,
  javascript_total_bytes_saved: 12087254,
  javascript_total_original_bytes: 25498944,
  image_rewrite_total_bytes_saved: 303593,
  image_rewrite_total_original_bytes: 1116562,
  ipro_daemon_served: 3122,
  ipro_daemon_fallthrough: 8884,
  num_flushes: 3671,
  num_resource_fetch_successes: 108,
  num_resource_fetch_failures: 0,
  num_rewrites_executed: 10428,
  total_rewrite_count: 3671,
};

export const STATS_GLOBAL = ok({ variables: GLOBAL_VARS, maxlength: 60, timestamp_ms: 1_790_000_000_000 });

const VHOST_VARS = {
  css_filter_total_bytes_saved: 712,
  css_filter_total_original_bytes: 146663,
  javascript_total_bytes_saved: 285283,
  javascript_total_original_bytes: 1359400,
  image_rewrite_total_bytes_saved: 1839,
  image_rewrite_total_original_bytes: 19898,
  ipro_daemon_served: 7,
  ipro_daemon_fallthrough: 1644,
  num_flushes: 457,
  num_resource_fetch_successes: 21,
  num_resource_fetch_failures: 0,
};

/** Per-vhost statistics as a real per-vhost console serves them (fewer counters than global). */
export const STATS_VHOST = ok({ variables: VHOST_VARS, maxlength: 60, timestamp_ms: 1_790_000_000_000 });

/** An older module build without the in-place optimizer counters. */
export const STATS_OLD_BUILD = ok({
  variables: Object.fromEntries(Object.entries(VHOST_VARS).filter(([k]) => !k.startsWith("ipro_daemon_"))),
  maxlength: 60,
});

/** A module-only server: real module traffic, but nothing routed through an optimizer. */
export const STATS_MODULE_ONLY = ok({
  variables: { ...GLOBAL_VARS, ipro_daemon_served: 0, ipro_daemon_fallthrough: 0 },
  maxlength: 60,
  timestamp_ms: 1_790_000_000_000,
});

export const STATS_EMPTY = ok({
  variables: Object.fromEntries(Object.keys(GLOBAL_VARS).map((k) => [k, 0])),
  maxlength: 60,
  timestamp_ms: 1_790_000_000_000,
});

export const statsWithFetchFailures = (n: number): Reply =>
  ok({ variables: { ...GLOBAL_VARS, num_resource_fetch_failures: n }, maxlength: 60 });

/** A message_history page with one message of each common severity. */
export const MESSAGES = ok({
  scope: "process",
  next: 3,
  messages: [
    { severity: "info", message: "[Sat, 26 Sep 2026 10:02:31 GMT] [Info] [531] CycloneCache enabled at /var/cache/mod_pagespeed/" },
    { severity: "warning", message: "[Sat, 26 Sep 2026 10:03:00 GMT] [Warning] [531] Slow origin response for https://www.example.test/b.js" },
    { severity: "error", message: "[Sat, 26 Sep 2026 10:04:00 GMT] [Error] [531] Fetch of https://www.example.test/a.css failed" },
  ],
});

export const CONFIG_GLOBAL = ok({ config: "", effective_config: "", scope: "global", host: "" });
export const CONFIG_VHOST = ok({ config: "", effective_config: "", scope: "vhost", host: "www.example.test:80" });

/** A per-vhost console whose server context has no name (the main server on a stock install). */
export const CONFIG_VHOST_NAMELESS = ok({ config: "", effective_config: "", scope: "vhost", host: ":0" });

/** A vhost console whose config leaves carry text, so the Configuration
 *  tabs and their tabpanel content render (the stock fixtures ship
 *  empty strings and the page then shows its empty state). */
export const CONFIG_VHOST_WITH_TEXT = ok({
  config:
    "ModPagespeed on\n" +
    "ModPagespeedRewriteLevel CoreFilters\n" +
    "ModPagespeedEnableFilters combine_css,rewrite_images\n" +
    "ModPagespeedFileCachePath /var/cache/mod_pagespeed/",
  effective_config:
    "ModPagespeed on\n" +
    "ModPagespeedRewriteLevel CoreFilters\n" +
    "ModPagespeedEnableFilters combine_css,rewrite_images\n" +
    "ModPagespeedFileCachePath /var/cache/mod_pagespeed/\n" +
    "ModPagespeedStatistics on",
  scope: "vhost",
  host: "www.example.test:80",
});

export const HEALTH_OK = ok({
  status: "ok",
  ready: true,
  version: "2.0.41",
  git_commit: "52344ff",
  uptime_seconds: 98626,
  inflight: 0,
  connections: { active: 1, max: 32 },
  checks: { cache_configured: { pass: true }, cache_open: { pass: true } },
  browser: { enabled: true, chrome_running: true },
});

/** An optimizer older than the console's minimum version (served with a working stats endpoint). */
export const HEALTH_BELOW_FLOOR = ok({ status: "ok", ready: true, version: "2.0.3", uptime_seconds: 60 });

/** Every worker thread busy: the optimizer reports ready:false. */
export const HEALTH_BUSY = ok({ status: "ok", ready: false, version: "2.0.41", uptime_seconds: 600 });

/** A malformed health body: version is not a string. The daemon is untrusted. */
export const HEALTH_BAD_VERSION = ok({ status: "ok", ready: true, version: { major: 2 }, uptime_seconds: 60 });

export const HEALTH_CHECK_FAILING = ok({
  status: "ok",
  ready: true,
  version: "2.0.41",
  checks: { cache_configured: { pass: true }, cache_open: { pass: false } },
});

export interface DaemonNumbers {
  errorsTotal?: number;
  originMisconfiguration?: number;
  writeFailures?: number;
}

export const daemonStats = (n: DaemonNumbers = {}): Reply =>
  ok({
    errors: { total: n.errorsTotal ?? 0, origin_misconfiguration: n.originMisconfiguration ?? 0, text_minify_parse_failures: 0 },
    alternates: { write_failures: n.writeFailures ?? 0, writes: 67 },
    thread_pool: { inflight: 0, size: 2 },
    connections: { active: 1, max: 32 },
    cache: { entries: 126, size_bytes: 97084216 },
    serve_savings: {
      css: { hits: 197, optimized_bytes: 21447548, original_bytes: 21472396 },
      html: { hits: 0, optimized_bytes: 0, original_bytes: 0 },
      image: { hits: 45, optimized_bytes: 258379, original_bytes: 922228 },
      js: { hits: 0, optimized_bytes: 0, original_bytes: 0 },
    },
  });

export const DAEMON_STATS = daemonStats();

export const DAEMON_STATS_EMPTY = ok({
  errors: { total: 0, origin_misconfiguration: 0 },
  alternates: { write_failures: 0, writes: 0 },
  thread_pool: { inflight: 0, size: 2 },
  connections: { active: 0, max: 32 },
  serve_savings: {
    css: { hits: 0, optimized_bytes: 0, original_bytes: 0 },
    image: { hits: 0, optimized_bytes: 0, original_bytes: 0 },
  },
});

/** A global stats reply whose CSS bytes-saved counter the caller sets. */
export const statsWithSavings = (savedBytes: number): Reply =>
  ok({ variables: { ...GLOBAL_VARS, css_filter_total_bytes_saved: savedBytes }, timestamp_ms: 1_790_000_000_000 });

/** The optimizer reports serving MORE bytes than the original (incompressible content). */
export const DAEMON_STATS_REGRESSION: Reply = ok({
  ...(DAEMON_STATS.body as Record<string, unknown>),
  serve_savings: { css: { hits: 10, optimized_bytes: 12000, original_bytes: 10000 } },
});

/** serve_savings with the wrong types: strings, arrays, negatives. */
export const DAEMON_STATS_MALFORMED: Reply = ok({
  ...(DAEMON_STATS.body as Record<string, unknown>),
  serve_savings: {
    css: { hits: "lots", optimized_bytes: [], original_bytes: -50 },
    image: "junk",
  },
});

export const UNREACHABLE: Reply = { status: 502, body: { error: "daemon_unreachable" } };
export const NOT_CONFIGURED: Reply = { status: 503, body: { error: "daemon_not_configured" } };
export const UNSUPPORTED: Reply = { status: 501, body: { error: "endpoint_unsupported_by_daemon" } };
export const BUSY: Reply = { status: 429, body: { error: "a request for this daemon endpoint is already in flight" } };
export const MODULE_FAILURE: Reply = { status: 500, body: { error: "statistics unavailable" } };

/** The cache leaf as a stock install answers it (purge off). */
export const CACHE_STRUCTURE = ok({
  caches: [
    { name: "HTTP Cache", summary: "HTTPCache(Stats(prefix=file_cache,cache=CycloneCache))" },
    {
      name: "Metadata Cache",
      summary:
        "Compressed(WriteThroughCache(l1=Stats(prefix=shm_cache,cache=SharedMemCache<64>),l2=Stats(prefix=file_cache_small,cache=CycloneCache(small_tier))))",
    },
    {
      name: "Property Cache",
      summary:
        "beacon_cohort:Stats(prefix=pcache-cohorts-beacon_cohort,cache=Compressed(WriteThroughCache(l1=Stats(prefix=shm_cache,cache=SharedMemCache<64>),l2=Stats(prefix=file_cache_small,cache=CycloneCache(small_tier)))))\n" +
        "dom:Stats(prefix=pcache-cohorts-dom,cache=Compressed(WriteThroughCache(l1=Stats(prefix=shm_cache,cache=SharedMemCache<64>),l2=Stats(prefix=file_cache_small,cache=CycloneCache(small_tier)))))",
    },
    { name: "FileSystem Metadata Cache", summary: "none" },
  ],
  backend_stats: "",
  purge_enabled: false,
});

/** The same caches on a server with purging turned on. */
export const CACHE_STRUCTURE_PURGE_ON = ok({ ...(CACHE_STRUCTURE.body as Record<string, unknown>), purge_enabled: true });

/** A graphs reply: three samples one minute apart ending at the request, one series per name. */
export const graphsReply = (names: string[]): Reply => {
  const now = Date.now();
  return ok({
    timestamps: [now - 120_000, now - 60_000, now],
    variables: Object.fromEntries(names.map((name, i) => [name, [i, i + 2, i + 5]])),
  });
};

/** Two populated histograms in the leaf's JSON shape. */
export const HISTOGRAMS = ok({
  histograms: [
    {
      name: "Html Time us", count: 4, avg: 1500, stddev: 200, min: 1200, median: 1500, max: 1800,
      p90: 1750, p95: 1780, p99: 1800,
      buckets: [{ start: 1000, limit: 1500, count: 2 }, { start: 1500, limit: 2000, count: 2 }],
    },
    {
      name: "Rewrite Latency ms", count: 3, avg: 12, stddev: null, min: 10, median: 12, max: 14,
      p90: null, p95: null, p99: null,
      buckets: [{ start: 10, limit: 15, count: 3 }],
    },
  ],
});

/** The entry every detail fixture describes: https://www.example.test/hero.png. */
export const HERO_KEY = { url: "/hero.png", host: "www.example.test", scheme: "https" } as const;

/** Page 1/2 of a two-page index (the daemon's shape: path urls), branching on `offset`. */
export const URLS_TWO_PAGES: Responder = (_call, request) => {
  const offset = Number(request.url.searchParams.get("offset") ?? "0");
  return offset === 0
    ? ok({
        urls: [
          { url: "/", hostname: "www.example.test", scheme: "https", alternate_count: 3, cache_key: "https://www.example.test/" },
          { url: "/hero.png", hostname: "www.example.test", scheme: "https", alternate_count: 2, cache_key: "https://www.example.test/hero.png" },
        ],
        offset: 0, limit: 50, next_offset: 2, has_more: true, total: 3,
      })
    : ok({
        urls: [
          { url: "/app.js?v=2", hostname: "cdn.example.test", scheme: "http", alternate_count: 0, cache_key: "http://cdn.example.test/app.js?v=2" },
        ],
        offset: 2, limit: 50, next_offset: 3, has_more: false, total: 3,
      });
};

export const URLS_EMPTY = ok({ urls: [], offset: 0, limit: 50, next_offset: 0, has_more: false, total: 0 });

/** A urls reply with garbage where numbers and rows belong: the page must not show NaN. */
export const URLS_MALFORMED = ok({
  urls: [null, 42, {}, { url: "/ok.png", hostname: "ok.test", scheme: "https", alternate_count: "many" }],
  offset: 0,
  limit: 50,
  has_more: "yes",
  total: -3,
});

/** The per-vhost answer to every cache leaf: the module gates them to the whole-server console. */
export const WHOLE_SERVER_ONLY: Reply = { status: 403, body: { error: "whole_server_console_only" } };

const heroMask = (format: string, viewport: string, density: string, saveData: boolean) => ({
  raw: 0, format, viewport, density, save_data: saveData, encoding: "identity",
});

/**
 * The cached variants of https://www.example.test/hero.png in the daemon's
 * shape (cache_handlers.cc:141-316): the original, a WebP and an AVIF
 * variant, and one sentinel. `content_type` is the CLASS "image"; the
 * origin's MIME is `origin_content_type`; `density` is a string.
 */
export const ALTERNATES_HERO = ok({
  url: "/hero.png",
  hostname: "www.example.test",
  scheme: "https",
  cache_key: "https://www.example.test/hero.png",
  count: 4,
  chain_length: 4,
  alternates: [
    {
      alternate_id: 0, size: 45210, hit_count: 128, is_sentinel: false,
      last_access: 1_790_000_000_000, mask: heroMask("original", "desktop", "1x", false),
      format: "original", viewport: "desktop", density: "1x", save_data: false, encoding: "identity",
      content_type: "image", origin_content_type: "image/png",
      flags: 0, needs_revalidation: false, cache_inserted_at: 1_789_999_000,
      origin_max_age: 3600, origin_s_maxage: 0, origin_cc_flags: 0, version: 1,
      original_size: 45210,
    },
    {
      alternate_id: 1, size: 30984, hit_count: 1042, is_sentinel: false,
      last_access: 1_790_000_100_000, mask: heroMask("webp", "desktop", "1x", false),
      format: "webp", viewport: "desktop", density: "1x", save_data: false, encoding: "identity",
      content_type: "image", origin_content_type: "image/png",
      flags: 0, needs_revalidation: false, cache_inserted_at: 1_789_999_100,
      origin_max_age: 3600, origin_s_maxage: 0, origin_cc_flags: 0, version: 1,
      ssimulacra2_score: 92.5, content_class: "photo", original_size: 45210,
    },
    {
      alternate_id: 2, size: 28120, hit_count: 88, is_sentinel: false,
      last_access: 1_790_000_200_000, mask: heroMask("avif", "mobile", "2x+", true),
      format: "avif", viewport: "mobile", density: "2x+", save_data: true, encoding: "identity",
      content_type: "image", origin_content_type: "image/png",
      flags: 0, needs_revalidation: false, cache_inserted_at: 1_789_999_200,
      origin_max_age: 3600, origin_s_maxage: 0, origin_cc_flags: 0, version: 1,
      ssimulacra2_score: 87.1, content_class: "photo", original_size: 45210,
    },
    {
      alternate_id: 60, size: 0, hit_count: 0, is_sentinel: true,
      sentinel_name: "content_hash", last_access: 1_790_000_300_000,
    },
  ],
});

/**
 * The shape the optimizer really sends: the cached original is the
 * "original_content" sentinel, and no variant has format "original".
 */
export const ALTERNATES_SENTINEL_ORIGINAL = ok({
  url: "/hero.png",
  hostname: "www.example.test",
  scheme: "https",
  cache_key: "https://www.example.test/hero.png",
  count: 2,
  chain_length: 2,
  alternates: [
    {
      alternate_id: 12, size: 45210, hit_count: 18, is_sentinel: true,
      sentinel_name: "original_content", last_access: 1_790_000_000_000,
    },
    {
      alternate_id: 9, size: 30984, hit_count: 13, is_sentinel: false,
      last_access: 1_790_000_100_000, mask: heroMask("webp", "desktop", "1x", false),
      format: "webp", viewport: "desktop", density: "1x", save_data: false, encoding: "identity",
      content_type: "image", origin_content_type: "image/png",
      flags: 0, needs_revalidation: false, cache_inserted_at: 1_789_999_100,
      origin_max_age: 3600, origin_s_maxage: 0, origin_cc_flags: 0, version: 1,
      ssimulacra2_score: 92.5, content_class: "photo", original_size: 45210,
    },
  ],
});

/** The module's answer for an entry the optimizer holds nothing for. */
export const ALTERNATES_NOT_IN_INDEX: Reply = { status: 404, body: { error: "not_in_index" } };

/** Alternates garbage: a non-array list. */
export const ALTERNATES_MALFORMED = ok({ url: "/hero.png", alternates: "no", count: "many" });

/** Two 1x1 PNGs (black, transparent) so diff previews can differ. */
export const PNG_1PX_BLACK = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
  "base64",
);
export const PNG_1PX_TRANSPARENT = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==",
  "base64",
);

const pngReply = (bytes: Buffer, contentType = "image/png"): Reply => ({
  status: 200,
  contentType,
  bytes,
  body: null,
});

/** The content leaf's bytes: alternate 1 (webp) transparent, every other id black. */
export const CONTENT_HERO: Responder = (_call, request) =>
  request.url.searchParams.get("alternate_id") === "1"
    ? pngReply(PNG_1PX_TRANSPARENT, "image/webp")
    : pngReply(PNG_1PX_BLACK);

const BUSY_REPLY: Reply = { status: 429, body: { error: "a request for this daemon endpoint is already in flight" } };

/**
 * A content leaf that is busy for the first GET of every variant (and for
 * the HEAD probe that follows it), then serves that variant's bytes: the
 * preview queue must retry each one. Counted per alternate_id, so parallel
 * previews do not steal each other's busy answer. A factory: every test
 * gets fresh counters.
 */
export function contentBusyOnce(): Responder {
  const gets = new Map<string, number>();
  return (_call: number, request: MockRequest): Reply => {
    const id = request.url.searchParams.get("alternate_id") ?? "";
    const seen = gets.get(id) ?? 0;
    if (request.method === "HEAD") return seen <= 1 ? BUSY_REPLY : pngReply(PNG_1PX_BLACK);
    gets.set(id, seen + 1);
    return seen === 0 ? BUSY_REPLY : pngReply(PNG_1PX_BLACK);
  };
}

/** A content leaf that is always busy. */
export const CONTENT_ALWAYS_BUSY: Reply = BUSY_REPLY;

/** A healthy reply for every leaf: a per-vhost console with an optimizer. */
export function allReplies(): Partial<Record<Leaf, Responder>> {
  return {
    config: CONFIG_VHOST,
    stats_json: STATS_VHOST,
    message_history: MESSAGES,
    histograms: HISTOGRAMS,
    cache: CACHE_STRUCTURE,
    graphs: graphsReply(["num_flushes", "css_filter_total_bytes_saved"]),
    "v1/daemon/health": HEALTH_OK,
    "v1/daemon/stats": DAEMON_STATS,
    "v1/daemon/cooldowns": ok({ cooldowns: [] }),
    "v1/daemon/cache/urls": WHOLE_SERVER_ONLY,
    "v1/daemon/cache/alternates": WHOLE_SERVER_ONLY,
    "v1/daemon/cache/content": WHOLE_SERVER_ONLY,
    "v1/daemon/logs": WHOLE_SERVER_ONLY,
  };
}

// ── Optimizer log ring (v1/daemon/logs) ─────────────────────────────

/**
 * A real GET /v1/logs page, byte for byte: a copy of the optimizer's
 * committed golden capture (pagespeed-optimizer repository,
 * test/src/worker/testdata/logs_page.golden.json, without its trailing
 * newline), which the optimizer's own test compares against the bytes it
 * serves. If that file changes, copy it here again.
 */
export const LOGS_PAGE_BYTES =
  '{"entries":[{"level":"info","message":"cache flush complete","module":"worker","seq":0,"source":"worker","timestamp":1759230000000,"type":"log"},{"level":"warning","message":"origin fetch slow","module":"cache","seq":1,"source":"cache","timestamp":1759230000500,"type":"log"}],"gap":false,"more":false,"newest_seq":1,"next_since":1,"oldest_seq":0,"shed_total":0,"stream_id":"9f2c4e1a7b3d5c80"}';

/** The golden page as a reply (the mock re-serializes it to the same bytes). */
export const LOGS_PAGE: Reply = ok(JSON.parse(LOGS_PAGE_BYTES));

/**
 * A real GET /v1/stats response, byte for byte: a copy of the optimizer's
 * committed golden capture (pagespeed-optimizer repository,
 * test/src/worker/testdata/stats_page.golden.json, without its trailing
 * newline), which the optimizer's own test compares against the bytes it
 * serves. If that file changes, copy it here again.
 */
export const OPT_STATS_PAGE_BYTES =
  '{"agent_markdown":{"preserved":0,"purged_on_change":0,"rebuild_forced":0},"alternates":{"write_failures":0,"writes":0,"writes_fenced":0},"browser_sandbox":"disabled","by_format":{"avif":0,"jpeg":0,"png":0,"webp":0},"by_type":{"css":{"count":0,"time_us":0},"html":{"count":0,"time_us":0},"image":{"count":0,"time_us":0},"js":{"count":0,"time_us":0}},"cache":{"entries":450,"size_bytes":314572800},"cache_auto_heal_exhausted":0,"cache_auto_heals":0,"cache_read_deferred_retries":0,"cache_read_deferred_successes":0,"cache_read_failures":0,"cache_read_retries":0,"connections":{"active":5,"max":128},"content_analysis":{"denoised":0,"illustration":0,"noisy":0,"photo":0,"screenshot":0},"dedup":{"content_hash_hits":0,"content_hash_stale":0,"writes_skipped":0},"errors":{"origin_misconfiguration":0,"text_minify_parse_failures":0,"total":0},"html_assembly":{"complete":0,"critical_css_skipped_high_coverage":0,"css_aborted":0,"skipped":0},"image_incomplete_matrices":0,"image_no_savings_skipped":0,"image_unconverted_fallthrough":0,"learned_quality":{"fallbacks":0,"predictions":0},"notifications":{"accepted_non_default_option_context":0,"dedup_heal_rate_limited":0,"dedup_healed":0,"received":0,"rejected_malformed":0,"rejected_option_context":0,"rejected_sentinel":0,"rejected_version":0,"skipped_dedup":0,"skipped_inflight":0},"origin_refresh":{"deferred":0,"purges":0,"rate_limited":0,"rebuild_refused":0,"unchanged":0},"policy":{"async_css_enabled":0,"async_css_record_dropped_empty_derivation":0,"async_css_suppressed_low_coverage":0,"async_css_suppressed_unvalidated":0,"computed":0,"script_deferral_enabled":0},"quality_baselining":{"capped_jpeg":0,"skip_reencode":0},"read_borrow_wrap_discards":0,"selector_invocations":0,"serve_classes":{"class_unrecognized":0,"flags_unrecognized":0,"notify_suppressed":0,"optimized":5,"original_cold":0,"original_declined":0,"original_pending":0,"original_skew":0},"serve_savings":{"css":{"by_encoding":{"br":{"bytes":0,"hits":0},"gzip":{"bytes":0,"hits":0},"identity":{"bytes":200000,"hits":3}},"hits":3,"optimized_bytes":200000,"original_bytes":200000},"html":{"by_encoding":{"br":{"bytes":0,"hits":0},"gzip":{"bytes":0,"hits":0},"identity":{"bytes":0,"hits":0}},"hits":0,"optimized_bytes":0,"original_bytes":0},"image":{"by_encoding":{"br":{"bytes":0,"hits":0},"gzip":{"bytes":0,"hits":0},"identity":{"bytes":100000,"hits":2}},"hits":2,"optimized_bytes":100000,"original_bytes":500000},"js":{"by_encoding":{"br":{"bytes":0,"hits":0},"gzip":{"bytes":0,"hits":0},"identity":{"bytes":0,"hits":0}},"hits":0,"optimized_bytes":0,"original_bytes":0}},"serve_savings_by_host":{"hosts":[{"hits":3,"host":"www.example.com","optimized_bytes":200000,"original_bytes":200000},{"hits":2,"host":"static.example.com","optimized_bytes":100000,"original_bytes":500000}],"limit":32,"other":{"hits":0,"optimized_bytes":0,"original_bytes":0}},"source_reads_from_durable_original":0,"ssimulacra2":{"avg_score_x100":0,"checks":0,"declines":0,"reencodes":0,"tombstone_hits":0},"stale_serves":{"stale_if_error":0,"swr_coalesced":0},"started_at_ms":1759230000000,"svg":{"bytes_saved":0,"candidates_evaluated":0,"candidates_rejected":0,"fidelity_rejected":0,"path_count_rejected":0,"served":0,"size_rejected":0,"timeout_exceeded":0,"vectorize_time_us":0,"vectorized":0,"written":0},"syscall_filter":"unknown","thread_pool":{"inflight":2,"size":8},"uptime_seconds":3600,"variants":{"brotli":0,"gzip":0,"proactive":0,"written":0},"verdicts":{"css":{"already_optimal":{"bytes":110554,"count":1}},"image":{"already_optimal":{"bytes":120000,"count":2}},"js":{"already_optimal":{"bytes":0,"count":0}}},"zerocopy":{"copy_then_verify_discards":0,"proactive_copyouts":0,"torn_aborts":0}}';

/** The golden page as a reply (the mock re-serializes it to the same bytes). */
export const OPT_STATS_PAGE: Reply = ok(JSON.parse(OPT_STATS_PAGE_BYTES));

/**
 * The previous golden capture, from an optimizer that predates serve
 * savings per host (no serve_savings_by_host): kept verbatim so the
 * console's degrade is tested against what such an optimizer really sends.
 */
export const OPT_STATS_PAGE_PRE_HOSTS_BYTES =
  '{"agent_markdown":{"preserved":0,"purged_on_change":0,"rebuild_forced":0},"alternates":{"write_failures":0,"writes":0,"writes_fenced":0},"browser_sandbox":"disabled","by_format":{"avif":0,"jpeg":0,"png":0,"webp":0},"by_type":{"css":{"count":0,"time_us":0},"html":{"count":0,"time_us":0},"image":{"count":0,"time_us":0},"js":{"count":0,"time_us":0}},"cache":{"entries":450,"size_bytes":314572800},"cache_auto_heal_exhausted":0,"cache_auto_heals":0,"cache_read_deferred_retries":0,"cache_read_deferred_successes":0,"cache_read_failures":0,"cache_read_retries":0,"connections":{"active":5,"max":128},"content_analysis":{"denoised":0,"illustration":0,"noisy":0,"photo":0,"screenshot":0},"dedup":{"content_hash_hits":0,"content_hash_stale":0,"writes_skipped":0},"errors":{"origin_misconfiguration":0,"text_minify_parse_failures":0,"total":0},"html_assembly":{"complete":0,"critical_css_skipped_high_coverage":0,"css_aborted":0,"skipped":0},"image_incomplete_matrices":0,"image_no_savings_skipped":0,"image_unconverted_fallthrough":0,"learned_quality":{"fallbacks":0,"predictions":0},"notifications":{"accepted_non_default_option_context":0,"dedup_heal_rate_limited":0,"dedup_healed":0,"received":0,"rejected_malformed":0,"rejected_option_context":0,"rejected_sentinel":0,"rejected_version":0,"skipped_dedup":0,"skipped_inflight":0},"origin_refresh":{"deferred":0,"purges":0,"rate_limited":0,"rebuild_refused":0,"unchanged":0},"policy":{"async_css_enabled":0,"async_css_record_dropped_empty_derivation":0,"async_css_suppressed_low_coverage":0,"async_css_suppressed_unvalidated":0,"computed":0,"script_deferral_enabled":0},"quality_baselining":{"capped_jpeg":0,"skip_reencode":0},"read_borrow_wrap_discards":0,"selector_invocations":0,"serve_classes":{"class_unrecognized":0,"flags_unrecognized":0,"notify_suppressed":0,"optimized":5,"original_cold":0,"original_declined":0,"original_pending":0,"original_skew":0},"serve_savings":{"css":{"by_encoding":{"br":{"bytes":0,"hits":0},"gzip":{"bytes":0,"hits":0},"identity":{"bytes":200000,"hits":3}},"hits":3,"optimized_bytes":200000,"original_bytes":200000},"html":{"by_encoding":{"br":{"bytes":0,"hits":0},"gzip":{"bytes":0,"hits":0},"identity":{"bytes":0,"hits":0}},"hits":0,"optimized_bytes":0,"original_bytes":0},"image":{"by_encoding":{"br":{"bytes":0,"hits":0},"gzip":{"bytes":0,"hits":0},"identity":{"bytes":100000,"hits":2}},"hits":2,"optimized_bytes":100000,"original_bytes":500000},"js":{"by_encoding":{"br":{"bytes":0,"hits":0},"gzip":{"bytes":0,"hits":0},"identity":{"bytes":0,"hits":0}},"hits":0,"optimized_bytes":0,"original_bytes":0}},"source_reads_from_durable_original":0,"ssimulacra2":{"avg_score_x100":0,"checks":0,"declines":0,"reencodes":0,"tombstone_hits":0},"stale_serves":{"stale_if_error":0,"swr_coalesced":0},"started_at_ms":1759230000000,"svg":{"bytes_saved":0,"candidates_evaluated":0,"candidates_rejected":0,"fidelity_rejected":0,"path_count_rejected":0,"served":0,"size_rejected":0,"timeout_exceeded":0,"vectorize_time_us":0,"vectorized":0,"written":0},"syscall_filter":"unknown","thread_pool":{"inflight":2,"size":8},"uptime_seconds":3600,"variants":{"brotli":0,"gzip":0,"proactive":0,"written":0},"verdicts":{"css":{"already_optimal":{"bytes":110554,"count":1}},"image":{"already_optimal":{"bytes":120000,"count":2}},"js":{"already_optimal":{"bytes":0,"count":0}}},"zerocopy":{"copy_then_verify_discards":0,"proactive_copyouts":0,"torn_aborts":0}}';

export const OPT_STATS_PAGE_PRE_HOSTS: Reply = ok(JSON.parse(OPT_STATS_PAGE_PRE_HOSTS_BYTES));

/** Per-host rows an optimizer must never be trusted with: markup, case, junk, a repeat, negatives. */
export const OPT_STATS_HOSTS_HOSTILE: Reply = ok({
  ...JSON.parse(OPT_STATS_PAGE_BYTES),
  serve_savings_by_host: {
    hosts: [
      { host: "<img src=x onerror=alert(1)>", hits: 9, original_bytes: 900, optimized_bytes: 100 },
      { host: "WWW.Example.COM", hits: 2, original_bytes: 1000, optimized_bytes: 500 },
      { host: "www.example.com", hits: 5, original_bytes: 1000, optimized_bytes: 500 },
      { host: "neg.example", hits: -1, original_bytes: 100, optimized_bytes: 50 },
      { host: 7, hits: 1, original_bytes: 1, optimized_bytes: 1 },
      "junk",
    ],
    limit: 32,
    other: { hits: 4, original_bytes: 4000, optimized_bytes: 1000 },
  },
});

/**
 * Per-host rows in every shape the optimizer reports: a name, a site
 * configured by IPv6 address, a name with an underscore, an IPv4 address.
 */
export const OPT_STATS_HOSTS_GRAMMAR: Reply = ok({
  ...JSON.parse(OPT_STATS_PAGE_BYTES),
  serve_savings_by_host: {
    hosts: [
      { host: "www.example.com", hits: 6, original_bytes: 6000, optimized_bytes: 3000 },
      { host: "[2001:db8::1]", hits: 4, original_bytes: 4000, optimized_bytes: 1000 },
      { host: "a_b.test", hits: 3, original_bytes: 3000, optimized_bytes: 1500 },
      { host: "10.1.2.3", hits: 2, original_bytes: 2000, optimized_bytes: 1000 },
    ],
    limit: 32,
    other: { hits: 1, original_bytes: 100, optimized_bytes: 50 },
  },
});

/** A per-host console's narrowed answer: the module's marker names the site, and its row is the only one. */
export const OPT_STATS_SITE_ROW: Reply = ok({
  ...JSON.parse(OPT_STATS_PAGE_BYTES),
  serve_savings_by_host: {
    hosts: [{ host: "example.test", hits: 4, original_bytes: 4000, optimized_bytes: 1000 }],
    limit: 32,
    other: { hits: 9, original_bytes: 9000, optimized_bytes: 4500 },
    site: "example.test",
  },
});

/** A narrowed answer for a site the optimizer has no row for yet. */
export const OPT_STATS_SITE_NO_ROW: Reply = ok({
  ...JSON.parse(OPT_STATS_PAGE_BYTES),
  serve_savings_by_host: {
    hosts: [],
    limit: 32,
    other: { hits: 13, original_bytes: 13000, optimized_bytes: 5500 },
    site: "example.test",
  },
});

/** A narrowed answer for a site without a name of its own: no row can be its own. */
export const OPT_STATS_SITE_NONE: Reply = ok({
  ...JSON.parse(OPT_STATS_PAGE_BYTES),
  serve_savings_by_host: {
    hosts: [],
    limit: 32,
    other: { hits: 13, original_bytes: 13000, optimized_bytes: 5500 },
    site: "",
  },
});

/** A marker that is present but not a host name, beside a row that carries the console's own name. */
export const OPT_STATS_SITE_UNUSABLE: Reply = ok({
  ...JSON.parse(OPT_STATS_PAGE_BYTES),
  serve_savings_by_host: {
    hosts: [{ host: "www.example.test", hits: 4, original_bytes: 4000, optimized_bytes: 1000 }],
    limit: 32,
    other: { hits: 9, original_bytes: 9000, optimized_bytes: 4500 },
    site: "<img src=x onerror=alert(1)>",
  },
});

/** A per-vhost console for the golden capture's second host. */
export const CONFIG_VHOST_STATIC = ok({ config: "", effective_config: "", scope: "vhost", host: "static.example.com:80" });

/**
 * A real stats_json body, byte for byte: a copy of the module's committed
 * golden capture (this repository,
 * test/pagespeed/system/testdata/stats_json.global.golden, without its
 * trailing newline), which the module's own test compares against the bytes
 * it dumps. If that file changes, copy it here again.
 */
export const STATS_JSON_GLOBAL_BYTES =
  '{"variables": {"resource_url_domain_acceptances": 0,"resource_url_domain_rejections": 0,"csp_blocked_rewrites": 0,"rewrite_cached_output_missed_deadline": 0,"rewrite_cached_output_hits": 0,"rewrite_cached_output_misses": 0,"resource_404_count": 0,"slurp_404_count": 0,"total_page_load_ms": 0,"page_load_count": 0,"beacon_overflow_count": 0,"resource_fetches_cached": 0,"resource_fetch_construct_successes": 0,"resource_fetch_construct_failures": 0,"num_cache_control_rewritable_resources": 0,"num_cache_control_not_rewritable_resources": 0,"num_flushes": 0,"num_fallback_responses_served": 0,"num_proactively_freshen_user_facing_request": 0,"num_fallback_responses_served_while_revalidate": 0,"num_conditional_refreshes": 0,"ipro_served": 0,"ipro_not_in_cache": 0,"ipro_not_rewritable": 0,"ipro_daemon_served": 1151,"ipro_daemon_fallthrough": 8847,"ipro_daemon_served_css": 345,"ipro_daemon_served_js": 239,"ipro_daemon_served_image": 567,"ipro_daemon_served_other": 0,"ipro_daemon_fallthrough_css": 2,"ipro_daemon_fallthrough_js": 1,"ipro_daemon_fallthrough_image": 3,"ipro_daemon_fallback_notified": 0,"ipro_daemon_fallback_notify_failed": 0,"ipro_daemon_refresh_notified": 0,"ipro_daemon_refresh_notify_failed": 0,"downstream_cache_purge_attempts": 0,"successful_downstream_cache_purges": 0,"total_fetch_count": 0,"total_rewrite_count": 0,"num_rewrites_executed": 0,"num_rewrites_dropped": 0,"num_resource_fetch_successes": 0,"num_resource_fetch_failures": 0,"process_start_ms": 1759200000000,"html-worker-queue-depth": 0,"rewrite-worker-queue-depth": 0,"low-priority-worked-queue-depth": 0}, "maxlength": 47, "timed_variables": {"num_rewrites_dropped": 0,"num_rewrites_executed": 0,"total_fetch_count": 0,"total_rewrite_count": 0}, "gauges": ["process_start_ms","html-worker-queue-depth","rewrite-worker-queue-depth","low-priority-worked-queue-depth"], "scope": "global", "host": "example.com:80", "timestamp_ms": 1759230000000}';

/** The golden stats body as a reply. */
export const STATS_JSON_GLOBAL: Reply = ok(JSON.parse(STATS_JSON_GLOBAL_BYTES));

/** Optimizer stats with the process started the given age ago (the warming
 *  cases need a live-relative start). */
export function optStatsStarted(ageMs: number): Reply {
  return ok({
    ...JSON.parse(OPT_STATS_PAGE_BYTES),
    started_at_ms: Date.now() - ageMs,
    uptime_seconds: Math.round(ageMs / 1000),
  });
}

/** An untouched ring, in the daemon's envelope. */
export const LOGS_EMPTY: Reply = ok({
  entries: [],
  gap: false,
  more: false,
  newest_seq: 0,
  next_since: 0,
  oldest_seq: 0,
  shed_total: 0,
  stream_id: "9f2c4e1a7b3d5c80",
});

/** The module's answer to an optimizer page over its cap. */
export const LOGS_TOO_LARGE: Reply = { status: 502, body: { error: "response_too_large" } };

interface LogEntryFixture {
  level: string;
  message: string;
  module: string;
  seq: number;
  source: string;
  timestamp: number;
  type: "log";
}

const logEntry = (seq: number, level: string, source: string, module: string, message: string): LogEntryFixture => ({
  level,
  message,
  module,
  seq,
  source,
  timestamp: 1759230000000 + seq * 1000,
  type: "log",
});

const logsReply = (entries: LogEntryFixture[]): Reply =>
  ok({
    entries,
    gap: false,
    more: false,
    newest_seq: entries[entries.length - 1].seq,
    next_since: entries[entries.length - 1].seq,
    oldest_seq: entries[0].seq,
    shed_total: 0,
    stream_id: "9f2c4e1a7b3d5c80",
  });

/** One entry per level, for the filter cases. */
export const LOGS_LEVELS: Reply = logsReply([
  logEntry(10, "debug", "worker", "config", "parsed 42 directives"),
  logEntry(11, "info", "worker", "worker", "cache flush complete"),
  logEntry(12, "warning", "cache", "cache", "origin fetch slow"),
  logEntry(13, "error", "chrome", "browser", "analysis timed out for https://www.example.test/heavy.png"),
]);

/**
 * Log lines can embed visitor-controlled strings: they must render as inert
 * text, and an embedded newline must not be able to draw a second, forged
 * log row inside one entry's message.
 */
export const LOGS_ATTACK: Reply = logsReply([
  logEntry(
    7,
    "error",
    "worker",
    "worker",
    "fetch failed for https://evil.test/<img src=x onerror=alert(1)>?next=javascript:alert(2) — <script>alert(3)</script>",
  ),
  logEntry(8, "error", "worker", "worker", "request aborted\n12:00:01.234 error worker cache purged"),
]);

const STREAM_IDS = ["9f2c4e1a7b3d5c80", "3b7d0e5f1a2c4968", "c41e8a07d2b95f36"];

export interface LogsDaemonOptions {
  /** Entries logged before the first read. */
  initial?: number;
  /** Entries logged between two polls (by poll number, 1 = before the second poll). */
  perPoll?: number | ((poll: number) => number);
  /** Entries the ring keeps (the optimizer keeps 2,000; a smaller ring wraps sooner). */
  ring?: number;
  /** At this read (0-based) the optimizer has restarted: a new stream_id, seqs from 0. */
  restartAtCall?: number;
  /** Entries the restarted optimizer has logged by that read. */
  afterRestart?: number;
}

/**
 * A stand-in optimizer that follows the GET /v1/logs contract as the
 * optimizer documents it (the golden page's envelope): a bounded ring;
 * `since` answers the oldest newer entries, no `since` the newest; `limit`
 * (default and maximum 500); `more`, `gap`, `oldest_seq`/`newest_seq`,
 * `shed_total`, and a `stream_id` that changes when it restarts — an old
 * cursor then gets the empty echo page a real restarted optimizer gives.
 * Time passes between polls, not between the catch-up reads inside one
 * poll: new entries are logged only before a read that follows an answer
 * with `more: false`.
 */
export function logsDaemon(options: LogsDaemonOptions = {}): (call: number, request: MockRequest) => Reply {
  const capacity = options.ring ?? 2000;
  const perPoll = options.perPoll ?? 1;
  let stream = 0;
  let ring: LogEntryFixture[] = [];
  let next = 0;
  let polls = 0;
  let caughtUp = true;
  const log = (n: number) => {
    for (let i = 0; i < n; i++) {
      const message = stream === 0 ? `stream entry ${next}` : `after restart entry ${next}`;
      ring.push(logEntry(next, "info", "worker", "worker", message));
      next += 1;
    }
    if (ring.length > capacity) ring = ring.slice(ring.length - capacity);
  };
  log(options.initial ?? 3);
  return (call, request) => {
    if (call === options.restartAtCall) {
      stream += 1;
      ring = [];
      next = 0;
      log(options.afterRestart ?? 2);
    } else if (call > 0 && caughtUp) {
      polls += 1;
      log(typeof perPoll === "function" ? perPoll(polls) : perPoll);
    }
    const limitParam = Number(request.url.searchParams.get("limit") ?? "500");
    const limit = Number.isInteger(limitParam) && limitParam > 0 ? Math.min(limitParam, 500) : 500;
    const sinceParam = request.url.searchParams.get("since");
    const oldest = ring.length > 0 ? ring[0].seq : 0;
    const newest = ring.length > 0 ? ring[ring.length - 1].seq : 0;
    let entries: LogEntryFixture[];
    let nextSince: number;
    let more = false;
    let gap = false;
    if (sinceParam === null) {
      entries = ring.slice(-limit);
      nextSince = entries.length > 0 ? entries[entries.length - 1].seq : 0;
    } else {
      const since = Number(sinceParam);
      const newer = ring.filter((e) => e.seq > since);
      entries = newer.slice(0, limit);
      more = newer.length > entries.length;
      gap = ring.length > 0 && since + 1 < oldest;
      nextSince = entries.length > 0 ? entries[entries.length - 1].seq : since;
    }
    caughtUp = !more;
    return ok({
      entries,
      gap,
      more,
      newest_seq: newest,
      next_since: nextSince,
      oldest_seq: oldest,
      shed_total: 0,
      stream_id: STREAM_IDS[stream % STREAM_IDS.length],
    });
  };
}

/** A live optimizer: `initial` entries, then one more before each poll. */
export function logsStream(initial = 3): (call: number, request: MockRequest) => Reply {
  return logsDaemon({ initial, perPoll: 1 });
}

// ── The module's message log, grouped (message_history?grouped=1) ──────────

/** The module's admin-exposure warning as a buffered line (an old one: never "recent"). */
export const ACL_LINE =
  "[Thu, 01 Oct 2026 08:00:00 GMT] [Warning] [12345] mod_pagespeed: admin handler 'pagespeed_global_admin' " +
  "received request from 172.18.0.4; intended for loopback. Restrict access at the web-server layer (Apache: " +
  "<Location> Require local; nginx: allow 127.0.0.1; deny all; IIS: InfoUrlsLocalOnly). See " +
  "https://www.modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle";

/** Its template, as the module's grouped answer carries it. */
export const ACL_TEMPLATE =
  "mod_pagespeed: admin handler 'pagespeed_global_admin' received request from N.N.N.N; intended for loopback. " +
  "Restrict access at the web-server layer (Apache: <Location> Require local; nginx: allow N.N.N.N; deny all; " +
  "IIS: InfoUrlsLocalOnly). See URL";

/** The module's refusal to open the optimizer's cache after a layout change. */
export const VOLUME_TEMPLATE =
  "in-place optimization is OFF: the optimizer daemon publishes cache directory generation N (cache_dir_generation) " +
  "and this module is built for generation N. The two cache layouts share nothing; nothing was opened and nothing " +
  "was created. Install a module and daemon package pair that agree.";

export interface GroupRowFixture {
  level: string;
  template: string;
  count: number;
  last_ms: number;
  recent?: number;
}

/**
 * message_history: these grouped rows to a grouped=1 read (the overview's),
 * the plain MESSAGES to any other (the Messages page's). A function picks the
 * rows by the 0-based count of grouped reads.
 */
export function messageGroups(rows: GroupRowFixture[] | ((call: number) => GroupRowFixture[])): Responder {
  let grouped = 0;
  return (_call, request) => {
    if (request.url.searchParams.get("grouped") !== "1") return MESSAGES;
    const list = typeof rows === "function" ? rows(grouped++) : rows;
    return ok({
      scope: "process",
      next: 10,
      now_ms: Date.now(),
      window_s: 900,
      truncated: false,
      groups: list.map((r) => ({ recent: 0, ...r })),
    });
  };
}

/** A module from before grouping: it ignores grouped=1 and answers its plain list. */
export const plainMessages = (messages: Array<{ severity: string; message: string }>): Reply =>
  ok({ scope: "process", next: messages.length, messages });

/** A buffered line stamped `agoMs` before now, in the module's own time format. */
export const recentLine = (agoMs: number, level: string, text: string): string =>
  `[${new Date(Date.now() - agoMs).toUTCString()}] [${level}] [531] ${text}`;

// ── Messages page states ─────────────────────────────────────────────

/** One warning logged three times (two variants of the same URL), between other lines. */
export const MESSAGES_REPEATS = ok({
  scope: "process",
  next: 5,
  messages: [
    { severity: "info", message: "[Fri, 02 Oct 2026 10:25:40 GMT] [Info] [531] CycloneCache enabled at /var/cache/mod_pagespeed/" },
    { severity: "warning", message: "[Fri, 02 Oct 2026 10:25:47 GMT] [Warning] [531] No permission to rewrite 'https://umami.example.test/script.js'" },
    { severity: "warning", message: "[Fri, 02 Oct 2026 10:25:49 GMT] [Warning] [531] No permission to rewrite 'https://umami.example.test/script.js?v=2'" },
    { severity: "error", message: "[Fri, 02 Oct 2026 10:25:50 GMT] [Error] [531] Fetch of https://www.example.test/a.css failed" },
    { severity: "warning", message: "[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] No permission to rewrite 'https://umami.example.test/script.js'" },
  ],
});

/** Markup, script URLs and a quote-broken URL inside a message: all must stay inert text. */
export const MESSAGES_ATTACK = ok({
  scope: "process",
  next: 1,
  messages: [
    {
      severity: "warning",
      message:
        "[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] <img src=x onerror=alert(1)> javascript:alert(2) " +
        "data:text/html,<b>x</b> https://ok.test/a\"onmouseover=alert(3)",
    },
  ],
});

/** Entries without a text message next to a real warning: the page still lists the warning. */
export const MESSAGES_MALFORMED = ok({
  scope: "process",
  next: 4,
  messages: [
    { severity: "warning" },
    { severity: "warning", message: null },
    { severity: "warning", message: 7 },
    { severity: "warning", message: "[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] Fetch of https://www.example.test/b.css failed" },
  ],
});

/** Only Info lines: hidden by the default filter. */
export const MESSAGES_INFO_ONLY = ok({
  scope: "process",
  next: 1,
  messages: [{ severity: "info", message: "[Sat, 26 Sep 2026 10:02:31 GMT] [Info] [531] CycloneCache enabled at /var/cache/mod_pagespeed/" }],
});

/** An empty message buffer. */
export const MESSAGES_EMPTY = ok({ scope: "process", next: 0, messages: [] });

/** A warning whose URL is far wider than a phone. */
export const MESSAGES_LONG_URL = ok({
  scope: "process",
  next: 1,
  messages: [
    {
      severity: "warning",
      message: `[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] Fetch of https://www.example.test/${"a".repeat(300)}.css failed`,
    },
  ],
});
