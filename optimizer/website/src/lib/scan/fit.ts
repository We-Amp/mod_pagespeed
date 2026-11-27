// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Whether the server-side parts of a result apply to the scanned site, and the
// line that says so at the top of the result. The scan names a hosted platform
// or it names nothing: it never says a site runs on its own server. Pure.

import { originTopicsApply } from './demand.mjs';
import { guidanceFor } from '../../data/platform-guidance';

interface Platform {
  id?: unknown;
  name?: unknown;
  originControl?: unknown;
}
type Report = Record<string, unknown> | null | undefined;

function platformOf(report: Report): Platform | null {
  const p = report?.platform;
  return p && typeof p === 'object' ? (p as Platform) : null;
}

/**
 * The one test for everything that needs a web server the visitor runs: the
 * gated topics, the audit, the download row, the "planned" sentences and the
 * configuration. False only when the scan found a hosted platform whose
 * owners have no control of the server.
 */
export function serverOffersApply(report: Report): boolean {
  return originTopicsApply(platformOf(report));
}

/** The longest platform name shown; a longer one is cut. */
export const MAX_PLATFORM_NAME = 60;

/** The platform's display name, or an empty string when the scan gave none. */
export function platformName(report: Report): string {
  const name = platformOf(report)?.name;
  return typeof name === 'string' ? name.trim().slice(0, MAX_PLATFORM_NAME).trim() : '';
}

export const FIT_LINE =
  'The fixes and offers below that use mod_pagespeed apply only if you run the web server yourself.';

/** One help page of the platform, checked against the vendor's own documentation. */
export interface FitLink {
  label: string;
  href: string;
}

export interface Fit {
  /** False for a hosted platform: nothing server-side is offered. */
  offers: boolean;
  /** The notice paragraph (hosted) or the muted line; empty when there is nothing to say. */
  text: string;
  /** What introduces the help links; empty when there are none. */
  linksLead: string;
  /** The platform's own help pages for the settings the findings are about. */
  links: FitLink[];
  /** One short sentence for the live region; empty when nothing is read out. */
  spoken: string;
}

/**
 * What the top of a result says about fit. Without a report (the scan failed)
 * it says nothing: the muted line belongs to a result, whose audit line and
 * topics are the server offers it is about.
 */
export function fitFor(report: Report): Fit {
  const none = { linksLead: '', links: [], spoken: '' };
  if (!report) return { offers: true, text: '', ...none };
  if (serverOffersApply(report)) return { offers: true, text: FIT_LINE, ...none };
  const name = platformName(report);
  const runsOn = name
    ? `This site runs on ${name}, a hosted platform`
    : 'This site runs on a hosted platform';
  const text = `${runsOn}. mod_pagespeed installs on a web server you run yourself, so it cannot be used for this site. The findings below still apply.`;
  const spoken = `${runsOn}; mod_pagespeed cannot be used here. The findings still apply.`;
  const links = (guidanceFor(platformOf(report)?.id)?.actions ?? []).map((a) => ({
    label: a.sourceLabel,
    href: a.source,
  }));
  return {
    offers: false,
    text,
    linksLead: links.length ? `${name || 'Platform'} help pages:` : '',
    links,
    spoken,
  };
}
