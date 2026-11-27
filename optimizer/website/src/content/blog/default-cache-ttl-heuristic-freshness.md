---
title: 'Default cache TTL when the origin sends no Cache-Control'
description: 'Default cache TTL with no Cache-Control: per-content-type heuristic TTLs, RFC 9111 Age adjustment and the shared-vs-private split in mod_pagespeed.'
date: 2026-06-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ["caching","operations","performance","deep-dive"]
draft: false
product: '2.0'
---

Plenty of origins ship responses with no `Cache-Control` header at all. A bare CMS, a misconfigured app server, a static handler that forgot the directive. The browser then falls back to its own heuristics, but an optimizing proxy sitting in front of that origin has a harder problem: it has already transformed the bytes, written an entry to its cache, and now has to decide a freshness lifetime for a response that told it nothing. That is the **default cache TTL when no Cache-Control** is present, and mod_pagespeed resolves it in one place: `EvaluateFreshness` in `lib/cache/freshness.cc`.

This post covers exactly that decision: the per-content-type heuristic defaults, the caps that bound them, the RFC 9111 Age adjustment applied at insert, and the shared-vs-private split that decides whether `s-maxage` even counts. It does not cover the `kSafe`/`kAggressive` mode toggle or `must-revalidate`/`immutable` handling (see [cache-mode safety](/blog/cache-mode-safety-must-revalidate-vs-aggressive/)), nor the 304 conditional-revalidation handshake (see [conditional revalidation](/blog/conditional-revalidation-304-vs-active-purge/)).

## Default cache TTL when no Cache-Control: per-content-type heuristic defaults

`EvaluateFreshness` takes a `FreshnessInput` (scalar fields lifted from the stored `AlternateMetadata`) and a `FreshnessConfig`. The config carries the heuristic defaults. From `lib/cache/freshness.h`:

```cpp
struct FreshnessConfig {
  uint32_t max_age_cap = 86400;             // Cap on effective max-age
  uint32_t immutable_max_age_cap = 604800;  // Cap for immutable content
  uint32_t html_max_age = 0;                // Default for HTML without CC
  uint32_t css_max_age = 300;               // Default for CSS/JS without CC
  uint32_t image_max_age = 3600;            // Default for images without CC
};
```

The defaults only fire when the origin sent no max-age **and** no `Cache-Control` header at all (`kCCOriginHeaderPresent` is clear):

```cpp
if (shared_max_age == 0 && ((cc & AM::kCCOriginHeaderPresent) == 0)) {
  if (input.content_type == ContentType::kHtml) {
    shared_max_age = config.html_max_age;
  } else if (input.content_type == ContentType::kCss ||
             input.content_type == ContentType::kJs) {
    shared_max_age = config.css_max_age;
  } else if (input.content_type == ContentType::kImage) {
    shared_max_age = config.image_max_age;
  }
}
```

CSS and JS share one knob (`css_max_age`); images get their own (`image_max_age`); HTML gets `html_max_age`, which defaults to `0`. A zero HTML default is deliberate. The very next block turns it into a `kServeNoCache` verdict:

```cpp
if (shared_max_age == 0 && input.content_type == ContentType::kHtml &&
    ((cc & AM::kCCOriginHeaderPresent) == 0)) {
  return {FreshnessVerdict::kServeNoCache, age, 0, 0, true};
}
```

HTML with no `Cache-Control` is treated as no-cache, not cached-for-an-hour. Guessing a TTL on a page that might be a logged-in dashboard is how you serve one user's account page to another.

Whatever default (or origin value) you land on is then clamped:

```cpp
uint32_t effective_max_age;
if ((cc & AM::kCCOriginImmutable) != 0) {
  effective_max_age = std::min(shared_max_age, config.immutable_max_age_cap);
} else {
  effective_max_age = std::min(shared_max_age, config.max_age_cap);
}
```

So `max_age_cap` (86400s, one day) bounds ordinary content and `immutable_max_age_cap` (604800s, one week) bounds anything flagged immutable. These are ceilings, not the values served when the origin is silent.

One thing the struct defaults do not tell you: the nginx integration overrides the CSS and image defaults by cache mode at config-merge time. In `merge_loc_conf` (`src/nginx/ngx_pagespeed_module.cc`), safe mode keeps `css_max_age = 300` but uses `image_max_age = 1800`, while aggressive mode raises both to `86400`. HTML stays `0` in both modes. The struct's `image_max_age = 3600` is the pure-library default; the nginx config you actually run resolves to `1800` (safe) or `86400` (aggressive). Read your resolved `pagespeed_css_max_age` / `pagespeed_image_max_age` directives, not the header constant, if you need the exact number on your box. When the origin is silent, the module also logs a one-time rate-limited warning per host and content type telling you which default it picked.

## The shared-vs-private split: s-maxage only counts for a shared cache

A directive like `s-maxage` is addressed specifically to shared (proxy) caches. A private cache must ignore it. `EvaluateFreshness` picks the effective lifetime accordingly:

```cpp
uint32_t shared_max_age;
if (((cc & AM::kCCOriginSMaxagePresent) != 0) && input.is_shared_cache) {
  shared_max_age = input.origin_s_maxage;
} else {
  shared_max_age = input.origin_max_age;
}
```

If `s-maxage` is present and this is a shared cache, it wins over `max-age`. If the same response is evaluated by a private cache (`is_shared_cache = false`), `s-maxage` is skipped and `max-age` is used. The nginx module always sets `is_shared_cache = true` (`fi.is_shared_cache = true; // nginx is always a shared cache`); the in-process ASP.NET middleware is the integration that flips it false.

