// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { normalizeUrl } from '../src/scripts/scan/app';

describe('normalizeUrl', () => {
  it.each([
    ['example.com', 'https://example.com/'],
    ['  https://example.com/path?q=1  ', 'https://example.com/path?q=1'],
    ['http://sub.example.co.uk', 'http://sub.example.co.uk/'],
    ['example.com.', 'https://example.com./'],
    ['xn--bcher-kva.example', 'https://xn--bcher-kva.example/'],
    ['bücher.example', 'https://xn--bcher-kva.example/'],
    [`${'a'.repeat(63)}.example`, `https://${'a'.repeat(63)}.example/`],
    ['my-site.io:8443/a', 'https://my-site.io:8443/a'],
  ])('accepts %s', (raw, url) => {
    expect(normalizeUrl(raw)).toEqual({ ok: true, url });
  });

  it.each([
    'foo',
    'http://foo',
    'https://a_b.example.com',
    'https://exa mple.com',
    'https://-bad.example.com',
    'https://bad-.example.com',
    'https://example..com',
    'https://.example.com',
    'https://example.123',
    'https://1.2.3.4',
    'https://[::1]/',
    'https://1.2',
    `https://${'a'.repeat(64)}.com`,
    'localhost',
    'localhost:3000',
  ])('rejects the host in %s before any request', (raw) => {
    expect(normalizeUrl(raw).ok).toBe(false);
  });

  it('asks for a URL when the field is empty', () => {
    expect(normalizeUrl('   ')).toEqual({ ok: false, reason: 'Enter a URL to check.' });
  });

  it('gives IP addresses their own message', () => {
    for (const raw of ['https://1.2.3.4', 'https://[::1]/', 'https://1.2']) {
      expect(normalizeUrl(raw)).toEqual({
        ok: false,
        reason: 'Enter the site’s domain name, not an IP address.',
      });
    }
  });

  it('rejects a URL that carries a username or password', () => {
    expect(normalizeUrl('https://user:pw@example.com').ok).toBe(false);
    expect(normalizeUrl('https://user@example.com').ok).toBe(false);
  });

  it('rejects other schemes', () => {
    expect(normalizeUrl('ftp://example.com').ok).toBe(false);
  });
});
