// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The savings math behind the Savings page. Two independent sources, always
 * labelled for what they are and never silently added together:
 *
 * - Module rewrites: the module's own *_total_bytes_saved /
 *   *_total_original_bytes counter families (bytes the module's filters
 *   saved while rewriting responses).
 * - Optimizer cache serves: the daemon's serve_savings block (bytes saved
 *   on responses served from the optimizer's cache).
 *
 * Both endpoints are untrusted: every counter is read through num(), byte
 * counts are clamped to >= 0, and percentages are rounded and clamped to
 * [0, 100]. A counter that says the optimized bytes exceed the original is
 * a regression: the display value clamps to "no net savings" and the raw
 * value stays visible so the page can say why.
 */

import { num } from "$lib/alerts";
import { formatBytes, formatPercent } from "./format";
import { serveSavingsView } from "./serve-savings";

export const MODULE_SAVINGS_FAMILIES = [
  {
    key: "css",
    label: "CSS",
    savedCounter: "css_filter_total_bytes_saved",
    originalCounter: "css_filter_total_original_bytes",
  },
  {
    key: "js",
    label: "JavaScript",
    savedCounter: "javascript_total_bytes_saved",
    originalCounter: "javascript_total_original_bytes",
  },
  {
    key: "image",
    label: "Images",
    savedCounter: "image_rewrite_total_bytes_saved",
    originalCounter: "image_rewrite_total_original_bytes",
  },
] as const;

export interface ModuleSavingsRow {
  key: string;
  label: string;
  /** Net bytes saved, clamped to >= 0 for display. */
  saved: number;
  /** The counter as reported; negative means the rewrite grew the bytes. */
  rawSaved: number;
  original: number;
  /** 0..100, or null when there is no original to compare against. */
  percent: number | null;
  regressed: boolean;
}

/** A counter as reported: a finite number (negative possible), else 0. */
function rawCounter(vars: Record<string, unknown>, name: string): number {
  const v = num(vars, name);
  return v === undefined ? 0 : v;
}

const clampPercent = (saved: number, original: number): number | null =>
  original > 0 ? Math.min(100, Math.max(0, Math.round((saved / original) * 100))) : null;

export function moduleSavingsByType(vars: Record<string, unknown>): ModuleSavingsRow[] {
  return MODULE_SAVINGS_FAMILIES.map(({ key, label, savedCounter, originalCounter }) => {
    const rawSaved = rawCounter(vars, savedCounter);
    const original = Math.max(0, rawCounter(vars, originalCounter));
    const saved = Math.max(0, rawSaved);
    return {
      key,
      label,
      saved,
      rawSaved,
      original,
      percent: clampPercent(saved, original),
      regressed: rawSaved < 0,
    };
  });
}

export interface ModuleSavingsTotal {
  saved: number;
  original: number;
  percent: number | null;
  regressed: boolean;
}

export function moduleSavingsTotal(rows: ModuleSavingsRow[]): ModuleSavingsTotal {
  const saved = rows.reduce((acc, row) => acc + row.saved, 0);
  const original = rows.reduce((acc, row) => acc + row.original, 0);
  return {
    saved,
    original,
    percent: clampPercent(saved, original),
    regressed: rows.some((row) => row.regressed),
  };
}

export interface CacheHitRate {
  served: number;
  total: number;
  percent: number | null;
}

/** The share of in-place optimizer requests served straight from its cache. */
export function cacheHitRate(vars: Record<string, unknown>): CacheHitRate {
  const served = Math.max(0, rawCounter(vars, "ipro_daemon_served"));
  const fallthrough = Math.max(0, rawCounter(vars, "ipro_daemon_fallthrough"));
  const total = served + fallthrough;
  return { served, total, percent: total > 0 ? clampPercent(served, total) : null };
}

/** The headline module figure: per-family clamped savings, summed. */
export function moduleSavedRaw(vars: Record<string, unknown>): number {
  return MODULE_SAVINGS_FAMILIES.reduce(
    (acc, family) => acc + Math.max(0, rawCounter(vars, family.savedCounter)),
    0,
  );
}

/**
 * Bytes saved on optimizer cache serves, summed over the served classes
 * without clamping: a negative result is a net regression. Null when no
 * class has served (or the block is unusable) -- "no data", not "zero".
 */
