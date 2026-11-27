// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Sitemap `lastmod` for the routes that have no content-collection date: the
 * `.astro` pages under `src/pages/` and a few generated route families.
 *
 * Each route is dated by the last commit that touched the files it is built
 * from: the page file itself, plus the section or data files named in
 * `EXTRA_SOURCES` / `ROUTE_FAMILIES`. The shared layout and components are
 * deliberately not counted: a header change would otherwise re-date every URL
 * at once, which is the always-new signal crawlers learn to ignore.
 *
 * `dateOf(file)` returns YYYY-MM-DD or null (see `gitLastModified`). When it
 * answers null for every source of a route (a shallow clone, an exported tree,
 * no git), the route gets no entry and the sitemap omits its `lastmod`; a
 * build-time "today" is never substituted.
 */
import { readdirSync } from 'node:fs';
import { join, relative, sep } from 'node:path';

/** Extra files a static page is built from, keyed by route. */
export const EXTRA_SOURCES = {
  // The home page sections live in their own directory (one file per section).
  '/': ['src/components/home'],
};

/**
 * Generated route families: every route under `prefix` is dated from the
 * template and the data it renders.
 */
export const ROUTE_FAMILIES = [
  {
    prefix: '/examples/',
    sources: [
      'src/pages/examples/[slug].astro',
      'src/data/examples.ts',
      'src/data/examples-data.json',
    ],
  },
  {
    prefix: '/core-web-vitals/',
    sources: ['src/pages/core-web-vitals/[metric].astro', 'src/data/cwv-pillars.ts'],
  },
];

/**
 * Map a page file (relative to `src/pages`) to its URL path, or null when the
 * file is not a static `.astro` page (dynamic `[param]` routes, endpoints).
 * @param {string} rel e.g. `alternatives/index.astro`
 * @returns {string | null} e.g. `/alternatives/`
 */
export function routeForPageFile(rel) {
  const parts = rel.split(/[\\/]/);
  if (!rel.endsWith('.astro') || parts.some((p) => p.startsWith('['))) return null;
  const last = parts[parts.length - 1].replace(/\.astro$/, '');
  const segs = last === 'index' ? parts.slice(0, -1) : [...parts.slice(0, -1), last];
  return segs.length ? `/${segs.join('/')}/` : '/';
}

/**
 * @param {string} dir absolute directory
 * @returns {string[]} absolute file paths, recursively
 */
function walk(dir) {
  /** @type {string[]} */
  const out = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, entry.name);
    if (entry.isDirectory()) out.push(...walk(p));
    else out.push(p);
  }
  return out;
}

/**
 * @param {Array<string | null>} dates YYYY-MM-DD strings or null
 * @returns {string | null} the latest date, or null when none is known
 */
export function latestDate(dates) {
  const known = dates.filter((d) => typeof d === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(d));
  return known.length ? known.sort().at(-1) : null;
}

/**
 * Date every static page under `<siteRoot>/src/pages`.
 * @param {string} siteRoot absolute path of `website/`
 * @param {(file: string) => string | null} dateOf
 * @returns {Map<string, string>} URL path -> ISO timestamp, dated routes only
 */
export function staticPageLastmods(siteRoot, dateOf) {
  /** @type {Map<string, string>} */
  const map = new Map();
  const pagesDir = join(siteRoot, 'src', 'pages');
  let files = [];
  try {
    files = walk(pagesDir);
  } catch {
    return map;
  }
  for (const file of files) {
    const route = routeForPageFile(relative(pagesDir, file).split(sep).join('/'));
    if (!route) continue;
    const extra = EXTRA_SOURCES[/** @type {keyof typeof EXTRA_SOURCES} */ (route)] ?? [];
    const date = latestDate([dateOf(file), ...extra.map((f) => dateOf(join(siteRoot, f)))]);
    if (date) map.set(route, new Date(date).toISOString());
  }
  return map;
}

/**
 * The date of a generated route family's sources, keyed by its prefix.
 * @param {string} siteRoot
 * @param {(file: string) => string | null} dateOf
 * @returns {Array<{ prefix: string, iso: string }>}
 */
export function routeFamilyLastmods(siteRoot, dateOf) {
  /** @type {Array<{ prefix: string, iso: string }>} */
  const out = [];
  for (const { prefix, sources } of ROUTE_FAMILIES) {
    const date = latestDate(sources.map((f) => dateOf(join(siteRoot, f))));
    if (date) out.push({ prefix, iso: new Date(date).toISOString() });
  }
  return out;
}

/**
 * Per-record dates from a file's history: for each key, the date of the newest
 * version whose record differs from the version before it (or the oldest
 * version that has it). A commit that rewrites one record re-dates that record
 * only, which a whole-file date cannot do.
 * @param {Array<{ date: string, text: string }>} history newest first
 * @param {(text: string) => Map<string, string>} records key -> record text
 * @returns {Map<string, string>} key -> YYYY-MM-DD, for keys in the newest version
 */
export function recordDates(history, records) {
  /** @type {Map<string, string>} */
  const out = new Map();
  /** @param {string} text */
  const parse = (text) => {
    try {
      return records(text);
    } catch {
      return new Map();
    }
  };
  const versions = history.map((h) => ({ date: h.date, recs: parse(h.text) }));
  if (!versions.length) return out;
  for (const [key, rec] of versions[0].recs) {
    let i = 0;
    while (i + 1 < versions.length && versions[i + 1].recs.get(key) === rec) i++;
    out.set(key, versions[i].date);
  }
  return out;
}

/**
 * The records of examples-data.json: each example's measured capture.
 * @param {string} text
 */
export function exampleDataRecords(text) {
  const data = JSON.parse(text);
  return new Map(
    Object.entries(data.examples ?? {}).map(([slug, rec]) => [slug, JSON.stringify(rec)]),
  );
}

/**
 * The records of the examples.ts catalog: each `{ slug: '…', … }` entry's text.
 * @param {string} text
 */
export function exampleCatalogRecords(text) {
  /** @type {Map<string, string>} */
  const out = new Map();
  for (const m of text.matchAll(/^ {2}\{\n {4}slug: '([^']+)',[\s\S]*?^ {2}\},?$/gm)) {
    out.set(m[1], m[0]);
  }
  return out;
}

/**
 * `/examples/<slug>/` dates. Each page is dated from its own catalog entry and
 * its own capture, plus the page template; a commit that re-measures some
 * examples re-dates only those. A slug with no answer (no usable history) is
 * absent, and the route family's date from `ROUTE_FAMILIES` stays the
 * fallback.
 * @param {string} siteRoot absolute path of `website/`
 * @param {(file: string) => string | null} dateOf
 * @param {(file: string) => Array<{ date: string, text: string }> | null} historyOf
 * @returns {Map<string, string>} URL path -> ISO timestamp
 */
export function exampleLastmods(siteRoot, dateOf, historyOf) {
  /** @type {Map<string, string>} */
  const map = new Map();
  const dataHistory = historyOf(join(siteRoot, 'src/data/examples-data.json'));
  const catalogHistory = historyOf(join(siteRoot, 'src/data/examples.ts'));
  if (!dataHistory || !catalogHistory) return map;
  const data = recordDates(dataHistory, exampleDataRecords);
  const catalog = recordDates(catalogHistory, exampleCatalogRecords);
  const template = dateOf(join(siteRoot, 'src/pages/examples/[slug].astro'));
  for (const [slug, date] of catalog) {
    const latest = latestDate([date, data.get(slug) ?? null, template]);
    if (latest) map.set(`/examples/${slug}/`, new Date(latest).toISOString());
  }
  return map;
}
