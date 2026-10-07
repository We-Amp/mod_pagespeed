// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it, vi } from "vitest";
import { ApiError } from "$lib/api/client";
import { NetworkError } from "$lib/api/connection";
import type { DaemonHealthResponse, DaemonStatsResponse, StatsResponse } from "$lib/api/types";
import {
  MIN_OPTIMIZER_VERSION,
  availabilityText,
  countText,
  daemonAvailability,
  daemonServeSavings,
  findingsStampText,
  moduleSummary,
  optimizerFloor,
  optimizerStateText,
  overviewScopeLine,
  sampleOverview,
  toSnapshot,
  variablesOf,
  type OverviewApi,
  type OverviewSample,
} from "./overview";

const STATS: StatsResponse = {
  variables: {
    css_filter_total_bytes_saved: 499345,
    css_filter_total_original_bytes: 1063346,
    javascript_total_bytes_saved: 12087254,
    javascript_total_original_bytes: 25498944,
    image_rewrite_total_bytes_saved: 303593,
    image_rewrite_total_original_bytes: 1116562,
    ipro_daemon_served: 3122,
    ipro_daemon_fallthrough: 8884,
    num_flushes: 3671,
    num_resource_fetch_successes: 108,
    num_resource_fetch_failures: 0,
  },
  maxlength: 60,
  timestamp: 0,
};

function api(over: Partial<OverviewApi> = {}): OverviewApi {
  return {
    getStats: async () => STATS,
    daemonHealth: async () => ({ status: "ok", ready: true, version: "2.0.41" }),
    daemonStats: async () => ({ thread_pool: { inflight: 0, size: 2 } }),
    ...over,
  };
}

const fail = (status: number, code: string) => async (): Promise<never> => {
  throw new ApiError(status, code);
};

describe("sampleOverview", () => {
  it("reads all three and stamps the time", async () => {
    const s = await sampleOverview(api(), () => 42);
    expect(s.at).toBe(42);
    expect(s.module.ok && s.health.ok && s.daemonStats.ok).toBe(true);
  });
  it("never rejects: each failure is carried in the sample", async () => {
    const s = await sampleOverview(
      api({ getStats: fail(500, "boom"), daemonHealth: fail(502, "daemon_unreachable"), daemonStats: async () => { throw "odd"; } }),
    );
    expect(s.module.ok).toBe(false);
    expect(!s.module.ok && s.module.error.message).toBe("HTTP 500: boom");
    expect(!s.daemonStats.ok && s.daemonStats.error.message).toBe("odd");
  });
});

describe("daemonAvailability", () => {
  it("maps the proxy's answers", () => {
    expect(daemonAvailability(new ApiError(503, "daemon_not_configured"))).toBe("not-configured");
    expect(daemonAvailability(new ApiError(404, "Unknown admin page"))).toBe("not-configured");
    expect(daemonAvailability(new ApiError(502, "daemon_unreachable"))).toBe("unreachable");
    expect(daemonAvailability(new ApiError(501, "endpoint_unsupported_by_daemon"))).toBe("unsupported");
  });
  it("classifies 429 and network failures as transient", () => {
    expect(daemonAvailability(new ApiError(429, "in flight"))).toBe("transient");
    expect(daemonAvailability(new ApiError(500, "x"))).toBe("transient");
    expect(daemonAvailability(new TypeError("Failed to fetch"))).toBe("transient");
  });
  it("a forwarded 503 without the proxy's code is not not-configured", () => {
    // Any upstream status other than 404 is forwarded as-is
    // (admin_daemon_handler.h:56); only the module's own 503 carries the code.
    expect(daemonAvailability(new ApiError(503, "Service Unavailable"))).toBe("transient");
  });
});

