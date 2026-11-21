// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import {
  agentPassCtaApplies,
  complianceFixCtaApplies,
  consentEnforcementCtaApplies,
  edgeSeoCtaApplies,
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
    ['the page does not render', { renderOk: false }],
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
