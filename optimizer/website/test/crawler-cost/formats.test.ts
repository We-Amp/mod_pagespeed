// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
import { describe, expect, it } from 'vitest';
import {
  IisParser,
  detectFormat,
  parseClfTime,
  parseCombinedLine,
  splitRequest,
} from '../../src/lib/crawler-cost/formats';

const NGINX =
  '203.0.113.6 - - [10/Oct/2026:13:56:01 +0000] "GET /guide/?x=1 HTTP/1.1" 200 5120 "https://chatgpt.com/" "Mozilla/5.0 (compatible; GPTBot/1.2; +https://openai.com/gptbot)"';

describe('parseClfTime', () => {
  it('parses the common log time with its offset', () => {
    expect(parseClfTime('10/Oct/2026:13:56:01 +0000')).toBe(Date.UTC(2026, 9, 10, 13, 56, 1));
    expect(parseClfTime('10/Oct/2026:15:56:01 +0200')).toBe(Date.UTC(2026, 9, 10, 13, 56, 1));
    expect(parseClfTime('10/Oct/2026:08:56:01 -0500')).toBe(Date.UTC(2026, 9, 10, 13, 56, 1));
  });
  it('returns null for junk', () => {
    expect(parseClfTime('yesterday')).toBeNull();
    expect(parseClfTime('10/Foo/2026:13:56:01 +0000')).toBeNull();
  });
});

describe('splitRequest', () => {
  it('splits method, path and query', () => {
    expect(splitRequest('GET /guide/?x=1 HTTP/1.1')).toEqual({
      method: 'GET',
      path: '/guide/',
      query: 'x=1',
    });
    expect(splitRequest('POST /api HTTP/2.0')).toEqual({ method: 'POST', path: '/api', query: '' });
  });
  it('keeps a malformed request line as the path with no method', () => {
    expect(splitRequest('-')).toEqual({ method: '', path: '-', query: '' });
    expect(splitRequest('\\x16\\x03\\x01')).toEqual({
      method: '',
      path: '\\x16\\x03\\x01',
      query: '',
    });
  });
});

describe('parseCombinedLine', () => {
  it('parses an nginx combined line', () => {
    expect(parseCombinedLine(NGINX)).toEqual({
      time: Date.UTC(2026, 9, 10, 13, 56, 1),
      method: 'GET',
      path: '/guide/',
      query: 'x=1',
      status: 200,
      bytes: 5120,
      referer: 'https://chatgpt.com/',
      userAgent: 'Mozilla/5.0 (compatible; GPTBot/1.2; +https://openai.com/gptbot)',
    });
  });
  it('parses Apache vhost_combined (prefix) and "-" bytes as zero', () => {
    const line =
      'www.example.com:443 203.0.113.6 - - [10/Oct/2026:13:56:01 +0000] "GET /guide/ HTTP/1.1" 200 - "-" "GPTBot/1.2"';
    const p = parseCombinedLine(line);
    expect(p?.bytes).toBe(0);
    expect(p?.referer).toBeNull();
    expect(p?.userAgent).toBe('GPTBot/1.2');
  });
  it('parses an nginx custom format with fields after the user agent and ignores them', () => {
    const line = NGINX + ' 0.123 "203.0.113.99, 10.0.0.1" rt=0.120';
    expect(parseCombinedLine(line)?.userAgent).toBe(
      'Mozilla/5.0 (compatible; GPTBot/1.2; +https://openai.com/gptbot)',
    );
  });
  it('parses Apache common with null referer and agent', () => {
    const p = parseCombinedLine(
      '203.0.113.6 - frank [10/Oct/2026:13:56:01 +0000] "GET /guide/ HTTP/1.1" 304 0',
    );
    expect(p).toMatchObject({ status: 304, bytes: 0, referer: null, userAgent: null });
  });
  it('returns null for junk and for a line without the core', () => {
    expect(parseCombinedLine('this is not a log line')).toBeNull();
    expect(parseCombinedLine('')).toBeNull();
    expect(
      parseCombinedLine('203.0.113.6 - - [10/Oct/2026:13:56:01 +0000] "GET /guide/ HTTP/1.1"'),
    ).toBeNull();
  });
  it('keeps a hostile request and agent as plain strings', () => {
    const line =
      '<script>x</script> - - [10/Oct/2026:13:55:36 +0000] "GET /<img src=x onerror=alert(1)> HTTP/1.1" 200 10 "javascript:alert(1)" "<script>document.title=1</script> GPTBot/1.0"';
    const p = parseCombinedLine(line);
    expect(p?.path).toBe('/<img');
    expect(p?.userAgent).toContain('<script>');
  });
});

describe('parseCombinedLine hostile input', () => {
  it('stays linear on a flood of opening brackets', () => {
    const t0 = performance.now();
    expect(parseCombinedLine('['.repeat(65_536))).toBeNull();
    const ms = performance.now() - t0;
    expect(ms).toBeLessThan(50);
  });
  it('stays linear on an escaped-quote line followed by a bracket flood', () => {
    const line = 'x - - [' + '['.repeat(8_000) + '] "GET /a\\" HTTP/1.1" 200 1';
    const t0 = performance.now();
    expect(parseCombinedLine(line)).toBeNull();
    const ms = performance.now() - t0;
    expect(ms).toBeLessThan(50);
  });
});

