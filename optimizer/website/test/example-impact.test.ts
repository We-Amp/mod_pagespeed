// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The measured sentences on /examples/<slug>/ and the examples listed on the
// filter topic pages. The sentences restate the impact tiles in words, so every
// number in them must be one the tiles show, taken from examples-data.json.

import { afterEach, beforeEach, describe, it, expect } from 'vitest';
import { EXAMPLES, examplesApplying } from '../src/data/examples';
import {
  browserOnlyNote,
  captureMissedResources,
  growthClause,
  exampleImpact,
  headlineMetric,
  measuredSentences,
  notAppliedNote,
  demonstrates,
  tradeOffClauses,
} from '../src/lib/example-impact';
import filtersData from '../src/data/reference/filters.json';
import examplesData from '../src/data/examples-data.json';

describe('measured sentences', () => {
  it('states only the numbers the impact tiles show, before any trade-off', () => {
    for (const ex of EXAMPLES) {
      const impact = exampleImpact(ex.slug)!;
      const text = measuredSentences(impact)
        .join(' ')
        .split(/[,;] but /)[0];
      const shown = impact.tiles.flatMap((t) => [t.before, t.after, t.pill.replace('−', '')]);
      for (const n of text.match(/\d+(?:\.\d+)?(?: [KM]?B|%)?/g) ?? []) {
        expect(shown, `${ex.slug}: ${n}`).toContain(n);
      }
    }
  });

  it('is empty when there is no reduction to state', () => {
    for (const ex of EXAMPLES) {
      const impact = exampleImpact(ex.slug)!;
      if (impact.tiles.length === 0) expect(measuredSentences(impact), ex.slug).toEqual([]);
    }
  });

  it('gives every example with a reduction its own text', () => {
    const texts = EXAMPLES.map((ex) => measuredSentences(exampleImpact(ex.slug)!).join(' ')).filter(
      Boolean,
    );
    expect(texts.length).toBeGreaterThan(20);
    expect(new Set(texts).size).toBe(texts.length);
  });

  it('reads the combine_css capture', () => {
    expect(measuredSentences(exampleImpact('combine_css')!)[0]).toBe(
      'Measured on the Combine CSS demo page, original against optimized: 5 HTTP requests become 2; the total weight (HTML plus the files it loads) falls from 4.7 KB to 4.6 KB, 3% smaller; and the HTML document falls from 435 B to 305 B, 30% smaller.',
    );
  });

  it('claims no request or weight saving when the capture missed a resource', () => {
    // remove_quotes unquotes an <img src>, which the capture then did not count.
    const text = measuredSentences(exampleImpact('remove_quotes')!).join(' ');
    expect(text).not.toMatch(/request|total weight/);
    expect(text).toMatch(/: the HTML document falls from 189 B to 179 B, 5% smaller\.$/);
  });
});

