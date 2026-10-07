// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** Parameters for time-range-based queries. */
export interface TimeRangeParams {
  start?: number;
  end?: number;
  granularity_ms?: number;
}

/** GET /stats_json */
export interface StatsResponse {
  variables: Record<string, number>;
  maxlength: number;
  /** Capture time, epoch milliseconds. Absent on module builds that predate it. */
  timestamp_ms?: number;
  /** Which scope this view reports: the whole server or one virtual host. */
  scope?: "global" | "vhost";
  /** The host:port this instance identifies as. */
  host?: string;
  /** Timed counters as totals since start. */
  timed_variables?: Record<string, number>;
  /** Names of the up/down counters (gauges): always drawn raw, never
   *  differenced. Absent when the module predates the field. */
  gauges?: string[];
  [key: string]: unknown;
}

/** GET /config */
export interface ConfigResponse {
  config: string;
  effective_config?: string;
  scope?: "global" | "vhost";
  host?: string;
  [key: string]: unknown;
}

/** One bucket of a histogram, as served by GET /histograms. */
export interface HistogramBucket {
  start: number;
  limit: number;
  count: number;
}

/** One populated histogram, as served by GET /histograms. */
export interface HistogramJson {
  name: string;
  count: number;
  avg: number | null;
  stddev: number | null;
  min: number | null;
  median: number | null;
  max: number | null;
  p90: number | null;
  p95: number | null;
  p99: number | null;
  buckets: HistogramBucket[];
  [key: string]: unknown;
}

/** GET /histograms — populated histograms only, in HistogramNames() order. */
export interface HistogramsResponse {
  histograms: HistogramJson[];
  [key: string]: unknown;
}

/** GET /cache?url=... */
export interface CacheEntryResponse {
  url: string;
  value?: string;
  error?: string;
  [key: string]: unknown;
}

/** GET /cache — returns a list of named caches with summary strings */
export interface CacheStructureResponse {
  caches?: Array<{ name: string; summary: string }>;
  backend_stats?: string;
  purge_enabled?: boolean;
  /** Legacy fallback: a single pre-formatted string */
  structure?: string;
  [key: string]: unknown;
}

/** GET /cache?physical_caches */
export interface PhysicalCachesResponse {
  caches: Array<{
    name: string;
    entries: number;
    size: number;
  }>;
  [key: string]: unknown;
}

/** POST /cache?purge=... */
export interface PurgeResponse {
  success: boolean;
  message?: string;
  /** Present on failure, e.g. "Purging not enabled: please add 'EnableCachePurge on'...". */
  error?: string;
  [key: string]: unknown;
}

/** Raw response from GET /cache?new_set — the backend returns a single
 *  newline-delimited string: "Global@datestring\nurl1@datestring\n..." */
export interface RawPurgeSetResponse {
  purge_set?: string;
  [key: string]: unknown;
}

/** Parsed purge-set data used by the UI. */
export interface PurgeSetResponse {
  purge_set?: string[];
  global_invalidation_timestamp_ms?: number;
  purge_enabled?: boolean;
  [key: string]: unknown;
}

/** GET /console */
export interface ConsoleResponse {
  graphs?: Array<{
    name: string;
    data: number[];
  }>;
  timestamps?: number[];
  [key: string]: unknown;
}

/** GET /message_history */
export interface MessagesResponse {
  /** "process": the buffer is process-wide, shared by every virtual host. */
  scope?: string;
  /** Monotonic write-count cursor; pass back as `since` to poll incrementally. */
  next?: number;
  messages: Array<{
    timestamp: number;
    severity: string;
    message: string;
  }>;
  [key: string]: unknown;
}

/** One row of GET /message_history?grouped=1: one level and message template. */
export interface MessageGroupRow {
  level?: string;
  /** The message with URLs, hexadecimal ids and numbers folded (URL, ID, N). */
  template?: string;
  count?: number;
  /** Epoch ms of the newest line in the row; 0 when no line carried a time. */
  last_ms?: number;
  /** Lines within the requested window_s; present only when one was asked for. */
  recent?: number;
  [key: string]: unknown;
}

