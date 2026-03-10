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
  [key: string]: unknown;
}

/** GET /cache?purge_set */
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

/** GET /v1/license/status */
export interface LicenseStatusResponse {
  licensed: boolean;
  license_type?: string;
  expires?: number;
  domain?: string;
  features?: string[];
  trial_available?: boolean;
  error?: string;
  [key: string]: unknown;
}

/** POST /v1/license/apply */
export interface LicenseApplyResponse {
  success: boolean;
  message?: string;
  error?: string;
  [key: string]: unknown;
}

/** POST /v1/license/activate */
export interface ActivateResponse {
  success: boolean;
  token?: string;
  error?: string;
  [key: string]: unknown;
}