describe('a saving that came with a cost', () => {
  it('never states a saving alone while the requests or the total weight went up', () => {
    for (const ex of EXAMPLES) {
      const impact = exampleImpact(ex.slug)!;
      const text = measuredSentences(impact).join(' ');
      if (!text) continue;
      const { savings, measured } = impact;
      const allMeasured =
        measured.before.resourcesMeasured === measured.before.resourceCount &&
        measured.after.resourcesMeasured === measured.after.resourceCount;
      if (savings.totalBytesPct < 0 && allMeasured) expect(text, ex.slug).toMatch(/% larger\.$/);
      if (savings.requests < 0 && !/@import/.test(measured.before.html)) {
        expect(text, ex.slug).toMatch(/ HTTP requests instead of /);
      }
    }
  });

  it("states the heavier total next to combine_javascript's saved request", () => {
    expect(measuredSentences(exampleImpact('combine_javascript')!)[0]).toBe(
      'Measured on the Combine JavaScript demo page, original against optimized: 3 HTTP requests become 2, but the total weight (HTML plus the files it loads) rises from 552 B to 737 B, 34% larger.',
    );
  });

  it("states the extra request and the heavier total next to outline_css's smaller HTML", () => {
    expect(measuredSentences(exampleImpact('outline_css')!)[0]).toBe(
      'Measured on the Outline CSS demo page, original against optimized: the HTML document falls from 5.4 KB to 524 B, 91% smaller, but the page makes 2 HTTP requests instead of 1, and the total weight (HTML plus the files it loads) rises from 5.4 KB to 5.5 KB, 2% larger.',
    );
  });

  it('does not state a request rise the capture caused by missing an @import', () => {
    // The original prioritize_critical_css page loads a stylesheet through
    // @import, which the capture does not count.
    expect(exampleImpact('prioritize_critical_css')!.savings.requests).toBeLessThan(0);
    expect(tradeOffClauses(exampleImpact('prioritize_critical_css')!)).toEqual([]);
  });

  it('never gives one page both a growth line and a trade-off clause', () => {
    for (const ex of EXAMPLES) {
      const impact = exampleImpact(ex.slug)!;
      if (growthClause(impact)) {
        expect(measuredSentences(impact), ex.slug).toEqual([]);
        expect(tradeOffClauses(impact).length && impact.tiles.length, ex.slug).toBeFalsy();
      }
    }
  });

  it('leaves a saving with no cost as it was', () => {
    expect(tradeOffClauses(exampleImpact('combine_css')!)).toEqual([]);
    expect(tradeOffClauses(exampleImpact('local_storage_cache')!)).toEqual([]);
  });

  it('states no total-weight rise when a resource went unmeasured', () => {
    const impact = {
      savings: { requests: 1, htmlBytesPct: 10, resourceBytesPct: -50, totalBytesPct: -40 },
      missedResources: false,
      measured: {
        before: {
          html: '',
          requestCount: 3,
          resourceCount: 2,
          resourcesMeasured: 1,
          htmlBytes: 200,
          resourceBytes: 100,
        },
        after: {
          html: '',
          requestCount: 2,
          resourceCount: 1,
          resourcesMeasured: 1,
          htmlBytes: 180,
          resourceBytes: 240,
        },
      },
    } as any;
    expect(tradeOffClauses(impact)).toEqual([]);
  });
});

describe('a capture that missed a resource', () => {
  // A capture that found no resources although the optimized HTML still loads
  // one (for instance an unquoted <img src>). No example in today's data does
  // this; the guard stays so a future capture like it cannot claim a saving.
  const missed = {
    settled: true,
    before: {
      requestCount: 2,
      resourceCount: 1,
      resourcesMeasured: 1,
      htmlBytes: 200,
      resourceBytes: 4000,
    },
    after: {
      requestCount: 1,
      resourceCount: 0,
      resourcesMeasured: 0,
      htmlBytes: 180,
      resourceBytes: 0,
      html: '<img src=images/a.png>',
    },
    savings: { requests: 1, htmlBytesPct: 10, resourceBytesPct: 100, totalBytesPct: 95 },
  };

  it('is flagged on a synthetic capture that lost its only resource', () => {
    expect(captureMissedResources(missed)).toBe(true);
  });

  it('is not detected on any example in the current data', () => {
    expect(
      EXAMPLES.filter((ex) => exampleImpact(ex.slug)!.missedResources).map((e) => e.slug),
    ).toEqual([]);
  });

  it('shows remove_quotes only its HTML saving, now that its image is counted', () => {
    const impact = exampleImpact('remove_quotes')!;
    expect(impact.tiles.map((t) => t.label)).toEqual(['HTML size']);
    expect(headlineMetric('remove_quotes')).toBe('−5% HTML');
  });

  it('does not count a connection hint as a missed resource', () => {
    const at = (html: string) => ({ after: { resourceCount: 0, html } });
    expect(captureMissedResources(at('<link rel="preconnect" href="http://a.test">'))).toBe(false);
    expect(captureMissedResources(at('<link rel=dns-prefetch href=//a.test>'))).toBe(false);
    expect(captureMissedResources(at('<link rel="stylesheet" href="a.css">'))).toBe(true);
    // inline_google_font_css inlines its only stylesheet and keeps a preconnect.
    expect(captureMissedResources(exampleImpact('inline_google_font_css')!.measured)).toBe(false);
    expect(headlineMetric('inline_google_font_css')).toBe('−1 request');
  });

  it('does not count a data: URI as a missed resource', () => {
    const at = (html: string) => ({ after: { resourceCount: 0, html } });
    expect(captureMissedResources(at('<img src="data:image/png;base64,AAAA">'))).toBe(false);
    expect(captureMissedResources(at('<img src=data:image/png;base64,AAAA>'))).toBe(false);
    expect(captureMissedResources(at('<img src=images/a.png>'))).toBe(true);
    expect(captureMissedResources(at('<script src="a.js"></script>'))).toBe(true);
    expect(captureMissedResources({ after: { resourceCount: 1, html: '<img src=a.png>' } })).toBe(
      false,
    );
  });

  it('leaves the other gallery badges as they were', () => {
    expect(headlineMetric('combine_css')).toBe('−3 requests');
    expect(headlineMetric('rewrite_images')).toBe('−96% bytes');
    expect(headlineMetric('outline_css')).toBe('−91% HTML');
  });
});

