// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The Logs page's timeline: the module's message log and the optimizer's
 * log ring as one list. Every entry keeps its source; levels share one scale
 * (fatal, error, warning, info, debug -- the module has no debug and the
 * optimizer no fatal); repeats group by (source, level, message template)
 * within a source, never across. Both endpoints are untrusted and their text
 * carries visitor-controlled bytes: nothing here interprets it, and the page
 * renders it as text (plus the one http(s) link rule, linkify.ts).
 */

import type { Msg } from "./message-cursor";
import { EMPTY_MESSAGE_TEXT, MAX_GROUP_ENTRIES, messageLevel } from "./message-groups";
import { messageLineBody, messageLineTimeMs, messageTemplate } from "./message-template";
import { displayLogText, type LogRow } from "./logs-buffer";
import { textNamesHost } from "./host-lens";

export type LogSource = "module" | "optimizer";
export type SourceFilter = "both" | LogSource;

export const TIMELINE_LEVELS = ["fatal", "error", "warning", "info", "debug"] as const;
export type TimelineLevel = (typeof TIMELINE_LEVELS)[number];

/** At most this many rows render; the note under the list says so. */
export const TIMELINE_RENDER_CAP = 500;

const RANK: Record<TimelineLevel, number> = { fatal: 0, error: 1, warning: 2, info: 3, debug: 4 };

export function isKnownTimelineLevel(level: string | null): level is TimelineLevel {
  return level !== null && Object.prototype.hasOwnProperty.call(RANK, level);
}

/**
 * The level filter a link presets: absent, warnings and worse; a known
 * level, that level and worse; anything else, every level.
 */
export function timelineLevelFilter(level: string | null): Record<TimelineLevel, boolean> {
  const max = level === null ? RANK.warning : isKnownTimelineLevel(level) ? RANK[level] : RANK.debug;
  return {
    fatal: RANK.fatal <= max,
    error: RANK.error <= max,
    warning: RANK.warning <= max,
    info: RANK.info <= max,
    debug: RANK.debug <= max,
  };
}

/** The source= parameter: "module" or "optimizer"; anything else is both. */
export function parseSourceFilter(value: string | null): SourceFilter {
  return value === "module" || value === "optimizer" ? value : "both";
}

export interface TimelineEntry {
  /** Stable for the entry while it is retained ("m<n>" module, "o<id>" optimizer). */
  id: string;
  source: LogSource;
  /** The entry's own level; an optimizer level this console predates is kept as sent. */
  level: string;
  /** Epoch ms, or null when the entry carries no readable time. */
  ts: number | null;
  /** The text without the module's line header, control characters made visible. */
  text: string;
  /** The optimizer's "source/module"; "" for the module's messages. */
  where: string;
}

/**
 * The module's retained messages (newest first, as mergeMessages keeps
 * them). An entry's id counts from the oldest retained message, so it stays
 * the same while newer messages arrive in front of it.
 */
export function moduleEntries(items: ReadonlyArray<Msg>): TimelineEntry[] {
  const out: TimelineEntry[] = [];
  const n = items.length;
  for (let i = 0; i < n; i++) {
    const item = items[i];
    // The endpoint is untrusted: a missing or non-string message is an
    // empty line, never a reason for the whole page to fail.
    const raw: unknown = item?.message;
    const line = typeof raw === "string" ? raw : "";
    const body = messageLineBody(line);
    out.push({
      id: `m${n - 1 - i}`,
      source: "module",
      level: messageLevel(item?.severity),
      ts: messageLineTimeMs(line),
      text: body === "" ? EMPTY_MESSAGE_TEXT : body,
      where: "",
    });
  }
  return out;
}

/** The optimizer's buffered entries (markers are counted by streamNotes, not listed). */
export function optimizerEntries(rows: ReadonlyArray<LogRow>): TimelineEntry[] {
  const out: TimelineEntry[] = [];
  for (const row of rows) {
    if (row.kind !== "entry") continue;
    const where = row.source && row.module ? `${row.source}/${row.module}` : row.source || row.module;
    out.push({
      id: `o${row.id}`,
      source: "optimizer",
      level: row.level,
      ts: row.ts > 0 ? row.ts : null,
      text: displayLogText(row.message),
      where: displayLogText(where),
    });
  }
  return out;
}

