---
title: 'Fix INP on nginx: a CMS-agnostic plan'
description: 'How to fix INP on nginx, CMS-agnostic. Server-layer JS minification helps if your JS is bloated; if your stack is already lean, the wins are upstream.'
date: 2026-05-14
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'inp', 'nginx', 'ngx_pagespeed']
draft: false
product: '1.1'
lastUpdated: 2026-10-22
howTo:
  name: How to fix INP on nginx
  description: Diagnose the slowest interaction, lighten served JS with ngx_pagespeed, tune nginx's upstream and transport layer, then confirm the INP win with field and lab data.
  tools:
  - nginx
  - mod_pagespeed 2 (ngx_pagespeed)
  - Chrome DevTools
  - web-vitals (attribution mode)
  - PageSpeed Insights
  - Google Search Console
  steps:
  - name: Find the responsible interaction
    text: Record the page in Chrome DevTools Performance, run every interactive flow, and read the Interactions track to find the worst INP input. Cross-check XHR TTFB in the Network panel, use web-vitals attribution mode to name the handler, and run awk on the nginx access log for >0.5s requests plus nginx -T to audit the loaded config. The goal is a specific interaction, the responsible handler or endpoint, and a baseline number to beat.
  - name: Defer, combine, and minify JS via ngx_pagespeed
    text: Enable defer_javascript (test first on staging, it changes execution order), combine_javascript, and rewrite_javascript so the module pushes script execution past first render and shrinks bloated bundles. This helps when the origin serves heavy non-minified legacy JS; if the stack already ships a lean code-split SPA bundle, expect little movement and profile before enabling.
  - name: Tune the upstream and transport layer
    text: Enable HTTP/2 (listen 443 ssl http2), set keepalive on every upstream block with proxy_http_version 1.1 and an empty Connection header, raise worker_connections to 4096, add short proxy_read_timeout on /api/* locations, serve pre-compressed gzip/brotli, and enable TLS session resumption. These connection-layer changes remove handshake and queue overhead, which is where most INP wins on a generic nginx stack actually live.
  - name: Fix slow upstream handlers server-side
    text: 'Profile the upstream, not just nginx: awk the access log for >0.5s endpoints and fix them at the source with caching, database indexing, or async I/O. No nginx-layer rewriter removes handler latency, so slow synchronous AJAX endpoints must be fixed in the application itself.'
  - name: Confirm the INP win
    text: Re-run the access-log scan to confirm a shorter slow-endpoint list, replay the baseline interaction in DevTools Performance to verify it is under 200 ms with low XHR TTFB, re-run PageSpeed Insights on representative URLs, then wait 28 days and check the Search Console Core Web Vitals report for the field INP improvement.

---

INP is a client-side metric — it measures the latency between a user input and the next paint — so nginx itself rarely _is_ the bottleneck. What nginx controls is _what JS gets served_ and _how fast the AJAX endpoints behind the interactive widgets respond_. **mod_pagespeed 2** (`ngx_pagespeed`) runs at the nginx layer to minify, combine, and defer that JS. If your origin already serves lean JS, mod_pagespeed adds little; if it serves a typical bundled-WordPress-or-similar mess, mod_pagespeed helps a lot. This is the CMS-agnostic guide for the in-between case where the stack is custom and you want to know where to push.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What INP measures

Interaction to Next Paint (INP) is the worst-case latency between a user input — click, tap, keypress — and the next frame the browser paints in response. Chrome tracks every interaction over the page's lifetime and reports the 98th-percentile slowest one as the page's INP. Good is **≤ 200 ms**, "needs improvement" extends to 500 ms, and anything over 500 ms is failing. Real-user numbers come from CrUX; the lab approximation from Lighthouse via PSI is unreliable because the lab issues no human input, so the field figure is the only one worth optimizing against. See [web.dev/inp](https://web.dev/articles/inp).

## The most common INP failures on nginx-fronted sites

INP failures on a generic nginx stack tend to be in the indirect dimensions — how response handling, upstream latency, and connection management interact with interactive JS.

1. **Long-tail TTFB on `/api/*` endpoints blocks synchronous interactive widgets.** Autocomplete, infinite scroll, "load more", and any AJAX-driven UI fires a request on the user's input. If nginx is fronting a slow upstream (un-tuned PHP-FPM, undersized Node process, a cold-cache database query), the request waits 200–800 ms before first byte. INP includes that wait whenever the handler is synchronous, which is most of them.
2. **No `keepalive` on upstream connections.** Default `nginx.conf` does not set `keepalive 32;` on `upstream` blocks. Every AJAX call from an interactive widget incurs a full TCP + TLS handshake to the upstream, adding 30–100 ms per interaction. The user notices this most on rapid-fire interactions (type-ahead, scroll).
3. **`worker_connections` exhausted under burst.** SPA hydration on page load often fires many parallel AJAX requests; if `worker_connections 768` (the default) is full, new connections queue behind closed worker slots. INP for the next interaction = worst-case queue wait. Symptom: INP spikes correlated with traffic peaks.
4. **HTTP/1.1 head-of-line blocking.** Sites still on HTTP/1.1 (no `listen 443 ssl http2;`) serialize XHRs over one or two parallel connections. Interactions that fire concurrent fetches all wait for the slowest in the queue.

## Find the responsible interaction

Generic stacks need a slightly more careful diagnose step than CMS stacks because you do not know up front which interaction is the worst. Sample widely.

- Open the page in Chrome, DevTools → **Performance** panel, record, perform every interactive flow on the page (nav, search, any form, any in-page widget). Stop. The **Interactions** track lists each input with its INP duration. Click the worst → flame chart.
- Open DevTools → **Network**, filter by Fetch/XHR, replay the worst interaction. Each XHR shows TTFB, content time, and total time. The TTFB column is what nginx + upstream contribute.
- Install [`web-vitals`](https://github.com/GoogleChrome/web-vitals) with `attribution` mode. Log `onINP(console.log, {reportAllChanges: true})` from a `<script type="module">` block. `attribution.eventEntry` names the responsible handler.
- On the server: `awk '$NF > 0.5 {print}' /var/log/nginx/access.log | head -50` lists requests with > 500 ms response time. The endpoints that show up here are the ones whose synchronous AJAX is contributing to INP.
- `nginx -T | grep -E 'gzip|http2|keepalive|worker_connections'` audits the actual loaded config. The file you think you are editing is not always the file nginx is loading (include-glob mismatches are common).

Output: a specific interaction, the responsible handler or XHR endpoint, and a baseline number to beat.

## Defer, combine, and minify JS via ngx_pagespeed

mod_pagespeed's leverage on a generic nginx site mirrors the WordPress case but without the CMS-specific failure modes. Enable:

- **`defer_javascript`** — pushes script execution past the initial render so first-interaction JS parse no longer collides with the click. Marked **Test first** in the [filters reference](/docs/filters/); test on a staging copy because it changes execution order.
- **`combine_javascript`** — concatenates separate `<script src>` files into one. Cuts per-file parser-setup overhead.
- **`rewrite_javascript`** — minifies inline and external JS. Useful if upstream serves non-minified bundles, which is common in custom stacks. The minifier is [ASI-safe on unterminated statements](/blog/safe-javascript-minification-semicolon-insertion/), so it will not break bundles that rely on automatic semicolon insertion.

Minimal nginx config:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters defer_javascript;
pagespeed EnableFilters combine_javascript;
pagespeed EnableFilters rewrite_javascript;
```

Sanity check: if your stack already ships a modern code-split SPA bundle (React/Vue/Svelte with tree-shaking, gzipped, served via a CDN), mod_pagespeed adds little to INP — those bundles are already lean. If your stack serves a bundled blob of legacy JS from the application server, mod_pagespeed helps a lot. The Performance trace from the Diagnose step tells you which one you have.

## Upstream and transport tuning

Most of the INP wins on a generic nginx site live in the upstream connection layer and the HTTP transport, not in the application JS. These are the tunables that move the metric independent of what CMS is behind you.

- **Enable HTTP/2.** `listen 443 ssl http2;`. Multiplexed connections eliminate per-XHR setup cost; concurrent fetches stop blocking each other. HTTP/3 if your nginx ≥ 1.25 is built with QUIC support.
- **`keepalive` on every `upstream` block** + `proxy_http_version 1.1;` + `proxy_set_header Connection "";` in the matching `location` blocks. Removes the TCP+TLS handshake from every upstream request. 30–100 ms saved per interaction.
- **`worker_connections 4096;`** (up from default 768). Costs a few MB of memory; eliminates connection-queue stalls under burst.
- **Short `proxy_read_timeout` on interactive endpoints.** `proxy_read_timeout 5s;` for `/api/*` locations — catches slow upstreams instead of letting them hang. INP-critical endpoints should error fast, not hang.
- **`gzip_static on;` + pre-compressed `.gz` and `.br` files** (Brotli via `ngx_brotli`). Removes per-request gzip CPU cost; first-byte on every response gets slightly faster.
- **`ssl_session_cache shared:SSL:10m; ssl_session_timeout 1d;`.** TLS resumption saves ~80 ms on repeat connections from the same client.
- **Profile upstream**, not just nginx. `awk` on `access.log` for `>0.5s` requests. The endpoints that show up are the real INP culprits; fix them server-side (caching, indexing, async I/O).

## Confirming the win

1. Re-run `awk '$NF > 0.5 {print}' /var/log/nginx/access.log | head -50` after a day of traffic. The slow-endpoint list should be much shorter; if it is not, the upstream tuning work remains.
2. DevTools → Performance, replay the worst interaction from your baseline, confirm under-200 ms in the **Interactions** track. Check DevTools → Network for XHR TTFB on interaction-triggered fetches — should be under 100 ms for cached endpoints, under 300 ms for dynamic ones.
3. Re-run PSI on representative URLs. Field "Interaction to Next Paint" should drop after the rewriter pass (small-to-moderate move) and after the transport tuning (often larger if upstream was the issue).
4. Wait 28 days, check Search Console → Core Web Vitals report. Generic nginx sites usually have a single URL group; track it as a whole.

## The nginx snippet

```nginx
# nginx — minimal config to address INP on a generic stack
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters defer_javascript;
pagespeed EnableFilters combine_javascript;
pagespeed EnableFilters rewrite_javascript;

# Transport tuning that often matters more than the rewrite layer:
# listen 443 ssl http2;
# worker_connections 4096;
# In upstream blocks: keepalive 32;
# In location blocks for upstream: proxy_http_version 1.1; proxy_set_header Connection "";
```

For Apache (the equivalent CMS-agnostic case):

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed
ModPagespeedRewriteLevel CoreFilters
ModPagespeedEnableFilters defer_javascript
ModPagespeedEnableFilters combine_javascript
ModPagespeedEnableFilters rewrite_javascript
```

## When this doesn't work

INP is a client-side metric and nginx is a server. The leverage points are real but bounded.

- **Modern code-split SPA bundles.** If your app already ships React or Vue with route-based code splitting, gzipped via Vite or webpack, and served from a CDN, mod_pagespeed's `combine_javascript` and `rewrite_javascript` add little — the bundle is already optimized. Profile first; do not enable filters that fight your build pipeline.
- **Third-party scripts you do not control.** Intercom, HotJar, GTM-injected analytics, chat widgets — INP attribution on these stays with their script. mod_pagespeed minifies them but cannot change their handlers. Defer via tag-manager config or remove; [Chrome Coverage tells you which of them ships unused bytes](/blog/remove-unused-javascript-chrome-coverage/) worth cutting.
- **Slow upstream behind nginx is an upstream problem.** If `awk` on `access.log` shows your `/api/search` endpoint takes 600 ms, no rewriter at the nginx layer fixes that. The fix is in the upstream — DB indexing, caching, async handlers. nginx tuning (`keepalive`, HTTP/2) removes connection overhead but not handler latency.
- **Geographic distance from origin.** Users on a continent away from your origin pay round-trip latency on every interactive XHR. A CDN with edge functions or a co-located cache layer is the fix; nginx alone cannot shorten the speed of light.
- **Hand-written JS with synchronous heavy work.** If a button-click handler runs 200 ms of DOM thrashing or a synchronous JSON parse on a 500 KB blob, mod_pagespeed minifies the source but the runtime cost is the same. Profile the handler, find what is slow, fix it. Common offender: hand-rolled state-machine code on form pages.

This post deliberately does _not_ try to be the definitive INP guide. The INP × CMS posts ([WordPress](/blog/fix-inp-wordpress-2026/), [WooCommerce](/blog/fix-inp-woocommerce-2026/), [Magento](/blog/fix-inp-magento-2026/), [ASP.NET Core](/blog/fix-inp-aspnet-core-2026/)) are where the platform-specific failure modes get named. This post is the generic transport-and-rewriter framing for when your stack is not on that list.

## Related

- [How to fix LCP on nginx](/blog/fix-lcp-nginx-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix CLS on nginx](/blog/fix-cls-nginx-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [How to fix INP on WordPress](/blog/fix-inp-wordpress-2026/)
- [Server-side critical CSS with nginx](/blog/server-side-critical-css-nginx/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full INP guide](/core-web-vitals/inp/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).
