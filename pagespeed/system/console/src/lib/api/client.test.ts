// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { AdminApiClient, ApiError, REQUEST_TIMEOUT_MS } from "./client";
import { NetworkError, connection } from "./connection";

function json(status: number, body: unknown): Response {
  return new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
}

let fetchMock: ReturnType<typeof vi.fn>;

beforeEach(() => {
  fetchMock = vi.fn();
  vi.stubGlobal("fetch", fetchMock);
  connection.reset();
});
afterEach(() => {
  vi.unstubAllGlobals();
});

describe("AdminApiClient", () => {
  it("exposes its base path without a trailing slash", () => {
    expect(new AdminApiClient("/pagespeed_admin/").basePath).toBe("/pagespeed_admin");
  });

  it("concurrent identical GETs share one request, across client instances", async () => {
    let answer!: (r: Response) => void;
    fetchMock.mockReturnValueOnce(new Promise<Response>((r) => (answer = r)));
    const p1 = new AdminApiClient("/pagespeed_admin").daemonStats();
    const p2 = new AdminApiClient("/pagespeed_admin/").daemonStats();
    answer(json(200, { cache: { entries: 1 } }));
    const [a, b] = await Promise.all([p1, p2]);
    expect(a).toEqual({ cache: { entries: 1 } });
    expect(b).toBe(a);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it("a settled GET is not reused: the next call asks again", async () => {
    fetchMock.mockImplementation(async () => json(200, { variables: {} }));
    const api = new AdminApiClient("");
    await api.getStats();
    await api.getStats();
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it("different URLs are different requests", async () => {
    fetchMock.mockImplementation(async () => json(200, {}));
    const api = new AdminApiClient("");
    await Promise.all([api.daemonHealth(), api.daemonStats(), api.getMessages(3), api.getMessages(4)]);
    expect(fetchMock).toHaveBeenCalledTimes(4);
  });

  it("daemonLogs asks for a full page, with the cursor when it has one", async () => {
    fetchMock.mockImplementation(async () => json(200, { entries: [] }));
    const api = new AdminApiClient("/pagespeed_global_admin");
    await api.daemonLogs();
    await api.daemonLogs(42);
    expect(fetchMock.mock.calls.map((c) => String(c[0]))).toEqual([
      "/pagespeed_global_admin/v1/daemon/logs?limit=500",
      "/pagespeed_global_admin/v1/daemon/logs?since=42&limit=500",
    ]);
  });

  it("a shared GET that fails rejects every caller", async () => {
    fetchMock.mockResolvedValueOnce(json(502, { error: "daemon_unreachable" }));
    const api = new AdminApiClient("");
    const results = await Promise.allSettled([api.daemonHealth(), api.daemonHealth()]);
    expect(results.map((r) => r.status)).toEqual(["rejected", "rejected"]);
    expect((results[0] as PromiseRejectedResult).reason).toBeInstanceOf(ApiError);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it("POSTs are never shared", async () => {
    fetchMock.mockImplementation(async () => json(200, { success: true }));
    const api = new AdminApiClient("");
    await Promise.all([api.purgeUrl("https://www.example.test/a.css"), api.purgeUrl("https://www.example.test/a.css")]);
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it("a network failure becomes NetworkError and marks the connection lost", async () => {
    fetchMock.mockRejectedValueOnce(new TypeError("Failed to fetch"));
    await expect(new AdminApiClient("").getStats()).rejects.toBeInstanceOf(NetworkError);
    expect(connection.view.status).toBe("reconnecting");
  });

  it("any HTTP answer marks the connection live, even an error status", async () => {
    fetchMock.mockRejectedValueOnce(new TypeError("Failed to fetch"));
    fetchMock.mockResolvedValueOnce(json(500, { error: "statistics unavailable" }));
    const api = new AdminApiClient("");
    await expect(api.getStats()).rejects.toBeInstanceOf(NetworkError);
    await expect(api.getStats()).rejects.toThrow("statistics unavailable");
    expect(connection.view.status).toBe("connected");
  });

  it("a gateway error on a module leaf marks the connection lost", async () => {
    fetchMock.mockResolvedValueOnce(new Response("<html>Bad Gateway</html>", { status: 504 }));
    await expect(new AdminApiClient("").getStats()).rejects.toBeInstanceOf(ApiError);
    expect(connection.view.status).toBe("reconnecting");
  });

  it("a request that never answers is aborted after the timeout and reported as no answer", async () => {
    vi.useFakeTimers();
    try {
      fetchMock.mockImplementation(
        (_url: string, init?: RequestInit) =>
          new Promise<Response>((_resolve, reject) => {
            init?.signal?.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")));
          }),
      );
      const pending = new AdminApiClient("").getStats();
      const outcome = expect(pending).rejects.toBeInstanceOf(NetworkError);
      await vi.advanceTimersByTimeAsync(REQUEST_TIMEOUT_MS - 1);
      expect(connection.view.status).toBe("connected");
      await vi.advanceTimersByTimeAsync(1);
      await outcome;
      expect(connection.view.status).toBe("reconnecting");
    } finally {
      vi.useRealTimers();
    }
  });

  it("headers that arrive but a body that stalls is also aborted after the timeout", async () => {
    vi.useFakeTimers();
    try {
      let errorBody!: (err: unknown) => void;
      fetchMock.mockImplementation((_url: string, init?: RequestInit) => {
        const body = new ReadableStream<Uint8Array>({
          start(controller) {
            errorBody = (err) => controller.error(err);
          },
        });
        init?.signal?.addEventListener("abort", () => errorBody(new DOMException("aborted", "AbortError")));
        return Promise.resolve(new Response(body, { status: 200 }));
      });
      const pending = new AdminApiClient("").getStats();
      const outcome = expect(pending).rejects.toBeInstanceOf(NetworkError);
      await vi.advanceTimersByTimeAsync(REQUEST_TIMEOUT_MS - 1);
      expect(connection.view.status).toBe("connected");
      await vi.advanceTimersByTimeAsync(1);
      await outcome;
      expect(connection.view.status).toBe("reconnecting");
    } finally {
      vi.useRealTimers();
    }
  });

  it("asks for the grouped message log with a window", async () => {
    fetchMock.mockResolvedValueOnce(json(200, { groups: [] }));
    await new AdminApiClient("/pagespeed_admin").getMessageGroups(900);
    expect(String(fetchMock.mock.calls[0][0])).toBe("/pagespeed_admin/message_history?grouped=1&window_s=900");
  });
});

describe("daemonCacheUrls", () => {
  it("asks for one host's URLs through the hostname parameter, encoded", async () => {
    fetchMock.mockImplementation(() => Promise.resolve(json(200, { urls: [] })));
    const api = new AdminApiClient("/pagespeed_global_admin");
    await api.daemonCacheUrls(50, 50, "www.example.test");
    expect(fetchMock.mock.calls[0][0]).toBe(
      "/pagespeed_global_admin/v1/daemon/cache/urls?offset=50&limit=50&hostname=www.example.test",
    );
    await api.daemonCacheUrls(0, 50);
    expect(fetchMock.mock.calls[1][0]).toBe("/pagespeed_global_admin/v1/daemon/cache/urls?limit=50");
  });
});
