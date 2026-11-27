---
title: 'Proactive variant generation: warming the cache for hot URLs'
description: 'How the mod_pagespeed worker warms hot URLs: nginx flags a hot URL and the worker pre-builds the whole image variant matrix instead of one per request.'
date: 2026-06-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['caching', 'architecture', 'images', 'performance']
draft: false
product: '2.0'
---

Lazy optimization has a real weakness: it only ever builds the variant the request in front of it asked for. The first visitor to a hot image is on a [Save-Data](/blog/save-data-bandwidth/) phone, so you encode one AVIF at mobile width and 1x density. The next visitor is on a 2x desktop with no Save-Data, and that variant is cold, so they get a fallback and a re-notify. The optimizer worker handles cache warmup variant generation differently: once nginx decides a URL is hot, it sends a warmup signal to the worker, which generates the whole format/viewport/density/Save-Data matrix proactively instead of waiting for each combination to be demanded one request at a time.

This post is about *when* and *why* variants get built ahead of demand. The mechanics of which variants exist live in [viewport-aware image optimization](/blog/viewport-aware-image-optimization/); how the cache key for each variant is built lives in [cache key derivation and alternate fallback](/blog/cache-key-derivation-and-alternate-fallback/). Here the question is the warmup trigger.

## What "lazy" leaves cold

Each image variant is stored as a Cyclone alternate, identified by the low 8 bits of a 32-bit capability mask. That mask packs image format (bits 0-1: Original/WebP/AVIF/SVG), viewport class (bits 2-3: Mobile/Tablet/Desktop), pixel density (bit 4), Save-Data (bit 5), and transfer encoding (bits 6-7). The full raster matrix for one image is 3 formats x 3 viewports x 2 densities x 2 Save-Data modes, which is 36 alternates under one cache key.

Under pure lazy optimization, a single request fills exactly one cell of that matrix. The cells nobody has asked for yet stay empty. For a long-tail URL that nobody hits twice, that is the correct behavior — you never waste an encode on a variant no client will ever request. The cost lands on URLs that *are* hot: every distinct client capability profile that arrives before its variant exists takes a fallback serve and triggers a re-notify, until the matrix fills in by attrition.

The warmup path exists to fill that matrix in one shot for exactly the URLs where it pays off.

## The hot threshold and the warmup sentinel

The decision that a URL is hot is made in nginx, on the cache-miss-and-fallback path. When a request finds no exact variant match and the worker is notified, the module also counts the URL. From `ngx_pagespeed_module.cc`, hot-URL tracking is a fixed-size hash map keyed by an FNV-1a hash of the URL (the map holds 4096 entries with probabilistic eviction). The count increments on fallback hits — not on revalidation re-notifications — and when it reaches the configured threshold, nginx sends a separate notification:

```cpp
if (c == conf->hot_threshold) {
  CacheNotification warmup;
  warmup.url = std::string(url);
  warmup.hostname = std::string(hostname);
  warmup.scheme = std::string(scheme);
  warmup.capability_mask = pagespeed::kWarmupSentinel;
  warmup.content_type = notification.content_type;
  (void)SendNotificationPersistent(socket_path, warmup);
  entry.count.store(0, std::memory_order_relaxed);
}
```

The threshold is the `pagespeed_hot_threshold N;` directive (the nginx module documents a default of 5; a value of 0 disables warmup entirely). After firing, the count resets to 0, so warmup is sent once per threshold window rather than on every hit, and the send is best-effort — a failed notification is non-fatal.

The signal itself is a sentinel, not a real capability mask. From `capability_mask.h`:

```cpp
inline constexpr uint32_t kWarmupSentinel = 0xFFFFFFFE;
```

The low byte is `0xFE`, whose viewport bits decode to `3`, which is not a valid `Viewport` value (the enum stops at Desktop = 2). That is deliberate: `CapabilityMask::FromHeaders()` can never produce a mask with viewport 3, so a warmup sentinel can never be mistaken for a client's real capabilities, and a warmup notification can never accidentally overwrite a real variant's alternate ID. It shares this trick with the other worker-only sentinels: the warmup (`0xFFFFFFFE`), llms.txt (`0xFFFFFFFD`), origin-refreshed (`0xFFFFFFFC`), and early-hints (`0xFFFFFFFF`) markers all decode to the same invalid viewport. The worker rejects ordinary notifications carrying viewport 3, and the reject gate whitelists exactly the three incoming-notification sentinels — warmup, llms.txt, and origin-refreshed — so they pass through. The early-hints value is a stored marker rather than an incoming notification, so it never reaches that gate. See [sentinel cache keys and 103 Early Hints](/blog/sentinel-cache-keys-and-103-early-hints/) for that family.

## What cache warmup variant generation produces

When the worker sees `capability_mask == kWarmupSentinel`, it dispatches to `HandleWarmupRequest` — but only if `enable_warmup` is set in the live config. Warmup is off by default; with it disabled the worker logs and ignores the sentinel.

