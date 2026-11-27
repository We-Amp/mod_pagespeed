// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Core Web Vitals metric-pillar content (D3). Each entry renders a deep
// per-metric page at /core-web-vitals/<metric>/ via src/pages/core-web-vitals/[metric].astro,
// sitting between the /core-web-vitals/ hub and the 15 per-platform fix-spoke posts.
//
// Content authored + AI-tell-stripped + ship-consensus-reviewed by the
// `cwv-altseeker-pillars` workflow; spoke links are derived deterministically
// from the maps below so the pillar never drifts from the hub or the spokes.

export interface CwvCause {
  title: string;
  detail: string; // may contain inline <a href>, <code>, <strong>
}

export interface CwvFix {
  heading: string;
  body: string; // may contain inline <a href>, <code>, <strong>
  layer: 'server' | 'application' | 'both';
}

export interface CwvFaq {
  q: string;
  a: string;
}

export interface CwvSeeAlso {
  href: string;
  text: string;
  note?: string;
}

export interface CwvPillar {
  metric: 'lcp' | 'cls' | 'inp';
  abbr: string;
  name: string;
  slug: string; // e.g. /core-web-vitals/lcp/
  title: string;
  description: string;
  h1: string;
  // mod_pagespeed filters that act on this metric at the server, shown in the
  // "Fix it at the server" box; names must exist in src/data/filters.ts.
  serverFilters?: string[];
  intro: string; // may contain inline <a href>, <code>, <strong>
  thresholds: { good: string; poor: string; unit: string };
  measures: string;
  causes: CwvCause[];
  fixes: CwvFix[];
  faq: CwvFaq[];
  seeAlso: CwvSeeAlso[];
}

// Platform spokes — single source of truth shared with core-web-vitals.astro.
export const platforms = [
  { key: 'wordpress', label: 'WordPress' },
  { key: 'woocommerce', label: 'WooCommerce' },
  { key: 'magento', label: 'Magento' },
  { key: 'nginx', label: 'nginx' },
  { key: 'aspnet-core', label: 'ASP.NET Core' },
] as const;

// Per-spoke post titles, keyed by `${metric}-${platform}` — verbatim from each
// post's frontmatter (mirrors the map in core-web-vitals.astro).
export const spokeTitles: Record<string, string> = {
  'lcp-wordpress': 'WordPress LCP: the server-layer fix',
  'lcp-woocommerce': 'WooCommerce LCP: shrink the gallery first',
  'lcp-magento': 'LCP on Magento without breaking RequireJS',
  'lcp-nginx': 'nginx LCP: transport, not content',
  'lcp-aspnet-core': 'ASP.NET Core LCP: Razor vs Blazor',
  'cls-wordpress': 'WordPress CLS: dimensions at the server',
  'cls-woocommerce': 'WooCommerce CLS: lock the gallery',
  'cls-magento': 'Magento CLS starts with Fotorama',
  'cls-nginx': 'nginx CLS via insert_image_dimensions',
  'cls-aspnet-core': 'ASP.NET Core CLS: pre-size async holes',
  'inp-wordpress': 'WordPress INP: plugin discipline',
  'inp-woocommerce': 'INP on WooCommerce: where the cost lives',
  'inp-magento': 'Magento INP: Hyvä matters more',
  'inp-nginx': 'nginx INP: a CMS-agnostic plan',
  'inp-aspnet-core': 'INP on ASP.NET Core: Razor vs Blazor',
};

export const spokeFor = (metric: string) =>
  platforms.map((p) => ({
    platform: p.label,
    href: `/blog/fix-${metric}-${p.key}-2026/`,
    title: spokeTitles[`${metric}-${p.key}`],
  }));

