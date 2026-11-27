// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Per-page hints for the site search index (Pagefind), derived from the path
 * alone so no page file has to carry them. The layout applies both inside the
 * `<main data-pagefind-body>` element:
 *
 * - `searchWeight(path)` is the `data-pagefind-weight` of the page's body
 *   text. A page a visitor searches for by name outranks a blog post that
 *   merely mentions the word, the documentation outranks the blog, and the
 *   two directive references outrank the guides that quote a directive, so a
 *   directive name lands on its reference entry. Everything else keeps
 *   Pagefind's default of 1.
 * - `searchTitle(path)` is a short result title for the pages whose headline
 *   is a sentence rather than the page's name. Pagefind reads it from a
 *   `data-pagefind-meta="title:…"` attribute; the title is also searched, so
 *   it is the strongest signal that "pricing" means the pricing page.
 *
 * These are index-time settings: they take effect at the next build. The
 * search-time half (the `ranking` options) is in SearchDialog.astro, and
 * `scripts/search-probes.mjs` prints where a set of probe queries land.
 */

/** Result titles for pages whose headline does not name the page. */
const SHORT_TITLES: Record<string, string> = {
  '/pricing/': 'Pricing',
  '/download/': 'Download',
  '/contact/': 'Contact',
  '/support/': 'Support',
  '/features/': 'Features',
  '/demo/': 'Demo',
  '/calculator/': 'Calculator',
  '/about/': 'About',
  '/alternatives/': 'Alternatives',
  '/wordpress/': 'WordPress',
};

/** The directive references: every directive has its entry on one of these. */
const REFERENCE_PAGES = new Set(['/docs/configuration/', '/docs/worker-configuration/']);

const NAMED_PAGE_WEIGHT = '8';
const REFERENCE_WEIGHT = '6';
const DOCS_WEIGHT = '2';

const withSlash = (path: string) => (path.endsWith('/') ? path : `${path}/`);

export function searchWeight(path: string): string | undefined {
  const p = withSlash(path);
  if (p in SHORT_TITLES) return NAMED_PAGE_WEIGHT;
  if (REFERENCE_PAGES.has(p)) return REFERENCE_WEIGHT;
  if (p.startsWith('/docs/')) return DOCS_WEIGHT;
  return undefined;
}

export function searchTitle(path: string): string | undefined {
  return SHORT_TITLES[withSlash(path)];
}
