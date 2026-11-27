// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
import { readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { describe, expect, it } from 'vitest';
import { GIVE_UP_WINDOW, analyzeFiles } from '../../src/lib/crawler-cost/analyze';
import type { LineSource } from '../../src/lib/crawler-cost/analyze';

const fixture = (name: string) =>
  readFileSync(new URL(`../fixtures/crawler-cost/${name}`, import.meta.url));
const enc = new TextEncoder();
function src(name: string, data: Uint8Array | string): LineSource {
  const blob = new Blob([(typeof data === 'string' ? enc.encode(data) : data) as BlobPart]);
  return { name, size: blob.size, stream: () => blob.stream() };
}
const T = (h: number, m: number, s: number) => Date.UTC(2026, 9, 10, h, m, s);

describe('analyzeFiles goldens', () => {
  it('nginx combined', async () => {
    const a = await analyzeFiles([src('access.log', fixture('cc-nginx-combined.log'))]);
    expect(a).toMatchObject({
      requests: 8,
      bytes: 45310,
      bytesMissing: 0,
      unparsed: 1,
      agentlessLines: 0,
      firstTime: T(13, 55, 36),
      lastTime: T(14, 4, 0),
      crawlerRequests: 5,
      crawlerBytes: 33870,
      crawlerPageRequests: 3,
      fetcherPageRequests: 1,
      referredPageRequests: 2,
      referredByOperator: { OpenAI: 2 },
    });
    expect(a.unparsedSamples).toEqual(['this is not a log line']);
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
      {
        token: 'ClaudeBot',
        operator: 'Anthropic',
        kind: 'training',
        requests: 1,
        bytes: 3000,
        pageRequests: 1,
      },
    ]);
    expect(a.files).toEqual([
      {
        name: 'access.log',
        format: 'combined',
        gzip: false,
        lines: 8,
        givenUp: false,
        sample: 'this is not a log line',
        error: null,
      },
    ]);
  });
  it('apache combined with a vhost prefix and dash bytes', async () => {
    const a = await analyzeFiles([src('access_log', fixture('cc-apache-combined.log'))]);
    expect(a).toMatchObject({
      requests: 4,
      bytes: 8120,
      crawlerPageRequests: 2,
      referredPageRequests: 2,
      referredByOperator: { Perplexity: 1, Anthropic: 1 },
    });
    expect(a.crawlers.map((r) => [r.token, r.requests, r.bytes, r.pageRequests])).toEqual([
      ['ClaudeBot', 1, 3000, 1],
      ['GPTBot', 1, 0, 1],
    ]);
    expect(a.files[0].format).toBe('combined');
  });
  it('IIS W3C with a second Fields directive mid-file', async () => {
    const a = await analyzeFiles([src('u_ex261010.log', fixture('cc-iis-w3c.log'))]);
    expect(a).toMatchObject({
      requests: 4,
      bytes: 8120,
      bytesMissing: 2,
      crawlerPageRequests: 2,
      referredPageRequests: 2,
      referredByOperator: { OpenAI: 2 },
    });
    expect(a.crawlers.map((r) => [r.token, r.requests, r.bytes])).toEqual([
      ['ClaudeBot', 1, 3000],
      ['GPTBot', 1, 0],
    ]);
    expect(a.files[0]).toMatchObject({ format: 'iis', lines: 4, givenUp: false });
  });
  it('CRLF line endings give the same numbers', async () => {
    const crlf = fixture('cc-nginx-combined.log').toString('utf8').replace(/\n/g, '\r\n');
    const a = await analyzeFiles([src('access.log', crlf)]);
    expect(a).toMatchObject({ requests: 8, bytes: 45310, unparsed: 1 });
  });
  it('an nginx format with fields after the user agent parses', async () => {
    const line =
      '203.0.113.6 - - [10/Oct/2026:13:56:01 +0000] "GET /guide/ HTTP/1.1" 200 5120 "-" "Mozilla/5.0 (compatible; GPTBot/1.2; +https://openai.com/gptbot)" 0.123 "203.0.113.99, 10.0.0.1" rt=0.120\n';
    const a = await analyzeFiles([src('access.log', line)]);
    expect(a.crawlers[0]).toMatchObject({ token: 'GPTBot', bytes: 5120 });
    expect(a.unparsed).toBe(0);
  });
  it('a gzip file under a .log name and a concatenated two-member archive both match the plain result', async () => {
    const text = fixture('cc-nginx-combined.log').toString('utf8');
    const lines = text.split('\n');
    const part1 = lines.slice(0, 4).join('\n') + '\n';
    const part2 = lines.slice(4).join('\n');
    const twoMembers = new Uint8Array(Buffer.concat([gzipSync(part1), gzipSync(part2)]));
    const plain = await analyzeFiles([src('access.log', text)]);
    const one = await analyzeFiles([src('access.log', new Uint8Array(gzipSync(text)))]);
    const two = await analyzeFiles([src('access.log.gz', twoMembers)]);
    for (const a of [one, two]) {
      expect(a).toMatchObject({
        requests: plain.requests,
        bytes: plain.bytes,
        crawlerPageRequests: 3,
        referredPageRequests: 2,
      });
      expect(a.files[0].gzip).toBe(true);
    }
  });
  it('a truncated gzip file reports an error on the file and keeps going with the next file', async () => {
    const whole = new Uint8Array(
      gzipSync(fixture('cc-nginx-combined.log').toString('utf8').repeat(500)),
    );
    const cut = whole.subarray(0, whole.length - 12);
    const a = await analyzeFiles([
      src('broken.log.gz', cut),
      src('ok.log', fixture('cc-apache-combined.log')),
    ]);
    expect(a.files[0].error).toMatch(/gzip/);
    expect(a.files[1]).toMatchObject({ format: 'combined', lines: 4, error: null });
  });
  it('several files are summed and progress reaches the total', async () => {
    const seen: Array<[number, number]> = [];
    const a = await analyzeFiles(
      [
        src('a.log', fixture('cc-nginx-combined.log')),
        src('b.log', fixture('cc-apache-combined.log')),
      ],
      {
        onProgress: (read, total) => seen.push([read, total]),
      },
    );
    expect(a.requests).toBe(12);
    const last = seen[seen.length - 1];
    expect(last[0]).toBe(last[1]);
  });
  it('gives up on a file whose first 200 lines are mostly junk', async () => {
    const junk = Array.from({ length: 300 }, (_, i) => `junk line ${i}`).join('\n') + '\n';
    const reports: string[] = [];
    const seen: Array<[number, number]> = [];
    const a = await analyzeFiles([src('junk.txt', junk)], {
      onFile: (r) => reports.push(r.name),
      onProgress: (r, t) => seen.push([r, t]),
    });
    expect(a.files[0]).toMatchObject({ format: 'unknown', givenUp: true, lines: 0 });
    expect(a.unparsed).toBe(GIVE_UP_WINDOW);
    expect(a.unparsedSamples[0]).toBe('junk line 0');
    expect(a.files[0].sample).toBe('junk line 0');
    expect(reports).toEqual(['junk.txt']);
    const last = seen[seen.length - 1];
    expect(last[0]).toBe(last[1]);
  });
  it('gives up on a file made only of blank lines', async () => {
    const a = await analyzeFiles([src('blank.txt', '\n'.repeat(GIVE_UP_WINDOW * 5))]);
    expect(a.files[0]).toMatchObject({ givenUp: true, lines: 0 });
  });
  it('a long run of blank lines after real lines stops the read without discarding them', async () => {
    const log = fixture('cc-nginx-combined.log').toString('utf8');
    const a = await analyzeFiles([src('access.log', log + '\n'.repeat(GIVE_UP_WINDOW * 5))]);
    expect(a.files[0]).toMatchObject({ givenUp: false });
    expect(a.requests).toBe(8);
  });
  it('blank lines inside a real log do not trigger the give-up', async () => {
    const log = fixture('cc-nginx-combined.log').toString('utf8');
    const a = await analyzeFiles([src('access.log', '\n'.repeat(50) + log)]);
    expect(a.files[0]).toMatchObject({ givenUp: false });
    expect(a.requests).toBe(8);
  });
  it('gives up at the end of a short file that is mostly junk', async () => {
    const junk = Array.from({ length: 150 }, (_, i) => `junk line ${i}`).join('\n') + '\n';
    const a = await analyzeFiles([src('junk.txt', junk)]);
    expect(a.files[0]).toMatchObject({
      format: 'unknown',
      givenUp: true,
      lines: 0,
      sample: 'junk line 0',
    });
    expect(a.unparsed).toBe(150);
  });
  it('gives up on a one-line JSON file', async () => {
    const a = await analyzeFiles([src('x.json', '{"a":1}')]);
    expect(a.files[0]).toMatchObject({ givenUp: true, lines: 0, sample: '{"a":1}' });
  });
  it('reports an empty file and a blank-lines file as no lines, not given up', async () => {
    const a = await analyzeFiles([src('empty.log', ''), src('blank.log', '\n\n  \n')]);
    for (const f of a.files) {
      expect(f).toMatchObject({ lines: 0, givenUp: false, sample: null });
    }
    expect(a.unparsed).toBe(0);
  });
  it('counts only parsed and unparsed lines toward the window, not blanks or directives', async () => {
    const build = (good: number, bad: number) => {
      const out = ['#Fields: date time cs-method cs-uri-stem cs(User-Agent) sc-status sc-bytes'];
      const body = [
        ...Array.from({ length: good }, () => '2026-10-10 14:00:00 GET /a Mozilla/5.0 200 10'),
        ...Array.from({ length: bad }, (_, i) => `junk ${i}`),
      ];
      body.forEach((l, i) => {
        out.push(l);
        if (i % 10 === 0) out.push('', '#Software: x');
      });
      return out.join('\n') + '\n';
    };
    const at200 = await analyzeFiles([src('a.log', build(99, 101))]);
    expect(at200.files[0]).toMatchObject({ format: 'iis', givenUp: true, lines: 99 });
    expect(at200.unparsed).toBe(101);
    // 199 counted lines with a good majority: not given up. (With a junk
    // majority the end-of-file rule gives up, see the short-file test.)
    const at199 = await analyzeFiles([src('b.log', build(100, 99))]);
    expect(at199.files[0]).toMatchObject({ givenUp: false, lines: 100 });
    expect(at199.unparsed).toBe(99);
  });
  it('an Apache common log and an IIS log without a user agent column are reported as format common', async () => {
    const common = '203.0.113.6 - - [10/Oct/2026:13:56:01 +0000] "GET /guide/ HTTP/1.1" 200 5120\n';
    const iis =
      '#Fields: date time cs-method cs-uri-stem sc-status\n2026-10-10 14:00:00 GET /pricing/ 200\n';
    const a = await analyzeFiles([src('c.log', common), src('i.log', iis)]);
    expect(a.files.map((f) => f.format)).toEqual(['common', 'common']);
    expect(a.files.map((f) => f.lines)).toEqual([1, 1]);
    expect(a.requests).toBe(0);
    expect(a.agentlessLines).toBe(0);
    expect(a.crawlers).toEqual([]);
  });
  it('a common log dropped with a combined log does not enter the totals', async () => {
    const common =
      [1, 2, 3]
        .map(
          (i) => `203.0.113.${i} - - [10/Oct/2026:13:56:01 +0000] "GET /g/${i} HTTP/1.1" 200 5120`,
        )
        .join('\n') + '\n';
    const a = await analyzeFiles([
      src('c.log', common),
      src('n.log', fixture('cc-nginx-combined.log')),
    ]);
    expect(a.requests).toBe(8);
    expect(a.files[0]).toMatchObject({ format: 'common', lines: 3 });
  });
  it('an IIS file whose later #Fields line adds the user agent column counts only that block', async () => {
    const iis =
      '#Fields: date time cs-method cs-uri-stem sc-status\n' +
      '2026-10-10 14:00:00 GET /a/ 200\n' +
      '#Fields: date time cs-method cs-uri-stem sc-status cs(User-Agent)\n' +
      '2026-10-10 14:00:01 GET /b/ 200 Mozilla/5.0\n';
    const a = await analyzeFiles([src('i.log', iis)]);
    expect(a.files[0]).toMatchObject({ format: 'iis', lines: 2 });
    expect(a.requests).toBe(1);
  });
  it('a privacy-stripped log yields no crawlers, counts the referral, and flags agentless lines', async () => {
    const a = await analyzeFiles([src('access.log', fixture('cc-stripped.log'))]);
    expect(a).toMatchObject({
      requests: 3,
      agentlessLines: 3,
      crawlerRequests: 0,
      referredPageRequests: 1,
    });
    expect(a.crawlers).toEqual([]);
  });
  it('hostile input parses without throwing, as plain strings', async () => {
    const extra = 'A'.repeat(1_000_000) + '\n';
    const bad = new Uint8Array([0x61, 0xff, 0x20, 0x2d, 0x20, 0x2d, 0x0a]);
    const a = await analyzeFiles([
      src('h.log', fixture('cc-hostile.log')),
      src('big.log', extra),
      src('bad.log', bad),
    ]);
    expect(a.crawlers[0]).toMatchObject({ token: 'GPTBot', requests: 1, bytes: 10 });
    expect(a.requests).toBe(2);
    expect(a.unparsed).toBeGreaterThanOrEqual(3);
    expect(a.unparsedSamples.some((s) => s.startsWith('AAAA'))).toBe(true);
  });
});
