// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// Backend API response types for the mod_pagespeed worker management API.
// ---------------------------------------------------------------------------

// ---- GET /v1/health -------------------------------------------------------

export interface HealthCheck {
  pass: boolean;
  detail?: string;
}

export interface HealthResponse {
  /** 'unhealthy' is reserved for future use (not currently emitted by the worker). */
  status: 'ok' | 'degraded' | 'unhealthy';
  checks: Record<string, HealthCheck>;
  version?: string;
  uptime_seconds: number;
  connections: { active: number; max: number };
  inflight: number;
  ready: boolean;
}

// ---- GET /v1/stats --------------------------------------------------------

export interface TypeStats {
  count: number;
  time_us: number;
}

export interface ServeSavingsEntry {
  original_bytes: number;
  optimized_bytes: number;
  hits: number;
}

export interface StatsResponse {
  notifications: {
    received: number;
    skipped_dedup: number;
    skipped_inflight: number;
    rejected_version: number;
    rejected_malformed: number;
    rejected_sentinel: number;
  };
  variants: {
    written: number;
    proactive: number;
    gzip: number;
    brotli: number;
  };
  errors: { total: number; origin_misconfiguration: number };
  cache: { entries: number; size_bytes: number };
  by_type: {
    html: TypeStats;
    css: TypeStats;
    js: TypeStats;
    image: TypeStats;
  };
  by_format: { webp: number; avif: number; jpeg: number; png: number };
  svg: {
    candidates_evaluated: number;
    candidates_rejected: number;
    vectorized: number;
    fidelity_rejected: number;
    timeout_exceeded: number;
    size_rejected: number;
    path_count_rejected: number;
    written: number;
    bytes_saved: number;
    vectorize_time_us: number;
    served: number;
  };
  content_analysis: {
    photo: number;
    screenshot: number;
    illustration: number;
    noisy: number;
    denoised: number;
  };
  ssimulacra2: {
    checks: number;
    reencodes: number;
    declines: number;
    tombstone_hits: number;
    avg_score_x100: number;
  };
  learned_quality: {
    predictions: number;
    fallbacks: number;
  };
  html_assembly: {
    complete: number;
    skipped: number;
    css_aborted: number;
  };
  alternates: { writes: number; write_failures: number };
  selector_invocations: number;
  cache_read_retries: number;
  cache_read_failures: number;
  cache_read_deferred_retries: number;
  cache_read_deferred_successes: number;
  cache_auto_heals: number;
  cache_auto_heal_exhausted: number;
  image_incomplete_matrices: number;
  image_no_savings_skipped: number;
  image_unconverted_fallthrough: number;
  dedup: {
    writes_skipped: number;
    content_hash_hits: number;
    content_hash_stale: number;
  };
  quality_baselining: { capped_jpeg: number; skip_reencode: number };
  thread_pool: { inflight: number; size: number };
  connections: { active: number; max: number };
  browser?: {
    enabled: boolean;
    chrome_running: boolean;
    profiles_generated: number;
    profiles_used: number;
    analysis_errors: number;
    chrome_crashes: number;
    queue_depth: number;
    css_inlining_attempted: number;
    css_inlining_stylesheets_found: number;
    css_inlining_stylesheets_cached: number;
    css_inlining_bytes_inlined: number;
    reanalyses_scheduled: number;
    scripts_analyzed: number;
    scripts_deferrable: number;
  };
  policy?: {
    computed: number;
    async_css_enabled: number;
    script_deferral_enabled: number;
  };
  serve_savings?: {
    html: ServeSavingsEntry;
    css: ServeSavingsEntry;
    js: ServeSavingsEntry;
    image: ServeSavingsEntry;
  };
}

// ---- GET /v1/config  &  PATCH /v1/config ----------------------------------

