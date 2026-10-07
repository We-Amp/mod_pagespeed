// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Overview findings: a fixed rule table evaluated on every overview sample.
 *
 * Each rule is one finding: a severity, a one-line why (`message`), the fix
 * (one sentence), and where to read more (`link` inside the console, `doc`
 * in the documentation). Rules read the module counters, the optimizer's
 * health and statistics, and the module's message log grouped by template.
 *
 * Rules are rate-based or state-based. A rate rule compares two consecutive
 * samples and fires on an increase of a counter (never on the first sample,
 * never on a counter that went backwards because a process restarted); it
 * then stays up for RATE_ALERT_HOLD_MS after the last time it fired, so a
 * single event does not flash for one refresh. A state rule fires while its
 * condition holds.
 *
 * A rule's check answers true (firing), false (clear) or undefined (its
 * inputs are missing: the optimizer is absent or older, the log could not be
 * read or was truncated, or this sample was transient). Undefined neither
 * raises the finding nor forgets an acknowledgement.
 *
 * An acknowledged finding moves out of the list until its condition clears;
 * a cleared finding forgets its acknowledgement, so it comes back armed. Both
 * endpoints are untrusted peers: every field is read through num() and type
 * guards and degrades to "no data"; message templates carry visitor-
 * controlled text and are only ever rendered as text.
 */

import type { DaemonHealthResponse, DaemonStatsResponse } from "$lib/api/types";
import { MODULE_DOCS } from "$lib/data/product-facts-console";
import { memoryAckStore, type AckStore } from "$lib/utils/finding-acks";
import { formatClock } from "$lib/utils/format";
import { EMPTY_MESSAGE_TEXT, type MessageDigest, type MessageGroup } from "$lib/utils/message-groups";
import { WARMUP_MS } from "$lib/utils/since";
import { isDirtyBuild } from "$lib/utils/versions";

/** What the daemon proxy said about the optimizer in one sample. */
export type DaemonAvailability =
  | "ok" // health answered
  | "not-configured" // no optimizer on this server (or no proxy in this build)
  | "unreachable" // configured but not answering
  | "unsupported" // the optimizer predates what this console reads
  | "transient"; // busy or failed this once; try again next refresh

/** Everything the rules read from one overview sample. */
export interface AlertSnapshot {
  /** Module counters (stats_json variables); null when that read failed. */
  module: Record<string, number> | null;
  daemon: DaemonAvailability;
  /** v1/daemon/health body when it answered, else null. */
  health: DaemonHealthResponse | null;
  /** v1/daemon/stats body when it answered, else null. */
  daemonStats: DaemonStatsResponse | null;
  /** Health answered but v1/daemon/stats is not provided by this optimizer. */
  daemonStatsUnsupported: boolean;
  /** Health answered with a version older than the console's minimum. */
  belowFloor: boolean;
  /** Epoch ms of the sample. */
  at?: number;
  /** The module's message log by template: undefined when not read, null when the read failed. */
  messages?: MessageDigest | null;
  /** The console's build stamp (the module and the console are built together). */
  buildStamp?: string;
  /** Epoch ms of the latest sample in which the optimizer did not answer; null when it always has. */
  daemonDownAt?: number | null;
}

/**
 * What the tracker keeps of an earlier sample for windowed rules: its time
 * and the module counters those rules read, nothing else (never the message
 * log or the optimizer's answers), so a page left open keeps a small ring.
 */
export interface CounterSample {
  at_ms: number;
  counters: Record<string, number>;
}

export type AlertSeverity = "error" | "warning" | "info";

export interface AlertRule {
  id: string;
  label: string;
  severity: AlertSeverity;
  kind: "rate" | "state";
  /**
   * `history` holds the time and the windowed counters of the earlier
   * samples that carried module counters, oldest first, reaching back just
   * past VOLUME_WINDOW_MS (for rules that judge a window rather than one step).
   */
  check: (cur: AlertSnapshot, prev: AlertSnapshot | null, history: readonly CounterSample[]) => boolean | undefined;
  /** The one-line why. */
  message: (cur: AlertSnapshot, prev: AlertSnapshot | null, history: readonly CounterSample[]) => string;
  /** The fix, in one sentence. */
  fix: string;
  /** A console route with the detail, when one exists. */
  link?: string;
  /** A documentation page about it, when one exists. */
  doc?: string;
  /** At most three short lines of detail (may carry log text: render as text). */
  details?: (cur: AlertSnapshot, prev: AlertSnapshot | null) => string[];
  /** A count the overview's summary line states (recent-warnings only). */
  count?: (cur: AlertSnapshot, prev: AlertSnapshot | null) => number;
  /** What `count` counts, singular and plural ("warning", "warnings"). */
  countNoun?: (cur: AlertSnapshot, prev: AlertSnapshot | null) => CountNoun;
}

