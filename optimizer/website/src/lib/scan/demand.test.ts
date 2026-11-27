// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import {
  agentPassCtaApplies,
  complianceFixCtaApplies,
  consentEnforcementCtaApplies,
  edgeSeoCtaApplies,
  lensSignalFor,
  markdownOriginCtaApplies,
  originTopicsApply,
  pageIntegrityCtaApplies,
  responseFirewallCtaApplies,
  thirdPartyFreezeCtaApplies,
  tollboothCtaApplies,
} from './demand.mjs';

describe('tollboothCtaApplies', () => {
  const detail = {
    verifiable: false,
    renderOk: true,
    aiCrawlersAllowed: ['chatgpt-user'],
    exposurePct: 30,
  };

  it('fires when the page renders, agents are allowed and exposure reaches 30', () => {
    expect(tollboothCtaApplies({ detail })).toBe(true);
  });

  it('does not fire without detail or without input', () => {
    expect(tollboothCtaApplies(undefined)).toBe(false);
    expect(tollboothCtaApplies(null)).toBe(false);
    expect(tollboothCtaApplies({})).toBe(false);
    expect(tollboothCtaApplies({ detail: null })).toBe(false);
  });

  const breaks: Array<[string, Record<string, unknown>]> = [
    ['the page renders but is verifiable', { verifiable: true }],
    ['verifiable is falsy but not false', { verifiable: 0 }],
    ['the page does not render', { renderOk: false }],
    ['renderOk is truthy but not true', { renderOk: 1 }],
    ['no AI crawler is allowed', { aiCrawlersAllowed: [] }],
    ['the allowed-crawler list is not a list', { aiCrawlersAllowed: 'chatgpt-user' }],
    ['the allowed-crawler list is missing', { aiCrawlersAllowed: undefined }],
    ['exposure is below 30', { exposurePct: 29.9 }],
  ];

  it.each(breaks)('does not fire when %s', (_why, patch) => {
    expect(tollboothCtaApplies({ detail: { ...detail, ...patch } })).toBe(false);
  });
});

describe('agentPassCtaApplies', () => {
  it('fires for a lens that ran and is verifying or signature-aware', () => {
    expect(agentPassCtaApplies({ status: 'ok', classification: 'verifying' })).toBe(true);
    expect(agentPassCtaApplies({ status: 'ok', classification: 'signature-aware' })).toBe(true);
  });

  it('does not fire for another classification, a lens that did not run, or missing input', () => {
    expect(agentPassCtaApplies({ status: 'ok', classification: 'ignoring' })).toBe(false);
    expect(agentPassCtaApplies({ status: 'error', classification: 'verifying' })).toBe(false);
    expect(agentPassCtaApplies({ classification: 'verifying' })).toBe(false);
    expect(agentPassCtaApplies(undefined)).toBe(false);
  });
});

describe('complianceFixCtaApplies', () => {
  it('fires when the lens is available with at least one fixable inline issue', () => {
    expect(complianceFixCtaApplies({ available: true, detail: { fixableInline: 1 } })).toBe(true);
  });

  it('does not fire when the lens is unavailable, has no detail or nothing fixable', () => {
    expect(complianceFixCtaApplies({ available: false, detail: { fixableInline: 3 } })).toBe(false);
    expect(complianceFixCtaApplies({ available: true })).toBe(false);
    expect(complianceFixCtaApplies({ available: true, detail: { fixableInline: 0 } })).toBe(false);
    expect(complianceFixCtaApplies({ available: true, detail: {} })).toBe(false);
    expect(complianceFixCtaApplies(undefined)).toBe(false);
  });
});

describe('consentEnforcementCtaApplies', () => {
  it('fires for a lens that ran, found leaks and saw at least one tracker', () => {
    expect(consentEnforcementCtaApplies({ status: 'ok', verdict: 'leaks', trackerCount: 1 })).toBe(
      true,
    );
  });

  it('does not fire without leaks, without trackers, or on a lens that did not run', () => {
    expect(consentEnforcementCtaApplies({ status: 'ok', verdict: 'clean', trackerCount: 2 })).toBe(
      false,
    );
    expect(consentEnforcementCtaApplies({ status: 'ok', verdict: 'leaks', trackerCount: 0 })).toBe(
      false,
    );
    expect(consentEnforcementCtaApplies({ status: 'ok', verdict: 'leaks' })).toBe(false);
    expect(
      consentEnforcementCtaApplies({ status: 'blocked', verdict: 'leaks', trackerCount: 2 }),
    ).toBe(false);
    expect(consentEnforcementCtaApplies(undefined)).toBe(false);
  });
});

