# mod_pagespeed 1.15.0+r19 Release Notes

**Release date:** 2026-07-17
**Status:** Stable

## Overview

Cache upgrade-safety release, plus zero-copy serving corrections. Update
recommended.

## Highlights

### Caching: version-safe cache files

- The cache now lives in a file fingerprinted by the bundled cache library's
  on-disk format version — not the mod_pagespeed release version. Format
  changes are anticipated to be infrequent, so most future upgrades will keep
  the cache warm. When the format does change (as it does in this release),
  old and new worker processes never open the same file, which removes a class
  of cache-corruption risk during upgrades, when both could briefly overlap on
  one cache file.
- **Upgrade note: the first start after upgrading to r19 begins with a cold
  cache.** Pages keep being served normally and re-optimization proceeds in the
  background, as with any cold cache.
- The previous version's cache file is left on disk untouched, so **rolling
  back to the previous package is warm** — it finds its cache exactly as it
  left it. Once you are confident you will not roll back, you can delete the
  older files in the cache directory to reclaim disk. Nothing is deleted
  automatically. (Cache files are sparse; apparent size overstates actual disk
  use.)
- Fixed: when a cache-directory hash bucket filled up entirely with
  current-version entries, new writes to that bucket were silently dropped (the
  entry was simply never cached, so affected resources were re-optimized on
  every request). Writes now land by evicting an existing entry, and a new
  `bucket_full_evictions` statistic makes the condition observable. The effect
  was most likely during upgrade-day write storms into a cold cache.

### Zero-copy serving (opt-in, now available on all three platforms)

- Cached resources can be served directly from the memory-mapped cache without
  copying the payload — available on nginx, Apache, and IIS.
- A correction to the r18 notes: they described zero-copy serving as on by
  default on nginx, but common configurations silently made every request
  ineligible, so it rarely engaged. That defect is fixed — and with r19 the
  feature is uniformly **opt-in on every platform** while it accrues production
  soak. On-by-default is planned for a future revision.
- To enable it:
  - nginx: `pagespeed CycloneZeroCopy on;`
  - Apache: `ModPagespeedCycloneZeroCopy on` and
    `ModPagespeedCycloneZeroCopyServe on`
  - IIS: `pagespeed CycloneZeroCopy on` and `pagespeed CycloneZeroCopyServe on`
- A new `zerocopy_serve_ineligible` statistic counts requests that fell back to
  copied serving, and a one-time log message explains the first fallback (on
  Apache this message needs `LogLevel info`; the statistic is always on).

### Performance and reliability

- The bundled cache library gains a lock-free read path — cache hits no longer
  take a lock — and no longer syncs to disk on every cache write (durability is
  periodic, and the power-loss window stays bounded), plus a hardening batch
  covering crash recovery and startup edge cases. Under write-heavy load,
  removing the per-write disk sync measured an order of magnitude higher
  sustained throughput in internal testing.
- Memory-mapped cache reads are verified before being promoted into the
  in-memory tier, hardening the serving path against torn or damaged entries.
- IIS: fixed a defect where optimization of a site's own sub-resources (CSS,
  JavaScript, images) could fail to converge on machines whose name resolution
  prefers the IPv6 loopback — pages then kept serving their original resources
  for minutes at a time. The server's internal fetches now pin the loopback
  address family explicitly and fall back to the other family automatically.
- Windows/IIS: cache-invalidation updates (purge requests) after the first one
  were silently discarded — the on-disk purge state never advanced, so later
  purges did not take effect across restarts or between worker processes.
  Atomic file replacement on Windows now works as intended and purges apply
  reliably.
- Windows/IIS: a cross-process guard now prevents one worker process from
  resetting a shared cache file while another process still has it mapped,
  closing a corruption window in multi-worker setups; two new statistics
  (`resets_gate_verified`, `resets_under_degraded_gate`) make gate health
  observable.
- IIS: fixed a race in the server's internal fetcher where a sub-resource fetch
  could be spuriously canceled just after its response had arrived. Because a
  failed internal fetch is remembered for several minutes, a single spurious
  abort could stall re-optimization of the affected resource well beyond the
  moment of failure, surfacing as intermittent optimization stalls.
- Fixed on all three platforms: behind a TLS-terminating proxy (when the
  `X-Forwarded-Proto` header is honored), the server's internal fetches for a
  page's own sub-resources combined the page's `https` scheme with the
  plain-HTTP listener port — a connection that could never succeed — so the
  affected CSS, JavaScript, and images were repeatedly re-fetched and never
  optimized. Internal fetches now use the protocol the listener actually
  speaks.
- Cache write-failure warnings are now rate-limited, so a persistent storage
  condition cannot flood the error log.
- The legacy JavaScript minifier (used when `UseExperimentalJsMinifier` is off)
  now minifies files containing ES2015 template literals; r18 passed such files
  through unmodified, r19 optimizes them.

### Experimental

- The native fetcher (`UseNativeFetcher`, nginx) remains off by default. Native
  HTTPS support has been introduced, so the fetcher can now retrieve `https://`
  resources directly. Enabling it requires a `resolver` directive in the nginx
  configuration.

