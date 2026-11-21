// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { serverOffersApply } from './fit';
import {
  MAX_STEPS,
  NOTHING_MEASURED,
  NOTHING_URGENT,
  NOTHING_URGENT_PARTIAL,
  READABLE_LINE,
  READABLE_LINE_SUBPAGE,
  TAG,
  emptyLine,
  nextSteps,
  showLabel,
} from './next-steps';

const fixture = (name: string) =>
  JSON.parse(
    readFileSync(
      new URL(`../../../tests/fixtures/scan/report-${name}.json`, import.meta.url),
      'utf8',
    ),
  ).report;

// The hand-made fixtures and three samples shaped like real scan results: a
// site on a hosted builder, a hand-built site and a product site.
const FIXTURES = [
  'full',
  'hosted',
  'unknown',
  'clean',
  'blocked',
  'sample-builder',
  'sample-static',
  'sample-product',
];

const POOR = { mobile: 40, desktop: 95 };
const GOOD = { mobile: 95, desktop: 99 };
const CONTENT =
  'Put this page’s main text in the HTML the server sends, so it does not depend on JavaScript.';
const CONTENT_HOSTED =
  'Most of this page’s text only appears after JavaScript runs. On Wix you cannot change how pages are served; check that the main text is in standard text blocks, not in an embedded app or widget.';
const TITLE_CHANGED =
  'JavaScript changes this page’s title after it loads. Put the final one in the HTML the server sends.';
const TITLE_HOSTED =
  'An app or script changes this page’s title after it loads. Set the title in the page’s SEO settings and remove the script that overwrites it.';
const SPEED = 'Speed is poor on mobile. The Speed panel lists what PageSpeed Insights flagged.';
const SPEED_HOSTED =
  'Speed is poor on mobile. On Wix the parts you control are images, fonts and added apps.';
const A11Y_FULL =
  'The scan found 7 accessibility issues: Images must have alternative text; Elements must meet minimum color contrast ratio thresholds; <html> element must have a lang attribute; and 4 more. These are automated checks, not an accessibility audit.';
const A11Y_BUILDER =
  'The scan found 2 accessibility issues: Elements must meet minimum color contrast ratio thresholds; Heading levels should only increase by one. These are automated checks, not an accessibility audit.';
const STRUCTURED =
  'Add structured data that says what this page is: your organization, a product, an article or a list of questions and answers.';
const pairs = (r: ReturnType<typeof nextSteps>) => r.steps.map((s) => [s.text, s.tag]);

describe('nextSteps on a hosted platform', () => {
  it('never asks for a developer, for server access or for a change to what the server sends', () => {
    const hostedCopies = FIXTURES.map((name) => {
      const report = fixture(name);
      report.platform = { id: 'wix', name: 'Wix', originControl: 'none' };
      return report;
    });
    const reports = [...FIXTURES.map(fixture), ...hostedCopies];
    let hosted = 0;
    for (const report of reports) {
      if (serverOffersApply(report)) continue;
      hosted += 1;
      for (const speed of [POOR, GOOD, null]) {
        const { steps, total } = nextSteps(report, speed);
        expect(total).toBeGreaterThanOrEqual(steps.length);
        for (const step of steps) {
          expect([TAG.editor, TAG.settings]).toContain(step.tag);
          expect(step.text).not.toMatch(/the server sends|needs a developer|server access/);
        }
      }
    }
    // The hosted fixture, the hosted sample, and a hosted copy of every report.
    expect(hosted).toBe(2 + FIXTURES.length);
  });

  it('uses the CMS tag, never the site editor, when no hosted platform was found', () => {
    for (const name of FIXTURES) {
      const report = fixture(name);
      if (!serverOffersApply(report)) continue;
      for (const step of nextSteps(report, POOR).steps) {
        expect([TAG.cms, TAG.server, TAG.developer]).toContain(step.tag);
      }
    }
  });
});

