// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The AI readability panel's formatter: grade, score, word and category rows
// for the fixture reports, and the lines it adds below them.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import {
  MAX_RULES,
  MAX_RULE_TEXT,
  accessibilityFindings,
  crawlerRulesLine,
  crawlersBlocked,
  formatAiread,
} from './airead';

const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      fileURLToPath(new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url)),
      'utf8',
    ),
  ).report;

// The grade, score, word and five category rows the panel shows for the two
// fixture reports.
const GRADING = {
  full: {
    grade: 'C',
    score: 64,
    word: 'mixed',
    categories: [
      {
        name: 'Content fidelity',
        score: 12,
        max: 30,
        pct: 40,
        flag: 'invisible without JS',
        note: '',
        fromRules: false,
        verdict:
          "Only ~38% of your content exists without JavaScript — AI tools that don't render (and free CDN markdown features) see an almost-empty page. THIS is the big one.",
      },
      {
        name: 'Token efficiency',
        score: 8,
        max: 20,
        pct: 40,
        flag: '',
        note: '',
        fromRules: false,
        verdict:
          'Bloated: the page is ~98× more HTML than actual content (52,300 tokens raw vs ~534 clean). Agents pay for — and choke on — the difference.',
      },
      {
        name: 'Structured data',
        score: 20,
        max: 20,
        pct: 100,
        flag: '',
        note: '',
        fromRules: false,
        verdict:
          'Strong machine-readable metadata (JSON-LD (Organization, WebSite, Product), Open Graph, canonical, meta description).',
      },
      {
        name: 'Crawler access',
        score: 18,
        max: 20,
        pct: 90,
        flag: '',
        note: '',
        fromRules: false,
        verdict: 'AI crawlers are allowed to fetch the page. robots.txt allows AI crawlers.',
      },
      {
        name: 'Citation readiness',
        score: 6,
        max: 10,
        pct: 60,
        flag: '',
        note: '',
        fromRules: false,
        verdict:
          'Thin attribution metadata (canonical URL, title, date) — an LLM citing you may get the source/author/date wrong.',
      },
    ],
  },
  clean: {
    grade: 'A',
    score: 100,
    word: 'strong',
    categories: [
      {
        name: 'Content fidelity',
        score: 30,
        max: 30,
        pct: 100,
        flag: '',
        note: '',
        fromRules: false,
        verdict:
          'Content is fully present in the server HTML — readable by every AI tool, no rendering required.',
      },
      {
        name: 'Token efficiency',
        score: 20,
        max: 20,
        pct: 100,
        flag: '',
        note: '',
        fromRules: false,
        verdict: 'Lean: ~14× HTML-to-content ratio — cheap for an agent to ingest.',
      },
      {
        name: 'Structured data',
        score: 20,
        max: 20,
        pct: 100,
        flag: '',
        note: '',
        fromRules: false,
        verdict:
          'Strong machine-readable metadata (JSON-LD (Organization, WebSite, Article), Open Graph, canonical, meta description).',
      },
      {
        name: 'Crawler access',
        score: 20,
        max: 20,
        pct: 100,
        flag: '',
        note: '',
        fromRules: false,
        verdict:
          'AI crawlers are allowed to fetch the page. robots.txt allows AI crawlers; llms.txt present.',
      },
      {
        name: 'Citation readiness',
        score: 10,
        max: 10,
        pct: 100,
        flag: '',
        note: '',
        fromRules: false,
        verdict: 'Attributable: canonical URL, title, author, date.',
      },
    ],
  },
} as const;

describe.each(['full', 'clean'] as const)('grading (%s report)', (name) => {
  const m = formatAiread(fixture(name));
  const want = GRADING[name];

  it('grade, score and word', () => {
    expect({ grade: m.grade, score: m.score, word: m.word }).toEqual({
      grade: want.grade,
      score: want.score,
      word: want.word,
    });
  });

  it('the five category rows', () => {
    expect(m.categories).toEqual(want.categories);
  });
});