The same split governs whether a stored entry is even allowed to be served stale. `ComputeSharedRevalidationRequired` in `cache_control_header.cc` is the single source of truth for "does RFC 9111 require revalidation before serving stale?":

```cpp
inline bool ComputeSharedRevalidationRequired(uint16_t origin_cc_flags,
                                              bool is_shared_cache) {
  using AM = AlternateMetadata;
  const uint16_t cc = origin_cc_flags;
  return ((cc & AM::kCCOriginNoCache) != 0) ||
         ((cc & AM::kCCOriginMustRevalidate) != 0) ||
         (((cc & AM::kCCOriginProxyRevalidate) != 0) && is_shared_cache) ||
         (((cc & AM::kCCOriginSMaxagePresent) != 0) && is_shared_cache);
}
```

`no-cache` and `must-revalidate` always apply. `proxy-revalidate` (RFC 9111 §5.2.2.10) and `s-maxage` (§5.2.2.9) only apply when `is_shared_cache` is true. `BuildCacheControlHeader` uses this verdict to suppress `stale-while-revalidate` and `stale-if-error` synthesis for any response that RFC 9111 §4.2.4 says may not be served stale. A private-cache integration that ignores those two directives correctly gets to keep serving stale where a shared cache would not.

## Age adjustment at insert, and why a plain reload must not purge anything

Freshness is `now - inserted_at` compared against the effective max-age. That comparison is only correct if `inserted_at` already accounts for time the response spent in caches upstream of you. RFC 9111 §4.2.3 handles this with the `Age` header, and mod_pagespeed applies it at insert time rather than at every read. From the nginx module:

```cpp
// Set insertion timestamp, adjusted by inbound Age header (D3).
meta.cache_inserted_at = static_cast<uint32_t>(time(nullptr));
ngx_str_t inbound_age = ngx_http_pagespeed_get_response_header(r, "Age");
if (inbound_age.len > 0) {
  ...
  if (age_val > 0 && age_val <= meta.cache_inserted_at) {
    meta.cache_inserted_at -= age_val;
  }
}
```

A response that arrives with `Age: 300` is stamped as if inserted five minutes ago, so a 600-second TTL leaves 300 seconds of real freshness, not 600. The `FreshnessInput` precondition documents this contract: `cache_inserted_at is already Age-adjusted at insert time`. The same subtraction runs on the 304-revalidation path when an entry's freshness is reset.

The age itself is computed defensively by `ClampAge`. A `cache_inserted_at` more than 60 seconds in the future is treated as a corrupted timestamp and fails toward stale (returns `UINT32_MAX`); small negative ages from clock skew clamp to `0`.

The `FreshnessVerdict` state machine then resolves to one of `kFresh`, `kServeNoCache`, or `kRevalidate`. (`kStaleServe` is retired — see below.) Crucially, `FreshnessResult` carries a separate `expired_by_age` flag, and getting that flag right is the difference between a working cache and one that thrashes:

```cpp
const bool expired_by_age = !is_no_cache && (age > effective_max_age);
```

`expired_by_age` is true **only** for genuine age-based expiry. It is explicitly false for two cases that also yield `kRevalidate`: origin `no-cache` content (permanently "stale" by definition, since every use revalidates), and client force-refresh. When a browser sends `Cache-Control: no-cache` or `max-age=0` (a plain reload, or Ctrl+F5), the nginx module sets `force_revalidate`, and `EvaluateFreshness` returns:

```cpp
if (input.force_revalidate) {
  return {FreshnessVerdict::kRevalidate, age, effective_max_age, 0, true,
          /*expired_by_age=*/false};
}
```

The reason for the `false` is in the comment right above it: the cached variants are not known to be outdated, and treating a reload as expiry would let any anonymous client purge a URL's entire optimized variant set with a single shift-reload. The nginx module gates the origin-refreshed variant-set purge and the bounded single-flight refresh on `expired_by_age` specifically. A reload revalidates; it does not nuke your cache.

This is the substance of the retired `kStaleServe` change (issue #652). Previously, stale content with no revalidation directive (a plain `public, max-age=30` that aged out) was served indefinitely as `kStaleServe` — a shared cache serving HITs past the freshness lifetime with no stale-while-revalidate machinery behind it. Now **all** stale content yields `kRevalidate`: a conditional re-fetch when validators (ETag / Last-Modified) exist, a full re-fetch otherwise. The enum value is kept so existing switch statements stay exhaustive, but `EvaluateFreshness` never returns it.

## Related

- [Cache mode safety: must-revalidate vs aggressive](/blog/cache-mode-safety-must-revalidate-vs-aggressive/)
- [Conditional revalidation: 304 vs active purge](/blog/conditional-revalidation-304-vs-active-purge/)
- [Single-URL cache purge in an optimizing proxy](/blog/single-url-cache-purge-optimizing-proxy/)
- [Content-hash URLs](/blog/content-hash-urls/)
- [Cache-key URL normalization and tracking params](/blog/cache-key-url-normalization-tracking-params/)
- [How it works: the metadata cache](/how-it-works/metadata-cache/)
- [Cache modes documentation](/docs/cache-modes/)

If your origin is silent on `Cache-Control`, the safe default behavior here keeps you correct: HTML revalidates, CSS/JS gets a short conservative TTL, and nothing is served stale without a re-fetch. When you want to tune the heuristic numbers, the `pagespeed_html_max_age` / `pagespeed_css_max_age` / `pagespeed_image_max_age` and `pagespeed_max_age` cap directives are documented under [cache configuration](/docs/cache-control/). [Grab a build](/download/) and run it in front of your stack — it is licensed under Apache-2.0 and free to run — to verify the freshness behavior against your own traffic.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
