---
title: 'Fix LCP on Magento without breaking RequireJS'
description: 'How to fix LCP on Magento: tame the RequireJS waterfall, regenerate catalog images, inline critical CSS at the server. Step-by-step for Magento 2 in 2026.'
date: 2026-04-28
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'lcp', 'magento']
draft: false
product: '1.1'
howTo:
  name: How to fix LCP on Magento 2
  description: Reduce Largest Contentful Paint on Magento 2 by diagnosing the LCP element, layering mod_pagespeed filters in front of Varnish, and regenerating catalog images and static assets inside Magento.
  tools:
  - PageSpeed Insights
  - Chrome DevTools
  - mod_pagespeed
  - nginx
  - Varnish
  - Magento 2 CLI (bin/magento)
  steps:
  - name: Pinpoint the LCP element
    text: Run PageSpeed Insights on a representative PDP and PLP and read the Largest Contentful Paint element breakdown. Use Chrome DevTools Performance and Network panels under mobile throttling to see whether the long pole is TTFB, the RequireJS JS waterfall, or the CSS chain, and capture the LCP element's file vs rendered dimensions.
  - name: Inline critical CSS and rewrite images with mod_pagespeed
    text: Run mod_pagespeed as an nginx module between Varnish and the client. Enable prioritize_critical_css to inline above-the-fold styles, recompress_images and convert_jpeg_to_webp to cut catalog image bytes, and insert_image_dimensions before resize_images so the resizer can size grid images to their rendered dimensions.
  - name: Tune Magento outside the rewriter
    text: Set production mode with bin/magento deploy:mode:set production and run setup:static-content:deploy after theme changes. Enable Merge and Minify JavaScript (not stock Bundle), match the etc/view.xml image sizes to the rendered CSS sizes then run bin/magento catalog:images:resize, and keep Varnish in front for full-page cache.
  - name: Confirm the hero loads faster
    text: In DevTools Network, confirm catalog images are served as WebP at rendered dimensions, and load the page with ?PageSpeedFilters=+debug to verify which filters ran. Re-run PSI to confirm smaller element render delay and resource load duration, then check the Search Console Core Web Vitals field data after 28 days.

---

LCP on Magento is one of the harder cells in the matrix. Platform-side work — production mode, Varnish, image cache regeneration — does most of the heavy lifting; mod_pagespeed handles the CSS chain and the byte budget on top. Magento 2 storefronts routinely render the category banner or product hero past 3 s on mobile, and the Adobe Page Builder pages are worse. The plan: tame the RequireJS waterfall, regenerate catalog images to match the theme's rendered sizes, then layer **mod_pagespeed** (an nginx module) in front of Varnish to inline critical CSS and rewrite images.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What LCP measures

