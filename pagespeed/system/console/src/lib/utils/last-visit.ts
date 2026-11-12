// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { formatRelative } from "./format";
import { sha256Hex } from "./sha256";
import { moduleTag } from "./versions";

/**
 * "Since your last visit": what changed on the server between this browser's
 * last look at the Overview and now. Every figure on the page counts from a
 * restart; an operator who looks once a week sees no restart, upgrade or
 * configuration change in them. The console keeps one small record per
 * console in this browser's localStorage (nothing on the server, no
 * history) and compares the next visit's first answer with it.
 *
 * Only what the Overview already reads is compared: the web server's start
 * (the module statistics' time base), the module build (the console is
 * built into the module), the optimizer's start and version (its health and
 * statistics), and a digest of the configuration text the console reads
 * once for its scope. A value this visit could not read is stored as
 * unknown, and an unknown on either side claims nothing.
 */

export const LAST_VISIT_STORAGE_PREFIX = "pagespeed.console.lastVisit";

/** What one visit saw. Times are epoch milliseconds. */
export interface VisitRecord {
  /** When this browser saw it (the browser's clock). */
  at: number;
  /** The module build stamp. */
  module: string | null;
  /** When the web server started (the server's clock). */
  moduleStart: number | null;
  /** The optimizer's version. */
  optimizer: string | null;
  /** When the optimizer started (the server's clock). */
  optimizerStart: number | null;
  /** A digest of the configuration text (configDigest). */
  config: string | null;
}

/**
 * A start that moved later by less than this is the same start: the
 * optimizer may derive its start from its uptime, which can round.
 */
export const START_TOLERANCE_MS = 5_000;

/**
 * The storage key of one console's record. The whole-server console and
 * each per-host console keep their own; the origin and the admin path are
 * part of the key as well, so two consoles never read each other's record.
 */
export function lastVisitKey(origin: string, basePath: string, isGlobal: boolean, host: string): string {
  return `${LAST_VISIT_STORAGE_PREFIX}:${origin}${basePath}:${isGlobal ? "global" : `host=${host}`}`;
}

/**
 * A digest of the configuration text. The cache invalidation time is part
 * of that text but is not configuration (a cache flush moves it), so its
 * line is left out. The server ends the text differently with and without
 * that time ("(none)" has no line break after it, a time has one, and an
 * unset time prints no line at all), so the end of the text is trimmed too.
 */
export function configDigest(text: string): string {
  const kept = text
    .split("\n")
    .filter((line) => !line.startsWith("Invalidation Timestamp:"))
    .join("\n")
    .trimEnd();
  return sha256Hex(kept).slice(0, 32);
}

const MAX_TEXT = 128;

function text(v: unknown): string | null {
  return typeof v === "string" && v.length > 0 && v.length <= MAX_TEXT ? v : null;
}

function time(v: unknown): number | null {
  return typeof v === "number" && Number.isFinite(v) && v > 0 ? v : null;
}

/** The record stored under `key`, checked field by field; null when there is none or it is unreadable. */
export function readVisit(storage: Storage | null, key: string): VisitRecord | null {
  if (storage === null) return null;
  try {
    const raw = storage.getItem(key);
    if (typeof raw !== "string") return null;
    const parsed: unknown = JSON.parse(raw);
    if (typeof parsed !== "object" || parsed === null) return null;
    const r = parsed as Record<string, unknown>;
    const at = time(r.at);
    if (at === null) return null;
    return {
      at,
      module: text(r.module),
      moduleStart: time(r.moduleStart),
      optimizer: text(r.optimizer),
      optimizerStart: time(r.optimizerStart),
      config: text(r.config),
    };
  } catch {
    return null;
  }
}

/** Stores `record`; a browser that refuses storage just does not remember. */
export function writeVisit(storage: Storage | null, key: string, record: VisitRecord): void {
  if (storage === null) return;
  try {
    storage.setItem(key, JSON.stringify(record));
  } catch {
    // Ignore: the next visit shows nothing, as a first visit does.
  }
}

