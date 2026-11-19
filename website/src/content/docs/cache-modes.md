---
title: 'Choose a cache mode: safe vs aggressive'
description: 'Safe or aggressive Cache-Control for optimized responses in mod_pagespeed, plus the module cache reference: Cyclone, memcached, Redis, purging, IPRO.'
order: 23
group: 'Configure'
lastUpdated: 2026-11-19
---

mod_pagespeed transforms your origin's content — optimizing images, minifying
CSS, and rewriting HTML. Because the output depends on mod_pagespeed's
configuration and software version, the [Cache-Control headers your origin
sends](/docs/cache-control/) don't apply verbatim to the transformed output. A misconfigured option or a software bug
can produce broken content that gets cached by browsers and CDNs.

Cache modes control how quickly you can recover from that situation. For the
reasoning behind the defaults — why `must-revalidate`, not `max-age`, is the
real safety net — see [Cache mode safety: must-revalidate vs aggressive TTL](/blog/cache-mode-safety-must-revalidate-vs-aggressive/).

## Two modes

|                   | Safe (default)                  | Aggressive (opt-in)                           |
| ----------------- | ------------------------------- | --------------------------------------------- |
| **CSS / JS**      | `max-age=300, must-revalidate`  | `public, max-age=86400, stale-if-error=86400` |
| **Images**        | `max-age=1800, must-revalidate` | `public, max-age=86400, stale-if-error=86400` |
| **HTML**          | `no-cache`                      | `no-cache`                                    |
| **Recovery time** | 5–30 minutes                    | Up to 24 hours (or CDN purge)                 |
| **SWR synthesis** | Suppressed                      | Allowed                                       |
| **`immutable`**   | Stripped                        | Stripped                                      |

Aggressive mode lets downstream caches serve optimized content without
revalidation for the full TTL. This matches typical production CDN-backed
deployments; the tradeoff is slower recovery if a misconfiguration produces
broken output.

## Safe mode (default)

Safe mode adds `must-revalidate` to all non-HTML responses and uses short
max-age values. After the TTL expires, downstream caches **must** revalidate
with the origin — they cannot serve stale content.

This means:

- Broken CSS self-corrects within 5 minutes.
- Broken images self-correct within 30 minutes.
- HTML is always fresh (revalidated on every request).
- If the origin is unreachable after TTL expiry, caches return 504 rather than
  serving stale content. This is intentional — a visible error is better than
  silently serving corrupted content.

Revalidation costs little: mod_pagespeed generates ETags on all cache hits, so
most revalidation requests return 304 Not Modified with no body transfer.

## Aggressive mode

Aggressive mode uses long TTLs with `public` and `stale-if-error=86400`. CDN
edges and browsers cache content for up to 24 hours. If the origin is
unreachable, stale content is served for up to 24 hours before returning 504.

`stale-while-revalidate` synthesis is enabled by default in aggressive mode,
allowing browsers to serve stale content while revalidating in the background.

Switch to aggressive mode when:

- You have run in safe mode for at least a week without issues.
- Your optimization configuration is stable and tested.
- You have CDN purge capability for emergency corrections.
- Cache efficiency matters more than instant recovery.

## Configuration

### nginx

```nginx
pagespeed_cache_mode safe;        # default
pagespeed_cache_mode aggressive;  # opt-in
```

**Context:** `http`, `server`, `location`
**Default:** `safe`

The per-type max-age defaults change based on the active mode:

| Directive                 | Safe default  | Aggressive default |
| ------------------------- | ------------- | ------------------ |
| `pagespeed_css_max_age`   | 300 (5 min)   | 86400 (1 day)      |
| `pagespeed_image_max_age` | 1800 (30 min) | 86400 (1 day)      |

You can override these in either mode:

```nginx
pagespeed_cache_mode safe;
pagespeed_css_max_age 600;     # 10 min instead of 5
pagespeed_image_max_age 3600;  # 1 hour instead of 30 min
```

In safe mode, the response includes `must-revalidate` regardless of your
max-age value. The safety mechanism is `must-revalidate`, not the TTL.

### ASP.NET Core

```json
{
  "PageSpeed": {
    "CacheMode": "Safe",
    "CssMaxAgeSeconds": 300,
    "ImageMaxAgeSeconds": 1800
  }
}
```

