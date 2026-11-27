---
title: 'Cache key derivation: host-scoped keys and variant fallback'
description: 'How cache key derivation in mod_pagespeed hashes host plus URL into one key and scores stored variants in one selector pass instead of probing keys.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['caching', 'architecture', 'deep-dive', 'nginx', 'save-data']
draft: false
product: '2.0'
---

Two sites behind the same proxy ask for the same path: `GET /logo.png`, one for `Host: a.example`, one for `Host: b.example`. If your cache key is the path, those two requests collide. The first site's logo gets served to the second site's visitors. The fix is in how the key is derived, and it changes more than just isolation.

This post walks the shipped cache key derivation in mod_pagespeed: how a key is built from scheme, host, and URL, how the many variants of one resource are stored as alternates under that single key, and how a scored single-pass selector picks the right variant instead of probing a sequence of separate keys.

## Cache key derivation: a digest of scheme, host, and URL

A cache key in mod_pagespeed is a Cyclone `CacheKey`: a 32-byte SHA-256 digest. The cache layer composes the string that gets hashed in `ComposeKey()` (private; callers reach it through `(url, hostname, scheme)` operations on `PageSpeedCache`), which builds `scheme://hostname/url` and hands it to the `cyclone::CacheKey` constructor. Host isolation falls out of it for free: different hostnames produce different strings, so they produce different digests, and `a.example/logo.png` and `b.example/logo.png` can never share a key. There is no escape hatch and no shared namespace. The isolation is cryptographic rather than conventional. Scheme is part of the string too, so `http` and `https` requests for the same host and path get distinct keys.

Two things go into the hash that an older path-only scheme would have dropped. The host goes in, which is what fixes isolation. And the variant information stays *out* of the key entirely — variants live as alternates under the one key, which is the more interesting half.

Normalization matters when you hash, because two requests that mean the same resource have to produce the same bytes before hashing, or you split the cache. `ComposeKey()` normalizes the hostname through `NormalizeHostname()` before composing the key: the host is ASCII-lowercased, a default port (`:80` or `:443`) is stripped, a non-default port (e.g. `:8080`) is preserved, and a trailing dot is removed (`example.com.` becomes `example.com`). The URL path is left alone here — paths are case-sensitive and percent-encoding is preserved as-is, which matches what browsers do. Query-string canonicalization, such as [stripping tracking parameters that fragment the cache](/blog/cache-key-url-normalization-tracking-params/), runs in a separate normalization layer before the string ever reaches key composition. Hostname normalization lives in the cache layer, not in the callers, so the rules are applied once at key composition. If you are reasoning about cache behavior, that is the layer that owns it.

A `Host`-less request is the edge case. `NormalizeHostname()` returns an empty string for empty input, so the key is composed against an empty hostname rather than failing. Worth knowing if you are tracing why two host-less requests share a key.

## One key, up to 64 alternates

If the variant mask is no longer in the key, where does it live? In the alternate. Cyclone — the cache library both parts of mod_pagespeed share — has native alternate selection: it can store multiple variants of one resource under a single key, each with its own stored bytes, and pick between them with a pluggable selector. The cap is `kMaxAlternatesPerKey = 64`.

A path-only-key scheme would have stored each variant under its own key. mod_pagespeed keeps one key with the variants hung off it as alternates. Each variant is identified by an `AlternateId`, a `uint8_t` that is the low byte of the 32-bit capability mask — a direct cast, no offset or remapping (`MaskToAlternateId()` and `AlternateIdToMask()` in `alternate_id.h`). That low byte holds five dimensions: image format (bits 0-1), viewport class (bits 2-3), pixel density (bit 4), Save-Data (bit 5), and transfer-encoding (bits 6-7). Because PageSpeed always uses its own `PageSpeedSelector`, never one of Cyclone's built-in selectors, the named Cyclone `AlternateId` values (Brotli, WebP, and so on) are irrelevant to PageSpeed's usage and never consulted — the comment in `alternate_id.h` is explicit about this.

The image-format dimension has four values (bits 0-1: `00` Original, `01` WebP, `10` AVIF, `11` SVG), viewport has three (mobile, tablet, desktop), density two, Save-Data two, and transfer-encoding three real values (identity, gzip, brotli; `11` reserved). Note what is *not* a dimension: there is no connection-speed or effective-connection-type bit in the mask. When a write would exceed 64 alternates, Cyclone returns a `TooManyAlternates` error (surfaced through the C API as `PS_ERR_TOO_MANY_ALTERNATES`) rather than corrupting the chain.

Each alternate also carries a small metadata blob, written as a prefix on the content bytes (kept as a content prefix rather than via Cyclone's per-alternate header, so the wire format stays under PageSpeed's control and metadata plus content are written atomically in one `close_sync()`). It holds the full 32-bit mask (the `AlternateId` only has room for 8 bits, but the selector and the worker want the rest), a content-type enum, and the origin's full `Content-Type` string — `text/html; charset=utf-8` and all. On a cache hit the serve path reads the stored content type straight from the metadata instead of sniffing it from the URL extension, which would lose the charset. The blob is versioned: its first byte is a version number (`kCurrentVersion = 7`), and the deserializer reads older v3–v6 blobs by defaulting the fields those versions did not carry, so the format can evolve without a cache flush.

