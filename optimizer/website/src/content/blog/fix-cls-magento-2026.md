---
title: 'Fix CLS on Magento 2: start with the Fotorama gallery'
description: 'Fix CLS on Magento 2 product pages: stop Fotorama gallery jumps, pre-size the mini-cart and private-content blocks, write img dimensions at the server.'
date: 2026-04-30
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'cls', 'magento']
lastUpdated: 2026-10-22
draft: false
product: '1.1'
howTo:
  name: How to fix CLS on Magento
  description: Reduce Cumulative Layout Shift on Magento 2 product pages by pre-sizing the Fotorama gallery with server-layer image dimensions and reserving space for the mini-cart and private-content blocks in Magento configuration and theme CSS.
  tools:
  - mod_pagespeed
  - nginx
  - Apache
  - Chrome DevTools
  - PageSpeed Insights
  - Magento CLI
  - Search Console
  steps:
  - name: Baseline the shift and identify the failing selector
    text: 'Run PSI on a product URL and read the "Avoid large layout shifts" diagnostic, then use Chrome DevTools Performance panel with Layout Shift Regions and Slow 4G/4x CPU throttling. Record a baseline CLS, the failing selector (usually .product.media or .fotorama__stage), and which of the three Magento patterns it maps to: Fotorama gallery, mini-cart count, or private-content injection.'
  - name: Pre-size the Fotorama gallery with insert_image_dimensions
    text: Enable insert_image_dimensions so the rewriter reads each catalog image's intrinsic dimensions and writes matching width and height attributes, letting Fotorama reserve the correct slot height before it initializes. Add lazyload_images for dimensioned thumbnail placeholders and prioritize_critical_css to shorten the FOUC window. Test on responsive templates before enabling store-wide.
  - name: Tune Magento outside the rewriter
    text: Set a uniform catalog aspect ratio in view.xml and run bin/magento catalog:images:resize, then reserve space in theme CSS (.product.media aspect-ratio plus min-height, and .minicart-wrapper min-width). Move customer-specific banners from client-side customer-data to Varnish ESI so private content renders inside the cached HTML, and deploy in production mode.
  - name: Apply the minimal nginx or Apache config
    text: Turn pagespeed on, set the file cache path and CoreFilters, and enable insert_image_dimensions, lazyload_images, and prioritize_critical_css. Ensure Magento media under /pub/media is fetchable by declaring the site Domain (and FetchHttps on nginx).
  - name: Confirm the fix took
    text: Reload a DevTools Performance trace under Slow 4G and verify the Layout Shift Regions overlay is empty across the gallery, re-run PSI to confirm large layout shifts drop to zero or only the mini-cart bubble, then watch the CrUX field CLS and the Search Console Core Web Vitals report move the product URL group out of "Needs improvement" after about 28 days.

---

Magento 2 product pages routinely score 0.3+ CLS on mobile and the offending element is almost always the Fotorama gallery, expanding from zero height to 700 px on init and dragging the price block, add-to-cart button, and tabs down with it. The biggest win is forcing image dimensions onto the catalog images via **mod_pagespeed** (an nginx module) so the gallery slot reserves the correct height before pixels arrive; everything else is sizing the mini-cart and private-content blocks. No fork of the gallery JavaScript, no Hyvä migration on day one.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What CLS measures

Cumulative Layout Shift adds up every unexpected shift of visible content from load start to the moment the page becomes interactive. Each shift is weighted by the fraction of the viewport it displaced and how far that content moved; the highest-scoring session window sets the page's CLS. Per [web.dev/cls](https://web.dev/articles/cls), under 0.1 is "good" and "poor" starts at 0.25.

Shifts inside 500 ms of a user interaction don't count, which matters on Magento because the mini-cart populates from `localStorage` shortly after page interactive — _not_ in response to a click — and any header re-flow it causes lands inside the CLS window. PSI shows the field 75th-percentile CLS from CrUX; Lighthouse shows a single lab run that often disagrees. Optimize toward the field number.

## The most common CLS failures on Magento

Magento 2 CLS has three repeat offenders, and Fotorama is by far the loudest of them.