export interface CountNoun {
  one: string;
  many: string;
}

export interface ActiveAlert {
  id: string;
  label: string;
  severity: AlertSeverity;
  message: string;
  fix: string;
  link?: string;
  doc?: string;
  details: string[];
  /** The rule's count, when it has one (recent-warnings: warnings and errors in the window). */
  count?: number;
  /** What `count` counts: "warnings", or "warnings and errors" when errors are among them. */
  countNoun?: CountNoun;
  /** Epoch ms when the finding started firing (kept while it keeps firing). */
  since: number;
}

/** How long a rate finding stays up after the last time it fired. */
export const RATE_ALERT_HOLD_MS = 60_000;

/** Optimizable requests within VOLUME_WINDOW_MS, all falling through, that mean the cache is not in use. */
export const VOLUME_MIN_REQUESTS = 50;

/** The window the in-place counters are judged over: the last 15 minutes. */
export const VOLUME_WINDOW_MS = 15 * 60_000;

/** At most this many earlier samples are kept for VOLUME_WINDOW_MS (one per 5 s needs 181). */
export const MAX_HISTORY = 1024;

/**
 * The module's cache-generation guard refusal (pagespeed/system/daemon_adapter.cc,
 * the cache-directory handshake), as a message template: the one log line
 * that means the web server and the optimizer disagree about the cache
 * location. Other "in-place optimization is OFF" refusals have other causes.
 */
export const VOLUME_GENERATION_TEMPLATE =
  "in-place optimization is OFF: the optimizer daemon publishes cache directory generation N (cache_dir_generation) " +
  "and this module is built for generation N. The two cache layouts share nothing; nothing was opened and nothing " +
  "was created. Install a module and daemon package pair that agree.";

const DOC_ACCESS = `${MODULE_DOCS}/admin-console/#access-control`;
const DOC_MESSAGES = `${MODULE_DOCS}/admin-console/#message-history`;
const DOC_DIAGNOSE = `${MODULE_DOCS}/troubleshooting/#how-to-get-more-diagnostic-information`;

/** A finite number at a dotted path of an untrusted value, else undefined. */
export function num(value: unknown, path: string): number | undefined {
  let v: unknown = value;
  for (const key of path.split(".")) {
    if (v === null || typeof v !== "object" || Array.isArray(v)) return undefined;
    v = (v as Record<string, unknown>)[key];
  }
  return typeof v === "number" && Number.isFinite(v) ? v : undefined;
}

/** Names of health checks that report a failure (`pass: false` or `false`). */
export function failedChecks(health: DaemonHealthResponse | null): string[] {
  const checks: unknown = health?.checks;
  if (checks === null || typeof checks !== "object" || Array.isArray(checks)) return [];
  return Object.entries(checks as Record<string, unknown>)
    .filter(
      ([, v]) =>
        v === false ||
        (v !== null && typeof v === "object" && (v as { pass?: unknown }).pass === false),
    )
    .map(([name]) => name);
}

function fmt(n: number): string {
  return n.toLocaleString();
}

function plural(n: number, one: string, many: string): string {
  return `${fmt(n)} ${n === 1 ? one : many}`;
}

function capText(s: string, max: number): string {
  return s.length > max ? `${s.slice(0, max)}…` : s;
}

/** Which part of a snapshot a counter is read from. */
type Source = (s: AlertSnapshot) => unknown;
const daemonStatsOf: Source = (s) => s.daemonStats;
const moduleOf: Source = (s) => s.module;

/**
 * Increase of a counter between the previous and the current sample;
 * undefined without a previous sample or when either side lacks the
 * counter; 0 when it went backwards (a restart).
 */
