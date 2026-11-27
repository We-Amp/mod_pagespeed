---
title: 'Fix LCP on WooCommerce: shrink the gallery first'
description: 'Fix LCP on WooCommerce: shrink the product gallery, drop cart-fragments on non-shop pages, and rewrite catalog images at the server with mod_pagespeed.'
date: 2026-04-16
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'lcp', 'woocommerce', 'image-optimization']
draft: false
product: '1.1'
faq:
  - q: Why is LCP slow on WooCommerce product pages?
    a: On most WooCommerce product pages the LCP element is the main gallery image, uploaded at 2000-3000 px and served into an 800-px slot. The flexslider lazy-loader also defers it until JS executes, so LCP waits on the cart-fragments AJAX call.
  - q: Can mod_pagespeed fix WooCommerce LCP?
    a: mod_pagespeed resizes, recompresses, and converts catalog images to WebP at the nginx or Apache layer, applying the fix to the whole catalog at once instead of editing theme files. It cannot help when the gallery is rendered by a JavaScript SPA or when TTFB is the long pole.
  - q: What counts as a good LCP score?
    a: Google calls field LCP under 2.5 s "good" and over 4 s "poor", measured on real users over the trailing 28 days in Chrome's CrUX dataset.
howTo:
  name: How to fix LCP on WooCommerce
  description: Fix WooCommerce LCP by attributing the largest gallery image, then resizing, recompressing, and converting catalog images to WebP at the server with mod_pagespeed, plus stacking WooCommerce-side wins.
  tools:
  - PageSpeed Insights
  - Chrome DevTools
  - Lighthouse
  - mod_pagespeed
  - nginx
  - Apache
  - WooCommerce
  - Google Search Console
  steps:
  - name: Find the gallery image blowing the LCP budget
    text: Run PageSpeed Insights on a representative product URL and read the Largest Contentful Paint element box. In Chrome DevTools, filter the Network panel on Img and sort by size to find above-the-fold images over 200-500 KB, and check WooCommerce > Status > Tools for registered image sizes. The target output is the LCP element's selector, file size, and the mismatch between file dimensions and the rendered slot.
  - name: Resize and recompress catalog images at the server
    text: Enable mod_pagespeed as an nginx or Apache module with recompress_images, convert_jpeg_to_webp, inline_preview_images, and resize_images. Run insert_image_dimensions before resize_images so the resizer has width/height to act on a typical WooCommerce gallery, and set ImageRecompressionQuality 78 for photographic catalogs after verifying quality on your own images.
  - name: Stack WooCommerce-side fixes
    text: Dequeue wc-cart-fragments.js outside is_woocommerce/is_cart/is_checkout, run Regenerate Thumbnails after any image-size change, set jpeg_quality to 82, and preload the hand-picked hero image with a high-fetchpriority link. Disable WooCommerce Blocks on stores not using block-based checkout.
  - name: Confirm the gallery shrank
    text: Reload the product URL in DevTools Network and confirm the gallery image is served as image/webp at a transferred size matching the rendered slot. Load the page with ?PageSpeedFilters=+debug to see which filters ran, re-run PSI to confirm the optimized variant in LCP attribution, and after 28 days check the Search Console Core Web Vitals report for the /product/ URL group moving to Good.

---

Three product pages, three different LCP elements: the gallery on the PDP, the hero block on the homepage, a category banner on `/shop/`. That's WooCommerce, and it's why we tackle this at the server layer with **mod_pagespeed** (an nginx or Apache module) instead of pasting `loading="eager"` into 40 theme files. Shrink the gallery image at the source, kill the cart-fragments AJAX call where it doesn't belong, and let the rewriter apply the fix to the whole catalog at once.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What LCP measures