## Platform Support

| Platform | Module | Status |
|----------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable |
| **Nginx 1.26+** | `ngx_pagespeed_module.so` | Stable |
| **IIS 10+** | `pagespeed_iis.dll` | Stable |

---

# mod_pagespeed 1.15.0+r18 Release Notes

**Release date:** 2026-07-11
**Status:** Stable

## Overview

Performance, caching, and correctness release for the 1.15 line, with security
hardening across input validation and output escaping. Update recommended.

## Highlights

### Performance

- Cached resources are now served with far less copying. Cache hits are
  served from the memory-mapped cache by reference (`CycloneZeroCopyServe`) —
  on by default on nginx, and an experimental opt-in on Apache and IIS — and
  an experimental fully zero-copy serve mode (`CycloneZeroCopy`, off by
  default) is available. Apache additionally streams optimized resource
  responses instead of double-buffering them, and nginx serves cached
  responses on HTTP/2 and HTTP/3 through a bounded copy ring.
- The default file cache size is raised to 1 GB.

### Caching

- Metadata and page-property cache entries are now stored in a dedicated
  small-object volume on file caches of ~256 MB or larger, keeping them warm
  across restarts independent of payload traffic. **Upgrade note: the first
  restart after upgrading rebuilds the payload cache once** on caches at or
  above that size (re-optimization proceeds normally from a cold payload
  cache; metadata is unaffected). Set `FileCacheSmallTierPercent 0` to
  disable.
- The cache's RAM tier is now sized independently of
  `LRUCacheKbPerProcess` via the new `CycloneRamCacheKb` directive
  (default 0: disabled — memory-mapped cache hits are already served from
  page cache).
- New cache observability counters on the admin console's caches page.

### Correctness and configuration

Configuration validation is stricter in this release; previously-accepted
invalid configurations may now fail to load, which is intentional:

- An invalid filter name in `?PageSpeedFilters=` now consistently rejects the
  whole query (rejection was previously position-dependent).
- Out-of-range values for bounded options now fail configuration load instead
  of being silently accepted: image qualities (-1..100), progressive JPEG
  scans (-1..10), `RewriteRandomDropPercentage` (0..100),
  `HttpCacheCompressionLevel` (-1..9), `CentralControllerPort` (1..65535).
- The `AddResourceHeader` limit of 20 headers is enforced exactly.
- Directive and option-scope matching is now case-consistent across all
  server ports, so scope enforcement can no longer be sidestepped by casing.
- The legacy JavaScript minifier (used when `UseExperimentalJsMinifier` is
  off) now passes files containing template literals through unmodified
  instead of corrupting them.

### Critical CSS and Content-Security-Policy

- `prioritize_critical_css` and other script-injecting filters now honor a
  restrictive Content-Security-Policy when `HonorCsp` is enabled, backing off
  instead of injecting scripts the policy would block, and the CSP policy
  engine received a set of correctness fixes.
- Inlined critical CSS preserves stylesheet charset fidelity and link
  attributes, and handles `@import`/`@keyframes` rules correctly.
- The critical-CSS beacon is viewport-aware, and beacon truncation is now
  observable in statistics instead of silently starving extraction.
- `lazyload_images` gains a native mode that emits `loading="lazy"` on
  below-the-fold images instead of injecting the JavaScript loader.

### IIS

- Fixed a defect in the IIS loopback fetcher where a sub-resource fetch that
  completed asynchronously could be treated as an empty response, suppressing
  optimization of the parent page for five minutes at a time — pages were
  intermittently served in their original form. Update recommended for IIS
  deployments.
- Configuration parsing is hardened: a malformed configuration line can no
  longer crash the module at startup, unknown options are reported instead of
  silently ignored, and option scoping is now enforced on IIS as on the other
  ports.

### Security hardening

This release hardens input validation and output escaping across the
rewriter, beacon handling, and configuration parsing. The bundled HTTPS
fetch library is updated to curl 8.21.0, which addresses a batch of
recently published curl vulnerabilities. No exploitation is known; update
recommended.

### Reliability

- The bundled Cyclone cache library is updated: deterministic teardown,
  key-verified directory election (a rare collision can no longer associate a
  cache entry with the wrong key), periodic directory sync for a tighter
  power-loss window (including on Windows), a fix for a startup race where
  multiple server processes opening the cache concurrently could corrupt or
  spuriously fail cache initialization (recovery after a crash during cache
  creation is now automatic), and roughly 4 MB less memory per cache stripe.
- A worker-pool sequence could be recycled while work was still queued;
  scheduler alarms are now driven from the event loop on nginx; the
  experimental native fetcher (`UseNativeFetcher`) gains native TLS support.
- Admin console reliability and usability pass.

## Platform Support

| Platform | Module | Status |
|----------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable |
| **Nginx 1.26+** | `ngx_pagespeed_module.so` | Stable |
| **IIS 10+** | `pagespeed_iis.dll` | Stable |

---

# mod_pagespeed 1.15.0 Release Notes

**Release date:** 2026-06-01
**Status:** Stable

## Overview