describe('pageIntegrityCtaApplies', () => {
  it('fires for a script inventory that ran and asks for attention', () => {
    expect(pageIntegrityCtaApplies({ status: 'ok', verdict: 'attention' })).toBe(true);
  });

  it('does not fire for a clean or unresolved inventory, or missing input', () => {
    expect(pageIntegrityCtaApplies({ status: 'ok', verdict: 'clean' })).toBe(false);
    expect(pageIntegrityCtaApplies({ status: 'ok', verdict: 'unknown' })).toBe(false);
    expect(pageIntegrityCtaApplies({ status: 'blocked', verdict: 'attention' })).toBe(false);
    expect(pageIntegrityCtaApplies(undefined)).toBe(false);
  });
});

describe('thirdPartyFreezeCtaApplies', () => {
  const totals = { thirdPartyExternal: 2, thirdPartyWithIntegrity: 1 };

  it('fires when more third-party scripts are external than pinned, even on a clean verdict', () => {
    expect(thirdPartyFreezeCtaApplies({ status: 'ok', verdict: 'clean', totals })).toBe(true);
  });

  it('does not fire when the counts tie or invert, the totals are missing, or the lens did not settle', () => {
    const inverted = { totals: { thirdPartyExternal: 1, thirdPartyWithIntegrity: 2 } };
    expect(thirdPartyFreezeCtaApplies({ status: 'ok', verdict: 'attention', ...inverted })).toBe(
      false,
    );
    const tied = { totals: { thirdPartyExternal: 2, thirdPartyWithIntegrity: 2 } };
    expect(thirdPartyFreezeCtaApplies({ status: 'ok', verdict: 'attention', ...tied })).toBe(false);
    expect(thirdPartyFreezeCtaApplies({ status: 'ok', verdict: 'attention' })).toBe(false);
    expect(thirdPartyFreezeCtaApplies({ status: 'ok', verdict: 'unknown', totals })).toBe(false);
    const errored = { status: 'error', verdict: 'attention', totals };
    expect(thirdPartyFreezeCtaApplies(errored)).toBe(false);
    expect(thirdPartyFreezeCtaApplies(undefined)).toBe(false);
  });
});

describe('edgeSeoCtaApplies', () => {
  it('fires for a lens that ran, asks for attention and lists defects', () => {
    expect(
      edgeSeoCtaApplies({ status: 'ok', verdict: 'attention', defects: ['canonical-js-only'] }),
    ).toBe(true);
  });

  it('does not fire without defects, with a non-list, or on a clean lens', () => {
    expect(edgeSeoCtaApplies({ status: 'ok', verdict: 'attention', defects: [] })).toBe(false);
    expect(edgeSeoCtaApplies({ status: 'ok', verdict: 'attention' })).toBe(false);
    const notAList = { status: 'ok', verdict: 'attention', defects: 'canonical-js-only' };
    expect(edgeSeoCtaApplies(notAList)).toBe(false);
    const clean = { status: 'ok', verdict: 'clean', defects: ['canonical-js-only'] };
    expect(edgeSeoCtaApplies(clean)).toBe(false);
    expect(edgeSeoCtaApplies(undefined)).toBe(false);
  });
});

describe('responseFirewallCtaApplies', () => {
  it('fires for a lens that ran, asks for attention and names debug output or end of life', () => {
    expect(
      responseFirewallCtaApplies({ status: 'ok', verdict: 'attention', reasons: ['debug-output'] }),
    ).toBe(true);
    expect(
      responseFirewallCtaApplies({ status: 'ok', verdict: 'attention', reasons: ['end-of-life'] }),
    ).toBe(true);
  });

  it('does not fire for other reasons, an empty or missing list, or a lens that did not run', () => {
    expect(
      responseFirewallCtaApplies({
        status: 'ok',
        verdict: 'attention',
        reasons: ['version-tell', 'missing-headers'],
      }),
    ).toBe(false);
    expect(responseFirewallCtaApplies({ status: 'ok', verdict: 'attention', reasons: [] })).toBe(
      false,
    );
    expect(responseFirewallCtaApplies({ status: 'ok', verdict: 'attention' })).toBe(false);
    expect(
      responseFirewallCtaApplies({ status: 'ok', verdict: 'clean', reasons: ['debug-output'] }),
    ).toBe(false);
    expect(responseFirewallCtaApplies(undefined)).toBe(false);
  });
});

