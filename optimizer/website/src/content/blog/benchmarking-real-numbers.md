---
title: 'Benchmarking ModPageSpeed 2.0: real numbers on real sites'
description: 'Measured ModPageSpeed 2.0 results on e-commerce, blog, news and portfolio sites: WebP/AVIF savings and LCP, FCP, CLS and Lighthouse gains, 3G to broadband.'
date: 2026-02-05
lastUpdated: 2026-10-25
author: 'Otto van der Schaaf'
tags: ['performance', 'benchmarks', 'image-optimization', 'core-web-vitals']
draft: false
product: '2.0'
---

> **Data collected:** February 2026 · **Tested with:** ModPageSpeed 2.0
>
> Written for the ModPageSpeed 2.0 line, the predecessor of today's mod_pagespeed; the numbers below were measured on 2.0. For the current release, start with [getting started](/docs/getting-started/).

## Methodology

All benchmarks were conducted on standard cloud infrastructure to reflect real-world conditions: 4-vCPU / 8 GB instances running the full ModPageSpeed 2.0 stack (nginx interceptor, Cyclone cache, worker). No bare-metal optimization or hardware acceleration was used. Network conditions were simulated using WebPageTest's traffic shaping profiles for 3G (1.6 Mbps / 300ms RTT), 4G (9 Mbps / 70ms RTT), and broadband (20 Mbps / 20ms RTT).

Each test site was loaded 10 times with ModPageSpeed 2.0 enabled and 10 times against the original origin, with the median of each metric reported. The cache was warmed before measurement by making an initial request, then waiting for the worker to process all variants. This reflects steady-state performance -- what real users experience after the first visitor triggers optimization. Cold-start (first-visit) numbers are reported separately where relevant.

Measurements were captured using Lighthouse CI (the same performance category [PageSpeed Insights](/pagespeed-insights/) reports), WebPageTest (filmstrip + waterfall), and Chrome DevTools Protocol (network transfer sizes). Core Web Vitals -- Largest Contentful Paint (LCP), First Contentful Paint (FCP), Cumulative Layout Shift (CLS), and Total Blocking Time (TBT) -- are the primary metrics.

The portfolio section is the exception: it was re-measured on 2026-10-25 for response bytes only, by requesting every image variant through nginx with the worker at its shipped defaults and comparing the response sizes with the originals. That section names its corpus and settings.

## E-commerce results

The test e-commerce site represents a typical product listing page: 42 product images (JPEG, average 180 KB each), a 320 KB CSS framework, 280 KB of JavaScript, and a 45 KB HTML document. Total original page weight: 8.4 MB.

With ModPageSpeed 2.0 enabled, image transcoding produced the largest single improvement. The worker produces [WebP and AVIF variants](/blog/avif-vs-webp-2026/) from a single decode pass (using the `TranscodeMulti` pipeline). Browsers supporting AVIF received images averaging 68% smaller than the original JPEGs. WebP clients saw 45% reductions. The capability mask's image format bits (bits 0-1) ensure each browser gets the best format it supports, with no markup changes required.

CSS minification through the four-phase pipeline -- comment stripping, whitespace collapsing around operators, trailing semicolon removal, and decimal optimization (`0.5` to `.5`) -- reduced the main stylesheet from 320 KB to 247 KB (23% reduction). Critical CSS injection inlined 12 KB of above-the-fold styles directly into the HTML, eliminating the render-blocking request.

**Results on 4G (median of 10 runs):**

| Metric                   | Without | With   | Improvement |
| ------------------------ | ------- | ------ | ----------- |
| Total page weight        | 8.4 MB  | 3.8 MB | -55%        |
| First Contentful Paint   | 2.8s    | 1.4s   | -50%        |
| Largest Contentful Paint | 4.9s    | 2.3s   | -53%        |
| Lighthouse Performance   | 48      | 82     | +34 points  |
| Time to Interactive      | 6.2s    | 4.1s   | -34%        |

The asynchronous architecture means none of this optimization adds latency to the request path. The first visitor gets the original page (with an `X-PageSpeed: MISS` header), while the worker processes variants in the background. Subsequent visitors get optimized content served via zero-copy mmap from the Cyclone cache.

## Blog results

The test blog site uses a WordPress theme with a 450 KB CSS framework (only 8% of rules used on any given page), minimal images, and text-heavy content. This is the ideal scenario for critical CSS extraction, where the ratio of critical to total CSS is extremely low.

The `CriticalCssExtractor` retained 47 of 1,284 total rules (3.7%) as critical, producing a 14 KB critical CSS block injected before `</head>`. This eliminated the render-blocking 450 KB stylesheet request entirely for the initial paint.

**Results on 3G (median of 10 runs):**

| Metric                 | Without | With                                      | Improvement    |
| ---------------------- | ------- | ----------------------------------------- | -------------- |
| Total CSS transferred  | 450 KB  | 14 KB (inline) + 348 KB (minified, async) | -20% effective |
| First Contentful Paint | 4.1s    | 1.9s                                      | -54%           |
| CLS                    | 0.12    | 0.02                                      | -83%           |
| Lighthouse Performance | 62      | 91                                        | +29 points     |

The FCP gain is large because the render-blocking CSS was the primary bottleneck. With critical styles inlined, the browser can paint meaningful content after receiving just the HTML response, while the full minified stylesheet loads asynchronously. The CLS improvement comes from the critical CSS including layout-defining rules for header, nav, and hero sections, preventing layout shifts as the full stylesheet arrives.

## News portal results

