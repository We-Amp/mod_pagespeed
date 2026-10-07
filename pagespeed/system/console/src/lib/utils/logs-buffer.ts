// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The Logs page's client buffer for the optimizer's log: a bounded,
 * ascending list of log rows fed one poll at a time (a poll may read
 * several pages to catch up), plus the pure helpers the page renders
 * through (level + text filters, the render window, local-time formatting).
 *
 * The page shape is the daemon's GET /v1/logs document, forwarded by the
 * module's daemon proxy: entries ascending in `seq`, a `next_since` cursor,
 * the retained bounds, `gap` when the ring wrapped past the cursor, `more`
 * when another page is waiting, `shed_total`, and a `stream_id` that changes
 * when the optimizer restarts. All of it is untrusted: every field is
 * guarded and anything unusable is dropped, never rendered as NaN or
 * "undefined".
 */

import type { DaemonLogEntry, DaemonLogsResponse } from "$lib/api/types";
import { globalConsoleHref } from "$lib/utils/urls-api";

/** The optimizer's log levels, most severe first. */
export const LOG_LEVELS = ["error", "warning", "info", "debug"] as const;
export type LogLevel = (typeof LOG_LEVELS)[number];

const LEVEL_RANK: Record<LogLevel, number> = { error: 0, warning: 1, info: 2, debug: 3 };

/** Whether `level` is one of the optimizer's four levels. */
export function isKnownLogLevel(level: string | null): level is LogLevel {
  return level !== null && Object.prototype.hasOwnProperty.call(LEVEL_RANK, level);
}

/** A `level=` link shows that level and anything more severe; absent or unknown shows all. */
export function logsLevelFilter(level: string | null): Record<LogLevel, boolean> {
  const max = isKnownLogLevel(level) ? LEVEL_RANK[level] : LEVEL_RANK.debug;
  return {
    error: LEVEL_RANK.error <= max,
    warning: LEVEL_RANK.warning <= max,
    info: LEVEL_RANK.info <= max,
    debug: LEVEL_RANK.debug <= max,
  };
}

/**
 * A buffer row: a log entry, or a marker where the stream is known to have
 * a hole (the ring wrapped past the poll cursor), a seam (the optimizer
 * restarted and its sequence numbers started over), or entries the
 * optimizer could not keep. Markers sit in the buffer so follow mode,
 * filters and the render window stay oblivious to them. `id` is unique per
 * buffer and increases with arrival (daemon seqs repeat across restarts).
 */
export type LogRow =
  | {
      kind: "entry";
      id: number;
      seq: number;
      ts: number;
      level: string;
      source: string;
      module: string;
      message: string;
      /** message, source and module, lowercased once for the text filter. */
      haystack: string;
    }
  | { kind: "gap"; id: number; fromSeq: number; toSeq: number }
  | { kind: "restart"; id: number }
  | { kind: "shed"; id: number; count: number };

export type LogEntryRow = Extract<LogRow, { kind: "entry" }>;

export interface LogBuffer {
  rows: LogRow[];
  /** The cursor the next read passes as `since`; null: read the newest page. */
  nextSince: number | null;
  /** The optimizer process the rows came from; null until a page names one. */
  streamId: string | null;
  /** The last `shed_total` seen in this stream. */
  shedTotal: number | null;
  /** The optimizer restarted: read the newest page again at once. */
  rereadNeeded: boolean;
  /** The next row id. */
  nextId: number;
}

export const MAX_LOG_ROWS = 2000;

/** At most this many matching rows render; the note under the stream says so. */
export const RENDER_CAP = 500;

/** Entries per read: the daemon's maximum page. */
export const LOGS_PAGE_LIMIT = 500;

/** Extra reads one poll may make to catch up with a busy optimizer. */
export const MAX_CATCH_UP_READS = 5;

export const EMPTY_LOG_BUFFER: LogBuffer = {
  rows: [],
  nextSince: null,
  streamId: null,
  shedTotal: null,
  rereadNeeded: false,
  nextId: 0,
};

/** A usable sequence number or count: a non-negative integer, else null. */
function asSeq(value: unknown): number | null {
  return typeof value === "number" && Number.isInteger(value) && value >= 0 ? value : null;
}