describe('nextSteps over the fixtures', () => {
  it('full report: content, the title a script changes, then accessibility', () => {
    const r = nextSteps(fixture('full'), GOOD);
    expect(pairs(r)).toEqual([
      [CONTENT, 'needs a developer'],
      [TITLE_CHANGED, 'needs a developer'],
      [A11Y_FULL, 'in your CMS or page template'],
    ]);
    expect(r.total).toBe(3);
    expect(r.readable).toBe(false);
    expect(r.steps.map((s) => [s.pillar, s.target])).toEqual([
      ['airead', '[data-scan-cat="Content fidelity"]'],
      ['risk', '[data-risk-row="seoDefects"]'],
      ['airead', '[data-scan-a11y]'],
    ]);
  });

  it('a poor speed result comes last: it is counted, and listed only when there is room', () => {
    const before = nextSteps(fixture('full'), null);
    const after = nextSteps(fixture('full'), POOR);
    // The steps already shown do not move when Speed lands.
    expect(after.steps).toEqual(before.steps);
    expect(after.total).toBe(before.total + 1);
    const room = nextSteps(fixture('sample-builder'), POOR);
    expect(pairs(room)).toEqual([
      [A11Y_BUILDER, 'in your site editor'],
      [SPEED_HOSTED, 'in your site editor'],
    ]);
    expect(room.steps.slice(0, 1)).toEqual(nextSteps(fixture('sample-builder'), null).steps);
  });

  it('hosted report: the same findings, worded for what the owner can change there', () => {
    const r = nextSteps(fixture('hosted'), GOOD);
    expect(pairs(r)).toEqual([
      [CONTENT_HOSTED, 'in your site editor'],
      [TITLE_HOSTED, 'in your site settings'],
      [A11Y_FULL, 'in your site editor'],
    ]);
  });

  it('hosted report with blocking robots.txt rules: the step is tagged for the site settings', () => {
    const report = fixture('hosted');
    report.access.crawlerRules.training.blocked = ['GPTBot'];
    expect(nextSteps(report, GOOD).steps[1]).toMatchObject({
      id: 'robots',
      text: 'Your robots.txt blocks GPTBot from the home page. Remove that rule if you want AI crawlers to read the site.',
      tag: 'in your site settings',
      target: '[data-scan-cat="Crawler access"]',
    });
  });

  it('unknown platform: the robots.txt step names the blocked crawlers and needs server access', () => {
    const r = nextSteps(fixture('unknown'), GOOD);
    expect(pairs(r)).toEqual([
      [CONTENT, 'needs a developer'],
      [
        'Your robots.txt blocks GPTBot, ClaudeBot and OAI-SearchBot from the home page. Remove those rules if you want AI crawlers to read the site.',
        'needs server access',
      ],
      [TITLE_CHANGED, 'needs a developer'],
    ]);
    expect(r.total).toBe(4);
  });

  it('clean report: no steps, except for a poor speed result', () => {
    expect(nextSteps(fixture('clean'), GOOD)).toMatchObject({ steps: [], total: 0 });
    expect(nextSteps(fixture('clean'), null)).toMatchObject({ steps: [], total: 0 });
    expect(pairs(nextSteps(fixture('clean'), POOR))).toEqual([[SPEED, 'needs a developer']]);
  });

  it('blocked report: the missing text first, and the older list of blocked crawlers', () => {
    const r = nextSteps(fixture('blocked'), null);
    expect(pairs(r)).toEqual([
      [
        'The scan found almost no text on this page, even after running JavaScript. Check that the page is public and does not block automated visitors, then scan it again.',
        'needs server access',
      ],
      [
        'Your robots.txt blocks 9 AI crawlers. Remove those rules if you want AI crawlers to read the site.',
        'needs server access',
      ],
    ]);
    expect(r.unreadable).toBe(true);
    expect(r.readable).toBe(false);
  });

  it('site on a hosted builder (sample): readable, with one accessibility step', () => {
    const r = nextSteps(fixture('sample-builder'), GOOD);
    expect(pairs(r)).toEqual([[A11Y_BUILDER, 'in your site editor']]);
    expect(r.readable).toBe(true);
    expect(r.unreadable).toBe(false);
  });

  it('hand-built site (sample): one step for the missing head tags', () => {
    const r = nextSteps(fixture('sample-static'), GOOD);
    expect(pairs(r)).toEqual([
      [
        'This page has no meta description and no canonical link. Add them.',
        'in your CMS or page template',
      ],
      [STRUCTURED, 'in your CMS or page template'],
      [
        'The scan found 4 accessibility issues: <html> element must have a lang attribute; All page content should be contained by landmarks; Heading levels should only increase by one; and 1 more. These are automated checks, not an accessibility audit.',
        'in your CMS or page template',
      ],
    ]);
    expect(r.total).toBe(3);
    expect(r.readable).toBe(true);
    expect(r.steps[0]).toMatchObject({ id: 'head', pillar: 'risk' });
  });

  it('product site (sample): readable and nothing to do', () => {
    expect(nextSteps(fixture('sample-product'), GOOD)).toEqual({
      steps: [],
      total: 0,
      unreadable: false,
      readable: true,
      readableLine: READABLE_LINE,
    });
  });

  it('never lists more than three steps', () => {
    for (const name of FIXTURES) {
      expect(nextSteps(fixture(name), POOR).steps.length).toBeLessThanOrEqual(MAX_STEPS);
    }
  });
});

