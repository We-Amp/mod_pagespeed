// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Overview page model: one sample of the module statistics and the optimizer
 * daemon's health and statistics, and the figures the page shows from it.
 * Kept out of the component so the behaviour is unit-testable. The daemon is
 * an untrusted peer: every field is read defensively.
 */

import { ApiError } from "$lib/api/client";
import type { DaemonHealthResponse, DaemonStatsResponse, StatsResponse } from "$lib/api/types";
import type { AlertSnapshot, DaemonAvailability, HealthSummary } from "$lib/alerts";
import { DOCS } from "$lib/data/product-facts-console";
import type { ConfigScope } from "$lib/utils/config-scope";
import { errorCode, NO_OPTIMIZER_CONFIGURED, OPTIMIZER_NOT_ANSWERING } from "$lib/utils/daemon";
import { MESSAGE_WINDOW_MS, MESSAGE_WINDOW_SECONDS, toMessageDigest, type MessageDigest } from "$lib/utils/message-groups";
import { formatBytes, formatClock, formatCount, formatRelative } from "$lib/utils/format";
import type { OptimizedCopyHitRate } from "$lib/utils/savings";
import { serveSavingsView, type ServeSavingsTotal } from "$lib/utils/serve-savings";
import { sinceLabel } from "$lib/utils/since";

export type Fetched<T> = { ok: true; data: T } | { ok: false; error: Error };

export interface OverviewSample {
  /** Epoch ms when the sample completed. */
  at: number;
  module: Fetched<StatsResponse>;
  health: Fetched<DaemonHealthResponse>;
  daemonStats: Fetched<DaemonStatsResponse>;
  /** The module's message log, grouped; absent when the api has no reader for it. */
  messages?: Fetched<unknown>;
}

/** The reads of one sample; AdminApiClient satisfies this. */
export interface OverviewApi {
  getStats(): Promise<StatsResponse>;
  daemonHealth(): Promise<DaemonHealthResponse>;
  daemonStats(): Promise<DaemonStatsResponse>;
  /** The message log grouped by template over the last `windowSeconds`. */
  getMessageGroups?(windowSeconds: number): Promise<unknown>;
}

function settle<T>(r: PromiseSettledResult<T>): Fetched<T> {
  if (r.status === "fulfilled") return { ok: true, data: r.value };
  const reason: unknown = r.reason;
  return { ok: false, error: reason instanceof Error ? reason : new Error(String(reason)) };
}

export interface SampleOptions {
  /** Read the message log too (the default). A page that shows no findings passes false. */
  messages?: boolean;
}

/** Take one sample. Never rejects: each read's failure is carried in the sample. */
export async function sampleOverview(
  api: OverviewApi,
  now: () => number = Date.now,
  options: SampleOptions = {},
): Promise<OverviewSample> {
  const log =
    options.messages !== false && api.getMessageGroups !== undefined
      ? api.getMessageGroups(MESSAGE_WINDOW_SECONDS)
      : null;
  const [module, health, daemonStats, messages] = await Promise.allSettled([
    api.getStats(),
    api.daemonHealth(),
    api.daemonStats(),
    log ?? Promise.resolve(null),
  ]);
  const sample: OverviewSample = {
    at: now(),
    module: settle(module),
    health: settle(health),
    daemonStats: settle(daemonStats),
  };
  if (log !== null) sample.messages = settle(messages);
  return sample;
}

/**
 * What a failed v1/daemon read says about the optimizer:
 * 404 — this build has no daemon proxy; 503 with the proxy's own
 * `daemon_not_configured` code — none configured; 502 — configured but not
 * answering; 501 — too old for this console. Anything else (429 in-flight,
 * a network failure, an upstream status the proxy forwarded as-is) is
 * transient: the next refresh tries again.
 */
export function daemonAvailability(error: Error): Exclude<DaemonAvailability, "ok"> {
  if (!(error instanceof ApiError)) return "transient";
  switch (error.status) {
    case 404:
      return "not-configured";
    case 503:
      return errorCode(error) === "daemon_not_configured" ? "not-configured" : "transient";
    case 501:
      return "unsupported";
    case 502:
      return "unreachable";
    default:
      return "transient";
  }
}