LCP — Largest Contentful Paint — clocks the render time of the largest above-the-fold element. On a WooCommerce product page that element is almost always the main gallery image; on category pages it's the first product card; on the homepage it depends on the theme. Google calls anything under 2.5 s "good" and anything over 4 s "poor". The field number comes from Chrome's CrUX dataset (real users over the trailing 28 days); the lab number comes from Lighthouse and PageSpeed Insights, and the two routinely disagree because real users run slower hardware than your developer machine. See [web.dev/articles/lcp](https://web.dev/articles/lcp) for the canonical definition.

## The most common LCP failures on WooCommerce

Almost every failing WooCommerce PDP fails for the same reason — gallery image weight. The other two failure modes are below:

1. **Product galleries lazy-load eagerly, but in the wrong order.** Storefront and WooCommerce Blocks insert the product gallery with `data-large_image` URLs at full upload resolution — often 2000–3000 px wide. On a product page, the LCP element is usually the main gallery image, but the lazy-loader (`woocommerce.flexslider.js`) defers it until JS executes, which means LCP waits on the cart-fragments AJAX call. Net effect: LCP firing at 3.5–5 s on 4G even with a moderately sized image.

2. **`wc-cart-fragments.js` runs on every page.** This AJAX call refreshes the mini-cart count via `admin-ajax.php`, fires on every page including the homepage and blog, and blocks `domInteractive`. Pages with the cart-fragments call typically show a 600–900 ms gap between FCP and LCP on slow connections.

3. **Product images aren't regenerated after a theme switch.** A common deploy: the theme changes from Storefront to a "premium" theme, the new theme declares a different `add_image_size( 'shop_single', 800, 800 )`, but `wp-content/uploads/` still holds the old sizes. WordPress serves the closest available size, which on most stores is the 2048-wide original. LCP regresses and nobody notices because the image still "looks fine".

## Find the gallery image that's blowing the budget

Before changing anything, get the baseline and the attribution.

- Run PageSpeed Insights on a representative product URL: `https://pagespeed.web.dev/analysis?url=<URL>`. Read the **Largest Contentful Paint element** box. The element selector usually points at the main gallery `<img>`; if it points at the `<h1>` instead, your gallery is being lazy-loaded out of LCP candidacy entirely (which is its own problem — see failure mode #1).
- Open Chrome DevTools → **Network** panel → filter on `Img`, sort by **Size**. Anything over 200 KB above the fold is a candidate for the server-side rewrite. Anything over 500 KB is the LCP culprit on most stores.
- Open DevTools → **Performance** panel → record under mobile throttling (Slow 4G + 4× CPU). Click the LCP marker on the Timings track and read the Related Node.
- For repeatable measurement: `npx lighthouse <PRODUCT_URL> --only-categories=performance --form-factor=mobile`.
- In WP Admin → WooCommerce → Status, scroll to **Tools** and look at the registered image sizes. If `shop_single` is 800×800 but the underlying upload is 2048×2048, you have the regenerate-thumbnails problem.

Output you're hunting for: the LCP element's selector, its file size, and its rendered dimensions. Disagreement between file dimensions and rendered dimensions (a 2000-px file served into an 800-px slot) is the most common LCP cause.

## Resize and recompress catalog images at the server

mod_pagespeed runs as an nginx module (most WooCommerce stores are nginx-fronted) or an Apache module. It rewrites HTML on the way out and serves optimized image variants from a shared cache. The filters that move LCP on WooCommerce:

- `recompress_images` — re-encodes JPEGs and PNGs at a configurable quality threshold. On photographic product imagery, dropping from quality 90 to 78 typically saves 30–50% of bytes with no visible difference.
- `convert_jpeg_to_webp` — transcodes to WebP for browsers that advertise support. The variant is served from cache on subsequent requests at HIT speed.
- `resize_images` — reads the `<img width=...>`/`<img height=...>` attributes and re-encodes the file to that pixel target, so a 2000-px upload served into an 800-px slot gets shrunk server-side. This is the real win on WooCommerce, where image-size mismatches are the rule rather than the exception. The catch: `resize_images` only fires when the `<img>` already carries `width` and `height` attributes — and WooCommerce galleries (`data-large_image`, Storefront's flexslider markup) routinely ship `<img>` tags with no dimensions at all. On those stores `resize_images` silently no-ops until something supplies the attributes.
- `insert_image_dimensions` — reads each image's real pixel size and writes the missing `width`/`height` onto the `<img>` tag. This is the prerequisite for `resize_images` on WooCommerce: run it _before_ `resize_images` in the pipeline so the resizer has dimensions to act on. (It also fixes the gallery's CLS — see [How to fix CLS on WooCommerce](/blog/fix-cls-woocommerce-2026/) — but here it earns its place by making the resize possible at all. Test it on responsive templates first: writing fixed `width`/`height` can interfere with some responsive gallery layouts.)
- `inline_preview_images` — ships a sub-1 KB low-quality placeholder inline in the HTML for above-the-fold images, so the first paint shows the LQIP while the gallery JS is still parsing.

For photographic catalogs, set `pagespeed ImageRecompressionQuality 78`. The default is 85, but 78 is the threshold where SSIM stays above 0.95 on typical e-commerce photography. Verify on your own catalog with a side-by-side comparison before rolling out.

Why a rewriter beats a per-image audit on WooCommerce: the theme + plugin ecosystem makes per-image hygiene impractical at scale. Regenerating thumbnails after every image-size change works, but it's a manual step that drifts. A [self-hosted image optimizer](/self-hosted-image-optimization/) applies the size + format + compression policy automatically to every page in the catalog, including the products the merchandising team uploads next week.

Minimal nginx config:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters recompress_images;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters insert_image_dimensions;
pagespeed EnableFilters resize_images;
pagespeed EnableFilters inline_preview_images;
pagespeed ImageRecompressionQuality 78;
```

`insert_image_dimensions` is listed before `resize_images` deliberately: it supplies the `width`/`height` attributes that `resize_images` needs to fire on a typical WooCommerce gallery. After enabling, re-run PSI. LCP attribution should show the rewritten WebP variant being served instead of the original JPEG, and the element render delay should drop.

## What WooCommerce can fix on its own

What you can do inside WooCommerce to stack additional wins:

- Disable `wc-cart-fragments.js` on non-shop pages. The snippet circulates widely; in `functions.php`, dequeue `wc-cart-fragments` outside `is_woocommerce()`, `is_cart()`, and `is_checkout()`. Roughly six lines.
- Run the "Regenerate Thumbnails" plugin after any image-size change. This is the WooCommerce-side fix for failure mode #3.
- Set `add_filter( 'jpeg_quality', fn() => 82 )` in `functions.php` to reduce upload-time JPEG size. Pairs cleanly with the server-side recompression; the smaller upload means the rewriter has less to do.
- For the homepage or a featured-product showcase, hand-pick the LCP image URL and add a `<link rel="preload" as="image" fetchpriority="high">` in `header.php`. This is the one place an explicit preload outperforms anything the rewriter can infer.
- Disable WooCommerce Blocks on stores not using the new block-based checkout. The Blocks bundle adds roughly 80 KB of JS that most non-shop pages don't need.

## How to confirm the gallery shrank

1. DevTools → Network panel → reload the product URL, filter on `Img`, confirm the gallery image is served with `content-type: image/webp` (assuming the client supports WebP) and the transferred size matches the rendered slot.
2. Open the page with `?PageSpeedFilters=+debug` — the response includes HTML comments naming which filters ran on which assets.
3. Re-run PSI on the same product URL. LCP attribution should show the optimized variant (`?PageSpeed=variant&v=...` or a hashed filename, depending on configuration). Element render delay should drop.
4. After 28 days, check Search Console → Core Web Vitals report. The `/product/` URL group should move from "Needs improvement" to "Good". Watch field data, not just lab.

## A drop-in nginx snippet

```nginx
# nginx — minimal config to address LCP on WooCommerce
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters recompress_images;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters insert_image_dimensions;
pagespeed EnableFilters resize_images;
pagespeed EnableFilters inline_preview_images;
pagespeed EnableFilters hint_preload_subresources;
# Tune recompression quality for photographic catalogs
pagespeed ImageRecompressionQuality 78;
```

For Apache:

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed
ModPagespeedRewriteLevel CoreFilters
ModPagespeedEnableFilters recompress_images
ModPagespeedEnableFilters convert_jpeg_to_webp
ModPagespeedEnableFilters insert_image_dimensions
ModPagespeedEnableFilters resize_images
ModPagespeedEnableFilters inline_preview_images
ModPagespeedImageRecompressionQuality 78
```

The full directive reference lives in the [configuration reference](/docs/configuration/).

## When this doesn't work

Cases where mod_pagespeed alone isn't enough on WooCommerce:

- **The gallery is rendered by a JavaScript SPA (WC Blocks Checkout, headless WP front-end).** mod_pagespeed rewrites server-rendered HTML; it cannot rewrite a React tree that constructs the gallery in the browser. For headless setups, image optimization moves to the build step (Next.js `<Image>`) or to a CDN-side image-resizing service.
- **TTFB is the long pole.** WooCommerce's uncached database queries on the product page (variations, related products, reviews) can take 800–1500 ms on under-provisioned hosting. No HTML rewriter can shorten a 1 s TTFB; the LCP minimum becomes `TTFB + image-render-time`. Fix the cache stack first — a [server-side page cache](/blog/wordpress-server-side-page-cache-plugin/), plus an object cache and Varnish or LiteSpeed Cache — before the rewriter.
- **The LCP is the checkout button on `/cart/`.** Cart and checkout are interactive flows where the LCP isn't really the user's problem — interactivity (INP) is. See [How to fix INP on WooCommerce](/blog/fix-inp-woocommerce-2026/) for that path.

## FAQ

**Why is LCP slow on WooCommerce product pages?** On most WooCommerce product pages the LCP element is the main gallery image, uploaded at 2000-3000 px and served into an 800-px slot. The flexslider lazy-loader also defers it until JS executes, so LCP waits on the cart-fragments AJAX call.

**Can mod_pagespeed fix WooCommerce LCP?** mod_pagespeed resizes, recompresses, and converts catalog images to WebP at the nginx or Apache layer, applying the fix to the whole catalog at once instead of editing theme files. It cannot help when the gallery is rendered by a JavaScript SPA or when TTFB is the long pole.

**What counts as a good LCP score?** Google calls field LCP under 2.5 s "good" and over 4 s "poor", measured on real users over the trailing 28 days in Chrome's CrUX dataset.

## Related

- [WordPress full-page caching plugin](/wordpress/)
- [How to fix INP on WooCommerce](/blog/fix-inp-woocommerce-2026/)
- [How to fix CLS on WooCommerce](/blog/fix-cls-woocommerce-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [How to fix LCP on WordPress](/blog/fix-lcp-wordpress-2026/)
- [The economics of image optimization](/blog/economics-of-image-optimization/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full LCP guide](/core-web-vitals/lcp/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).
