---
title: 'Fix LCP on nginx: transport, not content'
description: 'How to fix LCP on nginx: HTTP/2, gzip_static, TLS resumption and the mod_pagespeed rewriter. Stack-agnostic LCP fixes that work behind any backend.'
date: 2026-05-07
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'lcp', 'nginx']
draft: false
product: '1.1'
lastUpdated: 2026-10-22
howTo:
  name: How to fix LCP on nginx
  description: Diagnose the nginx transport layer, route image and critical-CSS work through the ngx_pagespeed (mod_pagespeed 2) module, tune HTTP/2 and pre-compressed assets, then confirm the LCP win.
  tools:
  - nginx
  - ngx_pagespeed (mod_pagespeed 2)
  - PageSpeed Insights
  - curl
  - Chrome DevTools
  - WebPageTest
  - Google Search Console
  steps:
  - name: Diagnose the transport layer and LCP element
    text: Run PageSpeed Insights on the affected URL and read the Largest Contentful Paint element breakdown (TTFB, resource load delay, resource load duration, element render delay). Use curl to check HTTP version, compression (br/gzip), and Cache-Control/s-maxage headers, and inspect the Chrome DevTools Network waterfall to confirm the LCP image is in the first request wave.
  - name: Rewrite images and inline critical CSS with ngx_pagespeed
    text: 'Load mod_pagespeed as the ngx_pagespeed nginx module and enable the LCP filters in the server block: recompress_images, convert_jpeg_to_webp, inline_preview_images, prioritize_critical_css, and hint_preload_subresources, plus ImageRecompressionQuality. This serves optimized WebP variants and inlines above-the-fold CSS regardless of backend.'
  - name: Tune the transport layer
    text: Enable HTTP/2 with listen 443 ssl http2;, turn on gzip_static and brotli_static with pre-compressed .gz/.br files, add TLS session resumption (ssl_session_cache and ssl_session_timeout), and set Cache-Control s-maxage with stale-while-revalidate on dynamic responses. Verify with nginx -T grep to confirm the loaded config matches what you edited.
  - name: Confirm the LCP win
    text: Re-run PSI and confirm the LCP element shows smaller resource load duration and delay. Use curl --http2 to confirm the hero serves as image/webp over HTTP/2, open the page with ?PageSpeedFilters=+debug to verify the hero is in the rewritten (non-lazy) set, then watch Search Console Core Web Vitals field data over the following weeks.

---

Three nginx-side LCP failures keep showing up, and none is a content problem: HTML on the slow path while cacheable images wait behind it, on-the-fly gzip burning CPU on every response, and HTTP/1.1 head-of-line blocking that queues the LCP image behind CSS. The fix is to get the transport layer out of the way (HTTP/2, pre-compressed assets, TLS resumption), then route image and CSS work through **mod_pagespeed 2** (the `ngx_pagespeed` module) so the same recipe applies regardless of what backend nginx is fronting.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What LCP measures

LCP — Largest Contentful Paint — is the render time of the largest above-the-fold element on a page. On most sites that is a hero image, video poster, or H1. The "good" line sits at 2.5 s; past 4 s is "poor". The field figure comes from Chrome's CrUX dataset (real users over the trailing 28 days), the lab figure from Lighthouse and PageSpeed Insights — and they often disagree because the network path real users take is slower than localhost. See [web.dev/articles/lcp](https://web.dev/articles/lcp) for the canonical definition.

## The most common LCP failures on nginx

These three keep showing up on nginx-fronted sites, independent of the backend:

1. **Origin is fast, but the static/dynamic split leaks.** A typical `try_files $uri @app;` pattern routes static files from disk and falls through to an upstream for dynamic content. The static side is fine — the dynamic side ships responses without `Cache-Control: s-maxage`, so the CDN treats HTML as uncacheable and re-fetches it. LCP suffers because the HTML is on the slow path and the LCP image (cacheable) waits behind it.

2. **`gzip on;` is set but `gzip_static off;` is the default.** nginx compresses on the fly on every request — 5–30 ms of CPU per response — instead of serving pre-compressed `.gz` files from disk. On a busy server, this serializes behind the worker pool and the LCP image's first byte is delayed.