**The Fotorama gallery initializes after page load and inflates the slot.** Magento's default product-page gallery uses Fotorama, which calculates its dimensions client-side based on the first loaded image. The gallery container starts at zero height in the initial HTML; it expands to 500–700 px when Fotorama runs. Every element below — price block, add-to-cart, description tabs, reviews — jumps by the full gallery height. This is the single most visible Magento CLS source.

**Mini-cart count updates after page render.** `Magento_Customer/js/customer-data.js` populates the cart-count `<span>` from `localStorage` after `DOMContentLoaded`. If the cart goes from 0 to 2+ items, the cart bubble appears and the header may re-flow on mobile, especially with constrained header widths in the Luma theme. Double-digit cart counts on mobile push the menu items left.

**Private-content blocks inject after page render.** Magento's "private content" architecture (used for "Hello, Customer Name", store-credit balance banners, customer-specific promo bars) deliberately defers customer-specific markup so the cacheable HTML can be served from Varnish. The injection happens after the page renders. CLS records every header expansion, every banner appearance, every "logged-in user" widget that materializes.

## Reading the shifts in DevTools

Run PSI on the product URL: `https://pagespeed.web.dev/analysis?url=https://example.com/example-product.html`. The "Avoid large layout shifts" diagnostic names the shifted elements; on Magento the worst offender is almost always `.product.media` or `.fotorama__stage`.

Open Chrome DevTools → Performance panel → Settings cog → enable "Web Vitals" and "Layout Shift Regions". Throttle CPU to 4× and network to "Slow 4G" so Fotorama's init is visibly after the initial paint. Reload; the gallery's expansion shows up as a tall red rectangle.

The Magento CLI helps too: `bin/magento dev:profiler:enable` then load the page and check the profiling output for client-side hydration timings — a long client-init chain almost always correlates with CLS.

For repeatable measurement, add `web-vitals` with attribution to a dev block in `default_head_blocks.xml`: `onCLS(console.log, { reportAllChanges: true })`. The console output names the worst shifted element per session.

What you want before changing anything: a baseline CLS, the failing selector, and which of the three patterns above it maps to.

## Pre-size the Fotorama gallery slot with insert_image_dimensions

mod_pagespeed is most effective on failure mode #1 (image dimensions); for #2 and #3 the fix is in Magento configuration, not the rewriter.

`insert_image_dimensions` inspects every `<img>` in the catalog product gallery, reads dimensions from the underlying file (cached after the first hit), and inserts matching `width` and `height` attributes. Because Fotorama uses the first image's intrinsic dimensions to compute its slot height, setting dimensions explicitly lets the browser reserve the correct box _before_ Fotorama runs. The "gallery jumps when it initializes" shift disappears as soon as the slot is pre-sized.

Test `insert_image_dimensions` on your responsive templates before enabling it store-wide. Writing fixed `width`/`height` onto catalog markup that was relying on CSS to size the image can interfere with some responsive grid layouts — the `view.xml` aspect-ratio work below plus a `max-width: 100%; height: auto;` rule covers the common case, but custom and Hyvä-adjacent themes vary.

`lazyload_images` reserves dimensioned placeholders for thumbnails and category-page tiles, deferring the actual fetch until they scroll into view. Because the placeholder carries dimensions, no shift fires when the real image swaps in.

`prioritize_critical_css` inlines above-fold CSS to reduce the FOUC window — useful on Magento, where Luma ships a long CSS chain and external stylesheet load contributes to the period during which Fotorama is computing dimensions.

What the rewriter does and doesn't change: Magento's gallery and private-content choices are architectural. The rewriter can't pre-size content that doesn't exist in the initial HTML. But it can guarantee that every `<img>` in the static catalog HTML carries dimensions, which removes the dominant CLS source for product pages.

What this does not fix: the mini-cart re-flow and private-content injection. Both are deliberate Magento behaviors for cacheability — the customer-specific markup is _supposed_ to arrive after the cached HTML renders. Fixes for these live in theme CSS and Varnish ESI, covered next.

## Tuning Magento outside the rewriter