mod_pagespeed 1.15.0 renumbers the maintained Apache-lineage line from 1.1 to
1.15. 1.15 is the direct successor to Google's final mod_pagespeed
release (1.14.36.1) and reads as the newest maintained build on the package and
version surfaces (dnf/yum/apt, WHM EasyApache 4) where the prior "1.1" numbering
looked older than Google's line.

This is a version-identity change: there is **no functional change** from the
1.1.0 line. Package names (`mod-pagespeed`, `nginx-module-pagespeed`,
`ea-apache24-mod_pagespeed`) and repository channels are unchanged, so existing
install commands keep working. The `X-Mod-Pagespeed` response header now reports
`1.15.0.0`. For the full change history see [CHANGELOG.md](CHANGELOG.md).

---

_Historical release notes for earlier releases follow._

# mod_pagespeed 1.1.0-beta.1 Release Notes

**Release date:** 2026-02-25
**Status:** Pre-release (beta)

## Overview

This is the first release of mod_pagespeed under We-Amp stewardship. The
codebase has been extensively modernized: new build system (Bazel 7.x),
modern C++ (C++20/23), and three new deployment platforms alongside the
original Apache module.

## Platform Support

| Platform | Module | Status | Tested |
|----------|--------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable | 195 pass, 14 skip |
| **Nginx 1.26/1.27** | `ngx_pagespeed_module.so` | Stable | 169 pass, 40 skip |
| **Envoy** | `pagespeed_filter.so` / `envoy_pagespeed` | Experimental | 189 pass, 20 skip |
| **IIS 10+** | `pagespeed_iis.dll` | Experimental | 350 pass (Python), 19 pass (C++) |

## Quick Start

### Apache

```bash
# Ubuntu/Debian
sudo dpkg -i mod-pagespeed-beta_1.1.0-beta.1_amd64.deb
sudo a2enmod pagespeed
sudo systemctl restart apache2

# RHEL/Rocky
sudo rpm -i mod-pagespeed-beta-1.1.0-beta.1.x86_64.rpm
sudo systemctl restart httpd
```

### Nginx

```bash
tar xzf ngx_pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
sudo cp ngx_pagespeed-1.1.0-beta.1/ngx_pagespeed_module.so /usr/lib/nginx/modules/
# Add to nginx.conf: load_module modules/ngx_pagespeed_module.so;
sudo nginx -t && sudo systemctl restart nginx
```

### Envoy

```bash
tar xzf envoy-pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
cd envoy-pagespeed-1.1.0-beta.1
sudo mkdir -p /var/cache/pagespeed
./envoy_pagespeed -c pagespeed-envoy.yaml.sample
```

### IIS

Requires: Visual C++ Redistributable 2022

```powershell
Expand-Archive pagespeed-iis-1.1.0-beta.1-win-x64.zip C:\inetpub\pagespeed
Stop-Service W3SVC
New-WebGlobalModule -Name PageSpeedModule -Image 'C:\inetpub\pagespeed\pagespeed_iis.dll'
Start-Service W3SVC
```

### Build from Source

```bash
docker compose up -d
docker compose exec dev bash
bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so
```

See [docs/install-apache.md](docs/install-apache.md), [docs/install-nginx.md](docs/install-nginx.md),
[docs/install-envoy.md](docs/install-envoy.md), or [docs/install-iis.md](docs/install-iis.md) for
detailed instructions.

## What's New

- **Bazel build system** replacing GYP/gyp_chromium
- **C++20/C++23** with Clang + GCC 13 libstdc++
- **Cyclone cache** for high-performance disk caching
- **CurlUrlAsyncFetcher** replacing Serf (linked against BoringSSL)
- **Envoy filter** with IPRO, HTML rewriting, Redis cache, Prometheus metrics
- **Nginx dynamic module** for 1.26.x and 1.27.x
- **IIS native module** with full HTML rewriting and IPRO
- **Python test framework** shared across all platforms
- **GitHub Actions CI/CD** with automated release packaging
- **Docker development environment**

See [CHANGELOG.md](CHANGELOG.md) for full details.

## Known Limitations

### Envoy (Experimental)

- `combine_css` may timeout due to async worker pool coordination
- IPRO cache lifetime differs from Apache behavior
- `X-PSA-Blocking-Rewrite` header not supported (Envoy is non-blocking)
- See [docs/envoy-limitations.md](docs/envoy-limitations.md)

### IIS (Experimental)

- IPRO async cache returns misses (Redis/disk caches not functional for IPRO)
- Beacon data silently discarded
- Requires `allowDoubleEscaping="true"` in IIS configuration
- See [docs/iis-limitations.md](docs/iis-limitations.md)

### Nginx

- Lazyload images filter times out on some configurations
- Inline preview images filter times out
- See [docs/test-catalog.md](docs/test-catalog.md) for per-platform skip details

## Reporting Issues

Please report issues at: https://github.com/we-amp/mod_pagespeed/issues

Include:
- Platform (Apache/Nginx/Envoy/IIS) and version
- Operating system and version
- Configuration snippet (relevant PageSpeed directives)
- Steps to reproduce
