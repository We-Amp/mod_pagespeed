---
title: 'Stop cache fragmentation: strip tracking params from URLs'
description: 'Strip tracking parameters to stop cache fragmentation: mod_pagespeed normalizes URLs before keying, drops UTM params, sorts the query, aliases hosts.'
date: 2026-06-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['caching', 'operations', 'performance', 'deep-dive']
draft: false
product: '2.0'
---

A marketing campaign goes out. The link is `https://shop.example/product/42?utm_source=newsletter&utm_medium=email&utm_campaign=spring`. Every recipient who clicks hits the same page, but each variation of the campaign string is, byte for byte, a different URL. If the cache keys on the raw request URL, that one product page now has dozens of cache entries that all hold the same optimized HTML. The cache fills with duplicates, the hit rate drops, and the first visitor on every variant pays the full optimization cost again. To strip tracking parameters and stop this cache fragmentation, mod_pagespeed normalizes the URL before it ever becomes a cache key.

The normalization layer lives in `lib/classify/url_normalizer.cc`. It runs at the front door in nginx and again in the worker's cache API, so the key written at serve time and the key looked up by the purge and inspection endpoints agree on exactly the same canonical form.

## How `NormalizeCacheUrl` strips tracking params and canonicalizes the query

The entry point is `NormalizeCacheUrl(url, config)`. It takes the path-plus-query string and a `UrlNormalizationConfig`, and returns the canonical form. The steps run in a fixed order, and the order matters.

First it strips anything that should not be part of a path-only key. If the input carries a scheme and authority (`https://host/path`), the scheme-and-authority prefix is removed down to the first `/`, leaving `/path?query#frag`. The match for `://` is deliberately constrained to occur before the first `/`, so a path-only URL that happens to carry `://` inside a query value (`/page?url=https://x`) is left intact. Any `#fragment` is then dropped from the post-scheme string.

If there is no `?` at all, the function returns early after one more pass: percent-encoding normalization on the path. That pass (`NormalizePercentEncoding`) decodes percent-escapes for RFC 3986 unreserved characters (`A-Za-z0-9-._~`) and uppercases the hex digits of everything that stays encoded, so `%2D` and `%2d` and a literal `-` all collapse to the same byte. Two URLs that differ only in escaping no longer split the cache.

When there is a query string, the path and query are percent-normalized separately, then the query goes through three steps:

1. **Extension-based whole-query stripping.** If `strip_query_extensions` is non-empty, the function extracts the lowercase file extension from the last path segment via `ExtractLowercaseExtension`. If that extension is in the set, the entire query string is discarded and the bare path is returned. A static asset requested as `/app.css?v=123` and `/app.css?v=124` becomes one key, `/app.css`.
2. **Tracking-param removal.** For everything else, the query is parsed into key/value pairs by `ParseQueryParams`, and any key listed in `strip_query_params` is erased. This is where `utm_source`, `utm_medium`, and friends go, if you configure them there.
3. **Sort, then dedup.** The surviving params are sorted by key, then by value within a key, and adjacent identical `key=value` pairs are collapsed with `std::unique`. So `?b=2&a=1` and `?a=1&b=2` produce the same string, and `?a=1&a=1` becomes `?a=1`. Parameter order and accidental duplication stop fragmenting the cache.

The pairs are reassembled into `path?k1=v1&k2=v2`, preserving the distinction between a key that had an `=` and a bare valueless key. If filtering emptied the query entirely, the bare path is returned with no trailing `?`.

One thing the source does *not* do: it ships no built-in tracking-param list. `strip_query_params` and `strip_query_extensions` are both empty by default. Normalization that drops query data is opt-in, by configuration, because silently discarding a parameter that the origin actually keys on would be a correctness bug, not an optimization. You decide which params are noise.

## Extension groups: `static` and `images`

Listing every static extension by hand is tedious, so the config accepts named groups. `ExpandExtensionGroup` resolves two of them:

- `static` expands to `.css`, `.js`, `.woff`, `.woff2`, `.ttf`, `.eot`.
- `images` expands to `.jpg`, `.jpeg`, `.png`, `.gif`, `.webp`, `.avif`, `.svg`, `.ico`, `.bmp`, `.tiff`.

An unknown group name resolves to an empty list, which is a no-op rather than an error. In the worker's config-apply path (`api_handlers.cc`), the configured groups are expanded and merged with any explicit `strip_query_extensions` into one deduplicated extension set, then written out as the comma-separated value the nginx side reads back. The group is a shorthand; the stored configuration is always the flat extension set.

## Host aliasing, and where this runs

URL fragmentation has a hostname twin: the same site reachable as `example.com`, `www.example.com`, and `example.com:443` keys three ways for one page. `NormalizeCacheHostname` handles that. It first runs `NormalizeHostname` (which canonicalizes case and default ports), then consults a `host_aliases` map and substitutes the canonical host if the normalized name has an alias entry. The map is loaded from a `pagespeed-hosts.conf` sidecar next to the cache directory, with a `version` line that lets a v2 file reset the alias set.

The wiring is what keeps the serve side and the management side from drifting. In `ngx_pagespeed_module.cc`, the request's URI and args are joined and passed straight through `NormalizeCacheUrl(url, g_url_norm_config)` before the cache key is composed, and the request host goes through `NormalizeCacheHostname` against the same global config. That config is rebuilt from the comma-separated `strip_query_extensions` / `strip_query_params` lines in `pagespeed-shared.conf` plus the host aliases file, and it is reset cleanly on cache reopen.

The worker's cache API (`cache_handlers.cc`, `api_handlers.cc`) uses the same normalizer when it composes the `cache_key` it reports back: `scheme://normalized_host/url`. So when you query `/v1/cache/urls` or inspect a key, the host you get back is the one nginx actually wrote under, normalized through the identical code path. The serve side and the management side cannot drift, because they call the same function. If you want the full picture of how that key is assembled from scheme, host, and URL, see the [cache key derivation deep-dive](/blog/cache-key-derivation-and-alternate-fallback/) — it covers the host-scoped key and the alternate model, and it leaves the query-normalization layer described here to this post.

## Related

- [Cache key derivation and alternate fallback](/blog/cache-key-derivation-and-alternate-fallback/)
- [Single-URL cache purge in an optimizing proxy](/blog/single-url-cache-purge-optimizing-proxy/)
- [Content-hash URLs](/blog/content-hash-urls/)
- [Cache mode safety: must-revalidate vs aggressive](/blog/cache-mode-safety-must-revalidate-vs-aggressive/)
- [Default cache TTL and heuristic freshness](/blog/default-cache-ttl-heuristic-freshness/)
- [Viewport-aware image optimization](/blog/viewport-aware-image-optimization/)
- [Cache modes documentation](/docs/cache-modes/)

If your hit rate looks lower than your traffic should produce, look at the keys before you blame the cache: tracking params, query order, and host aliases are the usual culprits, and all three are configuration away from collapsing into one entry. [Download mod_pagespeed](/download/) and set `strip_query_params` to the noise your origin ignores, then check the [cache modes documentation](/docs/cache-modes/) to pair normalization with the [right default-TTL and freshness heuristics](/blog/default-cache-ttl-heuristic-freshness/). The build is licensed under Apache-2.0 and free to run, so you can measure the hit-rate change on your own traffic.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
