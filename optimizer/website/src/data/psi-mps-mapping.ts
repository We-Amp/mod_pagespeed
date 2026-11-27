// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * PageSpeed Insights (Lighthouse) audit → mod_pagespeed mapping.
 *
 * Two edition surfaces are modelled:
 *
 *   - **1.1** — Named filters on every server (nginx, Apache, IIS, Envoy).
 *     Config form: `pagespeed EnableFilters foo,bar;`. Filter inventory on the
 *     filters index, `/docs/filters/` (`website/src/data/filters.ts`).
 *
 *   - **2.0** — No `EnableFilters` directive. The nginx module exposes a
 *     master switch (`pagespeed on;`) plus a Cyclone cache directive
 *     (`pagespeed_cache_path …`); the worker exposes per-transform on/off
 *     flags (e.g. `--no-lazy-load-images`). Image transcoding (WebP + AVIF +
 *     resize), CSS/JS minification, and content-hashed cache extension are
 *     always-on; the only knobs that turn them off are top-level switches
 *     like `--disable-image` / `--disable-css` / `--disable-js`. Critical
 *     CSS, lazy-load, image dimensions, LCP preload, preconnect, async CSS,
 *     and script deferral are individually toggleable HTML transforms.
 *     Source of truth: `website/src/content/docs/configuration.md`.
 *
 * 2.0 also caches HTML in the Cyclone shared-memory cache when the origin
 * allows it (`Cache-Control: public, max-age=…, must-revalidate` or
 * `pagespeed_html_max_age`), so slow-origin sites get an mmap'd HIT instead
 * of an origin round-trip. 1.1 never caches HTML at the module layer; HTML
 * always passes through to the origin. This is the load-bearing
 * 2.0-vs-1.1 difference for sites with slow TTFB.
 *
 * See psi-mps-mapping.md for methodology, 1.1-vs-2.0 coverage criteria, and
 * refresh cadence.
 */

export type Confidence = 'high' | 'medium' | 'experimental';
export type Coverage = 'full' | 'partial' | 'none';
export type Edition = '1.1' | '2.0';

export interface AuditMapping {
  /** PSI v5 / Lighthouse audit ID (stable identifier). */
  auditId: string;
  /** Human-friendly title — does not need to match Lighthouse exactly. */
  auditTitle: string;
  /** Whether MPS can address this audit. */
  coverage: Coverage;
  /** Canonical MPS filter names (1.1 naming). Empty for coverage=none. */
  filters: string[];
  /** How reliably the listed filters help. */
  confidence: Confidence;
  /** 1–2 sentence customer-facing explanation. */
  explanation: string;
  /** Optional: when this WON'T help, or risks. */
  caveats?: string;
  /** Which product editions cover this audit. */
  availableIn: Edition[];
  /** Optional: pagespeed directive(s) that enable the relevant filters (1.1 nginx syntax). */
  snippet11?: string;
  /**
   * Optional: 2.0 nginx + worker configuration that enables the relevant
   * transforms. 2.0 has no `EnableFilters` directive; the snippet is a mix
   * of `pagespeed on;`, `pagespeed_cache_path`, optional nginx directives,
   * and worker CLI flags (`factory_worker --…`). Comments mark which line
   * goes where. Undefined when no specific 2.0 directive applies (the
   * transform is always-on and the master switch alone enables it).
   */
  snippet20?: string;
}

