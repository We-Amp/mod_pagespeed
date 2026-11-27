---
title: 'How the metadata cache avoids re-optimizing'
description: "How mod_pagespeed avoids re-optimizing on every request: a metadata cache keyed by the output URL maps to the answer and skips rewrites that don't shrink."
order: 20
datePublished: 2026-06-13
lastUpdated: 2026-10-22
---

A web optimizer faces an awkward bootstrapping problem. To rewrite a `<link>` or `<img>` tag, it has to know the optimized resource's URL. But that URL carries a [content hash](/blog/content-hash-urls/) computed from the optimized bytes — and you cannot compute those bytes without doing the full optimization. So on the surface, every page load looks like it requires re-optimizing every asset just to learn where the optimized version lives.

That is the problem the metadata cache solves. It is the lookup table that lets the second request to a page skip almost all of the work the first request did. This page explains what it stores, why each piece is there, and how it maps onto the two cache tiers in mod_pagespeed.

## The problem: you can't name the output without producing it

Walk through optimizing a stylesheet. The optimizer [minifies `styles.css`](/how-it-works/css-parsing/), hashes the minified bytes, and writes the result to a content-hashed URL like `styles.css.pagespeed.ce.GhT8kP2mNq.css`. The hash is part of the filename, so the server can serve that URL for a year and never worry about a stale cache. The companion article on [content-hashed URLs](/blog/content-hash-urls/) covers why that property is worth so much.

Now the same page is requested again. The optimizer needs to write `styles.css.pagespeed.ce.GhT8kP2mNq.css` into the HTML again. But the only way it knows that filename is by hashing the minified output — which means minifying again. The content hash that makes the URL durable also makes the URL unknowable in advance. The content-hashed URL is the answer; the metadata cache is what remembers the answer so you don't have to recompute it.

## The lookup table: keyed by the output URL minus the hash

The fix is a second, separate cache. It is small, it is keyed by what the server already knows on the second request, and it returns the part the server cannot know without recomputing.

The key is the filter that ran plus the output URL with the hash and extension stripped off — essentially "the cache-extend filter, applied to `styles.css`." That is information available the instant the optimizer sees the original tag, before any work happens. The value is the missing piece: the content hash, and the final extension.

The extension matters because optimization can change formats. A GIF rewritten to PNG, or a JPEG transcoded to WebP, lands at a different extension than it started with. The metadata entry records which one, so HTML rewriting produces a URL that actually resolves.

Two properties make this cheap:

- **The entry is tiny.** It holds a hash, an extension, and a little bookkeeping — bytes, not kilobytes. The optimized resource itself can be large; the pointer to it is not.
- **It never touches the input bytes.** A metadata hit does not read or re-fetch the original asset. The optimizer rewrites the page's HTML straight from the cached entry, and the actual optimized bytes are served separately, on demand, when the browser requests that content-hashed URL.

Validity is tied to the inputs. The metadata entry inherits the freshness lifetime of the resource (or resources) it was built from. When an input's TTL runs out, the entry is treated as stale and the optimizer revalidates before trusting it again. So a deploy that changes a stylesheet does not get served the old hash indefinitely — the entry expires on the same clock as its source.

## The optimizable bit: remembering what not to bother with

A lookup table for successful rewrites is only half the story. Consider an image the optimizer tries to recompress and finds it cannot shrink — it is already near-optimal. Without memory, the optimizer would attempt that losing recompression on every single request, burning CPU to reach the same conclusion each time.

So the metadata entry also stores a single bit: was this resource worth optimizing at all? If a rewrite produced nothing meaningfully smaller, that fact is recorded. On the next request the optimizer reads the bit, sees there is no win to be had, and leaves the original reference in place without re-running the codec. This is the negative-result cache, and it is governed by the same expiry rules as everything else — when the entry goes stale, the optimizer gets one more chance to try again, in case the source changed.

This behavior is the same in principle across the product line. The image-filter [design background](/docs/image-filters/) states it directly: a rewrite is kept only when the output is smaller, and the optimizer remembers a losing result so it does not re-attempt it on every request. (Early builds encoded this as an `X-ModPagespeed-Unoptimizable` marker on the cache entry; that was an implementation detail of the time, not a header you configure today. The behavior is what carries forward, not the mechanism.)

## Remembered metadata: keeping megabytes of image data out of HTML rewriting

There is a subtler reason the metadata cache exists. The whole point of rewriting HTML from a small lookup table is to avoid loading large resources into memory just to edit a page. But some filters need facts that, naively, would require reading the resource.

Two examples make this concrete:

- **Image dimensions.** To add `width` and `height` to an `<img>` tag, or to resize an image to the size it is actually displayed at, the optimizer needs the image's pixel dimensions. Those come from decoding the image — exactly the megabytes-of-data load the metadata cache is supposed to avoid during HTML rewriting.
- **Inlined small images.** When an image is small enough to embed directly in the page as a `data:` URI, the optimizer needs the encoded data URI itself, again something derived from the image bytes.

The metadata entry solves this by carrying a small key/value store. When a filter decodes an image once, it records the dimensions, and — if the image qualifies for inlining — the data URI, right in the metadata entry. Every subsequent HTML rewrite reads those values back from the small entry and never re-decodes the image. The expensive work happens once; the cheap lookup happens on every request after that.

## How this maps onto two cache tiers

Both parts split caching into a metadata tier and a data tier, but the process model differs, and the difference is worth getting right.

**The optimizer worker** optimizes in a [separate process](/how-it-works/async-rewriting/) behind nginx. Both nginx and the worker open one [Cyclone cache](/docs/cache-modes/) volume file with memory-mapped sharing, so a write from the worker is immediately visible to nginx. On a request, nginx classifies the client into a 32-bit capability mask — image format support, viewport, pixel density, and a few other signals — and uses it as part of the cache key to find the right optimized variant. That mask is the modern, generalized form of "remembered dimensions": instead of one cached output per resource, the cache holds the variant matched to each class of client, and nginx serves it directly from the memory-mapped file with an `X-PageSpeed: HIT` header. No origin round-trip, no copy. The first request to a fresh page returns `X-PageSpeed: MISS` while the worker optimizes in the background; subsequent requests get the cached variant.

**The module** runs in-process inside the web server. It uses [Cyclone Cache](/docs/cache-modes/#native-module-cache-storage) as its data tier by default, fronted by a shared-memory metadata cache that all server processes share, plus a small per-process LRU for the hottest small entries. The split is the same idea — small lookup data kept close, large optimized bytes kept in the backing store — implemented for an in-process module rather than a separate worker. In v1.15.0+r17 and later, the shared-memory tier also writes through to Cyclone, so metadata and page properties survive a restart.

For how the Cyclone data tier compares to the classic file-per-entry cache it replaced — measured across concurrency, latency tails, and eviction on fast and realistic storage — see the [Cyclone vs. the file cache benchmark](/blog/cyclone-cache-vs-file-cache-benchmark/).

In both cases the metadata tier is the small, hot lookup this page has described, and the data tier holds the actual optimized bytes behind their content-hashed URLs. The deadline that governs the first request — serve the original now, finish optimizing, cache the result for next time — is `RewriteDeadlinePerFlushMs` in the module; the optimizer worker gets the same effect by optimizing off the request path entirely. Either way, the metadata cache is what makes "next time" fast.

## The shape of a second request

Put together, here is what the second load of a page costs:

1. The optimizer sees an original tag and builds the metadata key from the filter and the original URL.
2. A small metadata lookup returns the content hash, the final extension, and — for images — the remembered dimensions or inline data.
3. The HTML is rewritten from those values. No input bytes are read.
4. If the optimizable bit says a resource was not worth rewriting, it is left alone with no codec work.
5. The browser later requests the content-hashed URL, and the data tier serves the optimized bytes.

The expensive part — minifying, transcoding, decoding, measuring — happened once, on the first request. Everything after that is table lookups against entries measured in bytes.

## Where the design came from

The two-cache split, the optimizable bit, and the remembered-metadata store are part of the original design of the Apache PageSpeed project, where We-Amp was an initial committer alongside the original Google engineers, who wrote much of the early caching design. The mechanics have been rebuilt and the data tier replaced — Cyclone in both products, a separate worker and capability-mask variants in 2.0 — but the core insight has held up well: keep the thing you look up on every request small and separate from the thing you produce once.

If you want to see the metadata cache do its job, install the optimizer and watch the `X-PageSpeed` header flip from `MISS` to `HIT` on the second request, or compare image sizes once the worker has run. The [getting-started guide](/docs/getting-started/) walks through it; the optimizer is licensed under Apache-2.0 and free to run. For the companion piece on why the optimized URLs carry a hash in the first place, see [content-hashed URLs](/blog/content-hash-urls/); for how the variant matrix is built per client, see [viewport-aware image optimization](/blog/viewport-aware-image-optimization/).

_The caching design described here originates with the Apache PageSpeed project (Google, 2010–2018; later Apache-incubated), released under the Apache License 2.0. mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with Google and maintains mod_pagespeed independently._
