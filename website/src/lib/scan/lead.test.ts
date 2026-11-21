// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { buildLeadPayload, lensSignalFor } from './demand.mjs';
import {
  FOUND_MARKER,
  MAX_BODY_BYTES,
  MAX_SIGNAL_CHARS,
  bodyBytes,
  boundSignal,
  chipsFor,
  emailOk,
  factsFor,
  newSubmissionId,
  payloadForSubmission,
  topicsFor,
  type LeadForm,
} from './lead';
import { buildReportMarkdown, riskStates } from './report';
import { riskRows } from './risk-rows';

const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url),
      'utf8',
    ),
  ).report;
const full = fixture('full');
const clean = fixture('clean');

const GATED_ORDER = [
  'tollbooth',
  'agentpass',
  'compliancefix',
  'consent-enforcement',
  'page-integrity',
  'third-party-freeze',
  'edge-seo',
  'response-firewall',
  'markdown-origin',
];

// Break exactly one gate in the full report.
const without: Record<string, (r: Record<string, unknown>) => void> = {
  tollbooth: (r) =>
    ((r.agentVerifiability as { detail: { verifiable: boolean } }).detail.verifiable = true),
  agentpass: (r) =>
    ((r.signedAgentVerification as { classification: string }).classification = 'ignoring'),
  'markdown-origin': (r) =>
    ((r.agentSignals as { markdown: { negotiated: boolean } }).markdown.negotiated = true),
  compliancefix: (r) => ((r.accessibility as { available: boolean }).available = false),
  'consent-enforcement': (r) => ((r.preConsentLeak as { verdict: string }).verdict = 'clean'),
  'edge-seo': (r) => ((r.seoDefects as { verdict: string }).verdict = 'clean'),
  'response-firewall': (r) => ((r.responseExposure as { verdict: string }).verdict = 'clean'),
};