export const PSI_MPS_MAPPING: AuditMapping[] = [
  // -------------------------------------------------------------------
  // Metrics (weighted in PSI Performance score)
  // -------------------------------------------------------------------
  {
    auditId: 'first-contentful-paint',
    auditTitle: 'First Contentful Paint',
    coverage: 'partial',
    filters: [
      'inline_css',
      'prioritize_critical_css',
      'rewrite_css',
      'rewrite_javascript',
      'extend_cache',
    ],
    confidence: 'high',
    explanation:
      'FCP is gated by render-blocking CSS and slow byte delivery. mod_pagespeed inlines critical CSS, minifies all CSS/JS, and serves long-lived cache-busted URLs so repeat views render from cache.',
    caveats:
      'FCP is a metric, not a fixable defect. MPS removes common blockers but slow servers, large origin HTML, or third-party scripts in the critical path can still dominate.',
    availableIn: ['1.1', '2.0'],
    snippet11:
      'pagespeed EnableFilters prioritize_critical_css,inline_css,rewrite_css,rewrite_javascript;',
    // 2.0: critical CSS + minification + cache extension are all on as soon
    // as `pagespeed on` is set. The worker enables async CSS automatically
    // when critical CSS has been injected.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'largest-contentful-paint',
    auditTitle: 'Largest Contentful Paint',
    coverage: 'partial',
    filters: [
      'prioritize_critical_css',
      'inline_css',
      'rewrite_images',
      'convert_jpeg_to_webp',
      'resize_images',
      'hint_preload_subresources',
    ],
    confidence: 'high',
    explanation:
      'LCP is usually a hero image or above-the-fold text. mod_pagespeed transcodes the LCP image to WebP, resizes it to its rendered size, and removes render-blocking CSS via critical-CSS inlining.',
    caveats:
      'If LCP is a video poster, a background-image set by JavaScript, or a font-rendered headline, image filters cannot help directly.',
    availableIn: ['1.1', '2.0'],
    snippet11:
      'pagespeed EnableFilters rewrite_images,convert_jpeg_to_webp,resize_images,prioritize_critical_css,hint_preload_subresources;',
    // 2.0: image transcoding (WebP + AVIF) + viewport-aware resize + critical
    // CSS + LCP preload all run from the master switch. `--no-lcp-preload`
    // is the off switch, not on switch.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'total-blocking-time',
    auditTitle: 'Total Blocking Time',
    coverage: 'partial',
    filters: ['rewrite_javascript', 'combine_javascript', 'defer_javascript'],
    confidence: 'medium',
    explanation:
      'TBT measures main-thread blocking from JavaScript. Minifying and combining scripts reduces parse/compile time; deferring non-critical scripts moves work past the interactive boundary.',
    caveats:
      '`defer_javascript` is "test first" — it breaks any inline script that runs before DOM-ready. Tree-shaking and code-splitting are out of scope; for a heavy SPA, MPS will not move the needle as much as bundle surgery.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters rewrite_javascript,combine_javascript;',
    // 2.0: JS minification is automatic; script deferral runs against
    // browser-coverage analysis and only defers scripts judged safe to defer
    // (a safer replacement for the 1.1 defer_javascript flag).
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'cumulative-layout-shift',
    auditTitle: 'Cumulative Layout Shift',
    coverage: 'partial',
    filters: ['insert_image_dimensions'],
    confidence: 'high',
    explanation:
      'CLS from unsized images is the easy half: `insert_image_dimensions` measures each `<img>` and adds width/height so the browser reserves space before the bytes arrive.',
    caveats:
      'CLS from injected ads, late-loading web fonts, or dynamic banners is application-level — MPS cannot fix layout shifts caused by JavaScript adding DOM nodes after first paint.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters insert_image_dimensions;',
    // 2.0: image dimensions injection is on by default; flag is the negation.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'speed-index',
    auditTitle: 'Speed Index',
    coverage: 'partial',
    filters: [
      'prioritize_critical_css',
      'inline_css',
      'rewrite_images',
      'resize_images',
      'lazyload_images',
    ],
    confidence: 'medium',
    explanation:
      'Speed Index rewards pages that fill the viewport quickly. Critical CSS inlining unblocks first paint; image transcoding and resizing make above-the-fold images arrive sooner; lazyloading keeps below-the-fold images out of the critical path.',
    caveats:
      'Speed Index is a synthesized metric — improvements track other audits rather than being directly targetable.',
    availableIn: ['1.1', '2.0'],
    snippet11:
      'pagespeed EnableFilters prioritize_critical_css,rewrite_images,resize_images,lazyload_images;',
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'interaction-to-next-paint',
    auditTitle: 'Interaction to Next Paint',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'INP measures responsiveness of user interactions after load. It is dominated by event-handler JavaScript and React/Vue re-renders — work mod_pagespeed cannot rewrite without breaking the application.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'interactive',
    auditTitle: 'Time to Interactive',
    coverage: 'partial',
    filters: ['rewrite_javascript', 'combine_javascript', 'defer_javascript'],
    confidence: 'medium',
    explanation:
      'TTI improves when the main thread is idle. Minification reduces parse time, combining reduces request overhead, and deferring non-essential scripts moves work past the interactive point.',
    caveats:
      'Same caveat as TBT: defer_javascript can break scripts that depend on early DOM events.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'max-potential-fid',
    auditTitle: 'Max Potential First Input Delay',
    coverage: 'partial',
    filters: ['rewrite_javascript', 'defer_javascript'],
    confidence: 'medium',
    explanation:
      'Maximum input delay tracks the longest task during load. Reducing JS bytes via minification and deferring blocking scripts shortens worst-case tasks.',
    caveats: 'Hidden audit in modern Lighthouse; included for back-compat with older PSI reports.',
    availableIn: ['1.1', '2.0'],
  },

  // -------------------------------------------------------------------
  // Insight audits (replace the older "opportunity" audits PSI used to show)
  // -------------------------------------------------------------------
  {
    auditId: 'render-blocking-insight',
    auditTitle: 'Eliminate render-blocking resources',
    coverage: 'full',
    filters: [
      'prioritize_critical_css',
      'inline_css',
      'inline_javascript',
      'move_css_to_head',
      'move_css_above_scripts',
    ],
    confidence: 'high',
    explanation:
      'Render-blocking CSS in the `<head>` is exactly what prioritize_critical_css targets: it extracts the above-the-fold rules, inlines them, and defers the rest. Small JS files inline directly.',
    caveats:
      'prioritize_critical_css is marked "test first" — pages with complex CSS-in-JS or aggressive dark-mode swaps can flash unstyled content. Always inline a dark-mode override when using critical CSS.',
    availableIn: ['1.1', '2.0'],
    snippet11:
      'pagespeed EnableFilters prioritize_critical_css,inline_css,inline_javascript,move_css_to_head;',
    // 2.0: critical CSS extraction is always-on with `pagespeed on` and the
    // worker auto-enables async CSS for the deferred remainder.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'image-delivery-insight',
    auditTitle: 'Improve image delivery',
    coverage: 'full',
    filters: [
      'rewrite_images',
      'convert_jpeg_to_webp',
      'convert_jpeg_to_avif',
      'convert_to_webp_lossless',
      'recompress_images',
      'resize_images',
      'jpeg_sampling',
      'strip_image_meta_data',
    ],
    confidence: 'high',
    explanation:
      'mod_pagespeed transcodes JPEG/PNG/GIF to WebP and AVIF, recompresses with quality-aware encoders, resizes to the rendered display size, and strips EXIF/ICC metadata. Typical result: 40–70% smaller images at visually equivalent quality.',
    availableIn: ['1.1', '2.0'],
    snippet11:
      'pagespeed EnableFilters rewrite_images,convert_jpeg_to_webp,convert_jpeg_to_avif,resize_images;',
    // 2.0: image transcoding (WebP + AVIF + viewport-aware resize +
    // Save-Data variants + 2x density) is always-on under `pagespeed on`.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'lcp-discovery-insight',
    auditTitle: 'LCP request discoverability',
    coverage: 'partial',
    filters: ['hint_preload_subresources'],
    confidence: 'medium',
    explanation:
      'When the LCP image is referenced from CSS or injected late, the browser cannot discover it during preload-scan. mod_pagespeed emits `Link: rel=preload` headers for the LCP image so the browser can fetch it before the HTML parser reaches the tag.',
    caveats:
      'Authoritative LCP preload requires either an explicit `<link rel="preload">` from the origin or a fetchpriority="high" on the `<img>`. MPS surfaces hints but cannot guarantee preload for every LCP candidate.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters hint_preload_subresources;',
    // 2.0: LCP-image preload is on by default (`--no-lcp-preload` to disable),
    // with the URL also written to the Early Hints cache sentinel so nginx
    // can emit 103 Early Hints.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'lcp-breakdown-insight',
    auditTitle: 'LCP phase breakdown',
    coverage: 'partial',
    filters: ['rewrite_images', 'convert_jpeg_to_webp', 'resize_images', 'prioritize_critical_css'],
    confidence: 'medium',
    explanation:
      'The "resource load delay" and "resource load duration" phases shrink when MPS serves a smaller, format-optimized LCP image. The "element render delay" phase shrinks when critical CSS is inlined.',
    caveats:
      'The "TTFB" phase of LCP is server-side — the module alone cannot reduce origin response time. The optimizer worker caches the HTML response in Cyclone (origin Cache-Control permitting) and serves it zero-copy from the memory-mapped cache on subsequent requests, masking origin TTFB for repeat visits.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'cache-insight',
    auditTitle: 'Use efficient cache lifetimes',
    coverage: 'full',
    filters: ['extend_cache'],
    confidence: 'high',
    explanation:
      'mod_pagespeed rewrites static resource URLs to include a content hash and serves them with `Cache-Control: max-age=31536000, immutable`. Repeat visits skip the network entirely.',
    caveats:
      'Only applies to assets MPS rewrites (images, CSS, JS that flow through filters). Origin HTML and resources behind `Cache-Control: no-store` are untouched.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters extend_cache;',
    // 2.0: content-hashed URL extension is always-on with the master switch.
    // The cache mode controls how aggressively downstream caches treat the
    // optimized output (`pagespeed_cache_mode aggressive` for long TTLs).
    snippet20:
      'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;\npagespeed_cache_mode aggressive;  # opt in to long TTLs after validating output',
  },
  {
    auditId: 'font-display-insight',
    auditTitle: 'Font display',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      '`font-display: swap` is a CSS property that must be declared in the `@font-face` rule by the origin or font provider. mod_pagespeed does not rewrite font-loading semantics.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'duplicated-javascript-insight',
    auditTitle: 'Duplicated JavaScript',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Duplicated JS comes from bundlers shipping the same library twice (e.g., React in two chunks). Fixing it requires bundler configuration — MPS cannot deduplicate across separate compiled bundles.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'legacy-javascript-insight',
    auditTitle: 'Legacy JavaScript',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Legacy JS means polyfills and ES5 transpilation shipped to modern browsers. Fixing it requires a `module/nomodule` build split at the application layer.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'modern-http-insight',
    auditTitle: 'Use HTTP/2 or HTTP/3',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation: 'HTTP version is a server/CDN configuration concern, not a content optimization.',
    caveats:
      'On nginx, enable `listen 443 ssl http2;` (or `http3` on 1.25+). MPS rides on whatever protocol nginx negotiates.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'network-dependency-tree-insight',
    auditTitle: 'Critical request chains',
    coverage: 'partial',
    filters: [
      'inline_css',
      'inline_javascript',
      'combine_css',
      'combine_javascript',
      'flatten_css_imports',
    ],
    confidence: 'medium',
    explanation:
      'Long critical chains often start with CSS `@import` or many small `<link>`/`<script>` tags. Flattening imports and combining/inlining shortens the chain.',
    // 1.1 is the only edition with the full set; 2.0 does CSS `@import`
    // flattening but does not combine/inline arbitrary CSS or JS files.
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'third-parties-insight',
    auditTitle: 'Reduce the impact of third-party code',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Third-party scripts (analytics, tag managers, ads, chat widgets) are loaded by the origin and run cross-origin. mod_pagespeed does not rewrite or proxy third-party JavaScript.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'document-latency-insight',
    auditTitle: 'Document request latency',
    coverage: 'partial',
    filters: [],
    confidence: 'medium',
    explanation:
      'Document latency is origin TTFB plus DNS, TLS, and routing. The optimizer worker caches HTML in the Cyclone shared-memory cache when the origin allows it (set `Cache-Control: public, max-age=60, must-revalidate` on HTML, or use `pagespeed_html_max_age`), and serves cache hits zero-copy from the memory-mapped file — masking origin TTFB for repeat visits.',
    caveats:
      'Only repeat requests benefit; the first request for a URL still hits the origin. HTML output `Cache-Control` is always `no-cache` so downstream browsers and CDNs revalidate, but conditional revalidation makes that cheap (304 from origin, no body transfer).',
    // HTML caching at the module layer is unique to 2.0. 1.1 always passes
    // HTML through to the origin.
    availableIn: ['2.0'],
    snippet20:
      'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;\npagespeed_html_max_age 60;  # cache HTML for 60s when origin sends no Cache-Control',
  },
  {
    auditId: 'dom-size-insight',
    auditTitle: 'Avoid an excessive DOM size',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Large DOM size is a structural problem: too many elements, deep nesting, or oversized lists. Only the application can reduce it.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'viewport-insight',
    auditTitle: 'Has a `<meta name="viewport">` tag',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Viewport meta tag must be authored in the HTML head; MPS does not synthesize semantic meta tags.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'cls-culprits-insight',
    auditTitle: 'Layout shift culprits',
    coverage: 'partial',
    filters: ['insert_image_dimensions'],
    confidence: 'medium',
    explanation:
      'When the culprit is an unsized image, `insert_image_dimensions` adds width/height attributes. Other CLS culprits (late-loading fonts, injected content) need application-side fixes.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'inp-breakdown-insight',
    auditTitle: 'INP phase breakdown',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'INP is driven by interaction-handler JavaScript that MPS cannot rewrite without breaking behavior.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'forced-reflow-insight',
    auditTitle: 'Forced reflows',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Forced synchronous layout is caused by JavaScript reading layout properties after mutating the DOM. The fix is application code, not byte-level rewriting.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'slow-css-selector-insight',
    auditTitle: 'CSS selectors costing render time',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Slow selectors (deep descendant chains, universal `*` matching) must be rewritten by the author. `rewrite_css` minifies but preserves selector semantics.',
    availableIn: ['1.1', '2.0'],
  },

  // -------------------------------------------------------------------
  // Diagnostics (unweighted but commonly shown)
  // -------------------------------------------------------------------
  {
    auditId: 'unminified-css',
    auditTitle: 'Minify CSS',
    coverage: 'full',
    filters: ['rewrite_css'],
    confidence: 'high',
    explanation:
      'mod_pagespeed minifies every CSS file it serves — removing whitespace, comments, and redundant syntax. CoreFilter; enabled by default.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters rewrite_css;',
    // 2.0: CSS minification runs unconditionally for every cached CSS asset
    // (turn off only with worker flag `--disable-css`).
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'unminified-javascript',
    auditTitle: 'Minify JavaScript',
    coverage: 'full',
    filters: ['rewrite_javascript', 'rewrite_javascript_external', 'rewrite_javascript_inline'],
    confidence: 'high',
    explanation:
      'mod_pagespeed minifies every JS file it serves, plus inline `<script>` blocks. CoreFilter; enabled by default.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters rewrite_javascript;',
    // 2.0: JS minification is automatic (`--disable-js` to turn off).
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'unused-css-rules',
    auditTitle: 'Reduce unused CSS',
    coverage: 'partial',
    filters: ['prioritize_critical_css'],
    confidence: 'medium',
    explanation:
      '`prioritize_critical_css` extracts the rules used above the fold and defers the rest — the deferred CSS still loads, but it is no longer render-blocking.',
    caveats:
      'MPS does not delete unused CSS rules. True tree-shaking requires a build step (PurgeCSS, Tailwind JIT, etc.).',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters prioritize_critical_css;',
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'unused-javascript',
    auditTitle: 'Reduce unused JavaScript',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'mod_pagespeed cannot determine at rewrite time which JS will be executed at runtime. Tree-shaking unused JS requires bundler-level code analysis.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'total-byte-weight',
    auditTitle: 'Avoid enormous network payloads',
    coverage: 'partial',
    filters: [
      'rewrite_images',
      'convert_jpeg_to_webp',
      'rewrite_css',
      'rewrite_javascript',
      'inline_images',
    ],
    confidence: 'high',
    explanation:
      'The largest wins come from image transcoding (often 40–70% smaller). CSS/JS minification adds another 10–30%.',
    caveats:
      'If the page is heavy because of video, fonts, or large JSON payloads, those are outside MPS’s scope.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'bootup-time',
    auditTitle: 'Reduce JavaScript execution time',
    coverage: 'partial',
    filters: ['rewrite_javascript', 'defer_javascript'],
    confidence: 'medium',
    explanation:
      'Minifying reduces parse cost; deferring non-essential scripts moves their execution past initial bootup.',
    caveats:
      'Heavy framework runtimes (React, Vue, Angular) dominate bootup and cannot be made smaller by minification alone.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'mainthread-work-breakdown',
    auditTitle: 'Minimize main-thread work',
    coverage: 'partial',
    filters: ['rewrite_javascript', 'rewrite_css', 'defer_javascript'],
    confidence: 'medium',
    explanation:
      'Reducing JS bytes reduces parse + compile time. Smaller CSS reduces style-recalc cost. Both are partial wins; structural fixes are application-level.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'long-tasks',
    auditTitle: 'Avoid long main-thread tasks',
    coverage: 'partial',
    filters: ['rewrite_javascript', 'defer_javascript', 'combine_javascript'],
    confidence: 'medium',
    explanation:
      'Smaller, fewer scripts produce shorter compile/exec tasks. Tasks rooted in framework code remain long.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'non-composited-animations',
    auditTitle: 'Avoid non-composited animations',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Non-composited animations animate properties (top/left/width) that trigger layout. Fix is CSS-level: switch to `transform`/`opacity`.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'unsized-images',
    auditTitle: 'Image elements have explicit width and height',
    coverage: 'full',
    filters: ['insert_image_dimensions'],
    confidence: 'high',
    explanation:
      '`insert_image_dimensions` reads the intrinsic dimensions of each image and adds matching `width`/`height` attributes to the `<img>` tag.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters insert_image_dimensions;',
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'bf-cache',
    auditTitle: 'Page back/forward cache eligibility',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'bfcache eligibility is determined by response headers (`Cache-Control: no-store`), unload handlers, and other application behaviors that mod_pagespeed does not control.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'user-timings',
    auditTitle: 'User Timing marks and measures',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Diagnostic-only audit — surfaces application-emitted performance.mark() calls. No optimization required.',
    availableIn: ['1.1', '2.0'],
  },

  // -------------------------------------------------------------------
  // Legacy opportunity audits (no longer in default-config but may appear
  // in older PSI integrations and surface in third-party tooling)
  // -------------------------------------------------------------------
  {
    auditId: 'render-blocking-resources',
    auditTitle: 'Eliminate render-blocking resources (legacy)',
    coverage: 'full',
    filters: ['prioritize_critical_css', 'inline_css', 'inline_javascript', 'move_css_to_head'],
    confidence: 'high',
    explanation: 'Legacy audit ID for what is now `render-blocking-insight`. Same MPS coverage.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'uses-responsive-images',
    auditTitle: 'Properly size images (legacy)',
    coverage: 'full',
    filters: ['resize_images', 'resize_rendered_image_dimensions', 'responsive_images'],
    confidence: 'high',
    explanation:
      'mod_pagespeed resizes images to the rendered dimensions and can generate a `srcset` for multiple resolutions. The optimizer worker generates viewport-tagged variants (Mobile/Tablet/Desktop, 1x/2x density) keyed off the request capability mask instead of using `srcset`.',
    caveats:
      '`responsive_images` is "test first" — it changes the `<img>` markup and may interact with framework-rendered image components.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters resize_images,resize_rendered_image_dimensions;',
    // 2.0: viewport-aware resizing is part of proactive variant generation,
    // controlled by `--mobile-width` / `--tablet-width` / `--desktop-width`.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'offscreen-images',
    auditTitle: 'Defer offscreen images (legacy)',
    coverage: 'full',
    filters: ['lazyload_images'],
    confidence: 'high',
    explanation:
      'lazyload_images defers loading of below-the-fold images until the user scrolls toward them.',
    caveats:
      '"Test first" — interacts poorly with sites that already use native `loading="lazy"` or framework-controlled image components.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters lazyload_images;',
    // 2.0: `loading="lazy"` is injected on below-the-fold images
    // automatically; LCP candidate gets `fetchpriority="high"` instead.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'modern-image-formats',
    auditTitle: 'Serve images in next-gen formats (legacy)',
    coverage: 'full',
    filters: [
      'convert_jpeg_to_webp',
      'convert_jpeg_to_avif',
      'convert_to_webp_lossless',
      'convert_to_webp_animated',
    ],
    confidence: 'high',
    explanation:
      'JPEG/PNG/GIF transcoded to WebP and AVIF when the client advertises support via `Accept`.',
    availableIn: ['1.1', '2.0'],
    snippet11:
      'pagespeed EnableFilters convert_jpeg_to_webp,convert_jpeg_to_avif,convert_to_webp_lossless;',
    // 2.0: WebP + AVIF are always-on, plus SVG auto-vectorization in
    // `--svg-mode auto` for eligible images.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'uses-optimized-images',
    auditTitle: 'Efficiently encode images (legacy)',
    coverage: 'full',
    filters: [
      'recompress_images',
      'recompress_jpeg',
      'recompress_png',
      'recompress_webp',
      'jpeg_sampling',
    ],
    confidence: 'high',
    explanation:
      'Quality-aware recompression for JPEG, PNG, and WebP, with chroma subsampling tuned for the format.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters recompress_images,jpeg_sampling;',
    // 2.0: quality is driven by `--jpeg-quality` / `--webp-quality` /
    // `--avif-quality` and (by default) a learned LightGBM model that
    // predicts the optimal quality for a target SSIMULACRA2 score.
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'uses-text-compression',
    auditTitle: 'Enable text compression (legacy)',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      "In the module, gzip/brotli compression is configured on nginx itself (`gzip on;` / `brotli on;`), not via mod_pagespeed filters. The optimizer worker writes pre-compressed gzip and brotli alternates next to every cached text resource, so cache HITs serve a pre-compressed body with the right `Content-Encoding` header — but nginx is still the layer that selects the encoding, so this remains an nginx-config concern from the audit's perspective.",
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'uses-rel-preconnect',
    auditTitle: 'Preconnect to required origins (legacy)',
    coverage: 'partial',
    filters: ['insert_dns_prefetch'],
    confidence: 'medium',
    explanation:
      'The module inserts `<link rel="dns-prefetch">` hints for third-party domains it sees in the page. The optimizer worker detects third-party origins from external resources and writes `preconnect:` hints (the stronger form — DNS + TCP + TLS) to the Early Hints sentinel.',
    caveats:
      'DNS-prefetch is weaker than preconnect (DNS only, no TCP/TLS). The optimizer worker\'s preconnect injection closes that gap; with the module alone, authoritative preconnect needs an origin-emitted `<link rel="preconnect">`.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters insert_dns_prefetch;',
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'uses-rel-preload',
    auditTitle: 'Preload key requests (legacy)',
    coverage: 'partial',
    filters: ['hint_preload_subresources'],
    confidence: 'medium',
    explanation:
      'The module emits `Link: rel=preload` headers for subresources it knows the page will request. The optimizer worker narrows this to the detected LCP image (the highest-impact preload) and writes the URL to the Early Hints sentinel so nginx can emit `103 Early Hints` responses.',
    caveats:
      'Only resources MPS sees during rewriting are hinted. Late-discovered LCP candidates still need origin-emitted preloads.',
    availableIn: ['1.1', '2.0'],
    snippet11: 'pagespeed EnableFilters hint_preload_subresources;',
    snippet20: 'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  },
  {
    auditId: 'efficient-animated-content',
    auditTitle: 'Use video formats for animated content (legacy)',
    coverage: 'partial',
    filters: ['convert_to_webp_animated'],
    confidence: 'medium',
    explanation:
      'Animated GIFs transcoded to WebP. The Lighthouse-preferred fix is to convert to `<video>`; MPS cannot rewrite GIF tags into video elements.',
    caveats:
      'WebP is smaller than GIF but larger than equivalent MP4/WebM video; this partially closes the gap.',
    availableIn: ['1.1'],
    snippet11: 'pagespeed EnableFilters convert_to_webp_animated;',
  },
  {
    auditId: 'duplicated-javascript',
    auditTitle: 'Remove duplicate modules in JavaScript bundles (legacy)',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Legacy alias of `duplicated-javascript-insight` — same conclusion: bundler-level fix only.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'legacy-javascript',
    auditTitle: 'Avoid serving legacy JavaScript to modern browsers (legacy)',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Legacy alias of `legacy-javascript-insight` — same conclusion: needs module/nomodule build split.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'third-party-summary',
    auditTitle: 'Minimize third-party usage (legacy)',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation: 'MPS does not proxy or rewrite third-party JavaScript.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'redirects',
    auditTitle: 'Avoid multiple page redirects',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation:
      'Redirect chains are an origin/CDN configuration concern. Fix with a single canonical hostname and scheme on the origin.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'server-response-time',
    auditTitle: 'Reduce initial server response time (TTFB)',
    coverage: 'partial',
    filters: [],
    confidence: 'medium',
    explanation:
      'TTFB is dominated by origin work — database queries, application logic, framework rendering. The optimizer worker can mask slow TTFB on repeat requests by caching HTML in the Cyclone shared-memory cache when the origin allows it; the cache HIT is served zero-copy from the memory-mapped file with no origin round-trip. The module alone does not cache HTML, so it cannot help here.',
    caveats:
      'The optimizer worker caches HTML only when the origin allows it (set `Cache-Control: public, max-age=60, must-revalidate` on HTML responses, or use `pagespeed_html_max_age` for origins that send no Cache-Control). The first request for any URL still hits the origin; the speedup is on subsequent requests.',
    availableIn: ['2.0'],
    snippet20:
      'pagespeed on;\npagespeed_cache_path /var/lib/pagespeed/cache.vol;\npagespeed_html_max_age 60;  # cache HTML for 60s when origin sends no Cache-Control',
  },
  {
    auditId: 'network-requests',
    auditTitle: 'Network requests',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation: 'Diagnostic listing of every network request; no fix required.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'network-rtt',
    auditTitle: 'Network Round Trip Times',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation: 'Network RTT is a physics-of-the-internet diagnostic, not a fixable defect.',
    availableIn: ['1.1', '2.0'],
  },
  {
    auditId: 'network-server-latency',
    auditTitle: 'Server backend latencies',
    coverage: 'none',
    filters: [],
    confidence: 'high',
    explanation: 'Backend latency is origin-side; MPS cannot accelerate origin compute.',
    availableIn: ['1.1', '2.0'],
  },
];

/**
 * Convenience helpers consumed by the /analyze page renderer.
 */

export function getMappingByAuditId(auditId: string): AuditMapping | undefined {
  for (const m of PSI_MPS_MAPPING) {
    if (m.auditId === auditId) return m;
  }
  return undefined;
}

export function summarizeCoverage(auditIds: readonly string[]): {
  total: number;
  full: number;
  partial: number;
  none: number;
  unmapped: number;
} {
  const counters = { total: auditIds.length, full: 0, partial: 0, none: 0, unmapped: 0 };
  for (const id of auditIds) {
    const m = getMappingByAuditId(id);
    if (!m) {
      counters.unmapped += 1;
      continue;
    }
    counters[m.coverage] += 1;
  }
  return counters;
}