3. **HTTP/1.1 head-of-line blocking on a single connection.** Sites still on HTTP/1.1 (no `listen 443 ssl http2;`) push all assets through one or two parallel connections; the LCP image queues behind CSS and JS that the browser opens first. Even with the rewriter optimizing bytes, the transport bottleneck eats the win.

## What the transport layer reveals

Before changing anything:

- Run PageSpeed Insights on the affected URL: `https://pagespeed.web.dev/analysis?url=<URL>`. Read the **Largest Contentful Paint element** breakdown — the four phases (TTFB, resource load delay, resource load duration, element render delay) tell you which layer to fix.
- `curl -sI -H 'Accept-Encoding: gzip, br' https://<host>/index.html` — check for `content-encoding: br` (Brotli) or `gzip` on the document, and `Cache-Control` headers. Missing compression on the document is failure mode #2; missing `s-maxage` is failure mode #1.
- `curl -sI --http2 -o /dev/null -w '%{http_version}\n' https://<host>/` — should print `2`. If it prints `1.1`, HTTP/2 isn't enabled (failure mode #3).
- Chrome DevTools → **Network** panel → reload, look at the **Waterfall** column. HTTP/1.1 shows a clear "6-connection limit" stagger; HTTP/2 shows requests landing in parallel. The LCP image should be in the first wave of requests, not waiting on CSS/JS.
- For repeatable measurement across config changes: WebPageTest scripted runs on a fixed connection profile. WebPageTest's "Connection View" diagnoses transport problems faster than Lighthouse.

Output you're hunting for: protocol, compression, cache headers, and the LCP element's timing breakdown. If TTFB is the long pole, the rewriter can't help; fix the backend first. If resource load duration is the long pole, the rewriter is the right tool.

## Rewrite images and inline critical CSS with ngx_pagespeed

mod_pagespeed loads as an nginx module (`ngx_pagespeed`). It rewrites HTML on the way out and serves optimized image variants from a shared cache. nginx is the canonical deployment target, so this is the most direct configuration. The filters that move LCP here:

- `recompress_images` + `convert_jpeg_to_webp` — re-encodes hero images at a configured quality and transcodes to WebP for browsers that advertise support.
- `inline_preview_images` — ships a sub-1 KB LQIP placeholder inline in the HTML for the hero, so the first paint shows something during the network wait.
- `prioritize_critical_css` — extracts above-the-fold rules and inlines them into the document `<head>`, defers the rest. Kills the multi-stylesheet `<head>` chain that backend-emitted HTML usually carries.
- `hint_preload_subresources` — emits `Link: rel=preload` response headers for resources the rewriter has identified as needed for above-the-fold rendering. The browser starts fetching them before the document body parses. See [server-injected resource hints](/blog/server-injected-resource-hints-speculation-rules/) for how the rewriter picks what to preload.
- `lazyload_images` — defers offscreen image loading; this is marked "Test first" in the [filters reference](/docs/filters/) and changes load order. Enable it for image-heavy pages, but keep the LCP image out of the lazy set — the rewriter's beacon-based detection identifies above-the-fold images automatically; verify with `?PageSpeedFilters=+debug`.

Minimal nginx config:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters recompress_images;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters inline_preview_images;
pagespeed EnableFilters prioritize_critical_css;
pagespeed EnableFilters hint_preload_subresources;
# Tune image recompression
pagespeed ImageRecompressionQuality 82;
```

Place these directives in the `server { }` block. The rewriter runs on responses returned through the location block, regardless of whether they come from a `proxy_pass`, `fastcgi_pass`, or `try_files` path.

After enabling, re-run PSI. LCP attribution should show the rewritten WebP variant being served and the resource load duration should drop. If LCP element render delay stays high, the long pole is the CSS chain or the transport — see the transport-layer tuning below.

## Transport-layer tuning

What you do at the nginx layer to stack additional wins:

- Enable HTTP/2: `listen 443 ssl http2;` in the server block. HTTP/3 if available — requires nginx ≥ 1.25 with QUIC compiled in.
- `gzip_static on;` plus ship pre-compressed `.gz` and `.br` files alongside the originals. The `ngx_brotli` module is widely packaged; enable both with `brotli_static on;`.
- `ssl_session_cache shared:SSL:10m;` and `ssl_session_timeout 1d;` — TLS resumption saves roughly 80 ms on repeat visits, especially on mobile networks where RTT dominates.
- For dynamic responses, set `add_header Cache-Control "public, s-maxage=60, stale-while-revalidate=30";` so the CDN can serve stale HTML while revalidating in the background.
- Audit the loaded config: `nginx -T | grep -E 'gzip|http2|brotli|ssl_session'`. The file you edit isn't necessarily the file that's loaded if there are `include` glob mismatches or a stale package config under `/etc/nginx/conf.d/`.

## Confirming the transport win

1. `curl -sI --http2 -H 'Accept: image/webp,*/*' https://<host>/<hero-url>` — confirm `content-type: image/webp` and `http/2 200`. Confirm `cache-control` is set as configured.
2. Open the page with `?PageSpeedFilters=+debug` — the response includes HTML comments showing which filters ran on which assets. Confirm the hero is in the rewritten set and is not lazy-loaded.
3. Re-run PSI on the same URL. The LCP element timing should show smaller resource load duration (WebP + compression) and smaller resource load delay (HTTP/2 + preload).
4. After 28 days, check Search Console → Core Web Vitals report. The URL group should move from "Needs improvement" to "Good". Watch field data, not just lab.

