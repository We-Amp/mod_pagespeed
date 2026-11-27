---
title: 'Server-side critical CSS for nginx, any backend'
description: 'Extract and inline critical CSS at the nginx layer for any backend: PHP, Rails, Django, ASP.NET, Go. No WordPress plugin, headless browser or Node build.'
date: 2026-05-20
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['critical-css', 'nginx', 'core-web-vitals', 'lcp']
draft: false
---

The two most-recommended ways to ship critical CSS in 2026 are
[WP Rocket](https://wp-rocket.me/) (WordPress plugin) and
[criticalcss.com](https://criticalcss.com/) (SaaS that runs headless
Chrome over your URLs). Both work. Both also require something specific
about your stack: WordPress, a Node build pipeline, or both.

If your stack is anything else (a Rails app, a Django site, an ASP.NET
Core app behind nginx as a reverse proxy, a Go binary, a static-site
generator with no Node), you've been doing critical CSS by hand or
not at all.

The mod_pagespeed module does critical CSS at the nginx layer for any backend. The
nginx interceptor rewrites the HTML on the way out; the worker extracts
critical CSS via a heuristic that runs in under 5 ms per page without a
headless browser. Both pieces ship as a single package install.

## What "critical CSS" actually means

Above-the-fold CSS inlined in the document `<head>` so the browser can
paint without waiting for an external stylesheet. The rest of the CSS
loads async after first paint.

The performance win is real. A 50 KB external stylesheet linked from
`<head>` blocks [LCP](/core-web-vitals/lcp/) until it downloads, parses, and applies. Inlining
the 5–10 KB of rules needed for above-the-fold and deferring the rest
typically cuts LCP by 200 to 500 ms on a 3G connection. On a cold mobile
load with a hostile RTT, that's the difference between a Core Web Vitals
pass and a fail.

The cost is implementation. Hand-maintaining a critical-CSS partial
synchronized with your main stylesheet is brittle. Build-time tools
(`critical` npm, `penthouse`, `criticalcss.com`) all involve running a
headless browser against every page on every deploy, which adds minutes
to your CI and seconds of latency per generation.

## What the alternatives assume

| Tool                           | Where it runs             | Stack assumption       |
| ------------------------------ | ------------------------- | ---------------------- |
| WP Rocket                      | WordPress PHP plugin      | WordPress, MySQL, PHP  |
| criticalcss.com                | SaaS, headless Chrome     | You proxy URLs to them |
| `critical` (npm)               | Node CLI, headless Chrome | Node build pipeline    |
| `penthouse` (npm)              | Node CLI, headless Chrome | Node build pipeline    |
| Astro `<ViewTransitions>` etc. | Build-time tooling        | Astro/Vite             |

The common pattern: your stack already has Node, or it's WordPress. If
neither is true, the tool isn't a fit. That excludes most non-JS
backends shipping HTML through nginx.

## What the module does instead

Two pieces of software working together at the nginx layer:

1. **The nginx interceptor** intercepts every HTML response, hashes the
   request's capability mask (`Accept`, `Accept-Encoding`, `Save-Data`,
   viewport class), and checks the shared Cyclone cache for a
   pre-rewritten variant. If one exists, it serves the bytes via mmap.
   If not, it passes the original through and notifies the worker.
2. **The worker** reads the original HTML from the cache, scans
   it statically (no browser), extracts critical CSS using a heuristic
   that runs in single-digit milliseconds, injects the result into
   `<style data-pagespeed-critical>` near the top of `<head>`, and
   writes the rewritten HTML back to the cache at the requested
   capability mask. The next request hits the cache.

This is identical in architecture to how the same interceptor handles
WebP/AVIF transcoding, image lazy-loading, and HTML minification: they
are all worker-side passes. Critical CSS is one pipeline step among
several, not a separate tool.

The heuristic is documented in detail at [Critical CSS Without a Headless
Browser](/blog/critical-css-heuristics/). Short version: scan the first
300 elements inside `<body>` plus any fixed-position element, match selectors semantically (`header`, `nav`, `hero`,
`banner` are always critical; `footer`, `lazy`, `defer`, `below-fold` are
always excluded), exclude elements deeper than 10 levels, exclude
`@media print`. Tuned for over-inclusion (a small amount of extra CSS is
harmless; under-inclusion causes visible layout shift).

Pages that share a template share their extraction. The worker keys the
result by template hash, so one scan covers every page built from the
same layout — see [reusing critical CSS across pages via template
hashing](/blog/template-hashing-critical-css-reuse/).

## Enabling it in nginx

The filter directive is `prioritize_critical_css`. It's not in
`CoreFilters` by default because it changes the rendered HTML and needs
a deploy-time smoke test.

For **the module** (the lineage continuation, drop-in for the
archived Google module with the same syntax):

