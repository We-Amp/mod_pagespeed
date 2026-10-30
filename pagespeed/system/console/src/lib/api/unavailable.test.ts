// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { AdminApiClient, ApiError, UnavailableError, isUnavailable } from "./client";
import { NetworkError, connection } from "./connection";
import { Poller } from "./poller";
import { unavailable } from "./unavailable";

function reply(status: number, body: string): Response {
  return new Response(body, { status });
}

let fetchMock: ReturnType<typeof vi.fn>;

beforeEach(() => {
  fetchMock = vi.fn();
  vi.stubGlobal("fetch", fetchMock);
  connection.reset();
  unavailable.reset();
});
afterEach(() => {
  vi.unstubAllGlobals();
});

describe("a leaf the server has turned off", () => {
  for (const status of [401, 403, 410]) {
    it(`${status} on a module leaf is an UnavailableError, remembered, and not asked again`, async () => {
      fetchMock.mockImplementation(async () => reply(status, "<html>Forbidden</html>"));
      const api = new AdminApiClient("/pagespeed_admin");
      const err = await api.getConfig().catch((e: unknown) => e);
      expect(isUnavailable(err)).toBe(true);
      expect(err).toBeInstanceOf(ApiError);
      expect((err as UnavailableError).status).toBe(status);
      expect((err as UnavailableError).leaf).toBe("/config");
      expect(unavailable.has("/config")).toBe(true);
      const again = await api.getConfig().catch((e: unknown) => e);
      expect(isUnavailable(again)).toBe(true);
      expect(fetchMock).toHaveBeenCalledTimes(1);
    });
  }

  it("a leaf is remembered under its path, whatever the query", async () => {
    fetchMock.mockImplementation(async () => reply(403, ""));
    const api = new AdminApiClient("");
    await api.daemonLogs().catch(() => {});
    await api.daemonLogs(42).catch(() => {});
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(unavailable.has("/v1/daemon/logs")).toBe(true);
  });

  it("a refusal of one entry on a keyed leaf is shown but not remembered", async () => {
    const api = new AdminApiClient("");
    fetchMock.mockImplementationOnce(async () => reply(403, ""));
    expect(isUnavailable(await api.getCacheEntry("http://a/").catch((e: unknown) => e))).toBe(true);
    fetchMock.mockImplementationOnce(async () => new Response("{}", { status: 200 }));
    await expect(api.getCacheEntry("http://b/")).resolves.toEqual({});
    const key = { url: "/a.png", host: "h", scheme: "https" } as const;
    fetchMock.mockImplementationOnce(async () => reply(403, ""));
    expect(isUnavailable(await api.daemonCacheAlternates(key).catch((e: unknown) => e))).toBe(true);
    fetchMock.mockImplementationOnce(async () => new Response("{}", { status: 200 }));
    await expect(api.daemonCacheAlternates({ ...key, url: "/b.png" })).resolves.toEqual({});
    expect(unavailable.has("/cache")).toBe(false);
    expect(unavailable.has("/v1/daemon/cache/alternates")).toBe(false);
  });

  it("the optimizer's own object-shaped error is a coded refusal, not 'off'", async () => {
    fetchMock.mockImplementation(async () =>
      reply(403, JSON.stringify({ error: { code: "FORBIDDEN", message: "no" } })),
    );
    const err = await new AdminApiClient("").daemonCacheUrls(0, 10).catch((e: unknown) => e);
    expect(err).toBeInstanceOf(ApiError);
    expect(isUnavailable(err)).toBe(false);
    expect(unavailable.has("/v1/daemon/cache/urls")).toBe(false);
  });

  it("the remembered set is cleared when the server comes back after an outage", async () => {
    unavailable.mark("/config", 403);
    connection.record("/stats_json", null, 1);
    expect(unavailable.has("/config")).toBe(true);
    connection.record("/stats_json", 200, 2);
    expect(unavailable.has("/config")).toBe(false);
  });

  it("404 on a module leaf is unavailable but not remembered", async () => {
    fetchMock.mockImplementation(async () => reply(404, "not found"));
    const api = new AdminApiClient("");
    const err = await api.getMessages().catch((e: unknown) => e);
    expect(isUnavailable(err)).toBe(true);
    expect(unavailable.has("/message_history")).toBe(false);
  });

  it("404 on a daemon leaf stays a plain ApiError (its own explanation)", async () => {
    fetchMock.mockImplementation(async () => reply(404, ""));
    const err = await new AdminApiClient("").daemonCacheUrls(0, 10).catch((e: unknown) => e);
    expect(err).toBeInstanceOf(ApiError);
    expect(isUnavailable(err)).toBe(false);
  });

  it("the module's own coded refusals keep their meaning", async () => {
    fetchMock.mockImplementation(async () => reply(403, JSON.stringify({ error: "whole_server_console_only" })));
    const err = await new AdminApiClient("").daemonCacheUrls(0, 10).catch((e: unknown) => e);
    expect(isUnavailable(err)).toBe(false);
    expect((err as ApiError).message).toContain("whole_server_console_only");
    expect(unavailable.has("/v1/daemon/cache/urls")).toBe(false);
  });

  it("5xx and no answer are not 'off'", async () => {
    const api = new AdminApiClient("");
    fetchMock.mockImplementationOnce(async () => reply(500, "boom"));
    expect(isUnavailable(await api.getConfig().catch((e: unknown) => e))).toBe(false);
    fetchMock.mockImplementationOnce(async () => reply(503, "boom"));
    expect(isUnavailable(await api.getConfig().catch((e: unknown) => e))).toBe(false);
    fetchMock.mockImplementationOnce(async () => {
      throw new TypeError("failed");
    });
    expect(await api.getConfig().catch((e: unknown) => e)).toBeInstanceOf(NetworkError);
    expect(unavailable.has("/config")).toBe(false);
  });

  it("a refusal does not count against the connection", async () => {
    fetchMock.mockImplementation(async () => reply(403, ""));
    await new AdminApiClient("").getConfig().catch(() => {});
    expect(connection.view.status).toBe("connected");
    expect(connection.view.failures).toBe(0);
  });

  it("a refused POST is a plain ApiError and marks nothing", async () => {
    fetchMock.mockImplementation(async () => reply(403, ""));
    const err = await new AdminApiClient("").purgeUrl("http://a/").catch((e: unknown) => e);
    expect(isUnavailable(err)).toBe(false);
    expect(unavailable.has("/cache")).toBe(false);
  });

  it("allOff needs every leaf", () => {
    unavailable.mark("/message_history", 403);
    expect(unavailable.allOff(["/message_history", "/v1/daemon/logs"])).toBe(false);
    unavailable.mark("/v1/daemon/logs", 403);
    expect(unavailable.allOff(["/message_history", "/v1/daemon/logs"])).toBe(true);
    expect(unavailable.allOff(undefined)).toBe(false);
  });
});

describe("the poller and a leaf that is off", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("keeps the error as state, counts no failure, and schedules nothing", async () => {
    const fetcher = vi.fn(async () => {
      throw new UnavailableError(403, "Forbidden", "/rule_packs");
    });
    const p = new Poller(fetcher, { intervalMs: 5000, visibility: null });
    p.start();
    await vi.advanceTimersByTimeAsync(0);
    expect(p.snapshot.loading).toBe(false);
    expect(p.snapshot.failures).toBe(0);
    expect(p.snapshot.error).toBeInstanceOf(UnavailableError);
    expect(p.snapshot.nextAt).toBeNull();
    await vi.advanceTimersByTimeAsync(10 * 60_000);
    expect(fetcher).toHaveBeenCalledTimes(1);
    p.dispose();
  });
});
