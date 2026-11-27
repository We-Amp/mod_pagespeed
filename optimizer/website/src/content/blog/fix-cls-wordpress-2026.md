---
title: 'Fix CLS on WordPress: dimensions at the server'
description: 'How to fix CLS on WordPress: diagnose the shifts, set width/height on every image at the server layer, lock down fonts, reserve banner space.'
date: 2026-04-10
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'cls', 'wordpress']
draft: false
lastUpdated: 2026-10-22
product: '1.1'
howTo:
  name: How to fix CLS on WordPress
  description: Diagnose WordPress layout shifts, then reserve space at the server layer with mod_pagespeed image-dimension and critical-CSS filters and apply the manual font, banner, and ad-slot fixes the rewriter cannot make.
  tools:
  - mod_pagespeed
  - nginx
  - Apache
  - PageSpeed Insights
  - Chrome DevTools
  - web-vitals
  - Google Search Console
  steps:
  - name: Read the shift report and identify the owning element
    text: Run PageSpeed Insights on the affected URL and read the "Avoid large layout shifts" diagnostic. Use Chrome DevTools' Layout Shift Regions overlay (or web-vitals with onCLS attribution) to record a baseline CLS score and pin down the element that owns most of the shift and its failure mode.
  - name: Insert image dimensions at the server with mod_pagespeed
    text: 'Enable insert_image_dimensions in mod_pagespeed (nginx or Apache): the rewriter reads each image''s real pixel size and writes matching width/height attributes uniformly across every page, so the browser reserves the slot. Test it on responsive templates before going site-wide.'
  - name: Reserve critical CSS and lazyload placeholders
    text: Enable prioritize_critical_css to inline above-the-fold CSS and avoid the flash-of-unstyled-content shift, and lazyload_images so offscreen images defer behind a correctly sized placeholder. These stop the layout shifts the rewriter can see in the initial HTML.
  - name: Fix font-swap shifts in WordPress
    text: 'Because the swap window is a browser concern the rewriter cannot fix, switch font loading to font-display: optional in functions.php and self-host Google Fonts. This keeps the fallback for the page lifetime instead of reflowing text after the web font arrives.'
  - name: Reserve space for cookie banners and ad slots
    text: 'For content the rewriter cannot predict because it is injected by JavaScript, reserve space in CSS: give cookie banners fixed positioning with bottom padding, set ad containers a min-height matching the largest creative, and apply aspect-ratio to divs wrapping images of unknown size.'
  - name: Measure the lift in the field
    text: Re-run PSI and confirm the layout-shift diagnostic drops to zero or near-zero, then re-record a DevTools trace. After 28 days, check the Search Console Core Web Vitals report and watch the field CrUX CLS rather than the lab Lighthouse number.

---

A 0.32 CLS on the WordPress home page, Search Console flagging it under Core Web Vitals, and forty plugins between you and the layout: the fastest path under 0.1 is not auditing every featured image by hand. Most WordPress CLS regressions come from `<img>` tags missing `width`/`height`, and the fix sits one layer below the theme — **mod_pagespeed** (an nginx or Apache module) rewrites the HTML on the way out the door and applies dimensions uniformly to every page on the site.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What CLS measures

Cumulative Layout Shift captures every unexpected jump of visible content between load start and the moment the page becomes interactive. Each jump is scored by the viewport fraction that moved multiplied by how far it moved, and the highest-summing session window becomes the page's CLS. Per [web.dev/cls](https://web.dev/articles/cls), under 0.1 is "good" and anything above 0.25 is "poor".

