// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Site navigation data: the header menus and the footer link groups, with the
// analytics attributes their links carry. Rendered by
// src/layouts/parts/SiteHeader.astro and SiteFooter.astro.

// Primary nav (target IA): Docs (a structured, four-column menu), the tools,
// the commercial offers and the product pages are four dropdowns; Blog and Contact are flat;
// "Download & run" is the one button. Support items are paid CTAs and carry
// the cta_commercial event with offer/surface/position properties (umami
// `data-umami-event-<prop>`); other items keep their existing event names.
// At most one "New" badge anywhere (UX-15) — it lives on the AI readability
// check in Tools.
export type UmamiProps = Record<string, string>;
export type NavChild = {
  href: string;
  label: string;
  subtitle: string;
  badge?: string;
  umamiEvent?: string;
  umamiProps?: UmamiProps;
  external?: boolean;
};
export type DocsLink = { href: string; label: string; umamiEvent?: string };
export type DocsSection = { title: string; position: string; links: DocsLink[] };
// The Docs menu. Every /docs/<slug>/ href is checked against the docs
// collection by test/nav-docs-links.test.ts. Plain links, one heading per column.
export const docsHome = {
  href: '/docs/',
  label: 'Docs home',
  subtitle: 'All guides, by topic',
  umamiEvent: 'nav-docs',
};
export const docsSections: DocsSection[] = [
  {
    title: 'Get started',
    position: 'getting_started',
    links: [
      { href: '/docs/getting-started/', label: 'Getting started' },
      { href: '/docs/is-it-working/', label: 'Is it working?' },
      { href: '/docs/installation-module/', label: 'Install on Apache and nginx' },
      { href: '/docs/installation-docker/', label: 'Install with Docker' },
      { href: '/docs/helm-deployment/', label: 'Deploy with Helm' },
      { href: '/docs/install-iis/', label: 'Install on IIS' },
      { href: '/docs/aspnet-getting-started/', label: 'ASP.NET Core middleware' },
      { href: '/docs/upgrade/', label: 'Upgrade and migrate' },
    ],
  },
  {
    title: 'Configure',
    position: 'configure',
    links: [
      { href: '/docs/configuration/', label: 'Apache and nginx directives' },
      { href: '/docs/worker-configuration/', label: 'Worker configuration' },
      { href: '/docs/cache-modes/', label: 'Cache modes' },
      { href: '/docs/cdn-integration/', label: 'CDN integration' },
      { href: '/docs/filter-selection/', label: 'Filter selection' },
      { href: '/docs/filters/', label: 'Filter reference' },
    ],
  },
  {
    title: 'Rule & transform packs',
    position: 'packs',
    links: [
      { href: '/packs/', label: 'Browse rule packs', umamiEvent: 'nav-packs' },
      { href: '/docs/rule-packs/', label: 'Rule pack format' },
      { href: '/platform/', label: 'Transform packs' },
      { href: '/platform/edge-seo/', label: 'Edge SEO' },
      { href: '/platform/consent/', label: 'Consent enforcement' },
    ],
  },
  {
    title: 'Operate & reference',
    position: 'operate',
    links: [
      { href: '/docs/deployment/', label: 'Run in production' },
      { href: '/docs/monitoring/', label: 'Monitoring' },
      { href: '/docs/troubleshooting/', label: 'Troubleshooting' },
      { href: '/docs/http-api/', label: 'Worker HTTP API' },
      { href: '/docs/agent-optimize/', label: 'Markdown for AI agents' },
      { href: '/docs/release-notes/', label: 'Release notes' },
    ],
  },
];
const commercial = (offer: string, position: string): UmamiProps => ({
  offer,
  surface: 'nav',
  position,
});
// The header's own Pricing link, next to the search button on wide screens.
// Same page as the Pricing item in the Support menu; it reports its own
// position. The phone menu has the Support group's item only.
export const headerPricing = {
  href: '/pricing/',
  label: 'Pricing',
  umamiEvent: 'cta_commercial',
  umamiProps: commercial('pricing', 'header_pricing'),
};
export const navLinks: Array<{
  href?: string;
  label: string;
  badge?: string;
  umamiEvent?: string;
  docsMenu?: boolean;
  // Phone menu only: the group starts collapsed behind its label.
  mobileCollapsed?: boolean;
  children?: NavChild[];
}> = [
  { label: 'Docs', docsMenu: true },
  {
    label: 'Tools',
    mobileCollapsed: true,
    children: [
      {
        href: '/ai-readability/',
        label: 'AI readability check',
        subtitle: 'What AI crawlers see on your page (RenderPeek)',
        badge: 'New',
        umamiEvent: 'nav-ai-readability',
      },
      {
        href: '/analyze/',
        label: 'PageSpeed analyzer',
        subtitle: 'Score any URL with PageSpeed Insights',
        umamiEvent: 'nav-analyze',
      },
      {
        href: '/ai-crawler-cost/',
        label: 'AI crawler cost',
        subtitle:
          'Estimate what AI crawlers cost your server, and how many visits AI assistants send back',
        umamiEvent: 'nav-crawler-cost',
      },
      {
        href: '/examples/',
        label: 'Filter examples',
        subtitle: 'Each filter, before and after',
      },
      {
        href: 'https://we-amp.com/pagespeed_global_admin/',
        label: 'Live admin console',
        subtitle: 'The real console on our own sites',
        umamiEvent: 'nav-live-console',
        external: true,
      },
      {
        href: '/calculator/',
        label: 'Savings calculator',
        subtitle: 'Estimate your bandwidth savings',
      },
      {
        href: '/demo/',
        label: 'Demo',
        subtitle: 'Before and after, on a live site',
      },
    ],
  },
  {
    label: 'Support',
    children: [
      {
        href: '/support/',
        label: 'Support plans',
        subtitle: 'Three tiers, from the engineers who build it',
        umamiEvent: 'cta_commercial',
        umamiProps: commercial('support', 'support_plans'),
      },
      {
        href: '/pricing/',
        label: 'Pricing',
        subtitle: 'Prices for the three support tiers',
        umamiEvent: 'cta_commercial',
        umamiProps: commercial('pricing', 'nav_support_pricing'),
      },
      {
        href: '/support/#hardened-builds',
        label: 'Signed artifacts',
        subtitle: 'What is signed today, and how to verify it',
        umamiEvent: 'cta_commercial',
        umamiProps: commercial('hardened', 'hardened_builds'),
      },
      {
        href: '/hosting-partners/',
        label: 'Hosting partners',
        subtitle: 'For providers running it across a fleet',
        umamiEvent: 'cta_commercial',
        umamiProps: commercial('hosting', 'hosting_partners'),
      },
      {
        href: 'https://we-amp.com/consulting/',
        label: 'Consulting',
        subtitle: 'Performance audits and engagements (we-amp.com)',
        umamiEvent: 'cta_commercial',
        umamiProps: commercial('consulting', 'consulting'),
        external: true,
      },
    ],
  },
  {
    label: 'Product',
    mobileCollapsed: true,
    children: [
      {
        href: '/features/',
        label: 'Features',
        subtitle: 'The full feature set: images, CSS, caching',
        umamiEvent: 'nav-features',
      },
      {
        href: '/how-it-works/',
        label: 'How it works',
        subtitle: 'How a server-side optimizer works',
        umamiEvent: 'nav-how-it-works',
      },
      {
        href: '/alternatives/',
        label: 'Alternatives',
        subtitle: 'Head-to-head comparisons and migration paths',
      },
      {
        href: '/core-web-vitals/',
        label: 'Core Web Vitals',
        subtitle: 'What the metrics mean, and how we move them',
      },
    ],
  },
  { href: '/blog/', label: 'Blog' },
  { href: '/contact/', label: 'Contact', umamiEvent: 'nav-contact' },
];

