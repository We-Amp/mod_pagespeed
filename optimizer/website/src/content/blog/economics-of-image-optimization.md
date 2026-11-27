---
title: 'Image CDN cost comparison: Cloudflare, Cloudinary, imgix'
description: 'Image CDNs run $35 to $3,500/mo as traffic grows; self-hosted mod_pagespeed stays flat. Break-even is ~100 to 170K requests/mo. The 2026 cost model.'
date: 2026-02-07
lastUpdated: 2026-10-25
author: 'Otto van der Schaaf'
coverImage: '/images/economics-of-image-optimization-card.png'
tags: ['image optimization cost', 'image CDN', 'cloudinary', 'imgix', 'cloudflare images', 'self-hosted image cdn', 'gdpr', 'economics']
draft: false
faq:
  - q: 'Is self-hosted image optimization cheaper than Cloudinary or imgix?'
    a: 'Below roughly 100,000–170,000 image requests per month, a CDN like Cloudinary or imgix is usually cheaper, because per-unit rates are low at small volumes. Above that break-even, self-hosted mod_pagespeed wins: its cost is flat regardless of request volume, while CDN bills keep climbing with traffic, reaching about 60–100x the self-hosted cost at 10 million requests per month.'
  - q: 'Why do Cloudinary and imgix bills jump on renewal?'
    a: 'Both bill against consumable credits that scale with transformations and delivery. As your catalog and traffic grow, credit consumption grows with them and renewal quotes rise. Some plans restructure tiers or apply quota cutoffs rather than overage billing, so you can hit a hard cap instead of a predictable overage. A self-hosted model has no usage-based input, so the bill does not move with traffic.'
  - q: 'Is mod_pagespeed a DIY tool like Thumbor or imgproxy?'
    a: 'No. Thumbor and imgproxy are image-processing servers you wire into your stack with URL rewriting and API calls, which is the ops burden critics cite. mod_pagespeed is an nginx reverse proxy plus an async worker: Docker Compose in about five minutes, no URL rewriting, no API keys. It optimizes images on their original URLs automatically.'
  - q: 'Does self-hosting images help with GDPR and data sovereignty?'
    a: 'Routing image requests through a third-party CDN is a data transfer to a processor, which typically means a data processing agreement (DPA) and ongoing compliance review. mod_pagespeed decodes, transcodes, and serves images entirely on your own infrastructure, so user content never leaves your network — which is what GDPR and data-residency requirements are usually about.'
  - q: 'Are there bandwidth or per-transformation fees with self-hosted optimization?'
    a: 'No. There are no per-transformation, per-image, or vendor bandwidth fees. The software is licensed under Apache-2.0 and free to run; you pay for your own VM and the bandwidth your host already charges. Past the break-even point, additional image requests cost essentially nothing beyond that baseline host bandwidth.'
  - q: 'What happens to my images if the optimization service goes down?'
    a: 'With a rewriting CDN, an outage can slow or break image loading across the pages that depend on it. mod_pagespeed degrades gracefully: if the worker is down, nginx serves the original image from cache and the worker catches up on recovery, so images never fail to load.'
---

> **Data collected:** February 2026 · **Tested with:** ModPageSpeed 2.0 · **Pricing re-verified:** June 2026 (vendors); September 2026 (mod_pagespeed)

## Image CDN cost comparison: Cloudinary, imgix, and Cloudflare Images

If you are evaluating image CDNs rather than self-hosting, the table below maps the major providers' published list-price models onto one representative workload so you can compare like for like. The workload is the **1,000,000 monthly image requests / ~100 GB delivered** tier from the cost model below — a mid-sized catalog or content site. Pricing models differ (some bundle usage into credits, some bill per stored or delivered image, some fold it into a CDN contract), so the right-hand column states the cost driver each provider leans on rather than implying the bills are directly comparable line by line.

