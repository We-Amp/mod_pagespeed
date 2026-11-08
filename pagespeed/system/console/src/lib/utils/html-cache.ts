// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The HTML cache report (DaemonHtmlCache report-only), from the module's own
 * counters. It says what the policy would store and why it refuses the rest;
 * nothing is stored or served. A missing or malformed counter reads as zero.
 * Whether the report is on comes from the configuration (GET /config); only
 * when that cannot be read is it inferred from the counters.
 *
 * All of this panel's wording lives in HTML_CACHE_TEXT.
 */

export const HTML_CACHE_TEXT = {
  heading: "HTML cache report",
  off: "Off. Set DaemonHtmlCache report-only to see what the policy would store.",
  unavailable:
    "Unavailable: the report is on, but no optimizer is configured, the optimizer library is older than the policy, or this server (IIS, Envoy) has no report yet.",
  waiting: "On. No HTML response has been evaluated yet.",
  intro: "Report only: nothing is stored or served because of these numbers.",
  evaluated: "HTML responses evaluated",
  unavailableCount: "Not evaluated (unavailable)",
  wouldStore: "Would store",
  refused: "Refused",
  reasons: "Refusals by reason",
  reason: "Reason",
  count: "Responses",
} as const;

const REFUSED_PREFIX = "daemon_html_refused_";

/** The wait after a failed read of /config, doubled for each further failure, up to the cap. */
export const CONFIG_RETRY_FIRST_MS = 10_000;
export const CONFIG_RETRY_MAX_MS = 5 * 60_000;

/** How long to wait before reading /config again after `failures` failed reads in a row. */
export function configRetryDelayMs(failures: number): number {
  return Math.min(CONFIG_RETRY_MAX_MS, CONFIG_RETRY_FIRST_MS * 2 ** Math.max(0, failures - 1));
}

export interface HtmlCacheReason {
  name: string;
  count: number;
}

export type HtmlCacheState = "off" | "waiting" | "unavailable" | "reporting";

/** The DaemonHtmlCache option as configured; null when the configuration could not be read. */
export type HtmlCacheMode = "off" | "report-only" | null;

export interface HtmlCacheView {
  state: HtmlCacheState;
  evaluated: number;
  wouldStore: number;
  refused: number;
  unavailable: number;
  /** Would-store as a share of evaluated, 0 to 100; null with nothing evaluated. */
  wouldStorePercent: number | null;
  /** Largest first, ties by name; only reasons with a count. */
  reasons: HtmlCacheReason[];
}

function count(stats: Record<string, unknown>, name: string): number {
  const value = stats[name];
  return typeof value === "number" && Number.isFinite(value) && value > 0 ? value : 0;
}

const MODE_LINE = /^\s*DaemonHtmlCache \(dhc\)\s+(\S+)\s*$/m;

/**
 * The mode from a GET /config answer: the scope's effective configuration, else
 * the server's. An option left at its default is not listed, which is off.
 */
export function htmlCacheMode(config: unknown): HtmlCacheMode {
  if (config === null || typeof config !== "object") return null;
  const c = config as { effective_config?: unknown; config?: unknown };
  const text =
    typeof c.effective_config === "string" && c.effective_config !== ""
      ? c.effective_config
      : typeof c.config === "string"
        ? c.config
        : null;
  if (text === null) return null;
  return text.match(MODE_LINE)?.[1] === "report-only" ? "report-only" : "off";
}

export function htmlCacheView(stats: Record<string, unknown>, mode: HtmlCacheMode = null): HtmlCacheView {
  const evaluated = count(stats, "daemon_html_evaluated");
  const wouldStore = count(stats, "daemon_html_would_store");
  const refused = count(stats, "daemon_html_refused");
  const reasons: HtmlCacheReason[] = [];
  for (const name of Object.keys(stats)) {
    if (!name.startsWith(REFUSED_PREFIX)) continue;
    const value = count(stats, name);
    if (value > 0) reasons.push({ name: name.slice(REFUSED_PREFIX.length), count: value });
  }
  reasons.sort((a, b) => b.count - a.count || a.name.localeCompare(b.name));
  const unavailable = count(stats, "daemon_html_unavailable");
  let state: HtmlCacheState = "off";
  if (mode === "off") state = "off";
  else if (evaluated > 0) state = "reporting";
  else if (unavailable > 0) state = "unavailable";
  else if (mode === "report-only") state = "waiting";
  return {
    state,
    evaluated,
    wouldStore,
    refused,
    unavailable,
    wouldStorePercent: evaluated > 0 ? Math.min(100, Math.round((wouldStore / evaluated) * 100)) : null,
    reasons,
  };
}