```nginx
# The package loads ngx_pagespeed_module.so itself; no load_module line needed.
http {
    pagespeed on;
    pagespeed FileCachePath /var/cache/ngx_pagespeed;

    pagespeed RewriteLevel CoreFilters;
    pagespeed EnableFilters prioritize_critical_css;

    # Optional but useful with critical CSS:
    pagespeed EnableFilters defer_javascript;
    pagespeed EnableFilters inline_preview_images;
}
```

In the module, `prioritize_critical_css` is the classic filter: it derives the
above-the-fold selector set from a lightweight client-side beacon on the
first visits and caches the result, rather than the worker's static scan
described above. Same directive, same inlined result, still no headless
browser — a different mechanism for a different era. The single-digit-
millisecond static heuristic is specific to the optimizer worker.

For **the optimizer worker** (see [Run with Docker
Compose](/blog/run-with-docker-compose/) for the container path):

```nginx
load_module /usr/lib/nginx/modules/ngx_pagespeed_module.so;

http {
    server {
        listen 80;

        pagespeed on;
        pagespeed_cache_path /var/lib/pagespeed/cache.vol;

        # Critical-CSS injection is an always-on worker transform.
        # The worker has no RewriteLevel or named filters; tune it
        # via the worker config or the web console.

        location / {
            proxy_pass http://127.0.0.1:8081;
            proxy_set_header Accept-Encoding "";  # serve uncompressed to the worker
        }
    }
}
```

The `proxy_set_header Accept-Encoding "";` line matters. If your origin
gzips eagerly, the worker sees compressed bytes and the HTML pipeline
silently skips. Strip Accept-Encoding on the upstream and let the nginx
interceptor re-compress on the way out.

## Verify

The fastest check is the rewritten HTML. Pull a page through nginx and
search for the marker:

```bash
$ curl -s http://localhost/ | grep -o 'data-pagespeed-critical[^>]*' | head -3
data-pagespeed-critical=""
```

A response containing `<style data-pagespeed-critical>` means the
critical-CSS pass ran. Without the filter, that marker is absent.

For a deeper look at what was extracted:

```bash
$ curl -s http://localhost/ | python3 -c '
import sys, re
html = sys.stdin.read()
m = re.search(r"<style data-pagespeed-critical[^>]*>(.*?)</style>", html, re.S)
if m:
    css = m.group(1)
    print(f"critical CSS: {len(css)} bytes, {css.count(chr(123))} rules")
'
critical CSS: 4892 bytes, 73 rules
```

73 rules at 4.9 KB is typical. A site with no critical-CSS extraction
ships an external 50 to 200 KB stylesheet on every cold load. That's the
delta you're recovering.

On the optimizer worker, cache hits show up in the response header (the module path shows its version header, `X-Page-Speed`, instead):

```bash
$ curl -sI http://localhost/ | grep -iE 'page-?speed'
x-pagespeed: HIT
```

A `HIT` means the rewritten HTML came from the Cyclone cache (no
worker round-trip on this request). A `MISS` means the original went
out and the rewrite ran asynchronously; the next request gets the
inlined version.

## Benchmark: 150–400 ms on a representative landing page

The headline number critical-CSS tools quote is "1–3 seconds off LCP".
That's the worst-case on slow 3G against a render-blocking 200 KB
stylesheet. On a typical broadband connection with a 30 KB stylesheet,
the saving is smaller (100 to 300 ms), and it's hard to attribute
cleanly because critical CSS is one of several optimizations running
together.

What you can measure cleanly: the time-to-first-paint delta between
`pagespeed off` and `pagespeed on` with the filter enabled. Run a
WebPageTest filmstrip against both and compare the frame where the
header is first visible. On a representative landing page with a
medium-weight stylesheet, expect 150 to 400 ms.

The lab-vs-CrUX caveat applies: Lighthouse will under-report the win,
because it always loads the full stylesheet eventually and counts the
total bytes. Real user data (CrUX, RUM) will show the LCP saving on
the 75th percentile. Use both, trust the field data for ranking.

## Caveats and where it goes wrong

Limits of the server-layer fix.

### Dark mode flash of unstyled content

If your site supports `@media (prefers-color-scheme: dark)`, the
heuristic may pick up the light-mode rules for above-the-fold but miss
the dark-mode override. Users on a dark-mode device see a flash of light
content before the deferred stylesheet loads with their dark
preferences.

**Fix:** inline a manual dark-mode override at the top of `<body>`,
above the deferred stylesheet:

```html
<style>
  @media (prefers-color-scheme: dark) {
    :root {
      color-scheme: dark;
    }
    body {
      background: #0a0a0a;
      color: #e5e5e5;
    }
  }
</style>
```

This costs ~150 bytes and eliminates the flash. Add it manually to your
base template; the worker doesn't generate it for you (yet).

### JavaScript-injected DOM content

If client-side JavaScript injects above-the-fold elements after page
load (a cookie banner, a popup, a hero carousel rendered by React after
hydration), the worker's static scan misses them. Their styles are
treated as below-the-fold and deferred.

