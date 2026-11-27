---
title: 'Fix INP on Magento 2: why Hyvä beats a JS rewriter'
description: 'INP on Magento 2 lives in KnockoutJS and RequireJS, so theme architecture (Hyvä) moves it, not a server rewriter. What mod_pagespeed can and cannot do.'
date: 2026-05-05
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'inp', 'magento', 'hyva', 'javascript']
draft: false
product: '1.1'
howTo:
  name: How to fix INP on Magento
  description: Reduce Magento Interaction to Next Paint by diagnosing the slow handler, trimming JS parse cost with mod_pagespeed, and applying the architectural theme and module fixes that actually move INP.
  tools:
  - mod_pagespeed
  - nginx
  - PageSpeed Insights
  - Chrome DevTools Performance panel
  - web-vitals
  - Hyvä theme
  steps:
  - name: Trace the slow click
    text: Record the user flow in Chrome DevTools Performance panel and read INP per interaction in the Interactions track. Run PSI on a homepage, category page, and PDP, and add web-vitals attribution mode to name the responsible script and handler. Measure each page type separately since the worst interaction varies by journey.
  - name: Combine and minify the JS catalog with mod_pagespeed
    text: 'Enable combine_javascript and rewrite_javascript in the nginx or Apache module to cut transfer and parse overhead on Magento''s fragmented JS. Do NOT enable defer_javascript: it breaks RequireJS bootstrap order. This is a partial assist of 10-20%; the dominant INP cost is handler execution, not bytes.'
  - name: Apply the Magento-side architectural fixes
    text: The largest INP win is switching to the Hyvä theme, which replaces KnockoutJS and RequireJS with Alpine.js. Also enable production mode plus Varnish, leave the stock Bundle JavaScript toggle off, defer the customer-data localStorage sync, and audit heavy third-party Marketplace modules.
  - name: Measure the drop
    text: Compare web-vitals onINP per page type against your baseline and confirm the worst interaction is under 200 ms in the DevTools Interactions track. Re-run PSI on homepage, category, PDP, and cart, then check Search Console Core Web Vitals after 28 days, tracking each URL group.
  - name: Escalate where the server layer cannot help
    text: If checkout INP stays above 300 ms, the fix is Hyvä Checkout or a custom React/Vue checkout, not the module. PWA Studio (Venia) and headless storefronts get zero benefit from mod_pagespeed, and B2B form re-validation needs JS-side debouncing.
faq:
  - q: Can mod_pagespeed fix INP on Magento?
    a: Partially. combine_javascript and rewrite_javascript trim JS transfer and parse cost for a 10–20% reduction, but the dominant INP cost is handler execution in KnockoutJS and RequireJS, which only a theme or checkout re-architecture changes.
  - q: Why does the Hyvä theme lower Magento INP?
    a: Hyvä replaces KnockoutJS and RequireJS with Alpine.js, removing the synchronous data-binding traversal that runs on every interaction. Swatch-click INP drops from 300+ ms to 100–150 ms and checkout from 500+ ms to ~200 ms in customer benchmarks.
  - q: Should I enable defer_javascript on Magento?
    a: No. Magento's RequireJS bootstrap is order-sensitive, and deferring scripts breaks the define/require graph and produces "module not found" errors at runtime.

---

Magento 2's INP is the worst-case among the major CMSes, and the reason is not bytes on the wire: it is the KnockoutJS data-binding chain plus the RequireJS loader, both of which run synchronously on every interaction in the default Luma theme. The fix that pays off most is switching to the **Hyvä theme**, not enabling a server-layer rewriter. Hyvä replaces Knockout with Alpine.js and routinely drops PDP and cart INP by 40–70% in customer benchmarks. **mod_pagespeed** (an nginx or Apache module) can trim parse cost via `combine_javascript` and `rewrite_javascript`, but `defer_javascript` is actively unsafe on Magento — covered explicitly below.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What INP measures

Interaction to Next Paint (INP) is the worst-case latency between a user input — click, tap, keypress — and the next frame the browser paints in response. Chrome logs every interaction for the life of the page and reports the 98th-percentile slowest as that page's INP. Good is **≤ 200 ms**, "needs improvement" tops out at 500 ms, and anything beyond 500 ms fails. Real-user numbers come from CrUX; the Lighthouse-via-PSI lab figure is unreliable because the lab fires no human input — and on Magento, where the Knockout/RequireJS cost only shows up under real clicks, the field number is the one to trust. See [web.dev/inp](https://web.dev/articles/inp).

## The most common INP failures on Magento

