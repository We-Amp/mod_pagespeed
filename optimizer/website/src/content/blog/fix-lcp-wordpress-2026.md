---
title: 'Improve LCP, FCP and TTFB on WordPress at the server (2026)'
description: 'How to fix LCP on WordPress: diagnose the hero, inline critical CSS at the server, convert JPEG to WebP, and verify in Search Console. Step-by-step, 2026.'
date: 2026-04-08
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'lcp', 'wordpress', 'critical-css']
draft: false
product: '1.1'
howTo:
  name: How to fix LCP on WordPress
  description: Diagnose the WordPress LCP element, then fix it server-side with mod_pagespeed by inlining critical CSS and serving optimized WebP images, plus WordPress-side preload tweaks, and verify the improvement.
  tools:
  - mod_pagespeed
  - nginx
  - Apache
  - PageSpeed Insights
  - Chrome DevTools
  - Lighthouse
  - Google Search Console
  - WordPress
  steps:
  - name: Identify the LCP element
    text: 'Run PageSpeed Insights on the affected URL and read the Largest Contentful Paint element box, then confirm the attribution in Chrome DevTools Performance panel under mobile throttling. The named element decides what you fix: an image needs preload and CSS work, a text block needs the CSS chain and fonts.'
  - name: Inline critical CSS at the server
    text: Enable mod_pagespeed's prioritize_critical_css filter (nginx or Apache module) to inline above-the-fold CSS into the head and defer the rest, eliminating the multi-stylesheet render-blocking chain that delays paint on a typical WordPress install.
  - name: Optimize the hero image server-side
    text: Enable recompress_images and convert_jpeg_to_webp to shrink the hero JPEG and transcode it to WebP for supporting browsers, served from cache at HIT speed; add inline_preview_images and hint_preload_subresources to surface a placeholder fast and preload the hero.
  - name: Stack WordPress-side preload adjustments
    text: In the theme, add an explicit preload link with fetchpriority="high" for the known hero URL, set loading="eager" on the LCP image and lazy on the rest, and dequeue unused plugin stylesheets (such as WooCommerce on non-shop pages) in functions.php.
  - name: Verify the LCP moved
    text: Re-run PSI and the DevTools trace to confirm a smaller element render delay and resource load duration, load the page with ?PageSpeedFilters=+debug to confirm the hero is in the rewritten set, then after 28 days check the Search Console Core Web Vitals report for the URL group moving to Good.
faq:
- q: What is a good LCP score for WordPress?
  a: Under 2.5 seconds is good, 2.5 to 4 seconds needs improvement, and over 4 seconds is poor. The number that counts is the field measurement from real Chrome users over the trailing 28 days, not the lab number in Lighthouse.
- q: Why is my WordPress LCP worse on mobile than on desktop?
  a: Field LCP reflects real visitors on slower hardware and connections than your editing machine, and on mobile the hero image competes with a longer render-blocking CSS chain. Reproduce it by testing with mobile throttling — Slow 4G plus 4x CPU slowdown.
- q: Can a plugin fix WordPress LCP on its own?
  a: A plugin fixes one site at a time and re-runs its work on every page load. mod_pagespeed rewrites HTML at the nginx or Apache layer and serves optimized image variants from a shared cache, so the fix applies to every URL at once — including pages published later.

---

WordPress LCP regressions almost always trace back to one of three things: an unsized featured image, a theme stylesheet blocking paint, or a page builder whose hero element is injected by JS. We'll diagnose which one, then fix it server-side with **mod_pagespeed** (an nginx or Apache module) so the change applies to every URL at once — no theme rewrite, no per-image audit.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What LCP measures