/**
 * GET /message_history?grouped=1&window_s=N. A module that predates the
 * grouped mode ignores both parameters and answers MessagesResponse.
 */
export interface GroupedMessagesResponse {
  scope?: string;
  next?: number;
  now_ms?: number;
  window_s?: number;
  truncated?: boolean;
  groups: MessageGroupRow[];
  [key: string]: unknown;
}

// ── Daemon (read-only proxy of the optimizer daemon's management API) ──────
//
// Every field is optional: older daemons omit newer blocks, and the daemon is
// an untrusted peer — its JSON is rendered defensively throughout.

/** GET /v1/daemon/health */
export interface DaemonHealthResponse {
  status?: string;
  ready?: boolean;
  version?: string;
  git_commit?: string;
  uptime_seconds?: number;
  inflight?: number;
  connections?: {
    active?: number;
    max?: number;
  };
  /** Named sub-health checks; values are daemon-defined (string or object). */
  checks?: Record<string, unknown>;
  /** Browser-based analysis state; absent when the daemon is older. */
  browser?: {
    enabled?: boolean;
    chrome_running?: boolean;
    chrome_consecutive_failures?: number;
    chrome_restart_delay_ms?: number;
  };
  [key: string]: unknown;
}

/** GET /v1/daemon/stats */
export interface DaemonStatsResponse {
  thread_pool?: {
    inflight?: number;
    size?: number;
  };
  connections?: {
    active?: number;
    max?: number;
  };
  notifications?: {
    received?: number;
    skipped_dedup?: number;
    skipped_inflight?: number;
  };
  cache?: {
    entries?: number;
    size_bytes?: number;
  };
  /** Serve-savings counters from the daemon's shared statistics surface. */
  serve_savings?: Record<string, unknown>;
  /** Serve savings per host: the most served hosts and "other". Absent on older optimizers. */
  serve_savings_by_host?: unknown;
  /** Entries judged already optimal (no smaller variant produced), per type,
   *  counted once per entry over the worker process's lifetime. Absent on
   *  older optimizers. */
  verdicts?: Record<string, unknown>;
  /** Epoch milliseconds when the optimizer process started. Absent on older
   *  optimizers; 0 is "unknown", never "the epoch". */
  started_at_ms?: number;
  /** Seconds the optimizer process has been up. Absent on older optimizers. */
  uptime_seconds?: number;
  /** Error counters; `origin_misconfiguration` counts compressed origin responses. */
  errors?: {
    total?: number;
    origin_misconfiguration?: number;
  };
  /** Optimized-variant writes; `write_failures` counts failed cache writes. */
  alternates?: {
    writes?: number;
    write_failures?: number;
  };
  [key: string]: unknown;
}

/** One entry of GET /v1/daemon/cooldowns. */
export interface DaemonCooldownEntry {
  url?: string;
  /** The entry's host and scheme: with `url`, the cache key. */
  hostname?: string;
  scheme?: string;
  reason?: "processing" | "write_failure" | "revalidation" | string;
  remaining_seconds?: number;
  duration_seconds?: number;
  [key: string]: unknown;
}

/** GET /v1/daemon/cooldowns — a bare list or an object wrapping one. */
export type DaemonCooldownsResponse =
  | DaemonCooldownEntry[]
  | { cooldowns?: DaemonCooldownEntry[]; [key: string]: unknown };

/**
 * How the optimizer names one cached entry: `url` is the origin-form path +
 * query the module recorded (e.g. "/hero.png?v=2"), plus the host and scheme.
 * Required by every per-URL daemon leaf.
 */
export interface CacheEntryKey {
  url: string;
  host: string;
  scheme: "http" | "https";
}