Two things matter for the rest of this post. First, only _unexpected_ shifts count — a shift inside 500 ms of, _and attributable to_, a user input is excluded (a shift that happens to land in the window but wasn't caused by the click still scores). Second, the field number you see in PSI is the 75th-percentile CLS from CrUX (real Chrome users); the lab number from Lighthouse is a single synthetic run and often disagrees with the field. Optimize toward the field number.

## The most common CLS failures on WordPress

Most WordPress CLS pain comes from three patterns.

**Featured images and inline `<img>` tags without dimensions.** WordPress 5.5+ tries to set width and height attributes from media-library metadata, but it only does this for images inserted through the block editor with a valid attachment ID. Classic-editor content, externally hosted images, and images inserted via `<img src="...">` in custom templates skip the auto-detection path. The browser reserves zero space for these images, paints the text, then jumps the layout by the rendered image height when bytes arrive. The shift is visually obvious — you can see it in DevTools' Layout Shift overlay as a red rectangle on every paragraph below the image.

**Web fonts with `font-display: swap`.** Twenty Twenty-Two, Twenty Twenty-Three, Twenty Twenty-Four, and Twenty Twenty-Five all ship Google Fonts via `wp_enqueue_style` with the default `display=swap` parameter. Fallback metrics (line-height, x-height, character width) differ from the web font's; when the web font swaps in, every text block re-flows by a few pixels in both axes. On a content-heavy article this adds up to a 0.15–0.25 CLS hit on its own.

**Cookie banners injected via `wp_footer`.** CookieYes, Complianz, and similar plugins inject a fixed-position banner at the end of the body. If the banner is fixed-position at the bottom _and_ it doesn't push content, you're fine; if it's part of the document flow and pushes the footer up by 100–200 px on first visit, CLS records every shift inside the session window. Mediavine and AdSense ad units that lazy-mount into content slots have the same shape.

## Reading the shift report

Run PSI on the affected URL: `https://pagespeed.web.dev/analysis?url=https://example.com/`. Read the "Avoid large layout shifts" diagnostic — it lists the specific elements that shifted and the score each contributed.

In Chrome DevTools, open the page → Performance panel → Settings cog → enable "Web Vitals" and "Layout Shift Regions". Reload. Look for the red rectangle overlay; each rectangle is a shift event. Hover the shift markers in the Timings track to see which DOM node moved and by how much.

For repeatable measurement: install `web-vitals` (`npm i web-vitals`) and log `onCLS({ reportAllChanges: true })` with attribution enabled. This prints the worst element to the console as the page loads — your single attribution string.

The output you want before changing anything: a baseline CLS score, the element that owns most of it, and the failure mode (missing dimensions, font swap, late inject) it maps to.

## Insert image dimensions server-side with insert_image_dimensions

CLS is the metric mod_pagespeed is most effective on. Three filters do most of the work.

`insert_image_dimensions` is the headline filter. The HTML rewriter inspects every `<img>` tag, fetches the underlying file (cached after first hit), reads the actual pixel dimensions, and inserts matching `width` and `height` attributes. Combined with a CSS rule like `img { max-width: 100%; height: auto; }` (which every WordPress theme already ships), modern browsers compute the correct aspect-ratio box before the image loads and reserve the layout slot. The image-jump shift disappears uniformly across every page on the site, including classic-editor content, externally hosted images, and templates that bypass `wp_filter_content_tags`.

Test `insert_image_dimensions` on your responsive templates before enabling it site-wide. Writing fixed `width`/`height` onto markup that was relying on CSS to size the image can interfere with some responsive layouts — the `img { max-width: 100%; height: auto; }` rule that themes ship covers the common case, but page-builder and custom-template markup vary.

`prioritize_critical_css` inlines above-the-fold CSS into the document head and defers the rest. This prevents the flash-of-unstyled-content shift that hits during the brief window between HTML parse and external stylesheet load — a shift that compounds the font-swap CLS on slow connections. For how the filter derives the above-the-fold set, see [server-side critical CSS on nginx](/blog/server-side-critical-css-nginx/).

`lazyload_images` reserves a sized placeholder for offscreen images while deferring the load. This is the layout-shift-safe lazy-loading: the placeholder has the correct dimensions, the browser holds the slot, the real image swaps in without shifting anything.

Why a rewriter beats a theme audit on WordPress: you can audit every theme update, plugin update, and editor-pasted image for missing dimensions, or you can put a rewriter in front of the HTML and have the fix apply uniformly. WordPress's plugin ecosystem guarantees the audit approach loses ground every release cycle.

What this does not fix: web fonts (the swap window is a browser concern), cookie-banner shifts (the banner content doesn't exist until JS injects it), and ad-slot shifts (the slot dimensions depend on which creative the ad network returns).

## What WordPress can fix on its own

These changes stack with the server-layer fix.

- Switch font loading from `font-display: swap` to `font-display: optional` in your theme's `functions.php`. `optional` gives the web font 100 ms to load; if it misses, the fallback stays for the whole page lifetime. Counterintuitively this _reduces_ CLS because no swap happens after the initial paint.
- Self-host Google Fonts via the `Olympus` or `OMGF` plugin. The Google Fonts CDN occasionally returns different `font-face` declarations to different users; self-hosting eliminates that variance.
- For cookie banners, configure the plugin to use fixed positioning (`position: fixed; bottom: 0;`) rather than pushing the footer. Reserve viewport-bottom padding: `body { padding-bottom: 60px; }` while the banner is visible.
- For ad slots, set the container's `min-height` to the largest ad creative the network can return for that slot. AdSense documents recommended slot sizes; setting `min-height: 280px` on a 300×250 container blocks the shift entirely.
- Use the CSS `aspect-ratio` property on container divs that wrap images of unknown dimensions: `.wp-block-image > div { aspect-ratio: 16 / 9; }`. The browser reserves the box even when the image has not loaded.

## Measuring the lift

1. Re-run PSI on the same URL. The "Avoid large layout shifts" diagnostic should drop from "X elements caused shifts" to zero or near-zero.
2. Open DevTools → Performance → record a reload trace. The Layout Shift Regions overlay should be empty or limited to the cookie-banner area only.
3. After 28 days, check Search Console → Core Web Vitals report. The URL group should move from "Needs improvement" or "Poor" to "Good".
4. Watch the field CLS in PSI's CrUX panel, not just the lab Lighthouse number. Lab CLS sometimes reports 0 because the synthetic test doesn't trigger fonts or banners; field CLS is the truth.

## The minimal nginx config

```nginx
# nginx — minimal config to address CLS on WordPress
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters insert_image_dimensions;
pagespeed EnableFilters prioritize_critical_css;
pagespeed EnableFilters lazyload_images;
# Permit the rewriter to fetch image metadata from the origin:
pagespeed FetchHttps enable;
```

For Apache, the same filters under `mod_pagespeed`:

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed
ModPagespeedRewriteLevel CoreFilters
ModPagespeedEnableFilters insert_image_dimensions
ModPagespeedEnableFilters prioritize_critical_css
ModPagespeedEnableFilters lazyload_images
```

## When this doesn't work

Some CLS sources are outside mod_pagespeed's reach.

- If CLS attribution still names the same element after enabling `insert_image_dimensions`, the image is loaded by JavaScript (e.g., a slider that fetches images from a REST endpoint and injects them post-load). The rewriter only sees `<img>` tags present in the initial HTML. Fix: reserve container space in CSS via `aspect-ratio`, or pre-render the slider's first slide server-side.
- If CLS attribution names a cookie banner, ad unit, or third-party widget, no HTML rewriter can predict the height of content that doesn't exist yet. Reserve space in CSS for the expected container size. This is a markup decision, not a rewriter decision.
- If CLS attribution names a font-swap shift even after `prioritize_critical_css` is enabled, the fonts are still loading from a third-party CDN. Self-host fonts and switch to `font-display: optional`.
- If you see CLS in the lab Lighthouse run but not in CrUX, the lab is testing under throttled CPU that exposes a font-swap window real users do not see. Trust the field number.

## Related

- [WordPress full-page caching plugin](/wordpress/)
- [WordPress server-side page-cache plugin](/blog/wordpress-server-side-page-cache-plugin/)
- [How to fix LCP on WordPress](/blog/fix-lcp-wordpress-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix INP on WordPress](/blog/fix-inp-wordpress-2026/)
- [How to fix CLS on WooCommerce](/blog/fix-cls-woocommerce-2026/)
- [Critical CSS without a headless browser](/blog/critical-css-heuristics/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full CLS guide](/core-web-vitals/cls/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).
