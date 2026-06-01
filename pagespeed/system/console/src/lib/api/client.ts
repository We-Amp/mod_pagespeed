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
  LicenseStatusResponse,
  LicenseApplyResponse,
  ActivateResponse,
  ConsentResponse,
  TimeRangeParams,
} from "./types";

export class ApiError extends Error {
  status: number;
  constructor(status: number, statusText: string) {
    super(`HTTP ${status}: ${statusText}`);
    this.status = status;
    Object.setPrototypeOf(this, ApiError.prototype);
  }
}

export class AdminApiClient {
  private basePath: string;

  constructor(basePath: string) {
    this.basePath = basePath.replace(/\/$/, "");
  }

  private async get<T>(path: string): Promise<T> {
    const url = `${this.basePath}${path}`;
    const response = await fetch(url);

    if (!response.ok) {
      throw new ApiError(response.status, response.statusText);
    }

    // Always try JSON first — the backend may serve JSON with a wrong
    // content-type (e.g. application/javascript instead of application/json).
    const text = await response.text();
    return this.parseResponse<T>(response.status, text);
  }

  private async post<T>(
    path: string,
    body?: Record<string, unknown>,
  ): Promise<T> {
    const url = `${this.basePath}${path}`;
    const response = await fetch(url, {
      method: "POST",
      headers: body
        ? { "Content-Type": "application/json", "X-Requested-With": "XMLHttpRequest" }
        : { "X-Requested-With": "XMLHttpRequest" },
      body: body ? JSON.stringify(body) : undefined,
    });

    if (!response.ok) {
      throw new ApiError(response.status, response.statusText);
    }

    const text = await response.text();
    return this.parseResponse<T>(response.status, text);
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
    const raw = await this.get<RawPurgeSetResponse>("/cache?new_set");
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

  async getMessages(): Promise<MessagesResponse> {
    return this.get<MessagesResponse>("/message_history");
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

  // ── License ────────────────────────────────────────────────

  async getLicenseStatus(): Promise<LicenseStatusResponse> {
    return this.get<LicenseStatusResponse>("/v1/license/status");
  }

  async applyLicense(key: string): Promise<LicenseApplyResponse> {
    return this.post<LicenseApplyResponse>("/v1/license/apply", { key });
  }

  async activateLicense(nonce: string, orderRef?: string): Promise<ActivateResponse> {
    const body: Record<string, unknown> = { nonce };
    if (orderRef) body.order_ref = orderRef;
    return this.post<ActivateResponse>("/v1/license/activate", body);
  }

  async recordConsent(accepted: boolean): Promise<ConsentResponse> {
    return this.post<ConsentResponse>("/v1/license/consent", { accepted });
  }
}
