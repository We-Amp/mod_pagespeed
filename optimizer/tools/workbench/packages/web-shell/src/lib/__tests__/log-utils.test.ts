// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect, beforeEach } from 'vitest';
import type { LogEntry, LogLevel, LogSource } from '@pagespeed/api-client';
import {
  ALL_SOURCES,
  ALL_LEVELS,
  MAX_LOG_ENTRIES,
  MODULE_COLORS,
  filterLogs,
  matchesSearch,
  formatLogTimestamp,
  formatLogLine,
  tagEntry,
  appendToBuffer,
  resetClientIdCounter,
} from '../log-utils';
import type { ClientLogEntry } from '../log-utils';

// -- Helpers ----------------------------------------------------------------

function makeEntry(overrides: Partial<LogEntry> = {}): LogEntry {
  return {
    type: 'log',
    timestamp: 1707546896123,
    source: 'worker',
    level: 'info',
    module: 'image',
    message: 'Encoded WebP variant',
    ...overrides,
  };
}

function makeTagged(overrides: Partial<LogEntry> = {}): ClientLogEntry {
  return tagEntry(makeEntry(overrides));
}

function allSources(enabled = true): Record<LogSource, boolean> {
  return { worker: enabled, cache: enabled, chrome: enabled };
}

function allLevels(enabled = true): Record<LogLevel, boolean> {
  return { debug: enabled, info: enabled, warning: enabled, error: enabled };
}

// -- Tests ------------------------------------------------------------------

describe('log-utils constants', () => {
  it('ALL_SOURCES has three sources', () => {
    expect(ALL_SOURCES).toEqual(['worker', 'cache', 'chrome']);
  });

  it('ALL_LEVELS has four levels in severity order', () => {
    expect(ALL_LEVELS).toEqual(['debug', 'info', 'warning', 'error']);
  });

  it('MAX_LOG_ENTRIES is 5000', () => {
    expect(MAX_LOG_ENTRIES).toBe(5000);
  });

  it('MODULE_COLORS has entries for known modules', () => {
    expect(MODULE_COLORS).toHaveProperty('html_assembly');
    expect(MODULE_COLORS).toHaveProperty('image');
    expect(MODULE_COLORS).toHaveProperty('css');
    expect(MODULE_COLORS).toHaveProperty('js');
    expect(MODULE_COLORS).toHaveProperty('cache');
    expect(MODULE_COLORS).toHaveProperty('svg');
  });
});

describe('filterLogs', () => {
  beforeEach(() => resetClientIdCounter());

  const entries: ClientLogEntry[] = [
    tagEntry(makeEntry({ source: 'worker', level: 'info', message: 'WebP encode' })),
    tagEntry(makeEntry({ source: 'cache', level: 'debug', message: 'Cache miss' })),
    tagEntry(makeEntry({ source: 'chrome', level: 'error', message: 'CDP timeout' })),
    tagEntry(makeEntry({ source: 'worker', level: 'warning', message: 'Quality below target' })),
  ];

  it('returns all entries when all filters are enabled and no search', () => {
    const result = filterLogs(entries, allSources(), allLevels(), '');
    expect(result).toHaveLength(4);
  });

  it('filters by source', () => {
    const sources = { ...allSources(), cache: false };
    const result = filterLogs(entries, sources, allLevels(), '');
    expect(result).toHaveLength(3);
    expect(result.every((e) => e.source !== 'cache')).toBe(true);
  });

  it('filters by level', () => {
    const levels = { ...allLevels(), debug: false };
    const result = filterLogs(entries, allSources(), levels, '');
    expect(result).toHaveLength(3);
    expect(result.every((e) => e.level !== 'debug')).toBe(true);
  });

  it('filters by search text (case-insensitive)', () => {
    const result = filterLogs(entries, allSources(), allLevels(), 'webp');
    expect(result).toHaveLength(1);
    expect(result[0].message).toBe('WebP encode');
  });

  it('combines source, level, and search filters', () => {
    const sources = { ...allSources(), chrome: false };
    const result = filterLogs(entries, sources, allLevels(), 'cache');
    expect(result).toHaveLength(1);
    expect(result[0].source).toBe('cache');
  });

  it('returns empty when all sources disabled', () => {
    const result = filterLogs(entries, allSources(false), allLevels(), '');
    expect(result).toHaveLength(0);
  });

  it('returns empty when all levels disabled', () => {
    const result = filterLogs(entries, allSources(), allLevels(false), '');
    expect(result).toHaveLength(0);
  });
});

describe('matchesSearch', () => {
  it('matches message text', () => {
    const entry = makeEntry({ message: 'AVIF encode complete' });
    expect(matchesSearch(entry, 'avif')).toBe(true);
  });

  it('matches module name', () => {
    const entry = makeEntry({ module: 'html_assembly' });
    expect(matchesSearch(entry, 'html')).toBe(true);
  });

  it('matches source name', () => {
    const entry = makeEntry({ source: 'chrome' });
    expect(matchesSearch(entry, 'chrome')).toBe(true);
  });

  it('matches in details JSON', () => {
    const entry = makeEntry({
      message: 'Processing image',
      details: { url: '/hero.jpg', width: 1920 },
    });
    expect(matchesSearch(entry, 'hero.jpg')).toBe(true);
  });

  it('returns false when no match', () => {
    const entry = makeEntry({ message: 'Hello world', module: 'image' });
    expect(matchesSearch(entry, 'zzz')).toBe(false);
  });

  it('is case-insensitive', () => {
    const entry = makeEntry({ message: 'JPEG Quality check' });
    expect(matchesSearch(entry, 'jpeg quality')).toBe(true);
  });
});

