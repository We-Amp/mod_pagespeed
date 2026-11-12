// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The optimizer's serve savings, per content class. In this console's
 * deployment the optimizer serves no page responses: the web server's module
 * serves optimized responses from the optimizer's cache and records each one
 * (content class, original bytes, bytes served) in the statistics the
 * optimizer reports. A class with nothing recorded is named as "not served",
 * not shown as a row of zeros. The optimizer is untrusted: every figure is
 * read through num() and clamped.
 */

import { num } from "$lib/alerts";
import { formatBytes, formatSignedBytes } from "./format";

/** The share of served bytes the split must cover before it is shown. */
export const SPLIT_MIN_COVERAGE = 0.9;

/** The share of the original bytes within which a negative remainder shows as 0 B (see unsplitText). */
export const UNSPLIT_SKEW = 0.01;

/**
 * The saving divided into what the served encoding saved and what the
 * rewrite saved, over the serves whose entry recorded the size of the origin
 * bytes in the encoding served. Both parts are SIGNED sums, so
 * encodingSaved + rewriteSaved equals coveredOriginal - coveredServed
 * exactly; a display clamps only a percentage, and a negative encoding part
 * (a tiny file the encoding grew) is shown signed, never clamped.
 */
export interface EncodingSplit {
  coveredOriginal: number;
  compressedOriginal: number;
  coveredServed: number;
  /** coveredOriginal - compressedOriginal. */
  encodingSaved: number;
  /** compressedOriginal - coveredServed; negative when the rewrite grew the bytes. */
  rewriteSaved: number;
  /** The saving of the serves the split does not cover: the headline's
   *  (original - served) minus encodingSaved and rewriteSaved, so the three
   *  parts add up to the headline exactly at any coverage. */
  unsplit: number;
  /** -unsplit when that exceeds UNSPLIT_SKEW of the original bytes, else 0. */
  unsplitOver: number;
  /** True when the HTML class's own optimization part is negative (it gained
   *  markup); only set on the total. */
  htmlGrew: boolean;
  /** 0..1, the covered share of the original bytes. */
  coverage: number;
  /** True when coverage reaches SPLIT_MIN_COVERAGE. */
  available: boolean;
}

export interface ServeClassRow {
  key: string;
  label: string;
  hits: number;
  original: number;
  served: number;
  /** original - served, NOT clamped: negative when the class grew. */
  saved: number;
  /** 0..100: only the percentage is clamped. */
  percent: number | null;
  split: EncodingSplit | null;
}

export interface ServeSavingsTotal {
  /** The sum of the classes' saved, signed: a class that grew counts against
   *  the others, so the headline equals the split's two parts added up. */
  saved: number;
  original: number;
  percent: number;
  split: EncodingSplit | null;
}

export interface ServeSavingsView {
  served: ServeClassRow[];
  notServed: string[];
  total: ServeSavingsTotal | null;
}

const ORDER = ["html", "css", "js", "image"];
const LABELS: Record<string, string> = { html: "HTML", css: "CSS", js: "JavaScript", image: "Images" };

const rank = (key: string) => (ORDER.includes(key) ? ORDER.indexOf(key) : ORDER.length);

/** A counter as the optimizer sent it: absent, a finite number, or malformed. */
type Field = { kind: "absent" } | { kind: "number"; value: number } | { kind: "malformed" };

function field(entry: Record<string, unknown>, key: string): Field {
  if (!Object.prototype.hasOwnProperty.call(entry, key)) return { kind: "absent" };
  const v = num(entry, key);
  return v === undefined ? { kind: "malformed" } : { kind: "number", value: v };
}

const valueOf = (f: Field) => (f.kind === "number" ? f.value : 0);

function encodingSplitOf(
  covered: number,
  compressed: number,
  coveredServed: number,
  original: number,
  servedBytes: number,
): EncodingSplit {
  const encodingSaved = covered - compressed;
  const rewriteSaved = compressed - coveredServed;
  const coverage = original > 0 ? Math.min(1, covered / original) : 0;
  const unsplit = original - servedBytes - encodingSaved - rewriteSaved;
  return {
    coveredOriginal: covered,
    compressedOriginal: compressed,
    coveredServed,
    encodingSaved,
    rewriteSaved,
    unsplit,
    unsplitOver: -unsplit > original * UNSPLIT_SKEW ? -unsplit : 0,
    htmlGrew: false,
    coverage,
    available: covered > 0 && coverage >= SPLIT_MIN_COVERAGE,
  };
}

