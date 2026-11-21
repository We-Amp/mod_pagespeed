// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { RX_BUILDING, riskRows, siPlatformSplit } from './risk-rows';

const load = (name: string) =>
  JSON.parse(readFileSync(`tests/fixtures/scan/report-${name}.json`, 'utf8')).report;

const byKey = (rows: ReturnType<typeof riskRows>) =>
  Object.fromEntries(rows.map((r) => [r.key, r]));

describe('riskRows over the full report', () => {
  const rows = riskRows(load('full'));
  const m = byKey(rows);
  it('lists the four lenses in order', () => {
    expect(rows.map((r) => r.key)).toEqual([
      'preConsentLeak',
      'scriptInventory',
      'seoDefects',
      'responseExposure',
    ]);
    expect(rows.map((r) => r.name)).toEqual([
      'Pre-consent leak',
      'Script inventory',
      'SEO defects',
      'Response exposure',
    ]);
  });
  it('flags every lens and carries the gate-fired link with its event', () => {
    expect(rows.every((r) => r.state === 'attention' && r.chip === 'Attention')).toBe(true);
    expect(m.preConsentLeak.link).toMatchObject({
      href: '/platform/consent/',
      event: 'airead-cta-consent',
    });
    expect(m.scriptInventory.link).toMatchObject({
      href: '/platform/',
      event: 'airead-cta-page-integrity',
    });
    expect(m.seoDefects.link).toMatchObject({
      href: '/platform/edge-seo/',
      event: 'airead-cta-edge-seo',
    });
    expect(m.responseExposure.link).toMatchObject({
      href: '/platform/#pack-response-firewall',
      event: 'airead-cta-response-firewall',
    });
    expect(m.responseExposure.building).toEqual([RX_BUILDING]);
  });
  it('uses one verdict sentence per lens, without counts, host lists or server lines', () => {
    expect(m.preConsentLeak.sentence).toMatch(
      /^\d+ tracker hosts? contacted before any interaction, in a fresh browser with no consent given\./,
    );
    expect(m.preConsentLeak.sentence).toMatch(/not legal advice or a compliance certificate\.$/);
    expect(m.preConsentLeak.sentence).not.toMatch(/google|analytics|…|\(analytics\)/i);
    expect(m.scriptInventory.sentence).toMatch(/^Some scripts on this page need attention\./);
    expect(m.scriptInventory.sentence).toMatch(/not an assessment against any standard\.$/);
    expect(m.scriptInventory.sentence).not.toMatch(/Flagged:/);
    expect(m.scriptInventory.detail).toMatch(/^Flagged: .+\.$/);
    expect(m.scriptInventory.sentence).not.toMatch(
      /script\(s\)|CSP|from other hosts|with integrity/,
    );
    expect(m.seoDefects.sentence).toBe(
      '3 issues: canonical 1, title 1, structured data 1. One page only; not a ranking assessment.',
    );
    expect(m.seoDefects.sentence).not.toMatch(/Server header|CDN headers/);
    expect(m.responseExposure.sentence).toBe(
      '1 error-output pattern · 2 version tells · end of life: PHP 7.4 · missing: nosniff, framing. One page only; not a security assessment.',
    );
  });
  it('names no library, version or host in the script row', () => {
    expect(m.scriptInventory.sentence).not.toContain('jquery');
    expect(m.scriptInventory.sentence).not.toMatch(/\d+\.\d+/);
  });
  it('gives the script row both building sentences when both gates fire', () => {
    expect(m.scriptInventory.building).toHaveLength(2);
    expect(m.scriptInventory.building[0]).toBe(
      'Payment-page script integrity, a pack that would enforce an allowlist of approved scripts at your origin, is planned.',
    );
  });
});