describe('the statement that AI crawlers can read the page', () => {
  it('says only what the report shows', () => {
    expect(READABLE_LINE).toBe(
      'AI crawlers can read this page: the text is in the HTML and robots.txt blocks none of them.',
    );
    expect(READABLE_LINE).not.toMatch(/cite|rank|mention|answer/i);
  });

  it('needs the text in the HTML and a robots.txt reading that blocks nothing', () => {
    const base = () => fixture('sample-product');
    expect(nextSteps(base()).readable).toBe(true);
    let r = base();
    r.categories['Content fidelity'] = { score: 26, max: 30, flag: null };
    expect(nextSteps(r).readable).toBe(false);
    r = base();
    r.access.crawlerRules.answer.blocked = ['PerplexityBot'];
    expect(nextSteps(r).readable).toBe(false);
    r = base();
    r.access.crawlerRules.wildcardBlocksAll = true;
    expect(nextSteps(r).readable).toBe(false);
    r = base();
    r.access.crawlerRules = { parsed: false, reason: 'missing' };
    expect(nextSteps(r).readable).toBe(false);
    r = base();
    delete r.access.crawlerRules;
    expect(nextSteps(r).readable).toBe(false);
    expect(nextSteps(null, GOOD).readable).toBe(false);
    expect(nextSteps(null, GOOD).readableLine).toBe('');
  });

  it('is not made when robots.txt blocks a crawler only the older list checks', () => {
    const r = fixture('sample-product');
    r.access.aiBotsBlocked = ['Bytespider'];
    const steps = nextSteps(r);
    expect(steps.readable).toBe(false);
    expect(steps.readableLine).toBe('');
  });

  it('is not made when part of robots.txt was not read', () => {
    const r = fixture('sample-product');
    r.access.crawlerRules.truncated = true;
    expect(nextSteps(r).readable).toBe(false);
    expect(nextSteps(r).readableLine).toBe('');
  });

  it('speaks of robots.txt for this page only when the home page was scanned', () => {
    const r = fixture('sample-product');
    expect(new URL(r.url).pathname).toBe('/');
    expect(nextSteps(r).readableLine).toBe(
      'AI crawlers can read this page: the text is in the HTML and robots.txt blocks none of them.',
    );
    for (const url of [`${r.url}docs/install/`, `${r.url}pricing`, 'not a url', undefined]) {
      r.url = url;
      const steps = nextSteps(r);
      expect(steps.readable).toBe(true);
      expect(steps.readableLine).toBe(
        'AI crawlers can read this page’s text: it is in the HTML, and robots.txt lets them fetch the site’s home page.',
      );
      expect(steps.readableLine).toBe(READABLE_LINE_SUBPAGE);
    }
    r.url = 'https://www.example.com/?utm=1';
    expect(nextSteps(r).readableLine).toBe(READABLE_LINE);
  });
});