function isObject(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

/**
 * A non-string `message` rendered as text. A deeply nested value can make
 * `JSON.stringify` throw (`RangeError: Maximum call stack size exceeded`);
 * a fixed placeholder stands in rather than letting that throw take down
 * the fold.
 */
function messageText(value: unknown): string {
  if (typeof value === "string") return value;
  try {
    return JSON.stringify(value) ?? "";
  } catch {
    return "[message not shown]";
  }
}

/** One wire entry as a row, or null when it is not an object with a usable seq. */
function toEntryRow(raw: unknown, id: number): LogEntryRow | null {
  if (!isObject(raw)) return null;
  const entry = raw as DaemonLogEntry;
  const seq = asSeq(entry.seq);
  if (seq === null) return null;
  const source = typeof entry.source === "string" ? entry.source : "";
  const module = typeof entry.module === "string" ? entry.module : "";
  // A message is operator-facing text: byte-preserved, never interpreted.
  const message = messageText(entry.message);
  return {
    kind: "entry",
    id,
    seq,
    ts: typeof entry.timestamp === "number" && Number.isFinite(entry.timestamp) ? entry.timestamp : 0,
    level: typeof entry.level === "string" ? entry.level : "info",
    source,
    module,
    message,
    haystack: `${message}\n${source}\n${module}`.toLowerCase(),
  };
}

function capRows(rows: LogRow[], cap: number): LogRow[] {
  return rows.length > cap ? rows.slice(rows.length - cap) : rows;
}

/**
 * Fold one page of the log ring into the buffer. Pure: returns a new state
 * (or the same one when nothing changed), never mutates.
 *
 * - Anything but a page object changes nothing; entries that are not
 *   objects with a usable seq are dropped.
 * - A `stream_id` different from the buffer's means the optimizer
 *   restarted: a restart marker is added, the page's entries are dropped
 *   (they answer the old cursor), the cursor is reset and a re-read is
 *   flagged, so the caller reads the new process's newest page at once.
 * - Entries at or below the cursor are skipped, so folding the same page
 *   twice adds nothing.
 * - A rising `shed_total` adds a marker with the count; `gap: true`
 *   against an existing cursor adds a dropped-entries marker.
 * - A first read that found nothing keeps the cursor unset, so the first
 *   entry a fresh optimizer logs (seq 0) is not skipped; within one stream
 *   the cursor never moves backwards.
 * - Oldest rows are dropped past `cap`.
 */
export function appendLogPage(state: LogBuffer, page: unknown, cap: number = MAX_LOG_ROWS): LogBuffer {
  if (!isObject(page)) return state;
  const p = page as DaemonLogsResponse;
  if (p.entries !== undefined && !Array.isArray(p.entries)) return state;
  const rawEntries: unknown[] = p.entries ?? [];
  const streamId = typeof p.stream_id === "string" && p.stream_id !== "" ? p.stream_id : null;
  const shedTotal = asSeq(p.shed_total);

  const rows = [...state.rows];
  let nextId = state.nextId;

  if (state.streamId !== null && streamId !== null && streamId !== state.streamId) {
    rows.push({ kind: "restart", id: nextId++ });
    return { rows: capRows(rows, cap), nextSince: null, streamId, shedTotal, rereadNeeded: true, nextId };
  }

  if (state.shedTotal !== null && shedTotal !== null && shedTotal > state.shedTotal) {
    rows.push({ kind: "shed", id: nextId++, count: shedTotal - state.shedTotal });
  }
  if (p.gap === true && state.nextSince !== null) {
    rows.push({ kind: "gap", id: nextId++, fromSeq: state.nextSince, toSeq: asSeq(p.oldest_seq) ?? 0 });
  }

  let watermark = state.nextSince ?? -1;
  let appended = 0;
  for (const raw of rawEntries) {
    const row = toEntryRow(raw, nextId);
    if (row === null || row.seq <= watermark) continue;
    rows.push(row);
    nextId += 1;
    watermark = row.seq;
    appended += 1;
  }

  let nextSince: number | null;
  if (state.nextSince === null && appended === 0) {
    nextSince = null;
  } else {
    nextSince = asSeq(p.next_since) ?? (appended > 0 ? watermark : state.nextSince);
    if (state.nextSince !== null && nextSince !== null && nextSince < state.nextSince) {
      nextSince = state.nextSince;
    }
  }
  const nextStream = state.streamId ?? streamId;
  const nextShed = shedTotal ?? state.shedTotal;

  if (
    rows.length === state.rows.length &&
    nextSince === state.nextSince &&
    nextStream === state.streamId &&
    nextShed === state.shedTotal &&
    !state.rereadNeeded
  ) {
    return state; // the common idle poll
  }
  return { rows: capRows(rows, cap), nextSince, streamId: nextStream, shedTotal: nextShed, rereadNeeded: false, nextId };
}

/** Whether a poll should read again at once: `more`, a full page past a cursor, or a restart. */
export function needsCatchUp(page: unknown, state: LogBuffer): boolean {
  if (state.rereadNeeded) return true;
  if (!isObject(page)) return false;
  const p = page as DaemonLogsResponse;
  if (p.more === true) return true;
  return state.nextSince !== null && Array.isArray(p.entries) && p.entries.length >= LOGS_PAGE_LIMIT;
}

/**
 * One poll: read a page, fold it, and while `needsCatchUp` says so read
 * again from the new cursor — at most `maxCatchUp` extra reads, so one poll
 * covers up to (1 + maxCatchUp) pages. The first read's failure (a busy 429
 * included) rejects, so the poller's busy/error rules apply; a failed
 * catch-up read, or a catch-up page that cannot be folded, ends the poll
 * quietly with what it has, and the next poll continues from the cursor.
 */
export async function readLogPages(
  read: (since?: number) => Promise<unknown>,
  state: LogBuffer,
  maxCatchUp: number = MAX_CATCH_UP_READS,
): Promise<{ state: LogBuffer; page: unknown; reads: number }> {
  let page = await read(state.nextSince ?? undefined);
  let next = appendLogPage(state, page);
  let reads = 1;
  while (reads <= maxCatchUp && needsCatchUp(page, next)) {
    let fetched: unknown;
    try {
      fetched = await read(next.nextSince ?? undefined);
    } catch {
      break;
    }
    try {
      next = appendLogPage(next, fetched);
    } catch {
      break;
    }
    page = fetched;
    reads += 1;
  }
  return { state: next, page, reads };
}

export function filterLogRows(rows: LogRow[], enabledLevels: Record<LogLevel, boolean>, text: string): LogRow[] {
  const needle = text.trim().toLowerCase();
  return rows.filter((row) => {
    if (row.kind !== "entry") return true;
    // A level this console predates is SHOWN: hiding unknown levels would
    // hide exactly the news a newer daemon version adds.
    if (isKnownLogLevel(row.level) && !enabledLevels[row.level]) return false;
    return needle === "" || row.haystack.includes(needle);
  });
}

/**
 * The rows to render. Following (`anchorId` null): the newest `cap`.
 * Not following: `cap` rows from the anchor on, so rows arriving at the
 * bottom never shift what the reader is looking at (an anchor evicted by
 * the buffer cap falls back to the oldest rows left).
 */
export function visibleLogRows(filtered: LogRow[], anchorId: number | null, cap: number = RENDER_CAP): LogRow[] {
  if (anchorId === null) return filtered.slice(-cap);
  const start = filtered.findIndex((row) => row.id >= anchorId);
  return start === -1 ? filtered.slice(-cap) : filtered.slice(start, start + cap);
}

/** Entry rows (passing the filters) that arrived since `fromId`; 0 while following. */
export function countUnseen(filtered: LogRow[], fromId: number | null): number {
  if (fromId === null) return 0;
  return filtered.filter((row) => row.kind === "entry" && row.id >= fromId).length;
}

/** Unicode's "Symbol for Delete" (the DEL control picture). */
const DEL_PICTURE = "␡";

/**
 * A log message, or a source/module value, shown so an embedded control
 * character can never be mistaken for a real line break or for whitespace
 * that isn't there: LF becomes the return glyph, CR its own control
 * picture, and every other C0 control plus DEL becomes the matching
 * glyph from Unicode's control-pictures block. A tab stays a tab, and
 * anything else (including a bidi-override character) passes through
 * unchanged. Display only: the buffer keeps the original bytes.
 */
export function displayLogText(s: string): string {
  let out = "";
  for (const ch of s) {
    const code = ch.codePointAt(0) ?? 0;
    if (code === 0x0a) {
      out += "⏎"; // LF -> ⏎
    } else if (code === 0x09) {
      out += ch; // TAB stays a tab
    } else if (code === 0x7f) {
      out += DEL_PICTURE;
    } else if (code < 0x20) {
      out += String.fromCodePoint(0x2400 + code); // CR (0x0d) lands on ␍
    } else {
      out += ch;
    }
  }
  return out;
}

/** Local "2026-10-02 10:25:51.123": the date as well, like the module's messages on the Logs page. */
export function formatLogTimestamp(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return "—";
  const d = new Date(ms);
  if (Number.isNaN(d.getTime())) return "—"; // out of Date's range (> 8.64e15)
  const p2 = (n: number) => n.toString().padStart(2, "0");
  const date = `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())}`;
  const time = `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`;
  return `${date} ${time}.${d.getMilliseconds().toString().padStart(3, "0")}`;
}

/** The whole-server console's Logs page (keeping a known level preset), from a per-vhost mount. */
export function globalConsoleLogsHref(basePath: string, level: string | null): string | null {
  const hash = isKnownLogLevel(level) ? `#/logs?level=${encodeURIComponent(level)}` : "#/logs";
  return globalConsoleHref(basePath, hash);
}
