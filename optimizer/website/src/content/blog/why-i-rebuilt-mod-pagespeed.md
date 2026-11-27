---
title: 'Why I rebuilt mod_pagespeed from scratch'
description: 'The story behind ModPageSpeed 2.0: why mod_pagespeed stopped being developed, what was kept from PSOL, and how a new architecture replaced RewriteDriver.'
date: 2026-02-01
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['architecture', 'announcement']
product: '2.0'
draft: false
---

I maintained mod_pagespeed for years. It was a remarkable piece of engineering — a full-stack web optimization system that could rewrite HTML, compress images, minify CSS and JavaScript, defer loading, combine resources, and a dozen other things. Google built it, open-sourced it, and then moved on. (If you arrived here asking whether [mod_pagespeed is still maintained](/mod-pagespeed-still-maintained/), the short answer is that the original is not, and this is the story of what replaced it.)

## The mismatch

At the heart of mod_pagespeed sits RewriteDriver: 2,000+ lines of C++ that orchestrate 60+ filters through a sophisticated pipeline. Each filter transforms the response in sequence — rewrite image URLs, inline small CSS, defer JavaScript, collapse whitespace. The filters interact in subtle ways. Some depend on ordering. Some conflict. The configuration matrix is enormous. It is genuinely impressive engineering — the kind of system that takes years and a full team to get right.

For Apache, this design was a natural fit. The module hooked into Apache's output filter chain, and the integrated architecture made sense when a single process handled everything and HTTP/1.1 was the only game in town.

But the web moved to nginx, and nginx is fundamentally different — event-driven, non-blocking, built on assumptions that don't align with synchronous filter pipelines. The ngx_pagespeed port bridged this gap, but the architectural distance between Apache's model and nginx's meant every improvement required working against the grain.

The scope of the system — the filter interdependencies, the configuration surface, the Apache-native design — was hard to evolve at the pace the nginx world needed.

## The decision

I decided to replace RewriteDriver and the filter pipeline with a different architecture.

Not the whole codebase — the team that built mod_pagespeed wrote excellent low-level libraries. The image optimization code is proven across billions of pages. The CSS and JavaScript minification works. The HTML parser handles real-world markup correctly. These components were built by engineers who understood the edge cases. They handle malformed input, quirks-mode documents, and the kind of broken HTML that exists on the real web.

What I replaced was the orchestration layer — the part that decides what to optimize, when, and how to stitch it all back together. RewriteDriver, the filter pipeline, the resource manager, the cache coordination. The optimization libraries underneath are the foundation of 2.0.

## What was kept

The low-level [PSOL (PageSpeed Optimization Libraries)](/psol/) components that do the actual work:

- **Image optimization** — libjpeg-turbo for JPEG, libpng for PNG, libwebp for WebP encoding/decoding, giflib for GIF (including animated), and optipng for lossless PNG reduction. These libraries are mature and well-understood.
- **CSS minification** — Whitespace removal, comment stripping, trailing semicolon removal, and decimal optimization. Straightforward transforms that don't change semantics.
- **JavaScript minification** — Conservative minification that removes whitespace and comments without renaming variables. Deliberately conservative.
- **HTML whitespace handling** — The HTML parser that correctly handles `<pre>`, `<script>`, `<style>`, and CDATA sections.

These components do one thing each and do it well. The question was whether they could be coordinated differently — outside the request path instead of inside it.

## The new architecture

ModPageSpeed 2.0 separates the system into three components:

**1. Nginx Interceptor** — A C++ nginx interceptor that classifies each request into a 32-bit capability mask. The mask encodes what the client supports: image format (WebP, AVIF, SVG), viewport class (mobile, tablet, desktop), pixel density, Save-Data preference, and transfer encoding. The interceptor looks up the URL in the cache and selects the best-fit variant based on this mask, serving the result directly via `mmap`. No copying, no allocation, no processing in the request path.

**2. Cyclone Cache** — A variant-aware disk cache that stores multiple variants of the same resource as alternates under a single URL key. Each alternate is identified by its capability mask. Lookups use best-fit fallback: if there's no exact match for this client's capabilities, the selector picks the closest available variant, degrading gracefully until it hits the original. The cache file is shared between nginx and the worker via memory-mapped I/O.

**3. Worker** — A C++ worker that does the actual optimization work. When nginx records a cache miss, it sends a fire-and-forget notification to the worker over a Unix socket. The worker reads the original content from the shared cache, runs the appropriate optimization (image transcoding, CSS minification, etc.), and writes the optimized variant back at the client's capability mask. The next request from a similar client gets a cache hit.

This separation means nginx never blocks on optimization. The first request always gets the original content (fast, from cache). Optimized variants [appear asynchronously](/how-it-works/async-rewriting/) as the worker processes them. Subsequent requests from similar clients get the optimized version.

While nginx powers the caching proxy internally, the optimization worker and cache are server-agnostic. In reverse-proxy mode, the mod_pagespeed module fronts any HTTP origin — Apache, Node.js, Caddy, or anything else that speaks HTTP. The Docker Compose setup takes a `BACKEND_HOST` and `BACKEND_PORT`, and the rest is automatic.

## Why self-hosted matters

The trend in web performance is toward third-party proxies — CDN-based optimization, edge workers, SaaS image APIs. These work, but they come with trade-offs that matter for certain organizations.

When your traffic flows through someone else's infrastructure, your data is on their servers. For companies subject to GDPR, HIPAA, or data residency requirements, this creates compliance overhead. You need to audit their data processing, negotiate DPAs, and hope their infrastructure stays in the right jurisdictions.

mod_pagespeed runs on your servers. Your content never leaves your infrastructure. There is no third-party data processor in the request path, which means no DPA to negotiate and no external audit surface for that hop.

This also means no per-request pricing, no bandwidth fees, no API rate limits, and no bill of any kind — the software is free under the Apache License 2.0, and your optimization scales with your hardware, not a subscription tier. For the image pipeline specifically, this is what [self-hosted image optimization](/self-hosted-image-optimization/) looks like in practice — the transcoding runs on your own box, not a vendor's.

## Getting started

mod_pagespeed is available now. Deploy with Docker Compose, point it at your origin, and it optimizes out of the box. Setup takes about five minutes regardless of which web server you run behind it. See [pricing](/pricing/) and [license terms](/license/).

If you're running a site where performance matters and you want to keep your data on your own infrastructure, install it and run it — it's free under the Apache License 2.0, in development and in production. Paid support is available if you want it: see [support pricing](/pricing/).
