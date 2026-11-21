// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Grading parity: the v2 panel's formatter must produce the same grade, score,
// word and category rows as the original result page. The original is an
// inline script, so its grading block is sliced out of the page source and
// run in a sandbox, the same way the gate sync test does it.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import { describe, expect, it } from 'vitest';
import { crawlerRulesLine, crawlersBlocked, formatAiread } from './airead';

const PAGE = fileURLToPath(new URL('../../pages/ai-readability/index.astro', import.meta.url));
const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      fileURLToPath(new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url)),
      'utf8',
    ),
  ).report;

function v1Html(r: unknown): { grade: string; cats: string } {
  const src = readFileSync(PAGE, 'utf8');
  const escStart = src.indexOf('const esc = (s) =>');
  const gradeStart = src.indexOf('const grade =\n', src.indexOf('function render(data)'));
  const gradeEnd = src.indexOf('// SECTION 2', gradeStart);
  if (escStart < 0 || gradeStart < 0 || gradeEnd < 0) throw new Error('v1 anchors moved');
  const escSrc = extractStatement(src, escStart);
  const code = `${escSrc}\n${src.slice(gradeStart, gradeEnd)}\nglobalThis.__out__ = { grade, cats };`;
  const ctx = vm.createContext({ r });
  vm.runInContext(code, ctx);
  return (ctx as { __out__: { grade: string; cats: string } }).__out__;
}

// Take a `const x = ...;` statement by balancing brackets up to the closing semicolon.
function extractStatement(src: string, start: number): string {
  let depth = 0;
  for (let i = start; i < src.length; i++) {
    const ch = src[i];
    if ('([{'.includes(ch)) depth++;
    else if (')]}'.includes(ch)) depth--;
    else if (ch === ';' && depth === 0) return src.slice(start, i + 1);
  }
  throw new Error('unterminated statement');
}

const decode = (s: string) =>
  s
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, '&');
// Drop everything between a '<' and the next '>' by walking the string, so no
// tag pattern is matched with a regular expression.
function stripTags(html: string): string {
  let out = '';
  let inTag = false;
  for (const ch of html) {
    if (ch === '<') inTag = true;
    else if (ch === '>' && inTag) inTag = false;
    else if (!inTag) out += ch;
  }
  return out;
}
const text = (html: string) => decode(stripTags(html));

describe.each(['full', 'clean'])('grading parity (%s report)', (name) => {
  const report = fixture(name);
  const m = formatAiread(report);
  const v1 = v1Html(report);

  it('grade, score and word match', () => {
    expect(v1.grade).toContain(`>${m.grade}</div>`);
    expect(v1.grade).toContain(`<div class="ar-score">${m.score}/100</div>`);
    expect(text(v1.grade)).toContain(`Grade ${m.grade} — ${m.word}`);
    expect(m.word).not.toBe('');
  });

  it('the five category rows match', () => {
    const rows = v1.cats.split('<div class="ar-cat">').slice(1);
    expect(rows).toHaveLength(5);
    expect(m.categories).toHaveLength(5);
    rows.forEach((row, i) => {
      const c = m.categories[i];
      const flag = /<span class="ar-flag">([^<]*)<\/span>/.exec(row)?.[1] ?? '';
      const nameSpan =
        /<div class="ar-top"><span>(.*?)(?:<span class="ar-flag">|<\/span><span>)/.exec(row)![1];
      expect(decode(nameSpan)).toBe(c.name);
      expect(flag).toBe(c.flag);
      expect(row).toContain(`<span>${c.score}/${c.max}</span>`);
      expect(row).toContain(`<i style="width:${c.pct}%">`);
      expect(decode(/<div class="ar-v">(.*?)<\/div>/.exec(row)![1])).toBe(c.verdict);
      expect(text(row)).toContain(c.verdict);
    });
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
  const IMPOSTOR = ', but your origin can’t tell a real signed agent from an impostor.';
  const first = (r: Parameters<typeof formatAiread>[0]) => formatAiread(r).alsoFound[0].text;

  it('names no count of allowed crawlers when the result has no robots.txt reading', () => {
    const full = fixture('full');
    expect(first(full)).toBe(
      'About 38% of this page’s content is in the raw HTML, and AI crawlers are allowed to read it' +
        IMPOSTOR,
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
      'About 38% of this page’s content is in the raw HTML' + IMPOSTOR,
    );
    const open = fixture('unknown');
    open.access.crawlerRules = fixture('hosted').access.crawlerRules;
    expect(first(open)).toBe(
      'About 38% of this page’s content is in the raw HTML, and robots.txt lets AI crawlers fetch it' +
        IMPOSTOR,
    );
    // Hosted: the same two, without the part about the origin.
    const hosted = fixture('hosted');
    expect(first(hosted)).toBe(
      'About 38% of this page’s content is in the raw HTML, and robots.txt lets AI crawlers fetch it.',
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
      'All of this page’s content is in the raw HTML, and robots.txt lets AI crawlers fetch it.',
    );
    const unknown = fixture('unknown');
    unknown.agentVerifiability.detail.exposurePct = 100;
    expect(first(unknown)).toBe('All of this page’s content is in the raw HTML' + IMPOSTOR);
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
      expect(f.text).not.toMatch(/your own server|self-hosted|mod_pagespeed|impostor/);
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
    expect(formatAiread(plain).alsoFound[0].text).toContain('your origin can’t tell');
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