// The signal one topic carries: ids, counts and booleans from the lens behind
// the wedge. The shapes follow the scanner's contract.
describe('lensSignalFor', () => {
  it('tollbooth: the six verifiability fields', () => {
    const detail = {
      verifiable: false,
      exposurePct: 87,
      aiCrawlersAllowed: ['GPTBot', 'CCBot'],
      aiCrawlersBlocked: ['ClaudeBot'],
      botAuthHeaders: false,
      renderOk: true,
    };
    const report = { url: 'https://example.com/', agentVerifiability: { detail } };
    expect(lensSignalFor('tollbooth', report)).toEqual(detail);
  });

  it('agentpass: classification, evidence and probes, without the status', () => {
    const probes = {
      signed: { status: 403, headers: {}, bodyLength: 120 },
      broken: { status: 403, headers: {}, bodyLength: 120 },
      unsigned: { status: 200, headers: {}, bodyLength: 5120 },
    };
    const evidence = ['broken-signature vs unsigned: status 403 vs 200'];
    const report = {
      url: 'https://origin.example/',
      signedAgentVerification: {
        status: 'ok',
        classification: 'signature-aware',
        probes,
        evidence,
      },
    };
    expect(lensSignalFor('agentpass', report)).toEqual({
      classification: 'signature-aware',
      evidence,
      probes,
    });
  });

  it('compliancefix: the totals and the top rules', () => {
    const detail = {
      total: 12,
      fixableInline: 7,
      byImpact: { critical: 2, serious: 5 },
      topRules: ['image-alt', 'color-contrast'],
    };
    const report = { url: 'https://shop.example.org/checkout', accessibility: { detail } };
    expect(lensSignalFor('compliancefix', report)).toEqual(detail);
  });

  it('consent-enforcement: the top three hosts, without the tracker mark', () => {
    const report = {
      url: 'https://shop.example.com/',
      preConsentLeak: {
        status: 'ok',
        verdict: 'leaks',
        trackerCount: 4,
        thirdPartyCount: 7,
        thirdPartyHosts: [
          { host: 'www.google-analytics.com', requests: 3, category: 'analytics', tracker: true },
          { host: 'www.googletagmanager.com', requests: 1, category: 'tag-manager', tracker: true },
          { host: 'connect.facebook.net', requests: 1, category: 'social', tracker: true },
          { host: 'static.hotjar.com', requests: 1, category: 'session-replay', tracker: true },
          { host: 'consent.cookiebot.com', requests: 1, category: 'cmp', tracker: false },
        ],
        cmpDetected: { detected: true, vendor: 'Cookiebot' },
        firstParty: ['shop.example.com'],
        requestsObserved: 22,
      },
    };
    expect(lensSignalFor('consent-enforcement', report)).toEqual({
      verdict: 'leaks',
      trackerCount: 4,
      thirdPartyCount: 7,
      topHosts: [
        { host: 'www.google-analytics.com', requests: 3, category: 'analytics' },
        { host: 'www.googletagmanager.com', requests: 1, category: 'tag-manager' },
        { host: 'connect.facebook.net', requests: 1, category: 'social' },
      ],
      cmpDetected: { detected: true, vendor: 'Cookiebot' },
    });
    // A lens-free wedge has no signal, whatever the report holds.
    expect(lensSignalFor('pricing', report)).toBeNull();
  });

  it('consent-enforcement: an empty host list when the report has none', () => {
    const report = {
      url: 'https://shop.example.com/',
      preConsentLeak: {
        status: 'ok',
        verdict: 'leaks',
        trackerCount: 2,
        thirdPartyCount: 2,
        cmpDetected: { detected: false, vendor: null },
      },
    };
    expect(lensSignalFor('consent-enforcement', report)).toEqual({
      verdict: 'leaks',
      trackerCount: 2,
      thirdPartyCount: 2,
      topHosts: [],
      cmpDetected: { detected: false, vendor: null },
    });
  });

  // A 30-host, 5-duplicate inventory with script paths present: the signal
  // keeps the first three third-party hosts and three duplicates, and never a
  // path.
  it.each(['page-integrity', 'third-party-freeze'])(
    '%s: the shared script-inventory signal, hosts only',
    (wedge) => {
      const scriptHosts = [
        {
          host: 'shop.example.com',
          party: 'first',
          scripts: 9,
          withIntegrity: 0,
          category: 'first-party',
          listed: true,
        },
        ...Array.from({ length: 29 }, (_, i) => ({
          host: `cdn${i}.example.net`,
          party: 'third',
          scripts: 30 - i,
          withIntegrity: 0,
          category: i === 0 ? 'cdn' : 'other',
          listed: i === 0,
        })),
      ];
      const csp = { enforced: false, reportOnly: true, scriptSrc: ["'self'"], reporting: false };
      const report = {
        url: 'https://shop.example.com/checkout',
        scriptInventory: {
          status: 'ok',
          verdict: 'attention',
          reasons: ['sri-missing', 'duplicate-library'],
          blockedBy: [],
          totals: {
            scripts: 40,
            inline: 4,
            external: 36,
            firstPartyExternal: 9,
            thirdPartyExternal: 27,
            thirdPartyHosts: 29,
            thirdPartyWithIntegrity: 1,
            nonExecutable: 0,
          },
          scriptHosts,
          sriMissing: [{ host: 'cdn0.example.net', path: '/secret/path/lib.js' }],
          floating: [],
          duplicates: Array.from({ length: 5 }, (_, i) => ({
            library: `lib${i}`,
            versions: ['1.0.0', '2.0.0'],
          })),
          csp,
          note: 'x',
        },
      };
      const signal = lensSignalFor(wedge, report);
      expect(signal).toEqual({
        verdict: 'attention',
        reasons: ['sri-missing', 'duplicate-library'],
        totals: {
          scripts: 40,
          inline: 4,
          thirdPartyExternal: 27,
          thirdPartyHosts: 29,
          thirdPartyWithIntegrity: 1,
        },
        topHosts: [
          { host: 'cdn0.example.net', scripts: 30, category: 'cdn' },
          { host: 'cdn1.example.net', scripts: 29, category: 'other' },
          { host: 'cdn2.example.net', scripts: 28, category: 'other' },
        ],
        duplicates: [0, 1, 2].map((i) => ({ library: `lib${i}`, versions: ['1.0.0', '2.0.0'] })),
        csp,
      });
      expect(JSON.stringify(signal)).not.toContain('/secret/path');
      expect(JSON.stringify(signal).length).toBeLessThan(2000);
    },
  );

  it('edge-seo: ids, counts, booleans and two tokens, never page text or URLs', () => {
    const counts = { total: 20, canonical: 5, hreflang: 5, title: 4, description: 3, jsonld: 3 };
    const defects = Array.from({ length: 20 }, (_, i) => `defect-${i}`);
    const notMeasured = ['canonical-js', 'hreflang-js', 'title-js', 'description-js', 'jsonld-js'];
    const report = {
      url: 'https://news.example.com/story',
      seoDefects: {
        status: 'ok',
        verdict: 'attention',
        defects,
        counts,
        blockedBy: [],
        staticCompared: false,
        notMeasured,
        canonical: {
          static: { count: 0, href: 'https://news.example.com/secret-canonical' },
          rendered: { count: 1, href: 'https://news.example.com/secret-canonical' },
          header: null,
        },
        title: {
          static: { count: 1, text: 'Secret Title Text' },
          rendered: { count: 1, text: 'Secret Title Text' },
        },
        description: {
          static: { count: 1, text: 'Secret description text' },
          rendered: { count: 1, text: 'Secret description text' },
        },
        jsonLd: {
          static: { blocks: 0, invalid: 0, types: [] },
          rendered: { blocks: 2, invalid: 0, types: ['SecretType'] },
          renderOnlyTypes: ['SecretType'],
        },
        robots: { mismatch: true },
        delivery: { cdn: 'cloudflare', server: 'nginx' },
        note: 'x',
      },
    };
    const signal = lensSignalFor('edge-seo', report);
    expect(signal).toEqual({
      verdict: 'attention',
      defects,
      counts,
      staticCompared: false,
      notMeasured,
      robotsMismatch: true,
      jsonLdPresent: true,
      delivery: { cdn: 'cloudflare', server: 'nginx' },
    });
    const json = JSON.stringify(signal);
    for (const leak of ['secret-canonical', 'Secret Title', 'Secret description', 'SecretType']) {
      expect(json).not.toContain(leak);
    }
    expect(json.length).toBeLessThan(2000);
  });

  it('edge-seo: no robots mismatch, no JSON-LD and null delivery tokens when absent', () => {
    const report = {
      url: 'https://x.example/',
      seoDefects: {
        verdict: 'attention',
        defects: ['canonical-missing'],
        counts: { total: 1, canonical: 1, hreflang: 0, title: 0, description: 0, jsonld: 0 },
        staticCompared: true,
        notMeasured: [],
        jsonLd: { static: { blocks: 0 }, rendered: { blocks: 0 } },
      },
    };
    expect(lensSignalFor('edge-seo', report)).toMatchObject({
      robotsMismatch: false,
      jsonLdPresent: false,
      delivery: { cdn: null, server: null },
    });
  });

  it('markdown-origin: the verdict and the negotiation facts, no page text', () => {
    const report = {
      url: 'https://docs.example/guide',
      agentSignals: {
        status: 'ok',
        verdict: 'attention',
        markdown: {
          negotiated: false,
          httpStatus: 200,
          contentType: 'text/html; charset=utf-8',
          varyAccept: false,
          sample: 'Secret page text',
        },
      },
    };
    const signal = lensSignalFor('markdown-origin', report);
    expect(signal).toEqual({
      verdict: 'attention',
      negotiated: false,
      httpStatus: 200,
      contentType: 'text/html; charset=utf-8',
      varyAccept: false,
    });
    expect(JSON.stringify(signal)).not.toContain('Secret');
  });

  it('markdown-origin: null status and type and no Vary when the lens is absent', () => {
    expect(lensSignalFor('markdown-origin', { url: 'https://docs.example/' })).toEqual({
      verdict: undefined,
      negotiated: undefined,
      httpStatus: null,
      contentType: null,
      varyAccept: false,
    });
  });

  it('response-firewall: ids, counts, product and line tokens and no versions', () => {
    const responseExposure = {
      status: 'ok',
      verdict: 'attention',
      reasons: ['debug-output', 'version-tell', 'missing-headers'],
      leaks: [{ id: 'php-error', count: 2, where: 'both' }],
      tells: [
        { product: 'apache', source: 'server', version: '2.4.29' },
        { product: 'php', source: 'x-powered-by', version: '7.4.33' },
      ],
      endOfLife: [{ product: 'php', line: '7.4', basis: 'eol-date', date: '2022-11-28' }],
      pagespeed: { header: 'x-mod-pagespeed', bucket: 'google-era' },
      headers: { https: true, present: ['hsts'], missing: ['nosniff', 'framing'] },
      delivery: { cdn: 'cloudflare', server: 'apache' },
    };
    const signal = lensSignalFor('response-firewall', {
      url: 'https://shop.example/',
      responseExposure,
    });
    expect(signal).toEqual({
      verdict: 'attention',
      reasons: ['debug-output', 'version-tell', 'missing-headers'],
      leaks: [{ id: 'php-error', count: 2 }],
      endOfLife: [{ product: 'php', line: '7.4', basis: 'eol-date' }],
      tells: ['apache', 'php'],
      missingHeaders: ['nosniff', 'framing'],
      https: true,
      pagespeedBucket: 'google-era',
      delivery: { cdn: 'cloudflare', server: 'apache' },
    });
    const json = JSON.stringify(signal);
    expect(json).not.toContain('://');
    expect(json).not.toContain('7.4.33');
    expect(json).not.toContain('2.4.29');
  });

  it('response-firewall: null for a blocked, disabled, failed or missing lens', () => {
    for (const responseExposure of [
      { status: 'blocked' },
      { status: 'disabled' },
      { status: 'error' },
      undefined,
    ]) {
      expect(
        lensSignalFor('response-firewall', { url: 'https://shop.example/', responseExposure }),
      ).toBeNull();
    }
  });

  it('response-firewall: tolerates missing headers, pagespeed and delivery', () => {
    const report = {
      url: 'https://x.example/',
      responseExposure: { status: 'ok', verdict: 'attention', reasons: ['end-of-life'] },
    };
    expect(lensSignalFor('response-firewall', report)).toEqual({
      verdict: 'attention',
      reasons: ['end-of-life'],
      leaks: [],
      endOfLife: [],
      tells: [],
      missingHeaders: [],
      https: false,
      pagespeedBucket: null,
      delivery: { cdn: null, server: null },
    });
  });
});