describe('chipsFor', () => {
  it('offers every gated chip in display order, then the ungated ones, on the full report', () => {
    const chips = chipsFor(full, 'good');
    expect(chips.map((c) => c.id)).toEqual([...GATED_ORDER, 'consulting', 'agents', 'audit']);
    expect(chips.filter((c) => c.gated).every((c) => c.wedge === c.id)).toBe(true);
    expect(chips.filter((c) => !c.gated).every((c) => c.wedge === '')).toBe(true);
  });

  it('offers only the ungated chips on a clean report', () => {
    const chips = chipsFor(clean, 'good');
    expect(chips.map((c) => c.id)).toEqual(['consulting', 'agents', 'audit']);
    expect(chips.some((c) => c.selected)).toBe(false);
  });

  for (const [id, mutate] of Object.entries(without)) {
    it(`drops the ${id} chip when its gate does not fire`, () => {
      const report = structuredClone(full);
      mutate(report);
      const ids = chipsFor(report, null).map((c) => c.id);
      expect(ids).not.toContain(id);
      expect(ids).toContain('agents');
    });
  }

  it('drops both script chips when the script inventory is clean and fully pinned', () => {
    const report = structuredClone(full);
    report.scriptInventory.verdict = 'clean';
    report.scriptInventory.totals.thirdPartyWithIntegrity =
      report.scriptInventory.totals.thirdPartyExternal;
    const ids = chipsFor(report, null).map((c) => c.id);
    expect(ids).not.toContain('page-integrity');
    expect(ids).not.toContain('third-party-freeze');
  });

  it('no gated chip is pre-selected', () => {
    for (const speed of ['good', 'needs-work', 'none', null] as const) {
      const chips = chipsFor(full, speed);
      expect(chips.filter((c) => c.gated).map((c) => c.id)).toEqual(GATED_ORDER);
      expect(chips.filter((c) => c.selected)).toEqual([]);
    }
    // A Poor speed result ticks the speed-help chip and nothing else.
    const poor = chipsFor(full, 'poor');
    expect(poor.filter((c) => c.selected).map((c) => c.id)).toEqual(['consulting']);
    expect(poor.filter((c) => c.gated).some((c) => c.selected)).toBe(false);
  });

  it('lists the chips whose check fired first, whatever is selected', () => {
    const report = structuredClone(full);
    for (const id of [
      'tollbooth',
      'agentpass',
      'markdown-origin',
      'compliancefix',
      'consent-enforcement',
    ]) {
      without[id](report);
    }
    report.scriptInventory.verdict = 'clean';
    report.scriptInventory.totals.thirdPartyWithIntegrity =
      report.scriptInventory.totals.thirdPartyExternal;
    const chips = chipsFor(report, 'poor');
    expect(chips.map((c) => c.id)).toEqual([
      'edge-seo',
      'response-firewall',
      'consulting',
      'agents',
      'audit',
    ]);
    expect(chips.map((c) => c.gated)).toEqual([true, true, false, false, false]);
    expect(chips.filter((c) => c.selected).map((c) => c.id)).toEqual(['consulting']);
  });

  it('pre-selects the speed-help chip only when Speed is Poor', () => {
    for (const state of ['good', 'needs-work', 'none'] as const) {
      expect(chipsFor(clean, state).find((c) => c.id === 'consulting')!.selected).toBe(false);
    }
    expect(chipsFor(clean, 'poor').find((c) => c.id === 'consulting')!.selected).toBe(true);
  });

  it('names the marker for a chip whose check fired', () => {
    expect(FOUND_MARKER).toBe('(flagged by your scan)');
  });

  it('adds the speed-help chip only once Speed has settled; the agents chip is always there', () => {
    expect(chipsFor(clean, null).map((c) => c.id)).toEqual(['agents', 'audit']);
    expect(chipsFor(null, null).map((c) => c.id)).toEqual(['agents']);
    expect(chipsFor(null, 'poor').map((c) => c.id)).toEqual(['consulting', 'agents']);
  });

  it('never pre-selects the audit chip and labels it with the price from the facts', () => {
    const audit = chipsFor(full, 'poor').find((c) => c.id === 'audit')!;
    expect(audit.selected).toBe(false);
    expect(audit.gated).toBe(false);
    expect(audit.label).toBe('Agent-readiness audit (\u20ac950 per site, excl. VAT)');
  });

  describe('markdown-origin chip', () => {
    const ids = (r: Record<string, unknown>) => chipsFor(r, null).map((c) => c.id);
    it('fires on the full report and not on the clean one', () => {
      expect(ids(full)).toContain('markdown-origin');
      expect(ids(clean)).not.toContain('markdown-origin');
    });
    it('does not fire when the key is absent or the lens did not run', () => {
      const absent = structuredClone(full);
      delete absent.agentSignals;
      expect(ids(absent)).not.toContain('markdown-origin');
      const errored = structuredClone(full);
      errored.agentSignals.status = 'error';
      expect(ids(errored)).not.toContain('markdown-origin');
      const unknown = structuredClone(full);
      unknown.agentSignals.markdown.negotiated = null;
      expect(ids(unknown)).not.toContain('markdown-origin');
    });
    it('is the last gated chip, labelled for the wedge, offered but never pre-selected', () => {
      const chips = chipsFor(full, null);
      const at = chips.map((c) => c.id);
      expect(at.indexOf('markdown-origin')).toBe(at.indexOf('response-firewall') + 1);
      const chip = chips[at.indexOf('markdown-origin')];
      expect(chip.label).toBe('Serving AI agents a Markdown copy at the origin');
      expect(chip.gated).toBe(true);
      expect(chip.selected).toBe(false);
    });
    it('posts the scanner’s object shape with no page text', () => {
      const p = buildLeadPayload({
        wedge: 'markdown-origin',
        email: 'a@b.example',
        name: '',
        note: '',
        report: full,
      });
      expect(p.lensSignal).toEqual({
        verdict: 'attention',
        negotiated: false,
        httpStatus: 200,
        contentType: 'text/html; charset=utf-8',
        varyAccept: false,
      });
      expect(p.topic).toBe('markdown-origin');
      expect(p.wedge).toBe('markdown-origin');
    });
  });
});

const form: LeadForm = {
  email: 'a@b.example',
  sites: '7',
  note: ' hello ',
  url: 'https://typed.example/',
  mobile: 41,
  desktop: 88,
  t: 1700000000000,
  submissionId: 'sub_1-A',
};
const chips = chipsFor(full, 'good');

