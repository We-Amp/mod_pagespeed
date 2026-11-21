// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The one lead form: which topic chips a result offers, and how a selection
// becomes lead POSTs. Pure: no DOM, no network (the caller passes `post`).
// The endpoint takes one wedge per POST, so a selection of N chips is N
// sequential POSTs; gated chips use the shared payload builder unchanged.
// Topics are opt-in: a chip whose check fired is offered and marked, never
// ticked for the visitor.

import {
  agentPassCtaApplies,
  buildLeadPayload,
  complianceFixCtaApplies,
  consentEnforcementCtaApplies,
  edgeSeoCtaApplies,
  markdownOriginCtaApplies,
  pageIntegrityCtaApplies,
  responseFirewallCtaApplies,
  thirdPartyFreezeCtaApplies,
  tollboothCtaApplies,
} from './demand.mjs';
import { AUDIT_PRICE_EUR, formatEur } from '../../data/product-facts.mjs';
import type { TileState } from './status';

/** The most topics one submission may carry (the endpoint allows five POSTs a minute). */
export const MAX_TOPICS = 4;

export interface Chip {
  /** The contact topic; for gated chips it is also the wedge. */
  id: string;
  label: string;
  /** The wedge posted; empty for the ungated chips. */
  wedge: string;
  gated: boolean;
  selected: boolean;
}

type Report = Record<string, unknown> & { url?: string; grade?: string; score?: number };

interface ChipDef {
  id: string;
  label: string;
  fires: (r: Report) => boolean;
}

// Display order follows the panels: AI readability, Risk & SEO, then Speed.
const GATED: ChipDef[] = [
  {
    id: 'tollbooth',
    label: 'Origin controls (who reads / verifies)',
    fires: (r) => tollboothCtaApplies(r.agentVerifiability),
  },
  {
    id: 'agentpass',
    label: 'Signed-agent verification (prove which agent is which)',
    fires: (r) => agentPassCtaApplies(r.signedAgentVerification),
  },
  {
    id: 'compliancefix',
    label: 'Accessibility remediation',
    fires: (r) => complianceFixCtaApplies(r.accessibility),
  },
  {
    id: 'consent-enforcement',
    label: 'Consent enforcement',
    fires: (r) => consentEnforcementCtaApplies(r.preConsentLeak),
  },
  {
    id: 'page-integrity',
    label: 'Script control',
    fires: (r) => pageIntegrityCtaApplies(r.scriptInventory),
  },
  {
    id: 'third-party-freeze',
    label: 'Pinning third-party scripts',
    fires: (r) => thirdPartyFreezeCtaApplies(r.scriptInventory),
  },
  {
    id: 'edge-seo',
    label: 'SEO fixes at the server',
    fires: (r) => edgeSeoCtaApplies(r.seoDefects),
  },
  {
    id: 'response-firewall',
    label: 'Leaked errors and outdated software',
    fires: (r) => responseFirewallCtaApplies(r.responseExposure),
  },
  {
    id: 'markdown-origin',
    label: 'Serving AI agents a Markdown copy at the origin',
    fires: (r) => markdownOriginCtaApplies(r.agentSignals),
  },
];

export const CONSULTING = { id: 'consulting', label: 'Help applying the speed fixes' };
export const AGENTS = { id: 'agents', label: 'AI agents and crawlers' };
export const AUDIT = {
  id: 'audit',
  label: `Agent-readiness audit (${formatEur(AUDIT_PRICE_EUR)} per site, excl. VAT)`,
};

/** Wedges whose lead carries the optional "sites you run" count. */
export const SITES_WEDGES = ['edge-seo', 'response-firewall'];

/** Shown on a chip whose check fired on the scanned page; part of its accessible name. */
export const FOUND_MARKER = '(flagged by your scan)';

/**
 * The chips for a result, in display order. Gated chips appear only when
 * their gate fires; they come first and are never pre-selected, on either
 * page. The speed-help chip appears once the speed result has settled (`speed`
 * is null while it is still checking) and is pre-selected only when Speed is
 * Poor; the agents and audit chips are always offered, unselected. With no
 * report only the ungated chips remain.
 */
