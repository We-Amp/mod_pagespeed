// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// "Your next steps": up to three plain sentences that say what to do about a
// result, most important first, each with who can do it and the panel row it
// comes from. Built only from what the report and the speed scores already
// hold. Pure: no DOM, no network.

import { accessibilityFindings } from './airead';
import { platformName, serverOffersApply } from './fit';
import { plural } from './plural';
import { lensMeasured, speedStatus } from './status';

export type StepPillar = 'speed' | 'airead' | 'risk';

/** Who can act on a step. */
export const TAG = {
  /** A hosted platform: the owner works in the platform's editor and settings. */
  editor: 'in your site editor',
  settings: 'in your site settings',
  /** No hosted platform found: the page comes from a CMS or from templates. */
  cms: 'in your CMS or page template',
  server: 'needs server access',
  developer: 'needs a developer',
} as const;
export type StepTag = (typeof TAG)[keyof typeof TAG];

export interface Step {
  id: string;
  /** What to do, as one or two plain sentences. */
  text: string;
  tag: StepTag;
  /** The tile whose panel holds the finding. */
  pillar: StepPillar;
  /** A selector for the finding's row inside that panel; empty for the panel itself. */
  target: string;
}

export interface NextSteps {
  /** The steps shown: the first MAX_STEPS of everything that qualified. */
  steps: Step[];
  /** How many steps qualified in all. */
  total: number;
  /** True when the scan could read next to nothing of the page. */
  unreadable: boolean;
  /** True when the report supports the statement that AI crawlers can read the page. */
  readable: boolean;
  /** That statement, worded for the page that was scanned; empty when it does not apply. */
  readableLine: string;
}

/** The most steps listed. */
export const MAX_STEPS = 3;
export const STEPS_HEADING = 'Your next steps';
export const NOTHING_URGENT = 'Nothing urgent on this page. Open a panel for the details.';
/** A statement, not a step: shown above the list when the report supports it. */
export const READABLE_LINE =
  'The text of this page is in the HTML, and robots.txt blocks none of the AI crawlers this scan checks.';
/**
 * The same for a page other than the home page: the scan reads robots.txt for
 * the home page only, so the statement says no more than that.
 */
export const READABLE_LINE_SUBPAGE =
  'The text of this page is in the HTML, and robots.txt lets the AI crawlers this scan checks fetch the site’s home page.';

function isHomePage(url: unknown): boolean {
  try {
    return typeof url === 'string' && new URL(url).pathname === '/';
  } catch {
    return false;
  }
}

const PILLAR_NAME: Record<StepPillar, string> = {
  speed: 'Speed',
  airead: 'AI readability',
  risk: 'Risk & SEO',
};
/** The text of the control that opens a step's panel. */
export const showLabel = (pillar: StepPillar): string => `Show in ${PILLAR_NAME[pillar]}`;

export const NOTHING_URGENT_PARTIAL =
  'Nothing urgent in what the scan could measure. Open a panel for the details.';
export const NOTHING_MEASURED =
  'Nothing was measured, so there are no steps yet. Open a panel to try again.';

/**
 * The line shown in place of the list when it is empty. `measured` is how many
 * of the three areas have a result. When a tile still asks for attention the
 * line names its panel instead of saying nothing is urgent.
 */
export function emptyLine(attention: readonly StepPillar[], measured = 3): string {
  if (measured === 0) return NOTHING_MEASURED;
  if (attention.length === 0) return measured < 3 ? NOTHING_URGENT_PARTIAL : NOTHING_URGENT;
  const names = attention.map((p) => PILLAR_NAME[p]);
  return `The scan has no single next step for this page. Open the ${listOf(names)} ${
    names.length === 1 ? 'panel' : 'panels'
  } for what it flagged.`;
}

// The report is scanner output; every field is read defensively.
/* eslint-disable @typescript-eslint/no-explicit-any */
type Report = Record<string, any> | null | undefined;

export interface SpeedScores {
  mobile: number | null;
  desktop: number | null;
}

