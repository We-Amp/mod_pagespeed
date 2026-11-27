// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
import { describe, expect, it } from 'vitest';
import {
  classifyUserAgent,
  isPagePath,
  referralOperator,
} from '../../src/lib/crawler-cost/classify';

describe('classifyUserAgent', () => {
  it('matches a token case-insensitively as a substring of the claimed agent', () => {
    expect(
      classifyUserAgent('Mozilla/5.0 (compatible; GPTBot/1.2; +https://openai.com/gptbot)')?.token,
    ).toBe('GPTBot');
    expect(classifyUserAgent('mozilla/5.0 (compatible; gptbot/1.2)')?.token).toBe('GPTBot');
  });
  it('tells the fetcher apart from the crawler of the same operator', () => {
    expect(
      classifyUserAgent(
        'Mozilla/5.0 AppleWebKit/537.36; compatible; ChatGPT-User/1.0; +https://openai.com/bot',
      )?.kind,
    ).toBe('fetcher');
    expect(
      classifyUserAgent('Mozilla/5.0 (compatible; ClaudeBot/1.0; +claudebot@anthropic.com)')?.kind,
    ).toBe('training');
  });
  it('returns null for browsers, missing agents and robots-only tokens', () => {
    expect(classifyUserAgent('Mozilla/5.0 (Windows NT 10.0) Firefox/131')).toBeNull();
    expect(classifyUserAgent(null)).toBeNull();
    expect(classifyUserAgent('Google-Extended')).toBeNull();
  });
  it('uses the first token in file order when two match', () => {
    const list = [
      {
        token: 'bot',
        operator: 'A',
        kind: 'other' as const,
        docs: 'https://a.example',
        added: '2026-10-14',
      },
      {
        token: 'GPTBot',
        operator: 'B',
        kind: 'training' as const,
        docs: 'https://b.example',
        added: '2026-10-14',
      },
    ];
    expect(classifyUserAgent('GPTBot/1.0', list)?.operator).toBe('A');
  });
});

describe('isPagePath', () => {
  it('treats extensionless paths and .html as pages', () => {
    expect(isPagePath('/guide/')).toBe(true);
    expect(isPagePath('/guide')).toBe(true);
    expect(isPagePath('/docs/intro.html')).toBe(true);
    expect(isPagePath('/docs/intro.htm')).toBe(true);
    expect(isPagePath('/')).toBe(true);
  });
  it('treats static asset extensions as assets', () => {
    for (const p of [
      '/static/app.js',
      '/a.css',
      '/i.png',
      '/f.woff2',
      '/x.json',
      '/robots.txt',
      '/s.xml',
      '/v.mp4',
      '/d.pdf',
      '/m.js.map',
      '/l.gz',
    ]) {
      expect(isPagePath(p), p).toBe(false);
    }
  });
  it('ignores the case of the extension and a dot inside a directory name', () => {
    expect(isPagePath('/IMG.PNG')).toBe(false);
    expect(isPagePath('/v1.2/guide')).toBe(true);
  });
});

describe('referralOperator', () => {
  it('matches the referer host, its www form and its subdomains', () => {
    expect(referralOperator('https://chatgpt.com/', '')).toBe('OpenAI');
    expect(referralOperator('https://www.perplexity.ai/search/x', '')).toBe('Perplexity');
    expect(referralOperator('https://chat.openai.com/c/1', '')).toBe('OpenAI');
    expect(referralOperator('https://app.claude.ai/', '')).toBe('Anthropic');
  });
  it('matches a query marker when the referer is missing', () => {
    expect(referralOperator(null, 'utm_source=chatgpt.com')).toBe('OpenAI');
    expect(referralOperator('-', 'a=1&utm_source=chatgpt.com&b=2')).toBe('OpenAI');
  });
  it('returns null for search engines, own-site referers, junk and nothing', () => {
    expect(referralOperator('https://www.google.com/', '')).toBeNull();
    expect(referralOperator('https://example.com/guide/', 'utm_source=newsletter')).toBeNull();
    expect(referralOperator('javascript:alert(1)', '')).toBeNull();
    expect(referralOperator('notaurl', '')).toBeNull();
    expect(referralOperator(null, '')).toBeNull();
  });
  it('does not match a host that merely ends with a listed name', () => {
    expect(referralOperator('https://notchatgpt.com/', '')).toBeNull();
  });
});
