// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Delivered savings: the sizes of optimized images, stylesheets and scripts,
 * counted each time one is sent, from the cache as well. Two families of
 * module variables, shown side by side and never added together:
 *
 * - "module": responses this server optimized itself
 *   (`image_served_*_bytes`, `css_served_*_bytes`, `javascript_served_*_bytes`),
 *   in sizes before compression;
 * - "optimizer-cache": responses answered from the optimizer cache
 *   (`ipro_daemon_served_{image,css,js}_*_bytes`), which are sizes after
 *   compression where a stored compressed copy was sent.
 *
 * A request is answered by one of the two, never both. Each pair is two
 * counters that only grow; the saving is derived and is signed, because an
 * optimized response can be larger than its original. A pair that reads 0 / 0
 * is "nothing sent yet" (the counters start again from zero with the web
 * server), never "0% saved".
 */

import { num } from "$lib/alerts";
import { formatBytes, formatHistorySpan, formatPercent } from "./format";
import type { GraphsView } from "./graphs-view";

export type DeliveredFamilyKey = "module" | "optimizer-cache";

interface TypeDef {
  key: "css" | "js" | "image";
  label: string;
  /** In a sentence: "stylesheets". */
  noun: string;
  original: string;
  optimized: string;
}

export interface DeliveredFamilyDef {
  key: DeliveredFamilyKey;
  label: string;
  types: readonly TypeDef[];
}

export const DELIVERED_FAMILIES: readonly DeliveredFamilyDef[] = [
  {
    key: "module",
    label: "Optimized by this server",
    types: [
      { key: "css", label: "CSS", noun: "stylesheets", original: "css_served_original_bytes", optimized: "css_served_optimized_bytes" },
      {
        key: "js",
        label: "JavaScript",
        noun: "scripts",
        original: "javascript_served_original_bytes",
        optimized: "javascript_served_optimized_bytes",
      },
      { key: "image", label: "Images", noun: "images", original: "image_served_original_bytes", optimized: "image_served_optimized_bytes" },
    ],
  },
  {
    key: "optimizer-cache",
    label: "Answered from the optimizer cache",
    types: [
      {
        key: "css",
        label: "CSS",
        noun: "stylesheets",
        original: "ipro_daemon_served_css_original_bytes",
        optimized: "ipro_daemon_served_css_optimized_bytes",
      },
      {
        key: "js",
        label: "JavaScript",
        noun: "scripts",
        original: "ipro_daemon_served_js_original_bytes",
        optimized: "ipro_daemon_served_js_optimized_bytes",
      },
      {
        key: "image",
        label: "Images",
        noun: "images",
        original: "ipro_daemon_served_image_original_bytes",
        optimized: "ipro_daemon_served_image_optimized_bytes",
      },
    ],
  },
];

/** Every variable the delivered figures read, for the statistics-log request. */
export const DELIVERED_VARIABLES: readonly string[] = DELIVERED_FAMILIES.flatMap((f) =>
  f.types.flatMap((t) => [t.original, t.optimized]),
);

/** Which way the bytes sent compare with the originals; null when nothing was sent. */
export type Direction = "smaller" | "larger" | "same";

export interface DeliveredFigure {
  /** Bytes of the originals behind the optimized responses sent. */
  original: number;
  /** Bytes of the optimized responses sent. */
  sent: number;
  /** original - sent, NOT clamped: negative when the responses were larger. */
  saved: number;
  /** The size of the difference as a share of the original, 0..100; null without an original. */
  percent: number | null;
  /** True when the difference is more than the whole original (a response over twice its size). */
  percentOver: boolean;
  direction: Direction | null;
  /** False for a pair that reads 0 / 0: nothing sent yet, which is not "0% saved". */
  counted: boolean;
}

export interface DeliveredRow extends DeliveredFigure {
  key: string;
  label: string;
  noun: string;
}

