---
title: 'Sentinel cache keys: alternate IDs for 103 Early Hints'
description: 'How the mod_pagespeed worker precomputes 103 Early Hints preloads that nginx serves from cache, stored under reserved sentinel keys beside each URL.'
date: 2026-06-13
author: 'Otto van der Schaaf'
tags: ['caching', 'architecture', 'deep-dive', 'performance', 'nginx']
draft: false
product: '2.0'
lastUpdated: 2026-10-22
---

A browser asks for `https://example.com/`, and before the origin has produced a single byte of HTML, nginx writes back `HTTP/1.1 103 Early Hints` with a list of stylesheet preloads. The browser starts fetching CSS while the real response is still being assembled. That `103` payload did not come from the origin and it was not computed on the request path. It was read out of the cache, from a key that no client request can ever address.

This post is about sentinel cache keys, as they ship in mod_pagespeed. The proxy stores per-URL content variants — WebP, AVIF, mobile, desktop, retina, Save-Data — as Cyclone *alternates* under one cache key. The same alternate mechanism also holds metadata that is *not* a content variant: the Early Hints preload list, a content-hash binding, a per-template browser optimization profile. The classifier reserves a slice of the alternate ID space for exactly this, using a property of the capability mask that makes collision with a real client impossible.

## One key, many alternates, a reserved corner

In the alternate model, the cache key is a SHA-256 digest of `hostname/url` via `CacheKey::from_url(url, hostname)`. Every variant of that resource lives under the same key as a separate alternate, identified by a `uint8_t` `AlternateId` — the low 8 bits of `CapabilityMask::Encode()`. The bit layout (`lib/classify/capability_mask.h`) is what makes the reservation work:

- Bits 0-1: image format (00 Original, 01 WebP, 10 AVIF, 11 SVG)
- Bits 2-3: viewport (00 Mobile, 01 Tablet, 10 Desktop, **11 unreachable**)
- Bit 4: pixel density (0 = 1x, 1 = 2x+)
- Bit 5: Save-Data (0 off, 1 on)
- Bits 6-7: transfer-encoding (00 Identity, 01 Gzip, 10 Brotli, 11 Reserved)

The classifier's `FromHeaders()` only ever produces viewport values 0, 1, or 2 — Mobile, Tablet, Desktop. It never emits viewport 3. So any byte where `((byte >> 2) & 3) == 3` is unreachable from a real request. That is the "Viewport=3 trick": those byte values are free for the system to use as sentinels, because no header combination a browser sends can ever map to them. A request asks for a viewport that exists; a sentinel hides behind one that does not. The test is a one-liner in `alternate_id.h`:

```cpp
inline constexpr bool IsSentinel(AlternateId id) {
  return ((id >> 2) & 0x03) == 0x03;
}
```

The invariant is enforced at compile time, not by convention. Every sentinel constant is checked against it when the classifier compiles:

```cpp
static_assert(IsSentinel(static_cast<AlternateId>(SentinelId::kEarlyHints)));
```

If someone later picks a sentinel constant with the viewport bits set wrong, the build breaks instead of silently aliasing a real Desktop-2x-WebP variant.

## What sentinel cache keys carry

Each sentinel `AlternateId` is a named metadata channel. The enum `SentinelId` in `lib/classify/alternate_id.h` defines them, and the active channels have a clear writer and reader:

| `SentinelId` | Value | Purpose | Written by | Read by |
|--------------|-------|---------|------------|---------|
| `kEarlyHints` | `0x1C` | Early Hints preload list | worker | nginx (serves 103) |
| `kContentHash` | `0x3C` | SHA-256 of the raw origin body | worker | worker |
| `kBrowserProfile` | `0x5C` | Per-template optimization profile | worker | worker |
| `kOriginalContent` | `0x0C` | Reserved channel for original content | — | — |
| `kSubresourceManifest` | `0x4C` | Reserved channel for a subresource manifest | — | — |
| `kReserved2` | `0x6C` | Reserved for future use | — | — |

All of them satisfy `((id >> 2) & 3) == 3`, so none can collide with a client mask. (The enum carries a few more — `kWarmupRequest` `0x2C`, plus the agent-optimize channels `kAgentMarkdown`, `kLlmsTxt`, `kLlmsTxtMeta` — outside the scope of this post.) The first three rows are the ones doing work in the HTML pipeline today.

**`kEarlyHints` (0x1C) — Early Hints.** When the worker processes HTML on the miss path, it collects stylesheet links from the scan result, adds an LCP image preload and third-party `preconnect` hints where it found them, sanitizes each URL against CR/LF injection, and writes the newline-delimited list with `WriteSentinel(..., SentinelId::kEarlyHints, ...)`. nginx is the reader. On a later HTML miss it reads `kEarlyHints` and calls `SendEarlyHints()`, which emits the `103 Early Hints` response before declining to the upstream. On an HTML hit it reads the same channel and attaches the preloads as `Link` headers. No re-scan, no parse on the serve path — the hint list is precomputed and sitting in the cache. That is why the `103` can go out before the origin responds.

