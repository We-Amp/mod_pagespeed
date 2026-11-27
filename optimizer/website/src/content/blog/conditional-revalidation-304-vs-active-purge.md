---
title: '304 revalidation vs PURGE: which invalidation is cheaper'
description: 'When a 304 ETag round-trip beats PURGE and keeps optimized AVIF and WebP variants on an HTML-only deploy with no rebuild.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['caching', 'operations', 'performance', 'deep-dive', 'images']
draft: false
product: '2.0'
---
You ship an HTML-only deploy — a copy fix, a new banner, nothing under your hashed `/assets/` path changed — and then you fire a `PURGE` at every URL on the page, out of habit: the HTML *and* every image it references. The next page load is correct. It is also slower than it needed to be, because purging each image URL deleted that image's AVIF and WebP variants, and the proxy now has to re-optimize all of them from scratch. The HTML changed. The images did not. The HTML purge was fine — but you did not need to touch the image URLs at all, and doing so cost you every variant under them.

Cache invalidation has two halves. Purge is the loud one: it deletes, immediately and unconditionally, and the [single-URL purge post](/blog/single-url-cache-purge-optimizing-proxy/) covers what that actually fans out to inside an optimizing proxy. This post is about the quiet half — conditional revalidation, the 304 round-trip that refreshes a stale entry without re-downloading or re-optimizing anything. For routine deploys it is usually the cheaper tool, and for an HTML-only deploy it is the one that keeps your expensive image variants alive.

## What a 304 actually buys you

When mod_pagespeed caches a response from your origin, it stores the validators the origin sent: the `ETag`, the `Last-Modified` timestamp, or both. Those are the receipts. When the cached entry later goes stale — its `max-age` has elapsed — mod_pagespeed has a choice. It can re-fetch the resource in full, or it can ask the origin a narrower question: *has this changed since the version I'm holding?*

That narrower question is a conditional request. mod_pagespeed sends `If-None-Match` with the stored ETag, or `If-Modified-Since` with the stored timestamp. If the origin's copy is unchanged, it answers `304 Not Modified` with no body. mod_pagespeed marks its cached entry fresh again and serves it. No bytes crossed the wire beyond the headers, and — this is the part that matters for an optimizing proxy — no re-optimization happened, because the input bytes are provably the same ones already optimized.

In mod_pagespeed this is on by default:

```nginx
pagespeed_conditional_revalidation on;   # default: on
```

The docs are blunt about the alternative. Without conditional revalidation, `must-revalidate` degenerates into a full origin re-fetch on every stale request — which is why the cache-control guide gates its own recommendations behind "requires 2.0.x with conditional revalidation support." The header pattern only pays off if the revalidation underneath it is cheap.

## Why purge discards what revalidation keeps

This is the whole reason the two tools are not interchangeable on a deploy.

`PURGE` immediately invalidates *all* cached variants for a URL. That is its job, and for an urgent content correction it is exactly what you want. But "all variants" is a larger set than people expect. A single source image expands into many stored outputs — different formats ([AVIF, WebP](/blog/avif-vs-webp-2026/), fallback JPEG/PNG), viewport classes, pixel densities, a Save-Data variant. Purge the URL and every one of those is gone. The next request rebuilds them, and image transcoding is [the most expensive work the proxy does](/blog/economics-of-image-optimization/).

Conditional revalidation does not touch them. On a 304, the cached entry is refreshed in place: the stored optimized bytes stay exactly where they are, the validator's freshness window resets, and the variants survive untouched. The cache-control guide states the tradeoff directly — for routine deploys, let `must-revalidate` handle freshness, because "conditional revalidation is cheaper than purge-and-rebuild because it preserves optimized image variants (AVIF, WebP) when only the HTML changed."

Map that onto a real HTML-only deploy. Your HTML carries a short lifetime so it picks up edits quickly:

```http
Cache-Control: public, max-age=60, must-revalidate
ETag: "content-hash-or-version"
```

