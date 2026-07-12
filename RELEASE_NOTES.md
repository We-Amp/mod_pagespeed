# mod_pagespeed 1.15.0+r18 Release Notes

**Release date:** 2026-07-11
**Status:** Stable

## Overview

Performance, caching, and correctness release for the 1.15 line, with security
hardening across input validation and output escaping. Update recommended.

## Highlights

### Performance

- Cached resources are now served with far less copying. Apache and IIS serve
  memory-mapped cache hits zero-copy, and nginx gains an experimental
  `CycloneZeroCopyServe` mode that serves cache hits directly from the
  memory-mapped cache. Apache additionally streams optimized resource
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
