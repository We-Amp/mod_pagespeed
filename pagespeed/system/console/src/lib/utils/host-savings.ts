// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The optimizer's serve savings per host (GET /v1/stats
 * serve_savings_by_host): up to 32 hosts, drawn from the first 64 the
 * optimizer saw since it started, ordered by served HITs, and an "other"
 * row for the rest. Untrusted like every optimizer answer, and the rows
 * still add up to the totals: a row whose counters are not non-negative
 * numbers is dropped; a row whose host is outside the hostname grammar
 * counts under "other" (its name is never shown); a host that appears
 * twice is one row with both rows' counters, as the optimizer itself sums
 * a host's serves. Host names are visitor-controlled and render as text.
 */

import { num } from "$lib/alerts";
import { normalizeLensHost } from "./host-lens";

export interface HostSavingsRow {
  host: string;
  hits: number;
  original: number;
  served: number;
  saved: number;
  percent: number | null;
}

export interface HostSavingsView {
  /** Most served first, ties by name. */
  hosts: HostSavingsRow[];
  /** Every other serve; null when there is none. */
  other: HostSavingsRow | null;
  /**
   * The module's marker on a per-host console's narrowed answer: undefined
   * when absent (an older module, or the whole-server answer), the site's
   * host name or "" when usable, null when present but not a usable value.
   */
  site: string | null | undefined;
}

interface Counters {
  hits: number;
  original: number;
  served: number;
}

function countersOf(entry: unknown): Counters | null {
  if (entry === null || typeof entry !== "object" || Array.isArray(entry)) return null;
  const hits = num(entry, "hits");
  const original = num(entry, "original_bytes");
  const served = num(entry, "optimized_bytes");
  if (hits === undefined || original === undefined || served === undefined) return null;
  if (hits < 0 || original < 0 || served < 0) return null;
  return { hits, original, served };
}

function add(a: Counters | null, b: Counters): Counters {
  return a === null ? { ...b } : { hits: a.hits + b.hits, original: a.original + b.original, served: a.served + b.served };
}

function rowOf(c: Counters, host: string): HostSavingsRow {
  const { hits, original, served } = c;
  const saved = Math.min(original, Math.max(0, original - served));
  const percent = original > 0 ? Math.min(100, Math.max(0, Math.round((saved / original) * 100))) : null;
  return { host, hits, original, served, saved, percent };
}

export function hostSavingsView(block: unknown): HostSavingsView | null {
  if (block === null || typeof block !== "object" || Array.isArray(block)) return null;
  const b = block as Record<string, unknown>;
  if (!Array.isArray(b.hosts)) return null;
  const byHost = new Map<string, Counters>();
  let other = countersOf(b.other);
  for (const raw of b.hosts) {
    const counters = countersOf(raw);
    if (counters === null) continue;
    const host = normalizeLensHost((raw as { host?: unknown }).host);
    if (host === null) other = add(other, counters);
    else byHost.set(host, add(byHost.get(host) ?? null, counters));
  }
  const hosts = [...byHost].map(([host, c]) => rowOf(c, host));
  hosts.sort((a, z) => z.hits - a.hits || (a.host < z.host ? -1 : a.host > z.host ? 1 : 0));
  return { hosts, other: other !== null && other.hits > 0 ? rowOf(other, "") : null, site: siteMarker(b) };
}

/** The module's "site" marker: undefined when absent, the host or "" when usable, null otherwise. */
function siteMarker(block: Record<string, unknown>): string | null | undefined {
  if (!Object.prototype.hasOwnProperty.call(block, "site")) return undefined;
  return block.site === "" ? "" : normalizeLensHost(block.site);
}

/** The row for one host, compared exactly; null when the optimizer reported none. */
export function hostRow(view: HostSavingsView | null, host: string | null): HostSavingsRow | null {
  if (view === null || host === null) return null;
  return view.hosts.find((r) => r.host === host) ?? null;
}

/** The figures a per-host console shows: the row (null: muted) and whose they are (null: nobody named). */
export interface SiteSavings {
  row: HostSavingsRow | null;
  host: string | null;
}

/**
 * The row a per-host console shows. A module that narrows the answer for a
 * per-host console marks it with the site it is for: the row named by the
 * marker is shown (labelled with that name, which may be the site's primary
 * name rather than the name the console was opened under); no such row, an
 * empty marker (the site has no name of its own) or an unusable one leaves
 * the card muted -- never a guess by name. Without a marker (an older
 * module) only the console's own name matches.
 */
export function siteRow(view: HostSavingsView | null, consoleHost: string | null): SiteSavings {
  if (view === null || view.site === undefined) {
    return { row: hostRow(view, consoleHost), host: consoleHost };
  }
  if (view.site === null || view.site === "") return { row: null, host: null };
  return { row: hostRow(view, view.site), host: view.site };
}

/** A per-vhost console's own host as the optimizer names it (lowercase, no port, no trailing dot); null when unnamed. */
export function consoleHostName(host: string): string | null {
  return normalizeLensHost(host);
}