/**
 * The oldest optimizer version (`health.version`) this console supports: the
 * version the current optimizer release reports. The module's only other
 * floor is the daemon ABI minor, which is not comparable with this string.
 */
export const MIN_OPTIMIZER_VERSION = "2.0.41";

export type FloorVerdict = "at-or-above" | "below" | "unknown";

function parseVersion(v: unknown): [number, number, number] | null {
  if (typeof v !== "string") return null;
  const m = /^v?(\d+)\.(\d+)\.(\d+)/.exec(v.trim());
  return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
}

/** Compare an optimizer version with the floor; unparsable is "unknown". */
export function optimizerFloor(version: unknown, floor: string = MIN_OPTIMIZER_VERSION): FloorVerdict {
  const v = parseVersion(version);
  const f = parseVersion(floor);
  if (v === null || f === null) return "unknown";
  for (let i = 0; i < 3; i++) {
    if (v[i] !== f[i]) return v[i] > f[i] ? "at-or-above" : "below";
  }
  return "at-or-above";
}

/** The finite numeric counters of a stats_json response. */
export function variablesOf(stats: StatsResponse | null | undefined): Record<string, number> {
  const out: Record<string, number> = {};
  const vars: unknown = stats?.variables;
  if (vars === null || typeof vars !== "object" || Array.isArray(vars)) return out;
  for (const [name, value] of Object.entries(vars as Record<string, unknown>)) {
    if (typeof value === "number" && Number.isFinite(value)) out[name] = value;
  }
  return out;
}

export function asObject<T>(value: T): T {
  return value !== null && typeof value === "object" && !Array.isArray(value) ? value : ({} as T);
}

/** A read that may well answer next time: no HTTP answer at all, or a 429 (busy). */
function mayAnswerNextTime(error: Error): boolean {
  return !(error instanceof ApiError) || error.status === 429;
}

/**
 * The rule inputs of one sample. `prev` is the previous snapshot: a transient
 * health, stats or message-log read (429 in-flight, network, forwarded 5xx)
 * re-uses its block, so a busy refresh neither blanks the optimizer view nor
 * clears a firing finding; carried counters show no rise to the rate rules.
 * `buildStamp` is the console's own build stamp.
 */
export function toSnapshot(s: OverviewSample, prev: AlertSnapshot | null = null, buildStamp?: string): AlertSnapshot {
  let daemon: DaemonAvailability = s.health.ok ? "ok" : daemonAvailability(s.health.error);
  let health: DaemonHealthResponse | null = s.health.ok ? asObject(s.health.data) : null;
  if (daemon === "transient" && prev !== null && prev.daemon === "ok") {
    daemon = "ok";
    health = prev.health;
  }
  const statsFailure = s.daemonStats.ok ? null : daemonAvailability(s.daemonStats.error);
  let daemonStats: DaemonStatsResponse | null = s.daemonStats.ok ? asObject(s.daemonStats.data) : null;
  let statsUnsupported = statsFailure === "unsupported";
  if (statsFailure === "transient" && prev !== null) {
    daemonStats = prev.daemonStats;
    statsUnsupported = prev.daemonStatsUnsupported;
  }
  let messages: MessageDigest | null | undefined;
  if (s.messages === undefined) {
    messages = undefined;
  } else if (s.messages.ok) {
    messages = toMessageDigest(s.messages.data, s.at, MESSAGE_WINDOW_MS);
  } else {
    messages = mayAnswerNextTime(s.messages.error) && prev !== null ? (prev.messages ?? null) : null;
  }
  return {
    module: s.module.ok ? variablesOf(s.module.data) : null,
    daemon,
    health,
    daemonStats,
    daemonStatsUnsupported: daemon === "ok" && statsUnsupported,
    belowFloor: daemon === "ok" && optimizerFloor(health?.version) === "below",
    at: s.at,
    messages,
    buildStamp,
    daemonDownAt: daemon === "unreachable" ? s.at : (prev?.daemonDownAt ?? null),
  };
}

