// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// release-notes-ids-gate.test.ts — pre-deploy gate for the converged
// /docs/release-notes/ page's id contract: every era heading, every
// release-entry <details id>, every legacy `<span id>` alias carried
// forward from the pages this one replaced, and the two preserved
// upstream-bridge ids must resolve in the BUILT page. Reads the production
// BUILD output (dist/client), not the MDX source, so it catches anything
// that could drop an id between content and render — the same reasoning as
// anchor-gate.test.ts, which this file otherwise deliberately mirrors (own
// fixture, own test: the two id sets serve different pages and evolve
// independently).
//
// This is the STRONGER, run-after-a-build check — not the CI gate. CI's
// unit-test job runs `npx vitest run` without a preceding `npm run build`,
// and dist/ is gitignored, so this test SKIPS itself (never throws) when
// dist/client/docs/release-notes does not exist, rather than redding every
// PR. Run it explicitly after a build with
// `npx vitest run test/release-notes-ids-gate.test.ts`, or as part of
// `npm run build && npm run test:unit`.
//
// Extend test/fixtures/release-notes-ids.txt (append-only, never remove a
// row) whenever a later content pass adds a new id to the page.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect, beforeAll } from 'vitest';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const FIXTURE_PATH = path.join(WEBSITE_ROOT, 'test/fixtures/release-notes-ids.txt');
const HTML_PATH = path.join(WEBSITE_ROOT, 'dist/client/docs/release-notes/index.html');

function loadIdFixture(fixturePath: string): string[] {
  const raw = readFileSync(fixturePath, 'utf8');
  return raw
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.length > 0 && !l.startsWith('#'));
}

const IDS = loadIdFixture(FIXTURE_PATH);
const BUILT = existsSync(HTML_PATH);

if (!BUILT) {
  // Visible in the test run's output either way — a console.log, not a
  // silently-vanishing skip.
  // eslint-disable-next-line no-console
  console.log(
    `[release-notes-ids-gate] built output not found at ${HTML_PATH} — skipping the built-HTML ` +
      'id check. Run "npm run build" first to exercise it.',
  );
}

describe('release-notes id contract (source fixture)', () => {
  // Fixture sanity: this is the pre-deploy gate, so a shrinking fixture must
  // fail loudly rather than silently pass with fewer checks. Bump this
  // count in the same change that appends a row.
  it('carries all 112 ids the converged page must expose', () => {
    expect(IDS.length).toBe(112);
  });

  it('has no duplicate ids (each id attribute must be unique in valid HTML)', () => {
    const seen = new Set(IDS);
    expect(seen.size).toBe(IDS.length);
  });
});

describe.skipIf(!BUILT)('/docs/release-notes/ (built html)', () => {
  let html: string;

  beforeAll(() => {
    if (!existsSync(HTML_PATH)) {
      throw new Error(`${HTML_PATH} does not exist — was /docs/release-notes/ built?`);
    }
    html = readFileSync(HTML_PATH, 'utf8');
  });

  for (const id of IDS) {
    it(`has an id="${id}"`, () => {
      const idPattern = new RegExp(`id=["']${id}["']`);
      expect(html, `expected id="${id}" in built /docs/release-notes/`).toMatch(idPattern);
    });
  }
});