**`kContentHash` (0x3C) — origin change detection.** The worker stores a 32-byte SHA-256 digest of the raw origin body here, then reads it back on the next pass to decide whether the origin actually changed. If the stored hash still matches, there is nothing to rebuild; if it differs, the worker purges the URL's variant set so the next miss rebuilds everything from the fresh origin. Keeping the binding on a separate, unreachable key means content-change detection rides alongside the variants without ever being selectable as one.

**`kBrowserProfile` (0x5C) — browser optimization profile.** `BrowserAnalysisManager` writes a JSON optimization profile here, keyed by a template hash rather than a page URL, so structurally similar pages share one analysis. `LookupProfile()` reads it back; if the entry has been evicted, the manager drops the stale template record and re-analyzes. The profile is produced by headless browser analysis, not by parsing on the request path, which keeps the cost off the serve path entirely.

## Why a reserved value beats a separate key scheme

The alternative would be to encode the capability mask as a prefix on the cache key — `[4B mask][URL]` — and carve sentinels out of a reserved mask range. That pushes the sentinel-versus-content distinction into key *encoding*, which is exactly where bugs hide: a prefix has to be built and parsed on every operation, and a mistake on either side aliases the wrong record. Folding the distinction into the `AlternateId` removes the prefix entirely, so there is nothing to encode or decode incorrectly.

It also collapses two unrelated problems into one mechanism. PURGE under a prefix scheme means deleting one key per mask combination. Under alternates it is a single `remove_sync(key)` — one delete drops every variant *and* every sentinel for that URL at once. Content variants and metadata channels live in the same chain, are purged by the same call, and are distinguished by a single arithmetic test rather than a key-layout convention.

The selector keeps the two worlds apart at read time. `PageSpeedSelector::select()` receives every alternate stored under a key and scores them against the client's mask, but it skips anything where `IsSentinel(id)` holds. `ScoreAlternate()` returns 0 for a sentinel byte before any other scoring runs. Sentinels never participate in content selection; a client asking for the best image variant cannot be handed the Early Hints list by accident. The reservation is enforced on the write side (compile-time `static_assert`), the classification side (`FromHeaders()` never emits viewport 3), and the read side (selector skip) — three independent points, not one. The one deliberate exception is the operator-gated `kAgentMarkdown` channel, which only an agent request under the operator's `--agent-optimize` flag can select.

Two caveats. The trick reserves a viewport-3 slice of the ID space but the system uses a handful of channels, so most of that space sits idle — accepted, since alternate IDs are cheap. The collision risk of mapping a mask byte directly to an `AlternateId` is sidestepped by the selector itself: PageSpeed uses `PageSpeedSelector` exclusively and never invokes Cyclone's built-in selectors, so Cyclone's own alternate-ID semantics (Brotli=1, WebP=16, and so on) never apply to PageSpeed records and cannot collide. The `103` itself ships HTTP/1.1-only: raw `HTTP/1.1 103 Early Hints` bytes corrupt HTTP/2 and HTTP/3 framing, so nginx emits them only when `r->http_version == NGX_HTTP_VERSION_11` and skips the hint for every other protocol version. HTTP/2+ Early Hints, which would use each protocol's own informational-response framing, is genuine future work.

## Related

- [Cache key derivation and alternate fallback](/blog/cache-key-derivation-and-alternate-fallback/)
- [Server-injected resource hints and speculation rules](/blog/server-injected-resource-hints-speculation-rules/)
- [Single-URL cache purge in an optimizing proxy](/blog/single-url-cache-purge-optimizing-proxy/)
- [Critical CSS: from beacon to headless history](/blog/critical-css-beacon-to-headless-history/)
- [Server-side critical CSS with nginx](/blog/server-side-critical-css-nginx/)
- [Reducing TTFB at the server layer](/blog/reduce-ttfb-server-layer-2026/)
- [How it works: the metadata cache](/how-it-works/metadata-cache/)

The sentinel-cache-key scheme described here is in mod_pagespeed today: the `SentinelId` enum lives in the shipped classifier, the worker writes the Early Hints channel, and nginx reads it and serves the `103`. The remaining edge — `103` over HTTP/2 and HTTP/3 — is future work. [Download it](/download/), put it in front of a real site, and read the [cache modes documentation](/docs/cache-modes/) to see how the cache behaves under each mode. It is licensed under Apache-2.0 and free to run in development and in production, so you can measure the hit path on your own traffic.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
