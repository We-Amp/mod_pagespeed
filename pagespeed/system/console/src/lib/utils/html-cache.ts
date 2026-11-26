// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The HTML cache panel (DaemonHtmlCache report-only and serve-variants), from
 * the module's own counters. In report-only it says what the policy would
 * store and why it refuses the rest, and nothing is stored or served. In
 * serve-variants it also counts what was stored, removed and answered as
 * Markdown, and why a request fell through to the origin's page. A missing or
 * malformed counter reads as zero. The mode comes from the configuration (GET
 * /config); only when that cannot be read is it inferred from the counters.
 * Only counters are shown: no body, cookie, client address or user agent.
 *
 * All of this panel's wording lives in HTML_CACHE_TEXT.
 */

export const HTML_CACHE_TEXT = {
  heading: "HTML cache report",
  off: "Off. Set DaemonHtmlCache to report-only to see what the policy would store, or to serve-variants to store and serve Markdown.",
  unavailable:
    "Unavailable: the HTML cache is on, but no optimizer is configured, the optimizer library is older than the policy, or this server (IIS, Envoy) has no report yet.",
  waiting: "On. No HTML response has been evaluated yet.",
  intro: "Report only: nothing is stored or served because of these numbers.",
  introServe: "Serve variants: HTML responses the policy allows are stored, and a request that prefers Markdown is answered from the stored copy.",
  mode: "Mode",
  modeNames: { off: "off", "report-only": "report-only", "serve-variants": "serve-variants" },
  modeUnknown: "unknown (configuration not readable)",
  stored: "Stored",
  storedNote: "Entries written and kept. No byte total or per-class split is counted.",
  removed: "Removed",
  removedOnRefusal: "Removed on refusal",
  purgeRaceRefused: "Store refused after a purge",
  restoreSkipped: "Re-stores skipped (unchanged)",
  notifySent: "Optimizer notifications sent",
  notifyLimited: "Notifications held back (rate limit)",
  markdownServed: "Markdown answers (200)",
  markdownNotModified: "Markdown answers (304)",
  storeRefusals: "Store refusals by the engine",
  fallthroughs: "Fall-throughs to the origin page, by reason",
  degraded: "Serving is not active",
  degradedIntro:
    "The mode is serve-variants but the module is not serving, so it runs as report-only. One of these holds; the server log names which one:",
  degradedCauses: [
    "optimizer library below 1.14",
    "worker not in a markdown mode",
    "worker agent-optimize off",
  ],
  noOptimizer: "No optimizer is configured, or this server has no HTML cache yet; nothing is evaluated.",
  warnings: "Warnings",
  evaluated: "HTML responses evaluated",
  unavailableCount: "Not evaluated (unavailable)",
  wouldStore: "Would store",
  refused: "Refused",
  reasons: "Refusals by reason",
  reason: "Reason",
  count: "Responses",
} as const;

const REFUSED_PREFIX = "daemon_html_refused_";
const FALLTHROUGH_PREFIX = "daemon_markdown_fallthrough_";

/** Counters that should stay at zero; each is shown as a warning when it moves. */
export const HTML_CACHE_WARNINGS: ReadonlyArray<readonly [string, string]> = [
  ["daemon_html_sidecar_unpaired_stale", "Stored copies whose saved headers did not pair with them were found stale."],
  ["daemon_html_epoch_write_failed", "A purge could not be recorded; the site's stored copies are treated as purged until it can."],
  ["daemon_html_purge_remove_failed", "A purged copy could not be removed from the cache."],
  ["daemon_html_refused_url_disagree", "The server and the module disagree about a request's URL, so nothing is stored for it."],
  ["daemon_markdown_transport_conflict", "A Markdown answer clashed with another transport of the same response. Should stay at zero."],
];

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
export type HtmlCacheMode = "off" | "report-only" | "serve-variants" | null;

