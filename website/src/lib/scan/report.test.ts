// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { buildReportMarkdown, riskStates } from './report';

const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url),
      'utf8',
    ),
  ).report;

describe('riskStates', () => {
  it('marks every lens Attention on the full report, in panel order', () => {
    expect(riskStates(fixture('full'))).toEqual([
      { name: 'Trackers before consent', state: 'Attention' },
      { name: 'Scripts on the page', state: 'Attention' },
      { name: 'SEO defects', state: 'Attention' },
      { name: 'Errors and versions the server reveals', state: 'Attention' },
    ]);
  });

  it('marks every lens Nothing flagged on the clean report', () => {
    expect(riskStates(fixture('clean'))).toEqual([
      { name: 'Trackers before consent', state: 'Nothing flagged' },
      { name: 'Scripts on the page', state: 'Nothing flagged' },
      { name: 'SEO defects', state: 'Nothing flagged' },
      { name: 'Errors and versions the server reveals', state: 'Nothing flagged' },
    ]);
  });

  it('marks every lens Not measured on the blocked report, whose lenses did not run', () => {
    expect(riskStates(fixture('blocked'))).toEqual([
      { name: 'Trackers before consent', state: 'Not measured' },
      { name: 'Scripts on the page', state: 'Not measured' },
      { name: 'SEO defects', state: 'Not measured' },
      { name: 'Errors and versions the server reveals', state: 'Not measured' },
    ]);
  });

  it('returns no rows without a report', () => {
    expect(riskStates(null)).toEqual([]);
  });

  it('omits a disabled lens and a missing lens instead of listing them as not measured', () => {
    const rows = riskStates({
      preConsentLeak: { status: 'disabled' },
      scriptInventory: { status: 'ok', verdict: 'clean' },
      responseExposure: { status: 'ok', verdict: 'attention' },
    });
    expect(rows).toEqual([
      { name: 'Scripts on the page', state: 'Nothing flagged' },
      { name: 'Errors and versions the server reveals', state: 'Attention' },
    ]);
  });

  it('marks a lens that ran but reached no verdict as Not measured', () => {
    expect(riskStates({ scriptInventory: { status: 'ok', verdict: 'unknown' } })).toEqual([
      { name: 'Scripts on the page', state: 'Not measured' },
    ]);
  });

  it('keeps a not-ok lens out of the measured set even when its verdict is the flagged one', () => {
    expect(riskStates({ seoDefects: { status: 'error', verdict: 'attention' } })).toEqual([
      { name: 'SEO defects', state: 'Not measured' },
    ]);
  });
});

describe('buildReportMarkdown', () => {
  it('renders the whole document for a full report with failing audits', () => {
    const md = buildReportMarkdown({
      url: 'https://shop.example.com/',
      when: '2026-10-10T00:00:00.000Z',
      mobile: 58,
      desktop: 92,
      report: fixture('full'),
      audits: ['Largest Contentful Paint: 4.0 s', 'Eliminate render-blocking resources'],
    });
    expect(md).toBe(
      [
        '# Scan report',
        '',
        'Analyzed: https://shop.example.com/',
        'When: 2026-10-10T00:00:00.000Z',
        'Speed (PageSpeed Insights): mobile 58/100, desktop 92/100',
        'AI readability: grade C (64/100)',
        '',
        '## Risk and SEO',
        '',
        '- Trackers before consent: Attention',
        '- Scripts on the page: Attention',
        '- SEO defects: Attention',
        '- Errors and versions the server reveals: Attention',
        '',
        '## Failing audits',
        '',
        'Largest Contentful Paint: 4.0 s',
        'Eliminate render-blocking resources',
        '',
      ].join('\n'),
    );
  });

  it('renders the bare document without a report: scores n/a, AI readability not measured, no risk or audits sections', () => {
    const md = buildReportMarkdown({
      url: 'https://shop.example.com/',
      when: '2026-10-10T00:00:00.000Z',
      mobile: null,
      desktop: null,
      report: null,
    });
    expect(md).toBe(
      [
        '# Scan report',
        '',
        'Analyzed: https://shop.example.com/',
        'When: 2026-10-10T00:00:00.000Z',
        'Speed (PageSpeed Insights): mobile n/a, desktop n/a',
        'AI readability: not measured',
        '',
      ].join('\n'),
    );
  });

  it('writes None flagged for an empty audits list and one final newline', () => {
    const md = buildReportMarkdown({
      url: 'u',
      when: 'w',
      mobile: 1,
      desktop: 2,
      report: null,
      audits: [],
    });
    expect(md.endsWith('## Failing audits\n\n- None flagged\n')).toBe(true);
  });

  it('writes n/a for the AI score when the report carries only a grade', () => {
    const md = buildReportMarkdown({
      url: 'u',
      when: 'w',
      mobile: null,
      desktop: null,
      report: { grade: 'B' },
    });
    expect(md).toContain('AI readability: grade B (n/a)');
    expect(md).not.toContain('## Failing audits');
  });

  it('still lists the risk rows of a report that has no AI grade', () => {
    const md = buildReportMarkdown({
      url: 'u',
      when: 'w',
      mobile: null,
      desktop: null,
      report: { scriptInventory: { status: 'ok', verdict: 'attention' } },
    });
    expect(md).toContain('AI readability: not measured');
    expect(md).toContain('- Scripts on the page: Attention');
  });
});