For server-rendered content this is a non-issue. For SPAs with hydration,
critical CSS is often the wrong tool: your bottleneck is JS bundle size,
not stylesheet bytes. Profile first.

### Selectors with overrides

A selector like `.hero` matching an above-the-fold element gets
included. A subsequent `body.dark-mode .hero` rule deeper in the same
stylesheet gets evaluated independently: it's matched against `body`
plus `.hero`, neither of which has the `dark-mode` class at scan time,
so the rule is excluded. On a `body.dark-mode` page this means the
override is missing from critical CSS.

The over-inclusion bias mostly handles this; the worker will include
many adjacent rules even on partial matches. For tightly-themed sites
with class-based theming, verify with a manual scan.

### When you have ~80 KB+ of critical CSS

The heuristic retains 15 to 35% of the input stylesheet as critical. On
a huge framework stylesheet (Tailwind generates 10 to 20 MB unminified),
that fraction can still be too large to inline. The worker caps inlined
critical CSS at 100 KB to avoid blowing up HTML size; over that, it
falls back to a preload hint instead of full inlining.

If you're hitting the cap, Tailwind PurgeCSS / `tailwindcss --content`
content scanning is upstream of this. Clip your CSS to the actual
classes used, then let the worker extract critical from the smaller
input.

### First request after deploy

The HTML processing is async. The first request to a new URL after
deploy serves the original; the second request gets the rewritten
version. For pre-launch warm-up, hit your top URLs once during deploy:

```bash
curl -s -o /dev/null http://localhost/  # warm /
curl -s -o /dev/null http://localhost/products  # warm /products
curl -s -o /dev/null http://localhost/contact
```

A `wget --spider --recursive --no-parent` run against your sitemap
covers a whole site.

## Comparison

| Property                 | WP Rocket         | criticalcss.com    | mod_pagespeed nginx             |
| ------------------------ | ----------------- | ------------------ | ------------------------------ |
| Where it runs            | WordPress plugin  | SaaS               | nginx interceptor + worker     |
| Stack required           | WordPress         | Your URLs proxied  | Any backend behind nginx       |
| Extraction method        | Headless browser  | Headless browser   | Static heuristic, no browser   |
| Latency per page         | Build-time, batch | Build-time, batch  | Async at first request         |
| Update on content change | Manual rebuild    | Manual rebuild     | Automatic (cache invalidates)  |
| Operating cost           | Plugin license    | Monthly SaaS       | Self-hosted, Apache-2.0        |
| Data leaves your server  | No                | Yes (URLs to SaaS) | No                             |

WP Rocket is the right choice if your stack is WordPress and you want a
plugin UI. criticalcss.com is the right choice if you want a service to
manage extraction across a small site without infra. mod_pagespeed is the
right choice if you have nginx in front of anything and you want a
single optimization layer that handles critical CSS plus image variants
plus minification plus the rest, server-side, for any backend.

See [mod_pagespeed vs WP Rocket](/alternatives/wp-rocket/) for the full
side-by-side comparison.

## Installing it

The native nginx module pairs with the worker for CSS, JavaScript and image
variants; the worker's critical CSS needs the reverse-proxy deployment. The
packaged install for the native nginx module is the fastest. Install the
worker first, as its own command, then the module:

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install pagespeed-optimizer
sudo apt-get install nginx-module-pagespeed
```

Keep the two installs separate commands, worker first: the module package adds
the nginx user to the worker's `pagespeed` group only if that group already
exists. The [nginx install steps](/docs/installation-module/#native-nginx-module)
have the configuration that points the module at the worker.

For the ASP.NET Core middleware:

```bash
dotnet add package WeAmp.PageSpeed.AspNetCore
```

It optimizes out of the box, and it is licensed under Apache-2.0 and free
to run in development and in production — see the [license](/license/).

## Related

- [Critical CSS Without a Headless Browser](/blog/critical-css-heuristics/) — the heuristic in detail
- [Reusing critical CSS across pages via template hashing](/blog/template-hashing-critical-css-reuse/) — one extraction, every page on the same layout
- [From beacon to headless to heuristic](/blog/critical-css-beacon-to-headless-history/) — why the static scan replaced the old browser-round-trip approach
- [How CSS parsing works](/how-it-works/css-parsing/) — the syntax-tree layer beneath critical-CSS extraction
- [Run ModPageSpeed 2.0 with Docker Compose](/blog/run-with-docker-compose/) — full nginx + worker container setup
- [mod_pagespeed vs WP Rocket](/alternatives/wp-rocket/) — side-by-side comparison
- [ASP.NET Core image optimization](/blog/aspnet-core-image-optimization-c-sharp/) — same pipeline applied to image content negotiation
- [ngx_pagespeed alternative](/alternatives/ngx-pagespeed/): the maintained nginx module, for the nginx audience specifically
- [mod_pagespeed alternatives](/alternatives/mod-pagespeed/) — background on why mod_pagespeed needs a successor in 2026