export function daemonSavedRaw(block: unknown): number | null {
  const view = serveSavingsView(block);
  if (view === null || view.served.length === 0) return null;
  return view.served.reduce((acc, row) => acc + (row.original - row.served), 0);
}

export interface OptimizedCopyHitRate {
  percent: number | null;
  served: number;
  total: number;
  /** In-place requests outside the optimizable classes (or whose class was
   *  never learned); null in the all-requests fallback. */
  excluded: number | null;
  mode: "optimizable" | "all-in-place";
}

/** The share of optimizable in-place requests (CSS, JavaScript, images)
 * served from the optimizer's cache — the question "is the optimizer
 * working?" actually asks. Falls back to the all-requests rate, labelled as
 * such, when the per-class counters are absent (an older module). */
export function optimizedCopyHitRate(vars: Record<string, unknown>): OptimizedCopyHitRate {
  const served = Math.max(0, rawCounter(vars, "ipro_daemon_served"));
  const fallthrough = Math.max(0, rawCounter(vars, "ipro_daemon_fallthrough"));
  const css = num(vars, "ipro_daemon_fallthrough_css");
  const js = num(vars, "ipro_daemon_fallthrough_js");
  const image = num(vars, "ipro_daemon_fallthrough_image");
  if (css === undefined || js === undefined || image === undefined) {
    const total = served + fallthrough;
    return {
      percent: clampPercent(served, total),
      served,
      total,
      excluded: null,
      mode: "all-in-place",
    };
  }
  const classSum = Math.max(0, css) + Math.max(0, js) + Math.max(0, image);
  const total = served + classSum;
  return {
    percent: clampPercent(served, total),
    served,
    total,
    excluded: Math.max(0, fallthrough - classSum),
    mode: "optimizable",
  };
}

export interface SplitVerdict {
  count: number;
  bytes: number;
}

export interface SavingsSegment {
  key: "already-optimal" | "optimized-served" | "served-encoded";
  label: string;
  count: number;
  /** The bytes these serves sent; for "already optimal" with verdicts, the
   *  judged resources' original size. Never the type's saving: that is the
   *  split's summary. */
  bytes: number;
}

export interface SavingsSplit {
  key: string;
  label: string;
  /** Always three, in order: already optimal, optimized and served,
   *  served compressed. */
  segments: SavingsSegment[];
  verdict: SplitVerdict | null;
  /** True when the optimizer reports serves by transfer encoding. */
  encodingsKnown: boolean;
  savedBytes: number;
  totalServes: number;
  hasData: boolean;
  /** What the optimizer's per-type byte counts measure. */
  bytesBasis: BytesBasis;
  /** The type's whole saving, all encodings together: the per-type
   *  table's own row, so the two never disagree. Null when the type has
   *  no usable row (nothing served, or malformed counts). */
  summary: SplitSummary | null;
}

export interface SplitSummary {
  saved: number;
  original: number;
  /** 0..100, or null without an original to compare against. */
  percent: number | null;
}

/** The one line that states a type's saving: "Saved 368 KB of 432 KB
 * (85%), measured in optimized bytes". The percentage is left out when
 * there is no original; a figure that is not a usable number reads "—". */
export function splitSummaryText(summary: SplitSummary, basis: BytesBasis): string {
  const original = Number.isFinite(summary.original) && summary.original >= 0 ? summary.original : null;
  const percent =
    original !== null && original > 0 && summary.percent !== null && Number.isFinite(summary.percent)
      ? ` (${formatPercent(summary.percent, summary.saved)})`
      : "";
  return `Saved ${formatBytes(summary.saved)} of ${formatBytes(original)}${percent}, ${basis}`;
}

export type BytesBasis =
  | "measured in optimized bytes"
  | "measured in transfer bytes, because compressed copies were served";

/** The optimizer's per-type byte counts are the optimized identity copies'
 * sizes until compressed copies are served; from then on they include
 * transfer bytes, and the page says so. */
function bytesBasis(encodedServes: number): BytesBasis {
  return encodedServes > 0
    ? "measured in transfer bytes, because compressed copies were served"
    : "measured in optimized bytes";
}

export const SPLIT_TYPES: ReadonlyArray<{ key: string; label: string }> = [
  { key: "css", label: "CSS" },
  { key: "js", label: "JavaScript" },
  { key: "image", label: "Images" },
];

