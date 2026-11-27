// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for the URL the /examples/ gallery generator requests when it
// measures a subresource (tools/examples-gallery/resource-fetch-url.mjs).
// An unchanged resource URL must measure the same bytes before and after, so
// that only the filter under test makes a difference.

import { describe, expect, it } from 'vitest';
import { resourceFetchUrl } from '../../tools/examples-gallery/resource-fetch-url.mjs';

const ORIGIN = 'https://demo.test';
const BASE = `${ORIGIN}/mod_pagespeed_example`;

describe('examples gallery: resource fetch URL', () => {
  it('turns optimization off for an unchanged resource on the demo host', () => {
    expect(resourceFetchUrl(`${BASE}/images/a.png`, ORIGIN)).toBe(
      `${BASE}/images/a.png?PageSpeed=off`,
    );
  });

  it('gives the same URL for the same resource on both sides', () => {
    const ref = `${BASE}/styles/a.css`;
    expect(resourceFetchUrl(ref, ORIGIN)).toBe(resourceFetchUrl(ref, `${ORIGIN}/`));
  });

  it('keeps an existing query and replaces an existing PageSpeed value', () => {
    expect(resourceFetchUrl(`${BASE}/a.js?v=2`, ORIGIN)).toBe(`${BASE}/a.js?v=2&PageSpeed=off`);
    expect(resourceFetchUrl(`${BASE}/a.js?PageSpeed=on`, ORIGIN)).toBe(
      `${BASE}/a.js?PageSpeed=off`,
    );
  });

  it('drops a fragment', () => {
    expect(resourceFetchUrl(`${BASE}/sprite.svg#icon`, ORIGIN)).toBe(
      `${BASE}/sprite.svg?PageSpeed=off`,
    );
  });

  it('measures a rewritten resource as served', () => {
    const single = `${BASE}/images/xa.png.pagespeed.ic.AbCdEf.webp`;
    const combined = `${BASE}/a.css+b.css.pagespeed.cc.AbCdEf.css`;
    expect(resourceFetchUrl(single, ORIGIN)).toBe(single);
    expect(resourceFetchUrl(combined, ORIGIN)).toBe(combined);
  });

  it('leaves resources on other hosts alone', () => {
    const font = 'https://fonts.example/css?family=Roboto';
    expect(resourceFetchUrl(font, ORIGIN)).toBe(font);
    const otherScheme = 'http://demo.test/mod_pagespeed_example/a.png';
    expect(resourceFetchUrl(otherScheme, ORIGIN)).toBe(otherScheme);
  });

  it('does not treat "pagespeed" elsewhere in the URL as a rewrite', () => {
    expect(resourceFetchUrl(`${ORIGIN}/mod_pagespeed_example/pagespeed.png`, ORIGIN)).toBe(
      `${ORIGIN}/mod_pagespeed_example/pagespeed.png?PageSpeed=off`,
    );
  });
});