| Provider                    | Headline pricing model                                                                    | Free tier                                   | What drives the bill at scale                                  |
| --------------------------- | ----------------------------------------------------------------------------------------- | ------------------------------------------- | -------------------------------------------------------------- |
| **Cloudinary**              | Credit bundles (1 credit ≈ 1,000 transforms / 1 GB storage / 1 GB delivery)               | Yes — free plan (25 credits/mo)             | Credits consumed; scales with transformations + delivery       |
| **imgix**                   | Credit-based (management / delivery / transformation credits; ~1 credit per GB delivered) | Trial + paid Starter; no standing free plan | Credits; scales with delivery + transformation volume          |
| **Cloudflare Images**       | Per 100,000 images stored (~$5) + per 100,000 delivered (~$1)                             | Yes — 5,000 free transformations/mo         | Images stored + images delivered (flat per-unit rates)         |
| **Akamai Image Manager**    | Add-on to a CDN contract; usage- and contract-negotiated                                  | No (trial only)                             | Negotiated CDN contract + per-image transformation volume      |
| **Self-hosted (this post)** | Free software (Apache-2.0) + your own VM                                                  | Yes — free to run in development and in production | Nothing usage-based: cost is flat regardless of request volume |

**How to read this.** Per-unit CDN rates are low at the bottom of each tier, which is why a small site of under roughly 100,000 requests a month often pays less on a CDN than the flat self-hosted floor — your own VM (the software is free to run). The variable that matters is _which input grows with your traffic_. Cloudinary's and imgix's credits both scale with delivery and transformation volume; Cloudflare Images scales with the number of images you store and deliver; Akamai folds image transformation into a CDN contract that is itself usage-tied. In every case the bill rises with traffic. The self-hosted model has no usage-based input at all — past the break-even point below, additional requests cost only the bandwidth you would pay your host regardless. For a head-to-head against a specific provider, see how mod_pagespeed compares to [Cloudinary](/alternatives/cloudinary/) and [imgix](/alternatives/imgix/).

> **Pricing caveat.** The models above reflect each vendor's _publicly published list pricing structure_ as understood in February 2026, not negotiated or current per-unit rates. Vendors change pricing, restructure tiers, and negotiate enterprise discounts; the free tiers and exact per-unit figures move over time. Treat this as a guide to _what each model charges for_, then confirm current numbers on each vendor's own pricing page before you commit. We do not republish per-provider dollar totals here because doing so accurately would require pinning a specific transformation/density/format mix to each vendor's current rate card, which goes stale the moment any vendor updates it.