describe('payloadForSubmission', () => {
  it('one payload carries every selected topic with chosen flags', () => {
    const p = payloadForSubmission(chips, ['consulting', 'edge-seo', 'agentpass'], form, full);
    expect(p).toEqual({
      email: 'a@b.example',
      name: '',
      url: 'https://shop.example.com/',
      t: 1700000000000,
      origin: 'scan-result',
      submissionId: 'sub_1-A',
      topic: 'agentpass',
      wedge: 'agentpass',
      message: 'hello',
      topics: [
        { id: 'agentpass', wedge: 'agentpass', chosen: true },
        { id: 'edge-seo', wedge: 'edge-seo', chosen: true },
        { id: 'consulting', wedge: null, chosen: true },
        { id: 'tollbooth', wedge: 'tollbooth', chosen: false },
        { id: 'compliancefix', wedge: 'compliancefix', chosen: false },
        { id: 'consent-enforcement', wedge: 'consent-enforcement', chosen: false },
      ],
      lensSignals: {
        agentpass: lensSignalFor('agentpass', full),
        'edge-seo': lensSignalFor('edge-seo', full),
      },
      facts: {
        grade: 'C',
        score: 64,
        speedMobile: 41,
        speedDesktop: 88,
        sites: 7,
        cdn: 'cloudflare',
      },
    });
  });

  it('found topics follow chosen ones and the list is capped at six', () => {
    const one = topicsFor(chips, ['markdown-origin']);
    expect(one).toHaveLength(6);
    expect(one[0]).toEqual({ id: 'markdown-origin', wedge: 'markdown-origin', chosen: true });
    expect(one.slice(1).map((t) => [t.id, t.chosen])).toEqual(
      GATED_ORDER.slice(0, 5).map((id) => [id, false]),
    );
    // Four ticked chips leave room for two found ones; no ticked chip is dropped.
    const four = topicsFor(chips, ['audit', 'agents', 'consulting', 'response-firewall']);
    expect(four.map((t) => [t.id, t.wedge, t.chosen])).toEqual([
      ['response-firewall', 'response-firewall', true],
      ['consulting', null, true],
      ['agents', null, true],
      ['audit', null, true],
      ['tollbooth', 'tollbooth', false],
      ['agentpass', 'agentpass', false],
    ]);
    // An ungated chip that is not ticked is never listed.
    expect(topicsFor(chipsFor(clean, 'good'), ['audit'])).toEqual([
      { id: 'audit', wedge: null, chosen: true },
    ]);
  });

  it('an untouched form sends the general question as the only chosen topic', () => {
    const p = payloadForSubmission(chips, [], form, full);
    // The general topic is its own id, so it is not mistaken for a ticked chip.
    expect(p.topics[0]).toEqual({ id: 'general', wedge: null, chosen: true });
    expect(p.topics.filter((t) => t.chosen)).toHaveLength(1);
    expect(p.topics.slice(1).map((t) => [t.id, t.chosen])).toEqual(
      GATED_ORDER.slice(0, 5).map((id) => [id, false]),
    );
    expect(p.topic).toBe('general');
    expect(p.wedge).toBe('');
    const tickedAgents = payloadForSubmission(chips, ['agents'], form, full);
    expect(tickedAgents.topic).toBe('agents');
    expect(tickedAgents.topics[0]).toEqual({ id: 'agents', wedge: null, chosen: true });
    expect(p.lensSignals).toEqual({});
    // With no report at all the general question is the whole list.
    const bare = payloadForSubmission(chipsFor(null, null), [], form, null);
    expect(bare.topics).toEqual([{ id: 'general', wedge: null, chosen: true }]);
    expect(bare.url).toBe('https://typed.example/');
    expect(bare.facts).toEqual({ speedMobile: 41, speedDesktop: 88, sites: 7 });
  });

  it('scalar topic and wedge mirror the first chosen topic', () => {
    const gated = payloadForSubmission(chips, ['agents', 'edge-seo'], form, full);
    expect([gated.topic, gated.wedge]).toEqual(['edge-seo', 'edge-seo']);
    const ungated = payloadForSubmission(chips, ['audit', 'consulting'], form, full);
    expect([ungated.topic, ungated.wedge]).toEqual(['consulting', '']);
    expect(ungated.topics[0]).toEqual({ id: 'consulting', wedge: null, chosen: true });
  });

  it("the message is the visitor's note only", () => {
    const p = payloadForSubmission(
      chips,
      ['edge-seo'],
      { ...form, note: '  Line one\nline two ' },
      full,
    );
    expect(p.message).toBe('Line one\nline two');
    for (const note of [undefined, '', '   ']) {
      expect(payloadForSubmission(chips, ['edge-seo'], { ...form, note }, full).message).toBe('');
    }
  });

  it('the body stays under 8192 bytes with four chosen topics and a full-length non-ASCII note', () => {
    const ticked = ['agentpass', 'page-integrity', 'edge-seo', 'response-firewall'];
    // The note field takes 1000 characters; these take three bytes each.
    const note = '\u20ac'.repeat(1000);
    const p = payloadForSubmission(chips, ticked, { ...form, note }, full);
    expect(bodyBytes(p)).toBeLessThanOrEqual(MAX_BODY_BYTES);
    expect(p.message).toBe(note);
    expect(p.topics.filter((t) => t.chosen).map((t) => t.id)).toEqual(ticked);

    // A result with oversized signals loses them largest first, then the
    // unticked topics; the email, the note and the ticked topics stay.
    const big = structuredClone(full);
    big.signedAgentVerification.evidence = Array.from({ length: 300 }, (_, i) => `evidence-${i}`);
    big.seoDefects.defects = Array.from({ length: 200 }, (_, i) => `defect-${i}`);
    const cut = payloadForSubmission(chipsFor(big, 'good'), ticked, { ...form, note }, big);
    expect(bodyBytes(cut)).toBeLessThanOrEqual(MAX_BODY_BYTES);
    expect(Object.keys(cut.lensSignals)).not.toContain('agentpass');
    expect(cut.email).toBe('a@b.example');
    expect(cut.message).toBe(note);
    expect(cut.topics.filter((t) => t.chosen).map((t) => t.id)).toEqual(ticked);

    // Signals alone are not enough here: the unticked topics go as well.
    const long = { ...form, note: '\u20ac'.repeat(2530) };
    const bare = payloadForSubmission(chips, ticked, long, full);
    expect(bodyBytes(bare)).toBeLessThanOrEqual(MAX_BODY_BYTES);
    expect(bare.lensSignals).toEqual({});
    expect(bare.topics.map((t) => [t.id, t.chosen])).toEqual(ticked.map((id) => [id, true]));
    expect(bare.message).toBe(long.note);
  });

  it('facts carry platform fields when the report has them and omit them when it does not', () => {
    const hosted = structuredClone(full);
    hosted.platform = {
      status: 'ok',
      id: 'wix',
      name: 'Wix',
      kind: 'hosted-builder',
      originControl: 'none',
      confidence: 'high',
      evidence: [
        { signal: 'header', id: 'server-pepyaka' },
        { signal: 'generator', id: 'wix-generator' },
      ],
    };
    expect(factsFor(form, hosted)).toEqual({
      grade: 'C',
      score: 64,
      speedMobile: 41,
      speedDesktop: 88,
      sites: 7,
      platform: 'wix',
      platformName: 'Wix',
      originControl: 'none',
      server: 'pepyaka',
      cdn: 'cloudflare',
    });

    // No platform named: only the server software from the evidence.
    const own = structuredClone(full);
    own.platform = {
      status: 'ok',
      id: null,
      name: null,
      kind: 'unknown',
      originControl: 'unknown',
      evidence: [{ signal: 'header', id: 'server-nginx' }],
    };
    expect(factsFor(form, own)).toEqual({
      grade: 'C',
      score: 64,
      speedMobile: 41,
      speedDesktop: 88,
      sites: 7,
      server: 'nginx',
      cdn: 'cloudflare',
    });

    // A cached or failed result has no platform reading; unmeasured values are left out.
    for (const platform of [undefined, null, 'wix', { status: 'error' }]) {
      const report = structuredClone(full);
      report.platform = platform;
      expect(factsFor({ ...form, sites: 'abc', mobile: null, desktop: undefined }, report)).toEqual(
        { grade: 'C', score: 64, cdn: 'cloudflare' },
      );
    }
    expect(factsFor({ ...form, sites: '0' }, null)).toEqual({ speedMobile: 41, speedDesktop: 88 });
  });

  it('facts carry the CDN when the report names one', () => {
    const cdnOf = (r: Record<string, unknown>) => factsFor(form, r).cdn;
    expect(cdnOf(full)).toBe('cloudflare');
    // The second place a report names it, when the first has none.
    const other = structuredClone(full);
    other.seoDefects.delivery.cdn = null;
    other.responseExposure.delivery.cdn = 'fastly';
    expect(cdnOf(other)).toBe('fastly');
    for (const none of [null, '', 'none', undefined]) {
      const r = structuredClone(full);
      r.seoDefects.delivery.cdn = none;
      r.responseExposure.delivery.cdn = none;
      expect('cdn' in factsFor(form, r)).toBe(false);
    }
    const missing = structuredClone(full);
    delete missing.seoDefects;
    delete missing.responseExposure;
    expect('cdn' in factsFor(form, missing)).toBe(false);
  });

  it('cuts the scanned address to the 500 characters the endpoint keeps', () => {
    const url = 'https://example.com/' + '\u00e9'.repeat(1500);
    const long = { ...form, url, note: '\u6f22'.repeat(1000) };
    const p = payloadForSubmission(chipsFor(null, null), [], long, null);
    expect(p.url).toBe(url.slice(0, 500));
    expect(bodyBytes(p)).toBeLessThanOrEqual(MAX_BODY_BYTES);
    const fromReport = payloadForSubmission(chips, ['edge-seo'], long, { ...full, url });
    expect(fromReport.url).toHaveLength(500);
    expect(bodyBytes(fromReport)).toBeLessThanOrEqual(MAX_BODY_BYTES);
    expect(payloadForSubmission(chips, [], form, full).url).toBe('https://shop.example.com/');
  });

  it('never sends a signal the endpoint would drop', () => {
    // Long header echoes are cut, and the signal is then small enough to keep.
    const echo = structuredClone(full);
    for (const probe of Object.values(echo.signedAgentVerification.probes) as Array<{
      headers: Record<string, string>;
    }>) {
      for (const h of [
        'www-authenticate',
        'signature-input',
        'accept-signature',
        'server',
        'via',
      ]) {
        probe.headers[h] = 'x'.repeat(300);
      }
    }
    expect(JSON.stringify(lensSignalFor('agentpass', echo)).length).toBeGreaterThan(
      MAX_SIGNAL_CHARS,
    );
    const kept = payloadForSubmission(chipsFor(echo, 'good'), ['agentpass'], form, echo);
    const signal = kept.lensSignals.agentpass as {
      classification: string;
      probes: { signed: { status: number; headers: Record<string, string> } };
    };
    expect(JSON.stringify(signal).length).toBeLessThanOrEqual(MAX_SIGNAL_CHARS);
    expect(signal.classification).toBe('signature-aware');
    expect(signal.probes.signed.status).toBe(200);
    expect(signal.probes.signed.headers['www-authenticate']).toBe('x'.repeat(50));
    // A single long value is cut to 200 characters.
    expect(boundSignal({ a: 'y'.repeat(5000), n: 1 })).toEqual({ a: 'y'.repeat(200), n: 1 });

    // Many evidence entries: still too large after the cut, so it is left out.
    const big = structuredClone(full);
    big.signedAgentVerification.evidence = Array.from({ length: 30 }, (_, i) => ({
      id: `evidence-item-${i}`,
      detail: 'x'.repeat(60),
    }));
    big.seoDefects.defects = Array.from({ length: 200 }, (_, i) => `defect-${i}`);
    const ticked = ['agentpass', 'edge-seo', 'tollbooth'];
    const p = payloadForSubmission(chipsFor(big, 'good'), ticked, form, big);
    expect(Object.keys(p.lensSignals)).toEqual(['tollbooth']);
    expect(p.topics.filter((t) => t.chosen).map((t) => t.id)).toEqual([
      'tollbooth',
      'agentpass',
      'edge-seo',
    ]);
    for (const s of Object.values(p.lensSignals)) {
      expect(JSON.stringify(s).length).toBeLessThanOrEqual(MAX_SIGNAL_CHARS);
    }

    // A signal that fits is sent as it is; a cut never splits a character pair.
    const plain = lensSignalFor('edge-seo', full);
    expect(boundSignal(plain)).toEqual(plain);
    expect(boundSignal(null)).toBeNull();
    const pair = boundSignal({ text: 'a'.repeat(199) + '\u{1F600}' }) as { text: string };
    expect(pair.text).toBe('a'.repeat(199));
  });

  it('accepts an address only when the endpoint would', () => {
    for (const good of ['name@example.com', ' name@example.com ', 'a.b+c@sub.example.co']) {
      expect(emailOk(good)).toBe(true);
    }
    for (const bad of [
      '',
      'name',
      'name@gmail',
      'name@@example.com',
      'na me@example.com',
      '@a.b',
    ]) {
      expect(emailOk(bad), bad).toBe(false);
    }
  });

  it('carries the platform script count in the script signals when the scan reports it', () => {
    const hosted = structuredClone(full);
    hosted.scriptInventory.totals.platformExternal = 4;
    const p = payloadForSubmission(chipsFor(hosted, 'good'), ['page-integrity'], form, hosted);
    const signal = p.lensSignals['page-integrity'] as { totals: Record<string, number> };
    expect(signal.totals.platformExternal).toBe(4);
    const plain = lensSignalFor('page-integrity', full) as { totals: Record<string, number> };
    expect('platformExternal' in plain.totals).toBe(false);
  });

  it('leaves out the time when the form has none, and a signal the result cannot give', () => {
    const { t: _t, ...noTime } = form;
    expect('t' in payloadForSubmission(chips, [], noTime, full)).toBe(false);
    const broken = structuredClone(full);
    delete broken.seoDefects;
    const p = payloadForSubmission(chips, ['edge-seo', 'agentpass'], form, broken);
    expect(Object.keys(p.lensSignals)).toEqual(['agentpass']);
    expect(p.topics.filter((t) => t.chosen).map((t) => t.id)).toEqual(['agentpass', 'edge-seo']);
  });

  it('makes a fresh submission id each time, in the characters the endpoint takes', () => {
    const a = newSubmissionId();
    expect(a).toMatch(/^[A-Za-z0-9_-]{1,64}$/);
    expect(newSubmissionId()).not.toBe(a);
  });
});

