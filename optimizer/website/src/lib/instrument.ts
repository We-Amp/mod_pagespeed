// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Pure helpers behind the readout components. Every rendered number is
// computed from a data file through one of these — never typed as a literal
// in a page template.

/** 38608 -> "38,608 B" (en-US grouping, NBSP before the unit). */
export function formatBytes(n: number): string {
  return `${n.toLocaleString('en-US')}\u00a0B`;
}

/** Rounded integer percent change; negative = smaller. (180, 80) -> -56. */
export function pctDelta(before: number, after: number): number {
  if (before === 0) return 0;
  return Math.round(((after - before) / before) * 100);
}

/** -29 -> "−29%" (U+2212 minus sign), 0 -> "0%". */
export function formatPct(p: number): string {
  if (p === 0) return '0%';
  return `${p < 0 ? '\u2212' : ''}${Math.abs(p)}%`;
}

/** Gauge position as a clamped 0..100 percentage with 1 decimal. */
export function gaugeWidth(value: number, min: number, max: number): number {
  if (!(max > min)) return 0;
  const pct = ((value - min) / (max - min)) * 100;
  return Math.round(Math.min(100, Math.max(0, pct)) * 10) / 10;
}

/** "2026-05-29T…" -> "May 2026" (UTC, so the label never drifts by timezone). */
export function monthYear(iso: string): string {
  return new Date(iso).toLocaleDateString('en-US', {
    month: 'long',
    year: 'numeric',
    timeZone: 'UTC',
  });
}

/** 231341, 10000 -> "230,000+" — round down to a defensible lower bound. */
export function roundDownDisplay(n: number, step: number): string {
  const rounded = Math.floor(n / step) * step;
  return `${rounded.toLocaleString('en-US')}+`;
}
