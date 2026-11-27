---
title: 'Fix CLS on nginx with mod_pagespeed: insert_image_dimensions'
description: 'Fix CLS on nginx sites: add width and height to img tags with ngx_pagespeed (mod_pagespeed 2), add a CSS aspect-ratio backstop, set sizes at build.'
date: 2026-05-12
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'cls', 'nginx']
draft: false
product: '1.1'
howTo:
  name: How to fix CLS on nginx
  description: Eliminate Cumulative Layout Shift on nginx-served sites by rewriting img tags to add width/height with mod_pagespeed and reserving space for fonts and embeds with CSS.
  tools:
  - nginx
  - mod_pagespeed 2 (ngx_pagespeed)
  - PageSpeed Insights
  - Chrome DevTools
  steps:
  - name: Identify the shifting elements
    text: Run PageSpeed Insights on the affected URL and read the "Avoid large layout shifts" diagnostic, then confirm in Chrome DevTools Performance panel with "Layout Shift Regions" enabled and Slow 4G throttling. For static sites, grep HTML for img tags missing width/height attributes.
  - name: Insert image dimensions server-side
    text: Enable insert_image_dimensions in ngx_pagespeed so the module inspects each img tag, reads the actual pixel dimensions, and adds matching width and height attributes uniformly across all upstreams. Test on responsive templates first, since writing fixed dimensions can interfere with some CSS-driven layouts.
  - name: Add supporting rewriter filters
    text: Enable lazyload_images so offscreen placeholders are sized from the inserted dimensions, prioritize_critical_css to shrink the FOUC window, and extend_cache to content-hash assets so stale HTML never references images at mismatched dimensions.
  - name: Add a CSS aspect-ratio backstop
    text: 'Add the global rule img { max-width: 100%; height: auto; } so modern browsers compute the aspect-ratio box from the inserted width/height attributes and reserve the layout slot.'
  - name: Fix fonts, iframes, and JS-injected content manually
    text: 'These shifts are outside the rewriter''s scope: self-host fonts with font-display: optional, wrap third-party iframes in a fixed aspect-ratio container, and reserve space in CSS for any content injected by JavaScript after the initial response.'
  - name: Confirm the shift is gone
    text: Re-run PSI to verify zero shifted content elements, re-trace in DevTools with Slow 4G to confirm an empty Layout Shift Regions overlay, then track the CrUX field CLS in PSI and the Search Console Core Web Vitals report over the following weeks.
faq:
  - q: Why does my CLS differ between PageSpeed Insights and Lighthouse?
    a: PSI reports the field 75th-percentile CLS from CrUX; Lighthouse reports a single lab run. On static-HTML sites the lab serves HTML faster than real users do, so font swaps and lazy-image shifts don't fire in the lab but do in the field. Trust the field number.
  - q: Does insert_image_dimensions work with images on another domain?
    a: Only if the rewriter can fetch them. Same-origin paths and same-domain absolute URLs work out of the box; for a different origin you must whitelist it with pagespeed Domain, and the origin must return an image MIME type. If neither holds, no attributes are inserted.
  - q: Will setting width and height break my responsive images?
    a: 'Add the CSS rule img { max-width: 100%; height: auto; } and modern browsers compute the aspect-ratio box from the attributes without pinning the rendered size. Test on responsive templates first, since writing fixed dimensions can interfere with some CSS-driven layouts.'

---

nginx itself is fast; CLS is what nginx _serves_, not what nginx _does_. A 0.2+ CLS on mobile with the failing element a content image missing `width`/`height` is the classic shape, and auditing every static HTML file (or every template across every generator) is a recurring tax. Rewrite `<img>` tags at the HTML layer with `insert_image_dimensions` via **mod_pagespeed 2** (`ngx_pagespeed`) and stack CSS `aspect-ratio` as a backstop.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What CLS measures