describe('formatAiread extras', () => {
  it('flags the content gap on the full report only', () => {
    expect(formatAiread(fixture('full')).categories.map((c) => c.flag)).toContain(
      'invisible without JS',
    );
    expect(formatAiread(fixture('clean')).categories.every((c) => c.flag === '')).toBe(true);
  });
  it('fires the gate sentences on the full report only', () => {
    expect(formatAiread(fixture('full')).alsoFound.map((f) => f.wedge)).toEqual([
      'tollbooth',
      'agentpass',
      'compliancefix',
      'markdown-origin',
    ]);
    expect(formatAiread(fixture('clean')).alsoFound.map((f) => f.wedge)).toEqual([
      'markdown-origin',
    ]);
  });
  it('words the Markdown row by outcome and tolerates a missing or failed lens', () => {
    const full = formatAiread(fixture('full')).alsoFound.at(-1)!;
    expect(full.text).toBe(
      'This page did not return Markdown when asked for it (Accept: text/markdown). The mod_pagespeed optimizer worker can serve a rendered Markdown copy at the same URL, on your own server. It is experimental and off by default. The module alone never renders Markdown; with AgentOptimize on it only adds Vary: Accept.',
    );
    expect(full.link).toEqual({ label: 'How it works', href: '/docs/agent-optimize/' });
    const clean = formatAiread(fixture('clean')).alsoFound[0];
    expect(clean.text).toBe('This page returns Markdown on request.');
    expect(clean.link).toBeUndefined();
    const absent = fixture('clean');
    delete absent.agentSignals;
    expect(formatAiread(absent).alsoFound).toEqual([]);
    const failed = fixture('full');
    failed.agentSignals.status = 'error';
    expect(formatAiread(failed).alsoFound.map((f) => f.wedge)).not.toContain('markdown-origin');
    const unknown = fixture('full');
    unknown.agentSignals.markdown.negotiated = null;
    expect(formatAiread(unknown).alsoFound.map((f) => f.wedge)).not.toContain('markdown-origin');
  });
  const NO_CHECK =
    '. The scan saw no sign that your server checks agent signatures or license tokens.';
  const first = (r: Parameters<typeof formatAiread>[0]) => formatAiread(r).alsoFound[0].text;

  it('names no count of allowed crawlers when the result has no robots.txt reading', () => {
    const full = fixture('full');
    expect(first(full)).toBe(
      'About 38% of this page’s content is in the raw HTML, and AI crawlers are allowed to read it' +
        NO_CHECK,
    );
    full.access.crawlerRules = { parsed: false, reason: 'missing' };
    expect(first(full)).toContain('and AI crawlers are allowed to read it');
    expect(first({ ...fixture('hosted'), access: {} })).toBe(
      'About 38% of this page’s content is in the raw HTML, and AI crawlers are allowed to read it.',
    );
  });

  it('takes the crawler clause from the robots.txt reading when there is one', () => {
    // Unknown platform, some crawlers blocked: no claim that crawlers may fetch it.
    expect(first(fixture('unknown'))).toBe(
      'About 38% of this page’s content is in the raw HTML' + NO_CHECK,
    );
    const open = fixture('unknown');
    open.access.crawlerRules = fixture('hosted').access.crawlerRules;
    expect(first(open)).toBe(
      'About 38% of this page’s content is in the raw HTML, and robots.txt lets the AI crawlers this scan checks fetch it' +
        NO_CHECK,
    );
    // Hosted: the same two, without the part about the origin.
    const hosted = fixture('hosted');
    expect(first(hosted)).toBe(
      'About 38% of this page’s content is in the raw HTML, and robots.txt lets the AI crawlers this scan checks fetch it.',
    );
    hosted.access.crawlerRules.wildcardBlocksAll = true;
    expect(first(hosted)).toBe('About 38% of this page’s content is in the raw HTML.');
    hosted.access.crawlerRules = fixture('unknown').access.crawlerRules;
    expect(first(hosted)).toBe('About 38% of this page’s content is in the raw HTML.');
  });

  it('says "all" at 100 percent', () => {
    const hosted = fixture('hosted');
    hosted.agentVerifiability.detail.exposurePct = 100;
    expect(first(hosted)).toBe(
      'All of this page’s content is in the raw HTML, and robots.txt lets the AI crawlers this scan checks fetch it.',
    );
    const unknown = fixture('unknown');
    unknown.agentVerifiability.detail.exposurePct = 100;
    expect(first(unknown)).toBe('All of this page’s content is in the raw HTML' + NO_CHECK);
  });

  it('keeps the findings and drops the server sentences on a hosted platform', () => {
    const found = formatAiread(fixture('hosted')).alsoFound;
    expect(found.map((f) => f.wedge)).toEqual([
      'tollbooth',
      'agentpass',
      'compliancefix',
      'markdown-origin',
    ]);
    expect(found[2].text).toMatch(
      /^Automated checks flagged \d+ accessibility issues? on this page\. We don’t issue compliance certificates\.$/,
    );
    expect(found[3]).toEqual({
      wedge: 'markdown-origin',
      text: 'This page did not return Markdown when asked for it (Accept: text/markdown).',
    });
    for (const f of found) {
      expect(f.text).not.toMatch(/your own server|self-hosted|mod_pagespeed|agent signatures/);
      expect(f.link).toBeUndefined();
    }
    // The same report without the platform reading words them as for an unknown platform.
    const plain = fixture('hosted');
    delete plain.platform;
    expect(formatAiread(plain).alsoFound.map((f) => f.text)).toEqual(
      formatAiread({ ...fixture('hosted'), platform: fixture('unknown').platform }).alsoFound.map(
        (f) => f.text,
      ),
    );
    expect(formatAiread(plain).alsoFound[0].text).toContain('your server checks agent signatures');
  });
  it('builds the token line from the report', () => {
    expect(formatAiread(fixture('full')).tokenLine).toBe(
      'Readable text in raw HTML: ~190 tokens · after JavaScript: ~540 tokens',
    );
  });
});

