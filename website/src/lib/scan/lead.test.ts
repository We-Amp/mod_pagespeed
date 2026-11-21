// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { buildLeadPayload } from './demand.mjs';
import {
  FOUND_MARKER,
  chipsFor,
  noteFor,
  payloadFor,
  selectionFor,
  submitLeads,
  type Chip,
  type LeadPayload,
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

const consulting: Chip = { id: 'consulting', label: 'x', wedge: '', gated: false, selected: true };
const agents: Chip = { id: 'agents', label: 'y', wedge: '', gated: false, selected: true };
const chipOf = (id: string) => chipsFor(full, 'good').find((c) => c.id === id)!;

describe('noteFor', () => {
  it('prefixes the site count only for the wedges that use it', () => {
    const form = { sites: '12', note: ' WordPress ' };
    expect(noteFor('edge-seo', form)).toBe('sites: 12; WordPress');
    expect(noteFor('response-firewall', form)).toBe('sites: 12; WordPress');
    expect(noteFor('tollbooth', form)).toBe('WordPress');
    expect(noteFor('edge-seo', { sites: '', note: 'x' })).toBe('x');
    expect(noteFor('edge-seo', { sites: '3', note: '' })).toBe('sites: 3');
    for (const bad of ['-3', '0', '', 'abc']) {
      expect(noteFor('edge-seo', { sites: bad, note: 'x' })).toBe('x');
    }
    expect(noteFor('edge-seo', { sites: '1.5', note: '' })).toBe('sites: 1');
  });
});

describe('selectionFor', () => {
  it('sends the ticked chips as they are', () => {
    const picked = [chipOf('edge-seo'), consulting];
    expect(selectionFor(picked)).toBe(picked);
  });

  it('an untouched form sends one general question', async () => {
    const posted: LeadPayload[] = [];
    const out = await submitLeads(
      selectionFor([]),
      { email: 'a@b.example', note: 'How do I start?', url: 'https://typed.example/' },
      full,
      async (p) => {
        posted.push(p);
        return true;
      },
    );
    expect(out.map((o) => [o.chip.id, o.chip.gated, o.ok])).toEqual([['agents', false, true]]);
    expect(posted).toEqual([
      {
        topic: 'agents',
        email: 'a@b.example',
        name: '',
        url: 'https://shop.example.com/',
        wedge: '',
        message:
          '[agents] request from the scan result\nScanned: https://shop.example.com/\n' +
          'Grade: C (64/100)\nNote: How do I start?',
      },
    ]);
  });
});

describe('submitLeads', () => {
  const form = { email: 'a@b.example', sites: '7', note: 'hello', url: 'https://typed.example/' };

  it('posts one lead per chip, one after the other, in selection order', async () => {
    const log: string[] = [];
    let open = 0;
    const post = async (p: LeadPayload) => {
      open += 1;
      expect(open).toBe(1);
      log.push(`start:${p.topic}`);
      await new Promise((r) => setTimeout(r, 5));
      log.push(`end:${p.topic}`);
      open -= 1;
      return true;
    };
    const sel = [chipOf('edge-seo'), consulting, chipOf('tollbooth')];
    const out = await submitLeads(sel, form, full, post);
    expect(log).toEqual([
      'start:edge-seo',
      'end:edge-seo',
      'start:consulting',
      'end:consulting',
      'start:tollbooth',
      'end:tollbooth',
    ]);
    expect(out.map((o) => o.ok)).toEqual([true, true, true]);
  });

  it('builds gated payloads with the shared builder and the composed note', async () => {
    const posted: LeadPayload[] = [];
    await submitLeads([chipOf('edge-seo'), chipOf('tollbooth')], form, full, async (p) => {
      posted.push(p);
      return true;
    });
    expect(posted[0]).toEqual(
      buildLeadPayload({
        wedge: 'edge-seo',
        email: form.email,
        name: '',
        note: 'sites: 7; hello',
        report: full,
      }),
    );
    expect(posted[1]).toEqual(
      buildLeadPayload({
        wedge: 'tollbooth',
        email: form.email,
        name: '',
        note: 'hello',
        report: full,
      }),
    );
  });

  it('posts the ungated shape with an empty wedge and the facts in the message', () => {
    const c = payloadFor(consulting, { ...form, mobile: 41, desktop: 88 }, full);
    expect(c).toEqual({
      topic: 'consulting',
      email: 'a@b.example',
      name: '',
      url: 'https://shop.example.com/',
      wedge: '',
      message:
        '[consulting] request from the scan result\nScanned: https://shop.example.com/\n' +
        'Speed: mobile 41/100, desktop 88/100\nNote: hello',
    });
    const a = payloadFor(agents, form, full);
    expect(a.message).toContain('[agents] request from the scan result');
    expect(a.message).toContain('Grade: C (64/100)');
    expect(a.wedge).toBe('');
  });

  it('falls back to the scanned URL when there is no report', () => {
    const p = payloadFor(agents, { ...form, note: '' }, null);
    expect(p.url).toBe('https://typed.example/');
    expect(p.message).toBe(
      '[agents] request from the scan result\nScanned: https://typed.example/',
    );
  });

  it('keeps going after a failure and reports each outcome', async () => {
    const post = async (p: LeadPayload) => p.topic !== 'agentpass';
    const sel = [chipOf('tollbooth'), chipOf('agentpass'), agents];
    const out = await submitLeads(sel, form, full, post);
    expect(out.map((o) => [o.chip.id, o.ok])).toEqual([
      ['tollbooth', true],
      ['agentpass', false],
      ['agents', true],
    ]);
  });

  it('treats a throwing post as a failure', async () => {
    const out = await submitLeads([agents], form, full, async () => {
      throw new Error('offline');
    });
    expect(out[0].ok).toBe(false);
  });
});

// The v2 body for a gated chip must be exactly what the shared builder makes,
// for every wedge and both fixtures.
describe('payload parity', () => {
  for (const [name, report] of [
    ['full', full],
    ['clean', clean],
  ] as const) {
    for (const wedge of GATED_ORDER) {
      it(`${wedge} on the ${name} report equals buildLeadPayload`, () => {
        const chip: Chip = { id: wedge, label: wedge, wedge, gated: true, selected: true };
        const sites = wedge === 'edge-seo' || wedge === 'response-firewall' ? '5' : '';
        if (name === 'clean') {
          // No gate fires on the clean report; the builder must still not throw
          // for wedges without a lens, so only compare where it can run.
          let expected: unknown;
          try {
            expected = buildLeadPayload({
              wedge,
              email: 'a@b.example',
              name: '',
              note: 'n',
              report,
            });
          } catch {
            expect(() =>
              payloadFor(chip, { email: 'a@b.example', note: 'n', url: '' }, report),
            ).toThrow();
            return;
          }
          expect(payloadFor(chip, { email: 'a@b.example', note: 'n', url: '' }, report)).toEqual(
            expected,
          );
          return;
        }
        const note = (sites ? 'sites: 5; ' : '') + 'n';
        expect(
          payloadFor(chip, { email: 'a@b.example', sites, note: 'n', url: '' }, report),
        ).toEqual(buildLeadPayload({ wedge, email: 'a@b.example', name: '', note, report }));
      });
    }
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
