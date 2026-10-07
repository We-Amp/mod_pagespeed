// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type {
  StatsResponse,
  ConfigResponse,
  HistogramsResponse,
  CacheEntryResponse,
  CacheStructureResponse,
  PhysicalCachesResponse,
  PurgeResponse,
  RawPurgeSetResponse,
  PurgeSetResponse,
  ConsoleResponse,
  MessagesResponse,
  GroupedMessagesResponse,
  DaemonHealthResponse,
  DaemonStatsResponse,
  DaemonCooldownsResponse,
  CacheEntryKey,
  DaemonCacheUrlsResponse,
  DaemonAlternatesResponse,
  DaemonLogsResponse,
  TimeRangeParams,
} from "./types";

import { NetworkError, connection } from "./connection";

// Identical GETs in flight at the same time share one request, whichever
// page or client instance asked: the daemon proxy serves one read per
// endpoint at a time, so two pages of one tab must not compete for it.
// The shared answer is one object handed to every caller: callers treat it
// as read-only (none mutates it today).
const inflight = new Map<string, Promise<unknown>>();

/** Every request is aborted after this long and reported as "no answer". */
export const REQUEST_TIMEOUT_MS = 15_000;

export class ApiError extends Error {
  status: number;
  constructor(status: number, statusText: string) {
    super(`HTTP ${status}: ${statusText}`);
    this.status = status;
    Object.setPrototypeOf(this, ApiError.prototype);
  }
}

export class AdminApiClient {
  /** The admin path the console is served under, without a trailing slash. */
  readonly basePath: string;

  constructor(basePath: string) {
    this.basePath = basePath.replace(/\/$/, "");
  }

  private get<T>(path: string): Promise<T> {
    const url = `${this.basePath}${path}`;
    const shared = inflight.get(url);
    if (shared !== undefined) return shared as Promise<T>;
    const request = this.send<T>(path, url).finally(() => {
      inflight.delete(url);
    });
    inflight.set(url, request);
    return request;
  }

  private async send<T>(path: string, url: string, init?: RequestInit): Promise<T> {
    let response: Response;
    // A request that never answers (a black-holed connection, or one whose
    // headers arrive but whose body then stalls) must not stall the poller
    // waiting on it: abort it and report "no answer". The timer is cleared
    // only once the body has been consumed, on every path below, so a
    // stalled body is covered exactly like a stalled connect.
    const abort = new AbortController();
    const timer = setTimeout(() => abort.abort(), REQUEST_TIMEOUT_MS);
    try {
      response = await fetch(url, { ...init, signal: abort.signal });
    } catch {
      clearTimeout(timer);
      connection.record(path, null, Date.now());
      throw new NetworkError();
    }
    let text: string;
    try {
      // Always try JSON first — the backend may serve JSON with a wrong
      // content-type (e.g. application/javascript instead of application/json).
      text = await response.text();
    } catch {
      clearTimeout(timer);
      connection.record(path, null, Date.now());
      throw new NetworkError();
    }
    clearTimeout(timer);
    connection.record(path, response.status, Date.now(), text);
    if (!response.ok) {
      throw this.errorFromBody(response.status, response.statusText, text);
    }
    return this.parseResponse<T>(response.status, text);
  }

  private post<T>(
    path: string,
    body?: Record<string, unknown>,
  ): Promise<T> {
    return this.send<T>(path, `${this.basePath}${path}`, {
      method: "POST",
      headers: body
        ? { "Content-Type": "application/json", "X-Requested-With": "XMLHttpRequest" }
        : { "X-Requested-With": "XMLHttpRequest" },
      body: body ? JSON.stringify(body) : undefined,
    });
  }