export interface ConfigResponse {
  jpeg_quality: number;
  webp_quality: number;
  avif_quality: number;
  savedata_jpeg_quality: number;
  savedata_webp_quality: number;
  savedata_avif_quality: number;
  mobile_width: number;
  tablet_width: number;
  desktop_width: number;
  proactive_image_variants: boolean;
  proactive_viewport_variants: boolean;
  proactive_savedata_variants: boolean;
  proactive_density_variants: boolean;
  content_analysis: boolean;
  quality_verify: boolean;
  target_ssimulacra2: number;
  ssimulacra2_tolerance: number;
  disable_html: boolean;
  disable_css: boolean;
  disable_js: boolean;
  disable_script_deferral: boolean;
  disable_image: boolean;
  disable_lazy_load: boolean;
  disable_image_dimensions: boolean;
  disable_lcp_preload: boolean;
  disable_preconnect_injection: boolean;
  enable_speculation_rules: boolean;
  disable_async_css: boolean;
  disable_css_import_flattening: boolean;
  svg_preset: string;
  svg_fidelity_threshold: number;
  svg_timeout_ms: number;
  svg_candidacy_threshold: number;
  svg_color_precision: number;
  svg_exclude_lcp: boolean;
  svg_filter_speckle: number;
  svg_max_paths: number;
  svg_max_pixels: number;
  svg_max_svg_bytes: number;
  svg_mode: string;
  no_quality_cap: boolean;
  quality_cap_margin: number;
  savedata_score_reduction: number;
  gzip_level: number;
  brotli_level: number;
  max_url_length: number;
  max_html_size: number;
  max_css_size: number;
  max_js_size: number;
  max_image_size: number;
  enable_browser_analysis: boolean;
  chrome_binary: string;
  chrome_recycle_interval: number;
  chrome_page_timeout: number;
  chrome_max_memory: number;
  enable_browser_critical_css: boolean;
  enable_browser_lazy_loading: boolean;
  enable_browser_lcp_preload: boolean;
  enable_browser_image_sizing: boolean;
  browser_queue_size: number;
  browser_profile_ttl: number;
  learned_quality: boolean;
  learned_quality_jpeg: boolean;
  learned_quality_webp: boolean;
  learned_quality_avif: boolean;
  cache_mode: string;
  enable_warmup: boolean;
}

export interface ConfigPatchResponse {
  applied: Partial<ConfigResponse>;
  rejected: Partial<ConfigResponse>;
  warnings: string[];
  config: ConfigResponse;
}

// ---- GET /v1/cache/alternates ---------------------------------------------

export interface AlternateInfo {
  alternate_id: number;
  size: number;
  hit_count: number;
  is_sentinel: boolean;
  /** Sentinel type name (e.g., 'early_hints', 'browser_profile'). Only present for sentinels. */
  sentinel_name?: string;
  /** Image format string. Not present for sentinel entries. */
  format?: string;
  /** Viewport class string. Not present for sentinel entries. */
  viewport?: string;
  /** Pixel density string. Not present for sentinel entries. */
  density?: string;
  /** Save-Data flag. Not present for sentinel entries. */
  save_data?: boolean;
  /** Transfer encoding string. Not present for sentinel entries. */
  encoding?: string;
  /** Content type string. Not present for sentinel entries. */
  content_type?: string;
  /** Origin content type. Not present for sentinel entries. */
  origin_content_type?: string;
  /** Metadata flags byte (bit 0 = needs revalidation). Not present for sentinel entries. */
  flags?: number;
  /** True when HTML variant needs revalidation (CSS not yet cached). Not present for sentinel entries. */
  needs_revalidation?: boolean;
  /** Last access time as epoch milliseconds. */
  last_access: number;
  /** Unix timestamp (seconds) when this alternate was cached or revalidated. */
  cache_inserted_at?: number;
  /** Origin max-age value (seconds). 0 when origin sent no max-age. */
  origin_max_age?: number;
  /** Origin s-maxage value (seconds). 0 when absent. */
  origin_s_maxage?: number;
  /** Bitfield of origin Cache-Control directives. Bit 9 = header present. */
  origin_cc_flags?: number;
  /** Metadata wire format version (3, 4, or 5). */
  version?: number;
  /** SSIMULACRA2 quality score (0-100). Present for optimized image alternates. */
  ssimulacra2_score?: number;
  /** Content classification. Present when content analysis is enabled. */
  content_class?: 'photo' | 'screenshot' | 'illustration' | 'noisy';
  /** Original size in bytes before optimization. Present for non-original alternates. */
  original_size?: number;
}