// Spread helper: the umami property attributes for a link
// (`data-umami-event-offer`, `-surface`, `-position`). `positionSuffix`
// re-tags the same item where it renders elsewhere (the mobile menu).
export function umamiAttrs(props?: UmamiProps, positionSuffix = ''): Record<string, string> {
  if (!props) return {};
  return Object.fromEntries(
    Object.entries(props).map(([k, v]) => [
      `data-umami-event-${k}`,
      k === 'position' && positionSuffix ? `${v}${positionSuffix}` : v,
    ]),
  );
}

// Footer: five groups plus Legal. Tools holds the interactive tools; Guides
// holds the topic landing pages. Paid links carry cta_commercial
// with surface "footer"; links that already reported an event keep its name.
export type FooterLink = {
  href: string;
  label: string;
  umamiEvent?: string;
  umamiProps?: UmamiProps;
  external?: boolean;
};
const footerCommercial = (offer: string, position: string): UmamiProps => ({
  offer,
  surface: 'footer',
  position,
});
export const footerGroups: Array<{ title: string; links: FooterLink[] }> = [
  {
    title: 'Product',
    links: [
      { href: '/features/', label: 'Features' },
      { href: '/download/', label: 'Download & install', umamiEvent: 'cta_footer_download' },
      { href: '/how-it-works/', label: 'How it works' },
      { href: '/packs/', label: 'Rule pack catalogue' },
      {
        href: '/platform/',
        label: 'Transform packs',
        umamiEvent: 'nav-platform-footer',
      },
      { href: '/docs/release-notes/', label: 'Release notes' },
      { href: '/security/', label: 'Security' },
      {
        href: '/mod-pagespeed-still-maintained/',
        label: 'Is it still maintained?',
        umamiEvent: 'nav-still-maintained-footer',
      },
      { href: '/docs/upgrade/', label: 'Upgrade' },
      { href: '/alternatives/', label: 'Alternatives' },
      { href: '/core-web-vitals/', label: 'Core Web Vitals' },
    ],
  },
  {
    title: 'Install and docs',
    links: [
      { href: '/docs/', label: 'Docs' },
      { href: '/docs/getting-started/', label: 'Getting started' },
      { href: '/download/apt-yum/', label: 'Apache and nginx packages' },
      { href: '/docs/installation-docker/', label: 'Docker' },
      { href: '/docs/helm-deployment/', label: 'Helm' },
      { href: '/docs/cpanel/', label: 'cPanel' },
      { href: '/docs/install-iis/', label: 'IIS' },
      { href: '/docs/aspnet-getting-started/', label: 'ASP.NET Core' },
      { href: '/docs/configuration/', label: 'Configuration' },
      { href: '/docs/filters/', label: 'Filter reference' },
    ],
  },
  {
    title: 'Tools',
    links: [
      {
        href: '/ai-readability/',
        label: 'AI readability check',
        umamiEvent: 'nav-ai-readability-footer',
      },
      { href: '/analyze/', label: 'PageSpeed analyzer', umamiEvent: 'nav-analyze-footer' },
      {
        href: '/ai-crawler-cost/',
        label: 'AI crawler cost',
        umamiEvent: 'nav-crawler-cost-footer',
      },
      { href: '/examples/', label: 'Filter examples' },
      {
        href: 'https://we-amp.com/pagespeed_global_admin/',
        label: 'Live admin console',
        umamiEvent: 'nav-live-console-footer',
        external: true,
      },
      { href: '/calculator/', label: 'Savings calculator' },
      { href: '/demo/', label: 'Demo' },
    ],
  },
  {
    title: 'Guides',
    links: [
      { href: '/pagespeed-optimization/', label: 'PageSpeed optimization' },
      { href: '/pagespeed-markers/', label: 'PageSpeed markers' },
      { href: '/pagespeed-insights/', label: 'PageSpeed Insights' },
      { href: '/wordpress/', label: 'WordPress caching' },
      { href: '/aspnet-core-performance/', label: 'ASP.NET Core performance' },
      { href: '/self-hosted-image-optimization/', label: 'Self-hosted image optimization' },
      { href: '/psol/', label: 'PSOL', umamiEvent: 'nav-psol-footer' },
    ],
  },
  {
    title: 'Support and company',
    links: [
      {
        href: '/support/',
        label: 'Support plans',
        umamiEvent: 'cta_commercial',
        umamiProps: footerCommercial('support', 'support_plans'),
      },
      {
        href: '/pricing/',
        label: 'Pricing',
        umamiEvent: 'cta_commercial',
        umamiProps: footerCommercial('support', 'pricing'),
      },
      {
        href: '/hosting-partners/',
        label: 'Hosting partners',
        umamiEvent: 'cta_commercial',
        umamiProps: footerCommercial('hosting', 'hosting_partners'),
      },
      {
        href: 'https://we-amp.com/consulting/',
        label: 'Consulting (we-amp.com)',
        umamiEvent: 'cta_commercial',
        umamiProps: footerCommercial('consulting', 'consulting'),
        external: true,
      },
      {
        href: 'https://ngxpagespeed.com/',
        label: 'ngx_pagespeed install hub',
        external: true,
      },
      { href: '/contact/', label: 'Contact' },
      { href: '/about/', label: 'About' },
      { href: '/blog/', label: 'Blog' },
      {
        href: 'https://github.com/We-Amp/mod_pagespeed',
        label: 'Source on GitHub',
        external: true,
      },
    ],
  },
  {
    title: 'Legal',
    links: [
      { href: '/terms/', label: 'Terms' },
      { href: 'https://we-amp.com/privacy/', label: 'Privacy' },
      { href: '/license/', label: 'Source license' },
    ],
  },
];
