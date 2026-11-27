// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The measured impact of one /examples/<slug>/ demo, read from
// src/data/examples-data.json (written by tools/examples-gallery/generate.mjs).
// Shared by the example page (src/pages/examples/[slug].astro) and the filter
// topic pages (src/pages/docs/filters/[name].astro), so both show the same
// numbers from the same rules.

import { EXAMPLES, MECHANISM, type Example, type Mechanism } from '../data/examples';
import examplesData from '../data/examples-data.json';

export interface ImpactTile {
  label: string;
  before: string;
  after: string;
  pill: string;
}

/**
 * Why an entry has, or has no, source diff (tools/examples-gallery/README.md):
 * 'settled' changed the body and held; 'beacon' only carries the beacon
 * script; 'header' changed only a response hint header; 'not-applied' never
 * changed; 'unsettled' changed but did not hold.
 */
export type ExampleStatus = 'settled' | 'beacon' | 'header' | 'not-applied' | 'unsettled';

const STATUSES: readonly ExampleStatus[] = [
  'settled',
  'beacon',
  'header',
  'not-applied',
  'unsettled',
];

/** An entry's status, falling back to its `settled` flag for data written before `status`. */
export function exampleStatus(measured: any): ExampleStatus {
  if (STATUSES.includes(measured?.status)) return measured.status;
  return measured?.settled ? 'settled' : 'unsettled';
}

export interface ExampleImpact {
  ex: Example;
  /** Why the entry has, or has no, source diff; null when there is no entry. */
  status: ExampleStatus | null;
  /** How the filter shows when it is not a plain HTML rewrite. */
  mechanism: Mechanism | undefined;
  /** The raw generator entry, when there is one. */
  measured: any;
  /** The generator has an entry without an error. */
  present: boolean;
  /** ...and its status is 'settled': a stable before/after of the filter's work. */
  hasData: boolean;
  /**
   * Settled only when the measurement asked for the page as a browser: the
   * filter applies to browsers mod_pagespeed recognizes as supporting it.
   */
  browserOnly: boolean;
  /** The response hint header a 'header' entry gained, verbatim. */
  hintHeader: string | null;
  /** A beacon-gated capture that settled byte-identical: nothing real to show. */
  nonSubstantive: boolean;
  substantive: boolean;
  diff: { t: string; s: string }[] | undefined;
  savings: any;
  /** The capture missed a resource: no request or total-weight figures. */
  missedResources: boolean;
  /**
   * Why the page shows no diff or saving of this filter, or null when it shows
   * them. 'beacon', 'header' and 'not-applied': the entry's status.
   * 'unchanged': an unsettled capture whose optimized HTML is identical to the
   * original. 'no-cache-extension': a settled example that enables
   * extend_cache, but no URL in the optimized HTML was cache-extended, so the
   * diff and any saving come from its other filters only.
   */
  notApplied: 'beacon' | 'header' | 'not-applied' | 'unchanged' | 'no-cache-extension' | null;
  /** Search engines may index the page: settled data from a filter that acted. */
  indexable: boolean;
  /** The byte and request tiles worth showing. */
  tiles: ImpactTile[];
}

export function formatBytes(bytes: number): string {
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
  if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return bytes + ' B';
}

/**
 * True when the capture found no resources on an optimized page whose HTML
 * still references one (a src or href that is not a data: URI). The capture
 * then missed the resource, so its request and total-weight savings are not
 * real and are not shown; the HTML size is unaffected.
 */
