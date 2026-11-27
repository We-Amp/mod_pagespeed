// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { brandedTitle, TITLE_MAX } from './page-title';

describe('brandedTitle', () => {
  it('adds the product suffix to a short title', () => {
    expect(brandedTitle('Cache modes')).toBe('Cache modes — mod_pagespeed');
  });

  it('leaves the suffix off a title that already names the product', () => {
    expect(brandedTitle('Migrate from mod_pagespeed 1.15')).toBe('Migrate from mod_pagespeed 1.15');
    expect(brandedTitle('mod_pagespeed admin console')).toBe('mod_pagespeed admin console');
  });

  it('leaves the suffix off when it would pass the length limit', () => {
    const long = 'x'.repeat(TITLE_MAX - ' — mod_pagespeed'.length + 1);
    expect(brandedTitle(long)).toBe(long);
    const fits = 'x'.repeat(TITLE_MAX - ' — mod_pagespeed'.length);
    expect(brandedTitle(fits)).toBe(`${fits} — mod_pagespeed`);
  });
});
