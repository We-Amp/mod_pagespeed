// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
import { describe, expect, it } from 'vitest';
import { emptyAggregate } from '../../src/lib/crawler-cost/aggregate';
import {
  bytesState,
  estimateCost,
  formatBytes,
  formatMoney,
  formatRatio,
  formatSummary,
  headlineRatio,
  operatorRatios,
  parseRate,
  periodDays,
  projectTo30Days,
} from '../../src/lib/crawler-cost/summary';
import type { Aggregate } from '../../src/lib/crawler-cost/types';

const RATES = { perGb: 0.09, perMillionRequests: 0, currency: 'USD' };
function sample(): Aggregate {
  const a = emptyAggregate();
  a.requests = 8;
  a.bytes = 45310;
  a.crawlerRequests = 5;
  a.crawlerBytes = 33870;
  a.crawlerPageRequests = 3;
  a.fetcherPageRequests = 1;
  a.referredPageRequests = 2;
  a.referredByOperator = { OpenAI: 2 };
  a.firstTime = Date.UTC(2026, 9, 1);
  a.lastTime = Date.UTC(2026, 9, 11);
  a.crawlers = [
    {
      token: 'GPTBot',
      operator: 'OpenAI',
      kind: 'training',
      requests: 3,
      bytes: 25750,
      pageRequests: 2,
    },
    {
      token: 'ClaudeBot',
      operator: 'Anthropic',
      kind: 'training',
      requests: 1,
      bytes: 3000,
      pageRequests: 1,
    },
    {
      token: 'ChatGPT-User',
      operator: 'OpenAI',
      kind: 'fetcher',
      requests: 1,
      bytes: 5120,
      pageRequests: 1,
    },
  ];
  return a;
}

describe('rates and cost', () => {
  it('parses a decimal with a comma or a dot and rejects the rest', () => {
    expect(parseRate('0.09')).toBe(0.09);
    expect(parseRate(' 0,09 ')).toBe(0.09);
    expect(parseRate('0')).toBe(0);
    expect(parseRate('')).toBeNull();
    expect(parseRate('-1')).toBeNull();
    expect(parseRate('abc')).toBeNull();
  });
  it('estimates bytes per decimal gigabyte plus requests per million, and refuses bad rates', () => {
    expect(estimateCost(2e9, 0, RATES)).toBeCloseTo(0.18, 10);
    expect(estimateCost(0, 3e6, { ...RATES, perMillionRequests: 0.4 })).toBeCloseTo(1.2, 10);
    expect(
      estimateCost(1e9, 1e6, { perGb: 0.09, perMillionRequests: 0.4, currency: 'USD' }),
    ).toBeCloseTo(0.49, 10);
    expect(
      estimateCost(1, 1, { perGb: Number.NaN, perMillionRequests: 0, currency: 'USD' }),
    ).toBeNull();
    expect(estimateCost(1, 1, { perGb: -1, perMillionRequests: 0, currency: 'USD' })).toBeNull();
  });
});

describe('ratios and period', () => {
  it('divides crawler page requests by referred page requests, null with no referrals', () => {
    expect(headlineRatio(sample())).toBe(1.5);
    const none = sample();
    none.referredPageRequests = 0;
    expect(headlineRatio(none)).toBeNull();
  });
  it('builds per-operator ratios without the fetcher kind', () => {
    expect(operatorRatios(sample())).toEqual([
      {
        operator: 'Anthropic',
        requests: 1,
        bytes: 3000,
        crawlerPageRequests: 1,
        referred: 0,
        ratio: null,
      },
      {
        operator: 'OpenAI',
        requests: 4,
        bytes: 30870,
        crawlerPageRequests: 2,
        referred: 2,
        ratio: 1,
      },
    ]);
  });
  it('leaves Unknown operator rows out of the operator table', () => {
    const a = sample();
    a.crawlers.push({
      token: 'MysteryBot',
      operator: 'Unknown',
      kind: 'other',
      requests: 2,
      bytes: 10,
      pageRequests: 2,
    });
    a.referredByOperator = { OpenAI: 2, Unknown: 1 };
    expect(operatorRatios(a).map((o) => o.operator)).toEqual(['Anthropic', 'OpenAI']);
  });
  it('measures the period in days and projects to 30 days only from a full day on', () => {
    expect(periodDays(sample())).toBe(10);
    expect(projectTo30Days(1, 10)).toBe(3);
    expect(projectTo30Days(1, 0.5)).toBeNull();
    expect(projectTo30Days(1, null)).toBeNull();
    const a = emptyAggregate();
    expect(periodDays(a)).toBeNull();
  });
});