After the deploy, the HTML's stored ETag no longer matches origin, so its conditional request comes back `200` with the new body — correct, and cheap, because HTML is small and never gets transcoded. The images on that page never changed, their ETags still match, so they either stay fresh or revalidate with a `304`. Either way their AVIF/WebP variants are preserved. A blanket purge across the page would have been correct too, and slower, because it would have rebuilt every one of those variants to produce bytes identical to the ones it just deleted. That is the failure mode in the opening: paying to re-derive output that was already right.

## When purge is still the correct call

Revalidation is the default tool, not the only one. It has a hard precondition: it only works when the origin answers conditional requests correctly. If your origin returns `200` with a fresh ETag for content that genuinely changed, revalidation will faithfully refresh the entry — but it cannot detect a change the origin lied about. And if the origin *mishandles* conditional requests (returns the wrong status, or a stale ETag for new content), the cache-control guide's escape hatch is to turn the feature off:

```nginx
pagespeed_conditional_revalidation off;   # only if your origin mishandles conditionals
```

Reach for `PURGE` instead of waiting for revalidation when:

- **The change is urgent and the URL is stable.** A wrong price, a legal correction, a broken asset that must vanish *now* and cannot wait for `max-age` to lapse. Purge invalidates immediately; revalidation only acts when the entry next goes stale.
- **The content moved under a URL that did not change.** Content-hashed URLs invalidate themselves — change the bytes, the hash changes, the old URL is never requested again ([why optimized URLs carry a content hash](/blog/content-hash-urls/) covers that path). Purge is precisely for the case where the URL stays put and the content underneath it shifts.
- **You cannot trust the validators.** No ETag, no reliable `Last-Modified`, or an origin you have already caught mishandling conditionals. Without trustworthy validators there is nothing for revalidation to compare against, and purge is the fallback.

There is also the boundary case the cache-control guide flags: `max-age=0`. It does not disable caching — it forces a conditional revalidation on *every* request. That is an origin round-trip per hit even when nothing changed, so it belongs only on genuinely real-time content (stock tickers, live scores), not as a substitute for purge.

The two tools also differ in scope, which the cache-modes behavior makes concrete. Safe mode — the default — already stamps `must-revalidate` and uses short TTLs, so conditional revalidation is doing quiet work on every stale read whether you think about it or not. Aggressive mode trades that for long TTLs and fewer round-trips. (The TTL math behind each mode, and what longer freshness windows actually cost, is its own subject, covered in [cache-mode safety: must-revalidate vs aggressive](/blog/cache-mode-safety-must-revalidate-vs-aggressive/); this post stays on the purge-vs-revalidate cost decision.) Purge sits outside both: it is the manual override for when you cannot wait for the next stale read at all. Knowing which layer you are operating at is the difference between a deploy that re-optimizes everything and one that re-optimizes only what actually changed.

## Related

- [What Happens When You Purge One URL](/blog/single-url-cache-purge-optimizing-proxy/)
- [Cache-mode safety: must-revalidate vs aggressive](/blog/cache-mode-safety-must-revalidate-vs-aggressive/)
- [Why optimized URLs carry a content hash](/blog/content-hash-urls/)
- [Cutting TTFB at the server layer](/blog/reduce-ttfb-server-layer-2026/)
- [Self-hosted image optimization](/blog/self-hosted-image-optimization/)
- [Cache modes: safe vs aggressive](/docs/cache-modes/)
- [Set Cache-Control headers](/docs/cache-control/)

Revalidation is invisible when it works, which is the point — most of the savings come from a feature you never have to invoke. If you want to see it on your own origin, install mod_pagespeed; it optimizes out of the box, and the [web console flags URLs](/docs/cache-control/) where the origin sends no `Cache-Control` header at all, which is the usual reason revalidation cannot help you yet. The [download](/download/) is the place to start; it is licensed under Apache-2.0 and free to run in development and in production.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
