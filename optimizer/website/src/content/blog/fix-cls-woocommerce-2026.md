---
title: 'Fix CLS on WooCommerce: lock the gallery'
description: 'How to fix CLS on WooCommerce: stop the gallery shift, lock variation swap dimensions, reserve cross-sell space, and rewrite img tags at the server.'
date: 2026-04-21
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'cls', 'woocommerce']
draft: false
product: '1.1'
howTo:
  name: How to fix CLS on WooCommerce
  description: Reduce Cumulative Layout Shift on WooCommerce product pages by locking gallery and variation-swap image dimensions at the server layer and reserving space for the zero-height thumbnail and cross-sell containers.
  tools:
  - mod_pagespeed
  - nginx
  - Apache
  - PageSpeed Insights
  - Chrome DevTools
  - WooCommerce
  steps:
  - name: Diagnose the shifting elements
    text: Run PageSpeed Insights on the product URL and read the "Avoid large layout shifts" diagnostic to find the failing selector (usually .woocommerce-product-gallery or .cross-sells). Use Chrome DevTools Performance with Layout Shift Regions and throttling to reproduce the shift on reload and on a variation-swatch click, and record a baseline CLS.
  - name: Enable image-dimension filters at the server layer
    text: Configure mod_pagespeed (an nginx or Apache module) with insert_image_dimensions to read each gallery and thumbnail image's real size and write matching width/height attributes, plus lazyload_images, inline_preview_images, and prioritize_critical_css. Test on responsive templates before site-wide rollout, since fixed dimensions can interfere with some custom galleries.
  - name: Unify product image aspect ratios in WooCommerce
    text: In the theme's functions.php, call add_image_size('shop_single', 800, 800, true) with crop true and run Regenerate Thumbnails once so every variant shares one aspect ratio. The variation-swap shift disappears when all variants are the same shape; on WooCommerce 8.0+ consider the Blocks Product Gallery, which renders dimensions server-side.
  - name: Reserve space for the zero-height containers in CSS
    text: 'The rewriter cannot size the thumbnail strip or cross-sell sections because they start at zero height with no img to inspect. Reserve them manually: .woocommerce-product-gallery { aspect-ratio: 1 / 1; }, .cross-sells { min-height: 320px; }, and .flex-control-thumbs { min-height: 80px; }, tuning values to the expected populated height.'
  - name: Confirm the gallery and cross-sells stay put
    text: Re-run PageSpeed Insights until "Avoid large layout shifts" lists no shifted elements, and record a DevTools trace of a reload plus a variation-swatch click to confirm the Layout Shift Regions overlay is empty. Watch the CrUX field number over the following weeks, not just the Lighthouse lab run, since variation-swap shifts only fire under real interaction.
lastUpdated: 2026-10-22
faq:
- q: What CLS score does a WooCommerce product page need to pass Core Web Vitals?
  a: Under 0.1 at the 75th percentile of real Chrome users. PageSpeed Insights reports the field number; optimize toward it, not the Lighthouse lab run.
- q: Why does clicking a variation swatch cause layout shift?
  a: The variation script swaps the main image, and if the swapped-in image has different intrinsic dimensions than the layout reserved, the difference records as CLS. Give every variant the same aspect ratio and write width and height onto the img so the browser holds the slot before pixels arrive.
- q: Can insert_image_dimensions fix the cross-sell shift?
  a: 'No. The AJAX cross-sell container starts at zero height with no img for the rewriter to inspect, so it needs reserved space in CSS, such as .cross-sells { min-height: 320px; }.'

---

The gallery on a WooCommerce PDP scores 0.28 CLS on mobile, the variation swatches inflate it further, and the AJAX cross-sells finish the job. Getting under 0.1 doesn't require abandoning Storefront or rewriting the variation JavaScript: force dimensions onto every `<img>` in the gallery at the server layer with **mod_pagespeed** (an nginx or Apache module) so the variation swap and cross-sells stop pushing content around.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What CLS measures