function rise(cur: AlertSnapshot, prev: AlertSnapshot | null, pick: Source, path: string): number | undefined {
  if (prev === null) return undefined;
  const c = num(pick(cur), path);
  const p = num(pick(prev), path);
  if (c === undefined || p === undefined) return undefined;
  return c > p ? c - p : 0;
}

function rose(cur: AlertSnapshot, prev: AlertSnapshot | null, pick: Source, path: string): boolean | undefined {
  const d = rise(cur, prev, pick, path);
  return d === undefined ? undefined : d > 0;
}

/** Active/max connections from the stats block, else from health. */
function connections(s: AlertSnapshot): { active: number; max: number } | undefined {
  for (const source of [s.daemonStats, s.health]) {
    const active = num(source, "connections.active");
    const max = num(source, "connections.max");
    if (active !== undefined && max !== undefined) return { active, max };
  }
  return undefined;
}

/**
 * When the optimizer process started: its own started_at_ms, else the
 * sample time minus its health uptime (an optimizer from before
 * started_at_ms); undefined when neither is known.
 */
export function optimizerStartMs(s: AlertSnapshot): number | undefined {
  const started = num(s.daemonStats, "started_at_ms");
  if (started !== undefined && started > 0) return started;
  const uptime = num(s.health, "uptime_seconds");
  if (uptime !== undefined && uptime >= 0 && s.at !== undefined) return s.at - uptime * 1000;
  return undefined;
}

/**
 * Groups of the message log that pass `test`; undefined without a log, and
 * undefined when none passes but the module left rows out (a truncated
 * answer): an absent template may simply not have fit.
 */
function loggedGroups(s: AlertSnapshot, test: (group: MessageGroup) => boolean): MessageGroup[] | undefined {
  if (s.messages === undefined || s.messages === null) return undefined;
  const found = s.messages.groups.filter(test);
  if (found.length === 0 && s.messages.truncated) return undefined;
  return found;
}

// The module's admin-exposure warning (pagespeed/system/admin_site.cc), as it
// logs it at warning level:
//   "mod_pagespeed: admin handler '%s' received request from %s; intended for
//   loopback. Restrict access at the web-server layer (...). See https://..."
// The handler name and the client address (IPv4, IPv6 or "(unknown)") vary,
// so the template is matched by its fixed text, in order, around them.
const ADMIN_EXPOSURE_PREFIX = "mod_pagespeed: admin handler '";
const ADMIN_EXPOSURE_FROM = "' received request from ";
const ADMIN_EXPOSURE_LOOPBACK = "; intended for loopback.";

const isAdminExposure = (g: MessageGroup): boolean => {
  if (g.level !== "warning" && g.level !== "error" && g.level !== "fatal") return false;
  if (!g.template.startsWith(ADMIN_EXPOSURE_PREFIX)) return false;
  const from = g.template.indexOf(ADMIN_EXPOSURE_FROM, ADMIN_EXPOSURE_PREFIX.length);
  if (from === -1) return false;
  return g.template.indexOf(ADMIN_EXPOSURE_LOOPBACK, from + ADMIN_EXPOSURE_FROM.length) !== -1;
};

/** The cache-generation refusal at warning or error level (never the start-up race lines). */
const isGenerationRefusal = (g: MessageGroup): boolean =>
  (g.level === "warning" || g.level === "error") && g.template === VOLUME_GENERATION_TEMPLATE;

/** Warning-or-worse groups seen within the window, most frequent first. */
function recentWarnings(s: AlertSnapshot): MessageGroup[] | undefined {
  if (s.messages === undefined || s.messages === null) return undefined;
  return s.messages.groups
    .filter((g) => g.level !== "info" && g.recent > 0)
    .sort((a, b) => b.recent - a.recent);
}

/** What the recent groups count: "warnings and errors" when errors are among them, else "warnings". */
function recentNoun(groups: readonly MessageGroup[]): CountNoun {
  return groups.some((g) => g.level === "error" || g.level === "fatal")
    ? { one: "warning or error", many: "warnings and errors" }
    : { one: "warning", many: "warnings" };
}

const OPTIMIZABLE_FALLTHROUGH = [
  "ipro_daemon_fallthrough_css",
  "ipro_daemon_fallthrough_js",
  "ipro_daemon_fallthrough_image",
] as const;