Cumulative Layout Shift measures how much visible content jumps around unexpectedly between load start and the moment the page becomes interactive. Every shift is scored by the viewport fraction that moved times the distance travelled, and the heaviest session window sets the value. The [web.dev/cls](https://web.dev/articles/cls) thresholds: under 0.1 is "good", 0.25 is where "poor" begins.

Shifts within 500 ms of, _and attributable to_, a user input don't count — a shift that happens to land in the window but wasn't caused by the click still scores. PSI shows the field 75th-percentile CLS from CrUX; Lighthouse shows a single lab run. The two often disagree for static-HTML sites: the lab serves the HTML faster than real users do, so font swaps and lazy-image jumps don't fire in lab but do in field. Trust the field number.

## The most common CLS failures on nginx-served sites

This is the CMS-agnostic post in the series. nginx fronts WordPress, Magento, ASP.NET Core, static-site-generator output, and hand-written HTML; CLS patterns vary, but three causes dominate when nginx is serving HTML directly (no upstream app).

**Static HTML files have `<img>` tags without dimensions.** Hand-built sites, Jekyll/Hugo/Eleventy output where the templates don't pipe images through a dimension-aware shortcode, and Markdown-converted content (Pandoc, common Node converters) all routinely produce `<img src="...">` with no `width` or `height`. nginx serves the file as-is. The browser allocates zero space, paints text, jumps the layout when bytes arrive. The shift is exactly the rendered image height.

**Stale HTML cached by nginx (or a CDN in front of it) references images at new dimensions.** Common pattern on a content site: image gets re-edited, replaced at the same URL or under a new hash, but the HTML in cache still embeds the old dimensions (or no dimensions). nginx's `proxy_cache` or a Cloudflare/Fastly edge serves the stale HTML; the loaded image has different dimensions; layout shifts. This compounds when the cache TTL is long and the image refresh path is faster than the HTML refresh path.

**Web fonts and third-party iframes shift content after initial paint.** Self-hosted fonts loaded with `font-display: swap` re-flow every text block when the font swaps. Embedded iframes (YouTube, Twitter/X, Instagram) start at zero height in the parent's HTML, expand when their content loads, push everything below. nginx is not responsible for any of this — it's serving the HTML the author wrote — but it's the layer where the rewrite fix lands.

## Where to look in the page first

Run PSI on the affected URL: `https://pagespeed.web.dev/analysis?url=https://example.com/`. The "Avoid large layout shifts" diagnostic lists the specific elements and their score contribution.

In Chrome DevTools: Performance panel → Settings cog → enable "Web Vitals" and "Layout Shift Regions". Throttle network to "Slow 4G". Reload. Each shift is a red rectangle in the overlay; hover the shift markers in the Timings track to identify the DOM node.

Static-site-specific diagnostics:

- `grep -L 'width=' *.html | head -20` finds HTML files that lack any dimension attribute. Useful to identify how widespread the problem is across a generator's output.
- `curl -s https://example.com/article/ | grep -oE '<img [^>]+>' | grep -v 'width='` prints `<img>` tags missing dimensions on a specific URL.

For repeatable measurement, install `web-vitals` and load it from a dev-only `<script>` tag: `onCLS(console.log, { reportAllChanges: true })`. The console prints the worst element per session.

## Set image dimensions server-side with insert_image_dimensions

CLS is the metric mod_pagespeed is most effective on, and on nginx it is the main configuration to get right. Three filters do most of the work.

`insert_image_dimensions` inspects every `<img>` tag in the outgoing HTML, fetches the underlying file (cached after first hit; works with same-origin paths and same-domain absolute URLs), reads actual pixel dimensions, and inserts matching `width` and `height` attributes. Combined with a single CSS rule on the site (`img { max-width: 100%; height: auto; }`), the browser computes the correct aspect-ratio box from the attributes and reserves the layout slot. The image-jump shift disappears for every page on the site — including content that was hand-authored, Markdown-converted, or pasted from a CMS that doesn't track dimensions.

Test `insert_image_dimensions` on your responsive templates before enabling it across the board. Writing fixed `width`/`height` onto markup that was relying on CSS for sizing can interfere with some responsive layouts — the `img { max-width: 100%; height: auto; }` rule covers the standard case, but hand-built and generator-specific templates vary.

`lazyload_images` reserves dimensioned placeholders for offscreen images while deferring the load. Because the placeholder is sized via the freshly-inserted `width`/`height` attributes, no shift fires when the real image swaps in.

`prioritize_critical_css` inlines above-the-fold CSS and defers the rest, shrinking the FOUC window during which font swaps and layout-affecting CSS arrive. On sites with large external stylesheets (Bootstrap, Tailwind, custom framework CSS), this is the second-largest CLS contributor after un-sized images.

`extend_cache` content-hashes asset URLs (images, CSS, JS) and serves them with long-lived cache headers. For failure mode #2 (stale HTML referencing newer images), `extend_cache` prevents the mismatch in the first place — the HTML references a hashed URL that resolves to specific image bytes, so a re-edit produces a new hash and the HTML must be updated to reference it.

Why a rewriter beats per-template audits: nginx fronts every kind of upstream, and the rewriter sits between the upstream and the client. The fix applies uniformly regardless of whether the HTML was generated by Hugo, hand-typed, or pulled from a legacy CMS. The rewriter is a one-time configuration; per-template audits are a recurring tax.

What this does not fix: third-party iframes (the rewriter doesn't know how tall a YouTube embed will render), web-font swap shifts (browser concern), and any content injected by JavaScript after the initial response. These need CSS-level reserved space or different markup, covered next.

## Tuning beyond ngx_pagespeed

- Add a global CSS backstop on `<img>`: `img { max-width: 100%; height: auto; }`. With `width`/`height` attributes now present via the rewriter, every modern browser computes the aspect-ratio box from those attributes directly — no `aspect-ratio: attr(...)` rule needed (CSS `attr()` inside `aspect-ratio` is not reliably supported as of 2026).
- Self-host fonts via `font-display: optional` instead of `swap`. `optional` gives the web font 100 ms to arrive; if it misses, the fallback stays for the whole page lifetime. Counterintuitively this _reduces_ CLS because no swap fires after initial paint.
- For embedded iframes (YouTube, Twitter, Instagram), wrap them in a fixed-aspect-ratio container:

```html
<div style="aspect-ratio: 16 / 9; max-width: 100%;">
  <iframe src="https://www.youtube.com/embed/..." style="width: 100%; height: 100%;"></iframe>
</div>
```

- Ship dimensions at build time where the generator supports it: Astro's `<Image>` component, Hugo's `image_processing`, Eleventy's `eleventy-img` all set `width`/`height` automatically. The rewriter then becomes a no-op (it only fills missing attributes); fixes for upstream regressions still land via the rewriter.
- For content-hashed asset URLs, enable `extend_cache` and set a long `Cache-Control` header in nginx for hashed paths: `location ~ "\.[a-f0-9]{8,}\." { expires 1y; add_header Cache-Control "public, immutable"; }`. The HTML is short-cached, the assets are long-cached; the mismatch goes away.
- Set `gzip_static on` so pre-compressed `.html.gz` files are served when present; smaller HTML means shorter parse window and tighter CLS budget.

## Confirming the shift is gone

1. Re-run PSI. "Avoid large layout shifts" should drop to zero shifted elements or list only third-party embeds you haven't wrapped yet.
2. DevTools → Performance → reload trace with Slow 4G throttle. The Layout Shift Regions overlay should be empty across content.
3. After 28 days, Search Console → Core Web Vitals report. The URL group should move from "Needs improvement" to "Good".
4. Watch the CrUX field CLS in PSI's panel. Static-site lab CLS is often 0 because the lab loads files instantly; field CLS reveals the font-swap and lazy-image shifts real users hit.

## A drop-in nginx snippet

```nginx
# nginx — minimal config to address CLS on generic / static / hand-built sites
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters insert_image_dimensions;
pagespeed EnableFilters lazyload_images;
pagespeed EnableFilters prioritize_critical_css;
pagespeed EnableFilters extend_cache;
pagespeed FetchHttps enable;
```

Then a single CSS rule in your site's stylesheet to let modern browsers compute aspect-ratio boxes from the attributes:

```css
img {
  max-width: 100%;
  height: auto;
}
```

## When this doesn't work

- If `insert_image_dimensions` doesn't insert attributes, the rewriter cannot fetch the image — usually because the image is on a different origin and `pagespeed Domain` doesn't whitelist it, or because the origin returns a `Content-Type` other than an image MIME. Check the rewriter's debug log (`pagespeed Statistics on`) and confirm the fetch is happening.
- If CLS attribution still names content images after the fix, the images may be inserted by JavaScript after the initial response (a lightbox gallery, a content-loader script). The rewriter only sees server-side HTML. Fix in CSS via `aspect-ratio` on the container, or pre-render the first slide server-side.
- If CLS attribution names a third-party iframe, no rewriter knows the embed's intrinsic height. Wrap it in a fixed-aspect container.
- If you see CLS in the lab but not in CrUX, the lab is exposing a race condition real users do not hit (CPU throttling triggers font-swap timing that doesn't fire in production). Trust the field number; do not over-tune to lab.

## FAQ

**Why does my CLS differ between PageSpeed Insights and Lighthouse?**
PSI reports the field 75th-percentile CLS from CrUX; Lighthouse reports a single lab run. On static-HTML sites the lab serves HTML faster than real users do, so font swaps and lazy-image shifts don't fire in the lab but do in the field. Trust the field number.

**Does insert_image_dimensions work with images on another domain?**
Only if the rewriter can fetch them. Same-origin paths and same-domain absolute URLs work out of the box; for a different origin you must whitelist it with `pagespeed Domain`, and the origin must return an image MIME type. If neither holds, no attributes are inserted.

**Will setting width and height break my responsive images?**
Add the CSS rule `img { max-width: 100%; height: auto; }` and modern browsers compute the aspect-ratio box from the attributes without pinning the rendered size. Test on responsive templates first, since writing fixed dimensions can interfere with some CSS-driven layouts.

## Related

- [How to fix LCP on nginx](/blog/fix-lcp-nginx-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix INP on nginx](/blog/fix-inp-nginx-2026/)
- [How to fix CLS on WordPress](/blog/fix-cls-wordpress-2026/)
- [Server-side critical CSS on nginx](/blog/server-side-critical-css-nginx/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full CLS guide](/core-web-vitals/cls/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).
