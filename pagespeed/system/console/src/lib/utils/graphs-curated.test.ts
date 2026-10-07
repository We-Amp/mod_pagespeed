// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import type { DaemonStatsResponse } from "$lib/api/types";
import type { GraphsView } from "./graphs-view";
import {
  CURATED_CHARTS,
  CURATED_SERIES,
  CuratedFocus,
  countersHref,
  curatedCharts,
  curatedFromLog,
  curatedSample,
  intervalPercent,
  keepFocus,
  mergeCurated,
  parseCountersParam,
} from "./graphs-curated";

const VARS = {
  ipro_daemon_served: 90,
  ipro_daemon_fallthrough: 30,
  ipro_daemon_fallthrough_css: 5,
  ipro_daemon_fallthrough_js: 3,
  ipro_daemon_fallthrough_image: 2,
  css_filter_total_bytes_saved: 1000,
  javascript_total_bytes_saved: 500,
  image_rewrite_total_bytes_saved: -20,
  num_resource_fetch_failures: 4,
};
const DAEMON = {
  thread_pool: { inflight: 2, size: 8 },
  serve_savings: { css: { hits: 3, original_bytes: 300, optimized_bytes: 100 } },
  serve_savings_by_host: { hosts: [{ host: "a.test", hits: 1, original_bytes: 50, optimized_bytes: 10 }] },
};

describe("curatedSample", () => {
  it("reads one poll's raw values in series order", () => {
    // requests = served + fall-through; module savings ignore a negative
    // family; hit total = served + the three optimizable fall-throughs.
    expect(curatedSample(VARS, DAEMON, null)).toEqual([120, 1500, 200, 90, 100, 4, 2]);
    expect(CURATED_SERIES).toHaveLength(7);
  });

  it("a host's optimizer savings come from its own row, exactly", () => {
    expect(curatedSample(VARS, DAEMON, "a.test")[2]).toBe(40);
    expect(curatedSample(VARS, DAEMON, "test")[2]).toBeNull();
  });

  it("a missing source is no data, never zero", () => {
    const s = curatedSample(null, null, null);
    expect(s).toEqual([null, null, null, null, null, null, null]);
    // An untrusted answer of the wrong type, as an optimizer could send it.
    const junk = { thread_pool: { inflight: "x" } } as unknown as DaemonStatsResponse;
    expect(curatedSample({}, junk, null)).toEqual([null, 0, null, null, null, null, null]);
  });
});

describe("intervalPercent", () => {
  it("the share served in each interval; a reset or an idle interval is a gap", () => {
    expect(intervalPercent([0, 9, 18, 5, 5], [0, 10, 20, 8, 8])).toEqual([null, 90, 90, null, null]);
    expect(intervalPercent([0, null, 2], [0, 1, 4])).toEqual([null, null, null]);
  });
});

describe("curatedCharts", () => {
  it("six charts in order; rates per second; empty charts marked", () => {
    const raw = Object.fromEntries(CURATED_SERIES.map((n) => [n, [null, null]])) as Record<(typeof CURATED_SERIES)[number], (number | null)[]>;
    raw.requests = [10, 30];
    raw.queue = [3, 1];
    const charts = curatedCharts([100, 110], raw);
    expect(charts.map((c) => c.id)).toEqual(CURATED_CHARTS.map((c) => c.id));
    expect(charts.find((c) => c.id === "requests")?.series).toEqual([null, 2]);
    expect(charts.find((c) => c.id === "queue")?.series).toEqual([3, 1]);
    expect(charts.filter((c) => c.hasData).map((c) => c.id)).toEqual(["requests", "queue"]);
  });
});