// Populated from the workflow's ship-ready output (see [metric].astro).
export const pillars: CwvPillar[] = [
  {
    metric: 'lcp',
    abbr: 'LCP',
    name: 'Largest Contentful Paint',
    slug: '/core-web-vitals/lcp/',
    title: 'How to fix Largest Contentful Paint (LCP) in 2026',
    description:
      'Fix LCP (Largest Contentful Paint) by phase: diagnose TTFB, load delay, load duration, and render delay, then fix it at the server layer and in your app.',
    h1: 'How to fix Largest Contentful Paint (LCP)',
    serverFilters: [
      'prioritize_critical_css',
      'convert_jpeg_to_webp',
      'resize_images',
      'inline_preview_images',
      'hint_preload_subresources',
    ],
    intro:
      '<p><strong>Largest Contentful Paint (LCP)</strong> measures how long the largest element in the viewport &mdash; usually a hero image, a video poster, a banner, or the first heading &mdash; takes to render during load. It is one of the three Core Web Vitals, and Google scores it on <strong>field</strong> data: the 75th percentile of real Chrome users over the trailing 28 days, from the CrUX dataset. A page is good under 2.5 s and poor over 4 s.</p><p>There is no single fix for LCP, because it is not one number &mdash; it is four. PageSpeed Insights splits the LCP element into time to first byte, resource load delay, resource load duration, and element render delay. Each phase has different causes and different owners: some live in your backend, some in how the markup is delivered, some in your own application code. This page walks the four phases, then maps fixes to them, including the cases a server-layer optimizer cannot touch. When you are ready, <a href="/analyze/">analyze a page</a> to see which phase is the long pole.</p>',
    thresholds: {
      good: 'Under 2.5 s',
      poor: 'Over 4 s',
      unit: '(at the 75th percentile)',
    },
    measures:
      "Largest Contentful Paint is the render time of the largest element visible in the viewport during load &mdash; often a hero image, a video poster, a banner, or the first heading (the H1). The <strong>field</strong> figure comes from Chrome's CrUX dataset (real users over the trailing 28 days) and is what Google ranks on; the <strong>lab</strong> figure comes from Lighthouse and PageSpeed Insights, a single synthetic test. The two routinely disagree, because real visitors run slower hardware on slower connections than the test machine &mdash; optimize toward the field number. To make LCP diagnosable, PSI breaks the LCP element into four phases: <strong>TTFB</strong> (time to first byte, how long the backend takes to start sending HTML), <strong>resource load delay</strong> (how long before the browser starts fetching the LCP resource), <strong>resource load duration</strong> (how long that resource takes to download), and <strong>element render delay</strong> (how long after the resource arrives before the element paints). Fixing LCP means finding which phase dominates and fixing that one; the others are wasted effort.",
    causes: [
      {
        title: 'The LCP image is discovered late (resource load delay)',
        detail:
          'The hero/LCP image is not preloaded, so the browser does not learn it exists until it has parsed the HTML and, often, the CSS that references it. The fetch starts hundreds of milliseconds late. A <code>&lt;link rel=preload as=image&gt;</code> hint plus <code>fetchpriority=high</code> lets the browser start the download as soon as it sees the HTML.',
      },
      {
        title: 'Render-blocking CSS delays first paint (element render delay)',
        detail:
          'A chain of render-blocking stylesheets must download and parse before the browser will paint anything, so even an image that arrived on time cannot render until the CSS chain clears. Inlining the critical above-the-fold CSS removes that dependency.',
      },
      {
        title: 'A large unoptimized image takes too long to download (resource load duration)',
        detail:
          'A multi-megapixel JPEG or PNG served at full size is the LCP resource on most pages. Transcoding it to WebP or AVIF and serving a variant sized to the viewport cuts the bytes, so the same image finishes downloading sooner.',
      },
      {
        title: 'TTFB is the long pole (the backend is slow to first byte)',
        detail:
          'If the backend is slow to send the first byte, every later phase starts late and LCP cannot be good no matter what you do to the image. <strong>No HTML rewriting fixes this</strong> &mdash; fix the backend first: caching, a faster origin, edge HTML caching. An origin-side optimizer cannot make a slow application respond faster.',
      },
      {
        title: 'nginx transport bottlenecks queue the LCP image',
        detail:
          'On nginx specifically: on-the-fly gzip instead of pre-compressed assets burns CPU per request; missing HTTP/2 means HTTP/1.1 head-of-line blocking queues the LCP image behind CSS and JS; and a missing <code>Cache-Control: s-maxage</code> leaks HTML onto the slow path instead of serving it from cache. All three inflate either TTFB or load delay.',
      },
      {
        title: 'A text LCP waits on a web font, or the optimizer cannot see the element',
        detail:
          'When the LCP element is text, a web font loaded with <code>font-display: swap</code> can add render delay. And when the LCP element is a client-rendered SPA node or a third-party embed, an origin-side optimizer cannot see it in the response markup, so it cannot preload or rewrite it &mdash; that fix is application-layer.',
      },
    ],
    fixes: [
      {
        heading: 'Diagnose first: find the LCP element and its long-pole phase',
        body: 'Start in the field, not the lab. Find failing URLs in Search Console (CrUX), then run one through <a href="/analyze/">the analyzer</a> or PageSpeed Insights to identify the LCP element and read its four-phase breakdown (TTFB, load delay, load duration, render delay). Reproduce under throttling, fix the phase that dominates, then re-measure in the field. Skipping this step is how teams optimize an image when the real long pole was TTFB.',
        layer: 'both',
      },
      {
        heading: 'Preload the LCP image so the browser fetches it early',
        body: 'This attacks resource load delay. The optimizer worker gives the detected LCP candidate image a <code>&lt;link rel=preload as=image&gt;</code> hint plus <code>fetchpriority=high</code>, so the browser starts the fetch earlier instead of discovering the image late &mdash; enabled by default. The worker can also send <strong>103 Early Hints</strong> with <code>Link: rel=preload</code> headers before the origin responds, plus preconnect for discovered third-party origins. In the <a href="/alternatives/mod-pagespeed/">module</a> (Apache/nginx) the equivalent filters are <code>hint_preload_subresources</code> and <code>inline_preview_images</code> (an LQIP placeholder).',
        layer: 'server',
      },
      {
        heading: 'Inline critical CSS to remove the render-blocking chain',
        body: 'This attacks element render delay. The optimizer worker extracts heuristic <a href="/blog/critical-css-heuristics/">critical CSS</a> with no headless browser &mdash; it scans the HTML and matches selectors against the DOM (first 300 body elements; fixed-position elements; header/nav/hero patterns; <code>html</code>/<code>body</code>/<code>:root</code>/universal selectors) &mdash; then injects the result as a <code>&lt;style&gt;</code> tag before <code>&lt;/head&gt;</code>, removing render-blocking stylesheet requests. The module does the same via <code>prioritize_critical_css</code>. An optional <code>--enable-browser-analysis</code> pipeline adds coverage validation and async CSS loading.',
        layer: 'server',
      },
      {
        heading: 'Shrink the LCP image: WebP/AVIF + viewport-aware variants',
        body: 'This attacks resource load duration. The optimizer worker transcodes images to WebP and AVIF based on the client\'s <code>Accept</code> header and serves <a href="/blog/viewport-aware-image-optimization/">viewport-aware responsive variants</a> (mobile/tablet/desktop, 1x/2x), so the hero downloads sooner. A single decode produces up to 37 cache variants, and every variant is verified against the original with SSIMULACRA2 before it is cached. The module also transcodes to WebP and AVIF. Optimization is transparent: no URL rewrites, no markup changes, no build-pipeline changes.',
        layer: 'server',
      },
      {
        heading: 'Fix TTFB first if the backend is the long pole',
        body: 'If the four-phase breakdown shows <strong>TTFB</strong> dominating, the rewriter cannot help &mdash; the backend must be fixed first. On nginx that means serving pre-compressed assets instead of on-the-fly gzip, enabling HTTP/2 so the LCP image is not queued behind CSS/JS by HTTP/1.1 head-of-line blocking, and adding <code>Cache-Control: s-maxage</code> so HTML is cached rather than regenerated per request. mod_pagespeed serves the origin immediately on a cache miss and optimizes the variant out of the request path, but it does not make a slow application respond faster.',
        layer: 'both',
      },
      {
        heading: 'For SPA, third-party, and font-driven LCP, fix it in the app',
        body: 'Some LCP causes are out of reach for an origin-side optimizer. mod_pagespeed <strong>cannot</strong> rewrite third-party iframe contents, does not see the runtime DOM of client-rendered SPAs, and does not manage your font-loading strategy. If your LCP element is rendered client-side, embedded from a third party, or a text block waiting on <code>font-display: swap</code>, the fix lives in your application &mdash; server-render the LCP node, preload the font, or move the embed off the critical path.',
        layer: 'application',
      },
    ],
    faq: [
      {
        q: 'What is a good LCP score?',
        a: "LCP is good under 2.5 seconds and poor over 4 seconds; the band between 2.5 s and 4 s is 'needs improvement.' The score that counts is measured at the 75th percentile of real users in the field (Chrome's CrUX dataset, trailing 28 days), not a single lab test. Meeting 2.5 s at the 75th percentile means roughly three of every four visits load the largest element quickly.",
      },
      {
        q: 'How do I fix LCP?',
        a: 'Find which of the four phases is the long pole, then fix that one. If the LCP image is discovered late, preload it with <code>&lt;link rel=preload as=image&gt;</code> and <code>fetchpriority=high</code>. If render-blocking CSS is the delay, inline the critical above-the-fold CSS. If the image is too heavy, transcode it to WebP/AVIF and serve a viewport-sized variant. If TTFB dominates, fix the backend first. mod_pagespeed does the first three automatically at the server layer; the TTFB fix and any SPA/font fix are yours.',
      },
      {
        q: 'Why do my lab and field LCP numbers disagree?',
        a: 'Lab data is a single synthetic run from Lighthouse or PageSpeed Insights on a fast test machine; field data comes from real Chrome users on slower hardware and slower connections, aggregated over the trailing 28 days in CrUX. The field number is almost always worse, and it is the one Google ranks on. Optimize toward the field figure, and use the lab breakdown only to diagnose which phase to attack.',
      },
      {
        q: 'What are the four phases of LCP?',
        a: 'PageSpeed Insights splits LCP into TTFB (how long the backend takes to send the first byte), resource load delay (how long before the browser starts fetching the LCP resource), resource load duration (how long that resource takes to download), and element render delay (how long after it arrives before the element paints). Each phase has different causes, so diagnosing the dominant phase tells you which fix will move the number.',
      },
      {
        q: 'Can a server-side optimizer fix LCP?',
        a: 'Yes, for three of the four phases. mod_pagespeed inlines critical CSS to cut render delay, transcodes and right-sizes the hero image to cut load duration, and preloads the detected LCP image to cut load delay &mdash; each attacks a distinct part of the timing breakdown, all at the server layer below the CMS. It cannot fix LCP when TTFB is the long pole (fix the backend first), and it cannot see a client-rendered SPA node, a third-party iframe, or your font-loading strategy.',
      },
      {
        q: 'Should I lazy-load the LCP image?',
        a: 'No. The LCP image is, by definition, in the viewport at load, so lazy-loading it delays its discovery and download and makes LCP worse. Lazy-loading belongs on off-screen images. mod_pagespeed applies <code>loading=lazy</code> only to off-screen <code>&lt;img&gt;</code> and <code>&lt;iframe&gt;</code> elements and instead preloads the detected LCP candidate &mdash; both enabled by default.',
      },
    ],
    seeAlso: [
      {
        href: '/core-web-vitals/',
        text: 'Core Web Vitals: LCP, CLS, and INP',
        note: 'the hub, with per-platform fix guides for all three metrics',
      },
      {
        href: '/analyze/',
        text: 'Analyze a page',
        note: 'measure your LCP and see which of the four phases is the long pole',
      },
      {
        href: '/blog/critical-css-heuristics/',
        text: 'Critical CSS without a headless browser',
        note: 'the mechanism behind the render-delay fix above',
      },
      {
        href: '/blog/viewport-aware-image-optimization/',
        text: 'Viewport-aware image optimization',
        note: "WebP/AVIF and responsive variants that shrink the hero's load duration",
      },
      {
        href: '/how-it-works/async-rewriting/',
        text: 'How the optimizer avoids adding latency',
        note: 'optimization runs off the request path, so the preload and critical CSS never wait on it',
      },
    ],
  },
  {
    metric: 'cls',
    abbr: 'CLS',
    name: 'Cumulative Layout Shift',
    slug: '/core-web-vitals/cls/',
    title: 'How to fix Cumulative Layout Shift (CLS) in 2026',
    description:
      'Fix CLS (Cumulative Layout Shift): reserve space with server-set image dimensions and critical CSS, then handle the font, iframe, and JS-injection cases.',
    h1: 'How to fix Cumulative Layout Shift (CLS)',
    serverFilters: ['insert_image_dimensions', 'prioritize_critical_css', 'extend_cache'],
    intro:
      '<p>CLS is a layout-reservation problem, not a loading-speed problem. The page jumps because something arrived later than the layout assumed: an image with no reserved box, a font that reflowed the text, a banner that pushed content down. The fix is not "load faster"; it is "reserve the right space before the late thing arrives." With that framing, most CLS work is mechanical.</p><p>This page covers what CLS measures, how to find the element that owns the shift, and how to fix it, split between what a <a href="/features/">server-layer optimizer</a> handles for you and what only your own CSS and markup can. The largest single cause, images without explicit dimensions, mod_pagespeed fixes automatically; the rest needs reserved space you have to write. To see your own numbers, <a href="/analyze/">analyze a page</a> first.</p>',
    thresholds: {
      good: 'under 0.1',
      poor: '0.25 and up',
      unit: '(unitless, p75 field)',
    },
    measures:
      "<p>Cumulative Layout Shift is the sum of unexpected layout shifts during load, weighted by how much of the viewport moved and how far it moved. Each shift scores as <strong>impact fraction &times; distance fraction</strong>: the impact fraction is the share of the viewport the shifting element occupies across its before and after positions; the distance fraction is the greatest distance any unstable element moved divided by the viewport's largest dimension. Both terms are fractions, which is why CLS is unitless. Shifts are grouped into session windows, and the heaviest window sets the page's value, so one bad jump matters more than many tiny ones spread out. A shift within 500&nbsp;ms of, and attributable to, a user input does not count: expanding an accordion you just clicked is fine; the page lurching on its own is not.</p><p>The number you are scored on is the field 75th-percentile CLS from CrUX: real Chrome users over the trailing window. <strong>Under 0.1 is good, 0.1&ndash;0.25 needs improvement, and 0.25 and up is poor.</strong> Lighthouse and PageSpeed Insights show a single lab run, and the two often disagree: the lab serves HTML faster than real users, so font swaps and lazy-image jumps fire in the field but never in the lab. When they conflict, use the field number, which is what Google ranks on.</p>",
    causes: [
      {
        title: 'Images and embeds with no width and height',
        detail:
          'The most common cause, and the one Web Almanac surveys flag on a majority of pages. With no <code>width</code>/<code>height</code> attribute the browser allocates <strong>zero</strong> space, paints the surrounding text, then jumps the layout by exactly the rendered image height when the bytes arrive. Every paragraph below the image moves.',
      },
      {
        title: 'Web fonts that reflow text on swap',
        detail:
          "A font loaded with <code>font-display: swap</code> paints a fallback first, then swaps in the web font. Because the fallback's metrics (line-height, x-height, character width) differ, every text block re-flows by a few pixels in both axes when the swap fires. This is a browser-timing concern, so it shows up in the field even when the lab reports a clean 0.",
      },
      {
        title: 'Content injected above what the user is reading',
        detail:
          'JS-injected content, lightboxes, content-loader scripts, cookie banners, and ad slots that mount into the document flow push everything below them down. Anything inserted above the current scroll position after first paint records a shift for the full distance it displaces.',
      },
      {
        title: 'Stale cached HTML referencing mismatched image dimensions',
        detail:
          'An <code>nginx</code> <code>proxy_cache</code> or a CDN edge serves HTML that embeds old (or no) dimensions while the image at that URL has been re-edited to a new size. The reserved box and the actual image no longer agree, so the layout snaps when the real bytes load. This is worst when the HTML TTL is long and the image refresh path is faster than the HTML one.',
      },
      {
        title: 'Third-party iframes that start at zero height',
        detail:
          'YouTube, Twitter/X, and Instagram embeds render an iframe that begins at zero or a placeholder height and expands when its own content loads, shoving the page below it. The parent document has no way to know the final height in advance, so the space is never reserved.',
      },
    ],
    fixes: [
      {
        heading: 'Diagnose before you change anything: find the element that owns the shift',
        body: '<p>Start in the field, not the lab. <strong>Search Console &rarr; Core Web Vitals</strong> tells you which URL groups fail CLS on real users. Then reproduce it: run the URL through <a href="/analyze/">the analyzer</a> or PageSpeed Insights and read the <strong>"Avoid large layout shifts"</strong> diagnostic, which lists the specific elements and their score contribution.</p><p>Confirm it locally in Chrome DevTools: Performance panel &rarr; settings cog &rarr; enable <strong>Web Vitals</strong> and <strong>Layout Shift Regions</strong>, throttle to Slow 4G, and reload. Each shift is a red rectangle; hover the markers in the Timings track to name the DOM node. For repeatable numbers, load the <code>web-vitals</code> library and log <code>onCLS</code> with attribution. Get three things before touching code: a baseline score, the element that owns most of it, and which cause it maps to.</p>',
        layer: 'both',
      },
      {
        heading: 'Reserve image space automatically with server-injected dimensions',
        body: '<p>CLS is the metric mod_pagespeed moves most, and this is why. The <a href="/features/">optimizer worker injects explicit <code>width</code> and <code>height</code></a> on <code>&lt;img&gt;</code> tags that lack them, using dimensions read from the cached image data, so the browser reserves the correct layout box before pixels arrive. It is <strong>enabled by default</strong>. In <a href="/blog/fix-cls-nginx-2026/">the module</a> the same result comes from <code>insert_image_dimensions</code>, with <code>lazyload_images</code> deferring offscreen images (which must still carry explicit <code>width</code>/<code>height</code> so their space stays reserved).</p><p>This runs below the CMS, so one configuration fixes every URL the origin serves: hand-written HTML, classic-editor content, page-builder markup, with no markup changes or build step. Pair it with a single site rule, <code>img { max-width: 100%; height: auto; }</code>, so the browser computes the aspect-ratio box from the inserted attributes. Test it on responsive templates first: writing fixed dimensions can interfere with some CSS-driven responsive layouts.</p>',
        layer: 'server',
      },
      {
        heading: 'Stabilize early layout with inlined critical CSS',
        body: '<p>A render-blocking stylesheet chain leaves a window where the browser paints unstyled or partially-styled content, then re-lays it out when the CSS lands: a flash-of-unstyled-content shift on top of the font and image ones. The optimizer worker\'s <a href="/blog/critical-css-heuristics/">heuristic critical CSS extraction</a> scans the HTML, matches selectors against the above-the-fold DOM, and injects the result as a <code>&lt;style&gt;</code> tag before <code>&lt;/head&gt;</code>, with no headless browser required, so above-the-fold styling is available without waiting on an external stylesheet. In the module this is <code>prioritize_critical_css</code>, which shrinks the FOUC window during which layout-affecting CSS and font swaps arrive.</p>',
        layer: 'server',
      },
      {
        heading: 'Keep cached HTML and image dimensions in sync',
        body: "<p>If a proxy or CDN serves HTML that references an image at the wrong size, the reserved box is wrong no matter who set it. Content-hash your asset URLs so a re-edited image gets a new URL the HTML must reference, which the module's <code>extend_cache</code> does, and keep HTML short-cached while assets are long-cached and immutable. On a cache miss mod_pagespeed serves the origin immediately and regenerates variants out of the request path, so dimensions stay correct without stalling the response. URL <code>PURGE</code> removes all variants for a URL when you need to force a refresh.</p>",
        layer: 'both',
      },
      {
        heading: 'Tame web-font swap shifts',
        body: '<p>The rewriter does not fix font-swap reflow; that is a browser concern. Self-host your fonts and switch <code>font-display: swap</code> to <code>font-display: optional</code>: the browser gives the web font a brief window to arrive, and if it misses, the fallback stays for the whole page lifetime, so no swap fires after first paint and CLS drops. Where you must keep <code>swap</code>, preload the font file and add <code>size-adjust</code> / <code>@font-face</code> metric overrides so the fallback occupies the same space as the web font. This lives entirely in your own CSS.</p>',
        layer: 'application',
      },
      {
        heading: 'Reserve space for iframes and JS-injected content',
        body: '<p>mod_pagespeed cannot size a third-party embed (it does not know a YouTube iframe\'s intrinsic height) or content that JavaScript injects after the initial response; those need CSS-level reserved space you write. Wrap embeds in a fixed-ratio box: <code>&lt;div style="aspect-ratio: 16 / 9"&gt;</code> with the iframe at 100% width and height. Give ad slots a <code>min-height</code> matching the largest creative, pin cookie banners with <code>position: fixed</code> instead of letting them push content, and put <code>aspect-ratio</code> on any container whose child loads late. Never let a late element decide its own size at runtime.</p>',
        layer: 'application',
      },
    ],
    faq: [
      {
        q: 'What is a good CLS score?',
        a: 'Under 0.1 is good, 0.1 to 0.25 needs improvement, and 0.25 and up is poor. The score that matters is the 75th-percentile value from real Chrome users in the field (CrUX), not a single Lighthouse lab run. Meeting 0.1 at the 75th percentile means three of four visits see a stable layout.',
      },
      {
        q: 'How do I fix CLS?',
        a: 'Diagnose first: find the element that owns the shift in Search Console and the PageSpeed Insights "Avoid large layout shifts" diagnostic. Then reserve space for it. The biggest cause is images without dimensions: <a href="/features/">mod_pagespeed injects <code>width</code> and <code>height</code> automatically</a> at the server layer, enabled by default, so the browser holds the box before the image loads. Fonts, iframes, and JavaScript-injected content need reserved space in your own CSS.',
      },
      {
        q: 'Why is my field CLS worse than my Lighthouse CLS?',
        a: 'Because the lab serves HTML faster than real users do, so font swaps and lazy-image jumps fire in the field but not in the single lab run. A lab CLS of 0 with a failing field score is common and expected. Use the field number, which is what Google ranks on, and optimize toward it rather than over-tuning to the lab.',
      },
      {
        q: 'Does setting image width and height actually fix CLS?',
        a: 'For image-driven shifts, yes: it is the canonical fix. With explicit <code>width</code> and <code>height</code> (plus a <code>img { max-width: 100%; height: auto; }</code> rule) the browser computes the aspect-ratio box and reserves the slot before pixels arrive, so no jump occurs. mod_pagespeed does this automatically from cached image data for every <code>&lt;img&gt;</code> that lacks dimensions, so you do not have to audit markup by hand.',
      },
      {
        q: 'Can a server-side optimizer fix all of my CLS?',
        a: "No, and we are precise about that. It fixes the largest cause, unsized images, automatically, and inlined critical CSS stabilizes early layout. It does <strong>not</strong> fix shifts from third-party iframes (the rewriter does not know an embed's intrinsic height), web-font swap reflow (a browser concern), or content injected by JavaScript after the initial response. Those need CSS-level reserved space you write yourself.",
      },
      {
        q: 'Do user-triggered layout changes count against CLS?',
        a: 'No. A shift within 500 ms of, and attributable to, a user input is excluded: expanding an accordion or opening a menu you just clicked does not score. CLS only counts unexpected shifts the user did not cause. A shift that merely happens to land in that 500 ms window but was not triggered by the interaction still counts.',
      },
    ],
    seeAlso: [
      {
        href: '/core-web-vitals/',
        text: 'Core Web Vitals: the hub',
        note: 'how CLS fits with LCP and INP, and per-platform fix guides',
      },
      {
        href: '/analyze/',
        text: 'Analyze a page',
        note: 'see what is driving your CLS before you change anything',
      },
      {
        href: '/blog/critical-css-heuristics/',
        text: 'Critical CSS without a headless browser',
        note: 'the heuristic behind the early-layout stabilization above',
      },
      {
        href: '/features/',
        text: 'mod_pagespeed features',
        note: 'server-injected image dimensions, critical CSS, and variant-aware caching',
      },
    ],
  },
  {
    metric: 'inp',
    abbr: 'INP',
    name: 'Interaction to Next Paint',
    slug: '/core-web-vitals/inp/',
    title: 'How to fix Interaction to Next Paint (INP) in 2026',
    description:
      "Fix INP (Interaction to Next Paint): the input-delay, processing, and presentation phases, the app-layer fixes, and where a server optimizer can't help.",
    h1: 'Interaction to Next Paint (INP): diagnose it, then fix it',
    intro:
      '<p><strong>Interaction to Next Paint (INP)</strong> is the Core Web Vital that measures responsiveness: how long the user waits between a click, tap, or keypress and the next frame the browser paints in response. It replaced First Input Delay in March 2024. Unlike its predecessor it counts the whole interaction: not just the delay before your handler runs, but the handler itself and the paint that follows. That makes it the hardest vital to game, and the one most sites still fail.</p><p>Know this before you start: INP is the metric where a server-layer optimizer helps the least. The dominant cause is JavaScript running on the main thread, and that JavaScript lives in <em>your application code</em>, not at the server. This page gives you the diagnosis workflow and fixes split by where they actually live: the few things <a href="/features/">mod_pagespeed</a> can do at the server layer, and the larger set of changes only your app can make. Then it hands you off to the per-platform guide that names the failure modes on your stack.</p>',
    thresholds: {
      good: '≤ 200',
      poor: '> 500',
      unit: 'ms (p75, field)',
    },
    measures:
      '<p>INP is the worst-case latency between a user input (a click, tap, or keypress) and the next frame the browser paints in response. Chrome tracks <strong>every</strong> interaction over the page\'s lifetime and reports the 98th-percentile slowest one as that page\'s INP, so a single janky click late in a session can set the number. The good threshold is <strong>≤ 200 ms</strong> at the 75th percentile of real users; "needs improvement" runs from 200 ms to 500 ms; over 500 ms is poor.</p><p>Each interaction has three phases. <strong>Input delay</strong> is the time before your event handler starts running, usually the main thread being busy with other JavaScript. <strong>Processing time</strong> is the handler itself doing its work. <strong>Presentation delay</strong> is the time to compute layout and paint the resulting frame. Diagnosis is about finding which phase dominates the worst interaction.</p><p>One thing matters more for INP than for any other vital: <strong>use the field number, not the lab number</strong>. Real-user INP comes from CrUX (and your Search Console Core Web Vitals report). The lab approximation from Lighthouse / PageSpeed Insights is unreliable because the lab issues no human input: there is no interaction to measure, so it estimates. Optimize against the field figure; it is the only one Google ranks on and the only one worth chasing. <a href="/analyze/">Analyze a page</a> to see where it stands.</p>',
    causes: [
      {
        title: 'Long JavaScript tasks blocking the main thread',
        detail:
          'The dominant cause, and it lives in <strong>your application code</strong>. Event handlers, framework hydration, and startup work run as long tasks that occupy the main thread; an interaction that arrives mid-task waits behind it before the handler can start. This shows up as <em>input delay</em> in a DevTools trace.',
      },
      {
        title: 'Heavy event handlers',
        detail:
          'The handler itself doing too much. Classic offenders: jQuery validation re-running on every keystroke, hand-written synchronous DOM thrashing, or parsing a large JSON blob inside a click handler. This is <em>processing time</em>, and it is bounded below by whatever the slowest handler the user can trigger actually does.',
      },
      {
        title: 'Architectures that require a round-trip before paint',
        detail:
          'Some frameworks are INP-bound by design. <strong>Blazor Server</strong> sends every interaction over a SignalR connection, so INP is bounded below by network latency plus server render time: a transatlantic user can sit at the "needs improvement" boundary before any work is done. This is architectural, not an optimization problem.',
      },
      {
        title: 'Slow upstream behind interactive widgets',
        detail:
          'Autocomplete, infinite scroll, and "load more" fire a request on the user\'s input. If the <code>/api/*</code> endpoint has long-tail TTFB (un-tuned upstream, cold cache, an unindexed query) INP includes that wait whenever the handler is synchronous. The fix is in the upstream, not the page.',
      },
      {
        title: "Third-party scripts you don't control",
        detail:
          "Intercom, HotJar, GTM-injected analytics, and chat widgets attach their own handlers and run in their own scheduler. They compete for the main thread and frequently sit on top of INP attribution. You can defer or remove them, but you can't rewrite them.",
      },
    ],
    fixes: [
      {
        heading: 'Profile the worst interaction before changing anything',
        body: 'INP is a worst-case metric: you cannot fix it without knowing <em>which</em> interaction records it. Record a real flow in the Chrome DevTools <strong>Performance</strong> panel and read the <strong>Interactions</strong> track for the slowest input, or log <code>onINP</code> from the <code>web-vitals</code> library in attribution mode to name the responsible script and handler. Replicate it under CPU and network throttling. The output you want is a specific interaction, a specific handler or endpoint, and whether the cost is input delay, processing, or paint.',
        layer: 'application',
      },
      {
        heading: 'Reduce startup JavaScript competing for the main thread',
        body: "This is the server layer's narrow but real lever. mod_pagespeed can <code>rewrite_javascript</code> (minify, safe by construction: no AST transforms or renaming, and it only writes a smaller variant), <code>combine_javascript</code> (concatenate files to cut per-file parser setup), and <code>defer_javascript</code> (defer scripts it can prove safe; the optimizer worker uses browser analysis). That moves parser-blocking work off the critical path so it no longer collides with the first click. It helps when the origin serves heavy non-minified or legacy JS, and little when you already ship a lean, code-split SPA bundle. <code>defer_javascript</code> is marked <strong>Test first</strong> because it changes execution order: profile, then stage it.",
        layer: 'server',
      },
      {
        heading: 'Shrink and split the handlers themselves',
        body: "This is where most INP wins live, and it is application work. Keep handlers lightweight; break long tasks up and yield to the main thread (<code>scheduler.yield()</code>) so a long handler doesn't block the next paint; debounce per-keystroke work; move analytics and other non-critical callbacks out of the interaction's critical path. Replace jQuery validation that re-runs on every keystroke with native HTML5 validation. No server-layer filter can shrink a 90 ms handler; only rewriting it can.",
        layer: 'application',
      },
      {
        heading: 'Fix the upstream behind interactive widgets',
        body: "If INP attribution lands on a <code>fetch('/api/...')</code> that takes 300 ms to respond, INP is at least 300 ms and no rewriter changes that. Fix it at the source: caching, database indexing, async I/O. On nginx specifically, remove connection overhead too: set <code>keepalive</code> on upstream blocks (with <code>proxy_http_version 1.1</code> and an empty <code>Connection</code> header), raise <code>worker_connections</code> so bursts don't queue, and enable HTTP/2 to end HTTP/1.1 head-of-line blocking. These cut per-interaction connection cost; they don't shorten handler latency.",
        layer: 'both',
      },
      {
        heading: 'Defer or remove third-party scripts',
        body: "When attribution sits on Intercom, HotJar, a chat widget, or GTM-injected analytics, the rewrite layer can't help: they run in their own scheduler from another origin. Move them behind a tag manager that fires after first paint or first idle, or remove the ones that do no user-visible work. This is configuration and product decisions, not a server filter.",
        layer: 'application',
      },
      {
        heading: 'Re-architect when INP is structural',
        body: "Some INP failures cannot be optimized away. <strong>Blazor Server</strong>'s SignalR round-trip is a design floor: the fix is Blazor WebAssembly or <code>@rendermode InteractiveAuto</code>, which run in the browser. Third-party JS controls (Telerik, Syncfusion, DevExpress) are bounded by their own render path. Geographic distance from the origin adds round-trip latency to every interactive request; a CDN or co-located cache helps, but the page optimizer can't shorten the speed of light. We will not pretend a server filter fixes these.",
        layer: 'application',
      },
    ],
    faq: [
      {
        q: 'What is a good INP score?',
        a: '<strong>200 ms or less</strong> is good, measured at the 75th percentile of your real users in the field. From 200 ms to 500 ms is "needs improvement," and anything over 500 ms is poor. Because INP reports the 98th-percentile slowest interaction over the page\'s lifetime, one consistently slow click can fail an otherwise fast page.',
      },
      {
        q: 'How do I fix INP?',
        a: 'Profile first: record a real interaction flow in the Chrome DevTools Performance panel (or log <code>onINP</code> with the <code>web-vitals</code> attribution build) to find the single worst interaction and which phase dominates it. Then fix where the cost lives: shrink and split the responsible handler, move third-party scripts out of the critical path, and fix slow <code>/api/*</code> upstreams. At the server layer, mod_pagespeed can minify, combine, and defer startup JavaScript so it stops competing for the main thread, useful when the origin serves heavy legacy JS, less so when you already ship a lean SPA bundle.',
      },
      {
        q: 'What is the difference between INP and First Input Delay (FID)?',
        a: "FID only measured the delay <em>before</em> your handler started running on the very first interaction. INP, which replaced it as a Core Web Vital in March 2024, measures the full interaction: input delay plus the handler's processing time plus the paint, across every interaction on the page, reporting the worst. That makes INP a truer measure of responsiveness, and harder to game.",
      },
      {
        q: 'Why is my lab INP different from my field INP?',
        a: 'The lab number from Lighthouse / PageSpeed Insights is unreliable because the lab issues no human input: with no interaction to measure, it estimates. Real INP comes from CrUX field data (and your Search Console Core Web Vitals report). Always optimize against the field figure; it is the only one Google ranks on.',
      },
      {
        q: 'Can mod_pagespeed fix my INP?',
        a: 'Indirectly, and only sometimes. INP measures JavaScript main-thread responsiveness, and a server-layer optimizer does not rewrite your application logic; it will not make a slow click handler fast. What it can do is reduce how much JavaScript competes for the main thread before the user interacts, by minifying (<code>rewrite_javascript</code>), combining (<code>combine_javascript</code>), and deferring scripts proven safe (<code>defer_javascript</code>). That lowers INP on pages throttled by startup script execution: a lot when the origin serves heavy non-minified JS, little when the stack already ships a lean, code-split bundle. In the module these are the named filters <code>rewrite_javascript</code>, <code>combine_javascript</code>, and <code>defer_javascript</code>. The <a href="/blog/aspnet-core-middleware/">mod_pagespeed ASP.NET Core middleware</a> reaches the same outcome through its always-on pipeline — automatic JS minification and browser-analysis-driven deferral — not a named-filter list.',
      },
      {
        q: "Why can't a server-layer tool fix Blazor Server INP?",
        a: 'Blazor Server sends every interaction over a SignalR connection: a button click is network latency plus server render time plus applying the DOM diff. INP is bounded below by that round-trip by design, so no rewriter operating on the response stream can shorten it. The fix is architectural: Blazor WebAssembly or <code>@rendermode InteractiveAuto</code>, which move execution into the browser. See the <a href="/blog/fix-inp-aspnet-core-2026/">ASP.NET Core INP guide</a> for the full Razor-versus-Blazor split.',
      },
    ],
    seeAlso: [
      {
        href: '/core-web-vitals/',
        text: 'Core Web Vitals: LCP, CLS, and INP',
        note: 'the hub, with the full metric × platform matrix',
      },
      {
        href: '/analyze/',
        text: 'Analyze a page',
        note: 'see what is driving its INP before you change anything',
      },
      {
        href: '/blog/fix-inp-nginx-2026/',
        text: 'nginx INP: a CMS-agnostic plan',
        note: 'transport tuning plus the rewrite layer for custom stacks',
      },
      {
        href: '/blog/fix-inp-aspnet-core-2026/',
        text: 'INP on ASP.NET Core: Razor vs Blazor',
        note: 'the structural Blazor Server case, explained',
      },
    ],
  },
];