/** The module counters the windowed rules read; the only ones a kept sample holds. */
const WINDOWED_COUNTERS = ["ipro_daemon_served", ...OPTIMIZABLE_FALLTHROUGH] as const;

/** The windowed counters of a sample's module answer; a missing or non-numeric one is left out. */
function windowedCounters(module: Record<string, number>): Record<string, number> {
  const out: Record<string, number> = {};
  for (const name of WINDOWED_COUNTERS) {
    const v = num(module, name);
    if (v !== undefined) out[name] = v;
  }
  return out;
}

/** Increase of a kept counter from one sample to the next; 0 when it went backwards (a restart). */
function counterRise(cur: CounterSample, prev: CounterSample, name: string): number | undefined {
  const c = cur.counters[name];
  const p = prev.counters[name];
  if (c === undefined || p === undefined) return undefined;
  return c > p ? c - p : 0;
}

/**
 * CSS/JS/image fall-throughs over the last VOLUME_WINDOW_MS when nothing at
 * all was served from the optimizer's cache in that time; 0 when something
 * was served; undefined unless the optimizer answers, BOTH the optimizer and
 * the module process started at least WARMUP_MS ago (each side falls through
 * legitimately while it warms up), a sample at least VOLUME_WINDOW_MS old
 * exists (a shorter stretch is no evidence), and every counter is reported
 * in every sample of the window. The rises are added sample to sample, so a
 * counter that went backwards in between counts as no rise.
 */
function fallthroughWithoutServes(cur: AlertSnapshot, history: readonly CounterSample[]): number | undefined {
  if (cur.daemon !== "ok" || cur.at === undefined || cur.module === null) return undefined;
  const optimizerStart = optimizerStartMs(cur);
  if (optimizerStart === undefined || cur.at - optimizerStart < WARMUP_MS) return undefined;
  const moduleStart = num(cur.module, "process_start_ms");
  if (moduleStart === undefined || moduleStart <= 0 || cur.at - moduleStart < WARMUP_MS) return undefined;
  const windowStart = cur.at - VOLUME_WINDOW_MS;
  let base = -1;
  for (let i = history.length - 1; i >= 0; i--) {
    if (history[i].at_ms <= windowStart) {
      base = i;
      break;
    }
  }
  if (base === -1) return undefined;
  const chain = [...history.slice(base), { at_ms: cur.at, counters: windowedCounters(cur.module) }];
  let served = 0;
  let fallthrough = 0;
  for (let i = 1; i < chain.length; i++) {
    const d = counterRise(chain[i], chain[i - 1], "ipro_daemon_served");
    if (d === undefined) return undefined;
    served += d;
    for (const name of OPTIMIZABLE_FALLTHROUGH) {
      const f = counterRise(chain[i], chain[i - 1], name);
      if (f === undefined) return undefined;
      fallthrough += f;
    }
  }
  return served === 0 ? fallthrough : 0;
}