  /**
   * Build an ApiError from a non-2xx response's already-read body, preferring
   * the backend's JSON `error` field over the bare status text. Admin
   * handlers put the actionable message in the body (e.g. "console_logger
   * must be enabled to use '?json'", CSRF/rate-limit reasons, "Unknown admin
   * page"), so surfacing it turns "HTTP 404" into a fix.
   */
  private errorFromBody(status: number, statusText: string, text: string): ApiError {
    let detail = statusText;
    try {
      const stripped = text.replace(/^\)\]\}'?\s*\n/, "");
      const body = JSON.parse(stripped) as { error?: unknown };
      if (typeof body.error === "string" && body.error) detail = body.error;
    } catch {
      // Non-JSON or unreadable body: keep the status text.
    }
    return new ApiError(status, detail);
  }

  /**
   * Parse a response body as JSON, stripping any XSSI protection prefix
   * (e.g. ")]}'\n" or ")]}\n") that some endpoints prepend.
   */
  private parseResponse<T>(status: number, text: string): T {
    // Strip XSSI protection prefix if present.
    const stripped = text.replace(/^\)\]\}'?\s*\n/, "");
    try {
      return JSON.parse(stripped) as T;
    } catch {
      throw new ApiError(status, `Invalid JSON response: ${text.substring(0, 200)}`);
    }
  }

  // ── Statistics ──────────────────────────────────────────────

  async getStats(): Promise<StatsResponse> {
    return this.get<StatsResponse>("/stats_json");
  }

  // ── Configuration ──────────────────────────────────────────

  async getConfig(): Promise<ConfigResponse> {
    return this.get<ConfigResponse>("/config");
  }

  // ── Histograms ─────────────────────────────────────────────

  async getHistograms(): Promise<HistogramsResponse> {
    return this.get<HistogramsResponse>("/histograms");
  }

  // ── Caches ─────────────────────────────────────────────────

  async getCacheStructure(): Promise<CacheStructureResponse> {
    return this.get<CacheStructureResponse>("/cache");
  }

  async getCacheEntry(url: string): Promise<CacheEntryResponse> {
    return this.get<CacheEntryResponse>(
      `/cache?url=${encodeURIComponent(url)}`,
    );
  }

  async getPhysicalCaches(): Promise<PhysicalCachesResponse> {
    return this.get<PhysicalCachesResponse>("/cache?physical_caches");
  }

  async purgeUrl(url: string): Promise<PurgeResponse> {
    return this.post<PurgeResponse>(`/cache?purge=${encodeURIComponent(url)}`);
  }

  async getPurgeSet(): Promise<PurgeSetResponse> {
    const raw = await this.get<RawPurgeSetResponse>("/cache?new_set=");
    return AdminApiClient.parsePurgeSet(raw);
  }

  /**
   * Parse the raw purge_set string from the backend into the structured
   * format expected by the UI.
   *
   * Raw format: "Global@datestring\nurl1@datestring\nurl2@datestring\n..."
   * Parsed:
   *   - global_invalidation_timestamp_ms: unix-ms from the "Global@" line
   *   - purge_set: array of URL strings from non-Global lines
   */
  private static parsePurgeSet(raw: RawPurgeSetResponse): PurgeSetResponse {
    const result: PurgeSetResponse = {};
    const rawStr = raw.purge_set;
    if (typeof rawStr !== "string" || rawStr.length === 0) {
      return result;
    }

    const lines = rawStr.split("\n").filter((l) => l.length > 0);
    const urls: string[] = [];

    for (const line of lines) {
      const atIdx = line.lastIndexOf("@");
      if (atIdx === -1) continue;

      const key = line.substring(0, atIdx);
      const dateStr = line.substring(atIdx + 1);

      if (key === "Global") {
        const ts = Date.parse(dateStr);
        if (!isNaN(ts)) {
          result.global_invalidation_timestamp_ms = ts;
        }
      } else {
        urls.push(key);
      }
    }

    if (urls.length > 0) {
      result.purge_set = urls;
    }

    return result;
  }

  // ── Console ────────────────────────────────────────────────

  async getConsole(params?: TimeRangeParams): Promise<ConsoleResponse> {
    const query = new URLSearchParams();
    // The backend requires "json" to return JSON instead of the SPA shell.
    query.set("json", "1");
    // Backend uses "start_time" / "end_time" / "granularity".
    if (params?.start !== undefined) query.set("start_time", String(params.start));
    if (params?.end !== undefined) query.set("end_time", String(params.end));
    if (params?.granularity_ms !== undefined)
      query.set("granularity", String(params.granularity_ms));
    const qs = query.toString();
    return this.get<ConsoleResponse>(`/console${qs ? `?${qs}` : ""}`);
  }

  // ── Messages ───────────────────────────────────────────────

  async getMessages(since?: number): Promise<MessagesResponse> {
    return this.get<MessagesResponse>(
      since === undefined ? "/message_history" : `/message_history?since=${since}`,
    );
  }

  /**
   * The message log grouped by message template, with each group's count
   * within the last `windowSeconds`. A module that predates the grouped mode
   * ignores both parameters and answers the plain list; toMessageDigest
   * tells the two apart by shape.
   */
  async getMessageGroups(windowSeconds: number): Promise<GroupedMessagesResponse | MessagesResponse> {
    const query = new URLSearchParams({ grouped: "1", window_s: String(Math.floor(windowSeconds)) });
    return this.get<GroupedMessagesResponse | MessagesResponse>(`/message_history?${query.toString()}`);
  }

  // ── Graphs ─────────────────────────────────────────────────

  async getGraphs(params?: TimeRangeParams): Promise<ConsoleResponse> {
    const query = new URLSearchParams();
    // The backend requires "json" to return JSON instead of the SPA shell.
    query.set("json", "1");
    // Backend uses "start_time" / "end_time" / "granularity" (not "start"/"end"/"granularity_ms").
    if (params?.start !== undefined) query.set("start_time", String(params.start));
    if (params?.end !== undefined) query.set("end_time", String(params.end));
    if (params?.granularity_ms !== undefined)
      query.set("granularity", String(params.granularity_ms));
    const qs = query.toString();
    return this.get<ConsoleResponse>(`/graphs${qs ? `?${qs}` : ""}`);
  }

  // ── Daemon ─────────────────────────────────────────────────
  // Read-only proxy of the optimizer daemon's management API. A 502 means the
  // daemon is unreachable or not configured — a normal operating state the
  // panels render as an empty state, not an error.

  async daemonHealth(): Promise<DaemonHealthResponse> {
    return this.get<DaemonHealthResponse>("/v1/daemon/health");
  }

  async daemonStats(): Promise<DaemonStatsResponse> {
    return this.get<DaemonStatsResponse>("/v1/daemon/stats");
  }

  async daemonCooldowns(): Promise<DaemonCooldownsResponse> {
    return this.get<DaemonCooldownsResponse>("/v1/daemon/cooldowns");
  }

  // ── Daemon: cached-URL index ─────────────────────────────
  // Whole-server console only: the module answers 403
  // "whole_server_console_only" on a per-vhost console, and 404 without a
  // reason code on a module build that predates the leaves — both render
  // as explanations, not errors. An entry is named by path + host + scheme
  // (CacheEntryKey). cache/content serves image bytes and is addressed
  // through daemonCacheContentUrl() as an <img> src, never fetched into JS.

  async daemonCacheUrls(
    offset: number,
    limit: number,
    hostname?: string,
  ): Promise<DaemonCacheUrlsResponse> {
    const query = new URLSearchParams();
    if (offset > 0) query.set("offset", String(offset));
    query.set("limit", String(limit));
    // One host's URLs: the module's allow-listed, validated hostname
    // parameter, filtered by the optimizer itself.
    if (hostname !== undefined) query.set("hostname", hostname);
    return this.get<DaemonCacheUrlsResponse>(
      `/v1/daemon/cache/urls?${query.toString()}`,
    );
  }

  private entryQuery(entry: CacheEntryKey): URLSearchParams {
    return new URLSearchParams({
      url: entry.url,
      hostname: entry.host,
      scheme: entry.scheme,
    });
  }

  async daemonCacheAlternates(entry: CacheEntryKey): Promise<DaemonAlternatesResponse> {
    return this.get<DaemonAlternatesResponse>(
      `/v1/daemon/cache/alternates?${this.entryQuery(entry).toString()}`,
    );
  }

  /**
   * The URL of one variant's bytes, for use as an <img> src only: the bytes
   * pass the module's media-type gate (image/png|jpeg|gif|webp|avif behind
   * nosniff and a sandboxing CSP), so they are never fetched into JS.
   */
  daemonCacheContentUrl(entry: CacheEntryKey, alternateId: number): string {
    const query = this.entryQuery(entry);
    query.set("alternate_id", String(alternateId));
    return `${this.basePath}/v1/daemon/cache/content?${query.toString()}`;
  }

  /**
   * The HTTP status a HEAD for one variant's bytes gets — headers only, no
   * bytes read into JS. The preview queue uses it after an <img> failed to
   * tell "busy" (429, retry) from a permanent failure; 0 when no answer
   * arrives.
   */
  async daemonCacheContentStatus(entry: CacheEntryKey, alternateId: number): Promise<number> {
    try {
      const response = await fetch(this.daemonCacheContentUrl(entry, alternateId), {
        method: "HEAD",
        cache: "no-store",
      });
      return response.status;
    } catch {
      return 0;
    }
  }

  // ── Daemon: log ring ──────────────────────────────────────────
  // Whole-server console only: the module answers 403
  // "whole_server_console_only" on a per-vhost console, 404 on a module
  // without the leaf, 501 "endpoint_unsupported_by_daemon" on an optimizer
  // without the route, and 502 "response_too_large" for an answer over its
  // cap — all render as explanations, not raw errors.

  /**
   * A page of the optimizer's log ring: at most 500 entries (the daemon's
   * maximum page, which it also bounds in bytes). `since` is the last
   * page's `next_since`, passed back verbatim; absent, the newest entries.
   */
  async daemonLogs(since?: number): Promise<DaemonLogsResponse> {
    const query = new URLSearchParams();
    if (since !== undefined) query.set("since", String(since));
    query.set("limit", "500");
    return this.get<DaemonLogsResponse>(`/v1/daemon/logs?${query.toString()}`);
  }
}