/** A class's split counters, or null when absent, malformed or inconsistent. */
function readSplit(e: Record<string, unknown>, original: number, servedBytes: number): EncodingSplit | null {
  const block = e["split"];
  if (block === null || typeof block !== "object" || Array.isArray(block)) return null;
  const b = block as Record<string, unknown>;
  const [covered, compressed, coveredServed] = [
    num(b, "original_bytes"),
    num(b, "compressed_original_bytes"),
    num(b, "optimized_bytes"),
  ];
  if (covered === undefined || compressed === undefined || coveredServed === undefined) return null;
  if (covered < 0 || compressed < 0 || coveredServed < 0 || covered > original) return null;
  return encodingSplitOf(covered, compressed, coveredServed, original, servedBytes);
}
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));
const percentOf = (saved: number, original: number) =>
  original > 0 ? clamp(Math.round((saved / original) * 100), 0, 100) : null;

export function serveSavingsView(block: unknown): ServeSavingsView | null {
  if (block === null || typeof block !== "object" || Array.isArray(block)) return null;
  const entries = Object.entries(block as Record<string, unknown>).sort(
    ([a], [b]) => rank(a) - rank(b) || a.localeCompare(b),
  );
  const served: ServeClassRow[] = [];
  const notServed: string[] = [];
  for (const [key, entry] of entries) {
    if (entry === null || typeof entry !== "object" || Array.isArray(entry)) continue;
    const label = LABELS[key] ?? key;
    const e = entry as Record<string, unknown>;
    const [hitsF, originalF, servedF] = [field(e, "hits"), field(e, "original_bytes"), field(e, "optimized_bytes")];
    // A present but malformed counter makes the class no data at all.
    if ([hitsF, originalF, servedF].some((f) => f.kind === "malformed")) continue;
    if ([hitsF, originalF, servedF].every((f) => valueOf(f) === 0)) {
      notServed.push(label);
      continue;
    }
    // Something was recorded but a byte count is missing: no data, never a
    // made-up saving (a missing "served" must not read as 0 bytes served).
    if (originalF.kind === "absent" || servedF.kind === "absent") continue;
    const hits = Math.max(0, valueOf(hitsF));
    const original = Math.max(0, valueOf(originalF));
    const servedBytes = Math.max(0, valueOf(servedF));
    const saved = original - servedBytes;
    served.push({
      key,
      label,
      hits,
      original,
      served: servedBytes,
      saved,
      percent: percentOf(saved, original),
      split: readSplit(e, original, servedBytes),
    });
  }
  const original = served.reduce((sum, r) => sum + r.original, 0);
  const saved = served.reduce((sum, r) => sum + r.saved, 0);
  const splits = served.map((r) => r.split).filter((s): s is EncodingSplit => s !== null);
  const split =
    splits.length === 0
      ? null
      : encodingSplitOf(
          splits.reduce((sum, s) => sum + s.coveredOriginal, 0),
          splits.reduce((sum, s) => sum + s.compressedOriginal, 0),
          splits.reduce((sum, s) => sum + s.coveredServed, 0),
          original,
          served.reduce((sum, r) => sum + r.served, 0),
        );
  if (split !== null) {
    const html = served.find((r) => r.key === "html");
    split.htmlGrew = html?.split != null && html.split.rewriteSaved < 0;
  }
  const total =
    original > 0 ? { saved, original, percent: percentOf(saved, original) ?? 0, split } : null;
  return { served, notServed, total };
}

/** The note every headline of served-response savings carries: the figure is not optimization alone. */
export const SAVINGS_QUALIFIER = "Measured in transfer bytes, includes compression.";

/** The two table cells of a split: signed bytes, or a dash unless the split
 *  is available (covered enough) -- an all-zero block never reads as measured. */
export function splitCells(split: EncodingSplit | null): { encoding: string; rewrite: string; unsplit: string } {
  if (split === null || !split.available) return { encoding: "—", rewrite: "—", unsplit: "—" };
  return {
    encoding: formatSignedBytes(split.encodingSaved),
    rewrite: formatSignedBytes(split.rewriteSaved),
    unsplit: unsplitText(split),
  };
}

/**
 * The remainder as shown, never a negative saving. It is the saving of the
 * serves the split does not cover, so it falls below 0 when those serves
 * came out larger than their originals (seen on a live server: small scripts
 * served a few bytes larger). A second, smaller cause: the optimizer reads its
 * totals and its split one counter at a time, so a serve counted in between
 * is in the split but not in the totals. A deficit within UNSPLIT_SKEW of the
 * original bytes is immaterial and shows as 0 B; a larger one is said in words.
 */
export function unsplitText(split: EncodingSplit): string {
  const zero = formatBytes(0);
  return split.unsplitOver > 0
    ? `${zero} (the rest came out ${formatBytes(split.unsplitOver)} larger)`
    : formatBytes(Math.max(0, split.unsplit));
}
