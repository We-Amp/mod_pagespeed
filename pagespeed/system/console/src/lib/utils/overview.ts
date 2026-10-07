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
import type { AlertSnapshot, DaemonAvailability } from "$lib/alerts";
import type { ConfigScope } from "$lib/utils/config-scope";
import { errorCode } from "$lib/utils/daemon";
import { MESSAGE_WINDOW_MS, MESSAGE_WINDOW_SECONDS, toMessageDigest, type MessageDigest } from "$lib/utils/message-groups";
import { serveSavingsView, type ServeSavingsTotal } from "$lib/utils/serve-savings";
import type { SinceLabel } from "$lib/utils/since";

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

/** Take one sample. Never rejects: each read's failure is carried in the sample. */
export async function sampleOverview(api: OverviewApi, now: () => number = Date.now): Promise<OverviewSample> {
  const log = api.getMessageGroups !== undefined ? api.getMessageGroups(MESSAGE_WINDOW_SECONDS) : null;
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
    inPlaceRequests: sumOf(vars, ["ipro_daemon_served", "ipro_daemon_fallthrough"]),
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
        detail: "No optimizer daemon is configured for this server; the module optimizes on its own.",
      };
    case "unreachable":
      return {
        word: "Unreachable",
        detail: "The optimizer daemon is configured but not answering; the module keeps serving pages without it.",
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

/** "Counters: module since 3 h · optimizer since 2 min": what the numbers on this page cover. */
export function findingsStampText(moduleSince: SinceLabel | null, optimizerSince: SinceLabel | null): string {
  const parts: string[] = [];
  if (moduleSince !== null) parts.push(`module ${moduleSince.text}`);
  if (optimizerSince !== null) parts.push(`optimizer ${optimizerSince.text}`);
  return parts.length === 0 ? "" : `Counters: ${parts.join(" · ")}`;
}
