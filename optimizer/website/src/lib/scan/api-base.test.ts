// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { DEFAULT_API_BASE, resolveApiBase } from './api-base';

describe('resolveApiBase', () => {
  it('uses the site API path when no override is given', () => {
    expect(resolveApiBase('', 'modpagespeed.com')).toBe(DEFAULT_API_BASE);
  });

  it('ignores an api value on the public site', () => {
    expect(resolveApiBase('?api=https://other.example', 'modpagespeed.com')).toBe(DEFAULT_API_BASE);
    expect(resolveApiBase('?api=//other.example', 'www.modpagespeed.com')).toBe(DEFAULT_API_BASE);
  });

  it('keeps the local-development override on localhost', () => {
    expect(resolveApiBase('?api=http://localhost:8787', 'localhost')).toBe('http://localhost:8787');
    expect(resolveApiBase('?api=http://localhost:8787', '127.0.0.1')).toBe('http://localhost:8787');
  });

  it('falls back to the default on localhost without a value', () => {
    expect(resolveApiBase('?api=', 'localhost')).toBe(DEFAULT_API_BASE);
  });
});
