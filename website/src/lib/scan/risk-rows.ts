// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Pure row model for the Risk & SEO panel: for each of the four lenses, the
// state chip, the one verdict sentence, and (only when the lens's gate fires)
// the "what is planned" sentences with the link and its event name. On a
// hosted platform no gate fires, so a row keeps its finding and offers nothing.
// The verdict and card sentences of the first three lenses are the approved
// wording of the v1 results page. The gate predicates come from demand.mjs.

import {
  consentEnforcementCtaApplies,
  edgeSeoCtaApplies,
  pageIntegrityCtaApplies,
  responseFirewallCtaApplies,
  thirdPartyFreezeCtaApplies,
} from './demand.mjs';
import { platformName } from './fit';
import { lifecycleSentence, parseVersion } from './lifecycle';
import { plural } from './plural';
import { RISK_LENSES, lensMeasured } from './status';

export type RowState = 'attention' | 'clean' | 'none';

export interface RiskLink {
  href: string;
  label: string;
  /** The umami event the link carries (names unchanged from v1). */
  event: string;
}

export interface RiskRow {
  key: (typeof RISK_LENSES)[number]['key'];
  name: string;
  state: RowState;
  /** The chip word, always visible. */
  chip: string;
  /** The one verdict sentence, or the not-measured note. */
  sentence: string;
  /** Secondary muted line listing what was flagged; empty when there is none. */
  detail: string;
  /** "What is planned" sentences; empty unless the lens's gate fires. */
  building: string[];
  link: RiskLink | null;
}

const DEFAULT_NOTE = 'Not measured: the site blocked the scanner.';
const ERROR_NOTE = 'Not measured: the scanner could not complete this check.';

const NAMES: Record<RiskRow['key'], string> = {
  preConsentLeak: 'Pre-consent leak',
  scriptInventory: 'Script inventory',
  seoDefects: 'SEO defects',
  responseExposure: 'Response exposure',
};

// The lens objects are scanner output; every field is read defensively.
/* eslint-disable @typescript-eslint/no-explicit-any */
type Lens = Record<string, any>;

// ---- Pre-consent leak (v1 wording) -------------------------------------

function pclVerdict(pcl: Lens): string {
  if (pcl.verdict === 'unknown') {
    return 'We could not render this page, so we could not observe which third parties it contacts before consent.';
  }
  if (pcl.trackerCount === 0) {
    return 'No tracker requests observed before any interaction, in a fresh browser with no cookies and no storage. Heuristic host list; not legal advice.';
  }
  const vendor = pcl.cmpDetected && pcl.cmpDetected.detected ? pcl.cmpDetected.vendor : null;
  return (
    plural(pcl.trackerCount, 'tracker host') +
    ' contacted before any interaction, in a fresh browser with no consent given.' +
    (vendor
      ? ' A consent banner (' +
        vendor +
        ') is present, but these requests fire before it can be answered.'
      : '') +
    ' Heuristic host list; not legal advice or a compliance certificate.'
  );
}

// ---- Script inventory (v1 wording) -------------------------------------

function siReasonText(si: Lens): string {
  const reasons: string[] = Array.isArray(si.reasons) ? si.reasons : [];
  const bits: string[] = [];
  if (reasons.includes('sri-missing'))
    bits.push('scripts from public CDNs without an integrity hash');
  if (reasons.includes('floating-version')) bits.push('CDN scripts with no pinned version');
  if (reasons.includes('duplicate-library')) bits.push('the same library at more than one version');
  if (reasons.includes('unlisted-origin'))
    bits.push('scripts from hosts not on our known-host list');
  return bits.length ? 'Flagged: ' + bits.join('; ') + '.' : '';
}

/**
 * The external scripts with the platform's own apart from third parties, or an
 * empty string when the scan counted no platform scripts.
 */