| Option               | Type   | Default                          | Description                                      |
| -------------------- | ------ | -------------------------------- | ------------------------------------------------ |
| `CacheMode`          | string | `Safe`                           | `Safe` or `Aggressive`.                          |
| `CssMaxAgeSeconds`   | int    | 300 (safe) / 86400 (aggressive)  | Max-age for CSS and JS cache hits.               |
| `ImageMaxAgeSeconds` | int    | 1800 (safe) / 86400 (aggressive) | Max-age for image cache hits.                    |
| `HtmlMaxAgeSeconds`  | int    | 0                                | Max-age for HTML cache hits. 0 means `no-cache`. |

## What about `immutable`?

mod_pagespeed strips `immutable` from all transformed content in both modes.
Your origin may send `Cache-Control: immutable` for fingerprinted assets, but
mod_pagespeed's output depends on mutable state (configuration, software version,
capability detection). The long TTL from `pagespeed_immutable_max_age` is
preserved in aggressive mode — you get the cache performance without a false
immutability claim.

## CDN considerations

`must-revalidate` in safe mode means CDN edges must revalidate after TTL expiry.
If the origin is unreachable, the CDN returns 504 instead of serving stale
content. This is by design.

In aggressive mode, `stale-if-error=86400` allows CDN edges to serve stale
content for up to 24 hours during origin outages.

Some CDN configurations can override origin cache headers (e.g., Cloudflare
"Cache Everything" page rules with edge TTL). These CDN-level overrides take
precedence over mod_pagespeed's headers.

For CDN-specific guidance including Vary header recommendations, see
[CDN Integration](/docs/cdn-integration/).

<a id="cyclone-cache"></a>

## Native module cache storage