export interface ModuleSummary {
  /** Any traffic or optimization recorded at all. */
  active: boolean;
  bytesSaved: number | null;
  originalBytes: number | null;
  savedPercent: number | null;
  servedByOptimizer: number | null;
  inPlaceRequests: number | null;
  fetchFailures: number | null;
}

const SAVED = ["css_filter_total_bytes_saved", "javascript_total_bytes_saved", "image_rewrite_total_bytes_saved"];
const ORIGINAL = [
  "css_filter_total_original_bytes",
  "javascript_total_original_bytes",
  "image_rewrite_total_original_bytes",
];
const ACTIVITY = [
  ...ORIGINAL,
  "ipro_daemon_served",
  "ipro_daemon_served_original",
  "ipro_daemon_fallthrough",
  "num_flushes",
  "num_resource_fetch_successes",
  "num_resource_fetch_failures",
];

/** Sum of the named counters that are reported; null when none is. */
function sumOf(vars: Record<string, number>, names: readonly string[]): number | null {
  let seen = false;
  let total = 0;
  for (const name of names) {
    const v = vars[name];
    if (typeof v === "number" && Number.isFinite(v)) {
      seen = true;
      total += v;
    }
  }
  return seen ? total : null;
}

export function moduleSummary(vars: Record<string, number>): ModuleSummary {
  const saved = sumOf(vars, SAVED);
  const original = sumOf(vars, ORIGINAL);
  const bytesSaved = saved === null ? null : Math.max(0, saved);
  const savedPercent =
    bytesSaved !== null && original !== null && original > 0
      ? Math.min(100, Math.round((bytesSaved / original) * 100))
      : null;
  return {
    active: ACTIVITY.some((name) => (vars[name] ?? 0) > 0),
    bytesSaved,
    originalBytes: original,
    savedPercent,
    servedByOptimizer: sumOf(vars, ["ipro_daemon_served"]),
    inPlaceRequests: sumOf(vars, [
      "ipro_daemon_served",
      "ipro_daemon_served_original",
      "ipro_daemon_fallthrough",
    ]),
    fetchFailures: sumOf(vars, ["num_resource_fetch_failures"]),
  };
}

export type ServeSavings = ServeSavingsTotal;

/** Bytes saved on the responses served from the optimizer's cache, over all classes. */
export function daemonServeSavings(stats: DaemonStatsResponse | null): ServeSavings | null {
  return serveSavingsView(stats?.serve_savings)?.total ?? null;
}

export function availabilityText(a: DaemonAvailability): { word: string; detail: string } {
  switch (a) {
    case "ok":
      return { word: "Running", detail: "" };
    case "not-configured":
      return {
        word: "Not configured",
        detail: NO_OPTIMIZER_CONFIGURED,
      };
    case "unreachable":
      return {
        word: "Not answering",
        detail: OPTIMIZER_NOT_ANSWERING,
      };
    case "unsupported":
      return {
        word: "Outdated",
        detail: "This optimizer version does not provide the status this console reads. Update the optimizer package.",
      };
    case "transient":
      return { word: "Checking", detail: "The optimizer did not answer this refresh; the next refresh tries again." };
  }
}

/** The health line's levels, plus "unknown": no colour, because nothing is known. */
export interface OverviewHealth {
  text: string;
  level: HealthSummary["level"] | "unknown";
}

/**
 * The health line while the server is not answering. What the page last
 * read says nothing about now, so the line claims nothing: it names the time
 * of the server's last answer to the module read (an error answer counts)
 * instead of a verdict.
 */
export function unknownHealth(
  lastAnsweredAt: number | null,
  timeText: (ms: number) => string = (ms) => new Date(ms).toLocaleTimeString(),
): OverviewHealth {
  return {
    text:
      lastAnsweredAt === null
        ? "Status unknown. The server has not answered"
        : `Status unknown. Last answer ${timeText(lastAnsweredAt)}`,
    level: "unknown",
  };
}

/** How much of an unrecognised status string the card will show. */
const STATUS_TEXT_MAX_LEN = 80;

function capStatusText(s: string): string {
  return s.length > STATUS_TEXT_MAX_LEN ? `${s.slice(0, STATUS_TEXT_MAX_LEN)}…` : s;
}