For an image, the handler reads the original once with `ReadBestAlternate`, skips unsupported content types, and then iterates the matrix. The format set it tries is fixed:

```cpp
const CapabilityMask::ImageFormat all_fmts[] = {
    CapabilityMask::ImageFormat::kWebP,
    CapabilityMask::ImageFormat::kAvif,
    CapabilityMask::ImageFormat::kOriginal,
};
```

It loops over Save-Data (off, on), pixel density (1x, 2x), and viewport (Mobile, Tablet, Desktop). The sibling dimensions are gated by config: `proactive_savedata_variants`, `proactive_density_variants`, and `proactive_viewport_variants` each control whether the non-default value of that axis is generated. With all three on, warmup covers the full matrix; with them off it narrows to Desktop / 1x / Save-Data-off.

The handler does not blindly re-encode. It lists existing alternate IDs in a single disk traversal, then `FindMissingFormats` compares each candidate variant's alternate ID against that set and returns only the formats not already present. Whatever is missing is handed to `TranscodeMultiResized`, which produces the WebP, AVIF, and optimized-original outputs from the shared decode for that viewport/density/Save-Data combination. There is also a guard against the proactive handler: warmup checks a per-URL processed set and skips images already built by the inline proactive path, so the two paths do not race and overwrite each other's metadata (content class, SSIMULACRA2 scores).

CSS and JS warmup is simpler. Rather than fan out a matrix, the handler re-dispatches the notification as a normal one with the default mask, relying on the standard handler's "only write if smaller" check to ensure a minified variant exists without redundant writes.

## A note on the analysis queue and warmup

There is a separate piece of machinery worth distinguishing from the image path above, because it is easy to assume the warmup signal drives it and it does not. Browser-analysis work — the perf-profiling and rendering pass for HTML, distinct from image transcoding — runs through `AnalysisQueue`, a bounded priority queue (default capacity 1000). The warmup handler does not feed it: `HandleWarmupRequest` switches only on image, CSS, and JS content types (there is no HTML case), the image path writes variants directly to the cache, and the CSS/JS paths re-dispatch as normal notifications. The only `EnqueueAnalysis` call sites are in the regular HTML notification path, not the warmup path. So image, CSS, and JS warmup never touch `AnalysisQueue`.

The queue's `Item` struct does carry an `is_warmup` flag and a `frequency` field, and the comparator in `analysis_queue.cc` reads both:

```cpp
bool current_higher =
    (it->is_warmup && !item.is_warmup) ||
    (it->is_warmup == item.is_warmup && it->frequency > item.frequency);
```

The design intent is clear from the field names and the queue's own header comment ("warmup first, then by notification frequency"): warmup items would sort ahead of regular analysis, and within a class the most-requested templates would outrank rarely-seen ones, with the same admit rule deciding what survives tail-drop eviction when the queue is full. But neither key is populated in the shipped code. `EnqueueAnalysis` takes no `is_warmup` or `frequency` argument, nothing assigns either field, and both stay at their defaults (`is_warmup = false`, `frequency = 1`) on every enqueued item. So today the ordering collapses to insertion order, and the priority comparator is wiring waiting for inputs rather than a rule that fires in practice. It dedups by template hash and tail-drops on overflow; the warmup-then-frequency ranking is design direction, not live behavior.

## When to turn it on

Warmup trades CPU for tail latency on your most-requested assets. If a handful of URLs carry most of your traffic and you serve a wide spread of client capabilities, paying for the full matrix up front means later visitors hit warm variants instead of fallbacks. If your traffic is genuinely long-tail, leave it off (the default) and let lazy optimization build only what is asked for. Either way the warmup work runs off the request path, so enabling it does not slow the request that tripped the threshold.

## Related

- [Viewport-aware image optimization](/blog/viewport-aware-image-optimization/)
- [Cache key derivation and alternate fallback](/blog/cache-key-derivation-and-alternate-fallback/)
- [Memory-mapped cache and zero-copy serving](/blog/memory-mapped-cache-zero-copy-serving/)
- [Sentinel cache keys and 103 Early Hints](/blog/sentinel-cache-keys-and-103-early-hints/)
- [Single-URL cache purge for an optimizing proxy](/blog/single-url-cache-purge-optimizing-proxy/)
- [Asynchronous rewriting](/how-it-works/async-rewriting/)
- [Cache modes](/docs/cache-modes/)

Warmup is off by default; turn it on with `--enable-warmup` on the worker and `pagespeed_hot_threshold N;` in your nginx config, then watch the `proactive_variants_written` stat climb on your busiest URLs. [Download mod_pagespeed](/download/) to try it, and read [the cache-modes documentation](/docs/cache-modes/) to decide which mode pairs with your [TTL freshness heuristics](/blog/default-cache-ttl-heuristic-freshness/). It is licensed under Apache-2.0 and free to run, so you can measure the win on your own traffic.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
