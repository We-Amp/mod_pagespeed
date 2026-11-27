// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { PLATFORM_GUIDANCE } from '../../data/platform-guidance';
import { FIT_LINE, fitFor, platformName, serverOffersApply } from './fit';

const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url),
      'utf8',
    ),
  ).report;

const HEAD = (on: string) =>
  `This site runs on ${on}. mod_pagespeed installs on a web server you run yourself, so it cannot be used for this site. The findings below still apply.`;
const SPOKEN = (on: string) =>
  `This site runs on ${on}; mod_pagespeed cannot be used here. The findings still apply.`;
const NONE = { linksLead: '', links: [], spoken: '' };

describe('serverOffersApply', () => {
  it('is false only when the scan says the owner has no control of the server', () => {
    expect(serverOffersApply(fixture('hosted'))).toBe(false);
    expect(serverOffersApply({ platform: { originControl: 'none', confidence: 'low' } })).toBe(
      false,
    );
  });

  it('is true for every other reading and for no reading at all', () => {
    expect(serverOffersApply(fixture('unknown'))).toBe(true);
    expect(serverOffersApply(fixture('full'))).toBe(true);
    for (const platform of [
      undefined,
      null,
      'wix',
      { status: 'disabled' },
      { status: 'error' },
      { originControl: 'limited' },
      { originControl: 'unknown' },
    ]) {
      expect(serverOffersApply({ platform })).toBe(true);
    }
    expect(serverOffersApply(null)).toBe(true);
    expect(serverOffersApply(undefined)).toBe(true);
  });
});

describe('fitFor', () => {
  it('gives the muted line when the server offers may apply', () => {
    for (const report of [fixture('unknown'), fixture('full'), { platform: null }]) {
      expect(fitFor(report)).toEqual({ offers: true, text: FIT_LINE, ...NONE });
    }
    expect(FIT_LINE).toBe(
      'The fixes and offers below that use mod_pagespeed apply only if you run the web server yourself.',
    );
  });

  it('says nothing without a result', () => {
    for (const report of [null, undefined]) {
      expect(fitFor(report)).toEqual({ offers: true, text: '', ...NONE });
    }
  });

  it('keeps the notice to the fit sentences and links the platform’s help pages', () => {
    const fit = fitFor(fixture('hosted'));
    expect(fit.offers).toBe(false);
    expect(fit.text).toBe(HEAD('Wix, a hosted platform'));
    expect(fit.linksLead).toBe('Wix help pages:');
    expect(fit.links).toEqual(
      PLATFORM_GUIDANCE.wix.actions.map((a) => ({ label: a.sourceLabel, href: a.source })),
    );
    expect(fit.links.every((l) => l.href.startsWith('https://'))).toBe(true);
    // The live region gets one short sentence, not the whole note.
    expect(fit.spoken).toBe(SPOKEN('Wix, a hosted platform'));
  });

  it('lists no links for a platform without guidance', () => {
    const fit = fitFor({
      platform: { id: 'some-builder', name: 'Some Builder', originControl: 'none' },
    });
    expect(fit).toEqual({
      offers: false,
      text: HEAD('Some Builder, a hosted platform'),
      linksLead: '',
      links: [],
      spoken: SPOKEN('Some Builder, a hosted platform'),
    });
  });

  it('leaves the name out when the scan gave none', () => {
    for (const name of [undefined, null, '', '  ', 7]) {
      const fit = fitFor({ platform: { id: null, name, originControl: 'none' } });
      expect(fit.text).toBe(HEAD('a hosted platform'));
      expect(fit.spoken).toBe(SPOKEN('a hosted platform'));
    }
  });
});

describe('platformName', () => {
  it('is the trimmed name or an empty string', () => {
    expect(platformName(fixture('hosted'))).toBe('Wix');
    expect(platformName(fixture('unknown'))).toBe('');
    expect(platformName({ platform: { name: ' Shop ' } })).toBe('Shop');
    expect(platformName(null)).toBe('');
  });
  it('cuts a name longer than 60 characters', () => {
    const long = platformName({ platform: { name: 'N'.repeat(200) } });
    expect(long).toBe('N'.repeat(60));
    expect(fitFor({ platform: { name: 'N'.repeat(200), originControl: 'none' } }).text).toContain(
      `This site runs on ${'N'.repeat(60)}, a hosted platform.`,
    );
  });
});