If global edge latency is your priority and your traffic is modest, an image CDN is a reasonable choice — that is the case the break-even analysis below makes explicitly. If your traffic is high, your data must stay on your own infrastructure, or you want a bill that does not move with traffic, self-hosting wins. The two are not mutually exclusive: see [When self-hosted makes sense](#when-self-hosted-makes-sense) for the hybrid pattern.

## What image optimization actually costs

Images account for roughly 50% of page weight on the median website, according to the HTTP Archive's annual survey, and that share has held steady for a decade even as codecs improved — designers tend to fill whatever bandwidth a connection gives them. That makes image optimization the single intervention with the largest effect on bandwidth bills, load times, and Core Web Vitals. The open question is not whether to optimize but who you pay to do it: an image CDN that bills per transformation and per gigabyte, or your own infrastructure at a flat cost.

The market for image optimization has split into two camps. On one side are CDN and SaaS services -- Cloudinary, imgix, Cloudflare Images, Akamai Image Manager -- that charge per transformation, per bandwidth, or per stored image. On the other side are [self-hosted tools](/self-hosted-image-optimization/) that trade operational complexity for predictable, usage-independent costs and data sovereignty.

A fair comparison accounts for four cost categories: **compute** (CPU time to transcode images), **bandwidth** (data transfer to clients), **storage** (cached variants on disk), and **operational overhead** (setup time, maintenance, monitoring, and incident response). The model below prices each category, then finds the crossover point where self-hosted optimization wins.

## CDN pricing models

CDN-based image optimization services use several pricing structures, but they all share one trait: costs scale with traffic.

**Per-transformation pricing** is the most common model. Services charge $1-5 per 10,000 transformations (resize, format conversion, quality adjustment). A product catalog with 10,000 images, each served in 3 format variants (original, WebP, AVIF) at 2 sizes (mobile, desktop), generates 60,000 unique transformations. At $3 per 10,000, that is $18 for the initial transformation pass. The real cost comes from ongoing transformations as new products are added and cached variants expire.

**Bandwidth-based pricing** charges for the optimized data transferred. Rates range from $0.05-0.20 per GB depending on the provider and region. A site serving 100 GB of optimized images per month pays $5-20 in bandwidth fees -- on top of the transformation charges. A self-hosted model has no vendor bandwidth fees at all: you pay only what your host charges, and you can still [serve smaller images on slow connections](/blog/save-data-bandwidth/) with Save-Data variants at no per-byte surcharge.

**Hybrid models** include a quota of free transformations or storage, then charge overages. These are attractive at low volumes but converge to per-unit pricing as traffic grows.

Here is what the monthly bill looks like at three traffic levels, using median published rates:

| Monthly image requests | Transformations cost | Bandwidth cost (est.) | Total  |
| ---------------------- | -------------------- | --------------------- | ------ |
| 100,000                | $30                  | $5                    | $35    |
| 1,000,000              | $300                 | $50                   | $350   |
| 10,000,000             | $3,000               | $500                  | $3,500 |

These numbers are conservative. They assume static images with long cache TTLs. Sites with frequent content updates, A/B testing, or personalized images will see higher transformation counts.

## Self-hosted cost model

The mod_pagespeed optimizer worker transcodes images asynchronously in a separate process, writing optimized variants to the shared Cyclone cache. Each image is decoded once and encoded into multiple formats in a single pass (WebP, AVIF, and optimized original) when the `proactive_image_variants` option is enabled. Subsequent requests are served directly from the memory-mapped cache -- zero-copy, no re-encoding.

**Compute cost.** Image transcoding is CPU-intensive but bounded. JPEG to WebP conversion at quality 75 takes approximately 10-50 ms per image on a modern server core, depending on image dimensions. AVIF encoding at speed 6 and quality 60 (the shipped `avif_quality` default) takes roughly 50-200 ms for a mobile-sized image of about 480 pixels wide, and longer for a full-size photograph. The proactive multi-format pipeline decodes the source image once and runs the encoders, so the total per-image compute is roughly 100-300 ms. A single core can process 200-600 unique images per minute. Since each image is only processed once per format variant (deduplication via cache existence checks), a 4-core VM handles steady-state traffic of thousands of unique images per hour -- see [measured optimization results](/blog/benchmarking-real-numbers/) for real-world numbers.

**Storage cost.** Each image produces up to 3 cached variants (WebP, AVIF, optimized original -- see [AVIF vs WebP](/blog/avif-vs-webp-2026/) for when each format wins), and where you serve more than one size, the mobile and desktop renditions come from [viewport-aware image sizing](/blog/viewport-aware-image-optimization/) rather than separate billed transformations. Measured at the shipped encoder defaults, a 161 KB JPEG photograph produced desktop variants of 54 KB as optimized JPEG, 36 KB as WebP and 46 KB as AVIF: about 136 KB of variants per source image, or 0.84x the original size. For a catalog of 10,000 such images, the desktop variant set is approximately 1.4 GB; each additional viewport size adds its own, smaller set. The Cyclone cache pairs a disk tier (`cache_size_bytes`, default 1 GB, production deployments typically use 1-10 GB) with a small in-memory tier for hot variants, and evicts least-recently-used entries when full.

**Software cost.** mod_pagespeed is licensed under Apache-2.0 and free to run in development and in production — every server behind the site, all optimizations, the module, the worker, and all updates. Support subscriptions are sold separately. In the model below the software line is flat (zero unless you add a support subscription) and it does not move with image traffic.

**Infrastructure cost.** A 4-vCPU / 8 GB cloud VM costs $30-60/month depending on provider. This same VM can run both nginx and the worker, or they can be separated onto dedicated instances for higher traffic.

**Total self-hosted cost is flat:** your VM ($30–60/month), plus a support subscription if you want one — and it does not rise with image traffic.

## Break-even analysis

The crossover point depends on your image request volume:

<figure>
  <img
    src="/images/economics-breakeven.svg"
    alt="Line chart comparing monthly image-optimization cost. A cyan CDN line rises linearly from $35 at 100K requests to $3,500 at 10M, while a flat green self-hosted line stays level as traffic grows. The lines cross inside a marked 100K-170K break-even band; above that volume, self-hosting is cheaper."
    width="720"
    height="420"
    loading="lazy"
  />
  <figcaption><em>CDN cost scales linearly with image requests (cyan); self-hosting stays flat (green). The lines cross in the ~100–170K req/mo break-even band — above it, self-hosting wins and the gap widens fast: at 10M requests the CDN bill is about $3,500/mo while the self-hosted cost has not moved.</em></figcaption>
</figure>

At 100,000 monthly image requests, the CDN costs roughly $35/month -- in the same range as a $30–60 VM. At 500,000 requests, the CDN costs approximately $175/month, well past the self-hosted cost. The crossover happens somewhere around **100,000-170,000 monthly image requests**. Beyond that, every additional request is essentially free with self-hosted optimization (just bandwidth to the client), while CDN costs continue to climb linearly.

At 10 million monthly requests, the CDN bill reaches $3,500/month -- roughly 60-100x the self-hosted cost. And unlike CDN pricing, the self-hosted cost does not change with traffic volume. A second server for redundancy adds only another VM -- the software is free to run, so the software line does not move -- and the total stays a fraction of CDN pricing at scale.

:::tip
To see which side of the line you fall on, put your own monthly image-request volume into the model above. Past roughly 100,000–170,000 requests a month, compare it against your current CDN bill, then [install in about five minutes](/docs/getting-started/) — Docker Compose, the Helm chart, or the NuGet middleware. Below that volume, a CDN is likely the cheaper call, and this post says so.
:::

## Hidden costs of third-party services

The sticker price of CDN image optimization understates the true cost of ownership. Several categories of hidden cost accrue over time.

**Data processing agreements.** If your site serves users in the EU, every image request routed through a third-party service constitutes a data transfer to that processor. You need a Data Processing Agreement (DPA) with the service, documented legitimate interest or consent, and periodic reviews of their data handling practices. The legal and compliance overhead is real -- typically 10-20 hours of legal review for initial setup and 5-10 hours annually for review.

**Vendor lock-in.** CDN image services use proprietary URL schemes and transformation APIs. Cloudinary URLs look like `res.cloudinary.com/demo/image/upload/w_300,c_fill/sample.jpg`. imgix uses `images.example.com/photo.jpg?w=300&fit=crop`. Migrating between providers means rewriting every image URL in your templates, CMS, and API responses. Self-hosted optimization uses the original URLs -- no rewriting needed, no lock-in. With mod_pagespeed the variant is chosen by a capability mask on the original request, not a proprietary URL transform, so there is nothing in your markup to rewrite on the way in and nothing to unwind on the way out.

**Latency overhead.** Routing images through a third-party service adds a DNS lookup, TCP connection, and TLS handshake to every uncached request. Even with edge caching, the first request to each edge node incurs a round-trip to the transformation origin. mod_pagespeed serves optimized variants directly from the origin's local Cyclone cache with zero network hops for cached content.

**Service availability risk.** Your site's image performance becomes dependent on the third party's uptime. If their transformation pipeline has an outage, your images may load slowly (falling back to originals) or not at all (if your URLs are rewritten to point at their infrastructure). Self-hosted optimization degrades gracefully by design: if the worker is down, nginx serves the original from cache, and the worker catches up when it recovers.

## When self-hosted makes sense

[Self-hosted image optimization](/blog/self-hosted-image-optimization/) is the right choice for:

**Organizations with data sovereignty and GDPR requirements.** Government, healthcare, financial services, and EU-based companies that cannot or prefer not to route user content through third-party infrastructure. mod_pagespeed keeps all data on your servers -- images are decoded, transcoded, and served without ever leaving your network.

**High-traffic sites where per-request pricing is prohibitive.** E-commerce catalogs, media sites, and marketplaces with millions of image requests per month. The flat cost model makes budgeting predictable: adding servers to absorb traffic adds VM cost, nothing else.

**Teams with existing nginx infrastructure.** If you already run nginx as your reverse proxy or load balancer, mod_pagespeed fits the same model: it deploys as its own nginx reverse proxy in front of your origin, bundling an async worker and nginx that share a single cache file — minimal architectural change for a team that already speaks nginx.

**Companies that value operational independence.** No API keys to manage, no vendor relationship to maintain, no surprise billing, and no dependency on external service availability for a core part of your site's performance.

### Self-hosted does not mean DIY ops

The standard objection to self-hosting images is that it only pays off at large scale, once you have a platform team to run a [Thumbor](/alternatives/thumbor/) or [imgproxy](/alternatives/imgproxy/) deployment. That is true for Thumbor and imgproxy. It is not how mod_pagespeed works.

Thumbor and imgproxy are standalone image-processing servers. You route traffic through them, rewrite every image URL to a proxy prefix or query string, and build the content-negotiation layer yourself. That is the ops burden critics cite, and it is real for those tools.

mod_pagespeed is a Docker / nginx reverse proxy — the prebuilt `ngx_pagespeed_module.so` ships inside the image — plus an async worker process. You put it in front of the origin you already run. There is no separate transform service to route URLs through, no URL rewriting, and no API keys. Images are optimized on their original URLs: on a cache miss the module writes and serves the original itself, keyed by the request URL, and the worker fills in WebP, AVIF, and an optimized original against that same key. The right variant is chosen per request from the browser's `Accept` header, viewport, pixel density, and `Save-Data` -- content negotiation you would otherwise build by hand around a transform proxy.

Setup is [Docker Compose in about five minutes](/blog/run-with-docker-compose/). Public Docker images on `ghcr.io`, a published Helm chart, and a NuGet package cover the packaging a DIY stack leaves to you. If you are weighing tools, [measure it against the alternatives](/blog/mod-pagespeed-alternatives/) rather than assuming the imgproxy ops profile applies.

CDN-based solutions remain the better option for small sites with low traffic (under roughly 100,000 image requests/month), teams without server administration expertise, and globally distributed audiences where edge-based transformation provides latency benefits that origin-based processing cannot match. Some teams use both: mod_pagespeed at the origin for optimization and a CDN in front for global distribution.

## Frequently asked questions

**Is self-hosted image optimization cheaper than Cloudinary or imgix?**

Below roughly 100,000–170,000 image requests per month, a CDN like Cloudinary or imgix is usually cheaper, because per-unit rates are low at small volumes. Above that break-even, self-hosted mod_pagespeed wins: its cost is flat regardless of request volume, while CDN bills keep climbing with traffic, reaching about 60-100x the self-hosted cost at 10 million requests per month.

**Why do Cloudinary and imgix bills jump on renewal?**

Both bill against consumable credits that scale with transformations and delivery. As your catalog and traffic grow, credit consumption grows with them and renewal quotes rise. Some plans restructure tiers or apply quota cutoffs rather than overage billing, so you can hit a hard cap instead of a predictable overage. A self-hosted model has no usage-based input, so the bill does not move with traffic.

**Is mod_pagespeed a DIY tool like Thumbor or imgproxy?**

No. Thumbor and imgproxy are image-processing servers you wire into your stack with URL rewriting and API calls, which is the ops burden critics cite. mod_pagespeed is an nginx reverse proxy plus an async worker: Docker Compose in about five minutes, no URL rewriting, no API keys. It optimizes images on their original URLs automatically.

**Does self-hosting images help with GDPR and data sovereignty?**

Routing image requests through a third-party CDN is a data transfer to a processor, which typically means a data processing agreement (DPA) and ongoing compliance review. mod_pagespeed decodes, transcodes, and serves images entirely on your own infrastructure, so user content never leaves your network -- which is what GDPR and data-residency requirements are usually about.

**Are there bandwidth or per-transformation fees with self-hosted optimization?**

No. There are no per-transformation, per-image, or vendor bandwidth fees. The software is licensed under Apache-2.0 and free to run; you pay for your own VM and the bandwidth your host already charges. Past the break-even point, additional image requests cost essentially nothing beyond that baseline host bandwidth.

**What happens to my images if the optimization service goes down?**

With a rewriting CDN, an outage can slow or break image loading across the pages that depend on it. mod_pagespeed degrades gracefully: if the worker is down, nginx serves the original image from cache and the worker catches up on recovery, so images never fail to load.