News sites present the toughest test case: large HTML documents (120+ KB), dozens of images per page, complex multi-column layouts, frequent content updates that invalidate cache entries, and aggressive ad scripts that compete for bandwidth.

The test news portal included 28 images, a 280 KB CSS bundle, and 380 KB of JavaScript. The key metric here is how the system sustains throughput under continuous content churn.

The [fire-and-forget architecture](/blog/fire-and-forget-worker-ipc/) proved critical. Nginx records responses to cache and sends notifications to the worker without waiting for a response. The wire protocol is minimal: `[4B length][4B url_len][url][1B content_type][4B capability_mask]`. Notification delivery takes microseconds. If the worker is busy processing a backlog, nginx continues serving original content from cache (default mask fallback) with no added latency.

**Results on 4G with warm cache (median of 10 runs):**

| Metric                   | Without | With   | Improvement |
| ------------------------ | ------- | ------ | ----------- |
| Total page weight        | 6.2 MB  | 3.1 MB | -50%        |
| First Contentful Paint   | 2.4s    | 1.2s   | -50%        |
| Largest Contentful Paint | 5.1s    | 2.8s   | -45%        |
| Lighthouse Performance   | 41      | 73     | +32 points  |

Cache hit rates stabilized at 94% within 30 minutes of operation, with the remaining 6% being new articles and images. The notification deduplication logic avoids redundant work: when multiple requests trigger notifications for the same URL and capability mask before the worker finishes processing, duplicates are detected via a cache existence check and skipped.

## Portfolio results

The photography portfolio page is image-dominated: 18 photographs, minimal CSS, and almost no JavaScript. The portfolio figures below come from a byte-level re-measurement on 2026-10-25: 18 landscape photographs from the picsum.photos library (Unsplash License) at 2400x1600 pixels, 6.9 MB as JPEG (383 KB per image on average), fetched through nginx with the worker at its shipped defaults and compared variant by variant with the originals.

This is where the capability mask system pays off most. The 32-bit mask encodes image format (bits 0-1), [viewport class](/blog/viewport-aware-image-optimization/) (bits 2-3), pixel density (bit 4), Save-Data preference (bit 5), and transfer encoding (bits 6-7). The worker generates WebP, AVIF, and optimized-original variants proactively from a single decode pass when `proactive_image_variants` is enabled. It is off by default; these benchmarks were run with the flag turned on.

AVIF proved the most effective format for photographic content, with WebP close behind. At the shipped defaults (`avif_speed` 6 and `avif_quality` 60 as the starting point, with the learned quality model and SSIMULACRA2 verification choosing the final quality per image), the AVIF variants were 55% smaller than the original JPEGs by total bytes, 60% on average per image. WebP came out 52% smaller by total (53% per image) and the re-encoded JPEG 36% smaller (42% per image). One AVIF and two WebP variants had not finished encoding inside the five-minute measurement window and are left out of those totals. For comparison, pinning AVIF quality at 25 with the learned model off, as an earlier version of this post did, takes the AVIF total down by 87% at a quality well below the shipped default; that setting is not what ships.

**Image bytes served (desktop, Save-Data off; the measured ratios applied to the full 6.9 MB):**

| Metric            | Without | With (AVIF client) | With (WebP client) |
| ----------------- | ------- | ------------------ | ------------------ |
| Total image bytes | 6.9 MB  | about 3.1 MB       | about 3.3 MB       |

The portfolio page was re-measured for bytes only, so it carries no Largest Contentful Paint or Lighthouse row. On this corpus AVIF was the smaller format for 10 of the 15 photographs that had both an AVIF and a WebP variant, and WebP for the other 5: at an equal perceptual quality target the two formats are closer than their reputations suggest.

For clients sending the `Save-Data: on` header, the worker can be configured to apply more aggressive compression. The capability mask encodes this preference at bit 5, allowing the cache to serve a more compressed variant without any origin-side changes. On this corpus the Save-Data AVIF variants were a further 45% smaller than the normal AVIF variants, WebP a further 33% and JPEG a further 24%.

## Comparison with CDN-based solutions

CDN-based image optimization services (Cloudinary, imgix, Cloudflare Polish) are easy to adopt: change your image URLs or flip a switch, and images are optimized at the edge. But the [cost model is fundamentally different from self-hosted optimization](/blog/economics-of-image-optimization/).

CDN services bill per transformation plus bandwidth, so the image-optimization line item grows with every request served. At a million image requests a month it is a recurring monthly charge; at ten million it multiplies in step. These costs scale linearly with traffic and never stop.

mod_pagespeed is free under the Apache License 2.0 with all optimizations included ([see pricing](/pricing/)) -- it runs on every server behind the site at no cost, so a redundant pair or a load-balanced fleet costs the same as one box. A single server can handle tens of thousands of unique images -- each image is transcoded once per format variant, then served from the Cyclone cache for all subsequent requests. The cache uses [memory-mapped I/O for zero-copy serving](/blog/memory-mapped-cache-zero-copy-serving/), so the marginal cost per request is effectively zero.

Where CDN solutions have a genuine advantage is global edge distribution. If your audience is distributed across continents, edge-based transformation reduces latency that origin-based processing cannot. For sites with a regional audience, or those already using a CDN for distribution (which passes through the optimized origin responses), self-hosted optimization produces the same quality improvements at a fraction of the cost.

The other differentiator is scope. CDN image optimization handles images only. mod_pagespeed optimizes HTML (critical CSS injection, Early Hints), CSS (four-phase minification), JavaScript (token-aware minification), and images -- all from a single installation with no URL rewrites or application changes.