describe('detectFormat', () => {
  it('detects IIS by its directives, combined and common by the core, unknown otherwise', () => {
    expect(detectFormat('#Software: Microsoft Internet Information Services 10.0')).toBe('iis');
    expect(detectFormat('#Fields: date time cs-method')).toBe('iis');
    expect(detectFormat(NGINX)).toBe('combined');
    expect(
      detectFormat('203.0.113.6 - - [10/Oct/2026:13:56:01 +0000] "GET / HTTP/1.1" 200 12'),
    ).toBe('common');
    expect(detectFormat('hello')).toBe('unknown');
  });
});

describe('IisParser', () => {
  const HEADER =
    '#Fields: date time s-ip cs-method cs-uri-stem cs-uri-query s-port cs-username c-ip cs(User-Agent) cs(Referer) sc-status sc-substatus sc-win32-status time-taken';
  it('reads the Fields directive, decodes + as space, maps "-" to empty or null, and has no bytes without sc-bytes', () => {
    const p = new IisParser();
    expect(p.parseLine('#Software: Microsoft Internet Information Services 10.0')).toBe(
      'directive',
    );
    expect(p.parseLine(HEADER)).toBe('directive');
    expect(p.hasBytes).toBe(false);
    expect(p.hasUserAgent).toBe(true);
    const line = p.parseLine(
      '2026-10-10 13:55:36 10.0.0.1 GET /guide/ utm_source=chatgpt.com 443 - 203.0.113.5 Mozilla/5.0+(Macintosh)+Chrome/130 https://chatgpt.com/ 200 0 0 12',
    );
    expect(line).toEqual({
      time: Date.UTC(2026, 9, 10, 13, 55, 36),
      method: 'GET',
      path: '/guide/',
      query: 'utm_source=chatgpt.com',
      status: 200,
      bytes: null,
      referer: 'https://chatgpt.com/',
      userAgent: 'Mozilla/5.0 (Macintosh) Chrome/130',
    });
    const dash = p.parseLine(
      '2026-10-10 13:56:01 10.0.0.1 GET /guide/ - 443 - 203.0.113.6 - - 200 0 0 15',
    );
    expect(dash).toMatchObject({ query: '', referer: null, userAgent: null });
  });
  it('re-reads a second Fields directive mid-file and then reads sc-bytes', () => {
    const p = new IisParser();
    p.parseLine(HEADER);
    p.parseLine(
      '#Fields: date time cs-method cs-uri-stem cs-uri-query cs(User-Agent) cs(Referer) sc-status sc-bytes',
    );
    expect(p.hasBytes).toBe(true);
    const line = p.parseLine(
      '2026-10-10 14:00:00 GET /pricing/ - Mozilla/5.0+(compatible;+ClaudeBot/1.0) - 200 3000',
    );
    expect(line).toMatchObject({
      status: 200,
      bytes: 3000,
      userAgent: 'Mozilla/5.0 (compatible; ClaudeBot/1.0)',
    });
  });
  it('returns null for a data line before any Fields directive, a short line, or a non-numeric status', () => {
    const p = new IisParser();
    expect(p.parseLine('2026-10-10 14:00:00 GET /pricing/ - x - 200 3000')).toBeNull();
    p.parseLine(HEADER);
    expect(p.parseLine('2026-10-10 14:00:00 GET')).toBeNull();
    expect(
      p.parseLine('2026-10-10 13:55:36 10.0.0.1 GET /guide/ - 443 - 203.0.113.5 UA - abc 0 0 12'),
    ).toBeNull();
  });
  it('reports a Fields directive without a user agent column and tolerates duplicate or unknown columns', () => {
    const p = new IisParser();
    p.parseLine('#Fields: date date cs-method cs-uri-stem sc-status bogus');
    expect(p.hasUserAgent).toBe(false);
    const line = p.parseLine('2026-10-10 2026-10-11 GET /a 200 zzz');
    expect(line).toMatchObject({ path: '/a', status: 200, userAgent: null, time: null });
  });
  it('rejects an empty status token', () => {
    const p = new IisParser();
    p.parseLine(HEADER);
    expect(
      p.parseLine('2026-10-10 13:55:36 10.0.0.1 GET /guide/ - 443 - 203.0.113.5 UA -  0 0 12'),
    ).toBeNull();
  });
  it('maps a non-numeric sc-bytes to null', () => {
    const p = new IisParser();
    p.parseLine('#Fields: date time cs-method cs-uri-stem sc-status sc-bytes');
    expect(p.parseLine('2026-10-10 14:00:00 GET /a 200 abc')).toMatchObject({
      status: 200,
      bytes: null,
    });
  });
});
