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

export interface ServeClassRow {
  key: string;
  label: string;
  hits: number;
  original: number;
  served: number;
  saved: number;
  percent: number | null;
}

export interface ServeSavingsTotal {
  saved: number;
  original: number;
  percent: number;
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
    const saved = clamp(original - servedBytes, 0, original);
    served.push({ key, label, hits, original, served: servedBytes, saved, percent: percentOf(saved, original) });
  }
  const original = served.reduce((sum, r) => sum + r.original, 0);
  const saved = served.reduce((sum, r) => sum + r.saved, 0);
  const total = original > 0 ? { saved, original, percent: percentOf(saved, original) ?? 0 } : null;
  return { served, notServed, total };
}