LCP — Largest Contentful Paint — is the render time of the largest above-the-fold element on a page. On most WordPress sites that is the post's featured image, a hero block from the page builder, or the H1. Anything under 2.5 s counts as "good"; anything over 4 s is "poor". The field measurement comes from Chrome's CrUX dataset — real users over the trailing 28 days — while the lab measurement comes from Lighthouse and PageSpeed Insights, and the two routinely disagree because real visitors run slower hardware on slower connections than your editing machine. See [web.dev/articles/lcp](https://web.dev/articles/lcp) for the canonical definition.

## LCP, FCP and TTFB: how they stack

TTFB, FCP and LCP are three points on one timeline. TTFB (time to first byte) is how long the server takes to start sending the HTML. FCP (First Contentful Paint) is when the browser paints the first text or image. LCP is when the largest above-the-fold element paints. Each one comes after the one before it, so a slow TTFB pushes FCP and LCP back with it.

On WordPress that gives a fix order. TTFB first: if the server takes over a second to start the response, cache the HTML with a [server-side page cache](/blog/wordpress-server-side-page-cache-plugin/) before anything else; [how to reduce TTFB](/blog/reduce-ttfb-server-layer-2026/) covers the rest of the server layer. FCP next: the render-blocking stylesheet chain described below holds back the first paint, and inlining critical CSS at the server moves FCP and LCP together. The rest of this guide covers that step and the hero image, which is usually what separates LCP from FCP.

## The most common LCP failures on WordPress

On WordPress, three things blow LCP:

1. **The hero image isn't preloaded.** Themes (Twenty Twenty-Five, Astra free, Kadence) render the LCP element through `the_post_thumbnail()`. WordPress 6.4+ ships `wp_get_loading_optimization_attributes()`, which is supposed to set `fetchpriority="high"` on the first large image. In practice it misfires on pages where the LCP is set by a page-builder hero block (Elementor, Bricks, GenerateBlocks). The image then waits behind the CSS chain and LCP slips past 4 s on mobile.

2. **Render-blocking plugins inject CSS and JS from `wp_head`.** Yoast SEO, WooCommerce (even on non-shop pages), Contact Form 7, and Elementor all enqueue stylesheets unconditionally. A typical un-tuned WordPress install ships 8–14 `<link rel="stylesheet">` tags in `<head>`, every one of them render-blocking. LCP cannot fire until the last one resolves.

3. **Editor-pasted images have no `width` or `height`.** Classic-editor content and many shortcode-generated galleries omit the dimension attributes. The browser can't reserve a layout box early, which delays LCP candidacy until the image decodes. This also costs CLS, but the LCP impact is direct: the image is not counted as a paint until it has a known size.

## Spot the responsible image

Before changing anything, get the baseline number and the attribution. The attribution — _which element_ is the LCP — decides what you fix.

- Run PageSpeed Insights on the affected URL: `https://pagespeed.web.dev/analysis?url=<URL>`. Read the **Largest Contentful Paint element** box in the Diagnostics section. It names the element selector and shows the timing breakdown (TTFB, resource load delay, resource load duration, element render delay).
- Open Chrome DevTools → **Performance** panel → record a trace on the same URL with mobile throttling (Slow 4G + 4× CPU slowdown). Stop the recording, find the **LCP** marker on the Timings track, click it, and read the **Related Node** in the summary panel. Confirm it matches the element you think the hero is.
- For repeatable measurement in CI or before/after comparison: `npx lighthouse <URL> --only-categories=performance --form-factor=mobile`.
- WordPress Site Health (Tools → Site Health → Status) flags some upstream causes — outdated plugins, missing PHP modules — but does not measure LCP. Pair it with PSI for the actual number.

Output you're hunting for: a baseline LCP value and a named element. If the named element is an `<img>`, the fix is image + preload + CSS. If it's a text block (H1), the fix is the CSS chain and font loading.

## Inline above-the-fold CSS with prioritize_critical_css

mod_pagespeed runs as an nginx module (or Apache module) in front of WordPress. It rewrites HTML on the way out and serves optimized image variants from a shared cache. The filters that move LCP on WordPress:

- `prioritize_critical_css` — inlines the above-the-fold CSS rules into the document `<head>` and defers the rest. This eliminates the multi-stylesheet `<head>` chain that failure mode #2 creates. The extractor is heuristic, not headless-browser-based, so it runs in single-digit milliseconds per page; see [Critical CSS without a headless browser](/blog/critical-css-heuristics/) for the algorithm.
- `recompress_images` + `convert_jpeg_to_webp` — shrinks the hero JPEG by 30–60% with zero theme changes, then transcodes to WebP for browsers that advertise support. The variant is served from the cache on subsequent requests at HIT speed. See [the economics of image optimization](/blog/economics-of-image-optimization/) for why WebP conversion pays for itself on image-heavy pages.
- `inline_preview_images` — for phone-camera-grade JPEGs that still cost time even after recompression, this filter ships a sub-1 KB low-quality image placeholder inline in the HTML. The user sees something in under 100 ms while the full image continues loading.

The case for fixing this at the server layer is operational. Chasing the plugin and theme ecosystem one update at a time is a rolling cost; a rewriter in front of the HTML applies the fix uniformly to every page — including the ones published tomorrow.

Minimal nginx config to address the WordPress failure modes above:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters prioritize_critical_css;
pagespeed EnableFilters recompress_images;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters inline_preview_images;
pagespeed EnableFilters hint_preload_subresources;
```

After enabling, re-run PSI. The LCP attribution should change — the element render delay drops as critical CSS inlines, and the resource load duration drops as the WebP variant is served. If attribution stays on the same image, see the "When this doesn't work" section.

## WordPress-side adjustments

What you can do inside WordPress to stack additional wins without mod_pagespeed:

- In `functions.php`, dequeue WooCommerce styles on non-shop pages: `if ( ! is_woocommerce() ) wp_dequeue_style( 'woocommerce-general' );`. Same pattern for Yoast SEO assets on non-content templates.
- Add an explicit `<link rel="preload" as="image" fetchpriority="high" href="<hero-url>">` in the theme's `header.php` for known hero URLs. Page builders usually expose a hook for the hero image URL.
- Set `loading="eager"` on the LCP image and `loading="lazy"` on everything else. WordPress's auto-detection gets this wrong on roughly 20% of templates — fix it in the theme.
- On Elementor: test with "Improved Asset Loading" toggled both ways. Counter-intuitively it sometimes bundles CSS in a way that defers above-the-fold rules; the right setting depends on your template.
- Use a blocks-based theme (Twenty Twenty-Five) where you can. Page-builder wrapper divs add layout passes that block paint compared to native block markup.

Apply these, then re-measure before moving on.

## Confirming the LCP moved

A four-point checklist confirms the fix landed:

1. Re-run PSI on the same URL. The LCP element timing breakdown should show smaller element render delay and resource load duration.
2. DevTools → Performance panel → record again under the same throttling profile. The previously attributed element either has a smaller LCP marker or a different element has become the LCP. Either is progress.
3. Open the page with `?PageSpeedFilters=+debug` — the response includes HTML comments showing which filters ran and which images were rewritten. Confirm the hero image is in the rewritten set.
4. After 28 days, check Search Console → Core Web Vitals report. The URL group should move from "Needs improvement" to "Good". Watch the field number, not just lab — lab improvements that don't show up in CrUX after 28 days are usually attribution mismatches, not real regressions.

## Configuration cheat sheet

```nginx
# nginx — minimal config to address LCP on WordPress
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters prioritize_critical_css;
pagespeed EnableFilters recompress_images;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters inline_preview_images;
pagespeed EnableFilters hint_preload_subresources;
# Tuning: recompression quality for photographic content
pagespeed ImageRecompressionQuality 82;
```

For Apache:

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed
ModPagespeedRewriteLevel CoreFilters
ModPagespeedEnableFilters prioritize_critical_css
ModPagespeedEnableFilters recompress_images
ModPagespeedEnableFilters convert_jpeg_to_webp
ModPagespeedEnableFilters inline_preview_images
ModPagespeedEnableFilters hint_preload_subresources
ModPagespeedImageRecompressionQuality 82
```

The full directive reference lives in the [configuration reference](/docs/configuration/).

## When this doesn't work

Cases where mod_pagespeed alone isn't enough on WordPress:

- **The hero is rendered by JavaScript after page load.** Some page builders (especially React-based heroes in Bricks or Breakdance) inject the LCP image via JS after the initial HTML response. mod_pagespeed rewrites the static HTML; it cannot rewrite content that doesn't exist until the browser executes JS. Fix: move the hero into the server-rendered template, or hard-code a `<link rel="preload">` for the hero URL.
- **TTFB is the long pole.** If PSI shows time-to-first-byte over 1 s, no amount of HTML rewriting will get LCP under 2.5 s — the browser is already 1 s in when the first byte arrives. Fix: a [server-side page cache](/blog/wordpress-server-side-page-cache-plugin/) (LiteSpeed, WP Super Cache, or Varnish in front of nginx), database query tuning, or a faster hosting plan.
- **The LCP is a third-party embed.** Lazy-loaded YouTube embeds and CMP-injected video posters often become the LCP element on landing pages. mod_pagespeed cannot rewrite a third-party iframe's contents. Fix: use a `<facade>` pattern (a static thumbnail that replaces itself with the iframe on click) so the LCP is your own image, not a third-party load.

## Common questions

**What is a good LCP score for WordPress?** Under 2.5 seconds is good, 2.5 to 4 seconds needs improvement, and over 4 seconds is poor. The number that counts is the field measurement from real Chrome users over the trailing 28 days, not the lab number in Lighthouse.

**Why is my WordPress LCP worse on mobile than on desktop?** Field LCP reflects real visitors on slower hardware and connections than your editing machine, and on mobile the hero image competes with a longer render-blocking CSS chain. Reproduce it by testing with mobile throttling — Slow 4G plus 4x CPU slowdown.

**Can a plugin fix WordPress LCP on its own?** A plugin fixes one site at a time and re-runs its work on every page load. mod_pagespeed rewrites HTML at the nginx or Apache layer and serves optimized image variants from a shared cache, so the fix applies to every URL at once — including pages published later.

## Related

- [WordPress full-page caching plugin](/wordpress/)
- [How to fix INP on WordPress](/blog/fix-inp-wordpress-2026/)
- [How to fix CLS on WordPress](/blog/fix-cls-wordpress-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [How to fix LCP on WooCommerce](/blog/fix-lcp-woocommerce-2026/)
- [Critical CSS without a headless browser](/blog/critical-css-heuristics/)
- [Server-side critical CSS on nginx](/blog/server-side-critical-css-nginx/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full LCP guide](/core-web-vitals/lcp/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module and optimizes by default. See [pricing](/pricing/) and [license terms](/license/).