- Configure all catalog product images to a uniform aspect ratio in `app/design/frontend/<Vendor>/<theme>/etc/view.xml`. Set `<image id="product_page_image_large" type="image">` width and height to matched values (e.g., 800×800). Then run `bin/magento catalog:images:resize` to regenerate the cached image sizes. Combined with `insert_image_dimensions`, the gallery is correctly pre-sized for every product.
- Reserve gallery space in theme CSS as a defense-in-depth backstop: `.product.media { aspect-ratio: 1 / 1; min-height: 400px; }`. The `min-height` covers the case where CSS aspect-ratio hasn't computed yet.
- Reserve mini-cart width: `.minicart-wrapper { min-width: 50px; }`. This stops single-digit-to-double-digit cart count from re-flowing the header.
- Move customer-specific banners from `customer-data` (client-side) to Varnish ESI (server-side). Magento supports `<esi:include>` tags; the banner renders inside the cached HTML, no post-load injection, no shift. Magento's docs cover `Magento_PageCache` and ESI integration.
- For Hyvä-theme stores: Hyvä replaces Fotorama with a lighter Alpine.js gallery and removes most CLS sources by design. If you're considering Hyvä for other reasons, CLS improvement is a meaningful side benefit.
- Switch storefront to production mode (`bin/magento deploy:mode:set production`) and run `bin/magento setup:static-content:deploy` after every deployment. Developer mode adds inline `<style>` and JS that lengthens the render window during which Fotorama init happens.

## How to confirm it took

1. DevTools → Performance → reload trace with Slow 4G throttling. The Layout Shift Regions overlay should be empty across the gallery and cross-sells.
2. Re-run PSI on a product URL after the gallery aspect-ratio change. "Avoid large layout shifts" should drop to zero or list only the mini-cart bubble.
3. After 28 days, Search Console → Core Web Vitals report. The product URL group should move out of "Needs improvement". Magento sites typically see CLS gains first because product pages dominate URL count and they are the worst CLS offenders.
4. Watch CrUX field CLS, not just Lighthouse lab. The lab won't always trigger the mini-cart re-flow path; the field will.

## The minimal nginx config

```nginx
# nginx — minimal config to address CLS on Magento
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters insert_image_dimensions;
pagespeed EnableFilters lazyload_images;
pagespeed EnableFilters prioritize_critical_css;
pagespeed FetchHttps enable;
# Magento serves a lot of media from /pub/media — make sure it's fetchable:
pagespeed Domain https://example.com;
```

For Apache:

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed
ModPagespeedRewriteLevel CoreFilters
ModPagespeedEnableFilters insert_image_dimensions
ModPagespeedEnableFilters lazyload_images
ModPagespeedEnableFilters prioritize_critical_css
# Magento serves a lot of media from /pub/media — make sure it's fetchable:
ModPagespeedDomain https://example.com
```

## Limits of the server-layer fix

- If gallery CLS persists after both `insert_image_dimensions` and the `view.xml` aspect-ratio change, Fotorama is recomputing dimensions on a window resize event. Mobile rotation triggers this. Fix: lock `.fotorama__stage` height with CSS instead of relying on Fotorama's auto-fit.
- Mini-cart re-flow on mobile is a CSS problem, not a rewriter problem. If the bubble is sized correctly via `min-width` it stops shifting; the rewriter does not touch the mini-cart markup because it's generated client-side from a Knockout.js template.
- Private-content blocks require ESI migration or pre-rendering. A rewriter cannot intercept content that the application explicitly chose to defer for cacheability.
- If you are running on a CDN that doesn't allow the rewriter's server-side fetches to the origin (some Cloudflare configurations block server-to-server traffic), `insert_image_dimensions` cannot read image metadata. Allowlist the rewriter's IP, or pre-publish dimensions to the database and render them in the catalog template directly.

## Related

- [How to fix LCP on Magento](/blog/fix-lcp-magento-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix INP on Magento](/blog/fix-inp-magento-2026/)
- [How to fix CLS on nginx](/blog/fix-cls-nginx-2026/)
- [Critical CSS without a headless browser](/blog/critical-css-heuristics/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full CLS guide](/core-web-vitals/cls/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).
