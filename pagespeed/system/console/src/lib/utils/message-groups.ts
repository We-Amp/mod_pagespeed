// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The module's message log as groups of one level and message template: from
 * the module's own grouped answer (message_history?grouped=1) when it has
 * one, otherwise grouped here from the plain list with the same template
 * rules (message-template.ts). Both endpoints are untrusted: every field is
 * guarded, and template text — which carries visitor-controlled bytes — is
 * only ever rendered as text.
 */

import { messageLineBody, messageLineTimeMs, messageTemplate } from "./message-template";

export type MessageLevel = "fatal" | "error" | "warning" | "info";

export interface MessageGroup {
  level: MessageLevel;
  template: string;
  /** Lines of this group in the retained log. */
  count: number;
  /** Lines of this group within the window. */
  recent: number;
  /** Epoch ms of the newest line; 0 when no line carried a readable time. */
  lastMs: number;
}

export interface MessageDigest {
  groups: MessageGroup[];
  /** "module": the module grouped it; "console": an older module, grouped here. */
  source: "module" | "console";
  /** The module left rows out to stay under its size cap. */
  truncated: boolean;
  /** Epoch ms of the oldest retained line that carried a time; null when unknown. */
  firstMs?: number | null;
}

/**
 * The window a count over `windowMs` honestly covers: "in the last 15 min"
 * when the retained log reaches back that far (or its start is unknown), and
 * "in the retained messages since HH:MM" when the log is younger than the
 * window, so a 4-minute buffer never reads as 15 minutes.
 */
export function coveredWindowText(
  firstMs: number | null,
  nowMs: number,
  windowMs: number = MESSAGE_WINDOW_MS,
  clock: (ms: number) => string = clockHm,
  longUnit = false,
): string {
  if (firstMs !== null && firstMs > 0 && nowMs - firstMs < windowMs) {
    return `in the retained messages since ${clock(firstMs)}`;
  }
  return `in the last ${windowMs / 60_000} ${longUnit ? "minutes" : "min"}`;
}

