# Changelog

All notable changes to mod_pagespeed are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.15.0] - 2026-06-01

### Changed

- **Version line renumbered 1.1 -> 1.15**. mod_pagespeed 1.15 is the
  maintained continuation of the Apache-lineage codebase and the direct successor
  to Google's final mod_pagespeed release (1.14.36.1); the prior "1.1" numbering
  read as older than Google's line on package/version surfaces. This is a
  version-identity change with no functional change from the 1.1.0 line. Package
  names (`mod-pagespeed`, `nginx-module-pagespeed`, `ea-apache24-mod_pagespeed`)
  and the runtime behavior are unchanged; the `X-Mod-Pagespeed` header now reports
  `1.15.0.0`.

## [1.1.0-beta.1] - 2026-02-25

First release under We-Amp stewardship. This is a complete modernization of
the mod_pagespeed codebase, expanding from Apache-only to four deployment
platforms while preserving the full set of 40+ optimization filters.

### Added

#### New Platforms
- **Envoy filter** (experimental): PageSpeed as an Envoy HTTP filter with IPRO
  and HTML rewriting support. Uses `CurlUrlAsyncFetcher` (linked against
  BoringSSL) for independent outbound fetching. Includes standalone binary
  (~212MB) and shared library (~113MB).
- **nginx module** (stable): Dynamic module (`ngx_pagespeed_module.so`) for
  nginx 1.26.x stable and 1.27.x mainline. Full IPRO and HTML rewriting.
- **IIS module** (experimental): Native C++ IIS module (`pagespeed_iis.dll`)
  for Windows Server. Supports IPRO, HTML rewriting, streaming responses with
  chunked transfer encoding, and the full admin UI.

#### Build System
- **Bazel 7.x build system** replacing the legacy GYP/gyp_chromium toolchain.
  WORKSPACE-based dependencies with bzlmod disabled.
- **Docker development environment** with `docker-compose.yml` providing
  dev container, Redis, and Memcached services.
- **Clang + GCC 13 libstdc++** build configuration (`--config=clang-libstdcxx13`)
  for C++20/C++23 support.
- **Sanitizer configs**: `--config=clang-asan` (AddressSanitizer) and
  `--config=clang-tsan` (ThreadSanitizer).
- **Windows cross-compilation** via `--config=windows --config=clang-cl`
  using clang-cl toolchain.
- **GitHub Actions CI/CD** replacing Travis CI, with workflows for unit tests,
  Apache/Nginx/Envoy system tests, and automated release packaging.

#### Core Improvements
- **C++20 standard** (C++23 for Cyclone cache files via `per_file_copt`).
- **Cyclone cache** integration for high-performance disk caching.
- **CurlUrlAsyncFetcher** replacing Serf for HTTP fetching, linked against
  BoringSSL for HTTPS support.
- **APR decoupling**: Core libraries no longer depend on Apache Portable Runtime
  for non-Apache deployments.
- **libcurl linked against BoringSSL** on Linux (same instance used by Envoy,
  no symbol conflicts).

#### Testing
- **Python test framework** (`test/system/pagespeed_test_framework/`) shared
  across all four platforms with `fetch_until` polling, statistics delta
  checking, and WebP negotiation helpers.
- **Comprehensive system test suites**:
  - Apache: 195 pass, 14 skip
  - Nginx: 169 pass, 40 skip
  - Envoy: 189 pass, 20 skip
  - IIS: 350 pass (Python) + 19 pass (C++ unit)
- **pytest markers** for test categorization: `@pytest.mark.ipro`,
  `@pytest.mark.html_rewrite`, `@pytest.mark.slow`, etc.

#### Envoy-Specific
- Admin authentication with token, IP allowlist, and rate limiting.
- Circuit breaker for resource fetching.
- Redis cache backend support.
- Prometheus metrics endpoint at `/stats/prometheus`.
- Health endpoint at `/pagespeed/health`.

#### IIS-Specific
- `web.config` XML configuration parsing.
- Windows shared memory (`WindowsSharedMem`) for cross-process state.
- BCrypt API for cryptographic operations.
- Performance counter integration with Windows `perfmon.exe`.
- IIS Express support for development and testing.

### Changed

- **Version scheme**: Moved from Google's `1.15.0.0` four-part versioning to
  semantic versioning (`1.1.0-beta.1`).
- **Branding**: Updated from Google Inc. to We-Amp.
- **Apache 2.2 support dropped**: Only Apache 2.4+ is supported (single `.so`).
- **Build default paths**: Packaging scripts updated from GYP `out/Release/`
  to Bazel `bazel-bin/pagespeed/` paths.
- **`version.h` generation**: Now includes git short hash as `LASTCHANGE` and
  supports `PRERELEASE` field for pre-release versions.

### Removed

- GYP/gyp_chromium build system.
- Apache 2.2 module (`mod_pagespeed_ap24.so` split).
- Google-hosted update repository integration (cron jobs retained but
  repository config cleared).
- Travis CI configuration (replaced by GitHub Actions).

### Known Issues

#### Envoy (Experimental)
- `combine_css` may timeout due to async worker pool coordination.
- IPRO cache lifetime returns upstream `max-age` minus cache time instead of
  `implicit_cache_ttl_ms`.
- `X-PSA-Blocking-Rewrite` header is not supported (Envoy is non-blocking).
- Version header shows placeholders when built without workspace status.

#### IIS (Experimental)
- IPRO async cache stubs return misses (Redis/disk caches not functional).
- Beacon data silently discarded.
- Requires `allowDoubleEscaping="true"` in IIS for combined resource URLs.

## Previous Releases (Google Era)

For releases prior to We-Amp stewardship, see the
[Google mod_pagespeed release notes](https://www.modpagespeed.com/doc/release_notes).

The last official Google release was **1.14.36.1** (August 2020).
Version 1.15.0.0 was assigned in December 2018 but never released.

[1.1.0-beta.1]: https://github.com/we-amp/mod_pagespeed/releases/tag/v1.1.0-beta.1
