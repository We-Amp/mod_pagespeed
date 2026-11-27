---
title: 'How to reduce TTFB: server-layer wins before a CDN'
description: 'Reduce TTFB on nginx: split origin compute, network, and cache, then fix keep-alive, HTTP/2 or HTTP/3, TLS resumption, and variant-aware caching.'
date: 2026-06-06
lastUpdated: 2026-10-22
tags: ['performance', 'ttfb', 'nginx', 'core-web-vitals']
product: '2.0'
howTo:
  name: How to reduce TTFB at the server layer
  description: Measure TTFB and split it into origin compute, network, and cache. Fix the server-layer levers (keep-alive, HTTP/2 or HTTP/3, TLS session resumption, pre-compressed assets, and a variant-aware cache that serves optimized responses without recomputing per request), then confirm the result.
  tools:
  - nginx
  - PageSpeed Insights
  - curl
  - Chrome DevTools
  - WebPageTest
  steps:
  - name: Measure TTFB and read the audit
    text: 'Run PageSpeed Insights on the URL and read the "Reduce initial server response time" diagnostic — it flags red above 600 ms in the lab. Confirm field TTFB with curl: curl -w ''dns=%{time_namelookup} connect=%{time_connect} tls=%{time_appconnect} ttfb=%{time_starttransfer}\n'' -o /dev/null -s https://<host>/. The four phases tell you whether the cost is DNS, TCP, TLS, or the wait for the first byte (origin compute).'
  - name: Split TTFB into origin compute, network, and cache
    text: 'Subtract the connection phases (DNS, TCP, TLS) from the total to isolate the server-think portion (time_starttransfer minus time_appconnect). Hit a cached URL and an uncached one separately. A cached request that is still slow points at transport (no keep-alive, no session resumption, on-the-fly compression). An uncached request that is slow points at origin compute — the app, the database, or the backend.'
  - name: Fix the transport and connection layers
    text: 'Enable HTTP/2 (listen 443 ssl; http2 on;) or HTTP/3 (listen 443 quic reuseport; add the Alt-Svc header), turn on upstream keepalive so nginx reuses backend connections, enable TLS session resumption (ssl_session_cache shared:SSL:10m; ssl_session_tickets), and serve pre-compressed assets with gzip_static on; and brotli_static on; instead of compressing on every request.'
  - name: Serve optimized variants from cache instead of recomputing
    text: 'Put a variant-aware cache in front of the work so the optimized response is computed once and served from memory thereafter. With the mod_pagespeed optimizer worker, In-Place Resource Optimization (IPRO) generates optimized variants asynchronously; nginx serves the best-fit variant for each client from the memory-mapped Cyclone cache, so the per-request cost on a hit is a cache lookup, not a re-encode.'
  - name: Confirm the win and watch the field data
    text: 'Re-run PSI and confirm the server-response audit clears 600 ms. Re-run the curl timing on both a cached and an uncached URL to confirm the connection phases shrank. Then watch real-user TTFB in Search Console and CrUX over the following weeks, since field numbers move on the trailing 28-day window, not instantly.'

---

A slow Time to First Byte is the kind of audit finding that sends people straight to a CDN. Sometimes that is the right move. More often the first byte is slow for reasons a CDN does not touch, and you can fix most of them in your nginx config before you add a vendor to the request path. This post covers those server-layer changes, and where the server layer stops helping.

## What TTFB is, and how PSI flags it

TTFB is the time from the start of the request to the first byte of the response body arriving at the client. It covers DNS resolution, the TCP handshake, the TLS handshake, the request travelling to the server, the server producing a response, and the first byte travelling back.

TTFB is not itself a Core Web Vital. The three are [LCP](/core-web-vitals/lcp/), [INP](/core-web-vitals/inp/), and [CLS](/core-web-vitals/cls/). But TTFB sits upstream of LCP: the browser cannot start rendering before the first byte lands, so your LCP floor is your TTFB. If TTFB is 800 ms, nothing paints before 800 ms no matter how small your hero image is.

Two thresholds matter, and people mix them up:

