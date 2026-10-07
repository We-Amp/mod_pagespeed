// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Helpers for the graphs page's rate view.
 *
 * The graphs endpoint and the live poll both carry raw *cumulative* counter
 * values. Every server restart recreates the statistics, so a cumulative
 * series drops back to zero at each restart and the curve reads as a
 * sawtooth rather than as traffic. Dividing the difference of consecutive
 * samples by the seconds between them turns the series into a rate per
 * second, and a decrease is the signature of a counter reset -- it is
 * reported as a gap (`null`) rather than as a large negative spike, so a
 * restart shows up as a break in the line instead of as data.
 */

/** localStorage key holding the graphs page's rates-per-second toggle. */
export const DELTA_VIEW_STORAGE_KEY = "pagespeed.graphs.perIntervalDeltas";

/** A plotted sample: a number, or `null` for "no value here" (a gap). */
export type Sample = number | null;

/**
 * Per-second rates between consecutive samples: each difference divided by
 * the seconds between its two samples, so the statistics log's cadence and
 * the live poll's cadence stay comparable on one chart (a plain difference
 * would draw a cliff at the log-to-live seam). The first sample has no
 * predecessor; a decrease (a restarted counter), a non-finite value and a
 * non-positive time step are gaps. Same length as the input.
 */
export function toRatesPerSecond(
  timestampsSec: readonly number[],
  values: readonly Sample[],
): Sample[] {
  const out: Sample[] = new Array(Math.min(timestampsSec.length, values.length)).fill(null);
  for (let i = 1; i < out.length; i++) {
    const dt = timestampsSec[i] - timestampsSec[i - 1];
    const prev = values[i - 1];
    const cur = values[i];
    if (dt <= 0 || prev === null || cur === null) continue;
    if (!Number.isFinite(prev) || !Number.isFinite(cur)) continue;
    const rate = (cur - prev) / dt;
    out[i] = rate >= 0 && Number.isFinite(rate) ? rate : null;
  }
  return out;
}

/**
 * A rate with its unit: at most three significant digits below 100, no
 * decimals at or above 100 (`Number(v.toPrecision(3))` also strips the
 * trailing zeros `toPrecision` pads with).
 */
export function formatRate(v: number): string {
  const abs = Math.abs(v);
  const text = abs >= 100 ? String(Math.round(v)) : String(Number(v.toPrecision(3)));
  return `${text}/s`;
}

/**
 * The series to plot: rates per second when the view is on (each difference
 * divided by the seconds between its two samples, so the statistics log's
 * cadence and the live poll's cadence stay comparable on one chart),
 * otherwise the cumulative values unchanged. A gauge (`isGauge`) is always
 * drawn raw: its value moves in both directions by itself, so differencing
 * it would invent a rhythm the counter does not have.
 */
export function seriesForDisplay(
  timestampsSec: readonly number[],
  values: readonly Sample[],
  deltaView: boolean,
  isGauge = false,
): Sample[] {
  return deltaView && !isGauge ? toRatesPerSecond(timestampsSec, values) : values.slice();
}

/**
 * The value to report as "Latest": the newest non-null value of the
 * displayed series — the same reading the chart's text alternative names.
 * When the rate view ends on a counter reset the newest sample is a gap the
 * chart draws, and the rate per second up to the restart is still the latest
 * reading.
 */
export function latestForDisplay(series: readonly Sample[]): number | null {
  for (let i = series.length - 1; i >= 0; i--) {
    const v = series[i];
    if (v !== null && Number.isFinite(v)) return v;
  }
  return null;
}

/** Chart title, marked "(per second)" when the rate view actually applies —
 *  a gauge is never marked. */
export function graphTitle(name: string, deltaView: boolean, isGauge = false): string {
  return deltaView && !isGauge ? `${name} (per second)` : name;
}

/**
 * Read the persisted toggle: `true`/`false` for an explicit choice, `null`
 * when the viewer never chose — or when storage is unavailable (Safari's
 * private mode and "block all cookies" both make `localStorage` throw).
 * `null` lets the page default from whether the module's reply names its
 * gauges.
 */
export function loadDeltaView(storage: Storage | null = defaultStorage()): boolean | null {
  try {
    const v = storage?.getItem(DELTA_VIEW_STORAGE_KEY);
    return v === "1" ? true : v === "0" ? false : null;
  } catch {
    return null;
  }
}

/** Persist the toggle. A storage failure must never break the page. */
export function saveDeltaView(
  enabled: boolean,
  storage: Storage | null = defaultStorage(),
): void {
  try {
    storage?.setItem(DELTA_VIEW_STORAGE_KEY, enabled ? "1" : "0");
  } catch {
    // Ignore: the toggle still works for this page view, it just won't persist.
  }
}

/** `localStorage` when the environment has one; merely touching it can throw. */
function defaultStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}