describe("toSnapshot", () => {
  const sample = (over: Partial<OverviewSample>): OverviewSample => ({
    at: 0,
    module: { ok: true, data: STATS },
    health: { ok: true, data: { status: "ok", version: "2.0.41" } },
    daemonStats: { ok: true, data: {} },
    ...over,
  });
  it("healthy sample", () => {
    const s = toSnapshot(sample({}));
    expect(s.daemon).toBe("ok");
    expect(s.module?.ipro_daemon_served).toBe(3122);
    expect(s.daemonStatsUnsupported).toBe(false);
    expect(s.belowFloor).toBe(false);
  });
  it("below the floor by version: health answers with an older version", () => {
    const s = toSnapshot(sample({ health: { ok: true, data: { status: "ok", version: "2.0.3" } } }));
    expect(s.daemon).toBe("ok");
    expect(s.belowFloor).toBe(true);
    expect(toSnapshot(sample({ health: { ok: true, data: { status: "ok", version: "dev" } } })).belowFloor).toBe(false);
  });
  it("a stats-only 429 carries the previous statistics forward", () => {
    const prev = toSnapshot(sample({ daemonStats: { ok: true, data: { errors: { total: 3 } } } }));
    const busy = { ok: false as const, error: new ApiError(429, "in flight") };
    const s = toSnapshot(sample({ daemonStats: busy }), prev);
    expect(s.daemon).toBe("ok");
    expect(s.daemonStats).toBe(prev.daemonStats);
    // Without a previous snapshot there is nothing to carry.
    expect(toSnapshot(sample({ daemonStats: busy })).daemonStats).toBeNull();
  });
  it("a transient health read keeps a running optimizer running", () => {
    const prev = toSnapshot(sample({}));
    const busy = { ok: false as const, error: new ApiError(429, "in flight") };
    const s = toSnapshot(sample({ health: busy, daemonStats: busy }), prev);
    expect(s.daemon).toBe("ok");
    expect(s.health).toBe(prev.health);
    expect(s.daemonStats).toBe(prev.daemonStats);
    // After an outage the previous view is not "ok": the sample stays transient.
    const down = toSnapshot(sample({
      health: { ok: false, error: new ApiError(502, "daemon_unreachable") },
      daemonStats: { ok: false, error: new ApiError(502, "daemon_unreachable") },
    }));
    expect(toSnapshot(sample({ health: busy }), down).daemon).toBe("transient");
  });
  it("below the floor: health answers, stats is not provided", () => {
    const s = toSnapshot(sample({ daemonStats: { ok: false, error: new ApiError(501, "endpoint_unsupported_by_daemon") } }));
    expect(s.daemon).toBe("ok");
    expect(s.daemonStats).toBeNull();
    expect(s.daemonStatsUnsupported).toBe(true);
  });
  it("absent optimizer: no daemon data, module unaffected", () => {
    const s = toSnapshot(sample({
      health: { ok: false, error: new ApiError(502, "daemon_unreachable") },
      daemonStats: { ok: false, error: new ApiError(502, "daemon_unreachable") },
    }));
    expect(s.daemon).toBe("unreachable");
    expect(s.health).toBeNull();
    expect(s.daemonStatsUnsupported).toBe(false);
    expect(s.module).not.toBeNull();
  });
  it("a non-object daemon body is treated as an empty object", () => {
    const s = toSnapshot(sample({
      health: { ok: true, data: "ok" as unknown as DaemonHealthResponse },
      daemonStats: { ok: true, data: null as unknown as DaemonStatsResponse },
    }));
    expect(s.health).toEqual({});
    expect(s.daemonStats).toEqual({});
  });
  it("failed module read: module is null", () => {
    expect(toSnapshot(sample({ module: { ok: false, error: new Error("x") } })).module).toBeNull();
  });
});

describe("variablesOf", () => {
  it("keeps finite numbers only", () => {
    expect(variablesOf({ variables: { a: 1, b: "2", c: Number.NaN } as unknown as Record<string, number>, maxlength: 0, timestamp: 0 })).toEqual({ a: 1 });
    expect(variablesOf(null)).toEqual({});
  });
});

describe("moduleSummary", () => {
  it("global statistics", () => {
    const m = moduleSummary(variablesOf(STATS));
    expect(m.active).toBe(true);
    expect(m.bytesSaved).toBe(12890192);
    expect(m.originalBytes).toBe(27678852);
    expect(m.savedPercent).toBe(47);
    expect(m.servedByOptimizer).toBe(3122);
    expect(m.inPlaceRequests).toBe(12006);
    expect(m.fetchFailures).toBe(0);
  });
  it("missing counters read as null, not zero", () => {
    const m = moduleSummary({ num_flushes: 12 });
    expect(m.active).toBe(true);
    expect(m.bytesSaved).toBeNull();
    expect(m.savedPercent).toBeNull();
    expect(m.servedByOptimizer).toBeNull();
    expect(m.inPlaceRequests).toBeNull();
    expect(m.fetchFailures).toBeNull();
  });
  it("empty statistics: not active, zeros are real zeros", () => {
    const zeros = Object.fromEntries(Object.keys(STATS.variables).map((k) => [k, 0]));
    const m = moduleSummary(zeros);
    expect(m.active).toBe(false);
    expect(m.bytesSaved).toBe(0);
    expect(m.savedPercent).toBeNull();
  });
  it("net savings are clamped at zero", () => {
    const m = moduleSummary({ css_filter_total_bytes_saved: -50, css_filter_total_original_bytes: 100 });
    expect(m.bytesSaved).toBe(0);
    expect(m.savedPercent).toBe(0);
  });
});