export function captureMissedResources(measured: any): boolean {
  if (!measured?.after || measured.after.resourceCount !== 0) return false;
  const tags: string[] =
    (measured.after.html ?? '').match(/<(?:img|script|link|iframe)\b[^>]*>/gi) ?? [];
  return tags.some(
    (tag) =>
      /\b(?:src|href)\s*=\s*(?!["']?data:)/i.test(tag) &&
      // A preconnect or dns-prefetch link opens a connection and downloads
      // nothing, so the capture rightly does not count it.
      !/\brel\s*=\s*["']?[^"'>]*\b(?:preconnect|dns-prefetch)\b/i.test(tag),
  );
}

/** The example's after frame enables `name` (a filter in its PageSpeedFilters). */
function enablesFilter(ex: Example, name: string): boolean {
  return ex.filters
    .split(',')
    .map((f) => f.trim().replace(/^\+/, ''))
    .includes(name);
}

export function exampleImpact(slug: string): ExampleImpact | null {
  const ex = EXAMPLES.find((e) => e.slug === slug);
  if (!ex) return null;
  const mechanism = MECHANISM[ex.slug];
  const data = examplesData as { examples: Record<string, any> };
  const measured = data.examples[ex.slug];
  // "Settled" entries captured a stable before/after the generator could diff.
  // Unsettled-but-present entries are header-/beacon-/config-gated filters that
  // leave no HTML source diff — `mechanism` explains which.
  const present = !!measured && !measured.error;
  const status = present ? exampleStatus(measured) : null;
  // Only a settled entry's diff and savings are the filter's work: a beacon
  // entry stores the instrumented first-visit page, and the other statuses
  // changed nothing in the body.
  const hasData = status === 'settled';
  const diff: { t: string; s: string }[] | undefined = hasData ? measured.diff : undefined;
  const savings = hasData ? (measured.savings ?? undefined) : undefined;
  const evidence = present ? measured.statusEvidence : undefined;
  const browserOnly =
    hasData &&
    evidence?.userAgent === 'browser' &&
    !!evidence?.userAgentsTried?.includes('default');
  const hintHeader =
    status === 'header' && typeof evidence?.hintHeader === 'string' ? evidence.hintHeader : null;

  // A beacon-gated filter can "settle" on a byte-identical capture — a dedicated
  // demo page whose single-line tags leave no reflow — so the data is present but
  // shows no real change. Treat such a card as non-substantive: prefer the beacon
  // explanation over an empty/cosmetic diff and suppress the "Measured impact"
  // block. Cards with actual +/- diff lines (prioritize_critical_css,
  // inline_preview_images) keep their diff and are unaffected.
  const hasRealDiff = !!diff && diff.some((line) => line.t === '+' || line.t === '-');
  const nonSubstantive =
    mechanism === 'beacon' &&
    hasData &&
    !hasRealDiff &&
    (!savings || (savings.requests === 0 && savings.resourceBytesPct === 0));
  const substantive = !nonSubstantive;
  const missedResources = hasData && captureMissedResources(measured);
  const notApplied: ExampleImpact['notApplied'] =
    status === 'beacon' || status === 'header' || status === 'not-applied'
      ? status
      : status === 'unsettled' && measured.before?.html === measured.after?.html
        ? 'unchanged'
        : hasData &&
            enablesFilter(ex, 'extend_cache') &&
            !/\.pagespeed\.ce\./.test(measured.after?.html ?? '')
          ? 'no-cache-extension'
          : null;
  const indexable = hasData && !notApplied;

  const tiles: ImpactTile[] = [];
  if (savings && substantive && !notApplied) {
    if (savings.requests > 0 && !missedResources) {
      tiles.push({
        label: 'HTTP requests',
        before: String(measured.before.requestCount),
        after: String(measured.after.requestCount),
        pill: `−${savings.requests}`,
      });
    }
    if (savings.totalBytesPct > 0 && measured.before.resourcesMeasured > 0 && !missedResources) {
      const b = measured.before.htmlBytes + measured.before.resourceBytes;
      const a = measured.after.htmlBytes + measured.after.resourceBytes;
      tiles.push({
        label: 'Total bytes',
        before: formatBytes(b),
        after: formatBytes(a),
        pill: `−${savings.totalBytesPct}%`,
      });
    }
    if (savings.htmlBytesPct > 0) {
      tiles.push({
        label: 'HTML size',
        before: formatBytes(measured.before.htmlBytes),
        after: formatBytes(measured.after.htmlBytes),
        pill: `−${savings.htmlBytesPct}%`,
      });
    }
  }

  return {
    ex,
    status,
    mechanism,
    measured,
    present,
    hasData,
    browserOnly,
    hintHeader,
    nonSubstantive,
    substantive,
    diff,
    savings,
    missedResources,
    notApplied,
    indexable,
    tiles,
  };
}

/** "18 times over 78 seconds": how often and how long the measurement asked. */
function attempts(evidence: any): string | null {
  const n = evidence?.requests;
  const secs = evidence?.windowSeconds;
  if (!(n > 1) || !(secs > 0)) return null;
  return `${n} times over ${secs} seconds`;
}

/**
 * What an example page says when the latest capture shows no diff or saving
 * of its filter: what was measured, then why, where the reason is known for
 * this filter. Null when the capture shows the filter acting. `when` is the
 * capture's month.
 */
export function notAppliedNote(impact: ExampleImpact, when: string): string | null {
  const kind = impact.notApplied;
  if (!kind) return null;
  const evidence = impact.measured?.statusEvidence;
  const tried = attempts(evidence);
  const asBrowser = !!evidence?.userAgentsTried?.includes('browser');
  const configReason =
    "This filter needs configuration or content the public demo doesn't provide, such as a recognized JavaScript library.";

  if (kind === 'beacon') {
    return `In the latest measurement (${when}), the optimized page carried only mod_pagespeed's beacon script, which reports back what a real browser rendered. This filter acts only after such a report, which the measurement does not send, so there is no source diff or measured saving to show. The live frames above load it in your browser, so they can show it.`;
  }
  if (kind === 'header') {
    const hints = (impact.hintHeader?.match(/rel=preload/gi) ?? []).length;
    const what = hints > 0 ? `${hints} preload hint${hints === 1 ? '' : 's'}` : 'resource hints';
    return `In the latest measurement (${when}), the optimized HTML of this demo page came back identical to the original, and the response gained ${what} in its Link header instead. This filter adds its hints to the HTTP response headers, not to the HTML, so there is no source diff or measured saving to show.`;
  }
  if (kind === 'not-applied') {
    const how = tried
      ? ` The page was requested ${tried}${asBrowser ? ', also as a browser' : ''}, and it never changed.`
      : '';
    const reason = impact.mechanism === 'config' ? configReason : undefined;
    const base = `In the latest measurement (${when}), the optimized HTML of this demo page came back identical to the original, so there is no source diff or measured saving to show.${how}`;
    return reason ? `${base} ${reason}` : base;
  }
  const what =
    kind === 'no-cache-extension'
      ? `In the latest measurement (${when}), no URL on this demo page was cache-extended. The diff shows only what the example's other filters changed, so this page states no measured saving.`
      : `In the latest measurement (${when}), the optimized HTML of this demo page came back identical to the original, so there is no source diff or measured saving to show.`;
  // 'unchanged' comes from data written before `status`; its reason comes
  // from how the filter is known to act.
  const why: Partial<Record<Mechanism, string>> = {
    beacon:
      'This filter acts only after a real browser has loaded the page and reported back, which the measurement does not do.',
    browser:
      'This filter acts only for browsers mod_pagespeed recognizes as supporting it, and the measurement does not identify itself as one.',
    header:
      'This filter adds its hints to the HTTP response headers, not to the HTML the measurement compares.',
    config: configReason,
  };
  const reason = impact.mechanism ? why[impact.mechanism] : undefined;
  return reason ? `${what} ${reason}` : what;
}

/**
 * The line a settled example adds when the measurement had to ask for the
 * page as a browser before the filter acted. Null otherwise.
 */
export function browserOnlyNote(impact: ExampleImpact): string | null {
  return impact.browserOnly
    ? 'mod_pagespeed applies this filter only for browsers it recognizes as supporting it, so the measurement requested the optimized page as a browser.'
    : null;
}

/**
 * "the page makes 6 HTTP requests instead of 5" when the optimized page makes
 * more requests, or null. Shared by the growth line and the trade-off clause
 * so both say it the same way. Left out when the original page loads a
 * stylesheet through @import, which the capture does not count, so the
 * original's request count is too low to compare.
 */
function requestRise(measured: any): string | null {
  const b = measured?.before;
  const a = measured?.after;
  if (!(a?.requestCount > b?.requestCount) || /@import\b/i.test(b.html ?? '')) return null;
  return `the page makes ${a.requestCount} HTTP requests instead of ${b.requestCount}`;
}

/**
 * "the total weight (HTML plus the files it loads) rises from X to Y, P%
 * larger" when the page as a whole got heavier, or null. Shared by the growth
 * line and the trade-off clause. Stated only when the capture measured every
 * resource on both sides, the page loads at least one (otherwise the total is
 * the HTML figure again), and the original loads no stylesheet through
 * @import, which the capture does not count.
 */
function totalRise(measured: any, savings: any): string | null {
  const b = measured?.before;
  const a = measured?.after;
  if (!savings || !(savings.totalBytesPct < 0) || !b || !a) return null;
  if (b.resourcesMeasured !== b.resourceCount || a.resourcesMeasured !== a.resourceCount) {
    return null;
  }
  if (!(b.resourceCount > 0 || a.resourceCount > 0)) return null;
  if (/@import\b/i.test(b.html ?? '')) return null;
  return `the total weight (HTML plus the files it loads) rises from ${formatBytes(b.htmlBytes + b.resourceBytes)} to ${formatBytes(a.htmlBytes + a.resourceBytes)}, ${-savings.totalBytesPct}% larger`;
}

/**
 * What a settled example with no byte or request reduction grew by, in words:
 * "the HTML document grows from 964 B to 1.6 KB", plus the request count and
 * the total weight when those rose (requestRise, totalRise). Null when nothing grew, so the page keeps its
 * plain "not its size" line only where the data bears it out.
 */
export function growthClause(impact: ExampleImpact): string | null {
  const { measured, hasData, substantive, notApplied, tiles } = impact;
  if (!hasData || !substantive || notApplied || tiles.length > 0) return null;
  const b = measured.before;
  const a = measured.after;
  if (!(a?.htmlBytes > b?.htmlBytes)) return null;
  const parts = [
    `the HTML document grows from ${formatBytes(b.htmlBytes)} to ${formatBytes(a.htmlBytes)}`,
    requestRise(measured),
    totalRise(measured, impact.savings),
  ].filter((p): p is string => !!p);
  const last = parts[parts.length - 1];
  return parts.length === 1
    ? last
    : parts.length === 2
      ? `${parts[0]}, and ${last}`
      : `${parts.slice(0, -1).join(', ')}, and ${last}`;
}

/**
 * The measured result of one example in words, from the same numbers and
 * rules as the impact tiles, so the page states what this demo saved as text
 * and not only in figures. Empty when the example shows no byte or request
 * reduction.
 * Like the tiles, it leaves out request and total-weight savings when the
 * capture missed a resource (captureMissedResources).
 * When it states a saving while the request count or the total weight went
 * up, it says so in the same sentence (tradeOffClauses), so a saving in one
 * figure is never stated alone while the page as a whole got heavier.
 */
export function measuredSentences(impact: ExampleImpact): string[] {
  const { ex, measured, savings, hasData, substantive, missedResources, notApplied } = impact;
  if (!hasData || !substantive || !savings || notApplied) return [];
  const clauses: string[] = [];
  if (savings.requests > 0 && !missedResources) {
    clauses.push(
      `${measured.before.requestCount} HTTP requests become ${measured.after.requestCount}`,
    );
  }
  if (savings.totalBytesPct > 0 && measured.before.resourcesMeasured > 0 && !missedResources) {
    const b = measured.before.htmlBytes + measured.before.resourceBytes;
    const a = measured.after.htmlBytes + measured.after.resourceBytes;
    clauses.push(
      `the total weight (HTML plus the files it loads) falls from ${formatBytes(b)} to ${formatBytes(a)}, ${savings.totalBytesPct}% smaller`,
    );
  }
  if (savings.htmlBytesPct > 0) {
    clauses.push(
      `the HTML document falls from ${formatBytes(measured.before.htmlBytes)} to ${formatBytes(measured.after.htmlBytes)}, ${savings.htmlBytesPct}% smaller`,
    );
  }
  if (clauses.length === 0) return [];
  const last = clauses[clauses.length - 1];
  const list =
    clauses.length === 1
      ? last
      : clauses.length === 2
        ? `${clauses[0]}, and ${last}`
        : `${clauses.slice(0, -1).join('; ')}; and ${last}`;
  const costs = tradeOffClauses(impact);
  const but =
    costs.length === 0 ? '' : `${clauses.length > 2 ? ';' : ','} but ${costs.join(', and ')}`;
  return [`Measured on the ${ex.title} demo page, original against optimized: ${list}${but}.`];
}

/**
 * The figures that went the other way: more HTTP requests, or a heavier page
 * in total (HTML plus the files it loads). measuredSentences states them next
 * to the saving. A request rise is left out when the original page loads a
 * stylesheet through @import, which the capture does not count, so the
 * original's request count is too low. A total-weight rise follows the
 * guards of totalRise.
 */
export function tradeOffClauses(impact: ExampleImpact): string[] {
  const { measured, savings, missedResources } = impact;
  if (!savings || missedResources) return [];
  const costs: string[] = [];
  const rise = requestRise(measured);
  if (rise) costs.push(rise);
  const heavier = totalRise(measured, savings);
  if (heavier) costs.push(heavier);
  return costs;
}

/**
 * The one-line badge on a gallery card: the largest kind of saving the
 * capture shows, by the same rules as the tiles. Null when there is none.
 */
export function headlineMetric(slug: string): string | null {
  const d = (examplesData as { examples: Record<string, any> }).examples[slug];
  if (!d || d.error || exampleStatus(d) !== 'settled' || !d.savings) return null;
  if (exampleImpact(slug)?.notApplied) return null;
  const s = d.savings;
  const missed = captureMissedResources(d);
  if (s.requests > 0 && !missed) return `−${s.requests} request${s.requests === 1 ? '' : 's'}`;
  if (s.totalBytesPct > 0 && !missed) return `−${s.totalBytesPct}% bytes`;
  if (s.htmlBytesPct > 0) return `−${s.htmlBytesPct}% HTML`;
  return null;
}

/**
 * Whether the example's latest capture shows `name` acting on the demo page:
 * false for every filter of a beacon, not-applied or unchanged capture, and
 * for extend_cache when no URL was cache-extended. A header capture shows its
 * filter acting, in the response headers. Filter pages list an example
 * under "Also applied in" only when this holds.
 */
export function demonstrates(slug: string, name: string): boolean {
  const impact = exampleImpact(slug);
  if (!impact) return false;
  const n = impact.notApplied;
  if (n === 'beacon' || n === 'not-applied' || n === 'unchanged') return false;
  if (n === 'no-cache-extension') return name !== 'extend_cache';
  return true;
}