describe('formatLogTimestamp', () => {
  it('formats a Unix-ms timestamp as HH:MM:SS.mmm', () => {
    // 2024-02-10T12:34:56.123 UTC
    const ts = Date.UTC(2024, 1, 10, 12, 34, 56, 123);
    const result = formatLogTimestamp(ts);
    // Exact HH depends on local timezone, but format should match pattern.
    expect(result).toMatch(/^\d{2}:\d{2}:\d{2}\.\d{3}$/);
  });

  it('pads single-digit components', () => {
    const ts = new Date(2024, 0, 1, 1, 2, 3, 4).getTime();
    const result = formatLogTimestamp(ts);
    expect(result).toBe('01:02:03.004');
  });
});

describe('formatLogLine', () => {
  it('formats a basic log entry as a text line', () => {
    const entry = makeEntry({
      timestamp: new Date(2024, 0, 1, 10, 30, 0, 0).getTime(),
      level: 'info',
      source: 'worker',
      module: 'image',
      message: 'Encoded variant',
    });
    const line = formatLogLine(entry);
    expect(line).toContain('10:30:00.000');
    expect(line).toContain('INFO');
    expect(line).toContain('worker');
    expect(line).toContain('[image]');
    expect(line).toContain('Encoded variant');
  });

  it('omits module bracket when module is empty', () => {
    const entry = makeEntry({ module: '', message: 'Startup' });
    const line = formatLogLine(entry);
    expect(line).not.toContain('[]');
    expect(line).toContain('Startup');
  });

  it('includes details JSON when present', () => {
    const entry = makeEntry({
      message: 'Error',
      details: { code: 'TIMEOUT' },
    });
    const line = formatLogLine(entry);
    expect(line).toContain('"code":"TIMEOUT"');
  });

  it('does not include details when absent', () => {
    const entry = makeEntry({ message: 'OK', details: undefined });
    const line = formatLogLine(entry);
    // Should end with the message, no trailing JSON.
    expect(line).toMatch(/OK$/);
  });
});

describe('tagEntry', () => {
  beforeEach(() => resetClientIdCounter());

  it('assigns monotonically increasing _clientId values', () => {
    const a = tagEntry(makeEntry({ message: 'First' }));
    const b = tagEntry(makeEntry({ message: 'Second' }));
    const c = tagEntry(makeEntry({ message: 'Third' }));
    expect(a._clientId).toBe(0);
    expect(b._clientId).toBe(1);
    expect(c._clientId).toBe(2);
  });

  it('preserves all original LogEntry fields', () => {
    const original = makeEntry({ message: 'Test', details: { x: 1 } });
    const tagged = tagEntry(original);
    expect(tagged.message).toBe('Test');
    expect(tagged.source).toBe('worker');
    expect(tagged.level).toBe('info');
    expect(tagged.module).toBe('image');
    expect(tagged.details).toEqual({ x: 1 });
    expect(tagged.type).toBe('log');
  });

  it('produces unique IDs even for identical entries', () => {
    const a = tagEntry(makeEntry());
    const b = tagEntry(makeEntry());
    expect(a._clientId).not.toBe(b._clientId);
  });
});

describe('appendToBuffer', () => {
  beforeEach(() => resetClientIdCounter());

  it('appends to an empty buffer', () => {
    const entry = makeTagged({ message: 'First' });
    const result = appendToBuffer([], entry, 5);
    expect(result).toHaveLength(1);
    expect(result[0].message).toBe('First');
  });

  it('appends when below capacity', () => {
    const a = makeTagged({ message: 'A' });
    const b = makeTagged({ message: 'B' });
    const buf = [a];
    const result = appendToBuffer(buf, b, 5);
    expect(result).toHaveLength(2);
    expect(result[1].message).toBe('B');
  });

  it('trims oldest entry when at capacity', () => {
    const entries: ClientLogEntry[] = [
      makeTagged({ message: 'A' }),
      makeTagged({ message: 'B' }),
      makeTagged({ message: 'C' }),
    ];
    const d = makeTagged({ message: 'D' });
    const result = appendToBuffer(entries, d, 3);
    expect(result).toHaveLength(3);
    expect(result[0].message).toBe('B');
    expect(result[1].message).toBe('C');
    expect(result[2].message).toBe('D');
  });

  it('trims oldest when over capacity', () => {
    const entries: ClientLogEntry[] = [
      makeTagged({ message: 'A' }),
      makeTagged({ message: 'B' }),
      makeTagged({ message: 'C' }),
      makeTagged({ message: 'D' }),
    ];
    const e = makeTagged({ message: 'E' });
    // Max is 3, but buffer has 4 entries — should trim to 3 after append.
    const result = appendToBuffer(entries, e, 3);
    expect(result).toHaveLength(3);
    expect(result[0].message).toBe('C');
    expect(result[1].message).toBe('D');
    expect(result[2].message).toBe('E');
  });

  it('returns a new array (does not mutate original)', () => {
    const entries: ClientLogEntry[] = [makeTagged({ message: 'A' })];
    const b = makeTagged({ message: 'B' });
    const result = appendToBuffer(entries, b, 5);
    expect(result).not.toBe(entries);
    expect(entries).toHaveLength(1);
  });

  it('handles capacity of 1', () => {
    const a = makeTagged({ message: 'A' });
    const b = makeTagged({ message: 'B' });
    const result = appendToBuffer([a], b, 1);
    expect(result).toHaveLength(1);
    expect(result[0].message).toBe('B');
  });
});
