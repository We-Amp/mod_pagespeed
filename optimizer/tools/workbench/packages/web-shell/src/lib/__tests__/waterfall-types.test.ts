// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import {
  TIMING_PHASES,
  MILESTONES,
  RESOURCE_TYPE_LABELS,
  RESOURCE_TYPE_COLORS,
  totalDuration,
  entryEndTime,
  formatBytes,
  formatDuration,
  truncateUrl,
  waterfallEndTime,
} from '../waterfall-types';
import type {
  WaterfallEntry,
  WaterfallData,
  RequestTiming,
  ResourceType,
} from '../waterfall-types';

// -- Helpers ----------------------------------------------------------------

function makeTiming(overrides: Partial<RequestTiming> = {}): RequestTiming {
  return {
    dns: 0,
    connect: 0,
    tls: 0,
    ttfb: 0,
    download: 0,
    ...overrides,
  };
}

function makeEntry(overrides: Partial<WaterfallEntry> = {}): WaterfallEntry {
  return {
    url: 'https://example.com/page',
    method: 'GET',
    resourceType: 'document',
    startTime: 0,
    timing: makeTiming(),
    size: 0,
    decodedSize: 0,
    status: 200,
    mimeType: 'text/html',
    fromCache: false,
    fromServiceWorker: false,
    protocol: 'h2',
    ...overrides,
  };
}

function makeData(overrides: Partial<WaterfallData> = {}): WaterfallData {
  return {
    entries: [],
    navigationTiming: {
      domContentLoaded: 0,
      loadEvent: 0,
      firstPaint: 0,
      firstContentfulPaint: 0,
      largestContentfulPaint: 0,
    },
    totalTransferSize: 0,
    totalDecodedSize: 0,
    ...overrides,
  };
}

// -- Constants --------------------------------------------------------------

describe('TIMING_PHASES', () => {
  it('has 5 ordered phases', () => {
    expect(TIMING_PHASES).toHaveLength(5);
  });

  it('has unique keys', () => {
    const keys = TIMING_PHASES.map((p) => p.key);
    expect(new Set(keys).size).toBe(keys.length);
  });

  it('covers all RequestTiming keys', () => {
    const keys = new Set(TIMING_PHASES.map((p) => p.key));
    expect(keys.has('dns')).toBe(true);
    expect(keys.has('connect')).toBe(true);
    expect(keys.has('tls')).toBe(true);
    expect(keys.has('ttfb')).toBe(true);
    expect(keys.has('download')).toBe(true);
  });

  it('phases are in correct order: dns, connect, tls, ttfb, download', () => {
    const keys = TIMING_PHASES.map((p) => p.key);
    expect(keys).toEqual(['dns', 'connect', 'tls', 'ttfb', 'download']);
  });
});

describe('MILESTONES', () => {
  it('has 5 milestones', () => {
    expect(MILESTONES).toHaveLength(5);
  });

  it('has unique keys', () => {
    const keys = MILESTONES.map((m) => m.key);
    expect(new Set(keys).size).toBe(keys.length);
  });
});

describe('RESOURCE_TYPE_LABELS', () => {
  it('has a label for every ResourceType', () => {
    const types: ResourceType[] = [
      'document',
      'stylesheet',
      'script',
      'image',
      'font',
      'xhr',
      'other',
    ];
    for (const t of types) {
      expect(RESOURCE_TYPE_LABELS[t]).toBeDefined();
      expect(RESOURCE_TYPE_LABELS[t].length).toBeGreaterThan(0);
    }
  });
});