export interface DeliveredFamily {
  key: DeliveredFamilyKey;
  label: string;
  /** False when the module reports none of the family's variables (an older module). */
  reported: boolean;
  rows: DeliveredRow[];
  /** The family's types together; `counted` is false until a response was sent. */
  total: DeliveredFigure;
  /** At least one type's optimized responses were larger than their originals. */
  anyLarger: boolean;
}

function figure(original: number, sent: number): DeliveredFigure {
  const counted = original > 0 || sent > 0;
  const saved = original - sent;
  const share = original > 0 ? (Math.abs(saved) / original) * 100 : null;
  return {
    original,
    sent,
    saved,
    percent: share === null ? null : Math.min(100, share),
    percentOver: share !== null && share > 100,
    direction: !counted ? null : saved > 0 ? "smaller" : saved < 0 ? "larger" : "same",
    counted,
  };
}

/** A counter that only grows, as reported: a finite number at or above 0, else 0. */
function counter(vars: unknown, name: string): number {
  const v = num(vars, name);
  return v === undefined ? 0 : Math.max(0, v);
}

function familyOf(def: DeliveredFamilyDef, vars: unknown): DeliveredFamily {
  const rows = def.types.map((t) => ({
    key: t.key,
    label: t.label,
    noun: t.noun,
    ...figure(counter(vars, t.original), counter(vars, t.optimized)),
  }));
  return {
    key: def.key,
    label: def.label,
    reported: def.types.some((t) => num(vars, t.original) !== undefined || num(vars, t.optimized) !== undefined),
    rows,
    total: figure(
      rows.reduce((sum, r) => sum + r.original, 0),
      rows.reduce((sum, r) => sum + r.sent, 0),
    ),
    anyLarger: rows.some((r) => r.direction === "larger"),
  };
}

/** The two families, in page order: this server's own, then the optimizer cache's. */
export function deliveredFamilies(vars: unknown): DeliveredFamily[] {
  return DELIVERED_FAMILIES.map((def) => familyOf(def, vars));
}

/** One family's figures; see deliveredFamilies. */
export function deliveredFamily(vars: unknown, key: DeliveredFamilyKey): DeliveredFamily {
  return familyOf(DELIVERED_FAMILIES.find((f) => f.key === key) ?? DELIVERED_FAMILIES[0], vars);
}

/**
 * How much smaller, in words: "82% smaller", "<1% smaller", "4% larger",
 * "same size"; "—" while nothing was sent. Never a bare "0%": a pair that
 * reads 0 / 0 has no percentage, and a real difference under half a percent
 * reads "<1%".
 */
export function smallerText(f: DeliveredFigure): string {
  if (!f.counted || f.direction === null || f.percent === null) return "—";
  if (f.direction === "same") return "same size";
  if (f.percentOver) return `more than 100% ${f.direction}`;
  // Bytes were sent, so it was not 100% smaller, whatever the rounding says.
  if (f.direction === "smaller" && f.sent > 0 && f.percent >= 99.5) return "more than 99% smaller";
  return `${formatPercent(f.percent, Math.abs(f.saved))} ${f.direction}`;
}

/** "1.2 MB → 310 KB": the originals and what was sent for them. */
export function sizesText(f: DeliveredFigure): string {
  return `${formatBytes(f.original)} → ${formatBytes(f.sent)}`;
}

/**
 * The delivered figures over the history the statistics log holds. It is a
 * best-effort extra: the log is off by default, is sampled every few minutes
 * and starts over when its file reaches its size limit, so the span is the
 * one the log really covers and is stated with the figures.
 */
export type DeliveredWindow =
  /** No graphs endpoint here: say nothing. */
  | { kind: "unavailable" }
  /** No statistics log, or no samples in it: say how to get a history. */
  | { kind: "no-log" }
  /** A log without growth in these variables over its span: say nothing. */
  | { kind: "quiet" }
  | { kind: "data"; fromMs: number; toMs: number; spanText: string; families: DeliveredFamily[] };