// ---- GET /v1/cache/cooldowns ----------------------------------------------

export interface CooldownInfo {
  reason: 'processing' | 'write_failure' | 'revalidation';
  remaining_seconds: number;
  duration_seconds: number;
}

export interface CooldownListEntry extends CooldownInfo {
  url: string;
  hostname: string;
  /** Request scheme ("http" or "https"). Optional for backward compat with older workers. */
  scheme?: string;
}

export interface CooldownsResponse {
  cooldowns: CooldownListEntry[];
  count: number;
  enabled: boolean;
}

// ---- GET /v1/cache/alternates ---------------------------------------------

export interface CacheAlternatesResponse {
  url: string;
  hostname: string;
  /** Request scheme ("http" or "https"). Optional for backward compat with older workers. */
  scheme?: string;
  alternates: AlternateInfo[];
  /** Number of unique (deduplicated) alternates. */
  count: number;
  /** Raw chain length including stale duplicate entries. */
  chain_length: number;
  cooldown?: CooldownInfo;
}

// ---- GET /v1/cache/urls ---------------------------------------------------

export interface CacheUrlEntry {
  url: string;
  hostname: string;
  /** Request scheme ("http" or "https"). Optional for backward compat with older workers. */
  scheme?: string;
  /**
   * Cache keyspace (nginx server block) the entry belongs to. Absent or empty
   * for unsited entries. Pass it back to the alternates, select, content,
   * purge and reprocess calls so they address the same keyspace.
   */
  site?: string;
  alternate_count: number;
}

export interface CacheUrlsResponse {
  total: number;
  offset: number;
  limit: number;
  /** Offset value for fetching the next page. */
  next_offset: number;
  /** Whether there are more pages after this one. */
  has_more: boolean;
  urls: CacheUrlEntry[];
}

// ---- GET /v1/cache/select -------------------------------------------------

/** Decoded capability mask returned by the C++ MaskToJson helper. */
export interface MaskInfo {
  raw: number;
  format: string;
  viewport: string;
  density: string;
  save_data: boolean;
  encoding: string;
}

/** Per-alternate scoring entry in the select response. */
export interface CacheSelectAlternateEntry {
  alternate_id: number;
  is_sentinel: boolean;
  size: number;
  score: number;
  /** Present for sentinel entries. */
  sentinel_name?: string;
  /** Decoded mask (present for non-sentinel entries with metadata). */
  mask?: MaskInfo;
  /** Content type string (present for non-sentinel entries with metadata). */
  content_type?: string;
  /** Set when metadata read failed. */
  error?: string;
}

export interface CacheSelectResponse {
  url: string;
  hostname: string;
  /** Request scheme ("http" or "https"). Optional for backward compat with older workers. */
  scheme?: string;
  client_mask: MaskInfo;
  alternates: CacheSelectAlternateEntry[];
  best_index: number;
  best_score: number;
}

// ---- POST /v1/cache/purge -------------------------------------------------

export interface CachePurgeResponse {
  url: string;
  hostname: string;
  /** Request scheme ("http" or "https"). Optional for backward compat with older workers. */
  scheme?: string;
  deleted: number;
  note?: string;
}

export interface CachePurgeAllResponse {
  scope: 'all';
  urls_cleared: number;
  volume_reset: boolean;
}

// ---- POST /v1/cache/reprocess ---------------------------------------------

export interface CacheReprocessResponse {
  url: string;
  hostname: string;
  /** Request scheme ("http" or "https"). Optional for backward compat with older workers. */
  scheme?: string;
  reprocess_enqueued: boolean;
}

// ---- Error envelope -------------------------------------------------------

export interface ApiErrorDetail {
  code: string;
  message: string;
  details?: Record<string, unknown>;
}