export const ALERT_RULES: readonly AlertRule[] = [
  {
    id: "daemon-check-failed",
    label: "Optimizer health check",
    severity: "error",
    kind: "state",
    link: "#/optimizer",
    check: (cur) => (cur.health === null ? undefined : failedChecks(cur.health).length > 0),
    message: (cur) => {
      const names = failedChecks(cur.health);
      return `Optimizer health ${names.length === 1 ? "check" : "checks"} failing: ${names.join(", ")}.`;
    },
    fix: "Optimizer status shows each check; the optimizer's log usually names the cause.",
    doc: DOC_DIAGNOSE,
  },
  {
    id: "write-failures",
    label: "Optimizer write failures",
    severity: "error",
    kind: "rate",
    check: (cur, prev) => rose(cur, prev, daemonStatsOf, "alternates.write_failures"),
    message: (cur, prev) =>
      `The optimizer could not write ${plural(
        rise(cur, prev, daemonStatsOf, "alternates.write_failures") ?? 0,
        "optimized variant",
        "optimized variants",
      )} (${fmt(num(cur.daemonStats, "alternates.write_failures") ?? 0)} since it started).`,
    fix: "Check the free disk space and the permissions of the optimizer's cache directory.",
    doc: DOC_DIAGNOSE,
  },
  {
    id: "volume-not-opened",
    label: "Optimizer cache not in use",
    severity: "error",
    kind: "rate",
    link: "#/logs?source=module&level=warning",
    doc: DOC_DIAGNOSE,
    check: (cur, _prev, history) => {
      const fallthrough = fallthroughWithoutServes(cur, history);
      const counter = fallthrough === undefined ? undefined : fallthrough >= VOLUME_MIN_REQUESTS;
      if (counter === true) return true;
      const logged = loggedGroups(cur, isGenerationRefusal);
      if (logged !== undefined && logged.some((g) => g.recent > 0)) return true;
      if (logged === undefined && counter === undefined) return undefined;
      return false;
    },
    message: (cur, _prev, history) => {
      const logged = loggedGroups(cur, isGenerationRefusal) ?? [];
      if (logged.length > 0) {
        return `The web server is not using the optimizer's cache: ${capText(logged[0].template, 200)}`;
      }
      return (
        `The optimizer is answering, but none of the last ${fmt(fallthroughWithoutServes(cur, history) ?? 0)} ` +
        "CSS, JavaScript and image requests was served from its cache."
      );
    },
    fix: "Make the web server's DaemonVolumePath and the optimizer's cache directory (--cache-dir) agree.",
  },
  {
    id: "daemon-unreachable",
    label: "Optimizer unreachable",
    severity: "warning",
    kind: "state",
    link: "#/optimizer",
    check: (cur) => (cur.daemon === "transient" ? undefined : cur.daemon === "unreachable"),
    message: () =>
      "The optimizer daemon is configured but not answering. The module keeps serving pages without it.",
    fix: "Start the optimizer service, and check that the web server's DaemonApiSocketPath names the optimizer's socket.",
    doc: DOC_DIAGNOSE,
  },
  {
    id: "daemon-outdated",
    label: "Optimizer version",
    severity: "warning",
    kind: "state",
    link: "#/optimizer",
    check: (cur) =>
      cur.daemon === "transient"
        ? undefined
        : cur.daemon === "unsupported" ||
          (cur.daemon === "ok" && (cur.daemonStatsUnsupported || cur.belowFloor)),
    message: (cur) => {
      const version = typeof cur.health?.version === "string" && cur.health.version ? ` (${cur.health.version})` : "";
      if (cur.belowFloor) {
        return `This optimizer version${version} is older than the oldest this console supports.`;
      }
      return `This optimizer version${version} does not provide the statistics this console reads, so its findings are off.`;
    },
    fix: "Update the optimizer package to the version released with this module.",
    doc: DOC_DIAGNOSE,
  },
  {
    id: "admin-exposed",
    label: "Admin console reachable from the network",
    severity: "warning",
    kind: "state",
    link: "#/logs?source=module&level=warning",
    doc: DOC_ACCESS,
    check: (cur) => {
      const groups = loggedGroups(cur, isAdminExposure);
      return groups === undefined ? undefined : groups.length > 0;
    },
    message: (cur) => {
      const last = Math.max(0, ...(loggedGroups(cur, isAdminExposure) ?? []).map((g) => g.lastMs));
      return `An admin page was requested from an address other than this server itself${
        last > 0 ? ` (last at ${formatClock(last)})` : ""
      }.`;
    },
    fix:
      "Restrict the admin paths to trusted addresses at the web server (Apache: Require local; nginx: allow " +
      "127.0.0.1; deny all; IIS: InfoUrlsLocalOnly), or acknowledge this if the console is public on purpose.",
  },
  {
    id: "error-rate",
    label: "Optimizer errors",
    severity: "warning",
    kind: "rate",
    link: "#/logs?source=optimizer&level=error",
    check: (cur, prev) => rose(cur, prev, daemonStatsOf, "errors.total"),
    message: (cur, prev) =>
      `The optimizer reported ${plural(rise(cur, prev, daemonStatsOf, "errors.total") ?? 0, "new error", "new errors")} ` +
      `(${fmt(num(cur.daemonStats, "errors.total") ?? 0)} since it started).`,
    fix: "Logs (the optimizer's entries) lists the errors; most name the URL and the step that failed.",
  },
  {
    id: "origin-misconfiguration",
    label: "Origin sends compressed responses",
    severity: "warning",
    kind: "state",
    check: (cur) => {
      const v = num(cur.daemonStats, "errors.origin_misconfiguration");
      return v === undefined ? undefined : v > 0;
    },
    message: (cur) =>
      `The optimizer received ${plural(
        num(cur.daemonStats, "errors.origin_misconfiguration") ?? 0,
        "compressed origin response",
        "compressed origin responses",
      )} it cannot optimize.`,
    fix: "Configure the origin, or the proxy in front of it, to send uncompressed responses to the web server.",
  },
  {
    id: "fetch-failures",
    label: "Module fetch failures",
    severity: "warning",
    kind: "rate",
    link: "#/logs?source=module&level=warning",
    check: (cur, prev) => rose(cur, prev, moduleOf, "num_resource_fetch_failures"),
    message: (cur, prev) =>
      `The module could not fetch ${plural(
        rise(cur, prev, moduleOf, "num_resource_fetch_failures") ?? 0,
        "resource",
        "resources",
      )} for optimization (${fmt(num(cur.module, "num_resource_fetch_failures") ?? 0)} since the server started); ` +
      "those are served as the origin sent them.",
    fix: "Check that the web server itself can reach the origin for these URLs; Logs lists them.",
  },
  {
    id: "browser-stopped",
    label: "Browser analysis stopped",
    severity: "warning",
    kind: "state",
    check: (cur) => {
      const browser: unknown = cur.health?.browser;
      if (browser === null || typeof browser !== "object") return undefined;
      const b = browser as { enabled?: unknown; chrome_running?: unknown };
      return b.enabled === true && b.chrome_running === false;
    },
    message: () =>
      "Browser analysis is enabled but the browser is not running; the optimizer falls back to heuristic analysis.",
    fix: "Restart the optimizer; if the browser keeps stopping, its log says why it exits.",
  },
  {
    id: "thread-saturation",
    label: "Optimizer threads busy",
    severity: "warning",
    kind: "state",
    check: (cur) => {
      const size = num(cur.daemonStats, "thread_pool.size");
      const inflight = num(cur.daemonStats, "thread_pool.inflight");
      if (size === undefined || inflight === undefined) return undefined;
      return size > 0 && inflight >= size;
    },
    message: (cur) =>
      `All ${fmt(num(cur.daemonStats, "thread_pool.size") ?? 0)} optimizer worker threads are busy ` +
      `(${fmt(num(cur.daemonStats, "thread_pool.inflight") ?? 0)} in flight); new work waits until current jobs finish.`,
    fix: "If this persists, give the optimizer more worker threads or a machine with more cores.",
  },
  {
    id: "connection-saturation",
    label: "Optimizer connections",
    severity: "warning",
    kind: "state",
    check: (cur) => {
      const c = connections(cur);
      if (c === undefined) return undefined;
      return c.max > 0 && c.active / c.max > 0.9;
    },
    message: (cur) => {
      const c = connections(cur) ?? { active: 0, max: 0 };
      const pct = c.max > 0 ? Math.round((c.active / c.max) * 100) : 0;
      return `Optimizer connections are at ${pct}% (${fmt(c.active)} of ${fmt(c.max)}); change notifications from the web server may be dropped.`;
    },
    fix: "If this persists, raise the optimizer's connection limit or run fewer web server processes.",
  },
  {
    id: "optimizer-restarted",
    label: "Optimizer recently started",
    severity: "info",
    kind: "state",
    link: "#/optimizer",
    check: (cur) => {
      if (cur.daemon === "transient") return undefined;
      if (cur.daemon !== "ok") return false;
      const start = optimizerStartMs(cur);
      if (start === undefined || cur.at === undefined) return undefined;
      const age = cur.at - start;
      if (age < -5 * 60_000) return undefined; // clock skew, not a start time
      return age < WARMUP_MS;
    },
    message: (cur) => {
      const age = (cur.at ?? 0) - (optimizerStartMs(cur) ?? 0);
      const minutes = Math.floor(Math.max(0, age) / 60_000);
      const when = minutes < 1 ? "less than a minute ago" : `${minutes} min ago`;
      return `The optimizer started ${when}; its statistics cover only the time since then.`;
    },
    fix: "Nothing to do after an update or a planned restart; if it restarts on its own, its log says why.",
  },
  {
    id: "daemon-recovered",
    label: "Optimizer did not answer earlier",
    severity: "info",
    kind: "state",
    link: "#/optimizer",
    doc: DOC_DIAGNOSE,
    check: (cur) => {
      if (cur.daemon === "transient") return undefined;
      if (cur.daemon !== "ok") return false;
      if (cur.daemonDownAt === undefined || cur.at === undefined) return undefined;
      if (cur.daemonDownAt === null) return false;
      return cur.at - cur.daemonDownAt < WARMUP_MS;
    },
    message: (cur) => `The optimizer did not answer at ${formatClock(cur.daemonDownAt ?? 0)}; it is answering again.`,
    fix: "Nothing to do if it was restarting; if this repeats, check the optimizer's service log around that time.",
  },
  {
    id: "dirty-build",
    label: "Development build",
    severity: "info",
    kind: "state",
    link: "#/about",
    check: (cur) => (cur.buildStamp === undefined ? undefined : isDirtyBuild(cur.buildStamp)),
    message: (cur) =>
      `This module and console were built from a source tree with uncommitted changes (${cur.buildStamp ?? ""}).`,
    fix: "Run a released package on servers that matter; a build like this cannot be traced to a release.",
  },
  {
    id: "recent-warnings",
    label: "Warnings in the log",
    severity: "info",
    kind: "state",
    link: "#/logs?source=module&level=warning",
    doc: DOC_MESSAGES,
    check: (cur) => {
      const groups = recentWarnings(cur);
      return groups === undefined ? undefined : groups.length > 0;
    },
    message: (cur) => {
      const groups = recentWarnings(cur) ?? [];
      const total = groups.reduce((n, g) => n + g.recent, 0);
      const noun = recentNoun(groups);
      return `${plural(total, noun.one, noun.many)} in the last 15 minutes, of ${plural(groups.length, "kind", "kinds")}.`;
    },
    // A kind whose message is empty reads as the Logs page shows it, never as a bare count.
    details: (cur) =>
      (recentWarnings(cur) ?? [])
        .slice(0, 3)
        .map((g) => `×${fmt(g.recent)} ${g.template.trim() === "" ? EMPTY_MESSAGE_TEXT : capText(g.template, 160)}`),
    count: (cur) => (recentWarnings(cur) ?? []).reduce((n, g) => n + g.recent, 0),
    countNoun: (cur) => recentNoun(recentWarnings(cur) ?? []),
    fix: "Logs lists them grouped by kind, newest first; the kinds that need action have their own findings here.",
  },
];

