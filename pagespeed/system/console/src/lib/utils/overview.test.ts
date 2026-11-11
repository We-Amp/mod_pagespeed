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
  optimizerServes,
  optimizerStateText,
  overviewScopeLine,
  overviewVerdict,
  serveShareJudgement,
  timeBase,
  verdictLevel,
  findingsMark,
  sampleOverview,
  toSnapshot,
  unknownHealth,
  variablesOf,
  type OverviewApi,
  type OverviewSample,
  type VerdictInput,
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

describe("unknownHealth", () => {
  it("claims no verdict and names the time of the last answer", () => {
    expect(unknownHealth(1_800_000_000_000, () => "10:31:15")).toEqual({
      text: "Status unknown. Last answer 10:31:15",
      level: "unknown",
    });
  });
  it("says so when the server never answered", () => {
    expect(unknownHealth(null)).toEqual({ text: "Status unknown. The server has not answered", level: "unknown" });
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
    expect(s).toMatchObject({ saved: 688697, original: 22394624, percent: 3 });
  });
  it("null when nothing was served, the block is missing or malformed", () => {
    expect(daemonServeSavings({ serve_savings: { css: { original_bytes: 0, optimized_bytes: 0 } } })).toBeNull();
    expect(daemonServeSavings({})).toBeNull();
    expect(daemonServeSavings(null)).toBeNull();
    expect(daemonServeSavings({ serve_savings: { css: "x" } })).toBeNull();
  });
  it("a grown class reads negative, with the percent at zero", () => {
    expect(daemonServeSavings({ serve_savings: { css: { original_bytes: 10, optimized_bytes: 30 } } })).toMatchObject({ saved: -20, original: 10, percent: 0 });
  });
  it("saved stays within the original even if the daemon reports a negative optimized_bytes", () => {
    expect(daemonServeSavings({ serve_savings: { css: { original_bytes: 10, optimized_bytes: -1000 } } })).toMatchObject({
      saved: 10,
      original: 10,
      percent: 100,
    });
  });
});