describe('crawlerRulesLine', () => {
  const rules = (patch: Record<string, unknown> = {}) => ({
    access: {
      crawlerRules: {
        parsed: true,
        reason: null,
        truncated: false,
        training: { allowed: ['GPTBot', 'ClaudeBot', 'CCBot'], blocked: [] },
        answer: { allowed: ['OAI-SearchBot', 'PerplexityBot'], blocked: [] },
        wildcardBlocksAll: false,
        sitemaps: [],
        ...patch,
      },
    },
  });

  it('says nothing when the result has no reading', () => {
    expect(crawlerRulesLine({})).toBe('');
    expect(crawlerRulesLine({ access: {} })).toBe('');
    expect(crawlerRulesLine({ access: { crawlerRules: null } })).toBe('');
    expect(crawlerRulesLine(fixture('full'))).toBe('');
    expect(formatAiread(fixture('full')).categories.every((c) => c.note === '')).toBe(true);
    expect(crawlersBlocked(fixture('full'))).toBeNull();
  });

  it('says why there are no rules, without claiming that crawlers may fetch the site', () => {
    expect(crawlerRulesLine(rules({ parsed: false, reason: 'missing' }))).toBe(
      'The scan could not read a robots.txt file for this site.',
    );
    expect(crawlerRulesLine(rules({ parsed: false, reason: 'empty' }))).toBe(
      'robots.txt holds no rules, so it blocks no crawler.',
    );
    expect(crawlerRulesLine(rules({ parsed: false, reason: 'not-robots' }))).toBe(
      'The address /robots.txt does not return a robots.txt file.',
    );
    expect(crawlerRulesLine(rules({ parsed: false, reason: 'other' }))).toBe('');
    expect(crawlersBlocked(rules({ parsed: false, reason: 'missing' }))).toBeNull();
  });

  it('reports a wildcard block, with the crawlers that have a rule of their own', () => {
    expect(
      crawlerRulesLine(
        rules({
          wildcardBlocksAll: true,
          training: { allowed: [], blocked: ['GPTBot', 'ClaudeBot', 'CCBot'] },
          answer: { allowed: [], blocked: ['OAI-SearchBot', 'PerplexityBot'] },
        }),
      ),
    ).toBe('robots.txt blocks every crawler that has no rule of its own.');
    expect(
      crawlerRulesLine(
        rules({
          wildcardBlocksAll: true,
          training: { allowed: ['GPTBot'], blocked: ['ClaudeBot', 'CCBot'] },
          answer: { allowed: ['OAI-SearchBot'], blocked: ['PerplexityBot'] },
        }),
      ),
    ).toBe(
      'robots.txt blocks every crawler that has no rule of its own; 2 AI crawlers have their own rule and may fetch the home page (/).',
    );
    expect(
      crawlerRulesLine(
        rules({
          wildcardBlocksAll: true,
          training: { allowed: ['GPTBot'], blocked: [] },
          answer: { allowed: [], blocked: [] },
        }),
      ),
    ).toBe(
      'robots.txt blocks every crawler that has no rule of its own; 1 AI crawler has its own rule and may fetch the home page (/).',
    );
    expect(crawlersBlocked(rules({ wildcardBlocksAll: true }))).toBe(true);
  });

  it('says the reading is about the home page when none is blocked', () => {
    expect(crawlerRulesLine(rules())).toBe(
      'robots.txt lets all 5 AI crawlers we check fetch the home page (/). Rules for other paths are not read.',
    );
    expect(crawlerRulesLine(fixture('hosted'))).toBe(
      'robots.txt lets all 21 AI crawlers we check fetch the home page (/). Rules for other paths are not read.',
    );
    expect(crawlersBlocked(fixture('hosted'))).toBe(false);
  });

  it('counts and names the blocked crawlers', () => {
    expect(crawlerRulesLine(fixture('unknown'))).toBe(
      'robots.txt blocks 2 of 8 AI crawlers we check for training data and 1 of 13 that fetch pages to answer questions from the home page (/): GPTBot, ClaudeBot, OAI-SearchBot.',
    );
    expect(crawlersBlocked(fixture('unknown'))).toBe(true);
  });

  it('names at most six crawlers, then counts the rest', () => {
    const line = crawlerRulesLine(
      rules({
        training: { allowed: [], blocked: ['A', 'B', 'C', 'D'] },
        answer: { allowed: ['X'], blocked: ['E', 'F', 'G', 'H'] },
      }),
    );
    expect(line).toBe(
      'robots.txt blocks 4 of 4 AI crawlers we check for training data and 4 of 5 that fetch pages to answer questions from the home page (/): A, B, C, D, E, F and 2 more.',
    );
  });

  it('says when part of the file was not read', () => {
    expect(crawlerRulesLine(rules({ truncated: true }))).toBe(
      'robots.txt lets all 5 AI crawlers we check fetch the home page (/). Rules for other paths are not read. Part of the file was not read.',
    );
  });

  it('replaces the verdict of the crawler access row when the file was read', () => {
    for (const name of ['unknown', 'hosted']) {
      const report = fixture(name);
      const rows = formatAiread(report).categories;
      const row = rows.find((c) => c.name === 'Crawler access')!;
      expect(row.fromRules).toBe(true);
      expect(row.verdict).toBe(crawlerRulesLine(report));
      expect(row.note).toBe('');
      // One voice: the scanner's own sentence is not shown next to it.
      expect(row.verdict).not.toContain(report.categories['Crawler access'].verdict);
      expect(rows.filter((c) => c.fromRules || c.note)).toHaveLength(1);
      // The score is the scanner's.
      expect(row.score).toBe(report.categories['Crawler access'].score);
    }
  });

  it('replaces the verdict with the line when the file could not be read', () => {
    for (const [reason, line] of [
      ['missing', 'The scan could not read a robots.txt file for this site.'],
      ['not-robots', 'The address /robots.txt does not return a robots.txt file.'],
    ] as const) {
      const report = fixture('full');
      report.access.crawlerRules = { parsed: false, reason };
      const row = formatAiread(report).categories.find((c) => c.name === 'Crawler access')!;
      expect(row.fromRules).toBe(true);
      expect(row.verdict).toBe(line);
      expect(row.note).toBe('');
    }
  });
});

