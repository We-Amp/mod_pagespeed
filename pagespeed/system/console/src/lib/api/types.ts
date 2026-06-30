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

/** GET /v1/license/status */
export interface LicenseStatusResponse {
  licensed: boolean;
  is_global?: boolean;
  license_type?: string;
  expires?: number;
  expired?: boolean;
  /** Subscriber email (legacy key name — not the site domain). */
  domain?: string;
  /** the design record license scope: community | site | org | host. Absent on legacy tokens. */
  scope?: string;
  /** the design record registrable domain the scope binds to. Absent on legacy tokens. */
  site_domain?: string;
  /**
   * the design record: true when an active scope=site license is observed optimizing a
   * host OUTSIDE its licensed site (over-cap). Soft/display-only — never gates
   * optimization. Emitted only when true, and independent of `licensed` (a
   * fully licensed install can be over-cap).
   */
  over_cap?: boolean;
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
  found: boolean;
  token?: string;
  error?: string;
  [key: string]: unknown;
}

/** POST /v1/license/consent */
export interface ConsentResponse {
  success: boolean;
  error?: string;
}
