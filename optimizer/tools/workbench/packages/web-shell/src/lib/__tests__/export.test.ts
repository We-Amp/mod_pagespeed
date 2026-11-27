// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// @vitest-environment jsdom
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
  timestampedFilename,
  escapeCsvCell,
  buildCsvString,
  exportJson,
  exportCsv,
} from '../export';

// ---------------------------------------------------------------------------
// timestampedFilename
// ---------------------------------------------------------------------------

describe('timestampedFilename', () => {
  it('generates a filename with ISO timestamp (colons replaced)', () => {
    // Pin Date to a known instant.
    vi.useFakeTimers();
    vi.setSystemTime(new Date('2024-01-15T10:30:00.123Z'));

    const result = timestampedFilename('pagespeed-stats', 'json');
    expect(result).toBe('pagespeed-stats-2024-01-15T10-30-00.json');

    vi.useRealTimers();
  });

  it('works with different prefixes and extensions', () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date('2025-12-31T23:59:59.999Z'));

    expect(timestampedFilename('savings', 'csv')).toBe(
      'savings-2025-12-31T23-59-59.csv',
    );

    vi.useRealTimers();
  });
});

// ---------------------------------------------------------------------------
// escapeCsvCell
// ---------------------------------------------------------------------------

describe('escapeCsvCell', () => {
  it('returns plain strings unchanged', () => {
    expect(escapeCsvCell('hello')).toBe('hello');
  });

  it('returns numbers as strings', () => {
    expect(escapeCsvCell(42)).toBe('42');
    expect(escapeCsvCell(3.14)).toBe('3.14');
  });

  it('wraps strings containing commas in double-quotes', () => {
    expect(escapeCsvCell('a,b')).toBe('"a,b"');
  });

  it('wraps strings containing double-quotes and doubles them', () => {
    expect(escapeCsvCell('say "hello"')).toBe('"say ""hello"""');
  });

  it('wraps strings containing newlines', () => {
    expect(escapeCsvCell('line1\nline2')).toBe('"line1\nline2"');
  });

  it('handles empty string', () => {
    expect(escapeCsvCell('')).toBe('');
  });

  it('handles zero', () => {
    expect(escapeCsvCell(0)).toBe('0');
  });
});

// ---------------------------------------------------------------------------
// buildCsvString
// ---------------------------------------------------------------------------

describe('buildCsvString', () => {
  it('builds a valid CSV with headers and rows', () => {
    const headers = ['Name', 'Value', 'Unit'];
    const rows: (string | number)[][] = [
      ['Notifications', 150, 'count'],
      ['Errors', 3, 'count'],
    ];
    const csv = buildCsvString(headers, rows);
    expect(csv).toBe(
      'Name,Value,Unit\nNotifications,150,count\nErrors,3,count\n',
    );
  });

  it('escapes cells containing special characters', () => {
    const headers = ['Key', 'Description'];
    const rows: (string | number)[][] = [
      ['comma', 'value,with,commas'],
      ['quote', 'say "hi"'],
    ];
    const csv = buildCsvString(headers, rows);
    const lines = csv.trim().split('\n');
    expect(lines).toHaveLength(3);
    expect(lines[0]).toBe('Key,Description');
    expect(lines[1]).toBe('comma,"value,with,commas"');
    expect(lines[2]).toBe('quote,"say ""hi"""');
  });

  it('handles empty rows', () => {
    const csv = buildCsvString(['A', 'B'], []);
    expect(csv).toBe('A,B\n');
  });

  it('ends with a trailing newline', () => {
    const csv = buildCsvString(['X'], [[1]]);
    expect(csv.endsWith('\n')).toBe(true);
  });
});

// ---------------------------------------------------------------------------
// exportJson & exportCsv (browser download mocks)
// ---------------------------------------------------------------------------