The in-process module (Apache, nginx, IIS) and the reverse-proxy worker both
build on Cyclone Cache, the memory-mapped cache backend (open source:
[cyclone-cache on GitHub](https://github.com/We-Amp/cyclone-cache)). The worker's own
cache path, sizing and threading flags are in the
[configuration reference](/docs/configuration/); the rest of this page is the
in-process module's cache reference:
[cache path and size](#native-module-file-cache),
[memory usage](#memory-usage-and-monitoring), the
[RAM tier](#ram-cache-tier) and [legacy LRU directives](#legacy-lru-directives),
[zero-copy serving](#zero-copy-serving), the
[shared memory metadata cache](#shared-memory-metadata-cache),
external cache backends ([memcached](#memcached) and [Redis](#redis)),
cache flushing and purging ([the flush file](#flush-the-entire-cache),
[the admin page](#purge-via-admin-page), [the PURGE method](#purge-via-http-purge-method),
and [multi-server scope](#multi-server-deployments)),
[cache sizing](#cache-sizing),
[IPRO](#in-place-resource-optimization-ipro-considerations),
[fetch and performance](#fetch-and-performance) and
[additional cache directives](#additional-cache-directives).

Key properties:

- **Fixed-size cache file** — the cache never grows beyond the configured size. When it fills, the least-recently-used entries are evicted automatically.
- **Lock-free reads** — concurrent requests read from the cache without blocking each other.
- **Memory-mapped I/O** — the operating system manages which parts of the cache file are held in RAM. Frequently accessed entries stay in memory without a separate in-process cache layer.
- **Shared across processes** — nginx worker processes and Apache prefork children all share the same cache file. No duplication, no coordination overhead.

### Cache path and size {#native-module-file-cache}

The `FileCachePath` directive is accepted for compatibility, but Cyclone
Cache uses its own storage location within that directory. Set the path and
size:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed FileCachePath /var/cache/pagespeed;
pagespeed FileCacheSizeKb 2097152;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedFileCachePath /var/cache/pagespeed
ModPagespeedFileCacheSizeKb 2097152
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache
pagespeed FileCacheSizeKb 2097152
```

The default cache path is `%ProgramData%\We-Amp\PageSpeed\Cache`. The IIS app pool identity must have write access to this directory. The installer configures permissions automatically.

On IIS, the cache file is shared across all worker processes in the same app pool. If multiple app pools serve different sites, each uses its own cache file within its configured `FileCachePath`.

</div>

The default cache size is sufficient for most sites. See [Cache sizing](#cache-sizing) below for guidance on larger deployments.

### Memory usage and monitoring

Because Cyclone Cache is memory-mapped, standard per-process memory metrics
can be misleading. Pages of the cache file that a worker has touched are
counted in that worker's resident memory (`RSS` in `top`/`htop`,
`VmRSS`/`VmHWM` in `/proc`), so a busy server can show workers whose resident
size approaches the size of the cache file. This is expected and healthy:

- The cache pages are **shared** — there is one copy in the operating
  system's page cache, no matter how many workers show it in their RSS.
  Summing RSS across workers counts that one copy once per worker.
- The pages are **reclaimable** — they are clean, file-backed memory, the
  first thing the kernel drops under memory pressure, with no writeback cost.
  They never contribute to out-of-memory conditions the way private
  allocations do.

To monitor actual per-worker memory consumption on Linux:

- **`RssAnon`** in `/proc/<pid>/status` is the private (anonymous) memory a
  worker really owns — this is the number to alert on and to compare across
  releases or configurations.
- **`Pss` / `Pss_File`** in `/proc/<pid>/smaps_rollup` give a deduplicated
  view if you need a single per-process figure that fairly splits shared
  pages.
- **Memory pressure** (`/proc/pressure/memory`, or `memory.pressure` in
  cgroup v2) tells you whether the system is actually short on memory —
  rising cache RSS with flat pressure means the OS is simply making good use
  of free RAM.

When setting container or cgroup memory limits, remember that the page cache
for the cache file counts against the limit but is evicted automatically as
the limit is approached. You do not need to reserve the full cache file size
as RAM; size limits for the application's private memory plus your desired
hot working set.

On Windows/IIS the same principle applies: shared cache file pages appear in
each worker process's working set but exist once in system file cache, and
are trimmed automatically under memory pressure.

### RAM cache tier

Cyclone Cache is memory-mapped, so cache hits are served straight from the mapped cache file and the operating system decides which entries stay in RAM. There is no separate per-process LRU cache layer in 1.15.

Cyclone can additionally keep a per-process RAM tier in front of the mapped file, controlled by `CycloneRamCacheKb`:

| Value    | Effect                                                                                         |
| -------- | ---------------------------------------------------------------------------------------------- |
| `0`      | RAM tier off — reads come from the memory-mapped cache file. Default in v1.15.0+r18 and later. |
| positive | RAM tier of that size in KB, per process.                                                      |
| `-1`     | Sizes the RAM tier from `LRUCacheKbPerProcess`. Default before v1.15.0+r18.                    |

Leave the RAM tier off unless a measurement says otherwise: the memory-mapped file already keeps hot entries in RAM through the page cache, so a RAM tier mostly duplicates those bytes — once per worker process.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed CycloneRamCacheKb 8192;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedCycloneRamCacheKb 8192
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed CycloneRamCacheKb 8192
```

</div>

### Legacy LRU directives

`LRUCacheKbPerProcess` and `LRUCacheByteLimit` are accepted for compatibility. With the Cyclone backend, `LRUCacheKbPerProcess` matters in two cases only: it sizes the RAM tier when `CycloneRamCacheKb` is `-1`, and it sizes the in-memory fallback cache when the Cyclone cache file cannot be created (for example, an unwritable cache directory). `LRUCacheByteLimit` has no effect with the Cyclone backend.

### Zero-copy serving

Two related options reduce or skip the per-request copy of cached response bodies. Zero-copy serving is **opt-in on all three platforms**.

**`CycloneZeroCopy`** reads cached hits directly from the memory-mapped cache file without copying the payload. It is off by default on every platform and is the switch that opts a server in.

**`CycloneZeroCopyServe`** then carries those memory-mapped bytes into the server's output buffer by reference (aliased) instead of copying them, with a safe copy-out before a cache eviction could overwrite the bytes. It only engages once `CycloneZeroCopy` maps cache values. On nginx it applies as soon as `CycloneZeroCopy` is on; on Apache and IIS the aliased serve activates only when the option is set explicitly.

To enable zero-copy serving:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed CycloneZeroCopy on;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedCycloneZeroCopy on
ModPagespeedCycloneZeroCopyServe on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed CycloneZeroCopy on
pagespeed CycloneZeroCopyServe on
```

</div>

Not every response is eligible for the aliased serve. A response is served by reference only when it is delivered verbatim — on Apache that means a plain-HTTP/1.x main-request `200` of at least 16&nbsp;KB with no `Range` request and no transforming output filter (`deflate`, TLS, HTTP/2). Anything else — for example a response that must be re-compressed for the client — automatically falls back to the regular copying path, and bytes an output filter holds for a slow client are copied out at that point.

Three [statistics](/docs/admin-console/) make the behavior observable: `zerocopy_serve_aliased` (responses served by reference), `zerocopy_serve_copied_out` (aliased bytes safely copied out before reuse), and `zerocopy_serve_ineligible` (requests that fell back to copied serving). The first fallback also logs a one-time message explaining why; on Apache that message needs `LogLevel info`, while the statistic is always on.

For the design background, see [zero-copy serving](/blog/memory-mapped-cache-zero-copy-serving/) and the [Cyclone vs. file-cache benchmark](/blog/cyclone-cache-vs-file-cache-benchmark/).

### Shared memory metadata cache

The shared memory metadata cache allows all processes on a server to share small metadata (response headers, cache invalidation records) through a shared memory segment. This avoids each process maintaining its own copy and reduces disk lookups.

| Directive                    | Default |
| ---------------------------- | ------: |
| `DefaultSharedMemoryCacheKB` |   51200 |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed DefaultSharedMemoryCacheKB 100000;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedDefaultSharedMemoryCacheKB 100000
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed DefaultSharedMemoryCacheKB 100000
```

</div>

In v1.15.0+r17 and later, every entry stored in the shared memory cache is also written to the Cyclone cache on disk. Shared memory serves hot reads; Cyclone holds the durable copy. After a restart, mod_pagespeed reads the metadata back from disk, so the server comes back warm instead of re-optimizing everything. Entries remain subject to normal cache capacity and eviction. Earlier 1.15 revisions started with a cold shared memory cache after a restart. `ShmMetadataCacheCheckpointIntervalSec` is a no-op from this revision on, superseded by the write-through.

Page properties — beacon-collected data such as critical image and selector information — get the same treatment in v1.15.0+r17 and later: they are written through to Cyclone as they are stored, so they survive restarts and do not have to be re-learned from live traffic.

In v1.15.0+r18 and later, this metadata and page-property data is held in a dedicated area of the cache, kept apart from optimized images and other large responses. Large files churning through the cache can no longer push this smaller, higher-value data out, so the information that lets a server come back warm after a restart stays available even under heavy image traffic. The share of the cache reserved for it is set by `FileCacheSmallTierPercent` (default 10; set it to `0` to turn the reservation off; values are capped at 50, so at most half the cache can be reserved). The reservation engages once the cache is large enough to set aside a dedicated region — roughly 256 MB or more; below that, all entries share the cache as before.

Two virtual hosts that share the same `FileCachePath` also share the same shared memory cache segment.

### External cache backends: memcached and Redis

The native module additionally supports memcached and Redis as external
cache backends — useful when multiple servers need to share cached
resources. **You cannot use both memcached and Redis for the same virtual
host.** Pick one.

#### Memcached

Point mod_pagespeed at one or more memcached servers:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MemcachedServers "cache1.example.com:11211,cache2.example.com:11211";
pagespeed MemcachedTimeoutUs 500000;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMemcachedServers "cache1.example.com:11211,cache2.example.com:11211"
ModPagespeedMemcachedTimeoutUs 500000
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed MemcachedServers 127.0.0.1:11211
pagespeed MemcachedTimeoutUs 500000
```

On Windows, install memcached as a Windows service:

1. Download the 64-bit memcached binary for Windows
2. Extract and open an elevated command prompt in the directory
3. Run `memcached -d install` to install the service
4. Run `net start memcached` to start the service

The default memory pool is 64 MB. To increase it, modify the service's `ImagePath` in the registry at `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\memcached`:

```text
"C:\memcached\memcached.exe" -m 256 -d runservice
```

A file cache (`FileCachePath`) is still required alongside memcached — memcached has size limits per object, so the file cache handles larger resources and cache flushing.

</div>

| Directive            | Default |
| -------------------- | ------: |
| `MemcachedServers`   |  (none) |
| `MemcachedTimeoutUs` |  500000 |

Health checking: if a memcached server exceeds 4 timeouts within 30 seconds, mod_pagespeed stops performing optimization for 30 seconds rather than blocking requests on a slow cache.

#### Redis

Point mod_pagespeed at a Redis server:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed RedisServer "cache.example.com:6379";
pagespeed RedisTimeoutUs 50000;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedRedisServer "cache.example.com:6379"
ModPagespeedRedisTimeoutUs 50000
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed RedisServer "cache.example.com:6379"
pagespeed RedisTimeoutUs 50000
```

</div>

| Directive            |        Default |
| -------------------- | -------------: |
| `RedisServer`        |         (none) |
| `RedisTimeoutUs`     |          50000 |
| `RedisDatabaseIndex` |              0 |
| `RedisTTLSec`        | -1 (no expiry) |

`RedisDatabaseIndex` selects a specific Redis database. This directive is incompatible with Redis clustering, which does not support database selection.

`RedisTTLSec` sets a TTL on all cache entries. The default of -1 means entries do not expire in Redis (mod_pagespeed manages its own expiration logic). Set a positive value if you need Redis to reclaim memory independently.

### Cache flushing and purging

#### Flush the entire cache

Touch a `cache.flush` file in the `FileCachePath` directory to force mod_pagespeed to discard all cached data. The filename is set by `CacheFlushFilename` (default `cache.flush`; a name that does not start with `/` is relative to `FileCachePath`). Use the cache directory your configuration names. The Apache package's `pagespeed.conf` sets `/var/cache/mod_pagespeed/`; the nginx package's sample configuration uses `/var/cache/ngx_pagespeed`:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```bash
sudo touch /var/cache/ngx_pagespeed/cache.flush
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```bash
sudo touch /var/cache/mod_pagespeed/cache.flush
```

</div>

mod_pagespeed checks the file's modification time every `CacheFlushPollIntervalSec` seconds (default 5). All entries cached before that time are treated as expired.

This works only while `EnableCachePurge` is off (the default). With `EnableCachePurge on` the module keeps its purge records in `cache.purge` instead (or in the file `CacheFlushFilename` names) and reads no timestamp from `cache.flush`, so touching a file does nothing: purge everything through the [global admin page](#purge-via-admin-page) (`purge=*`).

#### Purge via admin page

Purging through the admin interface is a `POST` that carries the `X-Requested-With: XMLHttpRequest` header; a command-line client such as `curl` works, as in the examples below. A plain `GET` answers `405`, and a `POST` without the header answers `403`. It also needs `EnableCachePurge` on (see [below](#purge-via-http-purge-method)): with it off, the page answers a normal response whose JSON body says `"success":false`, so a script should check that field rather than the status code alone. Send it from a client that your [access rules for the admin pages](/docs/admin-console/#setup) allow.

The per-vhost admin page (`/pagespeed_admin`) purges only URLs on the host name it was opened under, and refuses a URL on another host with `403`. Purging everything is accepted from the global admin page (`/pagespeed_global_admin`) only. A purge target that ends in `*` is a whole-cache purge (it does not purge only the URLs that start with it), so the per-vhost page refuses it too.

```bash
# Purge a specific URL (per-vhost admin page)
curl -X POST -H 'X-Requested-With: XMLHttpRequest' \
  'http://example.com/pagespeed_admin/cache?purge=http://example.com/style.css'

# Purge everything (global admin page only)
curl -X POST -H 'X-Requested-With: XMLHttpRequest' \
  'http://example.com/pagespeed_global_admin/cache?purge=*'
```

Releases up to and including 1.16.0 accepted a plain `GET` for these URLs. Scripts that purge by URL can also use the [HTTP PURGE method](#purge-via-http-purge-method) below, which is unchanged and needs no header.

#### Purge via HTTP PURGE method

When `EnableCachePurge` is on, mod_pagespeed accepts standard HTTP PURGE requests:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed EnableCachePurge on;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedEnableCachePurge on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed EnableCachePurge on
```

</div>

Then purge by URL:

```bash
curl -X PURGE http://example.com/style.css
```

#### Multi-server deployments

Cache purge and flush operations apply only to the server that receives the request. In a multi-server deployment, run the purge on every server independently. mod_pagespeed does not propagate purge requests across servers.

### Cache sizing

Size the Cyclone Cache file at 3 to 4 times the total size of your original assets. mod_pagespeed stores variants alongside the originals, so the cache needs space for both.

For example, if your site serves 500 MB of images, CSS, and JavaScript, set the cache to 1.5-2 GB.

In v1.15.0+r18 and later, the default cache size is 1 GB (earlier revisions defaulted to 100 MB). Cache files are sparse, so the larger default raises the ceiling rather than reserving the space up front — disk grows only as content is cached, and Cyclone self-evicts at the target. On hosts where disk is tight, set an explicit lower `FileCacheSizeKb`.

This is the native module's own cache file; the reverse-proxy worker's Cyclone
volume is sized separately — see [Sizing the Cache](/docs/worker-configuration/#sizing-the-cache)
in the configuration reference.

<a id="in-place-resource-optimization-ipro"></a>

### In-place resource optimization (IPRO) considerations

IPRO optimizes resources served from your origin without changing their URLs. This is useful for resources referenced by third-party code or cached at CDN edge nodes where you cannot change the URL.

IPRO is on by default.

| Directive                     | Default |
| ----------------------------- | ------: |
| `InPlaceResourceOptimization` |      on |
| `InPlaceSMaxAgeSec`           |      10 |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed InPlaceResourceOptimization on;
pagespeed InPlaceSMaxAgeSec 10;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedInPlaceResourceOptimization on
ModPagespeedInPlaceSMaxAgeSec 10
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed InPlaceResourceOptimization on
pagespeed InPlaceSMaxAgeSec 10
```

</div>

`InPlaceSMaxAgeSec` controls how long the optimized resource is considered fresh before mod_pagespeed checks the origin again. Keep this low (the default of 10 seconds is appropriate for most sites) to ensure changes propagate quickly.

Considerations when using IPRO:

- Resources keep their original URLs, so they do not benefit from content-hash-based cache extension. Browser caches and CDNs follow the original `Cache-Control` headers.
- IPRO cannot combine resources (CSS combining, JS combining) because each resource retains its own URL.
- In-place optimization does not record resources (stylesheets, scripts, images) whose responses carry `Vary: Referer` or any Vary beyond Accept-Encoding. An origin that answers differently by Referer must send `Vary: Referer`, so that such resources are not recorded.

### Fetch and performance

These directives control how mod_pagespeed fetches resources from your origin and how long it spends optimizing them per request.

| Directive                   |        Default | Description                                                                                                                                                                                                                       |
| --------------------------- | -------------: | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `FetcherTimeOutMs`          |           5000 | Maximum time to wait for a resource fetch from origin                                                                                                                                                                             |
| `RewriteDeadlinePerFlushMs` |             10 | Time budget per flush window for rewriting. If rewrites take longer, the original resource is served and the optimized variant is cached for subsequent requests                                                                  |
| `ImplicitCacheTtlMs`        | 300000 (5 min) | Cache TTL applied to resources that do not have explicit `Cache-Control` headers                                                                                                                                                  |
| `FetchWithGzip`             |            off | Fetch resources from origin using gzip `Accept-Encoding`. Turn this on if your origin supports gzip and the network between mod_pagespeed and the origin is a bottleneck                                                          |
| `HttpCacheCompressionLevel` |              9 | Compression level (-1 to 9; 0 = off) for storing resources in the cache. Level 9 maximizes disk space savings at the cost of slightly more CPU during cache writes; out-of-range values fail configuration load since v1.15.0+r18 |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed FetcherTimeOutMs 10000;
pagespeed RewriteDeadlinePerFlushMs 20;
pagespeed ImplicitCacheTtlMs 600000;
pagespeed FetchWithGzip on;
pagespeed HttpCacheCompressionLevel 6;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedFetcherTimeOutMs 10000
ModPagespeedRewriteDeadlinePerFlushMs 20
ModPagespeedImplicitCacheTtlMs 600000
ModPagespeedFetchWithGzip on
ModPagespeedHttpCacheCompressionLevel 6
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed FetcherTimeOutMs 10000
pagespeed RewriteDeadlinePerFlushMs 20
pagespeed ImplicitCacheTtlMs 600000
pagespeed FetchWithGzip on
pagespeed HttpCacheCompressionLevel 6
```

The IIS module uses WinHTTP for resource fetching. WinHTTP handles SSL certificate verification using the Windows certificate store — no `SslCertDirectory` or `SslCertFile` directives are needed.

</div>

### Additional cache directives

A few more cache-related directives are covered by their one-line summary in
the [configuration reference](/docs/configuration/): `CacheFragment` (default:
auto) sets the cache partition key; `PurgeMethod` (default: none) sets the
HTTP method used for cache purge; `RateLimitBackgroundFetches` (default: on)
rate-limits background fetches; `RedisReconnectionDelayMs` (default: 1000)
sets the Redis reconnection delay. `InPlaceRewriteDeadlineMs` (default: 10)
sets the IPRO rewrite deadline per flush, alongside `InPlaceSMaxAgeSec`
above.

See the [configuration reference](/docs/configuration/) for the full list of native
module directives and their defaults.
