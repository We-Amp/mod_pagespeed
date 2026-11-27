---
title: 'Cache purge in an optimizing proxy: what one URL touches'
description: 'Purging one URL in mod_pagespeed 2 touches more than one cache entry: the fan-out problem, the 1.x lookup-time design, and why the worker deletes now.'
date: 2026-06-13
lastUpdated: 2026-10-22
tags: ["caching", "cache-invalidation", "architecture", "wordpress", "operations"]
product: "2.0"
draft: false
---
You change one stylesheet on your origin, fire a purge for its URL, and expect the next request to serve the new bytes. Simple. Except inside an optimizing proxy, "that URL" is not one thing in the cache. It is several. And some of what needs to go lives in the [metadata cache](/how-it-works/metadata-cache/), not under that URL at all.

This matters more now than it used to. The [WeAmp Cache Control for ModPageSpeed](/wordpress/) plugin calls the purge API every time you publish, update, or delete a post, so a purge fires on routine edits, not just the occasional manual one. That plugin is a [control plane, not a cache](/blog/wordpress-server-side-page-cache-plugin/) — it decides when to fire, but the proxy decides what the call reaches. It is worth knowing what that call actually does.

## One source URL, many cache entries

mod_pagespeed does not cache "the optimized version" of a resource. It caches whichever variant fits the request that asked for it.

Take `/images/hero.jpg`. A phone on a slow connection with Save-Data on gets a small AVIF. A desktop on a retina display gets a larger one. A browser that only speaks WebP gets WebP. mod_pagespeed encodes these differences in a 32-bit capability mask — image format, viewport class, pixel density, Save-Data, transfer encoding — and folds that mask into the cache key. One source image can expand into dozens of distinct stored outputs. The [configuration reference](/docs/configuration/) puts the ceiling at 37 distinct variants for a single image with all proactive generation enabled.

So a purge that only deleted the entry keyed on the bare URL would leave every variant behind, and the proxy would keep serving the old AVIF to phones while the desktop saw the new file. That is the first part of the fan-out.

The second part is subtler. A CSS file is not just cached as optimized CSS. Its URL also appears *inside other cached artifacts*. If `extend_cache` rewrote a reference to it into a content-hashed URL, that rewrite decision lives in the metadata cache — the small records that remember "for this input, here is the optimized output and its hash." HTML that inlined or combined the file carries its fingerprint too. Change the file and those downstream decisions are stale, even though none of them is stored under the file's own URL.

So the real question a purge has to answer is not "which entry has this key" but "everything that depends on these bytes, wherever it lives."

## Why 1.x could not just delete the keys

The obvious implementation is to walk the caches and delete every key that touches the URL. The Apache PageSpeed designers looked at that approach in mod_pagespeed 1.x and rejected it, for a reason that held up at the time. Each variant was stored under its own key, derived from the capability mask, so to delete a resource you would have had to reconstruct every metadata key that *might* reference it, across every capability-mask combination and every page that pulled it in. There was no clean way to enumerate them. The original notes called the deleting class "very tricky" and supporting wildcard patterns under it "hard, if not impossible."

So 1.x shipped lookup-time validation instead. A purge did not delete anything in the moment. It recorded an invalidation: a URL pattern plus a timestamp. On the next lookup, before any cached entry was trusted, it got checked against those records. An entry written before the relevant invalidation timestamp was treated as stale and re-optimized; one written after was served as-is.

That inverts the hard problem. Instead of finding every dependent entry up front, you let each entry prove its own freshness when it is next read. Stale variants and stale references fall away as they are touched, without anyone enumerating them. The cost moves to read time, where a timestamp comparison is cheap, and off the purge path, where the enumeration was intractable. This is the model the footer's 2012–2013 Apache PageSpeed work describes, and it is still the right account of how 1.x behaves.

## What changed in 2.0: a purge deletes immediately

mod_pagespeed does not work that way. The enumeration problem that forced 1.x into lookup-time validation no longer exists, because 2.0 changed how variants are stored. All the variants of a resource — every format, viewport, density, Save-Data, and encoding combination — plus the internal sentinel records for the URL live as alternates under a *single* cache key, not one key per mask. [Cache key derivation and alternate fallback](/blog/cache-key-derivation-and-alternate-fallback/) walks through how that key is built and how the variants hang off it; [sentinel cache keys](/blog/sentinel-cache-keys-and-103-early-hints/) covers the reserved records that ride along under the same key.

Once the whole chain lives under one key, there is nothing to enumerate. A purge in 2.0 is a single synchronous `remove_sync(key)` against that key, which drops every variant and every sentinel for the URL in one operation. There is no invalidation record and no timestamp comparison at read time: the entry is gone the moment the purge returns, and the next request rebuilds it from origin.