function listOf(items: readonly string[]): string {
  if (items.length <= 1) return items.join('');
  return `${items.slice(0, -1).join(', ')} and ${items[items.length - 1]}`;
}

/** The longest crawler name shown; a longer one is cut. */
const MAX_NAME = 40;
const strings = (v: unknown): string[] =>
  Array.isArray(v)
    ? v
        .filter((x): x is string => typeof x === 'string' && x !== '')
        .map((x) => x.slice(0, MAX_NAME))
    : [];

const cat = (name: string): string => `[data-scan-cat="${name}"]`;

/** Under this share of its points a category counts as weak. */
const WEAK = 0.5;
/** The scan calls a page attributable from this share of the citation points. */
const ATTRIBUTABLE = 0.7;
/** From this share of the content points the page's text counts as being in the HTML. */
const IN_HTML = 0.9;
const MAX_NAMED_CRAWLERS = 3;

function ratio(c: any): number | null {
  return c && typeof c.score === 'number' && typeof c.max === 'number' && c.max > 0
    ? c.score / c.max
    : null;
}

/** What a result is worded for: a hosted platform, or a site whose owner may run the server. */
interface Site {
  hosted: boolean;
  /** "On Wix" or "On this platform". */
  on: string;
  /** The tag for content and head tags the owner edits. */
  edit: StepTag;
}

function robotsStep(r: Record<string, any>, site: Site): Step | null {
  const cr = r.access?.crawlerRules;
  let text = '';
  if (cr && typeof cr === 'object') {
    if (cr.parsed !== true) return null;
    const blocked = [
      ...new Set([...strings(cr.training?.blocked), ...strings(cr.answer?.blocked)]),
    ];
    if (cr.wildcardBlocksAll === true) {
      text =
        'Your robots.txt blocks every crawler that has no rule of its own from the home page. Remove that rule if you want AI crawlers to read the site.';
    } else if (blocked.length > 0) {
      const who =
        blocked.length <= MAX_NAMED_CRAWLERS ? listOf(blocked) : `${blocked.length} AI crawlers`;
      text = `Your robots.txt blocks ${who} from the home page. Remove ${
        blocked.length === 1 ? 'that rule' : 'those rules'
      } if you want AI crawlers to read the site.`;
    }
  } else {
    // A result without the newer reading still lists the crawlers robots.txt blocks.
    const blocked = [...new Set(strings(r.access?.aiBotsBlocked))];
    if (blocked.length > 0) {
      const who =
        blocked.length <= MAX_NAMED_CRAWLERS ? listOf(blocked) : `${blocked.length} AI crawlers`;
      text = `Your robots.txt blocks ${who}. Remove ${
        blocked.length === 1 ? 'that rule' : 'those rules'
      } if you want AI crawlers to read the site.`;
    }
  }
  if (!text) return null;
  return {
    id: 'robots',
    text,
    tag: site.hosted ? TAG.settings : TAG.server,
    pillar: 'airead',
    target: cat('Crawler access'),
  };
}

const SEO_ROW = '[data-risk-row="seoDefects"]';
// Head tags a script adds or changes, and doubled ones; a missing tag is part
// of the head step below.
const SEO_ORDER = [
  'title-js-only',
  'title-js-changed',
  'description-js-only',
  'canonical-js-only',
  'description-js-changed',
  'canonical-js-changed',
  'title-multiple',
  'description-multiple',
  'canonical-multiple',
];
const SEO_THING: Record<string, string> = {
  title: 'title',
  description: 'meta description',
  canonical: 'canonical link',
};
const HEAD_ORDER = [
  'title',
  'meta description',
  'canonical link',
  'author name',
  'publication date',
];

function seoDefects(r: Record<string, any>): string[] {
  const sd = r.seoDefects;
  return lensMeasured(sd) && sd.verdict === 'attention' ? strings(sd.defects) : [];
}