function clockHm(ms: number): string {
  const d = new Date(ms);
  const pad = (n: number) => n.toString().padStart(2, "0");
  return `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** The window "recent" counts over: the last 15 minutes. */
export const MESSAGE_WINDOW_SECONDS = 900;
export const MESSAGE_WINDOW_MS = MESSAGE_WINDOW_SECONDS * 1000;

const LEVELS: ReadonlySet<string> = new Set(["fatal", "error", "warning", "info"]);

function levelOf(value: unknown): MessageLevel {
  return typeof value === "string" && LEVELS.has(value) ? (value as MessageLevel) : "info";
}

/** A message entry's level: one of the four, anything else is info. */
export function messageLevel(value: unknown): MessageLevel {
  return levelOf(value);
}

/** A non-negative whole number, else 0. */
function wholeOf(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) && value >= 0 ? Math.floor(value) : 0;
}

/** The module's order: newest first, then most frequent, then level and template. */
function compareGroups(a: MessageGroup, b: MessageGroup): number {
  if (a.lastMs !== b.lastMs) return b.lastMs - a.lastMs;
  if (a.count !== b.count) return b.count - a.count;
  if (a.level !== b.level) return a.level < b.level ? -1 : 1;
  if (a.template === b.template) return 0;
  return a.template < b.template ? -1 : 1;
}

/**
 * Group a plain message list (oldest first, as the module answers it). A
 * continuation line takes the time of the header line before it; a line
 * with no readable time is never recent.
 */
export function groupMessages(messages: unknown, nowMs: number, windowMs: number = MESSAGE_WINDOW_MS): MessageGroup[] {
  return groupMessagesWithStart(messages, nowMs, windowMs).groups;
}

/** groupMessages plus the time of the oldest line that carried one (null when none did). */
function groupMessagesWithStart(
  messages: unknown,
  nowMs: number,
  windowMs: number,
): { groups: MessageGroup[]; firstMs: number | null } {
  if (!Array.isArray(messages)) return { groups: [], firstMs: null };
  let firstMs: number | null = null;
  const byKey = new Map<string, MessageGroup>();
  const windowStart = nowMs - windowMs;
  let current: number | null = null;
  for (const entry of messages) {
    if (entry === null || typeof entry !== "object") continue;
    const text: unknown = (entry as { message?: unknown }).message;
    if (typeof text !== "string" || text === "") continue;
    const level = levelOf((entry as { severity?: unknown }).severity);
    const body = messageLineBody(text);
    if (body.length < text.length) current = messageLineTimeMs(text);
    if (current !== null && current > 0 && (firstMs === null || current < firstMs)) firstMs = current;
    const template = messageTemplate(body);
    const key = `${level}\u0000${template}`;
    let group = byKey.get(key);
    if (group === undefined) {
      group = { level, template, count: 0, recent: 0, lastMs: 0 };
      byKey.set(key, group);
    }
    group.count += 1;
    if (current !== null && current > group.lastMs) group.lastMs = current;
    if (current !== null && current >= windowStart) group.recent += 1;
  }
  return { groups: [...byKey.values()].sort(compareGroups), firstMs };
}

/**
 * Either answer of message_history as a digest: `groups` (the module grouped
 * it) or `messages` (an older module that ignores grouped=1). Anything else —
 * including an answer that is not an object — is null: no data.
 */
export function toMessageDigest(answer: unknown, nowMs: number, windowMs: number = MESSAGE_WINDOW_MS): MessageDigest | null {
  if (answer === null || typeof answer !== "object" || Array.isArray(answer)) return null;
  const a = answer as Record<string, unknown>;
  if (Array.isArray(a.groups)) {
    const groups: MessageGroup[] = [];
    for (const row of a.groups) {
      if (row === null || typeof row !== "object") continue;
      const r = row as Record<string, unknown>;
      if (typeof r.template !== "string") continue;
      groups.push({
        level: levelOf(r.level),
        template: r.template,
        count: wholeOf(r.count),
        recent: wholeOf(r.recent),
        lastMs: wholeOf(r.last_ms),
      });
    }
    const first = wholeOf(a.first_ms);
    return {
      groups: groups.sort(compareGroups),
      source: "module",
      truncated: a.truncated === true,
      firstMs: first > 0 ? first : null,
    };
  }
  if (Array.isArray(a.messages)) {
    const { groups, firstMs } = groupMessagesWithStart(a.messages, nowMs, windowMs);
    return { groups, source: "console", truncated: false, firstMs };
  }
  return null;
}

export interface DisplayEntry {
  ts: number | null;
  text: string;
}

export interface DisplayGroup {
  key: string;
  severity: MessageLevel;
  text: string;
  count: number;
  lastTs: number | null;
  entries: DisplayEntry[];
}

export const MAX_GROUP_ENTRIES = 50;

/** What an entry without a text message shows. */
export const EMPTY_MESSAGE_TEXT = "(empty)";

/**
 * The module's messages grouped for display: messages (newest first) folded
 * by severity and template, ordered by their newest line. A row shows the text of its newest
 * line without the header, the count, and at most `cap` individual entries
 * (newest first). Lines without a readable time sort last.
 */
export function groupForDisplay(
  items: ReadonlyArray<{ severity: string; message: string }>,
  cap: number = MAX_GROUP_ENTRIES,
): DisplayGroup[] {
  const byKey = new Map<string, DisplayGroup>();
  const order: DisplayGroup[] = [];
  for (const item of items) {
    // The endpoint is untrusted: a missing or non-string message is an empty
    // line, never a reason for the whole page to fail.
    const raw: unknown = item?.message;
    const line = typeof raw === "string" ? raw : "";
    const severity = levelOf(item?.severity);
    const body = messageLineBody(line);
    const text = body === "" ? EMPTY_MESSAGE_TEXT : body;
    const ts = messageLineTimeMs(line);
    const key = `${severity}\u0000${messageTemplate(text)}`;
    let group = byKey.get(key);
    if (group === undefined) {
      group = { key, severity, text, count: 0, lastTs: ts, entries: [] };
      byKey.set(key, group);
      order.push(group);
    }
    group.count += 1;
    if (ts !== null && (group.lastTs === null || ts > group.lastTs)) {
      group.lastTs = ts;
      group.text = text;
    }
    if (group.entries.length < cap) group.entries.push({ ts, text });
  }
  return order.sort((a, b) => (b.lastTs ?? -1) - (a.lastTs ?? -1));
}