/**
 * The optimizer's state word from its status. `ready: false` is not a state:
 * the optimizer reports it while every worker thread is busy, so it is a
 * separate `busy` note. The daemon is untrusted: an unrecognised status is
 * capped so it cannot blow out the card's layout.
 */
export function optimizerStateText(
  health: DaemonHealthResponse | null,
): { word: string; ok: boolean; busy: boolean } {
  if (health === null) return { word: "Unknown", ok: false, busy: false };
  const busy = health.ready === false;
  if (health.status === "ok") return { word: "Running", ok: true, busy };
  const status = typeof health.status === "string" && health.status ? health.status : "unknown";
  return { word: `Status: ${capStatusText(status)}`, ok: false, busy };
}

/** Grouped count, or an em dash for a counter this scope does not report. */
export function countText(n: number | null): string {
  return n === null ? "—" : n.toLocaleString();
}

/**
 * Which statistics the overview shows. The configuration's own scope wins;
 * the admin path is the fallback (a renamed global admin path cannot be
 * recognised from the URL).
 */
export function overviewScopeLine(isGlobal: boolean, cfg: ConfigScope | null): string {
  const global = cfg?.scope !== undefined ? cfg.scope === "global" : isGlobal;
  if (global) return "All virtual hosts (the whole server)";
  const host = cfg?.host ? ` (${cfg.host})` : "";
  return `This virtual host${host}: separate from other hosts only when per-virtual-host statistics are enabled`;
}

/**
 * The verdict: the one sentence the overview opens with. It states the state
 * in a word and backs it with the figures of the sources that have one, each
 * with the scope and the time base of its own source:
 *
 *  - the module's rewrite savings cover the console's scope (this site on a
 *    per-host console) since the web server started;
 *  - the optimizer's figures cover the whole server since the optimizer
 *    started, so a per-host console names the optimizer's state and never
 *    states its figures as this site's.
 *
 * A source that started less than WARMUP_MS ago is warming up: its figures
 * are left out and the sentence says so, for that source only. A number from
 * a failed read is never stated. The two savings figures are measured on
 * different bases and are never added up; `lead` names the one the page
 * leads with.
 */
export type VerdictState = "working" | "warming" | "idle" | "unknown";

export type LeadFigure = "optimizer" | "module";

/** What the optimizer's statistics say it served: responses from its cache and the transfer bytes saved on them. */
export interface OptimizerServes {
  responses: number;
  /** Signed: negative when the served responses grew. */
  saved: number;
}

/** The serves of an optimizer statistics answer; null when nothing was served or the block is unusable. */
export function optimizerServes(stats: DaemonStatsResponse | null): OptimizerServes | null {
  const view = serveSavingsView(stats?.serve_savings);
  if (view === null || view.total === null) return null;
  return { responses: view.served.reduce((n, row) => n + row.hits, 0), saved: view.total.saved };
}

export interface VerdictInput {
  /** The whole-server console (false: one virtual host's console). */
  wholeServer: boolean;
  /** Epoch ms of the sample the figures come from. */
  at: number;
  /** The server is not answering: nothing read earlier says anything about now. */
  down: boolean;
  /** Epoch ms of the server's last answer, for the sentence while it is down. */
  lastAnswerAt: number | null;
  /** The module figures of this sample; null when that read failed. */
  module: { summary: ModuleSummary; startMs: number | null } | null;
  /**
   * Epoch ms of the figures still on the page when this sample's module
   * read failed (the last good read); null or absent when there are none.
   */
  staleFiguresAt?: number | null;
  optimizer: {
    availability: DaemonAvailability;
    /** The optimizer's own state word ("Running", "Status: degraded"); used when it answers. */
    stateWord: string;
    serves: OptimizerServes | null;
    startMs: number | null;
  };
}

export interface OverviewVerdict {
  state: VerdictState;
  /** The state in a word or two: the start of `text`. */
  word: string;
  /** The whole sentence, `word` included. */
  text: string;
  /** Which savings figure leads the page; null when there is none to lead with. */
  lead: LeadFigure | null;
}

/** "3 h ago" for a start time; "less than a minute ago" under a minute. */
function agoText(startMs: number, at: number): string {
  return at - startMs < 60_000 ? "less than a minute ago" : formatRelative(startMs, at);
}