export interface StreamNotes {
  /** The optimizer restarted while this page was open. */
  restarts: number;
  /** Entries the optimizer's ring moved past before this page read them. */
  dropped: number;
  /** Entries the optimizer could not keep (logging faster than it records). */
  notRecorded: number;
}

export function streamNotes(rows: ReadonlyArray<LogRow>): StreamNotes {
  const notes: StreamNotes = { restarts: 0, dropped: 0, notRecorded: 0 };
  for (const row of rows) {
    if (row.kind === "restart") notes.restarts += 1;
    else if (row.kind === "gap") notes.dropped += Math.max(0, row.toSeq - row.fromSeq - 1);
    else if (row.kind === "shed") notes.notRecorded += row.count;
  }
  return notes;
}

export interface TimelineFilter {
  source: SourceFilter;
  levels: Record<TimelineLevel, boolean>;
  /** Matched case-insensitively against the text and the optimizer's source/module. */
  text: string;
  /** The host lens: keep only entries whose URLs name this host exactly; null or absent, no narrowing. */
  host?: string | null;
}

export function filterEntries(entries: ReadonlyArray<TimelineEntry>, filter: TimelineFilter): TimelineEntry[] {
  const needle = filter.text.trim().toLowerCase();
  return entries.filter((e) => {
    if (filter.source !== "both" && e.source !== filter.source) return false;
    // A level this console predates is SHOWN: hiding unknown levels would
    // hide exactly the news a newer optimizer adds.
    if (isKnownTimelineLevel(e.level) && !filter.levels[e.level]) return false;
    const host = filter.host ?? null;
    if (host !== null && !textNamesHost(e.text, host)) return false;
    return needle === "" || e.text.toLowerCase().includes(needle) || e.where.toLowerCase().includes(needle);
  });
}

/** Entries per level, for the filter's counts; a level this console predates counts as debug. */
export function levelCounts(entries: ReadonlyArray<TimelineEntry>): Record<TimelineLevel, number> {
  const counts: Record<TimelineLevel, number> = { fatal: 0, error: 0, warning: 0, info: 0, debug: 0 };
  for (const e of entries) counts[isKnownTimelineLevel(e.level) ? e.level : "debug"] += 1;
  return counts;
}

export interface TimelineGroup {
  key: string;
  source: LogSource;
  level: string;
  /** The newest entry's text: the real message, not the template. */
  text: string;
  where: string;
  count: number;
  lastTs: number | null;
  /** At most the cap, newest first. */
  entries: Array<{ ts: number | null; text: string }>;
}

/**
 * The timeline's rows, newest first. Grouped: one row per (source, level,
 * message template) -- repeats fold, sources never mix. Not grouped: one
 * row per entry. Rows and entries without a readable time sort last.
 */
export function groupTimeline(
  entries: ReadonlyArray<TimelineEntry>,
  grouped: boolean,
  cap: number = MAX_GROUP_ENTRIES,
): TimelineGroup[] {
  const byKey = new Map<string, TimelineGroup>();
  const order: TimelineGroup[] = [];
  for (const e of entries) {
    const key = grouped ? `${e.source}\u0000${e.level}\u0000${messageTemplate(e.text)}` : e.id;
    let group = byKey.get(key);
    if (group === undefined) {
      group = { key, source: e.source, level: e.level, text: e.text, where: e.where, count: 0, lastTs: e.ts, entries: [] };
      byKey.set(key, group);
      order.push(group);
    }
    group.count += 1;
    if (e.ts !== null && (group.lastTs === null || e.ts > group.lastTs)) {
      group.lastTs = e.ts;
      group.text = e.text;
      group.where = e.where;
    }
    group.entries.push({ ts: e.ts, text: e.text });
  }
  for (const group of order) {
    group.entries.sort((a, b) => (b.ts ?? -1) - (a.ts ?? -1));
    if (group.entries.length > cap) group.entries.length = cap;
  }
  return order.sort((a, b) => (b.lastTs ?? -1) - (a.lastTs ?? -1));
}
