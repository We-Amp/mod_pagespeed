// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// telemetry-capture.test.ts — schema gate for src/data/telemetry-capture.json,
// the dated capture behind the telemetry strip's server-rendered fallback.
// Validates structure, plus one drift check: the root capture's
// x-mod-pagespeed must equal MODULE_PACKAGE_VERSION, so a capture left over
// from the previous release fails the build instead of publishing that
// release's version (the capture feeds the homepage quick-start and the
// X-Mod-Pagespeed examples). Age in time is deliberately not checked (the
// strip always renders its capture date); recapture is a workflow rule
// (`npm run capture:telemetry` before each deploy), not a test.

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';
import { MODULE_PACKAGE_VERSION } from '../../src/data/product-facts.mjs';

const CAPTURE_FILE = resolve(__dirname, '../../src/data/telemetry-capture.json');

const HEADER_KEYS = [
  'x-mod-pagespeed',
  'vary',
  'content-encoding',
  'content-type',
  'cache-control',
  'content-length',
];

interface CaptureEntry {
  status: number;
  wireBytes: number;
  decodedBytes: number;
  headers: Record<string, string | null>;
}

function loadCapture(): {
  origin: string;
  capturedAt: string;
  paths: Record<string, CaptureEntry>;
} {
  return JSON.parse(readFileSync(CAPTURE_FILE, 'utf8'));
}

describe('telemetry-capture.json schema', () => {
  const capture = loadCapture();

  it('has an https origin and a parseable ISO capturedAt', () => {
    expect(typeof capture.origin).toBe('string');
    expect(new URL(capture.origin).protocol).toBe('https:');
    expect(typeof capture.capturedAt).toBe('string');
    expect(Number.isNaN(Date.parse(capture.capturedAt))).toBe(false);
  });

  it('captures at least the site root (the strip falls back to it)', () => {
    expect(Object.keys(capture.paths)).toContain('/');
  });

  it("the root capture's x-mod-pagespeed equals MODULE_PACKAGE_VERSION", () => {
    // A capture from the previous release would show that release's version
    // on the homepage and in the header examples; recapture before shipping.
    expect(capture.paths['/'].headers['x-mod-pagespeed']).toBe(MODULE_PACKAGE_VERSION);
  });

  it('every path entry carries status, byte counts and the header set', () => {
    for (const [path, entry] of Object.entries(capture.paths)) {
      expect(path.startsWith('/'), `path key ${path}`).toBe(true);
      expect(Number.isInteger(entry.status), `${path} status`).toBe(true);
      expect(entry.status, `${path} status`).toBeGreaterThanOrEqual(100);
      expect(entry.status, `${path} status`).toBeLessThan(600);
      expect(Number.isInteger(entry.wireBytes), `${path} wireBytes`).toBe(true);
      expect(entry.wireBytes, `${path} wireBytes`).toBeGreaterThan(0);
      expect(Number.isInteger(entry.decodedBytes), `${path} decodedBytes`).toBe(true);
      expect(entry.decodedBytes, `${path} decodedBytes`).toBeGreaterThan(0);
      for (const key of HEADER_KEYS) {
        expect(entry.headers, `${path} headers.${key}`).toHaveProperty(key);
        const value = entry.headers[key];
        expect(
          value === null || typeof value === 'string',
          `${path} headers.${key} must be a string or null`,
        ).toBe(true);
      }
    }
  });

  it('a gzipped entry decodes to at least as many bytes as went over the wire', () => {
    for (const [path, entry] of Object.entries(capture.paths)) {
      if (entry.headers['content-encoding']?.includes('gzip')) {
        expect(
          entry.decodedBytes,
          `${path}: decoded ${entry.decodedBytes} < wire ${entry.wireBytes}`,
        ).toBeGreaterThanOrEqual(entry.wireBytes);
      }
    }
  });
});
