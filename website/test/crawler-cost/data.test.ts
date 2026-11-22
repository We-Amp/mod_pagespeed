// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import crawlers from '../../src/data/ai-crawlers.json';
import referrers from '../../src/data/answer-engine-referrers.json';
import { ROBOTS_ONLY_TOKENS } from '../../src/lib/crawler-cost/classify';

const DATE = /^\d{4}-\d{2}-\d{2}$/;
const KINDS = new Set(['training', 'search', 'fetcher', 'other']);

describe('ai-crawlers.json', () => {
  it('has unique tokens, every field, valid kinds and dates', () => {
    const tokens = crawlers.map((c) => c.token.toLowerCase());
    expect(new Set(tokens).size).toBe(tokens.length);
    for (const c of crawlers) {
      expect(c.token.length).toBeGreaterThan(2);
      expect(c.operator.length).toBeGreaterThan(0);
      expect(KINDS.has(c.kind)).toBe(true);
      expect(c.docs).toMatch(/^https:\/\//);
      expect(c.added).toMatch(DATE);
      expect([true, false, 'unknown']).toContain(c.executes_js);
    }
  });
  it('sets executes_js only where the operator documents it', () => {
    // CCBot: commoncrawl.org/faq, "JavaScript is not executed and Cookies are not used."
    const set = crawlers.filter((c) => c.executes_js !== 'unknown');
    expect(set.map((c) => [c.token, c.executes_js])).toEqual([['CCBot', false]]);
  });
  it('carries the tokens the goldens rely on, with their kinds', () => {
    const byToken = new Map(crawlers.map((c) => [c.token, c]));
    expect(byToken.get('GPTBot')).toMatchObject({ operator: 'OpenAI', kind: 'training' });
    expect(byToken.get('ChatGPT-User')).toMatchObject({ operator: 'OpenAI', kind: 'fetcher' });
    expect(byToken.get('OAI-SearchBot')).toMatchObject({ operator: 'OpenAI', kind: 'search' });
    expect(byToken.get('ClaudeBot')).toMatchObject({ operator: 'Anthropic', kind: 'training' });
    expect(byToken.get('Claude-User')).toMatchObject({ operator: 'Anthropic', kind: 'fetcher' });
    expect(byToken.get('PerplexityBot')).toMatchObject({ operator: 'Perplexity', kind: 'search' });
    expect(byToken.get('Perplexity-User')).toMatchObject({
      operator: 'Perplexity',
      kind: 'fetcher',
    });
    expect(byToken.get('Bytespider')).toMatchObject({ operator: 'ByteDance', kind: 'training' });
    expect(byToken.get('CCBot')).toMatchObject({ operator: 'Common Crawl' });
    expect(byToken.get('Meta-ExternalAgent')).toMatchObject({ operator: 'Meta', kind: 'training' });
  });
  it('lists a token that contains another listed token before it', () => {
    const tokens = crawlers.map((c) => c.token.toLowerCase());
    tokens.forEach((t, i) => {
      for (const earlier of tokens.slice(0, i))
        expect(t.includes(earlier), `${earlier} shadows ${t}`).toBe(false);
    });
    const byToken = new Map(crawlers.map((c) => [c.token, c]));
    expect(byToken.get('Diffbot-User')).toMatchObject({ operator: 'Diffbot', kind: 'fetcher' });
    expect(byToken.get('AI2Bot-DeepResearchEval')).toMatchObject({ kind: 'fetcher' });
  });
  it('holds no robots.txt-only control token', () => {
    const lower = new Set(crawlers.map((c) => c.token.toLowerCase()));
    for (const t of ROBOTS_ONLY_TOKENS) expect(lower.has(t.toLowerCase())).toBe(false);
  });
  it('ships the MIT notice for the seed list beside the data', () => {
    const notice = readFileSync(
      new URL('../../src/data/ai-crawlers-LICENSE.txt', import.meta.url),
      'utf8',
    );
    expect(notice).toMatch(/MIT License/);
    expect(notice).toMatch(/ai\.robots\.txt/);
  });
});

describe('answer-engine-referrers.json', () => {
  it('has lowercase hosts without www, unique, with operator, product and a checked date', () => {
    const hosts = referrers.hosts.map((h) => h.host);
    expect(new Set(hosts).size).toBe(hosts.length);
    for (const h of referrers.hosts) {
      expect(h.host).toBe(h.host.toLowerCase());
      expect(h.host.startsWith('www.')).toBe(false);
      expect(h.operator.length).toBeGreaterThan(0);
      expect(h.product.length).toBeGreaterThan(0);
      expect(h.checked).toMatch(DATE);
    }
    expect(hosts).toContain('chatgpt.com');
    expect(hosts).toContain('perplexity.ai');
    expect(hosts).toContain('claude.ai');
  });
  it('starts the query markers with utm_source=chatgpt.com', () => {
    expect(referrers.queryMarkers[0]).toEqual({
      param: 'utm_source',
      value: 'chatgpt.com',
      operator: 'OpenAI',
    });
  });
  it('spells an operator identically in the crawler and referrer files', () => {
    const names = new Set(crawlers.map((c) => c.operator));
    for (const r of referrers.hosts) {
      for (const name of names) {
        if (name.toLowerCase() === r.operator.toLowerCase()) expect(name).toBe(r.operator);
      }
    }
  });
});