describe('markdownOriginCtaApplies', () => {
  it('fires when the lens ran and the origin does not negotiate Markdown', () => {
    expect(markdownOriginCtaApplies({ status: 'ok', markdown: { negotiated: false } })).toBe(true);
  });

  it('does not fire when Markdown is negotiated or unset, absent, or the lens did not run', () => {
    expect(markdownOriginCtaApplies({ status: 'ok', markdown: { negotiated: true } })).toBe(false);
    expect(markdownOriginCtaApplies({ status: 'ok', markdown: { negotiated: null } })).toBe(false);
    expect(markdownOriginCtaApplies({ status: 'ok', markdown: {} })).toBe(false);
    expect(markdownOriginCtaApplies({ status: 'ok' })).toBe(false);
    expect(markdownOriginCtaApplies({ status: 'error', markdown: { negotiated: false } })).toBe(
      false,
    );
    expect(markdownOriginCtaApplies(undefined)).toBe(false);
  });
});

describe('the platform argument', () => {
  const hosted = { originControl: 'none' };
  // One input per gate that fires without a platform.
  const firing: Array<[string, (platform?: unknown) => boolean]> = [
    [
      'tollbooth',
      (p) =>
        tollboothCtaApplies(
          {
            detail: {
              verifiable: false,
              renderOk: true,
              aiCrawlersAllowed: ['GPTBot'],
              exposurePct: 30,
            },
          },
          p,
        ),
    ],
    ['agentpass', (p) => agentPassCtaApplies({ status: 'ok', classification: 'verifying' }, p)],
    [
      'compliancefix',
      (p) => complianceFixCtaApplies({ available: true, detail: { fixableInline: 1 } }, p),
    ],
    [
      'consent-enforcement',
      (p) => consentEnforcementCtaApplies({ status: 'ok', verdict: 'leaks', trackerCount: 1 }, p),
    ],
    ['page-integrity', (p) => pageIntegrityCtaApplies({ status: 'ok', verdict: 'attention' }, p)],
    [
      'third-party-freeze',
      (p) =>
        thirdPartyFreezeCtaApplies(
          {
            status: 'ok',
            verdict: 'clean',
            totals: { thirdPartyExternal: 2, thirdPartyWithIntegrity: 0 },
          },
          p,
        ),
    ],
    [
      'edge-seo',
      (p) => edgeSeoCtaApplies({ status: 'ok', verdict: 'attention', defects: ['title-js'] }, p),
    ],
    [
      'response-firewall',
      (p) =>
        responseFirewallCtaApplies(
          { status: 'ok', verdict: 'attention', reasons: ['end-of-life'] },
          p,
        ),
    ],
    [
      'markdown-origin',
      (p) => markdownOriginCtaApplies({ status: 'ok', markdown: { negotiated: false } }, p),
    ],
  ];

  it('originTopicsApply is false only for origin control "none"', () => {
    expect(originTopicsApply(hosted)).toBe(false);
    expect(originTopicsApply({ originControl: 'none', confidence: 'low' })).toBe(false);
    for (const p of [
      undefined,
      null,
      {},
      { originControl: 'limited' },
      { originControl: 'full' },
      { originControl: 'unknown' },
      { status: 'disabled' },
    ]) {
      expect(originTopicsApply(p)).toBe(true);
    }
  });

  it.each(firing)('%s does not fire on a hosted platform', (_id, gate) => {
    expect(gate()).toBe(true);
    expect(gate(hosted)).toBe(false);
  });

  it.each(firing)('%s is unchanged by any other platform reading', (_id, gate) => {
    for (const p of [
      null,
      {},
      { originControl: 'limited' },
      { originControl: 'unknown' },
      { status: 'error' },
    ]) {
      expect(gate(p)).toBe(true);
    }
  });
});