describe('riskRows over the clean report', () => {
  const rows = riskRows(load('clean'));
  it('shows clean rows with no building sentence and no link', () => {
    expect(rows).toHaveLength(4);
    for (const r of rows) {
      expect(r.state).toBe('clean');
      expect(r.chip).toBe('Nothing flagged');
      expect(r.building).toEqual([]);
      expect(r.link).toBeNull();
    }
  });
  it('uses the approved clean sentences', () => {
    const m = byKey(rows);
    expect(m.preConsentLeak.sentence).toMatch(
      /^No tracker requests observed before any interaction, in a fresh browser/,
    );
    expect(m.preConsentLeak.sentence).not.toMatch(/third-party host/);
    expect(m.scriptInventory.sentence).toMatch(/^No script issues flagged on this page\./);
    expect(m.seoDefects.sentence).toMatch(/^No defects found: the canonical/);
  });
});

describe('riskRows over the blocked report', () => {
  const m = byKey(riskRows(load('blocked')));
  it('shows muted not-measured rows with the note, or the default', () => {
    for (const key of ['preConsentLeak', 'scriptInventory', 'seoDefects']) {
      expect(m[key].state).toBe('none');
      expect(m[key].chip).toBe('Not measured');
      expect(m[key].link).toBeNull();
      expect(m[key].building).toEqual([]);
    }
    // the error shape has a reason and no note: it gets the error default, not the blocked one
    expect(m.responseExposure.state).toBe('none');
    expect(m.responseExposure.sentence).toBe(
      'Not measured: the scanner could not complete this check.',
    );
  });
});