describe("daemonServeSavings", () => {
  it("sums every family; zero families add nothing", () => {
    const s = daemonServeSavings({
      serve_savings: {
        css: { hits: 197, optimized_bytes: 21447548, original_bytes: 21472396 },
        html: { hits: 0, optimized_bytes: 0, original_bytes: 0 },
        image: { hits: 45, optimized_bytes: 258379, original_bytes: 922228 },
        js: { hits: 0, optimized_bytes: 0, original_bytes: 0 },
      },
    });
    expect(s).toEqual({ saved: 688697, original: 22394624, percent: 3 });
  });
  it("null when nothing was served, the block is missing or malformed", () => {
    expect(daemonServeSavings({ serve_savings: { css: { original_bytes: 0, optimized_bytes: 0 } } })).toBeNull();
    expect(daemonServeSavings({})).toBeNull();
    expect(daemonServeSavings(null)).toBeNull();
    expect(daemonServeSavings({ serve_savings: { css: "x" } })).toBeNull();
  });
  it("never negative", () => {
    expect(daemonServeSavings({ serve_savings: { css: { original_bytes: 10, optimized_bytes: 30 } } })).toEqual({ saved: 0, original: 10, percent: 0 });
  });
  it("saved stays within the original even if the daemon reports a negative optimized_bytes", () => {
    expect(daemonServeSavings({ serve_savings: { css: { original_bytes: 10, optimized_bytes: -1000 } } })).toEqual({
      saved: 10,
      original: 10,
      percent: 100,
    });
  });
});

describe("texts", () => {
  it("availabilityText names every state", () => {
    expect(availabilityText("not-configured").word).toBe("Not configured");
    expect(availabilityText("unreachable").word).toBe("Unreachable");
    expect(availabilityText("unsupported").word).toBe("Outdated");
    expect(availabilityText("transient").word).toBe("Checking");
    expect(availabilityText("ok").word).toBe("Running");
  });
  it("optimizerStateText", () => {
    expect(optimizerStateText({ status: "ok", ready: true })).toEqual({ word: "Running", ok: true, busy: false });
    // ready:false means every worker thread is busy, not "starting".
    expect(optimizerStateText({ status: "ok", ready: false })).toEqual({ word: "Running", ok: true, busy: true });
    expect(optimizerStateText({ status: "degraded", ready: false })).toEqual({ word: "Status: degraded", ok: false, busy: true });
    expect(optimizerStateText({})).toEqual({ word: "Status: unknown", ok: false, busy: false });
    expect(optimizerStateText(null)).toEqual({ word: "Unknown", ok: false, busy: false });
  });
  it("optimizerStateText caps an unbroken status string", () => {
    const long = "x".repeat(200);
    const r = optimizerStateText({ status: long, ready: true });
    expect(r.word).toBe(`Status: ${"x".repeat(80)}…`);
  });
  it("optimizerFloor: at, above, below and unparsable versions", () => {
    expect(MIN_OPTIMIZER_VERSION).toBe("2.0.41");
    expect(optimizerFloor("2.0.41")).toBe("at-or-above");
    expect(optimizerFloor("v2.0.41-rc.1")).toBe("at-or-above");
    expect(optimizerFloor("2.0.42")).toBe("at-or-above");
    expect(optimizerFloor("2.1.0")).toBe("at-or-above");
    expect(optimizerFloor("2.0.3")).toBe("below");
    expect(optimizerFloor("1.99.99")).toBe("below");
    expect(optimizerFloor("dev")).toBe("unknown");
    expect(optimizerFloor("")).toBe("unknown");
    expect(optimizerFloor(undefined)).toBe("unknown");
    expect(optimizerFloor(2)).toBe("unknown");
    expect(optimizerFloor("2.0.41", "not-a-version")).toBe("unknown");
  });
  it("countText renders a dash for null", () => {
    expect(countText(null)).toBe("—");
    expect(countText(0)).toBe("0");
  });
  it("overviewScopeLine prefers the configuration's own scope over the admin path", () => {
    expect(overviewScopeLine(false, { scope: "global", host: "" })).toBe("All virtual hosts (the whole server)");
    expect(overviewScopeLine(true, null)).toBe("All virtual hosts (the whole server)");
    expect(overviewScopeLine(true, { scope: "vhost", host: "www.example.test:80" })).toBe(
      "This virtual host (www.example.test:80): separate from other hosts only when per-virtual-host statistics are enabled",
    );
    expect(overviewScopeLine(false, null)).toBe(
      "This virtual host: separate from other hosts only when per-virtual-host statistics are enabled",
    );
  });
});

