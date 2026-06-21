# CLAUDE.md

This file provides context and guidance for contributors (human and AI-assisted) working in this repository.

## Related Repositories

This repo is part of the We-Amp B.V. product family. Cross-repo context:

- **ModPageSpeed 2.0** (`We-Amp/pagespeed-optimizer`) — modern successor, shares Ed25519 licensing tokens
  - Customer-facing 1.1 docs live there at `website/src/content/docs-1.1/` (published to modpagespeed.com/1.1/docs/), NOT in this repo. `docs/*.md` here is internal/developer-facing.
- **Cyclone Cache** (`We-Amp/cyclone-cache`) — shared cache library, vendored at `vendor/cyclone/`

## Table of Contents
- [Project Overview](#project-overview)
- [Build System](#build-system)
- [Two Source Trees](#two-source-trees)
- [Code Architecture](#code-architecture)
- [Testing](#testing)
- [Code Style](#code-style)
- [Envoy Filter](#envoy-filter)
- [Windows/IIS Development](#windowsiis-development)
- [IIS Platform Internals](#iis-platform-internals)
- [Git Workflow Rules](#git-workflow-rules)

## Project Overview

mod_pagespeed is a web performance optimization middleware originally created by Google. It automatically applies 40+ optimization filters (image compression/resizing, CSS/JS minification, cache extension, etc.) to web pages.

**Deployment modes:**
- **Apache module** (`mod_instaweb`) - Traditional Apache HTTP Server integration
- **Envoy filter** - Modern proxy-based deployment
- **Windows/IIS** - Native IIS module (actively developed)

## Build System

**Bazel 7.x** with WORKSPACE-based dependencies (bzlmod disabled via `.bazelrc`).

### Docker Development (Required)

All builds must run inside the Docker container. The host toolchain is too old for C++23.

```bash
# Start environment (includes Redis and Memcached)
docker compose up -d
docker compose exec dev bash

# Inside container - preferred config uses Clang with GCC 13's libstdc++
bazel build --config=clang-libstdcxx13 //...

# Run C++ unit tests
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/... //test/net/...

docker compose down
```

**Build configs:**
| Config | Description |
|--------|-------------|
| `--config=clang-libstdcxx13` | **Preferred** - Clang + GCC 13 libstdc++ (C++23 support) |
| `--config=gcc` | GCC 13 native (works, slower diagnostics) |
| `--config=clang-asan` | Address sanitizer |
| `--config=clang-tsan` | Thread sanitizer |
| `--config=ci-linux-arm64` | ARM64 Linux (Docker on Apple Silicon or native ARM64); CI: `linux-arm64-build` job |

**Windows ASan**: `--config=win-asan` requires the LLVM ASan runtime DLL on PATH at test runtime:

```powershell
# The Windows CI runner installs/uses LLVM 19, so the ASan runtime lives under clang\19.
# Verify the LLVM version actually installed on your Windows box and adjust the path to match.
$env:PATH = "C:\Program Files\LLVM\lib\clang\19\lib\windows;$env:PATH"
bazelisk test --config=vendored --config=windows --config=clang-cl --config=win-asan //test/pagespeed/iis/...
```

ASan in IIS context requires `halt_on_error=0` (set in `dll_main.cc`) because `w3wp.exe` handles many requests and a halt would kill the process pool. ASan logs are written to `C:\pagespeed_asan*`.

**LLVM version divergence:** the Linux dev container and CI use **clang-20** (`docker/Dockerfile`, `clang-format-20`/`clang-tidy-20`). The Windows side uses **LLVM 19**, pre-installed on the Windows build machines. The Windows CI lanes (Windows Build, Windows Unit Tests, IIS System Tests, Windows ASan — maintainer-side) prepend the `C:\Program Files\LLVM\lib\clang\19\lib\windows` ASan runtime dir, and `.bazelrc` `build:win-asan` (lines ~343 and ~356) uses that same `LLVM\lib\clang\19\lib\windows` runtime path. The CI jobs and `.bazelrc` agree on the clang 19 runtime, so `win-asan` resolves the correct ASan runtime both in CI and on a local Windows box with LLVM 19 installed.

**Important:** Do NOT use `--jobs` to limit parallelism. Bazel manages resources automatically. Restricting jobs causes massive slowdowns especially for sanitizer builds.

**Build dependencies note:**
- **libcurl** is linked against BoringSSL on Linux for HTTPS support (same BoringSSL instance used by Envoy, no symbol conflicts)
- **Cyclone cache** uses HTTPS URL for broader Docker compatibility (SSH auth not always available)

### Key Targets

One canonical build command per port:

```bash
# Apache module
bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so

# Nginx module
bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_module.so

# Envoy filter
bazel build --config=clang-libstdcxx13 //pagespeed/envoy:envoy_pagespeed      # Standalone binary (~212MB)
bazel build --config=clang-libstdcxx13 //pagespeed/envoy:pagespeed_filter.so  # Shared library (~113MB)

# IIS module (Windows only — see Windows/IIS Development)
bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll
```

Building the Envoy targets requires the Envoy WORKSPACE — see [WORKSPACE Swap](#workspace-swap) below.

### WORKSPACE Swap

`WORKSPACE` is swapped in place. The default (lean) `WORKSPACE` supports Apache,
nginx, and IIS. For Envoy, run `scripts/use-envoy-workspace.sh` to activate the
Envoy dependency graph, and `scripts/use-envoy-workspace.sh --lean` to restore.

The canonical Envoy dependency graph lives in `WORKSPACE.envoy` — edit THAT for
Envoy deps, not the active `WORKSPACE` (the script overwrites it on swap).
`WORKSPACE.lean.bak` is the backup the script writes when activating Envoy.

## Two Source Trees

The engine spans two top-level source trees. Knowing which one owns a change is
the most common orientation mistake in this repo.

| Tree | Holds | Notes |
|------|-------|-------|
| `net/instaweb/` | ~57 rewriter filters (`net/instaweb/rewriter/*_filter.cc`, e.g. `cache_extender.cc`, `add_instrumentation_filter.cc`) plus their support code, `RewriteDriver`, and `RewriteOptions` (`net/instaweb/rewriter/rewrite_driver.cc`, `rewrite_options.cc`) — `net/instaweb/rewriter/` holds ~133 `.cc` in all | Legacy `net_instaweb::` namespace. To add or edit a rewriter filter, work HERE, not under `pagespeed/`. |
| `net/instaweb/{htmlparse,http,util,spriter}/` | Legacy HTML-parse / HTTP / util / image-spriter support | — |
| `net/instaweb/genfiles/` | Generated gperf/Closure outputs (minified `*_opt.js` / `*_dbg.js` bundles) | **GENERATED — DO NOT EDIT BY HAND.** `net/instaweb/genfiles/rewriter/*.js` are minified Closure output (30 bundles) and carry no in-file DO-NOT-EDIT marker. Edit the source `.js` and regenerate via `tools/regenerate-rewriter-js.sh`; the generated set is hash-guarded by `net/instaweb/rewriter/generated/.source-hash` (run `tools/regenerate-rewriter-js.sh --check-hash` to detect staleness). |
| `pagespeed/` | Modern subsystems: port adapters (`apache/`, `nginx/`, `envoy/`, `iis/`), `kernel/`, `system/`, `automatic/`, `controller/` | See [Code Architecture](#code-architecture). |
| `base/logging.h` | glog-compatible logging | At repo root, not under `pagespeed/` (see [Logging System](#logging-system)). |

There is no `rewriter/` directory under `pagespeed/` — an agent told "add a filter"
must look under `net/instaweb/rewriter/`.

## Code Architecture

### Core Components (`pagespeed/`)

| Directory | Purpose |
|-----------|---------|
| `kernel/` | Foundation: base utilities, HTML parsing, HTTP, caching, image processing, threading |
| `apache/` | Apache module integration |
| `envoy/` | Envoy filter (uses libcurl for fetching, not Envoy's HTTP client) |
| `iis/` | IIS native module |
| `system/` | System abstractions (admin UI, cache backends) |
| `automatic/` | ProxyFetch - standalone rewriting engine shared by all deployment modes |
| `controller/` | gRPC-based optimization coordination |

The admin console is a Svelte/Vite single-page app under `pagespeed/system/console/`. Its compiled output, `pagespeed/system/console/admin_console.html`, is a **GENERATED single-file Vite bundle — DO NOT EDIT BY HAND.** Edit the SPA source under `pagespeed/system/console/src/` and rebuild via `pagespeed/system/console/build.sh`; the checked-in bundle is guarded by the `console-drift` CI job (`pagespeed/system/console/check-no-drift.sh`) against `pagespeed/system/console/admin_console.html.srchash`.

### Key Abstractions

- **RewriteDriver** - Per-request optimization coordinator
- **RewriteDriverFactory** - Creates drivers, manages server-wide state
- **ProxyFetch** (`pagespeed/automatic/proxy_fetch.h`) - HTML rewriting engine used by Apache, Envoy, and IIS
- **ProcessContext** - Global singleton for domain registry, HTML keywords (must be constructed exactly once per process)

### Logging System

Custom glog-compatible logging in `base/logging.h` (at repo root, not under pagespeed/):
- `LOG(INFO/WARNING/ERROR/FATAL)`, `VLOG(level)`, `CHECK(condition)`, `DCHECK` variants
- Apache routes to error log via `pagespeed/apache/log_message_handler.cc`
- Envoy uses spdlog directly

### Dependencies

- Envoy HTTP proxy libraries, Protocol Buffers, gRPC
- APR/APRUtil/Serf (Apache module only)
- libjpeg-turbo, libpng, libwebp, giflib (image optimization)
- Brotli (compression)
- Cyclone Cache (high-performance disk cache, requires C++23)
- Python 3 (DRP code generation), Node.js (Closure Compiler via npx)

## Testing

### IIS Testing Strategy

IIS test coverage follows Apache's "golden standard" patterns. All IIS tests should mirror Apache test infrastructure and patterns.

**Test Hierarchy:**
1. **C++ Unit Tests** (`test/pagespeed/iis/`) - Fast, isolated component tests
2. **Python Integration Tests** (`test/iis/`) - HTTP-level filter verification
3. **System Tests** - Full end-to-end with running IIS server

**Test File Locations:**
| Type | Location | Purpose |
|------|----------|---------|
| C++ unit tests | `test/pagespeed/iis/*.cc` | Mock-based component testing |
| Python integration | `test/iis/test_*.py` | Filter behavior verification |
| Test fixtures | `test/iis/testsite/` | HTML/CSS/JS test content |
| Shared framework | `test/system/pagespeed_test_framework/` | Client, assertions |

**Key Test Patterns (from Apache):**
```python
# Pattern 1: fetch_until for async optimization
response = client.fetch_until_contains(url, pattern=r'\.pagespeed\.', timeout=30.0)

# Pattern 2: Statistics delta checking
old_stats = stats_snapshot()
client.get(url)
new_stats = stats_snapshot()
assert_stat_delta(old_stats, new_stats, "image_rewrites", 1)

# Pattern 3: Blocking rewrite for deterministic testing
response = client.get(url, headers={"X-PSA-Blocking-Rewrite": "psatest"})

# Pattern 4: WebP negotiation
response = client.with_webp().get(url)
```

**Pytest Markers:**
- `@pytest.mark.ipro` - IPRO (In-Place Resource Optimization) tests
- `@pytest.mark.html_rewrite` - HTML rewriting tests
- `@pytest.mark.slow` - Long-running stress tests
- `@pytest.mark.iis_only` - IIS-specific tests (not shared)

### C++ Unit Tests

```bash
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/... //test/net/...
```

On Linux, IIS/Windows-only tests are skipped. On Windows, rewriter shards 8-9 may time out (pre-existing, test too large for the default timeout). For current pass/skip counts per platform, see `docs/test-catalog.md`.

Tests mirror source layout: `test/pagespeed/kernel/base/` tests `pagespeed/kernel/base/`.

### System Tests (Integration)

Require a running server.

There are two `run_system_tests.sh`: `test/system/run_system_tests.sh` is the real
runner (change test logic THERE); `scripts/run_system_tests.sh` is a Docker
convenience wrapper that builds the Apache module and drives the real runner.

```bash
# Apache system tests
./test/system/run_system_tests.sh
./test/system/run_system_tests.sh -k sanity  # Specific tests

# Envoy system tests
./test/system/run_envoy_tests.sh

# Nginx system tests
./test/system/run_nginx_tests.sh

# IIS system tests (from Linux host, requires Windows VM)
./test/system/run_iis_tests.sh sanity
```

### Test Infrastructure Notes

- Apache tests use `//pagespeed/apache:apache_core` (excludes `mod_instaweb.cc`) to avoid ProcessContext conflicts
- TSAN suppressions: `tools/tsan_suppressions.txt`
- Tests use 10 shards by default
- See `docs/test-catalog.md` for comprehensive skip/xpass documentation per platform

## Code Style

- **C++20** standard (C++23 for Cyclone cache files via `per_file_copt`)
- Google C++ style (`.clang-format`)
- 80-column line limit

### Linting & Formatting (match CI locally)

These scripts run inside the dev container and reproduce the CI `clang-format
check` and clang-tidy steps exactly (both pinned to version 20). Run them before
pushing to avoid a CI round-trip:

| Command | Does |
|---------|------|
| `tools/fix-format.sh` | Reformats all in-scope C/C++ sources under `pagespeed/` and `net/` with `clang-format-20`, matching CI's scope (excludes `pagespeed/iis/*` and vendored files carrying the `DO NOT EDIT BY HAND` banner). |
| `clang-format-20 -i <file>` | Reformat a single file. |
| `tools/tidyup.sh [--fix]` | Runs `clang-tidy-20` (`run-clang-tidy-20`); `--fix` applies auto-fixes. |
| `tools/install-hooks.sh` | One-time per clone: points `core.hooksPath` at `.githooks/` (pre-commit format check). Bypass once with `git commit --no-verify`. |

## Envoy Filter

### Features

- **IPRO** (In-Place Resource Optimization) for CSS/JS/images
- **HTML rewriting** via ProxyFetch (same engine as Apache/IIS)
- **libcurl fetcher** (`CurlUrlAsyncFetcher`) - independent of Envoy's ClusterManager

### Architecture

```
Client Request → decodeHeaders() → Origin Response → encodeHeaders() (detect HTML)
    → CreateNewProxyFetch() → encodeData() (stream to ProxyFetch)
    → Done() → EnvoyAsyncFetch::HandleDone() → dispatcher_.post() → sendReply()
```

ProxyFetch runs on worker threads; `EnvoyAsyncFetch` bridges to Envoy's dispatcher thread via `shared_from_this()` and atomic flags.

### Production Configuration

See `pagespeed-envoy.yaml` for complete example including:
- Admin authentication (`admin_auth` with token, IP allowlist, rate limiting)
- Circuit breaker for resource fetching
- Redis cache backend
- Prometheus metrics at `/stats/prometheus`
- Health endpoint at `/pagespeed/health`

**Key metrics:** `pagespeed.html_rewrites_total`, `pagespeed.ipro_cache_hits`, `pagespeed.rewrite_latency_ms`

### Envoy System Test Status

Run `./test/system/run_envoy_tests.sh`; see `docs/test-catalog.md` for current pass/skip counts.

The Envoy skips are documented limitations (marked `@pytest.mark.not_envoy`):
- **Statistics** - `resource_404_count` not tracked
- **IPRO cache headers** - Different cache header behavior
- **CssFlattenMaxBytes header** - Header handling differs
- **Query params header** - PageSpeedFilters header not respected for IPRO
- **Other** - Content-Length, HTTPS combination

Run tests:
```bash
./test/system/run_envoy_tests.sh automatic/ -v  # All automatic tests
./test/system/run_envoy_tests.sh -k sanity      # Quick sanity check
```

See `ENVOY_TEST_PROGRESS.md` for detailed tracking.

### Apache System Test Status

Run `./test/system/run_system_tests.sh`; see `docs/test-catalog.md` for current pass/skip counts.

### Nginx System Test Status

Run `./test/system/run_nginx_tests.sh`; see `docs/test-catalog.md` for current pass/skip counts.

The nginx skips include all `not_envoy` tests, plus nginx-specific skips.

Run tests:
```bash
./test/system/run_nginx_tests.sh
```

### Nginx Thread Model

The nginx module uses a Unix pipe (`NgxEventConnection` in `pagespeed/nginx/ngx_event_connection.cc`) for cross-thread signaling. PSOL worker threads write to `pipe_write_fd_` to signal nginx's event loop, which calls `ReadCallback` on the nginx thread.

**Rules:**
- Never call nginx C API functions (`ngx_http_*`) directly from a PSOL worker thread. Only call them from the event loop thread (inside `ReadCallback` or functions it calls).
- New filter header modifications that need to reach nginx must be buffered and transmitted via the pipe mechanism, not via direct nginx API calls.
- The pipe capacity matters on high-throughput systems — a full pipe causes silent dropped events.

### Known Differences from Apache

| Issue | Cause |
|-------|-------|
| `combine_css` timeouts | Async worker pool coordination in streaming architecture |
| IPRO cache lifetime | Fixed: static file server now omits explicit Cache-Control (matching Apache), so PSOL uses `implicit_cache_ttl_ms` |
| IPRO ETag format | Fixed: PSOL core correctly sets `PSA-aj` ETag via `FixFetchFallbackHeaders` |
| `X-PSA-Blocking-Rewrite` | Supported via `blocking_rewrite_key` config option |
| Version shows placeholders | Missing genrule for `version.h.in` processing |

**`combine_css` timeout details**: Nginx streams response chunks through `OnSendResponse` → `NgxBaseFetch::Write()` → ProxyFetch. When ProxyFetch needs to fetch sub-resources (CSS files to combine) before it can emit output, and the sub-resource fetch takes longer than nginx's `send_timeout`, nginx terminates the connection. The fix pattern is to pre-fetch all sub-resources before beginning the response, then reply with the combined output.

## Windows/IIS Development

### Quick Start (Direct Windows)

```powershell
cd C:\pagespeed
git pull

# Build IIS module (requires clang-cl)
bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll

# Install in IIS
Stop-Service -Name W3SVC
Copy-Item bazel-bin/pagespeed/iis/pagespeed_iis.dll C:\inetpub\pagespeed\ -Force
New-WebGlobalModule -Name PageSpeedModule -Image 'C:\inetpub\pagespeed\pagespeed_iis.dll'
Start-Service -Name W3SVC
```

### Vendored/Offline Builds (from Source Tarball)

Release tarballs include all dependencies for offline builds (no SSH key or network access needed):

```powershell
# Extract the source tarball
tar xf mod_pagespeed-<version>.tar.gz
cd mod_pagespeed-<version>

# Build IIS module from vendored deps
bazel build --config=vendored --config=windows --config=clang-cl -c opt //pagespeed/iis:pagespeed_iis.dll

# Package
.\install\iis\build_iis_package.ps1 -OutputDir C:\output
```

The `--config=vendored` flag uses the `vendor/repo-cache` repository cache and the vendored Cyclone source. The tarball is produced by `tools/vendor-deps.sh` in CI. The `git_repository` dep (Cyclone) is vendored — no SSH keys or network access needed.

### Remote Development (Linux → Windows VM)

```bash
# Start Windows VM (first boot: 15-30 min)
./windows-dev/start-windows-dev.sh
./windows-dev/wait-for-windows.sh

# Build via SSH
./windows-dev/build-on-windows.sh //pagespeed/iis:pagespeed_iis.dll

# Interactive shell
./windows-dev/build-on-windows.sh --shell
```

**Connection:** `ssh -p 2222 Developer@localhost` (password: `ChangeMe1`)

**SSH Agent Required:** Set `SSH_AUTH_SOCK` before running Windows build commands.

### IIS Module Architecture

See `pagespeed/iis/CLAUDE.md` for the authoritative file map (key files, request-pipeline notifications, state-machine flags). Two facts worth restating here:

- **Config format:** `pagespeed.config` (primary) / `iiswebspeed.config` (fallback) — a flat file parsed with RE2 match rules (`iis_configuration.cpp`/`.h`). `web.config` is module registration only (`New-WebGlobalModule` + `requestFiltering`), not PageSpeed configuration.
- **Single server context:** The IIS module uses a single `IisServerContext` for all requests within an application pool. This simplified architecture avoids lifetime management issues that can occur with per-site contexts.

### Build Status

| Target | Status |
|--------|--------|
| `//pagespeed/kernel/base:pagespeed_base` | Builds |
| `//pagespeed/system:system_windows` | Builds |
| `//pagespeed/iis:pagespeed_iis.dll` | Builds, fully functional (IPRO + HTML rewriting) |

### Running IIS Tests

#### C++ Unit Tests (on Windows)

```powershell
# Build and run all IIS unit tests
bazel test --config=windows --config=clang-cl //test/pagespeed/iis:all
```

#### Python Integration Tests (on Windows)

```powershell
cd test\iis

# Setup IIS Express with the built module (auto-detects bazel-bin DLL)
powershell -ExecutionPolicy Bypass -File Setup-IISExpress.ps1 -Port 8080

# Start IIS Express in background
powershell -ExecutionPolicy Bypass -File Start-IISExpress.ps1 -Background -Wait -Port 8080

# Run all tests
cmd /c "set PAGESPEED_PORT=8080& set IIS_EXPRESS=1& set PAGESPEED_TEST_ROOT=& set PAGESPEED_EXAMPLE_ROOT=& python -m pytest test/iis/ -v"

# Run specific test category
cmd /c "set PAGESPEED_PORT=8080& set IIS_EXPRESS=1& set PAGESPEED_TEST_ROOT=& set PAGESPEED_EXAMPLE_ROOT=& python -m pytest test/iis/ -m ipro -v"

# Run stress tests (slower)
cmd /c "set PAGESPEED_PORT=8080& set IIS_EXPRESS=1& set PAGESPEED_TEST_ROOT=& set PAGESPEED_EXAMPLE_ROOT=& python -m pytest test/iis/test_stress.py -v"

# Stop IIS Express when done
powershell -ExecutionPolicy Bypass -File Stop-IISExpress.ps1
```

**Important environment variables:**
- `PAGESPEED_PORT` - Port where IIS/IIS Express is running (8080 for IIS Express)
- `IIS_EXPRESS=1` - Set when testing against IIS Express
- `PAGESPEED_TEST_ROOT=` - Must be empty (not `/`) to avoid double-slash URLs
- `PAGESPEED_EXAMPLE_ROOT=` - Must be empty (not `/`) to avoid double-slash URLs

**web.config Configuration:**
- If PageSpeedModule is registered globally via `New-WebGlobalModule`, the site's `web.config` must NOT add the module again
- A duplicate module entry causes 500.19 errors with "Cannot add duplicate collection entry"
- Minimal web.config should be empty `<configuration></configuration>` or removed entirely

**Module Freshness:**
- After rebuilding the DLL, IIS must be restarted to pick up changes
- Use: `Stop-Service W3SVC; Copy-Item ...; Start-Service W3SVC`
- Or recycle the app pool: `Restart-WebAppPool DefaultAppPool`

**Note:** If full IIS has a site on the same port, it will handle requests instead of IIS Express. Check with `Get-Website` in PowerShell and verify the physical path with `netstat -ano | grep :8080`.

#### System Tests from Linux Host

```bash
./test/system/run_iis_tests.sh --no-module sanity  # Baseline without module
./test/system/run_iis_tests.sh sanity              # With PageSpeed module
```

Tests in `test/iis/` (26 Python files):
- **Sanity**: `test_sanity.py`, `test_admin.py`
- **HTML rewriting**: `test_html_rewrite.py`, `test_html_rewrite_crash.py`
- **Filter-specific**: `test_extend_cache.py`, `test_defer_js.py`, `test_lazyload.py`, `test_combiners.py`, `test_css_minify.py`, `test_js_minify.py`, `test_flatten_imports.py`, `test_inliners.py`, `test_preload.py`, `test_responsive.py`, `test_webp.py`, `test_move_css.py`, `test_local_storage.py`
- **HTTP/Images**: `test_headers.py`, `test_image_resize.py`, `test_html_optimization.py`, `test_dns_prefetch.py`, `test_cache_control.py`
- **IPRO**: `test_ipro.py`
- **Stress/Error**: `test_stress.py`, `test_error_recovery.py`

C++ unit tests in `test/pagespeed/iis/` (`*_test.cc`, e.g.):
- Core: `iis_config_test.cc`, `iis_server_context_test.cc`, `iis_module_factory_test.cc`
- HTTP: `iis_http_module_test.cc`, `iis_header_util_test.cc`, `iis_base_fetch_test.cc`
- Streaming: `iis_streaming_fetch_test.cc`, `iis_dechunker_test.cc`, `iis_content_decoder_test.cc`
- Integration: `iis_rewrite_integration_test.cc`, `iis_html_flushing_integration_test.cc`

### Platform Differences

| Component | Windows/IIS | Linux/Apache |
|-----------|-------------|--------------|
| Crypto | BCrypt API | BoringSSL |
| Shared Memory | InProcessSharedMem (per-process; cross-process not implemented) | PthreadSharedMem |
| External Cache | Redis only | Redis + Memcached |
| Version Header | `X-PageSpeed` | `X-PageSpeed` |
| Config Format | `pagespeed.config` / `iiswebspeed.config` (flat file, RE2 rules); `web.config` = module registration only | httpd.conf directives |

### Current Test Coverage (IIS)

Run `./test/system/run_iis_tests.sh sanity` (Linux host + Windows VM) or the per-suite pytest invocations below; see `docs/test-catalog.md` for current pass/skip counts per platform. Coverage spans connectivity/headers (`test_sanity.py`), admin UI/health (`test_admin.py`), IPRO (`test_ipro.py`), HTML rewriting (`test_html_rewrite.py`), combiners (`test_combiners.py`), cache headers (`test_cache_control.py`, `test_headers.py`), and preload/DNS-prefetch hints (`test_preload.py`, `test_dns_prefetch.py` — both require a warm pcache).

### Build Performance on High-Core Machines (>64 cores)

Windows splits processors into groups of 64. By default, Bazel's JVM and all
child processes are confined to one group. On a 128-thread machine this means
half the cores sit idle. Two local fixes are needed (both go in `user.bazelrc`,
which is gitignored):

1. **JDK upgrade**: Install JDK 21.0.6+ (e.g. `winget install Azul.Zulu.21.JDK`)
   and add to `user.bazelrc`:
   ```
   startup --server_javabase="C:/Program Files/Zulu/zulu-21"
   startup --host_jvm_args=-XX:+UseAllWindowsProcessorGroups
   ```

2. **Move output_base to ReFS/Dev Drive**: Bazel's default output_base on C:
   (NTFS) causes MFT lock contention with many concurrent compilations. Add:
   ```
   startup --output_base=D:/_bazel
   ```

3. **Minifilters**: Run `fltmc filters` (admin) and check for `bindflt` — it
   can add up to 3x overhead on file metadata operations. Unload with
   `fltmc unload bindflt`. Also consider unloading `wcifs` and `CldFlt`.

4. **Job count**: Use `--jobs=96` on 128-thread machines to avoid Windows
   `CreateProcessW` OOM errors. The `--local_resources=cpu=HOST_CPUS` in
   `pagespeed.bazelrc` already advertises all cores for scheduling.

### Known Issues

1. **googleurl requires clang-cl** - LLVM installed automatically by provisioning
2. **Git Bash path conversion** - When running pytest from Git Bash, paths like `/` get converted to Windows paths. Use `cmd /c` to run tests with environment variables
3. **yq symlink on Windows** - Fixed via `bazel/envoy_repo_yq_windows.patch`. Windows CreateProcessW cannot execute symlinks, so the patch uses the direct path to `@yq_windows_amd64//:yq.exe`
4. **Winsock header ordering** - On Windows, `winsock2.h` must be included BEFORE `windows.h` and `httpserv.h` to avoid redefinition errors (fd_set, timeval, hostent)
5. **clang-cl defines `_WIN32` not `WIN32`** - Use `#if defined(_WIN32) || defined(WIN32)` for Windows-specific code
6. **IIS static files lack Cache-Control** - IIS serves static CSS/JS without Cache-Control headers by default; IPRO-processed resources get proper cache headers
7. **Noscript insertion for old browsers** - PageSpeed adds noscript fallback tags for older user-agents (e.g., Chrome 6), which can increase HTML size
8. **IIS `allowDoubleEscaping` required** - Combined resource URLs contain `+` characters (e.g., `a.css+b.css.pagespeed.cc.HASH.css`) which trigger IIS RequestFilteringModule 404.11 errors. Set `<requestFiltering allowDoubleEscaping="true" />` in `applicationhost.config` or `web.config`. The `Setup-IISExpress.ps1` script configures this automatically
9. **IIS `Clear()` clears headers AND body** - `IHttpResponse::Clear()` clears both response headers and entity body, not just the body. Headers set before `Clear()` in `OnSendResponse` will not survive. Headers must be set after `Clear()` or in `HandleDone()`/`HandleHeadersComplete()`
10. **IIS Express vs full IIS port conflict** - If full IIS (W3SVC, PID 4) is running on the same port, it handles requests instead of IIS Express. Stop W3SVC/WAS before starting IIS Express: `Stop-Service W3SVC -Force; Stop-Service WAS -Force`. Verify with `netstat -ano | grep :8080`
11. **IIS Express `allowDefinition` must use `AppHostOnly`** - When full IIS is also installed, `allowDefinition="MachineOnly"` in IIS Express's `applicationhost.config` causes startup failure ("Configuration section can only be set in machine.config"). Use `AppHostOnly` instead

See `windows-dev/README.md` for detailed troubleshooting.

### CI-Specific Notes

The Windows CI runs as a GitHub Actions runner **service** (not interactive), which has a minimal environment:

- **`--config=clang-cl` is required** — googleurl `#error`s on plain MSVC. The CI workflow must use `--config=vendored --config=windows --config=clang-cl`.
- **`--incompatible_strict_action_env`** (`.bazelrc` line 29) sanitizes the environment. Env vars like `PROGRAMFILES` must be set explicitly with a value in `--action_env=VAR=value`, not just forwarded with `--action_env=VAR`.
- **PATH must include Python and Git tools** — the service doesn't inherit the interactive user's PATH. Python must come before Git's `usr/bin` (Git ships a broken `python3` stub). Set per-step in the CI workflow.
- **`--workspace_status_command`** — the tarball has no `.git` directory, so the default workspace status script (`bazel/get_workspace_status`) fails. Override with `--workspace_status_command="cmd /c echo."` for Windows.
- **Runner services** run as Windows services managed via `sc.exe`, set to `start=auto` for boot persistence.

## IIS Platform Internals

Critical knowledge for anyone modifying IIS module code. These constraints are not obvious from the code structure alone.

### String Encoding

The helpers `s2ws()` and `ws2s()` in `iis_misc.cpp` and `util.cpp` use `CP_ACP` (ANSI Code Page) — they are only correct on US English / Latin-locale Windows. On any machine with a non-Latin ANSI code page (e.g., Asian-locale Windows Server), these conversions silently corrupt Unicode characters in paths and host names.

**Rule**: All new string conversions between `std::wstring` (IIS APIs) and `std::string`/`GoogleString` (PSOL) must use `CP_UTF8`. Use `WideCharToMultiByte(CP_UTF8, ...)` and `MultiByteToWideChar(CP_UTF8, ...)` directly. The URL conversion helpers in `iis_utils.cc` and the inline conversion in `iis_http_module.cpp` are the correct models.

### IIS Request Lifecycle

- **`RQ_NOTIFICATION_PENDING`** is a destructive commitment: returning this means IIS will not complete the request normally. The module must call `IndicateCompletion()` or `PostCompletion()` exactly once.
- **`http_context_`** is nulled immediately after `IndicateCompletion()` (`iis_module_base_fetch.cpp`). Any code that accesses `http_context_` after that call on any thread will crash.
- **`OnAsyncCompletion`** is called on a thread pool thread, not the original IIS thread. PSOL objects (`RewriteDriver`, `ProxyFetch`) are NOT thread-safe — do not access them from async callbacks without synchronization.
- **Multiple `OnSendResponse` calls**: IIS fires this potentially multiple times for chunked responses. Check `HTTP_SEND_RESPONSE_FLAG_MORE_DATA` to determine if more data is coming.

### Header-Before-Body Constraint

IIS modules in `RQ_SEND_RESPONSE` see response headers at the same time as (or just before) the first body chunk, but cannot re-transmit headers after they have been sent. Any filter that conditionally modifies HTTP response headers based on HTML body analysis must be disabled for IIS.

Currently, `convert_meta_tags` is forbidden for this reason (the `ForbidFiltersByCommaSeparatedList` call in `iis_rewrite_driver_factory.cpp`). If adding new filters that modify `Content-Type`, `Vary`, or custom headers based on body scanning, add them to that call.

### Frozen RewriteOptions

After `RewriteOptions` are initialized from the configuration file, they are frozen (immutable). Any attempt to modify options after initialization (e.g., via COM API or runtime reconfiguration) will silently fail — the options object rejects mutations after freeze. This affects:
- Dynamic filter enable/disable at runtime
- Any attempt to change options from `OnBeginRequest` based on per-request conditions
- COM API calls that try to modify page speed settings after app pool start

Per-request option overrides (e.g., from query parameters) work through a different mechanism (`determine_options` in `iis_misc.cpp`) that creates a request-scoped copy, not by mutating the frozen global options.

### Shared Memory (Correction)

The IIS module currently uses `InProcessSharedMem`, NOT `WindowsSharedMem`. Each `w3wp.exe` worker process has independent statistics — there is no cross-process statistics sharing on IIS. `WindowsSharedMem` exists in the codebase (`pagespeed/kernel/sharedmem/windows_shared_mem.h`) but is not wired in.

### App Pool Identity and Permissions

The IIS module runs as `IIS AppPool\<PoolName>` (`ApplicationPoolIdentity`) by default. This account has no rights to:
- `%ProgramData%\We-Amp\IISWebSpeed\` (config directory)
- Any custom `FileCachePath` directory

Grant `IIS_IUSRS` `ReadAndExecute` on the config directory and `Modify` on the cache directory. The cache path write test in `IisProcessContext::GetServerContext()` passes in dev (where the machine account is used) but fails in production without these permissions.

### `__x_` Header Prefix Convention

Headers prefixed with `__x_` in `PopulateResponseHeaders()` are internal IIS-specific bookmarks for cache-related headers (Expires, ETag, Last-Modified) that must survive PSOL processing and be re-applied to the final response. Do not add new uses of this prefix without understanding the full header-replay path in `IisModuleBaseFetch::HandleHeadersComplete()`.

### DllMain Loader Lock

`ProcessContext` is constructed in `DllMain(DLL_PROCESS_ATTACH)` (`dll_main.cc`), which runs under the Windows loader lock. Do NOT:
- Call any PSOL API that triggers dynamic library loading from inside `DllMain`
- Add initialization to `DllMain` that allocates resources requiring other DLLs
- Use static constructors in translation units linked into the IIS DLL that load DLLs

Apache creates `ProcessContext` in `ap_hook_pre_config` (no loader lock). Envoy creates `ProcessContext` lazily via `EnvoyProcessContext` constructor (in `http_filter_config.cc`). The IIS path is the most constrained.

### Two IisRewriteDriverFactory Files (edit the live one)

There are TWO files named `iis_rewrite_driver_factory`. Editing the wrong one
leaves the build green while the real code is untouched.

- **LIVE:** `pagespeed/iis/iis_rewrite_driver_factory.cpp` (721 lines) — the IIS-module factory. `ForbidFiltersByCommaSeparatedList` (see [Header-Before-Body Constraint](#header-before-body-constraint)) and the port-80 hardcoding below live here. Make IIS-module changes HERE.
- **BUILD-INERT:** `pagespeed/windows/iis_rewrite_driver_factory.cc` (85 lines) — a separate experimental C-API DLL factory. Its `pagespeed/windows/BUILD` target is `tags = ["manual"]`, so it is excluded from `//pagespeed/...` builds. Do not edit it for IIS-module changes.

### Port 80 Hardcoding (Known Limitation)

`IisRewriteDriverFactory` is constructed with hardcoded port `80` (`pagespeed/iis/iis_rewrite_driver_factory.cpp` — the live factory, see above). For HTTPS listeners on port 443 or non-standard ports, the loopback fetch URL constructed by `LoopbackRouteFetcher` will use the wrong port for IPRO resource back-fetching. This is a known bug (see `TODO: fix port!` in `iis_server_context.cpp:32`).

## Supplementary Documentation

- `DEVELOPER.md` - Contributor setup: Docker dev container, native Linux deps, single-test examples, common build issues
- `docs/GLOSSARY.md` - Load-bearing terms (PSOL, IPRO, instaweb, ProxyFetch, DomainLawyer, beacon, CLFUS)
- `docs/README.md` - Index of the `docs/` tree
- `windows-dev/README.md` - Detailed Windows/IIS setup
- `pagespeed-envoy.yaml` - Production Envoy configuration
- `test/system/pagespeed_test_framework/` - Shared system-test framework (client, assertions)
- `test/system/cpanel/README.md` - cPanel system-test notes
- `pagespeed/envoy/README.md` - Envoy filter internals
- `ENVOY_TEST_PROGRESS.md` - Envoy test validation tracking and known limitations
- `docs/plans/` - Historical planning documents (Envoy, IIS, nginx, WASM)

## Git Workflow Rules

- The default/canonical branch is `master`, NOT `main`. Branch from `master`, open PRs against `master`, and never assume `main` exists.
- Always verify current branch with `git branch --show-current` before committing.
- Never use `git commit --amend` unless the user explicitly requests it.
- Always `git push` after committing unless told otherwise.
- When working across branches, confirm the target branch with the user before committing.

## Debugging Methodology

- Read the full error output before hypothesizing a cause.
- For IIS/Windows crashes: request WinDbg or crash dump analysis first, not indirect breadcrumb approaches.
- For CI failures: check the full error output before hypothesizing; do not assume a cause without evidence.
- When a fix fails in CI, analyze WHY it failed before trying another approach — do not iterate blindly.
- Verify hypotheses with evidence before acting on them.