- **The lab audit.** [PageSpeed Insights](/pagespeed-insights/) and Lighthouse run a "Reduce initial server response time" audit. It flags red above **600 ms**, measured under Lighthouse's throttled lab conditions. That is the warning that prompts most people to read this kind of post.
- **The field metric.** [web.dev](https://web.dev/articles/ttfb) treats real-user TTFB as **good at 0.8 s or less** and **poor above 1.8 s**, at the 75th percentile across the trailing 28 days. This is the number Chrome's CrUX dataset and Search Console report, and it is the one that actually correlates with your LCP in the wild. The 2025 Web Almanac found only 44% of mobile pages hit "good" TTFB, so this is not a rare problem.

Fix the field number and the lab audit follows.

## Split the number before you spend money

A single TTFB figure hides three different problems with three different owners. Pull them apart first:

```
curl -w 'dns=%{time_namelookup} connect=%{time_connect} \
tls=%{time_appconnect} ttfb=%{time_starttransfer}\n' \
-o /dev/null -s https://<host>/
```

- **Origin compute** is `time_starttransfer` minus `time_appconnect`, the server-think time. This is your application, your database, your template render. Hit an uncached URL to see it.
- **Network** is the connection phases: DNS, TCP, TLS. A CDN's edge PoPs shorten these for distant users, which is the legitimate case for reaching for one.
- **Cache** is the difference between a cached and an uncached request. Hit the same URL twice and compare.

Run the timing against both a cached and an uncached path. If the cached request is still slow, the cost is transport, and transport lives in your nginx config. If the uncached request is slow but the cached one is fast, your origin compute is the long pole, and no amount of caching headers fixes a query that takes 900 ms to run.

## The server-layer levers

These are the changes that live entirely in nginx and cost nothing per request.

**Keep-alive to the upstream.** By default nginx opens a fresh connection to your backend for every request. An `upstream` block with `keepalive 32;` and `proxy_http_version 1.1;` reuses connections, which removes a TCP (and possibly TLS) handshake from the server-think portion of every dynamic response. This is the single most common forgotten setting on reverse-proxy setups.

**HTTP/2 or HTTP/3.** On HTTP/1.1, the client serializes requests across a handful of connections and the document can queue behind earlier assets. HTTP/2 multiplexes them over one connection. Use the current directive form (`listen 443 ssl;` plus `http2 on;`) rather than the deprecated `listen ... http2`. HTTP/3 over QUIC removes head-of-line blocking at the transport level and adds 0-RTT resumption for returning visitors; nginx has shipped QUIC since 1.25.0, and it carried over into the 1.26 and later stable branches (the current stable branch is 1.28.x). Enable it with a `quic` listener and an `Alt-Svc` header advertising `h3`. None of this helps the first byte of a single isolated request much, but it cuts the wait when the document competes with other in-flight requests, which is the real-world case.

**TLS session resumption.** A full TLS handshake is a round trip you pay on every cold connection. `ssl_session_cache shared:SSL:10m;` with `ssl_session_tickets on;` lets returning clients resume without it. For users far from your origin, this is often a larger TTFB win than anything you do to the application.

**Pre-compressed assets.** `gzip on;` compresses on the fly, burning 5–30 ms of CPU per response and serializing behind the worker pool under load. Pre-compress your static assets at build time and serve them with `gzip_static on;` and `brotli_static on;`, which hands nginx a `.gz` or `.br` file straight off disk. Brotli at a high level produces smaller files than gzip, and because you compressed once at build time you can afford the higher level.

**A sane upstream / FastCGI cache.** For dynamic responses that [stay fresh for even a few seconds](/blog/default-cache-ttl-heuristic-freshness/), `proxy_cache` or `fastcgi_cache` with `proxy_cache_use_stale updating;` lets nginx serve a cached response while it revalidates in the background. The slow origin render happens off the request path. A short `s-maxage` with `stale-while-revalidate` does the same thing for any CDN sitting in front.

## Serve optimized variants from cache, don't recompute them

There is a quieter TTFB cost most people never measure: doing optimization work in the request path. If your stack re-encodes an image, minifies CSS, or inlines critical CSS on the fly per request, that work lands inside TTFB for every uncached response.

This is the part [the optimizer worker](/features/) is built around. Optimization happens out of the request path. On a cache miss, nginx serves the original immediately and sends a notification to the worker; the worker reads the original, optimizes it, and writes the result into the [Cyclone cache](/blog/content-hash-urls/). The next matching request is a cache hit served via zero-copy `mmap`, with no re-encoding on the request path.

The cache is variant-aware, which matters for TTFB specifically. A naive cache that keys only on the URL either serves the wrong format to some clients or recomputes per client. Cyclone stores multiple variants under one URL key — WebP and AVIF and the optimized original, across viewport and density and Save-Data — each tagged with the client capability it matches. A request gets the best-fit variant from cache with no recomputation, and falls back to a closer variant or the original if its exact match is not warm yet. The optimization cost is paid once, asynchronously, instead of landing inside TTFB on every request. This is the same mechanism behind the LCP work in our [nginx LCP guide](/blog/fix-lcp-nginx-2026/); the TTFB benefit comes from the same design.

## What the server layer does not fix

None of the levers above make a slow application fast.

If your `time_starttransfer` on an uncached, keep-alive'd, resumed connection is still 700 ms, the time is going into your code or your database. An N+1 query, a cold cache in your ORM, a synchronous call to a third-party API, a render that walks a 5,000-item collection: a reverse proxy cannot speed any of that up. It can cache the result so you pay the cost less often, but the uncached path stays as slow as the backend makes it. Profile the application and fix the query first, then judge whether the residual network distance is worth a CDN.

A CDN's job is the network phase: shortening DNS, TCP, and TLS for users far from your origin, and absorbing traffic. It does not fix origin compute, and it does not give you variant-aware optimization unless you pay for that feature separately. A working order: split the number, fix the transport in nginx, [move optimization out of the request path](/how-it-works/async-rewriting/), fix the application if the uncached path is still slow, and reach for the CDN last for the network distance it addresses.