export function siPlatformSplit(si: Lens, platformName: string): string {
  const t = si.totals || {};
  const platform = Number(t.platformExternal) || 0;
  if (platform <= 0) return '';
  const third = Number(t.thirdPartyExternal) || 0;
  const first = Number(t.firstPartyExternal) || 0;
  // The head is the sum of the parts it lists, so the sentence always adds up.
  const external = platform + third + first;
  const owner = platformName ? platformName + '’s' : 'the platform’s';
  return (
    plural(external, 'external script') +
    ': ' +
    platform +
    ' from ' +
    owner +
    ' own hosts, ' +
    third +
    ' from third parties' +
    (first > 0 ? ', ' + first + ' from this site’s own host' : '') +
    '.'
  );
}

function siVerdict(si: Lens, report?: Record<string, unknown>): string {
  if (si.verdict === 'unknown') {
    return 'We could not render this page, so we could not inventory its scripts.';
  }
  const head =
    si.verdict === 'attention'
      ? 'Some scripts on this page need attention.'
      : 'No script issues flagged on this page.';
  const split = siPlatformSplit(si, platformName(report));
  return (
    head +
    (split ? ' ' + split : '') +
    ' Heuristic host list; not an assessment against any standard.'
  );
}

// ---- SEO defects (v1 wording) ------------------------------------------

const SD_GROUPS: [string, string][] = [
  ['canonical', 'canonical'],
  ['hreflang', 'hreflang'],
  ['title', 'title'],
  ['description', 'meta description'],
  ['jsonld', 'structured data'],
];
const SD_NOT_MEASURED: Record<string, string> = {
  'canonical-js': 'canonical',
  'hreflang-js': 'hreflang',
  'title-js': 'title',
  'description-js': 'meta description',
  'jsonld-js': 'structured data',
};

function sdFiredGroups(sd: Lens): [string, string][] {
  return sd.status === 'ok' && sd.counts ? SD_GROUPS.filter((g) => sd.counts[g[0]] > 0) : [];
}

function sdPartialText(sd: Lens): string {
  if (!(sd.status === 'ok' && sd.staticCompared === false)) return '';
  const names = (Array.isArray(sd.notMeasured) ? sd.notMeasured : [])
    .map((k: string) => SD_NOT_MEASURED[k])
    .filter(Boolean);
  if (!names.length) return '';
  return (
    'The no-JavaScript fetch was refused, so only the rendered page was checked. ' +
    'Not measured: whether the ' +
    names.join(', ') +
    ' in the served HTML match the page after JavaScript.'
  );
}

function sdVerdict(sd: Lens): string {
  if (sd.verdict === 'unknown') {
    return sdPartialText(sd)
      ? 'The no-JavaScript fetch was refused, so only the rendered page was checked and it showed no defect; the comparison with the served HTML did not run.'
      : 'We could not render this page, so we could not check its head.';
  }
  if (sd.verdict === 'clean' && sd.staticCompared === false) {
    return 'No defects found on the rendered page; the comparison with the served HTML did not run. One page only; not a ranking assessment.';
  }
  if (sd.verdict === 'clean') {
    return 'No defects found: the canonical, title, meta description, hreflang and structured data in the served HTML match the page after JavaScript. One page only; not a ranking assessment.';
  }
  const counts = sd.counts || {};
  return (
    plural(counts.total ?? 0, 'issue') +
    ': ' +
    sdFiredGroups(sd)
      .map((g) => g[1] + ' ' + counts[g[0]])
      .join(', ') +
    '. One page only; not a ranking assessment.'
  );
}

// ---- Response exposure --------------------------------------------------
// The wording follows the scanner's own response-exposure line.

const EOL_PRODUCT: Record<string, string> = {
  drupal: 'Drupal',
  php: 'PHP',
  apache: 'Apache',
  'microsoft-iis': 'IIS',
  joomla: 'Joomla',
};
const MISSING_HEADER: Record<string, string> = {
  hsts: 'HSTS',
  nosniff: 'nosniff',
  framing: 'framing',
};

const MPS_PRODUCT = /^(mod[_-]?pagespeed|modpagespeed)$/i;

/**
 * The detected mod_pagespeed versions, one per major.minor line: the full
 * version from a version tell wins over the line named by an end-of-life entry.
 */