describe('riskRows edge cases', () => {
  const rx = (extra: object) => ({ responseExposure: { status: 'ok', ...extra } });
  it('omits disabled and missing lenses', () => {
    const rows = riskRows({
      preConsentLeak: { status: 'disabled', verdict: 'unknown' },
      scriptInventory: {
        status: 'ok',
        verdict: 'clean',
        totals: { thirdPartyExternal: 0, thirdPartyWithIntegrity: 0 },
      },
    });
    expect(rows.map((r) => r.key)).toEqual(['scriptInventory']);
    expect(riskRows(null)).toEqual([]);
  });
  it('uses the could-not-render sentence for an unknown verdict', () => {
    const m = byKey(
      riskRows({
        preConsentLeak: { status: 'ok', verdict: 'unknown' },
        scriptInventory: { status: 'ok', verdict: 'unknown' },
        seoDefects: { status: 'ok', verdict: 'unknown', staticCompared: true, notMeasured: [] },
        ...rx({ verdict: 'unknown' }),
      }),
    );
    expect(m.preConsentLeak.sentence).toBe(
      'We could not render this page, so we could not observe which third parties it contacts before consent.',
    );
    expect(m.scriptInventory.sentence).toBe(
      'We could not render this page, so we could not inventory its scripts.',
    );
    expect(m.seoDefects.sentence).toBe(
      'We could not render this page, so we could not check its head.',
    );
    expect(m.responseExposure.sentence).toBe('We could not read enough of this response to say.');
    for (const r of Object.values(m)) {
      expect(r.state).toBe('none');
      expect(r.link).toBeNull();
    }
  });
  it('shows the clean response row as its facts line, and what was not checked', () => {
    const clean = riskRows(
      rx({ verdict: 'clean', leaks: [], tells: [], endOfLife: [], headers: { missing: [] } }),
    )[0];
    expect(clean.sentence).toBe(
      '0 error-output patterns · 0 version tells · end of life: none seen · missing: none. One page only; not a security assessment.',
    );
    const unknown = riskRows(
      rx({ verdict: 'unknown', notMeasured: ['static-html', 'headers'] }),
    )[0];
    expect(unknown.sentence).toBe(
      'We could not read enough of this response to say. Not checked: the HTML as served without JavaScript, the response headers.',
    );
  });
  it('lists the mod_pagespeed phrase once', () => {
    const row = riskRows(
      rx({
        verdict: 'attention',
        reasons: ['end-of-life'],
        endOfLife: [
          { product: 'mod_pagespeed', line: 'a', basis: 'no-fixes' },
          { product: 'mod_pagespeed', line: 'b', basis: 'no-fixes' },
        ],
      }),
    )[0];
    expect(row.sentence.match(/no longer receives fixes/g)).toHaveLength(1);
  });
  it('shows the response row without a link for version tells and missing headers only', () => {
    const row = riskRows(
      rx({
        verdict: 'attention',
        reasons: ['version-tell', 'missing-headers'],
        leaks: [],
        tells: [{ product: 'apache', source: 'server', version: '2.4.41' }],
        endOfLife: [],
        headers: { missing: ['hsts'] },
      }),
    )[0];
    expect(row.state).toBe('attention');
    expect(row.link).toBeNull();
    expect(row.building).toEqual([]);
    expect(row.sentence).toContain('end of life: none seen');
  });
  it('fires the response link for end-of-life alone, and names a mod_pagespeed build', () => {
    const row = riskRows(
      rx({
        verdict: 'attention',
        reasons: ['end-of-life'],
        endOfLife: [{ product: 'mod_pagespeed', line: '1.13', basis: 'no-fixes' }],
      }),
    )[0];
    expect(row.link?.event).toBe('airead-cta-response-firewall');
    expect(row.sentence).toContain(
      'mod_pagespeed 1.13: this line no longer receives fixes. Move to the current release; configuration carries over.',
    );
    expect(row.sentence).not.toContain('(no longer receives fixes)');
  });
  it('adds a lifecycle sentence per detected mod_pagespeed band, none for 2.1', () => {
    const sentence = (tell: string) =>
      riskRows(
        rx({
          verdict: 'clean',
          leaks: [],
          tells: [{ product: 'mod_pagespeed', source: 'x-page-speed', version: tell }],
          endOfLife: [],
          headers: { missing: [] },
        }),
      )[0].sentence;
    expect(sentence('1.16.0.2')).toContain(
      ' mod_pagespeed 1.16.0.2: a newer release is available. Update recommended. One page only',
    );
    expect(sentence('2.0.3')).toContain(
      ' ModPageSpeed 2.0.3: Migrate from ModPageSpeed 2.0 explains how your setup moves to the current release. One page only',
    );
    expect(sentence('2.1.0')).toBe(
      '0 error-output patterns · 1 version tell · end of life: none seen · missing: none. One page only; not a security assessment.',
    );
    expect(sentence('unknown')).not.toContain('mod_pagespeed ');
  });
  it('counts a single-number version tell such as PHP/8', () => {
    const row = riskRows(
      rx({
        verdict: 'clean',
        leaks: [],
        tells: [{ product: 'php', source: 'x-powered-by', version: '8' }],
        endOfLife: [],
        headers: { missing: [] },
      }),
    )[0];
    expect(row.sentence).toContain('1 version tell');
  });
  it('treats the generic header value "on" as active with no version disclosed', () => {
    for (const version of ['on', null]) {
      const row = riskRows(
        rx({
          verdict: 'clean',
          leaks: [],
          tells: [{ product: 'mod_pagespeed', source: 'x-page-speed', version }],
          endOfLife: [],
          headers: { missing: [] },
        }),
      )[0];
      expect(row.sentence).toBe(
        '0 error-output patterns · 0 version tells · end of life: none seen · missing: none. One page only; not a security assessment.',
      );
    }
  });
  it('does not fire the response link for a hostile blocked fixture', () => {
    const row = riskRows({
      responseExposure: {
        status: 'blocked',
        verdict: 'attention',
        reasons: ['debug-output'],
        note: 'Blocked by a challenge page.',
      },
    })[0];
    expect(row.state).toBe('none');
    expect(row.link).toBeNull();
    expect(row.sentence).toBe('Blocked by a challenge page.');
  });
  it('does not render a script link for a clean inventory with only pinned scripts', () => {
    const row = riskRows({
      scriptInventory: {
        status: 'ok',
        verdict: 'clean',
        totals: { scripts: 2, thirdPartyExternal: 1, thirdPartyWithIntegrity: 1 },
      },
    })[0];
    expect(row.link).toBeNull();
  });
});