export interface ApiErrorResponse {
  error: ApiErrorDetail;
}

// ---- WebSocket /v1/ws/stats -----------------------------------------------

export interface WsStatsSnapshot {
  type: 'snapshot';
  sequence: number;
  data: StatsResponse;
}

export interface WsStatsDelta {
  type: 'delta';
  sequence: number;
  data: Partial<StatsResponse>;
}

export type WsStatsMessage = WsStatsSnapshot | WsStatsDelta;

/** Client-to-server messages on the stats WebSocket. */
export interface WsStatsAuth {
  auth: string;
}

export interface WsStatsSetInterval {
  set_interval_ms: number;
}

export type WsStatsClientMessage = WsStatsAuth | WsStatsSetInterval;

// ---- POST /v1/capture/waterfall -------------------------------------------

/** Resource type classification for waterfall entries. */
export type CaptureResourceType =
  | 'document'
  | 'stylesheet'
  | 'script'
  | 'image'
  | 'font'
  | 'xhr'
  | 'other';

/** Per-request timing phases (milliseconds). */
export interface CaptureRequestTiming {
  dns: number;
  connect: number;
  tls: number;
  ttfb: number;
  download: number;
}

/** One network request in the captured waterfall. */
export interface CaptureWaterfallEntry {
  url: string;
  method: string;
  resourceType: CaptureResourceType;
  startTime: number;
  timing: CaptureRequestTiming;
  size: number;
  decodedSize: number;
  status: number;
  mimeType: string;
  fromCache: boolean;
  fromServiceWorker: boolean;
  protocol: string;
}

/** Navigation timing milestones (ms from navigation start). */
export interface CaptureNavigationTiming {
  domContentLoaded: number;
  loadEvent: number;
  firstPaint: number;
  firstContentfulPaint: number;
  largestContentfulPaint: number;
}

export interface CaptureWaterfallRequest {
  url: string;
  viewport_width?: number;
  through_proxy?: boolean;
}

export interface CaptureWaterfallResponse {
  url: string;
  viewport_width: number;
  entries: CaptureWaterfallEntry[];
  navigation_timing: CaptureNavigationTiming;
  total_transfer_size: number;
  total_decoded_size: number;
}

// ---- POST /v1/capture/screenshot ------------------------------------------

export interface CaptureScreenshotRequest {
  url: string;
  viewport_width?: number;
  through_proxy?: boolean;
  full_page?: boolean;
}

export interface CaptureScreenshotResponse {
  url: string;
  viewport_width: number;
  viewport_height: number;
  png_base64: string;
}

// ---- WebSocket /v1/ws/events ----------------------------------------------

/** A single event within a batched events envelope. */
export interface WsEventItem {
  type: string;
  data: Record<string, unknown>;
}

/** Batched events envelope sent by the worker over the events WebSocket. */
export interface WsEventBatch {
  type: 'events';
  sequence: number;
  data: WsEventItem[];
}

export interface WsEventOverflow {
  type: 'overflow';
  dropped_count: number;
}

/** Discriminate with `msg.type`: `'events'` for batched events, `'overflow'` for overflow. */
export type WsEventMessage = WsEventBatch | WsEventOverflow;

// ---- WebSocket /v1/ws/logs -----------------------------------------------

export type LogLevel = 'debug' | 'info' | 'warning' | 'error';
export type LogSource = 'worker' | 'cache' | 'chrome';

export interface LogEntry {
  type: 'log';
  timestamp: number;
  source: LogSource;
  level: LogLevel;
  module: string;
  message: string;
  details?: Record<string, unknown>;
}

export interface WsLogSnapshot {
  type: 'snapshot';
  entries: LogEntry[];
  total: number;
}

export interface WsLogOverflow {
  type: 'overflow';
  dropped_count: number;
  /** First and last seq of the entries that were not sent. */
  first_seq?: number;
  last_seq?: number;
}

export type WsLogMessage = LogEntry | WsLogSnapshot | WsLogOverflow;
