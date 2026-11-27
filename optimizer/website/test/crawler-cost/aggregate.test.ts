// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
import { describe, expect, it } from 'vitest';
import { Aggregator } from '../../src/lib/crawler-cost/aggregate';
import type { ParsedLine } from '../../src/lib/crawler-cost/types';

const T0 = Date.UTC(2026, 9, 10, 13, 55, 36);
function line(over: Partial<ParsedLine>): ParsedLine {
  return {
    time: T0,
    method: 'GET',
    path: '/guide/',
    query: '',
    status: 200,
    bytes: 100,
    referer: null,
    userAgent: 'Mozilla/5.0 Firefox/131',
    ...over,
  };
}
const GPT = 'Mozilla/5.0 (compatible; GPTBot/1.2; +https://openai.com/gptbot)';
const CGU = 'Mozilla/5.0; compatible; ChatGPT-User/1.0; +https://openai.com/bot';

describe('Aggregator', () => {
  it('counts crawler rows, page requests and bytes; fetchers apart from the headline', () => {
    const ag = new Aggregator();
    ag.add(line({ userAgent: GPT, bytes: 5120 }));
    ag.add(line({ userAgent: GPT, path: '/static/app.js', bytes: 20480 }));
    ag.add(line({ userAgent: GPT, path: '/old/', status: 404, bytes: 150, time: T0 + 60_000 }));
    ag.add(line({ userAgent: CGU, bytes: 5120 }));
    const a = ag.result();
    expect(a.requests).toBe(4);
    expect(a.bytes).toBe(30870);
    expect(a.crawlerRequests).toBe(4);
    expect(a.crawlerBytes).toBe(30870);
    expect(a.crawlerPageRequests).toBe(2);
    expect(a.fetcherPageRequests).toBe(1);
    expect(a.crawlers).toEqual([
      {
        token: 'GPTBot',
        operator: 'OpenAI',
        kind: 'training',
        requests: 3,
        bytes: 25750,
        pageRequests: 2,
      },
      {
        token: 'ChatGPT-User',
        operator: 'OpenAI',
        kind: 'fetcher',
        requests: 1,
        bytes: 5120,
        pageRequests: 1,
      },
    ]);
    expect(a.firstTime).toBe(T0);
    expect(a.lastTime).toBe(T0 + 60_000);
  });
  it('counts referred page requests by operator, only for 200/304 page requests by non-crawlers', () => {
    const ag = new Aggregator();
    ag.add(line({ referer: 'https://chatgpt.com/' }));
    ag.add(line({ query: 'utm_source=chatgpt.com' }));
    ag.add(line({ referer: 'https://claude.ai/', status: 304 }));
    ag.add(line({ referer: 'https://www.perplexity.ai/', path: '/favicon.ico' }));
    ag.add(line({ referer: 'https://chatgpt.com/', status: 404 }));
    ag.add(line({ referer: 'https://chatgpt.com/', userAgent: GPT }));
    const a = ag.result();
    expect(a.referredPageRequests).toBe(3);
    expect(a.referredByOperator).toEqual({ OpenAI: 2, Anthropic: 1 });
  });
  it('tracks missing bytes, agentless lines, unparsed samples and files', () => {
    const ag = new Aggregator();
    ag.add(line({ bytes: null, userAgent: null }));
    ag.addUnparsed('junk 1');
    ag.addUnparsed('junk 2');
    ag.addUnparsed('junk 3');
    ag.addUnparsed('x'.repeat(500));
    ag.addFile({
      name: 'a.log',
      format: 'combined',
      gzip: false,
      lines: 1,
      givenUp: false,
      sample: null,
      error: null,
    });
    const a = ag.result();
    expect(a.bytesMissing).toBe(1);
    expect(a.agentlessLines).toBe(1);
    expect(a.unparsed).toBe(4);
    expect(a.unparsedSamples).toEqual(['junk 1', 'junk 2', 'junk 3']);
    expect(a.files).toHaveLength(1);
  });
  it('sorts crawler rows by requests descending then token', () => {
    const ag = new Aggregator();
    ag.add(line({ userAgent: 'ClaudeBot/1.0' }));
    ag.add(line({ userAgent: 'Bytespider' }));
    ag.add(line({ userAgent: GPT }));
    ag.add(line({ userAgent: GPT }));
    expect(ag.result().crawlers.map((r) => r.token)).toEqual(['GPTBot', 'Bytespider', 'ClaudeBot']);
  });
  it('counts a crawler line without a bytes field as a request with 0 bytes', () => {
    const ag = new Aggregator();
    ag.add(line({ userAgent: GPT, bytes: null }));
    const a = ag.result();
    expect(a.crawlerRequests).toBe(1);
    expect(a.crawlerBytes).toBe(0);
    expect(a.crawlers[0]).toMatchObject({ token: 'GPTBot', requests: 1, bytes: 0 });
    expect(a.bytesMissing).toBe(1);
  });
});