There is one more piece, for correctness under concurrency. A 2.0 worker can be building a fresh variant in the background when a purge lands, and a naive delete could let that in-flight write resurrect pre-purge content. So the worker does two things atomically under one lock: it bumps a per-URL *purge generation*, then it removes the key. A background write that started before the bump is fenced out when it tries to commit — it sees the newer generation and is rejected rather than writing stale bytes back. The nginx side reads the generation too, and treats a generation change as a purge or reset, so it stops trusting whatever it was holding. The delete handles the common case; the generation bump closes the race.

So when this post talks about purge in 2.0, picture an immediate deletion, not a deferred check. The sections below on which entries a purge reaches, and on falling back to a full flush, are about *what* a purge has to cover; in 2.0 that coverage comes for free from the single-key layout rather than from a lookup-time invalidation set.

## Strict versus reference: HTML and resources differ

The 1.x design drew a line here that still describes what a purge needs to reach, whichever version you run.

When you purge an **HTML page**, the only thing to invalidate is that page's own cached entry. The page references resources, but those resources are not wrong just because the HTML changed. This is the strict case: invalidate this entry, and nothing else. Narrow on purpose, because nuking unrelated metadata on every HTML edit would throw away optimization work for no reason.

When you purge a **resource** — a stylesheet, a script, an image — the entry itself is not the whole story. Every cached decision that *references* that resource is now suspect: the metadata that recorded its rewrite, and any HTML that embedded its fingerprint. The 1.x reference case reached for exactly that, with a URL-pattern invalidation timestamp wide enough to mark the referencing metadata stale too. In practice you rarely have to lean on it, because the references are content-hashed: change the resource and its hash changes, so the old hashed URL is never requested again and the stale rewrite decision simply dies of disuse (the [content-hash URLs](/blog/content-hash-urls/) post covers that self-healing path).

The original authors framed it as a choice between invalidating "that entry only" and invalidating "all potential references to the resource." That distinction is the whole reason a single purge call can do the right thing for a CSS file and the right thing for an HTML page without you telling it which is which.

## The bounded purge set, and why it would rather over-flush

This part is specific to the 1.x lookup-time model, and it is worth understanding because it shows the same correctness instinct that survives into 2.0. Lookup-time validation needs somewhere to keep the invalidation records, and that store cannot grow forever. A site that purges thousands of URLs a day would otherwise accumulate an unbounded list that every lookup has to scan.

1.x solves this with a purge set that is capped in size. It holds individual URL invalidations up to a limit. When it would overflow, it does not silently drop the oldest entries — that would mean serving stale content for a URL someone explicitly asked to purge. Instead it collapses to a single global invalidation timestamp: everything cached before now is stale. The set chooses to over-invalidate, never to under-invalidate. A spilled purge set costs you some re-optimization work; the alternative would cost you correctness, which is the one thing a purge cannot get wrong.

2.0 does not keep a purge set at all — it deletes the key outright, so there is no list to bound. But the fallback survives: a full cache flush still exists alongside per-URL purge in both versions. The targeted purge handles the common case; the global flush is the floor either design falls back to rather than ever losing a request.

## What this means in practice

A few consequences worth holding onto:

- **Purge is per server.** A purge only acts on the cache of the node that received it — in 1.x it records the invalidation there, in 2.0 it deletes the key there. Either way, in a multi-server deployment you have to send the purge to every node. mod_pagespeed does not propagate purges across servers — the [1.1 caching docs](/docs/cache-modes/#multi-server-deployments) state this directly, and it holds for 2.0. Front a fleet with a small fan-out script or a config-management hook.
- **You often do not need to purge at all.** Assets under content-hashed URLs invalidate themselves: change the bytes, the hash changes, the old URL is simply never requested again. [Why optimized URLs carry a content hash](/blog/content-hash-urls/) covers that path. Purge is for the cases where the URL stays put and the content moves underneath it.
- **For routine deploys, [conditional revalidation](/blog/conditional-revalidation-304-vs-active-purge/) is usually cheaper than purge.** It keeps optimized variants when only the HTML changed, instead of forcing a rebuild. The [Cache-Control guide](/docs/cache-control/) walks through when to reach for which.

One thing the original authors floated but never built into the open-source product: a publish/subscribe channel to push invalidations across a cluster automatically. That was a hosted-platform idea. In the shipping product, propagation is your job, which is the tradeoff for a self-hosted proxy with no central coordinator.

The [features overview](/features/) has the full picture.

---

*The cache-invalidation design in this post descends from the original mod_pagespeed work in the Apache PageSpeed project — Srihari Sukumaran's "Cache invalidation of URL patterns" (2012), Ilya Grigorik's "Cache Purge API" (2013), and Joshua Marantz's "Flushing individual cache entries" (2013), released under the Apache License 2.0. We-Amp was an initial committer on that project alongside the original Google engineers. mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with Google and maintains the open-source project independently. The 2.0 re-architecture was an independent rebuild.*