describe('formatting', () => {
  it('formats money, bytes and ratios for people', () => {
    expect(formatMoney(0.49, 'USD')).toBe('0.49 USD');
    expect(formatMoney(0.001, 'EUR')).toBe('less than 0.01 EUR');
    expect(formatMoney(0, 'USD')).toBe('0.00 USD');
    expect(formatBytes(0)).toBe('0 B');
    expect(formatBytes(45310)).toBe('45.3 KB');
    expect(formatBytes(25_750_000_000)).toBe('25.8 GB');
    expect(formatRatio(1.5)).toBe('1.5');
    expect(formatRatio(1234.4)).toBe('1,234');
  });
  it('writes a plain-text summary with the ratio sentence, totals, the top crawlers and no addresses or paths', () => {
    const text = formatSummary(sample(), RATES, 'https://modpagespeed.com/ai-crawler-cost/');
    expect(text).toContain(
      '1.5 crawler page requests per visit an AI assistant sent back (3 crawler page requests, 2 visits)',
    );
    expect(text).toContain(
      'AI crawlers: 5 requests, 33.9 KB, estimated cost less than 0.01 USD at 0.09 USD per GB',
    );
    expect(text).toContain('GPTBot (OpenAI): 3 requests, 25.8 KB');
    expect(text).toContain('https://modpagespeed.com/ai-crawler-cost/');
    expect(text).not.toMatch(/203\.0\.113|\/guide\//);
    const none = sample();
    none.referredPageRequests = 0;
    none.referredByOperator = {};
    expect(formatSummary(none, RATES, 'https://modpagespeed.com/ai-crawler-cost/')).toContain(
      'No visits from AI assistants in this log (3 crawler page requests).',
    );
  });
  it('writes a cost line without a rate clause when the rates are not set', () => {
    const text = formatSummary(sample(), null, 'https://modpagespeed.com/ai-crawler-cost/');
    expect(text).toContain('estimated cost not set (enter a rate)');
    expect(text).not.toContain('per GB');
  });
});

describe('bytes not in the log', () => {
  const URL = 'https://modpagespeed.com/ai-crawler-cost/';
  it('classifies known, partial and missing', () => {
    const a = sample();
    expect(bytesState(a)).toBe('known');
    a.bytesMissing = 3;
    expect(bytesState(a)).toBe('partial');
    a.bytesMissing = a.requests;
    expect(bytesState(a)).toBe('missing');
    expect(bytesState(emptyAggregate())).toBe('known');
  });
  it('says bytes and cost are not in the log, with no per-crawler bytes', () => {
    const a = sample();
    a.bytesMissing = a.requests;
    const text = formatSummary(a, RATES, URL);
    expect(text).toContain(
      'AI crawlers: 5 requests, bytes not in this log, estimated cost not in this log',
    );
    expect(text).toContain('- GPTBot (OpenAI): 3 requests');
    expect(text).not.toMatch(/KB|per GB/);
  });
  it('prices requests only when a per-million rate is set', () => {
    const a = sample();
    a.bytesMissing = a.requests;
    const text = formatSummary(a, { ...RATES, perMillionRequests: 0.4 }, URL);
    expect(text).toContain(
      'AI crawlers: 5 requests, bytes not in this log, estimated cost less than 0.01 USD from requests only at 0.4 USD per million requests',
    );
  });
  it('marks a partial total and keeps the cost', () => {
    const a = sample();
    a.bytesMissing = 2;
    expect(formatSummary(a, RATES, URL)).toContain(
      'AI crawlers: 5 requests, 33.9 KB (partial: 2 lines carry no bytes field), estimated cost',
    );
  });
});