describe('exportJson', () => {
  let createObjectURLMock: ReturnType<typeof vi.fn>;
  let revokeObjectURLMock: ReturnType<typeof vi.fn>;
  let appendChildSpy: ReturnType<typeof vi.fn>;
  let removeSpy: ReturnType<typeof vi.fn>;
  let clickSpy: ReturnType<typeof vi.fn>;
  let createdAnchor: HTMLAnchorElement;

  beforeEach(() => {
    createObjectURLMock = vi.fn(() => 'blob:mock-url');
    revokeObjectURLMock = vi.fn();
    clickSpy = vi.fn();
    removeSpy = vi.fn();

    // Mock URL methods.
    globalThis.URL.createObjectURL = createObjectURLMock;
    globalThis.URL.revokeObjectURL = revokeObjectURLMock;

    // Mock document.createElement to capture the anchor element.
    const origCreateElement = document.createElement.bind(document);
    vi.spyOn(document, 'createElement').mockImplementation((tag: string) => {
      if (tag === 'a') {
        createdAnchor = origCreateElement('a');
        createdAnchor.click = clickSpy;
        createdAnchor.remove = removeSpy;
        return createdAnchor;
      }
      return origCreateElement(tag);
    });

    appendChildSpy = vi.spyOn(document.body, 'appendChild');
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('creates a JSON blob and triggers download', () => {
    const data = { hello: 'world', count: 42 };
    exportJson(data, 'test.json');

    // Blob was created.
    expect(createObjectURLMock).toHaveBeenCalledTimes(1);
    const blob = createObjectURLMock.mock.calls[0][0] as Blob;
    expect(blob).toBeInstanceOf(Blob);
    expect(blob.type).toBe('application/json');

    // Anchor was configured and clicked.
    expect(createdAnchor.href).toContain('blob:mock-url');
    expect(createdAnchor.download).toBe('test.json');
    expect(clickSpy).toHaveBeenCalledTimes(1);
    expect(appendChildSpy).toHaveBeenCalled();
  });

  it('pretty-prints JSON with 2-space indentation', async () => {
    const data = { a: 1, b: [2, 3] };
    exportJson(data, 'out.json');

    const blob = createObjectURLMock.mock.calls[0][0] as Blob;
    // jsdom Blob may not have .text(), so read via FileReader.
    const text = await new Promise<string>((resolve) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result as string);
      reader.readAsText(blob);
    });
    expect(text).toBe(JSON.stringify(data, null, 2) + '\n');
  });
});

describe('exportCsv', () => {
  let createObjectURLMock: ReturnType<typeof vi.fn>;
  let revokeObjectURLMock: ReturnType<typeof vi.fn>;
  let clickSpy: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    createObjectURLMock = vi.fn(() => 'blob:csv-url');
    revokeObjectURLMock = vi.fn();
    clickSpy = vi.fn();

    globalThis.URL.createObjectURL = createObjectURLMock;
    globalThis.URL.revokeObjectURL = revokeObjectURLMock;

    const origCreateElement = document.createElement.bind(document);
    vi.spyOn(document, 'createElement').mockImplementation((tag: string) => {
      if (tag === 'a') {
        const el = origCreateElement('a');
        el.click = clickSpy;
        el.remove = vi.fn();
        return el;
      }
      return origCreateElement(tag);
    });

    vi.spyOn(document.body, 'appendChild').mockImplementation(
      (node) => node as HTMLElement,
    );
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('creates a CSV blob and triggers download', async () => {
    exportCsv(['A', 'B'], [[1, 'x']], 'data.csv');

    expect(createObjectURLMock).toHaveBeenCalledTimes(1);
    const blob = createObjectURLMock.mock.calls[0][0] as Blob;
    expect(blob).toBeInstanceOf(Blob);
    expect(blob.type).toBe('text/csv');

    const text = await new Promise<string>((resolve) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result as string);
      reader.readAsText(blob);
    });
    expect(text).toBe('A,B\n1,x\n');
    expect(clickSpy).toHaveBeenCalledTimes(1);
  });
});