/** The two sources of the overview's figures, as the page names them. */
export type FigureSource = "module" | "optimizer";

const SOURCE_NAME: Record<FigureSource, string> = { module: "the web server", optimizer: "the optimizer" };

export interface TimeBase {
  /**
   * The time base in words, the one form the page uses in the sentence and
   * in the tiles: "since the web server started 3 h ago", or without an age
   * when the start is unknown.
   */
  text: string;
  /** "3 h ago"; null when the start is unknown or not believable. */
  ago: string | null;
  /** ISO timestamp for a tooltip; null when the start is unknown. */
  iso: string | null;
  /** The source started less than WARMUP_MS ago. */
  warming: boolean;
}

/**
 * A source's time base. The module's counters start over when the web
 * server starts (a restart or a reload); the optimizer's when the optimizer
 * starts. An absent or unbelievable start (see sinceLabel) has no age and is
 * never warming.
 */
export function timeBase(source: FigureSource, startMs: number | null, at: number): TimeBase {
  const label = sinceLabel(startMs, at);
  const name = SOURCE_NAME[source];
  if (label.iso === null || startMs === null) return { text: `since ${name} started`, ago: null, iso: null, warming: false };
  const ago = agoText(startMs, at);
  return { text: `since ${name} started ${ago}`, ago, iso: label.iso, warming: label.warming };
}

/**
 * The verdict of one sample. `timeText` formats the time of the last answer
 * while the server is down.
 */
export function overviewVerdict(
  input: VerdictInput,
  timeText: (ms: number) => string = (ms) => new Date(ms).toLocaleTimeString(),
): OverviewVerdict {
  const unknown = (text: string): OverviewVerdict => ({ state: "unknown", word: "Status unknown", text, lead: null });
  if (input.down) return unknown(`${unknownHealth(input.lastAnswerAt, timeText).text}.`);
  if (input.module === null) {
    // The figures still on the page are older than this sample: say from when.
    const stale = input.staleFiguresAt;
    return unknown(
      stale === null || stale === undefined
        ? "Status unknown."
        : `Status unknown. The module statistics could not be read; the figures are from ${formatClock(stale)}.`,
    );
  }

  const { summary } = input.module;
  const { availability, serves } = input.optimizer;
  const module = timeBase("module", input.module.startMs, input.at);
  const optimizer = timeBase("optimizer", input.optimizer.startMs, input.at);
  const answers = availability === "ok";
  const moduleSince = module.text;
  const optimizerSince = optimizer.text;
  // The optimizer's figures are a source of the sentence on the whole-server console only.
  const optimizerFigures = input.wholeServer && answers && serves !== null;
  const lead: LeadFigure | null = optimizerFigures ? "optimizer" : summary.active && summary.bytesSaved !== null ? "module" : null;

  // What a per-host console says about the optimizer: its state, never its figures.
  const optimizerState = (): string => {
    if (availability === "unreachable") return " Optimizer (whole server): not answering.";
    if (!answers) return "";
    const state = input.optimizer.stateWord === "Running" ? "running" : input.optimizer.stateWord;
    const restarted = optimizer.warming && optimizer.ago !== null ? `, restarted ${optimizer.ago}` : "";
    return ` Optimizer (whole server): ${state}${restarted}.`;
  };
  const optimizerNote = (): string => {
    if (!input.wholeServer) return optimizerState();
    if (availability === "unreachable") return " The optimizer is not answering.";
    return "";
  };

  // No traffic in this console's scope. The optimizer's serves are the whole server's.
  if (!summary.active && (!input.wholeServer || serves === null || serves.responses === 0)) {
    return {
      state: "idle",
      word: "Running",
      text: `Running, no traffic seen yet ${moduleSince}.${optimizerNote()}`,
      lead: null,
    };
  }

  const warmSources = [module, ...(optimizerFigures ? [optimizer] : [])];
  if (warmSources.every((source) => source.warming)) {
    // The most recent restart is the one the figures wait for.
    const latest = Math.max(input.module.startMs ?? 0, optimizerFigures ? (input.optimizer.startMs ?? 0) : 0);
    const whose = input.wholeServer ? "Counters" : "This site's counters";
    return {
      state: "warming",
      word: "Warming up",
      text: `Warming up. ${whose} restarted ${agoText(latest, input.at)}; figures are not meaningful yet.${optimizerNote()}`,
      lead,
    };
  }

  const site = input.wholeServer ? "" : "This site: ";
  const saved = summary.bytesSaved;
  const rewriting =
    saved === null
      ? `${site}${site ? "no" : "No"} rewrite savings are reported ${moduleSince}.`
      : saved === 0
        ? `${site}${site ? "nothing" : "Nothing"} saved by rewriting ${moduleSince}.`
        : `${site}${formatBytes(saved)} saved by rewriting ${moduleSince}.`;
  const working = (rest: string): OverviewVerdict => ({ state: "working", word: "Working", text: `Working. ${rest}`, lead });

  if (!optimizerFigures) return working(`${rewriting}${optimizerNote()}`);
  if (optimizer.warming) {
    return working(`${rewriting} Optimizer restarted ${optimizer.ago}; its figures are not meaningful yet.`);
  }
  // The optimizer's own count: full answers whose saving it could measure. The
  // module counts more (Overview's optimizer card says so), so whose count it is is said.
  const responses = serves.responses === 1 ? "1 optimized response" : `${formatCount(serves.responses)} optimized responses`;
  const served =
    serves.saved > 0
      ? `The optimizer counted ${responses} served from its cache, ${formatBytes(serves.saved)} saved ${optimizerSince}.`
      : `The optimizer counted ${responses} served from its cache ${optimizerSince}.`;
  const webServer = module.warming ? ` Web server restarted ${module.ago}; the module's figures are not meaningful yet.` : "";
  return working(`${served}${webServer}`);
}