function restarted(before: number | null, now: number | null): boolean {
  return before !== null && now !== null && now > before + START_TOLERANCE_MS;
}

function versionChange(what: string, before: string, now: string): string {
  return `${what} changed from ${before} to ${now}`;
}

/** What changed between two visits, in words, in the order the line names them. */
export function visitChanges(before: VisitRecord, now: VisitRecord): string[] {
  const changes: string[] = [];
  if (restarted(before.moduleStart, now.moduleStart)) changes.push("the web server restarted or reloaded");
  const moduleChanged = before.module !== null && now.module !== null && before.module !== now.module;
  // A new module build can list its options differently with the same
  // configuration: the configuration is compared on the same build only.
  if (!moduleChanged && before.config !== null && now.config !== null && before.config !== now.config) {
    changes.push("the configuration changed");
  }
  if (moduleChanged && before.module !== null && now.module !== null) {
    const a = moduleTag(before.module);
    const b = moduleTag(now.module);
    changes.push(a === b ? versionChange("the module build", before.module, now.module) : versionChange("the module", a, b));
  }
  if (restarted(before.optimizerStart, now.optimizerStart)) changes.push("the optimizer restarted");
  if (before.optimizer !== null && now.optimizer !== null && before.optimizer !== now.optimizer) {
    changes.push(versionChange("the optimizer", before.optimizer, now.optimizer));
  }
  return changes;
}

export interface SinceLastVisit {
  /** "Since your last visit 6 d ago: the web server restarted or reloaded." */
  text: string;
  /** The last visit's time, for a tooltip. */
  iso: string;
}

/** The line, or null when there was no earlier visit or nothing this console can see changed. */
export function sinceLastVisit(before: VisitRecord | null, now: VisitRecord): SinceLastVisit | null {
  if (before === null || before.at >= now.at) return null;
  const changes = visitChanges(before, now);
  if (changes.length === 0) return null;
  const list = changes.length === 1 ? changes[0] : `${changes.slice(0, -1).join(", ")} and ${changes[changes.length - 1]}`;
  return {
    text: `Since your last visit ${formatRelative(before.at, now.at)}: ${list}.`,
    iso: new Date(before.at).toISOString(),
  };
}

/** `localStorage` when the environment has one; merely touching it can throw. */
export function defaultVisitStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

/**
 * Once per page open: the first good answer is compared with the stored
 * record and then becomes the stored record. Later answers, and the
 * Overview opened again from another page, keep that comparison: storing
 * every refresh would compare the page with itself.
 */
export class VisitOnce {
  #started = false;
  #result: SinceLastVisit | null = null;
  readonly #storage: Storage | null;
  #resolveSettled: (line: SinceLastVisit | null) => void = () => {};
  /**
   * Resolves with this page open's line once it is settled. A page mounted
   * before that (also one opened again while the comparison waited) shows
   * the line when it comes.
   */
  readonly settled: Promise<SinceLastVisit | null>;

  constructor(storage: Storage | null = defaultVisitStorage()) {
    this.#storage = storage;
    this.settled = new Promise((resolve) => {
      this.#resolveSettled = resolve;
    });
  }

  /** True the first time only: the caller then settles this visit. */
  begin(): boolean {
    if (this.#started) return false;
    this.#started = true;
    return true;
  }

  /** Compares `now` with the record under `key`, stores `now`, and returns the line. */
  settle(key: string, now: VisitRecord): SinceLastVisit | null {
    const before = readVisit(this.#storage, key);
    writeVisit(this.#storage, key, now);
    this.#result = sinceLastVisit(before, now);
    this.#resolveSettled(this.#result);
    return this.#result;
  }

  /** This page open's line, once settled. */
  get result(): SinceLastVisit | null {
    return this.#result;
  }
}