## The nginx snippet

```nginx
# nginx — minimal config to address LCP, stack-agnostic
server {
    listen 443 ssl http2;
    server_name example.com;

    ssl_session_cache shared:SSL:10m;
    ssl_session_timeout 1d;

    gzip on;
    gzip_static on;
    # ngx_brotli module:
    brotli on;
    brotli_static on;

    pagespeed on;
    pagespeed FileCachePath /var/cache/ngx_pagespeed;
    pagespeed RewriteLevel CoreFilters;
    pagespeed EnableFilters recompress_images;
    pagespeed EnableFilters convert_jpeg_to_webp;
    pagespeed EnableFilters inline_preview_images;
    pagespeed EnableFilters prioritize_critical_css;
    pagespeed EnableFilters hint_preload_subresources;
    pagespeed ImageRecompressionQuality 82;

    location / {
        try_files $uri @app;
        add_header Cache-Control "public, s-maxage=60, stale-while-revalidate=30";
    }
}
```

The full directive reference lives in the [configuration reference](/docs/configuration/). For the install steps on nginx (Debian, Ubuntu, RHEL/AlmaLinux), see [installation as an nginx module](/docs/installation-module/).

## When this doesn't work

Cases where mod_pagespeed alone isn't enough on nginx:

- **TTFB is the long pole.** If `curl -w '%{time_starttransfer}\n'` shows over 1 s, no amount of HTML rewriting fixes LCP — the browser is already 1 s in when the first byte arrives. Fix the backend (PHP-FPM tuning, Node process count, database query plan) before the rewriter — [reduce TTFB before it delays LCP](/blog/reduce-ttfb-server-layer-2026/) walks through the server-layer wins to try first.
- **The LCP element is a third-party embed.** Lazy-loaded YouTube iframes and CMP-injected video posters often become the LCP candidate. mod_pagespeed cannot rewrite third-party iframe contents. Use a facade pattern; the LCP becomes your own static image.
- **The HTML is built by JavaScript on the client.** Single-page apps (React, Vue, Svelte hydrating from a thin shell) construct the LCP element after page load. The nginx-layer rewriter doesn't see the runtime DOM. SPA LCP work moves to build time (Next.js `<Image>`, route-level code-splitting) or to a server-side rendering / pre-rendering layer.
- **The LCP is text and the font is the long pole.** Web fonts loaded with `font-display: swap` cause a render delay on the H1 LCP. The fix is `font-display: optional`, self-hosting the font, and preloading with `<link rel="preload" as="font" crossorigin>`. The rewriter doesn't manage your font loading strategy.

## Related

- [How to fix INP on nginx](/blog/fix-inp-nginx-2026/)
- [How to fix CLS on nginx](/blog/fix-cls-nginx-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [How to fix LCP on WordPress](/blog/fix-lcp-wordpress-2026/)
- [Server-side critical CSS on nginx](/blog/server-side-critical-css-nginx/)
- [Image optimization as an nginx module](/blog/nginx-image-optimization-module/)
- [The economics of image optimization](/blog/economics-of-image-optimization/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full LCP guide](/core-web-vitals/lcp/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).
