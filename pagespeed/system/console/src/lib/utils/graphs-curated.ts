// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The Graphs view's default charts: six that answer "is it working?" at a
 * glance, from figures the page already reads (the module's statistics,
 * the optimizer's statistics, the statistics log). One sample per poll goes
 * into the live store's "curated" group; the module's series are also
 * rebuilt from the statistics log, so they have history. Both endpoints
 * are untrusted: a figure that is absent or not a number is a gap (null),
 * never a zero.
 */

import { num } from "$lib/alerts";
import type { DaemonStatsResponse } from "$lib/api/types";
import { toRatesPerSecond, type Sample } from "./graph-series";
import type { GraphsView } from "./graphs-view";
import { parseHash } from "./hash-route";
import { hostRow, hostSavingsView } from "./host-savings";
import { LIVE_SERIES_CAPACITY, type LiveSeriesView } from "./live-series";
import { mergeSeries, sliceSince } from "./merge-series";
import { daemonSavedRaw, moduleSavedRaw, MODULE_SAVINGS_FAMILIES, optimizedCopyHitRate } from "./savings";

export const CURATED_GROUP = "curated";
export const CURATED_SERIES = [
  "requests",
  "module_saved",
  "optimizer_saved",
  "hit_served",
  "hit_total",
  "fetch_failures",
  "queue",
] as const;
export type CuratedSeries = (typeof CURATED_SERIES)[number];

export type CuratedUnit = "per-second" | "bytes-per-second" | "percent" | "count";

export interface CuratedChartSpec {
  id: string;
  title: string;
  unit: CuratedUnit;
  source: "module" | "optimizer";
}

export const CURATED_CHARTS: readonly CuratedChartSpec[] = [
  { id: "requests", title: "In-place requests", unit: "per-second", source: "module" },
  { id: "module_saved", title: "Module rewrite savings", unit: "bytes-per-second", source: "module" },
  { id: "optimizer_saved", title: "Optimizer serve savings", unit: "bytes-per-second", source: "optimizer" },
  { id: "hit_rate", title: "Optimized-copy hit rate", unit: "percent", source: "module" },
  { id: "fetch_failures", title: "Resource fetch failures", unit: "per-second", source: "module" },
  { id: "queue", title: "Optimizer jobs in progress", unit: "count", source: "optimizer" },
];

export interface CuratedChart extends CuratedChartSpec {
  timestamps: number[];
  series: Sample[];
  hasData: boolean;
}

export interface CuratedHistory {
  /** Epoch seconds. */
  timestamps: number[];
  series: Record<CuratedSeries, Sample[]>;
}

const CLASS_FALLTHROUGHS = ["ipro_daemon_fallthrough_css", "ipro_daemon_fallthrough_js", "ipro_daemon_fallthrough_image"] as const;

function blankSeries(length: number): Record<CuratedSeries, Sample[]> {
  const out = {} as Record<CuratedSeries, Sample[]>;
  for (const name of CURATED_SERIES) out[name] = new Array<Sample>(length).fill(null);
  return out;
}

/**
 * One poll's raw values, in CURATED_SERIES order. `host`: the optimizer's
 * savings for that host only (the host lens, or a per-vhost console's own
 * host), compared exactly; null: the whole server.
 */
export function curatedSample(
  vars: Record<string, unknown> | null,
  daemon: DaemonStatsResponse | null,
  host: string | null,
): Array<number | null> {
  const v = (name: string): number | null => {
    if (vars === null) return null;
    const x = num(vars, name);
    return x === undefined ? null : x;
  };
  const served = v("ipro_daemon_served");
  const fallthrough = v("ipro_daemon_fallthrough");
  const requests = served !== null && fallthrough !== null ? served + fallthrough : null;
  const moduleSaved = vars === null ? null : moduleSavedRaw(vars);
  const hit = vars !== null && served !== null && fallthrough !== null ? optimizedCopyHitRate(vars) : null;
  let optimizerSaved: number | null = null;
  if (daemon !== null) {
    if (host !== null) {
      const row = hostRow(hostSavingsView(daemon.serve_savings_by_host), host);
      optimizerSaved = row === null ? null : row.original - row.served;
    } else {
      optimizerSaved = daemonSavedRaw(daemon.serve_savings);
    }
  }
  const queue = daemon === null ? null : (num(daemon, "thread_pool.inflight") ?? null);
  return [
    requests,
    moduleSaved,
    optimizerSaved,
    hit === null ? null : hit.served,
    hit === null ? null : hit.total,
    v("num_resource_fetch_failures"),
    queue,
  ];
}

/** The share served in each interval, in percent; a reset (a decrease) or an interval without requests is a gap. */
export function intervalPercent(served: readonly Sample[], total: readonly Sample[]): Sample[] {
  const n = Math.min(served.length, total.length);
  const out: Sample[] = new Array(n).fill(null);
  for (let i = 1; i < n; i++) {
    const s0 = served[i - 1];
    const s1 = served[i];
    const t0 = total[i - 1];
    const t1 = total[i];
    if (s0 === null || s1 === null || t0 === null || t1 === null) continue;
    const ds = s1 - s0;
    const dt = t1 - t0;
    if (ds < 0 || dt <= 0) continue;
    out[i] = Math.min(100, Math.max(0, (ds / dt) * 100));
  }
  return out;
}

/** The six charts from raw series sharing `timestamps` (epoch seconds), in CURATED_CHARTS order. */
export function curatedCharts(timestamps: readonly number[], raw: Record<CuratedSeries, Sample[]>): CuratedChart[] {
  const plotted: Record<string, Sample[]> = {
    requests: toRatesPerSecond(timestamps, raw.requests),
    module_saved: toRatesPerSecond(timestamps, raw.module_saved),
    optimizer_saved: toRatesPerSecond(timestamps, raw.optimizer_saved),
    hit_rate: intervalPercent(raw.hit_served, raw.hit_total),
    fetch_failures: toRatesPerSecond(timestamps, raw.fetch_failures),
    queue: raw.queue.slice(),
  };
  return CURATED_CHARTS.map((spec) => {
    const series = plotted[spec.id];
    return {
      ...spec,
      timestamps: [...timestamps],
      series,
      hasData: series.some((x) => x !== null && Number.isFinite(x)),
    };
  });
}

/** The module's curated series rebuilt from the statistics log (the optimizer's are live only). */
export function curatedFromLog(view: GraphsView | null): CuratedHistory {
  if (view === null || view.kind !== "data") return { timestamps: [], series: blankSeries(0) };
  const timestamps = view.timestamps.map((t) => t / 1000);
  const byName = new Map(view.graphs.map((g) => [g.name, g.data]));
  const at = (name: string, i: number): number | null => {
    const x = byName.get(name)?.[i] ?? null;
    return x === null || !Number.isFinite(x) ? null : x;
  };
  const sum = (names: readonly string[], i: number): number | null => {
    let total = 0;
    for (const name of names) {
      const x = at(name, i);
      if (x === null) return null;
      total += x;
    }
    return total;
  };
  const series = blankSeries(timestamps.length);
  for (let i = 0; i < timestamps.length; i++) {
    const served = at("ipro_daemon_served", i);
    const all = sum(["ipro_daemon_served", "ipro_daemon_fallthrough"], i);
    series.requests[i] = all;
    // A family the log does not carry counts as nothing saved, as moduleSavedRaw counts it.
    const known = MODULE_SAVINGS_FAMILIES.map((f) => at(f.savedCounter, i)).filter((x): x is number => x !== null);
    series.module_saved[i] = known.length === 0 ? null : known.reduce((acc, x) => acc + Math.max(0, x), 0);
    const classes = sum(CLASS_FALLTHROUGHS, i);
    series.hit_served[i] = served;
    series.hit_total[i] = served === null ? null : classes !== null ? served + classes : all;
    series.fetch_failures[i] = at("num_resource_fetch_failures", i);
  }
  return { timestamps, series };
}

/** The log's history and the live tail on one time axis, from `sinceSec` on; a live sample wins a tie. */
export function mergeCurated(
  log: CuratedHistory,
  live: LiveSeriesView | null,
  sinceSec: number,
): { timestamps: number[]; raw: Record<CuratedSeries, Sample[]> } {
  const raw = blankSeries(0);
  let timestamps: number[] = [];
  CURATED_SERIES.forEach((name, k) => {
    const fromLog = sliceSince(log.timestamps, log.series[name], sinceSec);
    const fromLive = live === null ? { timestamps: [], values: [] } : sliceSince(live.timestamps, live.series[k] ?? [], sinceSec);
    const merged = mergeSeries(fromLog.timestamps, fromLog.values, fromLive.timestamps, fromLive.values);
    raw[name] = merged.values;
    timestamps = merged.timestamps;
  });
  return { timestamps, raw };
}

/**
 * The focus host each live optimizer savings sample was recorded under (the
 * lens host, a per-vhost console's own host, or null for the whole server),
 * by sample time in seconds. Each sample is cumulative for its own focus
 * host, so samples of different hosts never belong on one series.
 */
export class CuratedFocus {
  private readonly at = new Map<number, string | null>();

  constructor(private readonly capacity = LIVE_SERIES_CAPACITY) {}

  record(atSec: number, host: string | null): void {
    this.at.delete(atSec);
    this.at.set(atSec, host);
    // Oldest first out, as the live ring buffer drops its samples.
    for (const key of this.at.keys()) {
      if (this.at.size <= this.capacity) break;
      this.at.delete(key);
    }
  }

  /** The focus host recorded at `atSec`; undefined when none was. */
  hostAt(atSec: number): string | null | undefined {
    return this.at.get(atSec);
  }
}

/** The page's record, kept with the live store, which also outlives a visit. */
export const curatedFocus = new CuratedFocus();

/**
 * `raw` with every optimizer savings sample not recorded under `host`
 * (null: the whole server) turned into a gap: the chart shows only that
 * host's history, and no rate is ever taken across a change of focus host.
 */
export function keepFocus(
  timestamps: readonly number[],
  raw: Record<CuratedSeries, Sample[]>,
  focus: CuratedFocus,
  host: string | null,
): Record<CuratedSeries, Sample[]> {
  const optimizerSaved = raw.optimizer_saved.map((v, i) => (i < timestamps.length && focus.hostAt(timestamps[i]) === host ? v : null));
  return { ...raw, optimizer_saved: optimizerSaved };
}

const COUNTER_NAME = /^[A-Za-z0-9_.:-]{1,128}$/;
const MAX_PINNED = 24;

/** The counters= parameter: "all" (every counter), or a list of counter names to show next to the default charts. */
export function parseCountersParam(value: string | null): { all: boolean; pinned: string[] } {
  if (value === "all") return { all: true, pinned: [] };
  const pinned: string[] = [];
  for (const name of (value ?? "").split(",")) {
    if (COUNTER_NAME.test(name) && !pinned.includes(name)) pinned.push(name);
    if (pinned.length === MAX_PINNED) break;
  }
  return { all: false, pinned };
}

/** `hash` with counters= set to the pinned list (removed when empty); other parameters kept. */
export function countersHref(pinned: readonly string[], hash: string): string {
  const { path, params } = parseHash(hash);
  const next = new URLSearchParams(params);
  if (pinned.length === 0) next.delete("counters");
  else next.set("counters", pinned.join(","));
  const query = next.toString();
  return query === "" ? path : `${path}?${query}`;
}