const SEVERITY_RANK: Record<AlertSeverity, number> = { error: 0, warning: 1, info: 2 };

/** Errors, then warnings, then info; rule order is kept within a severity. */
export function rankAlerts(alerts: readonly ActiveAlert[]): ActiveAlert[] {
  return [...alerts].sort((a, b) => SEVERITY_RANK[a.severity] - SEVERITY_RANK[b.severity]);
}

export interface HealthSummary {
  text: string;
  level: "healthy" | "info" | "warning" | "error";
}

/**
 * One line for the top of the overview: how many findings, how many need
 * action, the recent-warning count whenever that finding is present (listed
 * or acknowledged), and how many are acknowledged. Missing data never reads
 * as health: an unreadable message log is "Findings unavailable", never
 * "No findings".
 */
export function healthSummary(
  alerts: readonly ActiveAlert[],
  moduleOk: boolean,
  acknowledged: readonly ActiveAlert[] = [],
  logUnavailable = false,
): HealthSummary {
  const recent = [...alerts, ...acknowledged].find((a) => a.id === "recent-warnings");
  const n = recent?.count;
  const noun = recent?.countNoun ?? { one: "warning", many: "warnings" };
  const warnings = n === undefined ? "" : ` · ${n.toLocaleString()} ${n === 1 ? noun.one : noun.many} in the last 15 min`;
  const acked = acknowledged.length > 0 ? ` · ${acknowledged.length} acknowledged` : "";
  if (alerts.length === 0) {
    if (!moduleOk) return { text: "Module statistics unavailable", level: "warning" };
    if (logUnavailable) return { text: "Findings unavailable — the message log could not be read", level: "info" };
    return { text: `No findings${warnings}${acked}`, level: "healthy" };
  }
  const action = alerts.filter((a) => a.severity !== "info").length;
  const head = alerts.length === 1 ? "1 finding" : `${alerts.length} findings`;
  const needs = action === 0 ? "" : action === 1 ? " · 1 needs action" : ` · ${action} need action`;
  const level = alerts.some((a) => a.severity === "error") ? "error" : action > 0 ? "warning" : "info";
  return { text: `${head}${needs}${warnings}${acked}`, level };
}