/** One entry of GET /v1/daemon/cache/urls. `url` is a path, not an absolute URL. */
export interface DaemonCacheUrlEntry {
  url?: string;
  hostname?: string;
  scheme?: string;
  alternate_count?: number;
  cache_key?: string;
  [key: string]: unknown;
}

/** GET /v1/daemon/cache/urls — one page of the cached-URL index. */
export interface DaemonCacheUrlsResponse {
  urls?: DaemonCacheUrlEntry[];
  offset?: number;
  limit?: number;
  next_offset?: number;
  has_more?: boolean;
  total?: number;
  [key: string]: unknown;
}

/** The decoded capability mask of a variant. */
export interface DaemonAlternateMask {
  raw?: number;
  format?: string; // "original" | "webp" | "avif" | "svg"
  viewport?: string; // "mobile" | "tablet" | "desktop"
  density?: string; // "1x" | "2x+"
  save_data?: boolean;
  encoding?: string; // "identity" | "gzip" | "brotli"
  [key: string]: unknown;
}

/** One cached variant of an entry, from GET /v1/daemon/cache/alternates. */
export interface DaemonAlternate {
  alternate_id?: number;
  size?: number;
  hit_count?: number;
  is_sentinel?: boolean;
  sentinel_name?: string;
  last_access?: number; // epoch milliseconds
  mask?: DaemonAlternateMask;
  /** "original" | "webp" | "avif" | "svg"; worker-processed originals: "jpeg" | "png" | "gif". */
  format?: string;
  viewport?: string;
  density?: string; // "1x" | "2x+"
  save_data?: boolean;
  encoding?: string; // "identity" | "gzip" | "brotli"
  /** A content CLASS, not a MIME type: "html" | "css" | "js" | "image" | "other" (or ""). */
  content_type?: string;
  /** The origin's MIME type, possibly empty. */
  origin_content_type?: string;
  flags?: number;
  needs_revalidation?: boolean;
  cache_inserted_at?: number; // epoch seconds
  origin_max_age?: number;
  origin_s_maxage?: number;
  origin_cc_flags?: number;
  version?: number;
  ssimulacra2_score?: number;
  content_class?: string; // "photo" | "screenshot" | "illustration" | "noisy"
  original_size?: number;
  [key: string]: unknown;
}

/** GET /v1/daemon/cache/alternates. */
export interface DaemonAlternatesResponse {
  url?: string;
  hostname?: string;
  scheme?: string;
  cache_key?: string;
  alternates?: DaemonAlternate[];
  count?: number;
  chain_length?: number;
  cooldown?: { reason?: string; remaining_seconds?: number; duration_seconds?: number };
  [key: string]: unknown;
}

/**
 * One entry of the optimizer's in-memory log ring, as GET /v1/daemon/logs
 * returns it (the daemon's GET /v1/logs, forwarded). Untrusted: an older or
 * newer daemon omits or adds fields, and `message` can embed
 * visitor-controlled strings — render it as plain text only.
 */
export interface DaemonLogEntry {
  type?: string;
  /** Per-process sequence number, ascending in ring order. */
  seq?: number;
  /** Epoch milliseconds. */
  timestamp?: number;
  source?: string;
  level?: string;
  module?: string;
  message?: string;
  details?: unknown;
  [key: string]: unknown;
}

/** A page of the optimizer's log ring (ascending seq), plus poll cursors. */
export interface DaemonLogsResponse {
  entries?: DaemonLogEntry[];
  /** The cursor the next poll passes as `since`. */
  next_since?: number;
  /** The oldest / newest seq the ring still retains. */
  oldest_seq?: number;
  newest_seq?: number;
  /** True when the ring wrapped past the caller's cursor (entries dropped). */
  gap?: boolean;
  /** True when newer entries are waiting beyond this page: read again now. */
  more?: boolean;
  /** Entries the optimizer could not keep (never sequenced); monotonic per process. */
  shed_total?: number;
  /** Identifies the optimizer process; a change means it restarted and seqs started over. */
  stream_id?: string;
  [key: string]: unknown;
}

