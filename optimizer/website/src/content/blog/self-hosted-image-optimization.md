---
title: 'Self-hosted image optimization vs image SaaS egress'
description: 'Image SaaS bills per transformation and per GB of egress. Self-hosted optimization transcodes WebP/AVIF on your servers: no per-transformation cost.'
date: 2026-06-06
lastUpdated: 2026-10-22
tags: ['image-optimization', 'self-hosted', 'cloudinary', 'performance']
product: '2.0'
draft: false
---

An image SaaS bill starts with a small base fee. As traffic grows, the line items that scale with it grow too: transformations, stored derivatives, and GB of egress. Your traffic went up and the bill went up faster. That is how the pricing is designed to work.

Self-hosting image optimization removes the part of the bill that moves with traffic. You decode each image once on your own server, write the optimized variants to a local cache, and serve them from there. No per-transformation meter, no third party counting bytes on the way out.

## Why image-SaaS bills scale with traffic

Image CDNs and SaaS transformation services price on the two inputs that grow with success:

- **Per-transformation fees.** Every resize, format conversion, or quality change is a billable transformation. A catalog of 10,000 images served as WebP, AVIF, and optimized original across two viewport sizes is 60,000 transformations on the first pass, and more as products change and cached derivatives expire.
- **Egress.** Optimized bytes still leave someone's network to reach the browser. Whether it shows up as a per-GB delivery charge or as "credits" consumed, the bill rises with traffic.

The [full cost model is in a separate post](/blog/economics-of-image-optimization/). A mid-sized catalog at roughly a million image requests a month lands near $350 on the median CDN model, and a high-traffic site at ten million requests near $3,500. What matters is less the absolute figure than the slope. The figure tracks your traffic, and self-hosting flattens it.

## What self-hosting buys you

A third-party transformation service can't structurally offer any of the following.

**No third party in the request path.** The image is decoded, transcoded, and served without leaving your network. For a site serving EU users, that removes a data processor from the request path, and a Data Processing Agreement and audit surface with it. The bytes a browser receives came from your server.

**Flat, usage-independent cost.** The software is free to run — your cost is the VMs it runs on, plus a support plan if you want one. Past a break-even point of roughly 200,000–500,000 image requests a month, an additional request costs nothing usage-based — only the bandwidth you would pay your host regardless. A second server for redundancy adds a VM, nothing else.

**Your URLs, your headers, your cache policy.** CDN image services use proprietary URL schemes — `res.cloudinary.com/demo/image/upload/w_300/sample.jpg`, `images.example.com/photo.jpg?w=300`. Migrating off one means rewriting every image URL in your templates, CMS, and API responses. Self-hosted optimization serves the original URL. You set the `Cache-Control`, you set `Vary`, and nothing in your markup has to change.

**Graceful degradation you control.** If the optimization worker is down, nginx serves the original from cache and the worker catches up when it recovers. No external pipeline can turn your images slow or blank when it has an outage.

## How it works: one decode, many variants

mod_pagespeed runs the optimizer as an async worker behind nginx (or as ASP.NET Core middleware). The request path stays fast; the work happens out of band.

When nginx records a cache miss, it sends a fire-and-forget notification to the worker over a Unix socket. The worker reads the original from the shared cache, decodes it once, and generates every missing variant from that single pixel buffer. A 10-megapixel JPEG decodes to roughly 40 MB of raw pixels. Decode it once and run the encoders, instead of decoding it again for every format and size you want.

The variants come from a small set of dimensions that actually matter for image serving:

- **Format.** WebP and AVIF, with an optimized variant of the original as the fallback. AVIF now renders in every major browser — Chrome, Edge, Firefox, Opera, and every Safari since 16.4 — and sits above 94% global support in 2026, [typically 20–40% smaller than the equivalent WebP](/blog/avif-vs-webp-2026/). WebP has been universal since 2020 and covers the rest. The worker reads the client's `Accept` header and serves the best format that client can decode.
- **Viewport-aware sizing.** A phone does not need a 2400px-wide hero. The worker resizes per viewport class, so a mobile visitor gets a mobile-sized image. The [viewport-aware sizing post](/blog/viewport-aware-image-optimization/) goes into how the class is derived.
- **Save-Data.** When the browser sends `Save-Data: on`, the user has explicitly asked for less data. The worker serves a more aggressively compressed variant. That is [a separate quality tier, not a guess](/blog/save-data-bandwidth/).

Each of those is a bit in a 32-bit capability mask that nginx computes per request and uses as the cache key. The mask selects the best-fit variant on a hit; on a miss it picks the closest available and degrades toward the original. The browser asks for `/images/hero.jpg` the whole time — the right variant comes back without a single URL change in your HTML.

## The tradeoffs

Self-hosting moves the cost from a per-request meter to operational responsibility. Two things land on you.

**You run the server.** There is a VM to provision, a cache size to set, and updates to apply. If you have no one to administer a server, a managed service is the lower-effort choice, and that is a fair trade. In return: no API keys to rotate, no vendor relationship, no billing surprise.

**No global PoPs.** An image CDN transforms and serves from edge nodes physically close to the user. A single origin server does not. If your audience is globally distributed and first-byte edge latency is your bottleneck, origin-based optimization cannot match a CDN on its own.

The two approaches work together. The common pattern is to run mod_pagespeed at the origin for the optimization — decode-once transcoding, the variant matrix, the data staying put — and put a plain CDN in front for distribution. The CDN caches and serves the already-optimized variants from the edge. You get edge delivery without paying a transformation service to do work your origin already did once per image.

## Where the break-even line sits

The break-even depends on your request volume and how globally distributed your audience is. If you are under a few hundred thousand image requests a month and your users are spread across continents, an image CDN is often both cheaper and faster, and the economics post says so. Above the crossover, or when the content has to stay on your infrastructure, the free, self-hosted model comes out ahead and stays there as traffic grows.

For a number based on your own traffic rather than a median, the [cost calculator](/calculator/) takes your request volume and shows where the two curves cross. For a feature-and-pricing comparison against the services people evaluate most, see [mod_pagespeed vs Cloudinary](/alternatives/cloudinary/) and [vs imgix](/alternatives/imgix/). For the product page — what it does to each image, the variant matrix, and how to turn it on — see [self-hosted image optimization](/self-hosted-image-optimization/).

Self-hosted image optimization will not give you a CDN's edge footprint, and it will not fix a slow application or a slow database. It takes the part of your image bill that scales with traffic and makes it flat, while keeping every byte on your own origin.