export interface HtmlCacheWarning {
  name: string;
  count: number;
  text: string;
}

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
  /** The configured mode; inferred from the counters when it could not be read. */
  mode: HtmlCacheMode;
  stored: number;
  removed: number;
  removedOnRefusal: number;
  purgeRaceRefused: number;
  restoreSkipped: number;
  notifySent: number;
  notifyLimited: number;
  markdownServed: number;
  markdownNotModified: number;
  /** Store refusals the engine counts itself (budget, URL disagreement, sidecar write). */
  storeRefusals: HtmlCacheReason[];
  /** Requests that fell through to the origin page, by reason; largest first. */
  fallthroughs: HtmlCacheReason[];
  /** True in serve-variants while the module is not serving. */
  servingBlocked: boolean;
  /** Warning counters that moved. */
  warnings: HtmlCacheWarning[];
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
  const value = text.match(MODE_LINE)?.[1];
  return value === "report-only" || value === "serve-variants" ? value : "off";
}

function byCount(rows: HtmlCacheReason[]): HtmlCacheReason[] {
  return rows.sort((a, b) => b.count - a.count || a.name.localeCompare(b.name));
}

function withPrefix(stats: Record<string, unknown>, prefix: string): HtmlCacheReason[] {
  const rows: HtmlCacheReason[] = [];
  for (const name of Object.keys(stats)) {
    if (!name.startsWith(prefix)) continue;
    const value = count(stats, name);
    if (value > 0) rows.push({ name: name.slice(prefix.length), count: value });
  }
  return byCount(rows);
}

const ENGINE_REFUSALS: ReadonlyArray<readonly [string, string]> = [
  ["budget", "daemon_html_refused_budget"],
  ["url_disagree", "daemon_html_refused_url_disagree"],
  ["sidecar_write_failed", "daemon_html_sidecar_write_failed"],
];

export function htmlCacheView(stats: Record<string, unknown>, mode: HtmlCacheMode = null): HtmlCacheView {
  const evaluated = count(stats, "daemon_html_evaluated");
  const wouldStore = count(stats, "daemon_html_would_store");
  const refused = count(stats, "daemon_html_refused");
  const reasons = withPrefix(stats, REFUSED_PREFIX);
  const unavailable = count(stats, "daemon_html_unavailable");
  const stored = count(stats, "daemon_html_stored");
  const fallthroughs = withPrefix(stats, FALLTHROUGH_PREFIX);
  const markdownServed = count(stats, "daemon_markdown_served");
  const markdownNotModified = count(stats, "daemon_markdown_not_modified");
  const serveUnavailable = count(stats, "daemon_html_serve_unavailable");
  const engineSeen =
    stored > 0 || serveUnavailable > 0 || markdownServed > 0 || markdownNotModified > 0 || fallthroughs.length > 0;
  const effective: HtmlCacheMode = mode ?? (engineSeen ? "serve-variants" : null);
  const active = effective !== "off";
  let state: HtmlCacheState = "off";
  if (mode === "off") state = "off";
  else if (evaluated > 0 || engineSeen) state = "reporting";
  else if (unavailable > 0) state = "unavailable";
  else if (mode === "report-only" || mode === "serve-variants") state = "waiting";
  const storeRefusals: HtmlCacheReason[] = [];
  for (const [name, counter] of ENGINE_REFUSALS) {
    const value = count(stats, counter);
    if (value > 0) storeRefusals.push({ name, count: value });
  }
  byCount(storeRefusals);
  const warnings: HtmlCacheWarning[] = [];
  if (active) {
    for (const [name, text] of HTML_CACHE_WARNINGS) {
      const value = count(stats, name);
      if (value > 0) warnings.push({ name, count: value, text });
    }
  }
  return {
    state,
    evaluated,
    wouldStore,
    refused,
    unavailable,
    wouldStorePercent: evaluated > 0 ? Math.min(100, Math.round((wouldStore / evaluated) * 100)) : null,
    // The engine's URL-disagreement refusal is its own counter, not a report reason.
    reasons: reasons.filter((r) => r.name !== "budget" && r.name !== "url_disagree"),
    mode: effective,
    stored,
    removed: count(stats, "daemon_html_removed"),
    removedOnRefusal: count(stats, "daemon_html_removed_on_refusal"),
    purgeRaceRefused: count(stats, "daemon_html_purge_race_refused"),
    restoreSkipped: count(stats, "daemon_html_restore_skipped"),
    notifySent: count(stats, "daemon_html_notify_sent"),
    notifyLimited: count(stats, "daemon_html_notify_limited"),
    markdownServed,
    markdownNotModified,
    storeRefusals,
    fallthroughs,
    servingBlocked: effective === "serve-variants" && serveUnavailable > 0,
    warnings,
  };
}