/**
 * Each counter's growth over the logged samples, index for index. A decrease
 * in ANY of the series at a sample is a restart for ALL of them at that
 * sample: the counters restart together, and a busy counter can pass its old
 * value within one logging interval, so a pair read series by series could
 * count a restart on one side only. After a restart a counter began again at
 * zero, so its new value is what it grew by since (a lower bound: what was
 * counted between the last sample and the restart is lost). Gaps are skipped.
 */
export function growthAcross(series: Readonly<Record<string, ReadonlyArray<number | null>>>): Record<string, number> {
  const names = Object.keys(series);
  const usable = (v: number | null | undefined): v is number => typeof v === "number" && Number.isFinite(v) && v >= 0;
  const grown: Record<string, number> = Object.fromEntries(names.map((n) => [n, 0]));
  const prev: Record<string, number | null> = Object.fromEntries(names.map((n) => [n, null]));
  const length = names.reduce((max, n) => Math.max(max, series[n].length), 0);
  for (let i = 0; i < length; i++) {
    const restarted = names.some((n) => {
      const v = series[n][i];
      const before = prev[n];
      return usable(v) && before !== null && v < before;
    });
    for (const n of names) {
      const v = series[n][i];
      if (!usable(v)) continue;
      const before = prev[n];
      if (before !== null) grown[n] += restarted ? v : v - before;
      prev[n] = v;
    }
  }
  return grown;
}

export function deliveredWindow(view: GraphsView | null): DeliveredWindow {
  if (view === null || view.kind === "absent") return { kind: "unavailable" };
  if (view.kind === "no-log" || view.kind === "empty-range") return { kind: "no-log" };
  const series: Record<string, ReadonlyArray<number | null>> = {};
  for (const g of view.graphs) {
    if (DELIVERED_VARIABLES.includes(g.name)) series[g.name] = g.data;
  }
  // The span runs from the first to the last sample that holds one of these
  // variables: a log begun by a version without them does not lengthen it.
  const held = view.timestamps.filter((_, i) =>
    Object.values(series).some((data) => {
      const v = data[i];
      return typeof v === "number" && Number.isFinite(v);
    }),
  );
  if (held.length < 2) return { kind: "quiet" };
  const fromMs = Math.min(...held);
  const toMs = Math.max(...held);
  if (toMs <= fromMs) return { kind: "quiet" };
  const families = deliveredFamilies(growthAcross(series));
  if (!families.some((f) => f.total.counted)) return { kind: "quiet" };
  return { kind: "data", fromMs, toMs, spanText: formatHistorySpan((toMs - fromMs) / 1000), families };
}

/** "CSS 47% smaller (1.01 MB → 538 KB) · Images 73% smaller (…)": the types that sent something. */
export function windowLine(family: DeliveredFamily): string {
  return family.rows
    .filter((r) => r.counted)
    .map((r) => `${r.label} ${smallerText(r)} (${sizesText(r)})`)
    .join(" · ");
}

/** Requests the module's inlining and combining took away; these save requests, not bytes. */
export const REQUESTS_AVOIDED: ReadonlyArray<{ counter: string; one: string; many: string }> = [
  { counter: "image_inline", one: "image inlined", many: "images inlined" },
  { counter: "num_css_inlined", one: "stylesheet inlined", many: "stylesheets inlined" },
  { counter: "num_js_inlined", one: "script inlined", many: "scripts inlined" },
  { counter: "css_file_count_reduction", one: "stylesheet request combined away", many: "stylesheet requests combined away" },
  { counter: "js_file_count_reduction", one: "script request combined away", many: "script requests combined away" },
];

export interface AvoidedRow {
  counter: string;
  count: number;
  text: string;
}

/** The non-zero "requests avoided" counters, in a fixed order. */
export function requestsAvoided(vars: unknown): AvoidedRow[] {
  return REQUESTS_AVOIDED.map(({ counter: name, one, many }) => {
    const count = counter(vars, name);
    return { counter: name, count, text: count === 1 ? one : many };
  }).filter((r) => r.count > 0);
}