/** The share of requests the optimizer's cache answered, below which the tile says how to read it. */
export const LOW_SERVE_SHARE_PERCENT = 10;

/**
 * How to read a low share of serves from the optimizer's cache, once the
 * counters are past their warm-up; null when the share needs no comment or
 * is not known. Over all in-place requests a low share is ordinary (pages
 * are requests too, and the optimizer's cache answers CSS, JavaScript and
 * images); over the cacheable requests alone it is worth a look.
 */
export function serveShareJudgement(rate: OptimizedCopyHitRate | null, warming: boolean): string | null {
  if (rate === null || rate.percent === null || warming || rate.percent >= LOW_SERVE_SHARE_PERCENT) return null;
  return rate.mode === "all-in-place"
    ? "Normal when most requests are pages: the optimizer's cache answers CSS, JavaScript and images."
    : "A low share for CSS, JavaScript and images; Savings shows what was not served from the cache.";
}

/** The one line that relates the two savings figures, which are never added up. */
export const SAVINGS_RELATION =
  "Separate from the rewriting figure: these are responses served from the optimizer's cache.";

/** The colour of the strip's rule and dot: "neutral" has none. */
export type VerdictLevel = "ok" | "info" | "neutral";

/**
 * The colour that repeats the verdict's word on the strip's rule and dot:
 * working is the ok colour, warming up and idle are information, unknown
 * has no colour. Findings do not change it: their level is stated with the
 * findings count, in words.
 */
export function verdictLevel(state: VerdictState): VerdictLevel {
  if (state === "working") return "ok";
  return state === "unknown" ? "neutral" : "info";
}

/**
 * The word that marks the findings count when a finding needs action
 * ("Error", "Warning"), with its level; null when none does.
 */
export function findingsMark(level: HealthSummary["level"], needAction: boolean): { level: "error" | "warning"; word: string } | null {
  if (!needAction) return null;
  if (level === "error") return { level: "error", word: "Error" };
  return level === "warning" ? { level: "warning", word: "Warning" } : null;
}

/** Where the documentation explains how to add the optimizer to a server. */
export const OPTIMIZER_SETUP_DOC = DOCS.optimizerSetup;

/** The first-run check: the module's own response header on a page it handled (a generic host; the reader fills in theirs). */
export const RESPONSE_HEADER_CHECK = "curl -sI http://example.com/ | grep -i -E 'x-mod-pagespeed|x-page-speed'";