function seoStep(r: Record<string, any>, site: Site): Step | null {
  const defects = seoDefects(r);
  const top = SEO_ORDER.find((id) => defects.includes(id)) ?? '';
  const base = { id: 'seo', pillar: 'risk' as const, target: SEO_ROW };
  const [group, ...rest] = top.split('-');
  const kind = rest.join('-');
  const thing = SEO_THING[group];
  if (thing && kind === 'multiple') {
    return { ...base, tag: site.edit, text: `This page has more than one ${thing}. Keep one.` };
  }
  if (thing && site.hosted) {
    return {
      ...base,
      tag: TAG.settings,
      text: `An app or script changes this page’s ${thing} after it loads. Set the ${thing} in the page’s SEO settings and remove the script that overwrites it.`,
    };
  }
  if (thing && kind === 'js-only') {
    return {
      ...base,
      tag: TAG.developer,
      text: `This page’s ${thing} is only added by JavaScript. Put it in the HTML the server sends.`,
    };
  }
  if (thing && kind === 'js-changed') {
    return {
      ...base,
      tag: TAG.developer,
      text: `JavaScript changes this page’s ${thing} after it loads. Put the final one in the HTML the server sends.`,
    };
  }
  // Nothing this module words on its own, and no missing tag for the head step.
  const other = defects.filter((id) => !/^(title|description|canonical)-missing$/.test(id));
  if (other.length === 0) return null;
  return {
    ...base,
    tag: site.hosted ? TAG.settings : TAG.developer,
    text: `The scan flagged ${plural(other.length, 'SEO issue')} in the head of this page. The Risk & SEO panel lists them.`,
  };
}

const ARTICLE = /Article|BlogPosting/i;

/**
 * One step for everything the head of the page lacks: the tags the SEO check
 * found missing and what the citation signals miss, each named once.
 */
function headStep(r: Record<string, any>, site: Site): Step | null {
  const missing = new Set<string>();
  const defects = seoDefects(r);
  for (const [group, thing] of Object.entries(SEO_THING)) {
    if (defects.includes(`${group}-missing`)) missing.add(thing);
  }
  const fromSeo = missing.size > 0;
  const share = ratio(r.categories?.['Citation readiness']);
  const s = r.signals;
  if (share !== null && share < ATTRIBUTABLE && s && typeof s === 'object') {
    if (!s.title) missing.add('title');
    if (s.metaDescription === false) missing.add('meta description');
    if (s.canonical === false) missing.add('canonical link');
    // An author and a date belong on an article, so only an article is asked for them.
    if (strings(s.jsonLdTypes).some((t) => ARTICLE.test(t))) {
      if (s.author === false) missing.add('author name');
      if (s.published === false && s.modified === false) missing.add('publication date');
    }
  }
  const items = HEAD_ORDER.filter((t) => missing.has(t));
  if (items.length === 0) return null;
  return {
    id: 'head',
    text: `This page has ${listOf(items.map((t) => `no ${t}`))}. Add ${
      items.length === 1 ? 'it' : 'them'
    }.`,
    // On a hosted platform these fields are in the page's SEO settings.
    tag: site.hosted ? TAG.settings : TAG.cms,
    pillar: fromSeo ? 'risk' : 'airead',
    target: fromSeo ? SEO_ROW : cat('Citation readiness'),
  };
}

function accessibilityStep(r: Record<string, any>, site: Site): Step | null {
  const found = accessibilityFindings(r);
  if (!found) return null;
  const shown = found.rules.slice(0, MAX_STEPS);
  const more = found.total - shown.length;
  const named = shown.map((rule) => rule.help);
  if (named.length > 0 && more > 0) named.push(`and ${more} more`);
  return {
    id: 'accessibility',
    text: `The scan found ${plural(found.total, 'accessibility issue')}${
      named.length ? `: ${named.join('; ')}` : ''
    }. These are automated checks, not an accessibility audit.`,
    tag: site.edit,
    pillar: 'airead',
    target: '[data-scan-a11y]',
  };
}

function speedStep(speed: SpeedScores | null | undefined, site: Site): Step | null {
  if (!speed || speedStatus(speed.mobile, speed.desktop).state !== 'poor') return null;
  const where = typeof speed.mobile === 'number' ? 'mobile' : 'desktop';
  return {
    id: 'speed',
    text: site.hosted
      ? `Speed is poor on ${where}. ${site.on} the parts you control are images, fonts and added apps.`
      : `Speed is poor on ${where}. The Speed panel lists what PageSpeed Insights flagged.`,
    tag: site.hosted ? TAG.editor : TAG.developer,
    pillar: 'speed',
    target: '',
  };
}

