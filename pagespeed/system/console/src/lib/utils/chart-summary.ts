// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** One plottable series: a label, an optional color, values aligned with the timestamps. */
export interface ChartSeries {
  label: string;
  color?: string;
  data: (number | null)[];
}

/** uPlot columnar data: [timestamps, ...seriesValues]. */
export function alignedData(
  timestamps: number[],
  series: ChartSeries[],
): [number[], ...(number | null)[][]] {
  return [timestamps, ...series.map((s) => s.data)];
}

/** The most recent non-null, finite value, or null when the series has none. */
function latestValue(data: (number | null)[]): number | null {
  for (let i = data.length - 1; i >= 0; i--) {
    const v = data[i];
    if (v !== null && Number.isFinite(v)) return v;
  }
  return null;
}

/**
 * The chart's text alternative (its aria-label): the title plus, per series,
 * the latest value and the number of plottable points, so a screen reader
 * hears what a sighted user sees. `format` renders a latest value — pass the
 * page's unit formatter (e.g. `formatRate` for a rates chart) so the summary
 * carries the same unit the axes show.
 */
export function seriesSummary(
  title: string,
  series: ChartSeries[],
  format: (v: number) => string = String,
): string {
  if (series.length === 0) return `${title}: no data`;
  const parts = series.map((s) => {
    const v = latestValue(s.data);
    const points = s.data.filter((x) => x !== null && Number.isFinite(x)).length;
    const latest = v === null ? "no data" : format(v);
    return `${s.label} latest ${latest}, ${points} ${points === 1 ? "point" : "points"}`;
  });
  return `${title}: ${parts.join(", ")}`;
}

/**
 * The canvas font for chart axis labels. The chart library derives the label
 * size from a "<n>px" token in this string and lays out no tick labels
 * without one, so a bare font family (as the theme's font tokens are) gets a
 * pixel size in front.
 */
export function axisFont(family: string, sizePx = 12): string {
  const f = family.trim();
  if (f === "") return `${sizePx}px monospace`;
  return /\d+px/.test(f) ? f : `${sizePx}px ${f}`;
}

/**
 * A time-axis tick label: hours and minutes, plus seconds only when the ticks
 * are less than a minute apart, so labels stay short enough not to run into
 * each other. `timeZone` is for tests; the page uses the viewer's zone.
 */
export function timeTickLabel(tsSec: number | null, incrSec: number, timeZone?: string): string {
  if (tsSec === null || !Number.isFinite(tsSec)) return "";
  return new Date(tsSec * 1000).toLocaleTimeString("en-GB", {
    hour: "2-digit",
    minute: "2-digit",
    ...(incrSec < 60 ? { second: "2-digit" } : {}),
    hour12: false,
    ...(timeZone ? { timeZone } : {}),
  });
}

/** A duration in the largest unit that keeps it readable: "20 s", "6 min", "2 h". */
function spanText(seconds: number): string {
  if (seconds < 60) return `${Math.max(1, Math.round(seconds))} s`;
  if (seconds < 3600) return `${Math.round(seconds / 60)} min`;
  return `${Math.round(seconds / 3600)} h`;
}

/** The window a live chart actually covers, from its own timestamps:
 *  "Last 6 min · 10 s samples" — never a title the data does not support.
 *  The interval is the median gap, so one long pause does not rename it.
 *  Under two samples it is still collecting. */
export function chartWindowLabel(timestampsSec: ReadonlyArray<number>): string {
  if (timestampsSec.length < 2) return "collecting samples";
  const span = timestampsSec[timestampsSec.length - 1] - timestampsSec[0];
  const gaps: number[] = [];
  for (let i = 1; i < timestampsSec.length; ++i) gaps.push(timestampsSec[i] - timestampsSec[i - 1]);
  gaps.sort((a, b) => a - b);
  const median = gaps[Math.floor(gaps.length / 2)];
  return `Last ${spanText(span)} · ${spanText(median)} samples`;
}

const RATE_UNITS: ReadonlyArray<readonly [string, number]> = [
  ["B/s", 1],
  ["KB/s", 1024],
  ["MB/s", 1024 * 1024],
  ["GB/s", 1024 * 1024 * 1024],
];

/**
 * Y-axis labels for a byte-rate chart. The unit comes from the largest tick
 * (B/s under 1 KB/s, then KB/s, MB/s, GB/s -- the byte formatter's 1024
 * steps), and the labels take the fewest decimals (up to 6) that keep
 * every tick distinct, so a small range never reads "0 KB/s" on every line.
 */
export function bytesRateTicks(splits: ReadonlyArray<number | null>): string[] {
  const finite = splits.filter((v): v is number => v !== null && Number.isFinite(v));
  const max = finite.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
  let unit = 0;
  while (unit < RATE_UNITS.length - 1 && max >= RATE_UNITS[unit + 1][1]) unit++;
  const [label, scale] = RATE_UNITS[unit];
  // Distinct values only: a repeated tick (a flat series) needs no decimals to stay distinct.
  const distinct = [...new Set(finite)];
  let decimals = 0;
  while (decimals < 6 && new Set(distinct.map((v) => (v / scale).toFixed(decimals))).size < distinct.length) decimals++;
  return splits.map((v) => (v === null || !Number.isFinite(v) ? "" : `${(v / scale).toFixed(decimals)} ${label}`));
}

/**
 * The width a value axis reserves (CSS px): its widest label as measured in
 * the axis font, plus `extra` (the tick length and the gap to the label),
 * rounded up, and never less than `min`. Labels are drawn right-aligned
 * against the plot, so a narrower axis cuts off the start of a label -- the
 * number in "1.50 KB/s". Empty or missing labels do not count; a width
 * that is not a finite number falls back to `min`.
 */
export function valueAxisWidth(
  labels: ReadonlyArray<string | null | undefined> | null,
  measure: (label: string) => number,
  extra: number,
  min = 50,
): number {
  let widest = 0;
  for (const label of labels ?? []) {
    if (typeof label !== "string" || label === "") continue;
    const w = measure(label);
    if (!Number.isFinite(w)) return min;
    widest = Math.max(widest, w);
  }
  const width = Math.ceil(widest + extra);
  return Number.isFinite(width) ? Math.max(min, width) : min;
}