describe('accessibilityFindings', () => {
  it('orders the rules by impact and then by elements, and bounds what it returns', () => {
    const found = accessibilityFindings({
      accessibility: {
        available: true,
        detail: {
          total: 9,
          topRules: [
            { impact: 'moderate', nodes: 3, help: 'Heading levels should only increase by one' },
            { impact: 'serious', nodes: 2, help: 'Elements must meet minimum color contrast.' },
            { impact: 'serious', nodes: 7.9, help: `  ${'word '.repeat(60)}` },
            { impact: 'odd', nodes: 'x', help: 'Unknown impact' },
            { impact: 'minor', nodes: 1, help: '' },
            { impact: 'critical', nodes: 1, help: 'One' },
            { impact: 'minor', nodes: 1, help: 'Two' },
            { impact: 'minor', nodes: 1, help: 'Three' },
          ],
        },
      },
    })!;
    expect(found.total).toBe(9);
    expect(found.rules).toHaveLength(MAX_RULES);
    expect(found.rules.map((r) => r.nodes)).toEqual([1, 7, 2, 3, 1]);
    expect(found.rules[0].help).toBe('One');
    expect(found.rules[2].help).toBe('Elements must meet minimum color contrast');
    expect(found.rules[1].help.length).toBeLessThanOrEqual(MAX_RULE_TEXT);
    expect(found.rules[1].help.endsWith('…')).toBe(true);
  });
  it('is null when the checks found nothing or did not run', () => {
    expect(accessibilityFindings({})).toBeNull();
    expect(
      accessibilityFindings({ accessibility: { available: false, detail: { total: 3 } } }),
    ).toBeNull();
    expect(
      accessibilityFindings({ accessibility: { available: true, detail: { total: 0 } } }),
    ).toBeNull();
  });
});
