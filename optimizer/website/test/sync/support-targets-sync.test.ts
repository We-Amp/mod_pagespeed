// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// support-targets-sync.test.ts — drift guard for the first-response targets
// the terms of service state. The support page shows each tier's target on its
// tier card (SupportTiers.astro renders SUPPORT_TIERS); the terms page states
// the same targets in its "How support works" paragraph through the shared
// tierTarget helper. The two must never disagree.
//
// The source checks always run. The built-page check reads dist/client and
// SKIPS (never throws) when the build output is absent, because CI's
// unit-test job runs `npx vitest run` without a preceding build. Run it after
// a build with `npm run build && npx vitest run`.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { SUPPORT_TIERS, RESPONSE_TARGETS_LINE, tierTarget } from '../../src/data/product-facts.mjs';

const WEBSITE_ROOT = fileURLToPath(new URL('../..', import.meta.url));
const TERMS_SRC = path.join(WEBSITE_ROOT, 'src/pages/terms.astro');
const DIST = path.join(WEBSITE_ROOT, 'dist/client');
const TERMS_HTML = path.join(DIST, 'terms/index.html');
const SUPPORT_HTML = path.join(DIST, 'support/index.html');
const BUILT = existsSync(TERMS_HTML) && existsSync(SUPPORT_HTML);

// One pass with a lookup table, so each entity is decoded exactly once
// (`&amp;lt;` stays `&lt;`, never becomes `<`).
const ENTITIES: Record<string, string> = {
  amp: '&',
  lt: '<',
  gt: '>',
  quot: '"',
  nbsp: ' ',
  rsquo: "'",
  '#39': "'",
  '#x27': "'",
};
const decode = (s: string) =>
  s.replace(/&(amp|lt|gt|quot|nbsp|rsquo|#39|#x27);/g, (_m, e: string) => ENTITIES[e]);
const textOf = (file: string) =>
  decode(readFileSync(file, 'utf8').replace(/<[^>]+>/g, ' '))
    .replace(/\s+/g, ' ')
    .trim();

const tiers = SUPPORT_TIERS.filter((t) => t.kind === 'support');
const stripDot = (s: string) => s.replace(/\.$/, '');

describe('first-response targets: shared data', () => {
  it('every support tier has a response target on its card', () => {
    expect(tiers.length).toBeGreaterThan(0);
    for (const t of tiers) {
      const target = tierTarget(t);
      expect(t.includes, t.id).toContain(target);
      expect(target, `${t.id}: tierTarget is not a response target`).toMatch(
        /business (day|days|hours)\b/,
      );
    }
  });

  it('the terms page reads the targets through the helper, not a literal', () => {
    const src = readFileSync(TERMS_SRC, 'utf8');
    expect(src).toContain('tierTarget(');
    expect(src).toContain('{RESPONSE_TARGETS_LINE}');
    for (const t of tiers) {
      expect(src, `${t.id}: target restated as a literal`).not.toContain(stripDot(tierTarget(t)));
    }
  });
});

if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    `[support-targets-sync] built output not found under ${DIST}; skipping the ` +
      'rendered-page check. Run "npm run build" first to exercise it.',
  );
}

describe.runIf(BUILT)('first-response targets: rendered pages', () => {
  it('the terms page states the same target as the support page, for every tier', () => {
    const terms = textOf(TERMS_HTML);
    const support = textOf(SUPPORT_HTML);
    for (const t of tiers) {
      const target = stripDot(tierTarget(t));
      expect(support, `${t.id}: target missing on /support/`).toContain(target);
      expect(terms, `${t.id}: target missing on /terms/`).toContain(`${t.name}: ${target}.`);
    }
    const start = terms.indexOf('How support works.');
    const end = terms.indexOf(RESPONSE_TARGETS_LINE, start);
    expect(start, 'the "How support works" paragraph is missing').toBeGreaterThan(-1);
    expect(end, 'the response-targets line is missing from the paragraph').toBeGreaterThan(start);
    expect(terms.slice(start, end), 'double period in the targets').not.toContain('..');
    expect(support).toContain(RESPONSE_TARGETS_LINE);
  });
});