describe("the optimizer savings series follows one host", () => {
  // Two polls under host A, three under host B, two for the whole server.
  // Each figure is cumulative for its own focus host: A gains 50 a poll, B
  // 500, the whole server 5000.
  const ts = [0, 5, 10, 15, 20, 25, 30];
  const focus = new CuratedFocus(16);
  const hosts = ["a.test", "a.test", "b.test", "b.test", "b.test", null, null];
  ts.forEach((t, i) => focus.record(t, hosts[i]));
  const raw = Object.fromEntries(CURATED_SERIES.map((n) => [n, ts.map(() => null)])) as Record<(typeof CURATED_SERIES)[number], (number | null)[]>;
  raw.optimizer_saved = [100, 150, 9000, 9500, 10000, 900000, 905000];
  const plotted = (host: string | null) =>
    curatedCharts(ts, keepFocus(ts, raw, focus, host)).find((c) => c.id === "optimizer_saved")?.series;

  it("only the samples recorded under the current host are plotted; no rate spans a change", () => {
    // B: the first B sample has no B sample before it, so no rate there,
    // never (9000 − 150) / 5 from an A/B pair; A's samples are gaps.
    expect(plotted("b.test")).toEqual([null, null, null, 100, 100, null, null]);
    expect(plotted("a.test")).toEqual([null, 10, null, null, null, null, null]);
  });

  it("back to all hosts: only the whole server's samples", () => {
    expect(plotted(null)).toEqual([null, null, null, null, null, null, 1000]);
  });

  it("the other series are left as they are", () => {
    const kept = keepFocus(ts, { ...raw, requests: [1, 2, 3, 4, 5, 6, 7] }, focus, "b.test");
    expect(kept.requests).toEqual([1, 2, 3, 4, 5, 6, 7]);
  });

  it("keeps at most its capacity, oldest first out; an unrecorded sample is a gap", () => {
    const small = new CuratedFocus(2);
    small.record(1, "a.test");
    small.record(2, null);
    small.record(3, "b.test");
    expect(small.hostAt(1)).toBeUndefined();
    expect(small.hostAt(2)).toBeNull();
    expect(small.hostAt(3)).toBe("b.test");
    expect(keepFocus([1, 2, 3], { ...raw, optimizer_saved: [7, 8, 9] }, small, null).optimizer_saved).toEqual([null, 8, null]);
  });
});

describe("curatedFromLog", () => {
  it("rebuilds the module's series from the statistics log", () => {
    const view: GraphsView = {
      kind: "data",
      timestamps: [1_000_000, 1_060_000],
      graphs: [
        { name: "ipro_daemon_served", data: [10, 20] },
        { name: "ipro_daemon_fallthrough", data: [5, null] },
        { name: "css_filter_total_bytes_saved", data: [100, 200] },
        { name: "num_resource_fetch_failures", data: [1, 1] },
      ],
    };
    const h = curatedFromLog(view);
    expect(h.timestamps).toEqual([1000, 1060]);
    expect(h.series.requests).toEqual([15, null]);
    expect(h.series.module_saved).toEqual([100, 200]);
    expect(h.series.hit_served).toEqual([10, 20]);
    expect(h.series.hit_total).toEqual([15, null]);
    expect(h.series.fetch_failures).toEqual([1, 1]);
    expect(h.series.optimizer_saved).toEqual([null, null]);
    expect(h.series.queue).toEqual([null, null]);
  });

  it("no log: no history", () => {
    expect(curatedFromLog(null).timestamps).toEqual([]);
    expect(curatedFromLog({ kind: "no-log" }).series.requests).toEqual([]);
  });
});

describe("mergeCurated", () => {
  it("the log's history and the live tail on one time axis, inside the range", () => {
    const log = curatedFromLog({ kind: "data", timestamps: [10_000, 20_000], graphs: [{ name: "num_resource_fetch_failures", data: [1, 2] }] });
    const live = { names: [...CURATED_SERIES], timestamps: [30], series: CURATED_SERIES.map((n) => (n === "fetch_failures" ? [5] : [null])) };
    const merged = mergeCurated(log, live, 15);
    expect(merged.timestamps).toEqual([20, 30]);
    expect(merged.raw.fetch_failures).toEqual([2, 5]);
  });
});

describe("the counters parameter", () => {
  it("all, a list of names, or nothing", () => {
    expect(parseCountersParam(null)).toEqual({ all: false, pinned: [] });
    expect(parseCountersParam("all")).toEqual({ all: true, pinned: [] });
    expect(parseCountersParam("num_flushes,html-worker-queue-depth,num_flushes,<b>,,cache_hits")).toEqual({
      all: false,
      pinned: ["num_flushes", "html-worker-queue-depth", "cache_hits"],
    });
    expect(parseCountersParam(Array.from({ length: 30 }, (_, i) => `c${i}`).join(",")).pinned).toHaveLength(24);
  });

  it("writes the list into the address, keeping the lens", () => {
    expect(countersHref(["x", "y"], "#/graphs?lens=a.test")).toBe("#/graphs?lens=a.test&counters=x%2Cy");
    expect(countersHref([], "#/graphs?counters=x")).toBe("#/graphs");
    expect(parseCountersParam(new URLSearchParams("counters=x%2Cy").get("counters")).pinned).toEqual(["x", "y"]);
  });
});