interface RuleState {
  since: number;
  lastFired: number;
  message: string;
  details: string[];
  count?: number;
  countNoun?: CountNoun;
}

/** Evaluates the rules sample by sample; keeps firing state and acknowledgements. */
export class AlertTracker {
  private prev: AlertSnapshot | null = null;
  /** Time and windowed counters of earlier samples with module counters, oldest first. */
  private history: CounterSample[] = [];
  private readonly state = new Map<string, RuleState>();

  constructor(
    private readonly rules: readonly AlertRule[] = ALERT_RULES,
    private readonly holdMs: number = RATE_ALERT_HOLD_MS,
    private readonly acks: AckStore = memoryAckStore(),
  ) {}

  /** Evaluate one sample; returns the findings that are not acknowledged, ranked. */
  evaluate(cur: AlertSnapshot, nowMs: number): ActiveAlert[] {
    for (const rule of this.rules) {
      const result = rule.check(cur, this.prev, this.history);
      const st = this.state.get(rule.id);
      if (result === true) {
        this.state.set(rule.id, {
          since: st ? st.since : nowMs,
          lastFired: nowMs,
          message: rule.message(cur, this.prev, this.history),
          details: rule.details ? rule.details(cur, this.prev) : [],
          count: rule.count ? rule.count(cur, this.prev) : undefined,
          countNoun: rule.countNoun ? rule.countNoun(cur, this.prev) : undefined,
        });
        continue;
      }
      if (st !== undefined && rule.kind === "rate" && nowMs - st.lastFired < this.holdMs) {
        continue; // held: keep showing the last message
      }
      if (st !== undefined && rule.kind === "state" && result === undefined) {
        continue; // transient sample: keep showing the last state and message
      }
      this.state.delete(rule.id);
      if (result === false) this.acks.delete(rule.id); // cleared: re-arm
    }
    this.prev = cur;
    this.remember(cur);
    return this.visible();
  }

