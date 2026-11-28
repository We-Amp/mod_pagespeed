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
  introServe:
    "Serve variants: where this server, the optimizer library and the optimizer support it and AgentOptimize is on, pages the policy allows are stored and requests that prefer Markdown are answered from the stored copy. Whether that is happening shows in the counters below and in the list of reasons serving may be off; otherwise the numbers are the report.",
  inferred: "(inferred from the counters)",
  decisionLog: "Per-request decisions are in the decision log under LogDir/daemon-html on the server.",
  unavailableSince: "requests could not use serving since the server started",
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
  markdownCodings: "Markdown 200s by coding",
  markdownBr: "brotli",
  markdownGzip: "gzip",
  markdownIdentity: "uncompressed",
  encodedCopies: "Compressed copies not sent",
  encodedMissing: "missing",
  encodedUnbound: "out of date",
  compressDemands: "asked of the optimizer",
  codedFallback: "Compressed copies sent uncompressed (a body filter of the server)",
  storeRefusals: "Store refusals by the engine",
  fallthroughs: "Fall-throughs to the origin page, by reason",
  degraded: "Serving is not active",
  degradedIntro:
    "The mode is serve-variants but the module is not serving, so it runs as report-only. Usually one of these holds; the server log names which one:",
  degradedHistory: "Serving was not active at some point since the server started. If you have since fixed the cause, this count stops growing. Possible causes:",
  degradedCauses: [
    "optimizer library below 1.14",
    "worker not in a markdown mode",
    "worker agent-optimize off",
    "the optimizer's shared cache is not open",
  ],
  agentOptimizeOff: "With the module's AgentOptimize off, the module only reports and no counter shows it.",
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
  ["daemon_markdown_transport_conflict", "A Markdown answer still carried a Content-Encoding or Content-Range header (a compression or range filter ignored the module's no-compress mark). Must stay at zero. On Apache a stage that applies the compressed copy's own coding a second time is not detected."],
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
  /** Markdown 200s by the coding sent (C API 1.16 libraries only). */
  markdownServedBr: number;
  markdownServedGzip: number;
  markdownServedIdentity: number;
  /** Compressed copies a request preferred that were missing or out of date,
   * and the requests to the optimizer to write them. */
  encodedCopyMissing: number;
  encodedCopyUnbound: number;
  compressDemandSent: number;
  /** Compressed answers the server sent as the plain copy (a body filter). */
  codedFallback: number;
  /** Store refusals the engine counts itself (budget, URL disagreement, sidecar write). */
  storeRefusals: HtmlCacheReason[];
  /** Requests that fell through to the origin page, by reason; largest first. */
  fallthroughs: HtmlCacheReason[];
  /** True when the mode was inferred from the counters, not read from the configuration. */
  modeInferred: boolean;
  /** True in serve-variants while serving is blocked now: the counter rose since the previous sample. */
  servingBlocked: boolean;
  /** HTML responses that could not use serving since the server started. */
  servingUnavailableTotal: number;
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
  if (value === undefined || value === "off") return "off";
  // A value this console does not know (a newer server): the mode is unknown, not off.
  return value === "report-only" || value === "serve-variants" ? value : null;
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
  ["request_header", "daemon_html_refused_request_header"],
  ["sidecar_write_failed", "daemon_html_sidecar_write_failed"],
];

/**
 * `previousServeUnavailable` is daemon_html_serve_unavailable at the previous
 * poll, null on the first. That counter only grows, so "serving is blocked
 * now" means it rose between two polls; otherwise it is history.
 */
export function htmlCacheServeUnavailable(stats: Record<string, unknown>): number {
  return count(stats, "daemon_html_serve_unavailable");
}

export function htmlCacheView(
  stats: Record<string, unknown>,
  mode: HtmlCacheMode = null,
  previousServeUnavailable: number | null = null,
): HtmlCacheView {
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
    // The engine's own refusals are their own counters, not report reasons.
    reasons: reasons.filter((r) => !ENGINE_REFUSALS.some(([name]) => name === r.name)),
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
    markdownServedBr: count(stats, "daemon_markdown_served_br"),
    markdownServedGzip: count(stats, "daemon_markdown_served_gzip"),
    markdownServedIdentity: count(stats, "daemon_markdown_served_identity"),
    encodedCopyMissing: count(stats, "daemon_markdown_encoded_copy_missing"),
    encodedCopyUnbound: count(stats, "daemon_markdown_encoded_copy_unbound"),
    compressDemandSent: count(stats, "daemon_markdown_compress_demand_sent"),
    codedFallback: count(stats, "daemon_markdown_coded_fallback"),
    storeRefusals,
    fallthroughs,
    modeInferred: mode === null && effective !== null,
    servingBlocked:
      effective === "serve-variants" && previousServeUnavailable !== null && serveUnavailable > previousServeUnavailable,
    servingUnavailableTotal: effective === "serve-variants" ? serveUnavailable : 0,
    warnings,
  };
}