// The signal a topic carries in the one request is the signal the shared
// builder puts in a single-topic body, for every wedge.
describe('signal parity with the single-topic builder', () => {
  for (const wedge of GATED_ORDER) {
    it(`${wedge} on the full report`, () => {
      const single = buildLeadPayload({
        wedge,
        email: 'a@b.example',
        name: '',
        note: 'n',
        report: full,
      });
      expect(single).toMatchObject({ topic: wedge, wedge, url: 'https://shop.example.com/' });
      expect(single.message).toContain(`[${wedge}] lead from RenderPeek result`);
      const p = payloadForSubmission(chips, [wedge], form, full);
      expect(p.lensSignals).toEqual({ [wedge]: single.lensSignal });
    });
  }
});

describe('report markdown', () => {
  it('lists the AI readability grade and the four risk states', () => {
    expect(riskStates(full).map((r) => r.name)).toEqual([
      'Pre-consent leak',
      'Script inventory',
      'SEO defects',
      'Response exposure',
    ]);
    const md = buildReportMarkdown({
      url: 'https://shop.example.com/',
      when: '2026-10-10T00:00:00.000Z',
      mobile: 58,
      desktop: null,
      report: full,
    });
    expect(md).toContain('Speed (PageSpeed Insights): mobile 58/100, desktop n/a');
    expect(md).toContain('AI readability: grade C (64/100)');
    expect(md).toContain('- Pre-consent leak: Attention');
  });

  it('says not measured without a report', () => {
    const md = buildReportMarkdown({
      url: 'u',
      when: 'w',
      mobile: null,
      desktop: null,
      report: null,
    });
    expect(md).toContain('AI readability: not measured');
    expect(md).not.toContain('Risk and SEO');
  });
});

describe('report risk states match the panel', () => {
  const cases: Record<string, Record<string, unknown>> = {
    'ok with no verdict': { scriptInventory: { status: 'ok' } },
    'ok with unknown verdict': { seoDefects: { status: 'ok', verdict: 'unknown' } },
    'ok and flagged': { preConsentLeak: { status: 'ok', verdict: 'leaks' } },
    'ok and clean': { responseExposure: { status: 'ok', verdict: 'clean' } },
    blocked: { seoDefects: { status: 'blocked' } },
    error: { responseExposure: { status: 'error' } },
  };
  for (const [name, report] of Object.entries(cases)) {
    it(name, () => {
      expect(riskStates(report).map((r) => r.state)).toEqual(riskRows(report).map((r) => r.chip));
    });
  }

  it('does not report a lens with no verdict as nothing flagged', () => {
    expect(riskStates({ scriptInventory: { status: 'ok' } })).toEqual([
      { name: 'Script inventory', state: 'Not measured' },
    ]);
  });
});