describe('an example whose filter did not act', () => {
  const statusOf = (slug: string) => exampleImpact(slug)!.status;

  it('reads the status the generator wrote', () => {
    expect(statusOf('combine_css')).toBe('settled');
    expect(statusOf('prioritize_critical_images')).toBe('beacon');
    expect(statusOf('resize_rendered_image_dimensions')).toBe('beacon');
    expect(statusOf('hint_preload_subresources')).toBe('header');
    expect(statusOf('extend_cache')).toBe('settled');
    expect(statusOf('rewrite_css_extend_cache')).toBe('settled');
    expect(statusOf('canonicalize_javascript_libraries')).toBe('not-applied');
  });

  it('claims no saving, is not indexable and says what was measured', () => {
    const noted = EXAMPLES.filter((ex) => exampleImpact(ex.slug)!.notApplied);
    expect(noted.length).toBe(4);
    for (const ex of noted) {
      const impact = exampleImpact(ex.slug)!;
      expect(impact.tiles, ex.slug).toEqual([]);
      expect(measuredSentences(impact), ex.slug).toEqual([]);
      expect(headlineMetric(ex.slug), ex.slug).toBeNull();
      expect(impact.indexable, ex.slug).toBe(false);
      expect(notAppliedNote(impact, 'November 2026'), ex.slug).toMatch(
        /^In the latest measurement \(November 2026\), /,
      );
    }
  });

  it('explains a beacon entry and never shows its instrumented page as a saving', () => {
    for (const slug of ['prioritize_critical_images', 'resize_rendered_image_dimensions']) {
      const impact = exampleImpact(slug)!;
      expect(impact.notApplied).toBe('beacon');
      expect(impact.diff).toBeUndefined();
      expect(impact.savings).toBeUndefined();
      expect(notAppliedNote(impact, 'x')).toMatch(
        /carried only mod_pagespeed's beacon script.*The live frames above load it in your browser, so they can show it\.$/,
      );
      expect(demonstrates(slug, slug)).toBe(false);
    }
  });

  it('quotes the hint header of a header entry', () => {
    const impact = exampleImpact('hint_preload_subresources')!;
    expect(impact.notApplied).toBe('header');
    expect(impact.hintHeader).toMatch(/rel=preload/);
    expect(notAppliedNote(impact, 'x')).toMatch(
      /the response gained 5 preload hints in its Link header instead\. This filter adds its hints to the HTTP response headers, not to the HTML,/,
    );
    expect(demonstrates('hint_preload_subresources', 'hint_preload_subresources')).toBe(true);
  });

  it('says what was tried for a not-applied entry, with the reason where one is known', () => {
    expect(notAppliedNote(exampleImpact('canonicalize_javascript_libraries')!, 'x')).toBe(
      "In the latest measurement (x), the optimized HTML of this demo page came back identical to the original, so there is no source diff or measured saving to show. The page was requested 18 times over 78 seconds, also as a browser, and it never changed. This filter needs configuration or content the public demo doesn't provide, such as a recognized JavaScript library.",
    );
  });

  it('shows the examples that settled as a browser, with their diff', () => {
    for (const slug of ['lazyload_images', 'defer_javascript', 'insert_dns_prefetch']) {
      const impact = exampleImpact(slug)!;
      expect(impact.status, slug).toBe('settled');
      expect(impact.notApplied, slug).toBeNull();
      expect(impact.indexable, slug).toBe(true);
      expect(impact.browserOnly, slug).toBe(true);
      expect(browserOnlyNote(impact), slug).toMatch(/requested the optimized page as a browser\.$/);
      expect(
        impact.diff!.some((l) => l.t === '+'),
        slug,
      ).toBe(true);
      // Their HTML grows and no request or byte falls: no figure, no badge.
      expect(impact.tiles, slug).toEqual([]);
      expect(measuredSentences(impact), slug).toEqual([]);
      expect(headlineMetric(slug), slug).toBeNull();
      expect(demonstrates(slug, slug), slug).toBe(true);
    }
    expect(browserOnlyNote(exampleImpact('combine_css')!)).toBeNull();
    // What they show instead of a saving is what grew.
    expect(growthClause(exampleImpact('lazyload_images')!)).toBe(
      'the HTML document grows from 964 B to 1.6 KB, and the total weight (HTML plus the files it loads) rises from 83.3 KB to 83.9 KB, 1% larger',
    );
    expect(growthClause(exampleImpact('defer_javascript')!)).toBe(
      'the HTML document grows from 1.0 KB to 1.8 KB, the page makes 6 HTTP requests instead of 5, and the total weight (HTML plus the files it loads) rises from 264.5 KB to 285.5 KB, 8% larger',
    );
    expect(growthClause(exampleImpact('insert_dns_prefetch')!)).toBe(
      'the HTML document grows from 281 B to 392 B',
    );
    expect(growthClause(exampleImpact('combine_css')!)).toBeNull();
    // The original's @import stylesheet is not counted, so neither its
    // request rise (1 to 4) nor its total-weight rise (+1102%) is claimed.
    expect(growthClause(exampleImpact('inline_import_to_link')!)).toBe(
      'the HTML document grows from 406 B to 427 B',
    );
    // No resources on either side: the total is the HTML again, so it is not repeated.
    expect(growthClause(exampleImpact('add_instrumentation')!)).toBe(
      'the HTML document grows from 172 B to 4.1 KB',
    );
  });

  it('shows the cache-extension examples with their extended URLs', () => {
    for (const slug of ['extend_cache', 'rewrite_css_extend_cache']) {
      const impact = exampleImpact(slug)!;
      expect(impact.notApplied, slug).toBeNull();
      expect(impact.indexable, slug).toBe(true);
      expect(notAppliedNote(impact, 'x'), slug).toBeNull();
      expect(
        impact.diff!.some((l) => l.t === '+' && /\.pagespeed\.ce\./.test(l.s)),
        slug,
      ).toBe(true);
      // The extended URLs are longer, and nothing on these pages fell.
      expect(impact.tiles, slug).toEqual([]);
      expect(headlineMetric(slug), slug).toBeNull();
      expect(demonstrates(slug, 'extend_cache'), slug).toBe(true);
    }
    expect(growthClause(exampleImpact('extend_cache')!)).toBe(
      'the HTML document grows from 393 B to 421 B',
    );
    expect(growthClause(exampleImpact('rewrite_css_extend_cache')!)).toBe(
      'the HTML document grows from 390 B to 431 B, and the total weight (HTML plus the files it loads) rises from 490 B to 597 B, 22% larger',
    );
  });

  it('has no note when the filter acted', () => {
    const impact = exampleImpact('combine_css')!;
    expect(impact.notApplied).toBeNull();
    expect(impact.indexable).toBe(true);
    expect(notAppliedNote(impact, 'x')).toBeNull();
    expect(demonstrates('combine_css', 'combine_css')).toBe(true);
  });
});