export function chipsFor(
  report: Report | null | undefined,
  speed: TileState | null | undefined,
): Chip[] {
  const chips: Chip[] = [];
  if (report) {
    for (const def of GATED) {
      if (def.fires(report)) {
        chips.push({
          id: def.id,
          label: def.label,
          wedge: def.id,
          gated: true,
          selected: false,
        });
      }
    }
  }
  if (speed) {
    chips.push({ ...CONSULTING, wedge: '', gated: false, selected: speed === 'poor' });
  }
  chips.push({ ...AGENTS, wedge: '', gated: false, selected: false });
  if (report) chips.push({ ...AUDIT, wedge: '', gated: false, selected: false });
  return chips;
}

/**
 * What a submit sends: the ticked chips, or, when none is ticked, one request
 * for the general topic. An untouched form is a general question, never an error.
 */
export function selectionFor(picked: Chip[]): Chip[] {
  return picked.length > 0 ? picked : [{ ...AGENTS, wedge: '', gated: false, selected: false }];
}

export interface LeadForm {
  email: string;
  /** The optional count of sites, as typed. */
  sites?: string;
  note?: string;
  /** The scanned URL, used when the report has none (or no report exists). */
  url: string;
  /** Speed scores for the speed-help lead, when measured. */
  mobile?: number | null;
  desktop?: number | null;
  /** The page-load time (ms since the epoch), so the handler can judge a too-fast submit. */
  t?: number;
}

export interface Outcome {
  chip: Chip;
  ok: boolean;
}

/** One lead as sent to the endpoint. */
export type LeadPayload = Record<string, unknown>;

const SCORE = (n: number | null | undefined) => (typeof n === 'number' ? `${n}/100` : 'n/a');

/** The note for one chip: the site count only for the wedges that use it. */
export function noteFor(wedge: string, form: Pick<LeadForm, 'sites' | 'note'>): string {
  const n = parseInt((form.sites ?? '').trim(), 10);
  const text = (form.note ?? '').trim();
  const sites = SITES_WEDGES.includes(wedge) && Number.isInteger(n) && n > 0 ? 'sites: ' + n : '';
  return [sites, text].filter(Boolean).join('; ');
}

/** The POST body for one chip. Gated chips use the shared builder unchanged. */
export function payloadFor(chip: Chip, form: LeadForm, report: Report | null): LeadPayload {
  const payload = basePayloadFor(chip, form, report);
  return form.t === undefined ? payload : { ...payload, t: form.t };
}

function basePayloadFor(chip: Chip, form: LeadForm, report: Report | null): LeadPayload {
  const note = noteFor(chip.wedge, form);
  if (chip.gated) {
    return buildLeadPayload({
      wedge: chip.wedge,
      email: form.email,
      name: '',
      note,
      report,
    });
  }
  const url = (report && report.url) || form.url;
  const facts =
    chip.id === 'consulting'
      ? `Speed: mobile ${SCORE(form.mobile)}, desktop ${SCORE(form.desktop)}`
      : report && report.grade
        ? `Grade: ${report.grade} (${SCORE(report.score)})`
        : null;
  const message = [
    `[${chip.id}] request from the scan result`,
    `Scanned: ${url}`,
    facts,
    note ? `Note: ${note}` : null,
  ]
    .filter(Boolean)
    .join('\n');
  return { topic: chip.id, email: form.email, name: '', url, wedge: '', message };
}

/**
 * Send one lead per selected chip, one after the other, and report each
 * outcome. A failed POST does not stop the rest, so the caller can retry only
 * the chips that failed.
 */
export async function submitLeads(
  selection: Chip[],
  form: LeadForm,
  report: Report | null,
  post: (payload: LeadPayload) => Promise<boolean>,
): Promise<Outcome[]> {
  const outcomes: Outcome[] = [];
  for (const chip of selection) {
    let ok = false;
    try {
      ok = await post(payloadFor(chip, form, report));
    } catch {
      ok = false;
    }
    outcomes.push({ chip, ok });
  }
  return outcomes;
}