describe("texts", () => {
  it("availabilityText names every state", () => {
    expect(availabilityText("not-configured").word).toBe("Not configured");
    expect(availabilityText("unreachable").word).toBe("Not answering");
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

  it("sampleOverview leaves the log alone for a page that shows no findings", async () => {
    const getMessageGroups = vi.fn(async (_w: number) => ({ groups: [] }));
    const s = await sampleOverview({ ...api(), getMessageGroups }, () => 42, { messages: false });
    expect(getMessageGroups).not.toHaveBeenCalled();
    expect(s.messages).toBeUndefined();
    expect(s.module.ok && s.health.ok && s.daemonStats.ok).toBe(true);
    expect(toSnapshot(s).messages).toBeUndefined();
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

// The verdict sentence: one per case, every number with the scope and the
// time base of its own source, and never a number from a failed read.
describe("overviewVerdict", () => {
  const NOW = Date.UTC(2026, 0, 15, 12, 0, 0);
  const HOUR = 3_600_000;
  const MIN = 60_000;
  const active = moduleSummary({
    css_filter_total_bytes_saved: 1024 * 281,
    css_filter_total_original_bytes: 1024 * 1500,
    num_flushes: 40,
  });
  const idle = moduleSummary({ num_flushes: 0 });
  const clock = () => "10:25:51";

  function input(patch: Partial<VerdictInput> = {}): VerdictInput {
    return {
      wholeServer: true,
      at: NOW,
      down: false,
      lastAnswerAt: NOW,
      module: { summary: active, startMs: NOW - 26 * HOUR },
      optimizer: {
        availability: "ok",
        stateWord: "Running",
        serves: { responses: 242, saved: 1024 * 673 },
        startMs: NOW - 3 * HOUR,
      },
      ...patch,
    };
  }
  const optimizer = (patch: Partial<VerdictInput["optimizer"]>): VerdictInput["optimizer"] => ({ ...input().optimizer, ...patch });
  const moduleOnly = optimizer({ availability: "not-configured", stateWord: "", serves: null, startMs: null });

  it("whole server, optimizer running: its serves and savings since the optimizer started, and they lead", () => {
    const v = overviewVerdict(input());
    expect(v.text).toBe(
      "Working. 242 optimized responses served from the optimizer's cache, 673 KB saved since the optimizer started 3 h ago.",
    );
    expect(v).toMatchObject({ state: "working", word: "Working", lead: "optimizer" });
    // The module's rewrite figure is not added to it, and not mentioned as a sum.
    expect(v.text).not.toContain("281 KB");
  });

  it("whole server: one response is singular, and served responses that grew state no saving", () => {
    expect(overviewVerdict(input({ optimizer: optimizer({ serves: { responses: 1, saved: 10 } }) })).text).toContain(
      "1 optimized response served from the optimizer's cache, 10.0 B saved",
    );
    expect(overviewVerdict(input({ optimizer: optimizer({ serves: { responses: 5, saved: -40 } }) })).text).toBe(
      "Working. 5 optimized responses served from the optimizer's cache since the optimizer started 3 h ago.",
    );
  });

  it("per-host, optimizer running: this site's rewriting since the web server started; the optimizer by state only", () => {
    const v = overviewVerdict(input({ wholeServer: false }));
    expect(v.text).toBe(
      "Working. This site: 281 KB saved by rewriting since the web server started 1 d ago. Optimizer (whole server): running.",
    );
    expect(v.lead).toBe("module");
    // The optimizer's whole-server figures are never stated as this site's.
    expect(v.text).not.toContain("242");
    expect(v.text).not.toContain("673 KB");
  });

  it("per-host: an optimizer state other than running is named as it reports it", () => {
    const v = overviewVerdict(input({ wholeServer: false, optimizer: optimizer({ stateWord: "Status: degraded" }) }));
    expect(v.text).toContain("Optimizer (whole server): Status: degraded.");
  });

  it("module-only: the rewriting figure since the web server started, on either console", () => {
    expect(overviewVerdict(input({ optimizer: moduleOnly }))).toMatchObject({
      text: "Working. 281 KB saved by rewriting since the web server started 1 d ago.",
      lead: "module",
    });
    expect(overviewVerdict(input({ wholeServer: false, optimizer: moduleOnly })).text).toBe(
      "Working. This site: 281 KB saved by rewriting since the web server started 1 d ago.",
    );
  });

  it("an optimizer that does not answer is said, without a figure of its own", () => {
    const down = optimizer({ availability: "unreachable", stateWord: "", serves: null, startMs: null });
    expect(overviewVerdict(input({ optimizer: down })).text).toBe(
      "Working. 281 KB saved by rewriting since the web server started 1 d ago. The optimizer is not answering.",
    );
    expect(overviewVerdict(input({ wholeServer: false, optimizer: down })).text).toBe(
      "Working. This site: 281 KB saved by rewriting since the web server started 1 d ago. Optimizer (whole server): not answering.",
    );
    // A busy refresh or an outdated optimizer adds nothing: the findings name those.
    for (const availability of ["transient", "unsupported"] as const) {
      expect(overviewVerdict(input({ optimizer: optimizer({ availability, serves: null }) })).text).toBe(
        "Working. 281 KB saved by rewriting since the web server started 1 d ago.",
      );
    }
  });

  it("one source warming: the warm source keeps its figure, the other says it restarted", () => {
    // The optimizer restarted 4 minutes ago; the module has been up for a day.
    const optimizerWarm = overviewVerdict(input({ optimizer: optimizer({ startMs: NOW - 4 * MIN }) }));
    expect(optimizerWarm.text).toBe(
      "Working. 281 KB saved by rewriting since the web server started 1 d ago. Optimizer restarted 4 min ago; its figures are not meaningful yet.",
    );
    expect(optimizerWarm.state).toBe("working");
    // The mirror: the web server reloaded 4 minutes ago; the optimizer has been up for hours.
    const moduleWarm = overviewVerdict(input({ module: { summary: active, startMs: NOW - 4 * MIN } }));
    expect(moduleWarm.text).toBe(
      "Working. 242 optimized responses served from the optimizer's cache, 673 KB saved since the optimizer started 3 h ago. " +
        "Web server restarted 4 min ago; the module's figures are not meaningful yet.",
    );
    expect(moduleWarm.state).toBe("working");
  });

  it("per-host: a warming optimizer does not make this site warm", () => {
    const v = overviewVerdict(input({ wholeServer: false, optimizer: optimizer({ startMs: NOW - 4 * MIN }) }));
    expect(v.text).toBe(
      "Working. This site: 281 KB saved by rewriting since the web server started 1 d ago. " +
        "Optimizer (whole server): running, restarted 4 min ago.",
    );
  });

  it("every source warming: no figure at all, and the most recent restart is the one named", () => {
    const v = overviewVerdict(
      input({ module: { summary: active, startMs: NOW - 9 * MIN }, optimizer: optimizer({ startMs: NOW - 4 * MIN }) }),
    );
    expect(v).toMatchObject({
      state: "warming",
      word: "Warming up",
      text: "Warming up. Counters restarted 4 min ago; figures are not meaningful yet.",
    });
    expect(v.text).not.toMatch(/KB|MB|242/);
    // Module-only: the module is the one source.
    expect(overviewVerdict(input({ module: { summary: active, startMs: NOW - 30_000 }, optimizer: moduleOnly })).text).toBe(
      "Warming up. Counters restarted less than a minute ago; figures are not meaningful yet.",
    );
    // Per-host: the site's counters, and the optimizer still by state only.
    expect(overviewVerdict(input({ wholeServer: false, module: { summary: active, startMs: NOW - 9 * MIN } })).text).toBe(
      "Warming up. This site's counters restarted 9 min ago; figures are not meaningful yet. Optimizer (whole server): running.",
    );
  });

  it("idle: running with no traffic, since the start; never a zero dressed up as a saving", () => {
    const v = overviewVerdict(input({ module: { summary: idle, startMs: NOW - 2 * HOUR }, optimizer: moduleOnly }));
    expect(v).toMatchObject({
      state: "idle",
      word: "Running",
      text: "Running, no traffic seen yet since the web server started 2 h ago.",
      lead: null,
    });
    // A fresh install is idle before it is warming up: the next steps matter more.
    expect(overviewVerdict(input({ module: { summary: idle, startMs: NOW - MIN }, optimizer: moduleOnly })).state).toBe("idle");
    // An optimizer that served nothing does not change that; one that served does, on the whole server.
    expect(
      overviewVerdict(input({ module: { summary: idle, startMs: NOW - 2 * HOUR }, optimizer: optimizer({ serves: null }) })).state,
    ).toBe("idle");
    expect(overviewVerdict(input({ module: { summary: idle, startMs: NOW - 2 * HOUR } })).state).toBe("working");
    // On a per-host console the whole server's serves are not this site's traffic.
    expect(overviewVerdict(input({ wholeServer: false, module: { summary: idle, startMs: NOW - 2 * HOUR } })).text).toBe(
      "Running, no traffic seen yet since the web server started 2 h ago. Optimizer (whole server): running.",
    );
  });

  it("disconnected: status unknown with the time of the last answer, and no figure from before", () => {
    const v = overviewVerdict(input({ down: true, lastAnswerAt: NOW }), clock);
    expect(v).toMatchObject({ state: "unknown", word: "Status unknown", text: "Status unknown. Last answer 10:25:51.", lead: null });
    expect(overviewVerdict(input({ down: true, lastAnswerAt: null }), clock).text).toBe(
      "Status unknown. The server has not answered.",
    );
  });

  it("a failed module read states no number, not even the optimizer's", () => {
    const v = overviewVerdict(input({ module: null }));
    expect(v).toMatchObject({ state: "unknown", text: "Status unknown.", lead: null });
    expect(v.text).not.toMatch(/\d/);
  });

  it("a failed module read over figures still on the page says why, and from when they are", () => {
    const at = new Date(2026, 0, 15, 10, 25, 51).getTime();
    const v = overviewVerdict(input({ module: null, staleFiguresAt: at }));
    expect(v).toMatchObject({
      state: "unknown",
      text: "Status unknown. The module statistics could not be read; the figures are from 10:25:51.",
      lead: null,
    });
    // No figure of either source, only the time of the last good read.
    expect(v.text).not.toMatch(/KB|MB|242/);
    expect(overviewVerdict(input({ module: null, staleFiguresAt: null })).text).toBe("Status unknown.");
  });

  it("right after a web server restart the optimizer's figures still lead: not idle, and no module figure", () => {
    // The module's counters are all zero again; the optimizer has been serving for hours.
    const v = overviewVerdict(input({ module: { summary: idle, startMs: NOW - 20_000 } }));
    expect(v).toMatchObject({ state: "working", lead: "optimizer" });
    expect(v.text).toBe(
      "Working. 242 optimized responses served from the optimizer's cache, 673 KB saved since the optimizer started 3 h ago. " +
        "Web server restarted less than a minute ago; the module's figures are not meaningful yet.",
    );
    // Past the warm-up with still nothing rewritten, the optimizer's figures stand alone.
    expect(overviewVerdict(input({ module: { summary: idle, startMs: NOW - 2 * HOUR } }))).toMatchObject({
      text: "Working. 242 optimized responses served from the optimizer's cache, 673 KB saved since the optimizer started 3 h ago.",
      lead: "optimizer",
    });
  });

  it("an unknown or unbelievable start is a restart without an age, and never warming", () => {
    expect(overviewVerdict(input({ module: { summary: active, startMs: null }, optimizer: moduleOnly })).text).toBe(
      "Working. 281 KB saved by rewriting since the web server started.",
    );
    expect(overviewVerdict(input({ optimizer: optimizer({ startMs: NOW + HOUR }) })).text).toBe(
      "Working. 242 optimized responses served from the optimizer's cache, 673 KB saved since the optimizer started.",
    );
  });

  it("traffic without a saving, or without the savings counters, says so instead of a figure", () => {
    const nothing = moduleSummary({ css_filter_total_bytes_saved: 0, css_filter_total_original_bytes: 0, num_flushes: 3 });
    expect(overviewVerdict(input({ module: { summary: nothing, startMs: NOW - 2 * HOUR }, optimizer: moduleOnly })).text).toBe(
      "Working. Nothing saved by rewriting since the web server started 2 h ago.",
    );
    const unreported = moduleSummary({ num_flushes: 3 });
    const v = overviewVerdict(
      input({ wholeServer: false, module: { summary: unreported, startMs: NOW - 2 * HOUR }, optimizer: moduleOnly }),
    );
    expect(v.text).toBe("Working. This site: no rewrite savings are reported since the web server started 2 h ago.");
    expect(v.lead).toBeNull();
  });

  it("whole server, optimizer running but nothing served yet: the module's figure leads", () => {
    const v = overviewVerdict(input({ optimizer: optimizer({ serves: null }) }));
    expect(v).toMatchObject({ text: "Working. 281 KB saved by rewriting since the web server started 1 d ago.", lead: "module" });
  });
});

describe("optimizerServes", () => {
  it("adds up the served classes; nothing served or no block is null", () => {
    const stats = {
      serve_savings: {
        css: { hits: 12, original_bytes: 4000, optimized_bytes: 1000 },
        image: { hits: 30, original_bytes: 9000, optimized_bytes: 6000 },
        html: { hits: 0, original_bytes: 0, optimized_bytes: 0 },
      },
    } as unknown as DaemonStatsResponse;
    expect(optimizerServes(stats)).toEqual({ responses: 42, saved: 6000 });
    expect(optimizerServes(null)).toBeNull();
    expect(optimizerServes({ serve_savings: { css: { hits: 0, original_bytes: 0, optimized_bytes: 0 } } } as unknown as DaemonStatsResponse)).toBeNull();
  });
});

describe("serveShareJudgement", () => {
  const rate = (percent: number | null, mode: "optimizable" | "all-in-place") => ({ percent, served: 7, total: 1651, excluded: null, mode });
  it("a low share over all requests is ordinary; over cacheable requests it points at Savings", () => {
    expect(serveShareJudgement(rate(0, "all-in-place"), false)).toMatch(/^Normal when most requests are pages/);
    expect(serveShareJudgement(rate(3, "optimizable"), false)).toMatch(/Savings shows what was not served/);
  });
  it("says nothing while warming up, for an ordinary share, or without a share", () => {
    expect(serveShareJudgement(rate(0, "all-in-place"), true)).toBeNull();
    expect(serveShareJudgement(rate(10, "all-in-place"), false)).toBeNull();
    expect(serveShareJudgement(rate(null, "all-in-place"), false)).toBeNull();
    expect(serveShareJudgement(null, false)).toBeNull();
  });
});

// One wording for a time base, used by the sentence and by the tiles.
describe("timeBase", () => {
  const NOW = Date.UTC(2026, 0, 15, 12, 0, 0);
  it("names the source and the age of its start", () => {
    expect(timeBase("module", NOW - 3 * 3_600_000, NOW)).toMatchObject({
      text: "since the web server started 3 h ago",
      ago: "3 h ago",
      warming: false,
      iso: new Date(NOW - 3 * 3_600_000).toISOString(),
    });
    expect(timeBase("optimizer", NOW - 26 * 3_600_000, NOW).text).toBe("since the optimizer started 1 d ago");
    expect(timeBase("optimizer", NOW - 30_000, NOW)).toMatchObject({
      text: "since the optimizer started less than a minute ago",
      warming: true,
    });
    expect(timeBase("module", NOW - 14 * 60_000, NOW)).toMatchObject({ text: "since the web server started 14 min ago", warming: true });
    expect(timeBase("module", NOW - 15 * 60_000, NOW).warming).toBe(false);
  });
  it("an unknown or unbelievable start has no age, no tooltip and is never warming", () => {
    for (const start of [null, 0, -5, NOW + 3_600_000]) {
      expect(timeBase("module", start, NOW), String(start)).toEqual({
        text: "since the web server started",
        ago: null,
        iso: null,
        warming: false,
      });
    }
    expect(timeBase("optimizer", null, NOW).text).toBe("since the optimizer started");
  });
  it("the sentence uses the same words", () => {
    const summary = moduleSummary({ css_filter_total_bytes_saved: 2048, css_filter_total_original_bytes: 4096, num_flushes: 1 });
    const v = overviewVerdict({
      wholeServer: false,
      at: NOW,
      down: false,
      lastAnswerAt: NOW,
      module: { summary, startMs: NOW - 3 * 3_600_000 },
      optimizer: { availability: "not-configured", stateWord: "", serves: null, startMs: null },
    });
    expect(v.text).toContain(timeBase("module", NOW - 3 * 3_600_000, NOW).text);
  });
});

describe("verdictLevel and findingsMark", () => {
  it("the strip's colour repeats the verdict's word, whatever the findings", () => {
    expect(verdictLevel("working")).toBe("ok");
    expect(verdictLevel("warming")).toBe("info");
    expect(verdictLevel("idle")).toBe("info");
    expect(verdictLevel("unknown")).toBe("neutral");
  });
  it("findings that need action are marked in a word, by their worst level", () => {
    expect(findingsMark("error", true)).toEqual({ level: "error", word: "Error" });
    expect(findingsMark("warning", true)).toEqual({ level: "warning", word: "Warning" });
    expect(findingsMark("info", false)).toBeNull();
    expect(findingsMark("healthy", false)).toBeNull();
    // "Module statistics unavailable" is a warning level without a finding: no mark.
    expect(findingsMark("warning", false)).toBeNull();
  });
});