function mpsVersions(rx: Lens): string[] {
  const byLine = new Map<string, string>();
  const add = (v: unknown) => {
    const p = parseVersion(v);
    if (!p || typeof v !== 'string') return;
    const key = p.major + '.' + p.minor;
    const had = byLine.get(key);
    if (!had || v.trim().length > had.length) byLine.set(key, v.trim());
  };
  for (const t of Array.isArray(rx.tells) ? rx.tells : []) {
    if (t && MPS_PRODUCT.test(String(t.product))) add(t.version);
  }
  for (const e of Array.isArray(rx.endOfLife) ? rx.endOfLife : []) {
    if (e && MPS_PRODUCT.test(String(e.product))) add(e.line);
  }
  return [...byLine.values()];
}

function rxVerdict(rx: Lens): string {
  const nm: string[] = Array.isArray(rx.notMeasured) ? rx.notMeasured : [];
  const notChecked = nm.length
    ? ' Not checked: ' +
      nm
        .map((k) =>
          k === 'static-html' ? 'the HTML as served without JavaScript' : 'the response headers',
        )
        .join(', ') +
      '.'
    : '';
  if (rx.verdict === 'unknown') {
    return 'We could not read enough of this response to say.' + notChecked;
  }
  const leaks = Array.isArray(rx.leaks) ? rx.leaks.length : 0;
  // Only a version the scan can read counts: the generic header value `on` means
  // "active, version not disclosed" and is never reported as a version.
  const tells = (Array.isArray(rx.tells) ? rx.tells : []).filter(
    (t: Lens) => t && typeof t.version === 'string' && /^\d+(\.\d+)*$/.test(t.version),
  ).length;
  const list: Lens[] = Array.isArray(rx.endOfLife) ? rx.endOfLife : [];
  const lifecycle = mpsVersions(rx)
    .map((v) => lifecycleSentence(v))
    .filter(Boolean)
    .map((t) => ' ' + t)
    .join('');
  const eol = [
    ...list
      .filter((e) => e.basis === 'eol-date' && EOL_PRODUCT[e.product])
      .map((e) => EOL_PRODUCT[e.product] + ' ' + e.line),
    // A no-fixes entry with no parsable version keeps the bare phrase; with a
    // version, the lifecycle sentence below says it.
    ...(list.some((e) => e.basis === 'no-fixes') && !lifecycle.includes('no longer receives fixes')
      ? ['mod_pagespeed (no longer receives fixes)']
      : []),
  ];
  const missing = (Array.isArray(rx.headers?.missing) ? rx.headers.missing : [])
    .map((m: string) => MISSING_HEADER[m] || m)
    .join(', ');
  return (
    plural(leaks, 'error-output pattern') +
    ' · ' +
    plural(tells, 'version tell') +
    ' · end of life: ' +
    (eol.length ? eol.join(', ') : 'none seen') +
    ' · missing: ' +
    (missing || 'none') +
    '.' +
    notChecked +
    lifecycle +
    ' One page only; not a security assessment.'
  );
}

export const RX_BUILDING =
  'A response-firewall pack for mod_pagespeed is planned. It would mask leaked stack traces, strip version tells and add missing security headers in the response, on your own server. No pack ships today.';

// ---- Rows ----------------------------------------------------------------

function notMeasuredRow(key: RiskRow['key'], lens: Lens): RiskRow {
  const fallback = lens.status === 'error' ? ERROR_NOTE : DEFAULT_NOTE;
  const note = typeof lens.note === 'string' && lens.note.trim() ? lens.note.trim() : fallback;
  return {
    key,
    name: NAMES[key],
    state: 'none',
    chip: 'Not measured',
    sentence: note,
    detail: '',
    building: [],
    link: null,
  };
}