describe("the message log in the sample", () => {
  const sample = (over: Partial<OverviewSample>): OverviewSample => ({
    at: 0,
    module: { ok: true, data: STATS },
    health: { ok: true, data: { status: "ok", version: "2.0.41" } },
    daemonStats: { ok: true, data: {} },
    ...over,
  });

  it("sampleOverview reads the grouped log over the last 15 minutes when the api can", async () => {
    const getMessageGroups = vi.fn(async (_w: number) => ({ groups: [] }));
    const s = await sampleOverview({ ...api(), getMessageGroups }, () => 42);
    expect(getMessageGroups).toHaveBeenCalledWith(900);
    expect(s.messages?.ok).toBe(true);
    expect((await sampleOverview(api())).messages).toBeUndefined();
  });

  it("toSnapshot turns either answer into a digest; no read is no digest", () => {
    const grouped = toSnapshot(sample({ messages: { ok: true, data: { groups: [{ level: "warning", template: "t", count: 1, last_ms: 5 }] } } }));
    expect(grouped.messages?.source).toBe("module");
    expect(toSnapshot(sample({ messages: { ok: true, data: { messages: [] } } })).messages?.source).toBe("console");
    expect(toSnapshot(sample({})).messages).toBeUndefined();
  });

  it("a busy or unanswered log read carries the previous digest; any other failure is no data", () => {
    const prev = toSnapshot(sample({ messages: { ok: true, data: { groups: [] } } }));
    expect(toSnapshot(sample({ messages: { ok: false, error: new ApiError(429, "busy") } }), prev).messages).toBe(prev.messages);
    expect(toSnapshot(sample({ messages: { ok: false, error: new NetworkError() } }), prev).messages).toBe(prev.messages);
    expect(toSnapshot(sample({ messages: { ok: false, error: new ApiError(404, "Unknown admin page") } }), prev).messages).toBeNull();
    expect(toSnapshot(sample({ messages: { ok: false, error: new ApiError(429, "busy") } })).messages).toBeNull();
  });

  it("stamps the sample time and the build, and remembers the last outage", () => {
    const down = toSnapshot(
      sample({
        at: 100,
        health: { ok: false, error: new ApiError(502, "daemon_unreachable") },
        daemonStats: { ok: false, error: new ApiError(502, "daemon_unreachable") },
      }),
      null,
      "v1.16.0-dirty",
    );
    expect(down.at).toBe(100);
    expect(down.buildStamp).toBe("v1.16.0-dirty");
    expect(down.daemonDownAt).toBe(100);
    expect(toSnapshot(sample({ at: 200 }), down).daemonDownAt).toBe(100);
    expect(toSnapshot(sample({ at: 300 })).daemonDownAt).toBeNull();
  });
});

describe("findingsStampText", () => {
  const since = (text: string) => ({ text, iso: null, warming: false });
  it("names both time bases, either one, or nothing", () => {
    expect(findingsStampText(since("since 3 h"), since("since 2 min"))).toBe(
      "Counters: module since 3 h · optimizer since 2 min",
    );
    expect(findingsStampText(since("since 3 h"), null)).toBe("Counters: module since 3 h");
    expect(findingsStampText(null, since("since restart"))).toBe("Counters: optimizer since restart");
    expect(findingsStampText(null, null)).toBe("");
  });
});
