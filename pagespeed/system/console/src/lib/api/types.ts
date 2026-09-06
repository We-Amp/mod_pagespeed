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
  timestamp: number;
  [key: string]: unknown;
}

/** GET /config */
export interface ConfigResponse {
  config: string;
  [key: string]: unknown;
}

/** GET /histograms */
export interface HistogramsResponse {
  histograms: string;
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
  messages: Array<{
    timestamp: number;
    severity: string;
    message: string;
  }>;
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
  [key: string]: unknown;
}

/** One entry of GET /v1/daemon/cooldowns. */
export interface DaemonCooldownEntry {
  url?: string;
  reason?: "processing" | "write_failure" | "revalidation" | string;
  remaining_seconds?: number;
  duration_seconds?: number;
  [key: string]: unknown;
}

/** GET /v1/daemon/cooldowns — a bare list or an object wrapping one. */
export type DaemonCooldownsResponse =
  | DaemonCooldownEntry[]
  | { cooldowns?: DaemonCooldownEntry[]; [key: string]: unknown };