/** Whether the report supports saying that AI crawlers can read the page. */
function readableBy(r: Record<string, any>): boolean {
  const share = ratio(r.categories?.['Content fidelity']);
  const cr = r.access?.crawlerRules;
  return (
    share !== null &&
    share >= IN_HTML &&
    !!cr &&
    typeof cr === 'object' &&
    cr.parsed === true &&
    // Part of the file was not read: it may hold a rule the scan did not see.
    cr.truncated !== true &&
    cr.wildcardBlocksAll !== true &&
    // The older list checks a few more crawlers; the statement holds for both.
    strings(r.access?.aiBotsBlocked).length === 0 &&
    strings(cr.training?.blocked).length + strings(cr.answer?.blocked).length === 0
  );
}

/**
 * The steps for a result. `report` is null when the scan gave none and
 * `speed` is null while the speed result is not in. On a hosted platform no
 * step asks for a developer, for server access or for a change to what the
 * server sends: the owner can do none of that there. The speed step always
 * comes last, so a speed result that lands late never moves a step already
 * shown.
 */
export function nextSteps(report: Report, speed?: SpeedScores | null): NextSteps {
  const all: Step[] = [];
  const add = (step: Step | null) => {
    if (step) all.push(step);
  };
  const hosted = !serverOffersApply(report);
  const name = platformName(report);
  const site: Site = {
    hosted,
    on: name ? `On ${name}` : 'On this platform',
    edit: hosted ? TAG.editor : TAG.cms,
  };
  let unreadable = false;
  let readable = false;
  if (report && typeof report === 'object') {
    const fidelity = report.categories?.['Content fidelity'];
    unreadable = fidelity?.flag === 'thin';
    const share = ratio(fidelity);
    if (unreadable) {
      // The scan read next to nothing, so the other rows describe an empty or
      // a blocked page: the one useful step is to find out why.
      add({
        id: 'no-content',
        text: 'The scan found almost no text on this page, even after running JavaScript. Check that the page is public and does not block automated visitors, then scan it again.',
        tag: hosted ? TAG.settings : TAG.server,
        pillar: 'airead',
        target: cat('Content fidelity'),
      });
    } else if (fidelity?.flag === 'csr-gap' || (share !== null && share < WEAK)) {
      add({
        id: 'content',
        text: hosted
          ? `Most of this page’s text only appears after JavaScript runs. ${site.on} you cannot change how pages are served; check that the main text is in standard text blocks, not in an embedded app or widget.`
          : 'Put this page’s main text in the HTML the server sends, so it does not depend on JavaScript.',
        tag: hosted ? TAG.editor : TAG.developer,
        pillar: 'airead',
        target: cat('Content fidelity'),
      });
    }
    add(robotsStep(report, site));
    readable = all.length === 0 && readableBy(report);
    if (!unreadable) {
      add(seoStep(report, site));
      add(headStep(report, site));
      const structured = ratio(report.categories?.['Structured data']);
      if (structured !== null && structured < WEAK) {
        add({
          id: 'structured-data',
          text: 'Add structured data that says what this page is: your organization, a product, an article or a list of questions and answers.',
          tag: site.edit,
          pillar: 'airead',
          target: cat('Structured data'),
        });
      }
      add(accessibilityStep(report, site));
    }
  }
  add(speedStep(speed, site));
  // Belt and braces: nothing a hosted owner cannot do is ever listed.
  const kept = all.filter((s) => !hosted || (s.tag !== TAG.server && s.tag !== TAG.developer));
  const readableLine = !readable
    ? ''
    : isHomePage(report?.url)
      ? READABLE_LINE
      : READABLE_LINE_SUBPAGE;
  return {
    steps: kept.slice(0, MAX_STEPS),
    total: kept.length,
    unreadable,
    readable,
    readableLine,
  };
}