describe('nextSteps conditions', () => {
  const base = () => fixture('clean');

  it('gives no steps without a report, except a poor speed result', () => {
    expect(nextSteps(null, null)).toMatchObject({ steps: [], total: 0 });
    expect(nextSteps(undefined, GOOD)).toMatchObject({ steps: [], total: 0 });
    expect(pairs(nextSteps(null, POOR))).toEqual([[SPEED, 'needs a developer']]);
    expect(nextSteps(null, { mobile: null, desktop: 30 }).steps[0].text).toBe(
      'Speed is poor on desktop. The Speed panel lists what PageSpeed Insights flagged.',
    );
  });

  it('counts a weak content score without the flag, and leaves a strong one alone', () => {
    const r = base();
    r.categories['Content fidelity'] = { score: 14, max: 30, flag: null };
    expect(nextSteps(r).steps[0]).toMatchObject({ id: 'content', text: CONTENT });
    r.categories['Content fidelity'] = { score: 15, max: 30, flag: null };
    expect(nextSteps(r).total).toBe(0);
  });

  it('tags missing head tags for the site settings on a hosted platform', () => {
    const r = fixture('sample-static');
    r.platform = { id: 'wix', name: 'Wix', originControl: 'none' };
    expect(nextSteps(r, GOOD).steps[0]).toMatchObject({
      id: 'head',
      text: 'This page has no meta description and no canonical link. Add them.',
      tag: 'in your site settings',
    });
  });

  it('names a hosted platform without a name as "this platform"', () => {
    const r = base();
    r.platform = { id: null, name: null, originControl: 'none' };
    r.categories['Content fidelity'] = { score: 3, max: 30, flag: 'csr-gap' };
    expect(nextSteps(r, POOR).steps.map((s) => s.text)).toEqual([
      'Most of this page’s text only appears after JavaScript runs. On this platform you cannot change how pages are served; check that the main text is in standard text blocks, not in an embedded app or widget.',
      'Speed is poor on mobile. On this platform the parts you control are images, fonts and added apps.',
    ]);
  });

  it('counts crawlers past three and words the wildcard rule on its own', () => {
    const r = base();
    r.access.crawlerRules = {
      parsed: true,
      training: { allowed: [], blocked: ['GPTBot', 'ClaudeBot', 'CCBot'] },
      answer: { allowed: [], blocked: ['GPTBot', 'OAI-SearchBot'] },
      wildcardBlocksAll: false,
    };
    expect(nextSteps(r).steps[0].text).toBe(
      'Your robots.txt blocks 4 AI crawlers from the home page. Remove those rules if you want AI crawlers to read the site.',
    );
    r.access.crawlerRules.wildcardBlocksAll = true;
    expect(nextSteps(r).steps[0].text).toBe(
      'Your robots.txt blocks every crawler that has no rule of its own from the home page. Remove that rule if you want AI crawlers to read the site.',
    );
    // A file the scan could not parse, or one that blocks nothing, gives no step.
    r.access.crawlerRules = { parsed: false, reason: 'missing' };
    r.access.aiBotsBlocked = ['GPTBot'];
    expect(nextSteps(r).total).toBe(0);
    r.access.crawlerRules = {
      parsed: true,
      training: { allowed: ['GPTBot'], blocked: [] },
      answer: { allowed: [], blocked: [] },
      wildcardBlocksAll: false,
    };
    expect(nextSteps(r).total).toBe(0);
  });

  it('falls back to the older list only when the result has no robots.txt reading', () => {
    const r = base();
    delete r.access.crawlerRules;
    r.access.aiBotsBlocked = ['GPTBot', 'x'.repeat(200)];
    expect(nextSteps(r).steps[0].text).toBe(
      `Your robots.txt blocks GPTBot and ${'x'.repeat(40)}. Remove those rules if you want AI crawlers to read the site.`,
    );
    r.access.aiBotsBlocked = [];
    expect(nextSteps(r).total).toBe(0);
  });

  it('asks for structured data only when that category is under half', () => {
    const r = base();
    r.categories['Structured data'] = { score: 9, max: 20 };
    expect(nextSteps(r).steps[0]).toMatchObject({
      id: 'structured-data',
      text: STRUCTURED,
      tag: 'in your CMS or page template',
    });
    r.categories['Structured data'] = { score: 10, max: 20 };
    expect(nextSteps(r).total).toBe(0);
  });

  it('names what the head lacks in one step; author and date only on an article', () => {
    const r = base();
    r.categories['Citation readiness'] = { score: 2, max: 10 };
    r.signals = {
      jsonLdTypes: ['Organization'],
      title: 'T',
      metaDescription: false,
      canonical: false,
      author: false,
      published: false,
      modified: false,
    };
    expect(nextSteps(r).steps).toEqual([
      {
        id: 'head',
        text: 'This page has no meta description and no canonical link. Add them.',
        tag: 'in your CMS or page template',
        pillar: 'airead',
        target: '[data-scan-cat="Citation readiness"]',
      },
    ]);
    r.signals.jsonLdTypes = ['NewsArticle'];
    r.signals.title = '';
    expect(nextSteps(r).steps[0].text).toBe(
      'This page has no title, no meta description, no canonical link, no author name and no publication date. Add them.',
    );
    r.signals.title = 'T';
    r.signals.metaDescription = true;
    r.signals.canonical = true;
    r.signals.modified = true;
    expect(nextSteps(r).steps[0].text).toBe('This page has no author name. Add it.');
    // Nothing to name: no step, however low the score.
    r.signals.author = true;
    expect(nextSteps(r).total).toBe(0);
    // An attributable page is left alone.
    r.signals.author = false;
    r.categories['Citation readiness'] = { score: 7, max: 10 };
    expect(nextSteps(r).total).toBe(0);
  });

  it('merges a tag the SEO check found missing with what the citation signals miss', () => {
    const r = base();
    r.seoDefects = {
      status: 'ok',
      verdict: 'attention',
      defects: ['description-missing'],
      counts: { total: 1 },
    };
    r.categories['Citation readiness'] = { score: 2, max: 10 };
    r.signals = { jsonLdTypes: [], title: 'T', metaDescription: false, canonical: false };
    expect(nextSteps(r).steps).toEqual([
      {
        id: 'head',
        text: 'This page has no meta description and no canonical link. Add them.',
        tag: 'in your CMS or page template',
        pillar: 'risk',
        target: '[data-risk-row="seoDefects"]',
      },
    ]);
  });

  it('words the top SEO defect a script causes, and counts the ones it has no words for', () => {
    const r = base();
    const seo = (defects: string[]) => {
      r.seoDefects = {
        status: 'ok',
        verdict: 'attention',
        defects,
        counts: { total: defects.length },
      };
      return nextSteps(r).steps.find((s) => s.id === 'seo');
    };
    expect(seo(['canonical-js-only', 'title-js-only'])).toMatchObject({
      text: 'This page’s title is only added by JavaScript. Put it in the HTML the server sends.',
      tag: 'needs a developer',
    });
    expect(seo(['canonical-multiple'])).toMatchObject({
      text: 'This page has more than one canonical link. Keep one.',
      tag: 'in your CMS or page template',
    });
    expect(seo(['hreflang-invalid', 'jsonld-invalid', 'title-missing'])).toMatchObject({
      text: 'The scan flagged 2 SEO issues in the head of this page. The Risk & SEO panel lists them.',
      tag: 'needs a developer',
    });
    // A missing tag alone is the head step's, not this one's.
    expect(seo(['description-missing'])).toBeUndefined();
    expect(nextSteps(r).steps.map((s) => s.id)).toEqual(['head']);
    // A lens that did not run gives no step.
    r.seoDefects = { status: 'blocked', verdict: 'unknown', defects: ['title-missing'] };
    expect(nextSteps(r).total).toBe(0);
  });

  it('lists accessibility rules by impact, then by elements, each cut to a fixed length', () => {
    const r = base();
    r.accessibility = {
      available: true,
      detail: {
        total: 1,
        topRules: [{ id: 'a', impact: 'minor', nodes: 1, help: `${'Long rule '.repeat(30)}.` }],
      },
    };
    const one = nextSteps(r).steps[0].text;
    expect(one).toMatch(/^The scan found 1 accessibility issue: Long rule /);
    expect(one.length).toBeLessThan(180);
    r.accessibility.detail = {
      total: 3,
      topRules: [
        { id: 'a', impact: 'moderate', nodes: 9, help: 'Moderate, many' },
        { id: 'b', impact: 'critical', nodes: 1, help: 'Critical, one' },
        { id: 'c', impact: 'moderate', nodes: 2, help: 'Moderate, few' },
      ],
    };
    expect(nextSteps(r).steps[0].text).toBe(
      'The scan found 3 accessibility issues: Critical, one; Moderate, many; Moderate, few. These are automated checks, not an accessibility audit.',
    );
    r.accessibility.detail = { total: 4, topRules: [] };
    expect(nextSteps(r).steps[0].text).toBe(
      'The scan found 4 accessibility issues. These are automated checks, not an accessibility audit.',
    );
    r.accessibility = { available: false, detail: { total: 4 } };
    expect(nextSteps(r).total).toBe(0);
  });
});

describe('emptyLine and showLabel', () => {
  it('says nothing is urgent only when no tile asks for attention', () => {
    expect(emptyLine([])).toBe('Nothing urgent on this page. Open a panel for the details.');
    expect(emptyLine([])).toBe(NOTHING_URGENT);
    expect(emptyLine([], 2)).toBe(NOTHING_URGENT_PARTIAL);
    expect(emptyLine([], 0)).toBe(NOTHING_MEASURED);
    expect(emptyLine(['risk'])).toBe(
      'The scan has no single next step for this page. Open the Risk & SEO panel for what it flagged.',
    );
    expect(emptyLine(['airead', 'risk'])).toBe(
      'The scan has no single next step for this page. Open the AI readability and Risk & SEO panels for what it flagged.',
    );
  });
  it('names the panel a step opens', () => {
    expect(showLabel('airead')).toBe('Show in AI readability');
    expect(showLabel('risk')).toBe('Show in Risk & SEO');
    expect(showLabel('speed')).toBe('Show in Speed');
  });
});