Cumulative Layout Shift tallies every unexpected movement of visible content from load start until the page goes interactive. Each shift scores as the fraction of the viewport that moved times the distance it travelled; the worst session window's running total is the page's CLS. A "good" score per [web.dev/cls](https://web.dev/articles/cls) sits under 0.1.

Shifts inside 500 ms of a user click are excluded — important on WooCommerce because a click on a variation swatch will not register a CLS hit _if the image swap completes within the window_. The variation swap becomes a problem when the network is slow or the swapped-in image has different dimensions than what the layout reserved. PSI shows you the 75th-percentile CLS from real Chrome users; Lighthouse shows the lab single run. Optimize toward the field number.

## The most common CLS failures on WooCommerce

Three product-page patterns dominate.

**Product gallery thumbnails mount after JS init.** WooCommerce's `product-thumbnails.js` builds the horizontal thumbnail strip after `DOMContentLoaded`. The strip starts at zero height in the initial HTML, then expands to ~80 px when JS runs. Everything below (price block, add-to-cart button, description tabs, related products) jumps down by exactly that amount. The shift is reproducible: throttle network to 3G in DevTools, reload, watch the layout settle in two visible stages.

**Variation swatches swap the main image without preserving height.** Click a color swatch, `add-to-cart-variation.js` fetches the variation's image URL and swaps it into the main gallery slot. If the swap target's intrinsic dimensions differ from the current image (one variant is a square crop, another is portrait), CLS records the difference. This is especially common for stores with both shot-on-model and product-only photography across variants.

**AJAX cross-sells, upsells, and "Customers also bought" carousels.** Storefront and many premium themes render the cross-sell section as a placeholder that gets populated via AJAX. The placeholder is empty, the populated section is 300–500 px tall. CLS records the inflation. The same shape applies to YITH Frequently Bought Together, "Related Products by Category", and most upsell plugins.

## Spotting the gallery shift

Run PSI on the affected product URL: `https://pagespeed.web.dev/analysis?url=https://example.com/product/your-product/`. The "Avoid large layout shifts" diagnostic lists the shifted elements; on WooCommerce the worst offender is usually `.woocommerce-product-gallery` or `.cross-sells`.

Open Chrome DevTools → Performance panel → Settings cog → enable "Web Vitals" and "Layout Shift Regions". Throttle CPU to 4× and network to "Slow 4G". Reload the product page and click a variation swatch. Each shift surfaces as a red rectangle in the overlay; the Layout Shifts track in the trace shows which DOM node moved.

The `web-vitals` library with attribution gives you a single string per shift naming the worst element. Stick `onCLS(console.log, { reportAllChanges: true })` into the theme's footer for one diagnostic session, then remove.

What you want before changing anything: a baseline CLS, the failing selector, and which of the three patterns above it maps to.

## Lock gallery and variation-swap dimensions with insert_image_dimensions

CLS on WooCommerce is largely an image-dimensions problem, and `insert_image_dimensions` is the central filter for it. The rewriter inspects every `<img>` in the gallery and the thumbnail strip, reads dimensions from the underlying file (cached after first hit), and inserts matching `width` and `height` attributes. With the theme's existing `img { max-width: 100%; height: auto; }` rule, the browser reserves the correct aspect-ratio box before the image loads. The variation-swap shift specifically benefits: when the swapped-in image already carries dimension attributes, the browser holds the slot at the new size before fetching pixels.

Test `insert_image_dimensions` on your responsive templates before rolling it out site-wide. Writing fixed `width`/`height` onto markup that was relying on CSS to size the image can interfere with some responsive gallery layouts — the `img { max-width: 100%; height: auto; }` rule above covers the common case, but custom Storefront children and builder-plugin galleries vary.

`lazyload_images` reserves a sized placeholder for offscreen gallery images and below-fold cross-sell tiles, deferring the actual network fetch. Because the placeholder is dimensioned, the slot is reserved and no shift fires when the real image swaps in.

`inline_preview_images` inserts a low-quality data-URI placeholder for the hero image so the gallery slot has visible content before the full image arrives. Combined with `insert_image_dimensions`, the gallery never shows an empty box and never re-flows.

`prioritize_critical_css` reduces the FOUC window when the theme's external CSS is slow to arrive — useful on shops with long CSS chains from Storefront + a child theme + a builder plugin.

The case for the server layer: WooCommerce's plugin layering (theme + child theme + page builder + variation plugins) means _something_ in the stack will eventually inject an image without dimensions. The rewriter catches them all regardless of which layer produced the markup.

What this does not fix: the thumbnail-strip and cross-sell containers themselves. Those containers start at zero height in the initial HTML — there is no `<img>` for the rewriter to find. They need server-side rendering or reserved space in CSS (next section).

## Tuning WooCommerce itself

- Force all product images to a uniform aspect ratio: in your theme's `functions.php`, call `add_image_size( 'shop_single', 800, 800, true )` with crop `true`. Run "Regenerate Thumbnails" once after the change. The variation-swap shift disappears entirely if every variant shares one aspect ratio.
- Reserve space for the gallery container in theme CSS: `.woocommerce-product-gallery { aspect-ratio: 1 / 1; }`. With `insert_image_dimensions` running and a uniform image aspect ratio, the slot is correct before any image loads.
- Reserve the cross-sells section: `.cross-sells { min-height: 320px; }`. Tune the number to the largest expected populated height.
- Reserve the thumbnail strip: `.flex-control-thumbs { min-height: 80px; }` (the default Storefront thumbnail row height).
- Disable mobile zoom on the product gallery — the zoom overlay adds a brief layout shift on touch. CSS-only: `@media (max-width: 768px) { .woocommerce-product-gallery .zoomImg { display: none; } }`.
- If you are on WooCommerce 8.0+, migrate the product page to the Blocks "Product Gallery" block. The block renders dimensions server-side and ships fewer JS-driven mutations than the legacy template.

## How to confirm the gallery stays put

1. Re-run PSI on the product URL. "Avoid large layout shifts" should drop to zero shifted elements, or list only the cookie banner if you have one.
2. DevTools → Performance → record a reload trace + click a variation swatch. The Layout Shift Regions overlay should be empty across the gallery and cross-sells.
3. After 28 days, Search Console → Core Web Vitals report. The product URL group should move from "Needs improvement" to "Good" — WooCommerce sites typically see the largest gain here because product pages dominate the URL count.
4. Watch the CrUX field number, not just Lighthouse lab. Variation-swap shifts only fire under real user interaction; the lab may report 0 while the field reports 0.3.

## What to add to your nginx.conf

```nginx
# nginx — minimal config to address CLS on WooCommerce
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters insert_image_dimensions;
pagespeed EnableFilters lazyload_images;
pagespeed EnableFilters inline_preview_images;
pagespeed EnableFilters prioritize_critical_css;
pagespeed FetchHttps enable;
```

For Apache:

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed
ModPagespeedRewriteLevel CoreFilters
ModPagespeedEnableFilters insert_image_dimensions
ModPagespeedEnableFilters lazyload_images
ModPagespeedEnableFilters inline_preview_images
ModPagespeedEnableFilters prioritize_critical_css
```

## When this doesn't work

- If gallery CLS persists after enabling `insert_image_dimensions`, your product images are likely served from a third-party CDN that does not return the correct `Content-Length` and dimensions on the first response. The rewriter needs to fetch the image to read its size; if the CDN blocks crawlers or rate-limits server-to-server traffic, the fetch fails and the dimensions are not inserted. Allow your origin server's IP through the CDN, or stage product images on the origin.
- If the variation-swap shift persists even with uniform aspect ratio configured, the variation plugin (e.g., Variation Swatches Pro) may be writing `<img>` tags through JavaScript at swap time with no dimensions. The rewriter only rewrites HTML in the initial server response. Fix at the plugin level: most variation-swatch plugins have an "Apply image dimensions" setting in their admin.
- AJAX-loaded cross-sells require reserved CSS space — no rewriter can predict how many products the AJAX endpoint will return.
- If you see CLS in Lighthouse but not in CrUX, the lab is throttling CPU to expose race conditions real users do not hit. Trust the field number.

## FAQ

**What CLS score does a WooCommerce product page need to pass Core Web Vitals?**

Under 0.1 at the 75th percentile of real Chrome users. PageSpeed Insights reports the field number; optimize toward it, not the Lighthouse lab run.

**Why does clicking a variation swatch cause layout shift?**

The variation script swaps the main image, and if the swapped-in image has different intrinsic dimensions than the layout reserved, the difference records as CLS. Give every variant the same aspect ratio and write width and height onto the img so the browser holds the slot before pixels arrive.

**Can insert_image_dimensions fix the cross-sell shift?**

No. The AJAX cross-sell container starts at zero height with no img for the rewriter to inspect, so it needs reserved space in CSS, such as .cross-sells { min-height: 320px; }.

## Related

- [WordPress full-page caching plugin](/wordpress/)
- [How to fix LCP on WooCommerce](/blog/fix-lcp-woocommerce-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix INP on WooCommerce](/blog/fix-inp-woocommerce-2026/)
- [How to fix CLS on WordPress](/blog/fix-cls-wordpress-2026/)
- [The economics of image optimization](/blog/economics-of-image-optimization/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full CLS guide](/core-web-vitals/cls/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).