1. **KnockoutJS data-binding re-evaluates on every UI event.** Magento's checkout, mini-cart, and product gallery use Knockout. Each interaction triggers a dependency-graph traversal across 200–500 observables on a typical checkout. INP on the "Next" button in checkout is routinely 400–700 ms on mid-range Android. This is not a bug, it is what Knockout does — but it is also why Hyvä exists.
2. **RequireJS chain on first interaction.** First click on a product gallery triggers a chain of `define`/`require` calls (`Magento_ProductVideo`, `magnifier/magnifier`, `Magento_Swatches/swatch-renderer`). If any of these are not already loaded, the browser fetches and parses them _before_ responding to the click. INP on first gallery interaction is consistently above 300 ms.
3. **Customer-data localStorage sync on `DOMContentLoaded` and every nav.** `Magento_Customer/js/customer-data.js` syncs cart count, wishlist, and recently-viewed via localStorage on every page load, then re-syncs on every navigation. The sync runs synchronously and blocks any interaction during the first ~200 ms after load. Click "Add to cart" too early and you record that wait as the INP.
4. **Layered-navigation filters re-render synchronously.** On category pages, every filter click reloads the product grid via a synchronous-ish XHR + DOM re-render. INP on filter clicks is consistently above 250 ms even on small catalogs.

## Trace the slow click

Magento has multiple distinct INP profiles depending on which page you are on — PDP, category, cart, checkout. Measure each one separately; the worst is rarely on the home page.