  /** The findings of the last evaluation that are not acknowledged, ranked. */
  visible(): ActiveAlert[] {
    return rankAlerts(this.active().filter((a) => !this.acks.has(a.id)));
  }

  /** The findings of the last evaluation that are acknowledged, ranked. */
  acknowledged(): ActiveAlert[] {
    return rankAlerts(this.active().filter((a) => this.acks.has(a.id)));
  }

  acknowledge(id: string): void {
    this.acks.add(id);
  }

  unacknowledge(id: string): void {
    this.acks.delete(id);
  }

  /** Forget the previous sample: the next one is a baseline for rate rules. */
  resetBaseline(): void {
    this.prev = null;
    this.history = [];
  }

  /**
   * Keep the time and the windowed counters of `cur` when it carries module
   * counters, and drop every sample older than the newest one at least
   * VOLUME_WINDOW_MS old. A clock that went backwards starts the history
   * afresh.
   */
  private remember(cur: AlertSnapshot): void {
    if (cur.module === null || cur.at === undefined) return;
    const last = this.history[this.history.length - 1];
    if (last !== undefined && cur.at < last.at_ms) this.history = [];
    this.history.push({ at_ms: cur.at, counters: windowedCounters(cur.module) });
    const windowStart = cur.at - VOLUME_WINDOW_MS;
    let drop = 0;
    while (drop + 1 < this.history.length && this.history[drop + 1].at_ms <= windowStart) drop++;
    if (this.history.length - drop > MAX_HISTORY) drop = this.history.length - MAX_HISTORY;
    if (drop > 0) this.history = this.history.slice(drop);
  }

  private active(): ActiveAlert[] {
    const out: ActiveAlert[] = [];
    for (const rule of this.rules) {
      const st = this.state.get(rule.id);
      if (st === undefined) continue;
      out.push({
        id: rule.id,
        label: rule.label,
        severity: rule.severity,
        message: st.message,
        fix: rule.fix,
        link: rule.link,
        doc: rule.doc,
        details: st.details,
        count: st.count,
        countNoun: st.countNoun,
        since: st.since,
      });
    }
    return out;
  }
}