function objectAt(block: unknown, key: string): Record<string, unknown> | null {
  if (block === null || typeof block !== "object" || Array.isArray(block)) return null;
  const entry = (block as Record<string, unknown>)[key];
  if (entry === null || typeof entry !== "object" || Array.isArray(entry)) return null;
  return entry as Record<string, unknown>;
}

const nonNegative = (v: number | undefined): number => Math.max(0, v ?? 0);

function segments(
  alreadyOptimal: number,
  alreadyBytes: number,
  served: number,
  servedBytes: number,
  encoded: number,
  encodedBytes: number,
): SavingsSegment[] {
  return [
    { key: "already-optimal", label: "Already optimal", count: alreadyOptimal, bytes: alreadyBytes },
    { key: "optimized-served", label: "Optimized and served", count: served, bytes: servedBytes },
    { key: "served-encoded", label: "Served compressed", count: encoded, bytes: encodedBytes },
  ];
}

/** The per-type split of what the optimizer did with a type's traffic:
 * already optimal (identity serves of a type with no net saving),
 * optimized and served (identity serves of a saving type), and serves of
 * the stored encodings (zero until the server serves them — the segment
 * exists so that absence is visible, not silent). Verdicts add the
 * per-entry numbers behind the first segment when the optimizer reports
 * them. */
export function savingsSplit(serveSavings: unknown, verdicts: unknown): SavingsSplit[] {
  const table = serveSavingsView(serveSavings);
  return SPLIT_TYPES.map(({ key, label }) => {
    const row = table?.served.find((r) => r.key === key);
    const summary: SplitSummary | null =
      row === undefined ? null : { saved: row.saved, original: row.original, percent: row.percent };
    const entry = objectAt(serveSavings, key);
    const verdictEntry = objectAt(objectAt(verdicts, key), "already_optimal");
    const verdictCount = verdictEntry === null ? undefined : num(verdictEntry, "count");
    const verdictBytes = verdictEntry === null ? undefined : num(verdictEntry, "bytes");
    const verdict: SplitVerdict | null =
      verdictCount !== undefined && verdictBytes !== undefined
        ? { count: Math.max(0, verdictCount), bytes: Math.max(0, verdictBytes) }
        : null;
    if (entry === null) {
      return {
        key,
        label,
        segments: segments(0, 0, 0, 0, 0, 0),
        verdict,
        encodingsKnown: false,
        savedBytes: 0,
        totalServes: 0,
        hasData: verdict !== null && verdict.count > 0,
        bytesBasis: "measured in optimized bytes",
        summary: null,
      };
    }
    const original = nonNegative(num(entry, "original_bytes"));
    const optimized = nonNegative(num(entry, "optimized_bytes"));
    const hits = nonNegative(num(entry, "hits"));
    const encodingsKnown = objectAt(entry, "by_encoding") !== null;
    const ident = encodingsKnown ? nonNegative(num(entry, "by_encoding.identity.hits")) : hits;
    const identBytes = encodingsKnown ? nonNegative(num(entry, "by_encoding.identity.bytes")) : optimized;
    const encoded =
      nonNegative(num(entry, "by_encoding.gzip.hits")) + nonNegative(num(entry, "by_encoding.br.hits"));
    const encodedBytes =
      nonNegative(num(entry, "by_encoding.gzip.bytes")) + nonNegative(num(entry, "by_encoding.br.bytes"));
    const saved = Math.max(0, Math.min(original, original - optimized));
    const totalServes = ident + encoded;
    return {
      key,
      label,
      segments: segments(
        saved <= 0 ? ident : 0,
        verdict !== null ? verdict.bytes : identBytes,
        saved > 0 ? ident : 0,
        saved > 0 ? identBytes : 0,
        encoded,
        encodedBytes,
      ),
      verdict,
      encodingsKnown,
      savedBytes: saved,
      totalServes,
      hasData: totalServes > 0 || (verdict !== null && verdict.count > 0),
      bytesBasis: bytesBasis(encoded),
      summary,
    };
  });
}

/** In-place requests the optimizer had no answer for and had not processed
 * yet (pending or cold), across all types — the classes are a single
 * partition with no type axis. Null without the block (an older
 * optimizer). */
export function notYetOptimized(daemonStats: unknown): number | null {
  const pending = num(daemonStats, "serve_classes.original_pending");
  const cold = num(daemonStats, "serve_classes.original_cold");
  if (pending === undefined && cold === undefined) return null;
  return nonNegative(pending) + nonNegative(cold);
}