There is one more use of the `AlternateId` space worth naming. Real client requests only ever produce viewport values 0, 1, or 2 (mobile, tablet, desktop), never 3. So any `AlternateId` whose viewport bits are `0b11` is unreachable from a real request, which makes it a safe namespace for internal sentinel records: the original content, the subresource manifest, the Early Hints preload data, and so on (the `SentinelId` enum lists them, each with viewport bits set to 3). They live under the same key as the content, but a real client mask can never collide with them, and the selector skips them. Compile-time asserts enforce that every sentinel has viewport=3 and that valid content masks never do. The original mod_pagespeed team would recognize the instinct — carve an unreachable region of an existing key space rather than invent a parallel one.

## Scoring variants in one pass instead of probing keys

Here is the part that actually changes request latency. A per-mask-key scheme makes a lookup for a variant that is not present fall back: build the next-best mask, build its key, do a cache read, miss, build the next mask, read again — each probe a separate cache I/O. The selector header notes that its scoring "intentionally differ[s] from the old `FallbackMasks()` probe order," so that sequential approach is the design it replaces.

With alternates, the whole chain for a key is in hand once you have the key. `PageSpeedSelector`, which implements Cyclone's `StorageAlternateSelector` interface, gets handed every alternate stored under the key in one `select()` callback and scores each one against the client's requested mask. It returns the index of the highest scorer. One pass over the alternates, O(alternates), instead of O(fallback_masks × I/O). The selector reads the client mask from the first 4 bytes of the request metadata (little-endian); when no alternate scores above zero it returns no index, so the caller's miss path runs.

The scoring is a weighted match (`ScoreAlternate()`), with format dominating and the lesser dimensions refining. The shipped weights:

```
score = 0
sentinel alternate (viewport bits = 3):              return 0   // skipped
if stored format is SVG:                             score += 1200  // universal
  else if stored format == client format:            score += 1000
  else if stored format == Original:                 score += 100
if SVG, or viewport matches:                         score += 80
if SVG, or density matches:                          score += 40
if SVG and client wants Save-Data:                   score += 50
  else if save_data matches:                         score += 20
if encoding matches:                                 score += 60
  else if stored encoding is identity:               score += 5    // nginx can compress
  else:                                              return 0      // can't serve undecodable
```

Format is weighted to win because serving the wrong image format is the costly mistake; viewport, density, Save-Data, and transfer-encoding refine from there. SVG gets a +1200 universal bonus — higher than an exact +1000 format match — because it is resolution-independent and universally supported, so its viewport and density bonuses are always awarded and it earns a +50 Save-Data bonus as an inherently lightweight format. The one hard rule is transfer-encoding: a stored variant in a non-identity encoding (gzip or brotli) that the client did not ask for scores zero outright, because serving content in an encoding the client cannot decode is worse than a miss. A stored identity variant still scores a small +5 for an encoding-mismatched client, on the basis that nginx can compress identity content at serve time.

The selection dimensions are the same ones that drive [viewport-aware image optimization](/blog/viewport-aware-image-optimization/) and the [Save-Data](/blog/save-data-bandwidth/) path, so the variant a request gets and the variant the optimizer produced are scored against the same mask. And because every variant lives under one key, purge gets trivial as a side effect: one `Remove()` call drops the whole alternate chain. That fan-out collapse is the subject of [single URL cache purge](/blog/single-url-cache-purge-optimizing-proxy/), which is worth reading next if purge behavior is what you came for.

## Related

- [Single-URL cache purge in an optimizing proxy](/blog/single-url-cache-purge-optimizing-proxy/)
- [Sentinel cache keys and 103 Early Hints](/blog/sentinel-cache-keys-and-103-early-hints/)
- [Zero-copy serving from the memory-mapped cache](/blog/memory-mapped-cache-zero-copy-serving/)
- [Default cache TTL and freshness heuristics](/blog/default-cache-ttl-heuristic-freshness/)
- [Viewport-aware image optimization](/blog/viewport-aware-image-optimization/)
- [Content-hash URLs and why they self-purge](/blog/content-hash-urls/)
- [Self-hosted image optimization](/blog/self-hosted-image-optimization/)
- [AVIF vs WebP in 2026](/blog/avif-vs-webp-2026/)
- [How the metadata cache works](/how-it-works/metadata-cache/)
- [Cache modes](/docs/cache-modes/)

If you want to see this caching layer in practice, the nginx build is on the [downloads page](/download/), and the [cache-control behavior](/docs/cache-control/) doc covers how mod_pagespeed reads and emits cache directives. It is licensed under Apache-2.0 and free to run in development and in production, so you can confirm the host-scoped keys and single-pass selection hold up against your own traffic.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