describe('riskRows counts are pluralised from the number', () => {
  it('uses the singular for exactly one', () => {
    const report = load('full');
    report.preConsentLeak.trackerCount = 1;
    report.seoDefects.counts = { ...report.seoDefects.counts, total: 1 };
    const m = byKey(riskRows(report));
    expect(m.preConsentLeak.sentence).toMatch(/^1 tracker host contacted before/);
    expect(m.seoDefects.sentence).toMatch(/^1 issue: /);
    expect(JSON.stringify(riskRows(report))).not.toContain('(s)');
  });
});

describe('riskRows unpinned script wording', () => {
  it('uses singular agreement after exactly one unpinned script', () => {
    const report = load('full');
    report.scriptInventory.totals.thirdPartyWithIntegrity =
      report.scriptInventory.totals.thirdPartyExternal - 1;
    const text = byKey(riskRows(report)).scriptInventory.building.join('\n');
    expect(text).toContain(
      '1 third-party script on this page loads without an integrity hash, so its content can change',
    );
    expect(text).toContain('it would serve that script at a version you approved');
    expect(text).not.toContain('it would serve those scripts');
  });
});

describe('riskRows on a hosted platform', () => {
  const rows = riskRows(load('hosted'));
  const m = byKey(rows);
  it('keeps every finding and offers nothing that needs a server', () => {
    expect(rows.map((r) => r.state)).toEqual(riskRows(load('full')).map((r) => r.state));
    for (const row of rows) {
      expect(row.building).toEqual([]);
      expect(row.link).toBeNull();
      expect(row.sentence).not.toBe('');
    }
  });
  it('shows the platform scripts apart from third-party scripts', () => {
    expect(m.scriptInventory.sentence).toBe(
      'Some scripts on this page need attention. 29 external scripts: 27 from Wix’s own hosts, 2 from third parties. Heuristic host list; not an assessment against any standard.',
    );
  });
});

describe('riskRows without a hosted platform', () => {
  it('keeps the offers and the script sentence for an unknown platform and for none', () => {
    const full = riskRows(load('full'));
    const unknown = riskRows(load('unknown'));
    expect(unknown).toEqual(full);
    expect(byKey(full).scriptInventory.sentence).toBe(
      'Some scripts on this page need attention. Heuristic host list; not an assessment against any standard.',
    );
  });
});

describe('siPlatformSplit', () => {
  it('is empty without platform scripts', () => {
    expect(siPlatformSplit({}, 'Wix')).toBe('');
    expect(siPlatformSplit({ totals: { external: 4, thirdPartyExternal: 4 } }, 'Wix')).toBe('');
    expect(siPlatformSplit({ totals: { external: 4, platformExternal: 0 } }, 'Wix')).toBe('');
  });
  it('falls back to "the platform" and counts the site\'s own scripts when there are any', () => {
    expect(
      siPlatformSplit(
        {
          totals: {
            external: 6,
            platformExternal: 1,
            thirdPartyExternal: 3,
            firstPartyExternal: 2,
          },
        },
        '',
      ),
    ).toBe(
      '6 external scripts: 1 from the platform’s own hosts, 3 from third parties, 2 from this site’s own host.',
    );
    // The head is the sum of the listed parts, whatever the scan's own total says.
    expect(
      siPlatformSplit(
        { totals: { external: 9, platformExternal: 5, thirdPartyExternal: 1 } },
        'Shop',
      ),
    ).toBe('6 external scripts: 5 from Shop’s own hosts, 1 from third parties.');
    expect(siPlatformSplit({ totals: { platformExternal: 1 } }, 'Shop')).toBe(
      '1 external script: 1 from Shop’s own hosts, 0 from third parties.',
    );
  });
});