function buildingFor(
  key: RiskRow['key'],
  lens: Lens,
  platform: unknown,
): { building: string[]; link: RiskLink | null } {
  switch (key) {
    case 'preConsentLeak':
      return consentEnforcementCtaApplies(lens, platform)
        ? {
            building: [
              'Consent enforcement at the origin is planned: third-party tags would stay inert at the server until the consent cookie grants them.',
            ],
            link: {
              href: '/platform/consent/',
              label: 'Consent enforcement at the origin →',
              event: 'airead-cta-consent',
            },
          }
        : { building: [], link: null };
    case 'scriptInventory': {
      const building: string[] = [];
      if (pageIntegrityCtaApplies(lens, platform)) {
        building.push(
          'Payment-page script integrity, a pack that would enforce an allowlist of approved scripts at your origin, is planned.',
        );
      }
      if (thirdPartyFreezeCtaApplies(lens, platform)) {
        const unpinned = lens.totals.thirdPartyExternal - lens.totals.thirdPartyWithIntegrity;
        building.push(
          plural(unpinned, 'third-party script') +
            (unpinned === 1
              ? ' on this page loads without an integrity hash, so its content can change upstream without notice.'
              : ' on this page load without an integrity hash, so their content can change upstream without notice.') +
            (unpinned === 1
              ? ' A third-party freeze pack is planned: it would serve that script at a version you approved until you accept the update.'
              : ' A third-party freeze pack is planned: it would serve those scripts at a version you approved until you accept the update.'),
        );
      }
      return building.length
        ? {
            building,
            link: {
              href: '/platform/',
              label: 'Transform packs →',
              event: 'airead-cta-page-integrity',
            },
          }
        : { building: [], link: null };
    }
    case 'seoDefects':
      return edgeSeoCtaApplies(lens, platform)
        ? {
            building: [
              'Issues like these are the kind a rule on the server can correct in the HTML it serves, without a CMS release. The free edge-SEO pack for mod_pagespeed is a template for writing those rules, starting in report mode.',
            ],
            link: {
              href: '/platform/edge-seo/',
              label: 'Edge SEO at the origin →',
              event: 'airead-cta-edge-seo',
            },
          }
        : { building: [], link: null };
    case 'responseExposure':
      return responseFirewallCtaApplies(lens, platform)
        ? {
            building: [RX_BUILDING],
            link: {
              href: '/platform/#pack-response-firewall',
              label: 'Transform packs: tell me when it ships →',
              event: 'airead-cta-response-firewall',
            },
          }
        : { building: [], link: null };
  }
}

const VERDICT: Record<RiskRow['key'], (lens: Lens, report?: Record<string, unknown>) => string> = {
  preConsentLeak: pclVerdict,
  scriptInventory: siVerdict,
  seoDefects: sdVerdict,
  responseExposure: rxVerdict,
};

/**
 * The rows to show, in panel order. A missing or disabled lens has no row; a
 * lens that did not run (blocked, error) or could not reach a verdict is a
 * muted not-measured row.
 */
export function riskRows(report: Record<string, unknown> | null | undefined): RiskRow[] {
  const rows: RiskRow[] = [];
  if (!report) return rows;
  for (const { key, flagged } of RISK_LENSES) {
    const lens = report[key] as Lens | null | undefined;
    if (!lens || typeof lens !== 'object' || lens.status === 'disabled') continue;
    if (lens.status !== 'ok') {
      rows.push(notMeasuredRow(key, lens));
      continue;
    }
    if (!lensMeasured(lens)) {
      // ok but no verdict: the lens's own "could not render" sentence.
      rows.push({
        key,
        name: NAMES[key],
        state: 'none',
        chip: 'Not measured',
        sentence: VERDICT[key]({ ...lens, verdict: 'unknown' }),
        detail: '',
        building: [],
        link: null,
      });
      continue;
    }
    const attention = lens.verdict === flagged;
    const { building, link } = buildingFor(key, lens, report.platform);
    rows.push({
      key,
      name: NAMES[key],
      state: attention ? 'attention' : 'clean',
      chip: attention ? 'Attention' : 'Nothing flagged',
      sentence: VERDICT[key](lens, report),
      detail: attention && key === 'scriptInventory' ? siReasonText(lens) : '',
      building,
      link,
    });
  }
  return rows;
}
