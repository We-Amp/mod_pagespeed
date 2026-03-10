import type {
  StatsResponse,
  ConfigResponse,
  HistogramsResponse,
  CacheEntryResponse,
  CacheStructureResponse,
  PhysicalCachesResponse,
  PurgeResponse,
  PurgeSetResponse,
  ConsoleResponse,
  MessagesResponse,
  LicenseStatusResponse,
  LicenseApplyResponse,
  ActivateResponse,
  TimeRangeParams,
} from "./types";

export class AdminApiClient {
  private basePath: string;

  constructor(basePath: string) {
    this.basePath = basePath.replace(/\/$/, "");
  }

  private async get<T>(path: string): Promise<T> {
    const url = `${this.basePath}${path}`;
    const response = await fetch(url);

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`);
    }

    // Always try JSON first — the backend may serve JSON with a wrong
    // content-type (e.g. application/javascript instead of application/json).
    const text = await response.text();
    return this.parseResponse<T>(text);
  }

  private async post<T>(
    path: string,
    body?: Record<string, unknown>,
  ): Promise<T> {
    const url = `${this.basePath}${path}`;
    const response = await fetch(url, {
      method: "POST",
      headers: body ? { "Content-Type": "application/json" } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`);
    }

    const text = await response.text();
    return this.parseResponse<T>(text);
  }

  /**
   * Parse a response body as JSON, stripping any XSSI protection prefix
   * (e.g. ")]}'\n" or ")]}\n") that some endpoints prepend.
   */
  private parseResponse<T>(text: string): T {
    // Strip XSSI protection prefix if present.
    const stripped = text.replace(/^\)\]\}'?\s*\n/, "");
    try {
      return JSON.parse(stripped) as T;
    } catch {
      return { raw: text } as unknown as T;
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
    return this.get<PurgeSetResponse>("/cache?purge_set");
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

  async activateLicense(key: string): Promise<ActivateResponse> {
    return this.post<ActivateResponse>("/v1/license/activate", { key });
  }

  async startTrial(): Promise<ActivateResponse> {
    return this.post<ActivateResponse>("/v1/license/trial");
  }

  async recordConsent(accepted: boolean): Promise<LicenseApplyResponse> {
    return this.post<LicenseApplyResponse>("/v1/license/consent", {
      accepted,
    });
  }
}
