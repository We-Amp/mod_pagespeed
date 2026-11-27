// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// Debug console log utilities — filtering, formatting, constants.
// ---------------------------------------------------------------------------

import type { LogEntry, LogLevel, LogSource } from '@pagespeed/api-client';

/** LogEntry with a client-assigned ID for stable Svelte #each keying. */
export interface ClientLogEntry extends LogEntry {
  _clientId: number;
}

export const ALL_SOURCES: LogSource[] = ['worker', 'cache', 'chrome'];
export const ALL_LEVELS: LogLevel[] = ['debug', 'info', 'warning', 'error'];
export const MAX_LOG_ENTRIES = 5000;

let _nextClientId = 0;

/** Assign a unique client ID to a log entry. */
export function tagEntry(entry: LogEntry): ClientLogEntry {
  return { ...entry, _clientId: _nextClientId++ };
}

/** Reset the client ID counter (for testing). */
export function resetClientIdCounter(): void {
  _nextClientId = 0;
}

/**
 * Append an entry to the ring buffer, returning a new array.
 * Trims the oldest entry if the buffer is at capacity.
 */
export function appendToBuffer(
  entries: ClientLogEntry[],
  entry: ClientLogEntry,
  max: number = MAX_LOG_ENTRIES,
): ClientLogEntry[] {
  if (entries.length >= max) {
    return [...entries.slice(entries.length - max + 1), entry];
  }
  return [...entries, entry];
}

/** Module → CSS color for badge rendering. */
export const MODULE_COLORS: Record<string, string> = {
  html_assembly: 'var(--ps-accent)',
  image: '#22c55e',
  css: '#a855f7',
  js: '#f97316',
  cache: '#14b8a6',
  chrome: '#ec4899',
  config: '#6b7280',
  svg: '#8b5cf6',
};

/**
 * Filter log entries by source, level, and search text.
 */
export function filterLogs(
  entries: ClientLogEntry[],
  enabledSources: Record<LogSource, boolean>,
  enabledLevels: Record<LogLevel, boolean>,
  searchText: string,
): ClientLogEntry[] {
  const needle = searchText.toLowerCase();
  return entries.filter((entry) => {
    if (!enabledSources[entry.source]) return false;
    if (!enabledLevels[entry.level]) return false;
    if (needle && !matchesSearch(entry, needle)) return false;
    return true;
  });
}

/**
 * Check if a log entry matches a search needle (case-insensitive).
 * Searches message, module, source, and details JSON.
 */
export function matchesSearch(entry: LogEntry, needle: string): boolean {
  if (entry.message.toLowerCase().includes(needle)) return true;
  if (entry.module.toLowerCase().includes(needle)) return true;
  if (entry.source.toLowerCase().includes(needle)) return true;
  if (entry.details) {
    const detailStr = JSON.stringify(entry.details).toLowerCase();
    if (detailStr.includes(needle)) return true;
  }
  return false;
}

/**
 * Format a Unix-ms timestamp as HH:MM:SS.mmm.
 */
export function formatLogTimestamp(ms: number): string {
  const d = new Date(ms);
  const h = d.getHours().toString().padStart(2, '0');
  const m = d.getMinutes().toString().padStart(2, '0');
  const s = d.getSeconds().toString().padStart(2, '0');
  const ms3 = d.getMilliseconds().toString().padStart(3, '0');
  return `${h}:${m}:${s}.${ms3}`;
}

/**
 * Format a log entry as a single text line for TXT export.
 */
export function formatLogLine(entry: LogEntry): string {
  const ts = formatLogTimestamp(entry.timestamp);
  const lvl = entry.level.toUpperCase().padEnd(7);
  const src = entry.source.padEnd(6);
  const mod = entry.module ? `[${entry.module}] ` : '';
  const det = entry.details ? ' ' + JSON.stringify(entry.details) : '';
  return `${ts} ${lvl} ${src} ${mod}${entry.message}${det}`;
}