LCP — Largest Contentful Paint — is the render time of the largest above-the-fold element on a page. On Magento PDPs that is usually the main gallery image; on category (PLP) pages it's the first product card; on CMS pages built with Page Builder it depends on the row layout. The "good" threshold is under 2.5 s and "poor" starts at 4 s. The field figure comes from Chrome's CrUX dataset — real users over the trailing 28 days — while the lab figure comes from Lighthouse and PageSpeed Insights. See [web.dev/articles/lcp](https://web.dev/articles/lcp) for the canonical definition.

## The most common LCP failures on Magento

Magento 2 LCP has a small number of repeat offenders:

1. **The RequireJS waterfall blocks the hero.** Magento 2 ships RequireJS for module loading; the default Luma and Blank themes pull roughly 20 JS modules in series before the page becomes interactive. The gallery widget (`Magento_ProductVideo`, `Magento_Swatches`) initializes synchronously, which holds back the hero image's render. On a fresh cache miss, this routinely adds 400–800 ms to LCP.

2. **The image catalog uses upload-sized originals.** Magento stores product images at upload resolution and resizes on demand under `pub/media/catalog/product/cache/...`. The cached resize uses the _theme's_ configured size from `etc/view.xml`. If the theme declares `<image id="category_page_grid"><width>240</width><height>300</height></image>` but the theme's CSS sizes that slot at 400×500, the browser upscales — LCP attribution moves to the now-blurry image's decode time and the byte cost is much higher than the rendered size requires. Matching served bytes to the slot they land in is the same problem covered in [viewport-aware image optimization](/blog/viewport-aware-image-optimization/).

3. **Critical CSS lives in `styles-m.css`, not in the HTML.** Magento's full-page cache (built-in or Varnish) serves cached HTML quickly, but the critical styles live in `_static/version<N>/frontend/.../css/styles-m.css` — a separate file fetched after HTML parse. On a fresh visit, that round-trip adds 200–400 ms to LCP even when everything else is warm.

## Pinpoint the LCP element

Before changing anything:

- Run PageSpeed Insights on a representative PDP and PLP: `https://pagespeed.web.dev/analysis?url=<URL>`. Read the **Largest Contentful Paint element** breakdown. On Magento the resource-load-delay portion is usually large — that's the RequireJS waterfall pushing image decode out.
- Chrome DevTools → **Performance** panel → record under mobile throttling (Slow 4G + 4× CPU). Look at the Main thread: a wall of RequireJS `define` calls before the LCP marker is the diagnostic signature.
- DevTools → Network panel → filter on `JS`, sort by **Time**. Magento's `requirejs/require.js` followed by 15–20 `Magento_*` modules in series is the canonical waterfall.
- `bin/magento dev:profiler:enable` enables the built-in profiler; the rendered page footer (with profiler on, in developer mode only) shows timing per layout block. Useful for finding slow blocks contributing to TTFB.
- `bin/magento cache:status` confirms which Magento caches are enabled. Production stores should show all caches "Enabled".

Output you're hunting for: the LCP element, its file dimensions vs rendered dimensions, and whether the long pole is TTFB, the JS waterfall, or the CSS chain.

## Defer non-critical CSS while Varnish handles the cache layer

mod_pagespeed runs as an nginx module in front of Magento — most Magento 2 stores already use nginx (typically with Varnish in front for full-page cache). The filters that move Magento LCP:

- `prioritize_critical_css` — extracts the above-the-fold rules from the styles-m.css chain and inlines them into the cached HTML response. This eliminates the round-trip from failure mode #3.
- `recompress_images` + `convert_jpeg_to_webp` — re-encodes catalog images at a lower quality threshold and transcodes to WebP. Catalog images are the byte-budget half of LCP.
- `resize_images` — for category grids where the rendered size doesn't match the cache-folder size, the rewriter resizes server-side to the rendered dimensions. One dependency to know: `resize_images` only fires when the `<img>` already carries `width` and `height` attributes. Many Magento templates emit catalog `<img>` tags without them, so pair this with `insert_image_dimensions` (run it first) to supply the dimensions the resizer needs. Test `insert_image_dimensions` on responsive templates before rollout — writing fixed dimensions can interfere with some responsive grid layouts.

If the customer is running Magento's PWA Studio (Venia), they're serving an SPA — the initial paint is a JS skeleton, and mod_pagespeed is irrelevant to LCP. Be explicit about this with the team before configuring anything; see "When this doesn't work" below.

Minimal nginx config:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters prioritize_critical_css;
pagespeed EnableFilters recompress_images;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters insert_image_dimensions;
pagespeed EnableFilters resize_images;
# Magento serves catalog images via /media/catalog/product/cache/...
# insert_image_dimensions runs before resize_images so the resizer has
# width/height to act on; keep the recompression conservative on photos
pagespeed ImageRecompressionQuality 80;
```

Place the `pagespeed` directives in the same `server { }` block as the existing Magento/Varnish config. mod_pagespeed sits between Varnish and the client; the rewriter runs on Varnish-cached HTML responses.

After enabling, re-run PSI. LCP attribution should show the rewritten WebP variant being served and the resource-load-duration portion of the element timing should drop. If LCP element render delay stays high, the long pole is the RequireJS waterfall (failure mode #1), which is platform-side.

## Tuning Magento outside the rewriter

What you do inside Magento to stack additional wins:

- Switch to production mode: `bin/magento deploy:mode:set production`. Developer mode disables many caches and ships unminified assets.
- Run `bin/magento setup:static-content:deploy -f en_US` after any theme or CSS change. This generates the static `pub/static/` assets that the storefront serves.
- In Admin → Stores → Configuration → Advanced → Developer → JavaScript Settings, enable **Merge JavaScript Files** and **Minify JavaScript Files**. Do _not_ enable "Bundle JavaScript Files" without testing — Magento's stock bundler ships unused modules per page-type and typically worsens LCP, not improves it.
- Configure `<image>` sizes in `app/design/frontend/<vendor>/<theme>/etc/view.xml` to match the rendered size in CSS, then flush the image cache: `bin/magento catalog:images:resize`. This regenerates `pub/media/catalog/product/cache/` at the correct sizes.
- Run Varnish in front of nginx. Magento's PHP execution for an uncached page is 800–2000 ms; LCP cannot recover from that without a full-page cache layer.

## How to confirm the hero loads faster

1. DevTools → Network panel → reload, filter on `Img`, confirm catalog images are served as WebP. Confirm they are sized at the rendered dimensions, not the upload dimensions.
2. Open the page with `?PageSpeedFilters=+debug` — the response includes HTML comments showing which filters ran on which assets.
3. Re-run PSI on the same URL. The LCP element timing should show smaller element render delay (critical CSS inlined) and smaller resource load duration (WebP variant served).
4. After 28 days, check Search Console → Core Web Vitals report. The product and category URL groups should move from "Needs improvement" to "Good". Watch field data, not just lab.

## Configuration cheat sheet

```nginx
# nginx — minimal config to address LCP on Magento 2
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters prioritize_critical_css;
pagespeed EnableFilters recompress_images;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters insert_image_dimensions;
pagespeed EnableFilters resize_images;
pagespeed EnableFilters hint_preload_subresources;
pagespeed ImageRecompressionQuality 80;
```

The full directive reference lives in the [configuration reference](/docs/configuration/).

## Where this approach falls short

Cases where mod_pagespeed alone isn't enough on Magento:

- **PWA Studio (Venia) or any headless Magento front-end.** The initial paint is a JS skeleton from a React/Apollo bundle. mod_pagespeed rewrites server-rendered HTML; it has no contact with the SPA's render pipeline. LCP for headless Magento is a build-time concern — Next.js `<Image>`, route-level code-splitting, and a CDN-side image-resizing service do the work.
- **Uncached PDP TTFB over 1.5 s.** If Varnish ESI is misconfigured or the PHP layer is doing synchronous database work on every product view, the TTFB ceiling alone keeps LCP above 2.5 s. Fix the cache stack first.
- **Adobe Page Builder rows with heavy third-party embeds.** Builder pages often include YouTube or video-poster embeds that lazy-load below-the-fold but become the LCP candidate on shorter viewports. mod_pagespeed cannot rewrite third-party iframe content. Use a static-poster facade pattern, or move the embed below the fold.
- **The biggest INP wins on Magento come from replacing the front-end (Hyvä theme).** LCP work helps the visual metric; the Knockout + RequireJS architecture is an INP problem that no rewriter addresses. See [How to fix INP on Magento](/blog/fix-inp-magento-2026/).

## Related

- [How to fix INP on Magento](/blog/fix-inp-magento-2026/)
- [How to fix CLS on Magento](/blog/fix-cls-magento-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [How to fix LCP on nginx](/blog/fix-lcp-nginx-2026/)
- [Critical CSS without a headless browser](/blog/critical-css-heuristics/)
- [Self-hosted image optimization](/blog/self-hosted-image-optimization/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full LCP guide](/core-web-vitals/lcp/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. CoreFilters are enabled by default. See [pricing](/pricing/) and [license terms](/license/).
