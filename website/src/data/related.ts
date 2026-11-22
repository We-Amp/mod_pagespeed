// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// "Related" links shown at the end of the standalone landing pages, so a
// visitor who lands on one from search can reach its neighbours. Rendered by
// src/layouts/parts/RelatedLinks.astro from BaseLayout, keyed on the page
// path. Labels reuse the header and footer labels for the same pages; every
// internal href is checked against src/pages by test/related-links.test.ts,
// which also keeps the block off pages that end with their own "See also" list.
export type RelatedLink = { href: string; label: string };

const L = {
  features: { href: '/features/', label: 'Features' },
  howItWorks: { href: '/how-it-works/', label: 'How it works' },
  alternatives: { href: '/alternatives/', label: 'Alternatives' },
  cwv: { href: '/core-web-vitals/', label: 'Core Web Vitals' },
  examples: { href: '/examples/', label: 'Filter examples' },
  analyze: { href: '/analyze/', label: 'PageSpeed analyzer' },
  upgrade: { href: '/docs/upgrade/', label: 'Upgrade and migrate' },
  stillMaintained: { href: '/mod-pagespeed-still-maintained/', label: 'Is it still maintained?' },
  packs: { href: '/packs/', label: 'Rule packs' },
  platform: { href: '/platform/', label: 'Transform packs' },
  edgeSeo: { href: '/platform/edge-seo/', label: 'Edge SEO' },
  consent: { href: '/platform/consent/', label: 'Consent enforcement' },
  psi: { href: '/pagespeed-insights/', label: 'PageSpeed Insights' },
  psopt: { href: '/pagespeed-optimization/', label: 'PageSpeed optimization' },
} satisfies Record<string, RelatedLink>;

export const relatedLinks: Record<string, RelatedLink[]> = {
  '/features/': [L.howItWorks, L.examples, L.cwv],
  '/how-it-works/': [L.features, L.cwv, L.alternatives],
  '/psol/': [L.howItWorks, L.stillMaintained, L.upgrade, L.psopt],
  '/about/': [L.features, L.howItWorks, L.stillMaintained],
  '/platform/consent/': [L.platform, L.edgeSeo, L.packs],
  '/platform/edge-seo/': [L.platform, L.consent, L.packs],
  '/pagespeed-markers/': [L.psi, L.analyze, L.stillMaintained],
  '/pagespeed-optimization/': [L.psi, L.cwv, L.features, L.alternatives],
};

// The links for a page path, or none. Accepts the path with or without its
// trailing slash.
export function relatedFor(path: string): RelatedLink[] {
  const key = path.endsWith('/') ? path : `${path}/`;
  return relatedLinks[key] ?? [];
}
