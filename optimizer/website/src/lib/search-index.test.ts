// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import { searchTitle, searchWeight } from './search-index';

describe('search index hints', () => {
  it('weights the named pages above the references, the docs and the rest', () => {
    const named = Number(searchWeight('/pricing/'));
    const reference = Number(searchWeight('/docs/configuration/'));
    const docs = Number(searchWeight('/docs/getting-started/'));
    expect(named).toBeGreaterThan(reference);
    expect(reference).toBeGreaterThan(docs);
    expect(docs).toBeGreaterThan(1);
    expect(searchWeight('/blog/some-post/')).toBeUndefined();
    expect(searchWeight('/')).toBeUndefined();
  });

  it('keeps every weight inside the range Pagefind accepts', () => {
    for (const path of ['/pricing/', '/docs/configuration/', '/docs/upgrade/']) {
      const weight = Number(searchWeight(path));
      expect(weight).toBeGreaterThan(0);
      expect(weight).toBeLessThanOrEqual(10);
    }
  });

  it('gives the named pages a short result title, with or without a trailing slash', () => {
    expect(searchTitle('/pricing/')).toBe('Pricing');
    expect(searchTitle('/pricing')).toBe('Pricing');
    expect(searchTitle('/wordpress/')).toBe('WordPress');
    expect(searchWeight('/download')).toBe(searchWeight('/download/'));
  });

  it('leaves every other page titled by its own headline', () => {
    expect(searchTitle('/')).toBeUndefined();
    expect(searchTitle('/docs/configuration/')).toBeUndefined();
    expect(searchTitle('/pricing/extra/')).toBeUndefined();
  });

  it('a short title is plain text: nothing that could end the attribute value early', () => {
    for (const path of [
      '/pricing/',
      '/download/',
      '/contact/',
      '/support/',
      '/features/',
      '/demo/',
      '/calculator/',
      '/about/',
      '/alternatives/',
      '/wordpress/',
    ]) {
      expect(searchTitle(path)).toMatch(/^[A-Za-z]+$/);
    }
  });
});