describe('the rendering follows the status in the data', () => {
  const entries = (examplesData as { examples: Record<string, any> }).examples;
  let saved: Record<string, any>;
  beforeEach(() => {
    saved = structuredClone(entries);
  });
  afterEach(() => {
    for (const k of Object.keys(entries)) delete entries[k];
    Object.assign(entries, saved);
  });

  it('hides a settled example once its status says beacon', () => {
    expect(headlineMetric('combine_css')).toBe('−3 requests');
    entries.combine_css.status = 'beacon';
    const impact = exampleImpact('combine_css')!;
    expect(impact.notApplied).toBe('beacon');
    expect(impact.diff).toBeUndefined();
    expect(impact.tiles).toEqual([]);
    expect(measuredSentences(impact)).toEqual([]);
    expect(headlineMetric('combine_css')).toBeNull();
    expect(impact.indexable).toBe(false);
    expect(demonstrates('combine_css', 'combine_css')).toBe(false);
  });

  it('shows a not-applied example once its status says settled', () => {
    const e = entries.canonicalize_javascript_libraries;
    e.status = 'settled';
    e.settled = true;
    e.diff = [{ t: '+', s: '<script src="lib.js"></script>' }];
    e.savings = { requests: 1, htmlBytesPct: 0, resourceBytesPct: 0, totalBytesPct: 0 };
    e.after.requestCount = e.before.requestCount - 1;
    const impact = exampleImpact('canonicalize_javascript_libraries')!;
    expect(impact.notApplied).toBeNull();
    expect(notAppliedNote(impact, 'x')).toBeNull();
    expect(impact.indexable).toBe(true);
    expect(impact.tiles.map((t) => t.label)).toEqual(['HTTP requests']);
    expect(headlineMetric('canonicalize_javascript_libraries')).toBe('−1 request');
  });

  it('turns a settled example into a header note when its status says header', () => {
    entries.combine_css.status = 'header';
    entries.combine_css.statusEvidence = { hintHeader: '</a.css>; rel=preload; as=style' };
    const impact = exampleImpact('combine_css')!;
    expect(impact.notApplied).toBe('header');
    expect(notAppliedNote(impact, 'x')).toMatch(/gained 1 preload hint in its Link header/);
    expect(headlineMetric('combine_css')).toBeNull();
  });

  it('turns a settled example into a not-applied note when its status says not-applied', () => {
    entries.combine_css.status = 'not-applied';
    entries.combine_css.statusEvidence = {
      requests: 3,
      windowSeconds: 9,
      userAgentsTried: ['default'],
    };
    const impact = exampleImpact('combine_css')!;
    expect(notAppliedNote(impact, 'x')).toMatch(
      /The page was requested 3 times over 9 seconds, and it never changed\.$/,
    );
    expect(impact.indexable).toBe(false);
  });

  it('gives no fixed reason when extend_cache does not act', () => {
    const e = entries.extend_cache;
    e.status = 'not-applied';
    e.settled = false;
    e.after.html = e.before.html;
    e.statusEvidence = { requests: 18, windowSeconds: 78, userAgentsTried: ['default', 'browser'] };
    const impact = exampleImpact('extend_cache')!;
    expect(impact.notApplied).toBe('not-applied');
    expect(notAppliedNote(impact, 'x')).toMatch(/also as a browser, and it never changed\.$/);
    expect(demonstrates('extend_cache', 'extend_cache')).toBe(false);
  });

  it('claims no saving for CSS images once no URL is cache-extended', () => {
    const e = entries.rewrite_css_extend_cache;
    e.after.html = e.after.html.replace(/\.pagespeed\.ce\.[^.]+\.png/g, '');
    e.savings = { requests: 0, htmlBytesPct: 4, resourceBytesPct: 0, totalBytesPct: 3 };
    const impact = exampleImpact('rewrite_css_extend_cache')!;
    expect(impact.notApplied).toBe('no-cache-extension');
    expect(impact.indexable).toBe(false);
    expect(impact.tiles).toEqual([]);
    expect(measuredSentences(impact)).toEqual([]);
    expect(headlineMetric('rewrite_css_extend_cache')).toBeNull();
    expect(notAppliedNote(impact, 'x')).toMatch(
      /^In the latest measurement \(x\), no URL on this demo page was cache-extended\./,
    );
    expect(notAppliedNote(impact, 'x')).not.toMatch(/one-year/);
    expect(demonstrates('rewrite_css_extend_cache', 'extend_cache')).toBe(false);
    expect(demonstrates('rewrite_css_extend_cache', 'rewrite_css')).toBe(true);
  });

  it('states no total-weight rise beside a saving when the original uses @import', () => {
    expect(tradeOffClauses(exampleImpact('combine_javascript')!)).toHaveLength(1);
    entries.combine_javascript.before.html += '<style>@import url(a.css);</style>';
    expect(tradeOffClauses(exampleImpact('combine_javascript')!)).toEqual([]);
  });

  it('treats an unsettled status as still in progress', () => {
    entries.combine_css.status = 'unsettled';
    const impact = exampleImpact('combine_css')!;
    expect(impact.hasData).toBe(false);
    expect(impact.notApplied).toBeNull();
    expect(headlineMetric('combine_css')).toBeNull();
    expect(impact.indexable).toBe(false);
  });

  it('falls back to the settled flag for data written before status', () => {
    delete entries.combine_css.status;
    expect(exampleImpact('combine_css')!.status).toBe('settled');
    expect(headlineMetric('combine_css')).toBe('−3 requests');
    const e = entries.defer_javascript;
    delete e.status;
    e.settled = false;
    e.after.html = e.before.html;
    const impact = exampleImpact('defer_javascript')!;
    expect(impact.status).toBe('unsettled');
    expect(impact.notApplied).toBe('unchanged');
    expect(notAppliedNote(impact, 'x')).toMatch(/does not identify itself as one\.$/);
  });

  it('tolerates a non-settled entry with no savings stored', () => {
    entries.combine_css.status = 'beacon';
    entries.combine_css.savings = null;
    expect(exampleImpact('combine_css')!.savings).toBeUndefined();
    expect(headlineMetric('combine_css')).toBeNull();
  });
});

describe('examples applying a filter', () => {
  const names = new Set(
    [...filtersData.filters, ...filtersData.aliases].map((f: { name: string }) => f.name),
  );

  it('names only filters the module accepts', () => {
    for (const ex of EXAMPLES) {
      for (const n of ex.shows ?? []) expect(names.has(n), `${ex.slug}: ${n}`).toBe(true);
    }
  });

  it('counts enabled filters and skips disabled ones', () => {
    const slugs = (n: string) => examplesApplying(n).map((e) => e.slug);
    expect(slugs('rewrite_images')).toContain('map_proxy_domain');
    expect(slugs('inline_images')).not.toContain('map_proxy_domain');
    expect(slugs('convert_jpeg_to_webp')).toEqual(['rewrite_images']);
    expect(slugs('rewrite_javascript_inline')).toEqual(['rewrite_javascript']);
  });

  it('gives every gallery example a filter page that lists it', () => {
    const listed = new Set(
      [...filtersData.filters, ...filtersData.aliases].flatMap((f: { name: string }) =>
        examplesApplying(f.name).map((e) => e.slug),
      ),
    );
    expect(EXAMPLES.filter((ex) => !listed.has(ex.slug)).map((ex) => ex.slug)).toEqual([]);
  });
});