- Open the product page in Chrome, open DevTools → **Performance** panel, click record, then walk the user flow: load the page, click a swatch, click the gallery's "next image" arrow, click "Add to cart". Stop. The **Interactions** track lists each input with INP duration. Click the worst one → flame chart shows the responsible handler.
- Run PSI: `https://pagespeed.web.dev/analysis?url=<URL>` on a homepage, a category page, and a PDP. Field INP numbers will differ by URL group.
- Install [`web-vitals`](https://github.com/GoogleChrome/web-vitals) with `attribution` mode in your `Magento_Theme/templates/html/footer.phtml`. Log to console with `onINP(console.log, {reportAllChanges: true})`. The `attribution.eventEntry` field names the script and handler.
- Magento dev profiler: `bin/magento dev:profiler:enable` then `?profiler=1` on a page. Useful for backend latency but not for INP directly — INP is client-side. Skip this unless you suspect a slow XHR endpoint contributes.

Output you want before continuing: a specific interaction (e.g., "click on the product gallery's next-arrow") and a specific script (e.g., "`Magento_ProductVideo` initialization").

## Combine and minify the RequireJS catalog (but skip defer_javascript)

Magento ships a fragmented JS catalog (100+ files split across modules), and combining plus [safe minification](/blog/safe-javascript-minification-semicolon-insertion/) reduces transfer and parse overhead. But the dominant INP cost is _handler execution_, not parse, so this step is real but partial.

**Important warning: do not enable `defer_javascript` on Magento.** Magento's RequireJS bootstrap is order-sensitive; deferring scripts breaks the `define`/`require` graph and produces nonsensical "module not found" errors at runtime. The [filters reference](/docs/filters/) marks `defer_javascript` as **Test first** for exactly this reason. Stick to the safer pair below.

Enable, in order:

- **`combine_javascript`** — concatenates the dozens of small JS files Magento serves into one bundle. RequireJS still loads modules in the right order; combining only removes per-file parser-setup cost. Generally safe.
- **`rewrite_javascript`** — minifies inline and external JS. Magento's production mode already minifies, but theme JS and many third-party module JS files are not minified at upload time.

Minimal nginx config:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters combine_javascript;
pagespeed EnableFilters rewrite_javascript;
# Note: do NOT enable defer_javascript on Magento — breaks RequireJS bootstrap order
```

After enabling, re-run PSI. INP should improve modestly across the board — usually a 10–20 % reduction. If the numbers do not move, the bottleneck is handler execution, not JS bytes, and the Magento-side tuning below is the only path that changes them.

## Magento-side knobs that survive

This is the section where Magento INP is actually fixable. Server-layer leverage on Knockout-bound interactions is small; theme and module choices dominate.

- **Switch to the Hyvä theme.** Hyvä replaces Knockout + RequireJS with Alpine.js + Magewire. PDP swatch-click INP drops from 300+ ms to 100–150 ms in customer benchmarks; checkout INP drops from 500+ ms to ~200 ms. This is the single biggest INP win available on Magento. If you are evaluating "fix INP on Magento" as a project, evaluate Hyvä first.
- **Enable production mode and Varnish.** `bin/magento deploy:mode:set production` then `bin/magento setup:static-content:deploy -f en_US`. Production mode minifies and merges JS; Varnish in front of nginx removes the PHP execution latency from interactive XHRs.
- **Do not enable "Bundle JavaScript Files."** Admin → Stores → Configuration → Advanced → Developer → JavaScript Settings has a tempting "Bundle" toggle. Magento's stock bundler ships unused modules per page-type and _worsens_ INP by inflating parse cost. "Merge" and "Minify" are fine; "Bundle" is not.
- **Defer customer-data sync.** `customer-data.js` config has `expires` settings; bumping them reduces sync frequency. The default is aggressive — a sync per page nav is more than necessary for most stores.
- **Audit third-party modules.** Magento Marketplace modules often inject frontend JS that does heavy work on common interactions (variant change, add-to-cart). Use the Performance trace to identify them, cross-check against [Chrome coverage to find unused JavaScript](/blog/remove-unused-javascript-chrome-coverage/), and disable or replace.
- **For high-traffic stores: Adobe-partner Magento Optimization profile.** It is an upstream config + caching audit that does what no general-purpose tool can — touches indexing, DB config, full-page cache strategy, ESI block tuning.

## Measuring the drop

1. Field-data attribution via `web-vitals` is more useful on Magento than on most CMSes because the worst interaction varies by user journey. Log `onINP` per page-type and compare against your Diagnose-step baseline.
2. Open DevTools → Performance, replay the user flow, confirm the worst-interaction INP is under 200 ms in the **Interactions** track. If checkout INP is still above 300 ms, you almost certainly need Hyvä Checkout or a re-architecture.
3. Re-run PSI on the homepage, a category page, a PDP, and the cart. INP should drop on all four; the PDP and cart are the headline numbers.
4. Wait 28 days, then check Search Console → Core Web Vitals report. Magento sites typically have separate URL groups for PDP, category, and cart — track each one.

## The Magento-safe config

```nginx
# nginx — minimal config to address INP on Magento
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters combine_javascript;
pagespeed EnableFilters rewrite_javascript;
# Do NOT enable defer_javascript on Magento — RequireJS bootstrap is order-sensitive
# Optional, helps homepage/category first-paint settle before first interaction:
pagespeed EnableFilters prioritize_critical_css;
```

For Apache (less common on Magento, but supported):

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed
ModPagespeedRewriteLevel CoreFilters
ModPagespeedEnableFilters combine_javascript
ModPagespeedEnableFilters rewrite_javascript
```

## Limits of the server-layer fix

Magento INP is dominated by the platform's frontend architecture, not by anything a reverse-proxy rewriter can change. Be ready to escalate.

- **Checkout INP is largely architectural.** If your worst interaction is in checkout (Knockout's `Magento_Checkout/js/view/billing-address` re-renders on every field change), mod_pagespeed cannot help. The fix is Hyvä Checkout or a custom React/Vue checkout that bypasses Knockout entirely. Vendor benchmarks routinely cite 40–70 % INP improvement from this migration.
- **PWA Studio (Venia) sites do not benefit from MPS at all.** If you are running Venia or any headless storefront in front of Magento, you are serving a JS bundle that boots an SPA. The initial paint is a skeleton; INP cost is entirely in the React app. mod_pagespeed at the Magento backend has zero effect on the SPA's INP.
- **B2B and large-catalog instances have their own profile.** Magento B2B's quote and requisition-list interactions involve large form re-validations on every keystroke. The fix is JS-side debouncing, not server-side.
- **Third-party Marketplace modules can dominate INP.** Wishlist plugins, custom-product configurators, and ajax-search modules all ship heavy JS. Profile each one and disable what you do not need.

If you are committed to Luma and cannot adopt Hyvä, the realistic ceiling on Magento INP without architectural change is the "Needs improvement" band (200–500 ms). That is not a mod_pagespeed limitation — it is the cost of the framework.

## FAQ

**Can mod_pagespeed fix INP on Magento?**
Partially. combine_javascript and rewrite_javascript trim JS transfer and parse cost for a 10–20% reduction, but the dominant INP cost is handler execution in KnockoutJS and RequireJS, which only a theme or checkout re-architecture changes.

**Why does the Hyvä theme lower Magento INP?**
Hyvä replaces KnockoutJS and RequireJS with Alpine.js, removing the synchronous data-binding traversal that runs on every interaction. Swatch-click INP drops from 300+ ms to 100–150 ms and checkout from 500+ ms to ~200 ms in customer benchmarks.

**Should I enable defer_javascript on Magento?**
No. Magento's RequireJS bootstrap is order-sensitive, and deferring scripts breaks the define/require graph and produces "module not found" errors at runtime.

## Related

- [How to fix LCP on Magento](/blog/fix-lcp-magento-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix CLS on Magento](/blog/fix-cls-magento-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [How to fix INP on nginx (generic)](/blog/fix-inp-nginx-2026/)
- [Critical CSS without a headless browser](/blog/critical-css-heuristics/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full INP guide](/core-web-vitals/inp/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).