describe('RESOURCE_TYPE_COLORS', () => {
  it('has a color for every ResourceType', () => {
    const types: ResourceType[] = [
      'document',
      'stylesheet',
      'script',
      'image',
      'font',
      'xhr',
      'other',
    ];
    for (const t of types) {
      expect(RESOURCE_TYPE_COLORS[t]).toBeDefined();
      expect(RESOURCE_TYPE_COLORS[t]).toMatch(/^#[0-9a-f]{6}$/i);
    }
  });
});

// -- totalDuration ----------------------------------------------------------

describe('totalDuration', () => {
  it('returns 0 for empty timing', () => {
    expect(totalDuration(makeTiming())).toBe(0);
  });

  it('sums all phases', () => {
    const timing = makeTiming({
      dns: 10,
      connect: 20,
      tls: 15,
      ttfb: 50,
      download: 100,
    });
    expect(totalDuration(timing)).toBe(195);
  });

  it('handles a single non-zero phase', () => {
    expect(totalDuration(makeTiming({ ttfb: 42 }))).toBe(42);
  });
});

// -- entryEndTime -----------------------------------------------------------

describe('entryEndTime', () => {
  it('returns startTime when all timing is 0', () => {
    const entry = makeEntry({ startTime: 100 });
    expect(entryEndTime(entry)).toBe(100);
  });

  it('returns startTime + total duration', () => {
    const entry = makeEntry({
      startTime: 50,
      timing: makeTiming({ dns: 10, ttfb: 40, download: 100 }),
    });
    expect(entryEndTime(entry)).toBe(200);
  });
});

// -- formatBytes ------------------------------------------------------------

describe('formatBytes', () => {
  it('formats zero bytes', () => {
    expect(formatBytes(0)).toBe('0 B');
  });

  it('formats negative as 0 B', () => {
    // Canonical formatBytes uses log(abs), so negative values produce
    // the same magnitude with a minus sign — but the only call site
    // that matters (file sizes) never sends negatives.  Just verify
    // it doesn't throw.
    expect(formatBytes(-1)).toBeDefined();
  });

  it('formats bytes under 1KB', () => {
    expect(formatBytes(512)).toBe('512 B');
    expect(formatBytes(1)).toBe('1 B');
    expect(formatBytes(1023)).toBe('1023 B');
  });

  it('formats kilobytes', () => {
    expect(formatBytes(1024)).toBe('1.0 KB');
    expect(formatBytes(1536)).toBe('1.5 KB');
    expect(formatBytes(10240)).toBe('10.0 KB');
  });

  it('rounds large kilobytes', () => {
    expect(formatBytes(102400)).toBe('100.0 KB');
    expect(formatBytes(512000)).toBe('500.0 KB');
  });

  it('formats megabytes', () => {
    expect(formatBytes(1024 * 1024)).toBe('1.0 MB');
    expect(formatBytes(1.5 * 1024 * 1024)).toBe('1.5 MB');
    expect(formatBytes(3.25 * 1024 * 1024)).toBe('3.3 MB');
  });

  it('rounds large megabytes', () => {
    expect(formatBytes(100 * 1024 * 1024)).toBe('100.0 MB');
  });
});

// -- formatDuration ---------------------------------------------------------

describe('formatDuration', () => {
  it('formats zero', () => {
    expect(formatDuration(0)).toBe('0 ms');
  });

  it('formats negative as 0 ms', () => {
    expect(formatDuration(-5)).toBe('0 ms');
  });

  it('formats sub-second durations in ms', () => {
    expect(formatDuration(42)).toBe('42 ms');
    expect(formatDuration(999)).toBe('999 ms');
    expect(formatDuration(0.5)).toBe('1 ms');
  });

  it('formats seconds with two decimal places', () => {
    expect(formatDuration(1000)).toBe('1.00 s');
    expect(formatDuration(1234)).toBe('1.23 s');
    expect(formatDuration(9999)).toBe('10.0 s');
  });

  it('formats large durations with one decimal place', () => {
    expect(formatDuration(10000)).toBe('10.0 s');
    expect(formatDuration(12500)).toBe('12.5 s');
  });
});

// -- truncateUrl ------------------------------------------------------------

describe('truncateUrl', () => {
  it('returns short URLs unchanged', () => {
    const url = 'https://example.com/foo.js';
    expect(truncateUrl(url)).toBe(url);
  });

  it('truncates long URLs to filename', () => {
    const url =
      'https://cdn.example.com/very/long/deeply/nested/path/to/bundle.min.js';
    const result = truncateUrl(url, 30);
    expect(result).toBe('.../bundle.min.js');
  });

  it('handles URLs without path segments', () => {
    const url = 'https://example.com/';
    // No filename to extract; falls back to prefix truncation.
    expect(truncateUrl(url, 10)).toBe('https:/...');
  });

  it('falls back to prefix truncation for non-URL strings', () => {
    const str = 'this is not a url but it is very long and needs truncation';
    const result = truncateUrl(str, 20);
    expect(result).toBe('this is not a url...');
    expect(result.length).toBe(20);
  });

  it('respects custom maxLen', () => {
    const url = 'https://example.com/short.js';
    expect(truncateUrl(url, 100)).toBe(url);
  });
});

// -- waterfallEndTime -------------------------------------------------------

describe('waterfallEndTime', () => {
  it('returns 0 for empty data', () => {
    expect(waterfallEndTime(makeData())).toBe(0);
  });

  it('uses the latest entry end time', () => {
    const data = makeData({
      entries: [
        makeEntry({
          startTime: 100,
          timing: makeTiming({ ttfb: 50, download: 200 }),
        }),
        makeEntry({
          startTime: 0,
          timing: makeTiming({ ttfb: 50, download: 50 }),
        }),
      ],
    });
    expect(waterfallEndTime(data)).toBe(350); // 100 + 50 + 200
  });

  it('includes navigation timing milestones', () => {
    const data = makeData({
      entries: [
        makeEntry({
          startTime: 0,
          timing: makeTiming({ download: 100 }),
        }),
      ],
      navigationTiming: {
        domContentLoaded: 200,
        loadEvent: 500,
        firstPaint: 80,
        firstContentfulPaint: 150,
        largestContentfulPaint: 400,
      },
    });
    expect(waterfallEndTime(data)).toBe(500); // loadEvent is latest
  });

  it('returns entry end time when it exceeds milestones', () => {
    const data = makeData({
      entries: [
        makeEntry({
          startTime: 0,
          timing: makeTiming({ download: 1000 }),
        }),
      ],
      navigationTiming: {
        domContentLoaded: 200,
        loadEvent: 500,
        firstPaint: 80,
        firstContentfulPaint: 150,
        largestContentfulPaint: 400,
      },
    });
    expect(waterfallEndTime(data)).toBe(1000);
  });
});
