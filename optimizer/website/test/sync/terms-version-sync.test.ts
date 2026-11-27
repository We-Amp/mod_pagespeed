// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// terms-version-sync.test.ts — drift guard for the terms-of-service version
// string, which is stamped in TWO places that must never disagree:
//
//   1. website/src/pages/terms.astro          — "Version YYYY-MM[-DD]" (source of truth,
//                                                the version shown to and accepted by users)
//   2. website/src/pages/pricing.astro         — TERMS_VERSION_WEB (the terms
//                                                version the pricing page names)
//
// The checkout placeholder at /buy/ carried the second stamp until it retired
// (it now redirects to /pricing/). The pricing page states the terms version a
// subscription is made under, so the stamp moved there. This file-reading guard is
// the enforcement: if either stamp drifts, a subscription would name the wrong
// version.
//
// The admin console used to carry a third stamp (TERMS_VERSION in the
// api-client, for the daemon's /v1/license/consent endpoint). Both were removed
// at 2.1 GA (D3: the daemon has no license or consent surface), so this
// guard no longer reads the console tree.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, it, expect } from 'vitest';

function read(rel: string): string {
  return readFileSync(fileURLToPath(new URL(rel, import.meta.url)), 'utf8');
}

function extract(rel: string, re: RegExp, label: string): string {
  const m = read(rel).match(re);
  if (!m) {
    throw new Error(
      `terms-version-sync: could not find ${label} in ${rel}. ` +
        `If the file moved or the stamp format changed, update this guard.`,
    );
  }
  return m[1];
}

describe('terms-of-service version is in lockstep across all stamp sites', () => {
  const termsVersion = extract(
    '../../src/pages/terms.astro',
    /Version\s+(\d{4}-\d{2}(?:-\d{2})?)\b/,
    'the "Version YYYY-MM[-DD]" line',
  );
  const pricingVersion = extract(
    '../../src/pages/pricing.astro',
    /TERMS_VERSION_WEB\s*=\s*'([^']+)'/,
    'TERMS_VERSION_WEB',
  );

  it('terms.astro version matches the /pricing/ stamp', () => {
    expect(pricingVersion).toBe(termsVersion);
  });
});
