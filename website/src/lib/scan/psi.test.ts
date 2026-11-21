// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { PSI_MPS_MAPPING } from '../../data/psi-mps-mapping';
import {
  aggregate,
  buildConfigSnippet,
  classifyError,
  describePsiError,
  collectFlaggedAuditIds,
  coverageSentence,
  flaggedAudits,
  savingsSentence,
  type PsiResult,
} from './psi';

const fixture = (name: string): PsiResult =>
  JSON.parse(
    readFileSync(new URL(`../../../tests/fixtures/scan/${name}`, import.meta.url), 'utf8'),
  );
const mobile = fixture('psi-mobile.json');
const desktop = fixture('psi-desktop.json');

describe('aggregate', () => {
  const items = aggregate(mobile, desktop, PSI_MPS_MAPPING);
  it('matches the golden list for the fixtures', () => {
    expect(
      items.map((i) => [
        i.mapping.auditId,
        i.mapping.coverage,
        i.overallSavingsMs,
        i.overallSavingsBytes,
      ]),
    ).toMatchInlineSnapshot(`
      [
        [
          "modern-image-formats",
          "full",
          1540,
          419840,
        ],
        [
          "render-blocking-resources",
          "full",
          792,
          0,
        ],
        [
          "render-blocking-insight",
          "full",
          748,
          0,
        ],
        [
          "offscreen-images",
          "full",
          550,
          153600,
        ],
        [
          "uses-optimized-images",
          "full",
          396,
          98304,
        ],
        [
          "unminified-javascript",
          "full",
          132,
          22528,
        ],
        [
          "cache-insight",
          "full",
          0,
          184320,
        ],
        [
          "image-delivery-insight",
          "full",
          0,
          0,
        ],
        [
          "unused-css-rules",
          "partial",
          330,
          86016,
        ],
        [
          "network-dependency-tree-insight",
          "partial",
          0,
          0,
        ],
        [
          "lcp-breakdown-insight",
          "partial",
          0,
          0,
        ],
        [
          "speed-index",
          "partial",
          0,
          0,
        ],
        [
          "unused-javascript",
          "none",
          1144,
          217088,
        ],
        [
          "uses-text-compression",
          "none",
          198,
          40960,
        ],
        [
          "forced-reflow-insight",
          "none",
          0,
          0,
        ],
      ]
    `);
  });
  it('orders full coverage first, then partial, then none', () => {
    const rank = { full: 0, partial: 1, none: 2 } as const;
    const ranks = items.map((i) => rank[i.mapping.coverage]);
    expect(ranks).toEqual([...ranks].sort((a, b) => a - b));
  });
  it('counts every flagged audit, mapped or not', () => {
    const ids = collectFlaggedAuditIds(mobile, desktop);
    expect(ids.size).toBeGreaterThanOrEqual(items.length);
    for (const i of items) expect(ids.has(i.mapping.auditId)).toBe(true);
  });
  it('builds both edition snippets from the fixable audits', () => {
    const fixable = items.filter((i) => i.mapping.coverage !== 'none');
    expect(buildConfigSnippet(fixable, '2.0')).toContain('pagespeed on;');
    expect(buildConfigSnippet(fixable, '1.1')).toContain('pagespeed EnableFilters');
  });
});

describe('sentences and errors', () => {
  it('keeps the coverage sentences', () => {
    expect(coverageSentence(0, 0, 0)).toBe(
      'PSI flagged no failing performance audits on this page — your site is already fast.',
    );
    expect(coverageSentence(2, 0, 2)).toBe(
      'PSI flagged 2 performance audits; mod_pagespeed cannot automatically fix any of them on this page.',
    );
    expect(coverageSentence(4, 3, 3)).toBe(
      'mod_pagespeed addresses 3 of 4 flagged audits. (1 additional audit is not yet mapped — see the project mapping data.)',
    );
  });
  it('keeps the savings sentences', () => {
    expect(savingsSentence(1200, 2048)).toBe(
      'Across the fixable audits PSI flagged, mod_pagespeed could recover 1.20 s of estimated load-time savings and 2.0 KB of byte savings on this page.',
    );
    expect(savingsSentence(0, 0)).toBe(
      'PSI did not attach numeric savings estimates to the flagged audits.',
    );
  });
  it('classifies errors by status', () => {
    expect(classifyError({ status: 429, message: 'x' }).title).toBe('PSI rate limit hit.');
    expect(classifyError({ status: 404, message: 'x' }).title).toBe(
      'PSI couldn’t analyze that URL.',
    );
    expect(classifyError({ status: 503, message: 'x' }).title).toBe('PSI is having trouble.');
    expect(classifyError({ message: 'x' }).title).toBe('Couldn’t reach PSI.');
    expect(classifyError({ reload: true, message: 'm' }).title).toBe('This page needs a reload.');
  });
  it('spells out the service in the titles shown on the tile and in the panel', () => {
    const t = (status?: number) => describePsiError({ status, message: 'x' }).title;
    expect(t(429)).toBe('Rate limit reached. Try again in a minute.');
    expect(t(404)).toBe('PageSpeed Insights could not analyze that URL.');
    expect(t(503)).toBe('PageSpeed Insights returned an error. Try again later.');
    expect(t()).toBe('PageSpeed Insights did not answer.');
  });
});

describe('flaggedAudits', () => {
  const all = flaggedAudits(mobile, desktop);
  it('lists every flagged audit once, mapped or not, with its PageSpeed Insights title', () => {
    const ids = collectFlaggedAuditIds(mobile, desktop);
    expect(all.map((a) => a.id).sort()).toEqual([...ids].sort());
    expect(all.length).toBeGreaterThan(aggregate(mobile, desktop, PSI_MPS_MAPPING).length - 1);
    for (const a of all) {
      const audit =
        mobile.lighthouseResult?.audits?.[a.id] ?? desktop.lighthouseResult?.audits?.[a.id];
      expect(a.title).toBe(audit?.title ?? a.id);
      expect(a.title).not.toBe('');
    }
  });
  it('orders by the estimated time saving and falls back to the id for a missing title', () => {
    const ms = all.map((a) => a.overallSavingsMs);
    expect(ms).toEqual([...ms].sort((x, y) => y - x));
    const bare: PsiResult = {
      lighthouseResult: { audits: { 'some-audit': { id: 'some-audit', score: 0.2 } } },
    };
    expect(flaggedAudits(bare, {})).toEqual([
      { id: 'some-audit', title: 'some-audit', overallSavingsMs: 0, overallSavingsBytes: 0 },
    ]);
  });
});
