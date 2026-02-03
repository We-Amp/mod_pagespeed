# CLAUDE.md

This file provides context and guidance for contributors (human and AI-assisted) working in this repository.

## Project Overview

mod_pagespeed is an open-source web performance optimization middleware originally created by Google. It automatically applies 40+ optimization filters (image compression/resizing, CSS/JS minification, cache extension, etc.) to web pages without requiring content modifications.

The project supports three deployment modes:
- **Apache module** (`mod_instaweb`) - Traditional Apache HTTP Server integration
- **Envoy filter** - Modern proxy-based deployment using Envoy
- **Windows/IIS** (experimental) - IIS native module with direct C++ integration

## Build System

**Bazel 7.x** is the build system. The project uses WORKSPACE-based dependency management (bzlmod is disabled via `.bazelrc`).

### Docker Development Environment (Required)

All builds and testing **must** be done inside the Docker development environment. Do not attempt to build natively on the host (e.g. the host Clang is too old for C++23).

The `docker-compose.yml` mounts volumes for:
- Source code at `/src`
- Cyclone cache source at `/cyclone-cache` (from host: `/home/builder/code/we-amp/trafficserver/cyclone-cache`)
- SSD-backed Bazel caches at `/ssd-cache/` (from host: `/media/usbcssd/bazel-cache/`)

```bash
# Start the environment (includes Redis and Memcached)
docker compose up -d

# Enter the dev container
docker compose exec dev bash

# Inside container - build everything
bazel build --config=clang-libstdcxx13 //...

# Inside container - run all C++ unit tests (with cache backends)
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/... //test/net/...

# Stop when done
docker compose down
```

**Preferred compiler config: `--config=clang-libstdcxx13`** (Clang with GCC 13's libstdc++). This provides the best combination of build diagnostics and C++23 support.

Other build configs (less preferred):
- `--config=gcc` - GCC 13 with native C++23 support (works but slower diagnostics)
- `--config=clang` - **Will fail** to build Cyclone (Clang 17's `__cpp_concepts` macro doesn't meet libstdc++ 13's requirement for `std::expected`)

### Windows Builds

Windows builds can be done either directly on Windows or via SSH from a Linux workstation.

#### Direct Windows Development (Preferred)

The Windows VM uses a local git clone at `C:\pagespeed`. This avoids network path issues with Bazel.

```powershell
# Sync latest changes
cd C:\pagespeed
git pull --recurse-submodules

# Build the IIS module
bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll

# Build core libraries
bazel build --config=windows --config=clang-cl //pagespeed/kernel/base:pagespeed_base
bazel build --config=windows --config=clang-cl //pagespeed/system:system_windows

# Run unit tests
bazel test --config=windows --config=clang-cl //test/pagespeed/kernel/base:string_util_test
```

**First-time setup:** After VM provisioning, add the SSH key to GitHub:
```powershell
cat ~/.ssh/id_ed25519.pub
# Copy output and add to GitHub Settings > SSH Keys
```

**IIS Module Installation:**
```powershell
# Stop IIS, copy DLL, register module, start IIS
Stop-Service -Name W3SVC
Copy-Item bazel-bin/pagespeed/iis/pagespeed_iis.dll C:\inetpub\pagespeed\ -Force
New-WebGlobalModule -Name PageSpeedModule -Image 'C:\inetpub\pagespeed\pagespeed_iis.dll'
Start-Service -Name W3SVC
```

**Running IIS Tests:**
```bash
# Set port and run pytest
PAGESPEED_PORT=8080 python -m pytest test/iis/test_sanity.py -v
```

#### Remote Windows Development (via SSH)

For development from a Linux workstation using **dockur/windows** containers:

```bash
# Start Windows x64 VM (first boot takes 15-30 min for installation + provisioning)
./windows-dev/start-windows-dev.sh

# Wait for SSH to become available
./windows-dev/wait-for-windows.sh

# Build on Windows via SSH
./windows-dev/build-on-windows.sh //pagespeed/iis:pagespeed_iis.dll

# Interactive PowerShell session
./windows-dev/build-on-windows.sh --shell

# Stop the VM when done
docker compose --profile windows-x64 down
```

**Requirements for remote development:**
- Linux workstation with KVM support (`/dev/kvm` accessible)
- Docker and Docker Compose
- ~100GB SSD space at `/media/usbcssd/windows-vm/`
- 16GB+ RAM available for the Windows VM

**Connection Details:**
| VM | SSH | Web VNC | RDP |
|----|-----|---------|-----|
| Windows x64 | `ssh -p 2222 Developer@localhost` | http://localhost:8006 | localhost:33389 |
| Windows ARM64 | `ssh -p 2223 Developer@localhost` | http://localhost:8007 | localhost:33390 |

**Credentials:** Username `Developer`, Password `ChangeMe1`

**SSH Agent Requirement:** The Windows build scripts use SSH key authentication. Claude Code needs the SSH_AUTH_SOCK environment variable to access the user's SSH agent. Ask the user for their SSH_AUTH_SOCK path (e.g., `export SSH_AUTH_SOCK=/tmp/ssh-XXXXXXFUpMth/agent.2898308`) before running Windows build commands.

**Key Windows targets:**
```
//pagespeed/system:system_windows     # System library (no memcached/fork)
//pagespeed/windows:pagespeed_c_api   # C API for IIS integration
//pagespeed/windows:iis_rewrite_driver_factory  # IIS factory
```

**Platform-specific implementations:**
| Component | Windows | Linux/macOS |
|-----------|---------|-------------|
| Crypto (MD5) | BCrypt API | BoringSSL |
| Threading | StdThreadSystem | StdThreadSystem + pthread |
| Shared Memory | NullSharedMem | PthreadSharedMem |
| External Cache | Redis only | Redis + Memcached |

**Current Build Status (February 2026):**
| Target | Status | Notes |
|--------|--------|-------|
| `//pagespeed/kernel/base:pagespeed_base` | ✅ Builds | Core utilities, threading, file system |
| `//pagespeed/kernel/http` | ✅ Builds | Requires clang-cl for googleurl |
| `//pagespeed/system:system_windows` | ✅ Builds | Requires clang-cl |
| `//pagespeed/windows:pagespeed_c_api` | ✅ Builds | Requires clang-cl |
| `//pagespeed/iis:pagespeed_iis.dll` | ⚠️ Builds but crashes | DLL builds, loads in IIS, crashes during initialization |

**IIS Module Status:**
- **DLL builds successfully** with `--config=windows --config=clang-cl`
- **Minimal module works** (RegisterModule returning S_OK loads and runs)
- **Full initialization crashes** with 0xc0000409 (STATUS_STACK_BUFFER_OVERRUN)
- **Root cause**: Likely in SystemRewriteDriverFactory initialization or ProcessContext lifecycle

**Known Issues:**
1. **googleurl requires clang-cl** - The provisioning script installs LLVM automatically.
2. **yq junction bug** - After `bazel clean --expunge`, run `bazel fetch @yq_windows_amd64//:yq_toolchain` then copy the yq directory to fix broken junctions.
3. **Python UTF-8 encoding** - DRP genrule requires `PYTHONUTF8=1` (automatically set in `bazel/drp.bzl`).
4. **Cyclone cache source** - Available as git submodule at `third_party/cyclone-cache`. Run `git submodule update --init` if missing.
5. **IIS module initialization crash** - Full PageSpeed initialization crashes; needs debugging with WinDbg to identify specific cause.

See `windows-dev/README.md` for detailed documentation including troubleshooting and manual setup options.

### Key Commands

All commands below are run **inside the Docker container** (`docker compose exec dev bash`). The source tree is mounted at `/src` inside the container.

```bash
# Build everything
bazel build --config=clang-libstdcxx13 //...

# Build just the Envoy filter binary (standalone Envoy with PageSpeed)
bazel build --config=clang-libstdcxx13 //pagespeed/envoy:envoy_pagespeed

# Build the PageSpeed filter as a shared library (for use with standard Envoy)
bazel build --config=clang-libstdcxx13 //pagespeed/envoy:pagespeed_filter.so

# Run all C++ unit tests (excludes Python system tests that need Apache)
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/... //test/net/...

# Run a specific test
bazel test --config=clang-libstdcxx13 //test/pagespeed/kernel/base:string_util_test

# Run tests with debug output (as used in CI)
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  -c dbg --test_output=streamed //test/pagespeed/... //test/net/...

# Build and test everything (must exclude Python system tests)
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //... -- -//test/system/...

# Other build configurations (less preferred)
bazel build --config=gcc //...               # GCC 13
bazel build --config=clang-asan //...        # ASAN sanitizer
bazel build --config=clang-tsan //...        # TSAN sanitizer

# Standard test exclusions (Python system tests require running Apache)
# Use //test/pagespeed/... //test/net/... instead of //test/... to avoid these
EXCL="-//test/system/..."

# Run tests with sanitizers (include exclusions and cache backends)
bazel test --keep_going --config=clang-asan \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  --test_output=errors //test/... -- $EXCL

bazel test --keep_going --config=clang-tsan \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  --test_output=errors //test/... -- $EXCL
```

TSAN suppressions are in `tools/tsan_suppressions.txt` and referenced from `.bazelrc`.

Tests require Redis (port 6379) and Memcached (port 11211) for full coverage. In the Docker environment, pass the following test environment variables to connect to the containerized services:
- `--test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis`
- `--test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached`

Tests use default sharding of 10 shards.

**Note on memory requirements**: Building the Envoy binary requires significant memory. With constrained memory (e.g., Docker containers with limited RAM), use `--jobs=2` to `--jobs=4` and exclude the heavy integration test:
```bash
bazel build --config=clang-libstdcxx13 --jobs=4 //pagespeed/envoy:envoy_pagespeed
bazel build --config=clang-libstdcxx13 --jobs=2 //... -- -//pagespeed/envoy:http_filter_integration_test
```
The Bazel server may crash (exit code 14 / "Socket closed") if memory is exhausted during linking. If this happens, reduce `--jobs` and re-run — cached actions will be reused.

## Code Architecture

### Core Components (`pagespeed/`)

- **kernel/** - Foundation layer with platform abstractions
  - `base/` - Core utilities (strings, statistics, threading, file systems)
  - `html/` - HTML parsing and filter infrastructure
  - `http/` - HTTP protocol handling, content types, headers
  - `cache/` - Caching backends (LRU, file, memcached, redis)
  - `image/` - Image processing and optimization
  - `js/` - JavaScript handling
  - `thread/` - Threading primitives
  - `sharedmem/` - POSIX shared memory support

- **apache/** - Apache HTTP Server module integration
  - `mod_instaweb.cc` - Main Apache module hooks (contains static `ApacheProcessContext`)
  - `instaweb_handler.cc` - Request/response handling
  - Build targets:
    - `:apache` - Full library including `mod_instaweb.cc` (for Apache module builds)
    - `:apache_core` - Core library without `mod_instaweb.cc` (for tests)
    - `:apache_apr` - APR-dependent components (memcached client, pool utilities)

- **envoy/** - Envoy proxy filter integration
  - Protocol buffer definitions for configuration
  - Envoy-specific driver factory and fetch implementations
  - Uses libcurl for resource fetching (not Envoy's native HTTP client)
  - Optimized dependencies: uses `filter_config_interface` instead of `main_common_lib`
    to avoid pulling in QUIC, HTTP/3, gRPC implementations (62% reduction in deps)

- **system/** - System-level abstractions (admin UI, controller management)
  - `system` - Full system library (memcached, Redis, fork-based controller)
  - `system_envoy` - Optimized for Envoy (Redis only, no fork, no memcached)
  - `system_windows` - Windows cross-compilation (same deps as system_envoy)

- **windows/** - Windows/IIS integration (experimental)
  - `pagespeed_c_api` - C ABI for IIS native modules
  - `iis_rewrite_driver_factory` - IIS-specific factory with CurlUrlAsyncFetcher

- **iis/** - IIS native module implementation
  - `iis_http_module` - CHttpModule implementation for IIS request pipeline
  - `iis_module_factory` - IIS module factory (RegisterModule entry point)
  - `iis_server_context` - Per-application-pool state management
  - `iis_request_context` - Per-request state and response handling
  - `iis_async_fetch` - AsyncFetch adapter for ProxyFetch output (blocking wait pattern)
  - `iis_config` - web.config XML configuration parsing
  - `iis_admin_handler` - Admin UI endpoints (/pagespeed_admin, /pagespeed_statistics)
  - `license_validator` - Commercial license key validation

- **controller/** - Central optimization coordination (gRPC-based)

- **automatic/** - Standalone rewriting (static rewriter, proxy interface)

- **opt/** - Specialized optimizations (ads, HTTP, logging)

### Test Structure

Tests mirror the source layout under `test/`:
```
test/pagespeed/kernel/base/  -> pagespeed/kernel/base/
test/pagespeed/kernel/html/  -> pagespeed/kernel/html/
```

Test macros are defined in `bazel/pagespeed_test.bzl`:
- `pagespeed_cc_test()` - Standard unit tests
- `pagespeed_cc_test_library()` - Test helper libraries
- `pagespeed_cc_benchmark()` - Performance benchmarks

### Types of Tests

The project has two types of tests:

#### C++ Unit Tests (`//test/pagespeed/...`)

Unit tests that test internal code without requiring a running server:

```bash
# Run all C++ unit tests
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/...
```

#### Python System Tests (`//test/system/...`)

Integration tests that require a running Apache server with mod_pagespeed:

```bash
# Inside Docker container - run full system test workflow:
# 1. Builds libmod_pagespeed.so
# 2. Installs and configures Apache with mod_pagespeed
# 3. Runs Python pytest tests against the server
./test/system/run_system_tests.sh

# Run specific tests
./test/system/run_system_tests.sh -k sanity              # Run sanity tests only
./test/system/run_system_tests.sh automatic/test_extend_cache.py  # Specific file

# Options
./test/system/run_system_tests.sh --build-only           # Just build the module
./test/system/run_system_tests.sh --skip-build           # Skip build, run tests
./test/system/run_system_tests.sh --keep-running         # Keep Apache running after
```

The system tests are in `test/system/automatic/` and test specific PageSpeed filters against a live server. They use the pytest framework with custom assertions in `test/system/pagespeed_test_framework/`.

#### IIS System Tests (Windows)

Integration tests can also run against IIS on the Windows VM. These tests use the same pytest framework but connect to IIS instead of Apache.

**Prerequisites:**
- Windows VM running (`docker compose --profile windows-x64 up -d`)
- SSH agent with keys (`SSH_AUTH_SOCK` environment variable set)
- Source code accessible on Windows (via SMB mount at `C:\pagespeed`)

**Quick Start:**
```bash
# Set SSH agent socket (get path from your SSH agent)
export SSH_AUTH_SOCK=/tmp/ssh-XXX/agent.123

# Run baseline tests (without PageSpeed module)
./test/system/run_iis_tests.sh --no-module sanity

# Set up IIS only (no tests)
./test/system/run_iis_tests.sh --build-only --no-module

# Run with verbose output
./test/system/run_iis_tests.sh --no-module -v sanity

# Keep IIS running after tests (for debugging)
./test/system/run_iis_tests.sh --no-module --keep-running sanity
```

**Running Tests with PageSpeed Module:**
```bash
# First, build the IIS module on Windows
./windows-dev/build-on-windows.sh //pagespeed/iis:pagespeed_iis_dll

# Install the module in IIS
ssh -p 2222 Developer@localhost \
  'powershell -File C:\pagespeed\test\system\install_pagespeed_module.ps1'

# Run tests with module
./test/system/run_iis_tests.sh sanity
```

**Test Scripts:**
| Script | Purpose |
|--------|---------|
| `test/system/run_iis_tests.sh` | Main entry point (runs from Linux host) |
| `test/system/setup_iis_full.ps1` | Full IIS setup (site, app pool, content) |
| `test/system/install_pagespeed_module.ps1` | Install PageSpeed module in IIS |

**Test Markers:**
- `@pytest.mark.iis_only` - Test only runs on IIS
- `@pytest.mark.not_iis` - Test skipped on IIS
- `@pytest.mark.requires_stats` - Test requires statistics endpoint

**Environment Variables (set automatically by run_iis_tests.sh):**
```
PAGESPEED_HOST=localhost
PAGESPEED_PORT=8080
PAGESPEED_SERVER_TYPE=iis
PAGESPEED_STATS_ENABLED=0  (or 1 with module)
PAGESPEED_EXAMPLE_ROOT=/mod_pagespeed_example
PAGESPEED_TEST_ROOT=/mod_pagespeed_test
```

#### Test Infrastructure Notes

**ProcessContext and Static Initialization:**
The `ProcessContext` class manages global initialization (domain registry, HTML keywords, etc.) and must be constructed exactly once per process. However, test binaries may link code with static `ProcessContext` instances:
- `RewriteTestBaseProcessContext` in test infrastructure (`test/net/instaweb/rewriter/rewrite_test_base.cc`)
- `ApacheProcessContext` in `mod_instaweb.cc` (which has a `ProcessContext` member)

To avoid conflicts, Apache tests use `//pagespeed/apache:apache_core` instead of `//pagespeed/apache` - this excludes `mod_instaweb.cc` and its static `ApacheProcessContext`.

**Libevent Test Sharding:**
Tests using `LibeventDispatcher` (in `//test/pagespeed/kernel/thread:event_dispatcher_test`) don't work well with Bazel's test sharding. The test uses `shard_count = 1` to ensure all tests run in a single process and avoid resource contention between parallel event loops.

### Key Dependencies

The project builds on Envoy's infrastructure and uses:
- Envoy HTTP proxy libraries (used by Envoy filter for HTTP fetching)
- APR/APRUtil/Serf (Apache portable runtime - used by Apache module only)
- Protocol Buffers / gRPC
- libjpeg-turbo, libpng, libwebp (image optimization)
- Brotli (compression)
- Google Test (testing)
- DRP (Domain Registry Provider) - public suffix validation
- Cyclone Cache - High-performance disk cache (requires C++23, needs GCC 13+)

**Build-time requirements:**
- Python 3 - Required for DRP registry tables code generation
- Java JRE - Required for Closure Compiler

### Logging System

The project uses a custom logging implementation in `base/logging.h` that provides glog-compatible macros without the glog dependency:

- **LOG(severity)** - Standard logging (INFO, WARNING, ERROR, FATAL, DFATAL)
- **VLOG(level)** / **DVLOG(level)** - Verbose logging (debug builds only for DVLOG)
- **CHECK(condition)** / **CHECK_EQ/NE/LT/LE/GT/GE** - Runtime assertions
- **DCHECK** variants - Debug-only checks

**Key files:**
- `base/logging.h` - Macro definitions and LogMessage class
- `base/logging.cc` - Implementation with spdlog integration
- `pagespeed/kernel/util/gflags.h` - Minimal gflags replacement for CLI tools

**Log sinks:** Different deployment modes register custom log sinks:
- Apache: `pagespeed/apache/log_message_handler.cc` routes to Apache error log
- Envoy: Uses spdlog directly (Envoy's logging framework)

**Command-line flags:** The `gflags.h` replacement provides basic `--flag=value` parsing for build tools like `data2c`. It doesn't support all gflags features but handles the common cases.

### Fine-Grained Build Targets

The project provides fine-grained build targets for faster incremental builds. Use these when working on specific components to avoid recompiling unrelated code.

#### System Library (`pagespeed/system/`)

| Target | Description | External Deps |
|--------|-------------|---------------|
| `:system` | Full system library (backward-compatible) | hiredis, libmemcached |
| `:system_envoy` | Envoy-optimized (no memcached, no fork) | hiredis |
| `:system_windows` | Windows-compatible (no memcached/fork) | hiredis |
| `:redis_cache` | Redis cache backend only | hiredis |
| `:memcached_cache` | Memcached cache backend only | libmemcached |
| `:external_server_spec` | Server spec utilities | none |
| `:circuit_breaker` | Circuit breaker state machine | none |
| `:circuit_breaker_fetcher` | Fetcher wrapper with circuit breaker | none |

```bash
# Build specific cache backend (faster iteration)
bazel build --config=clang-libstdcxx13 //pagespeed/system:redis_cache
bazel build --config=clang-libstdcxx13 //pagespeed/system:memcached_cache

# Run specific cache backend tests
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  //test/pagespeed/system:redis_cache_test
bazel test --config=clang-libstdcxx13 \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/system:memcached_cache_test

# Run system tests without external cache backends
bazel test --config=clang-libstdcxx13 //test/pagespeed/system:system_core_test
bazel test --config=clang-libstdcxx13 //test/pagespeed/system:external_server_spec_test
```

#### Image Library (`pagespeed/kernel/image/`)

| Target | Description | External Deps |
|--------|-------------|---------------|
| `:image` | Full image library (backward-compatible) | all codecs |
| `:image_base` | Common interfaces | libwebp (for format detection) |
| `:jpeg` | JPEG codec | libjpeg-turbo |
| `:png` | PNG codec | libpng, optipng |
| `:webp` | WebP codec | libwebp |
| `:gif` | GIF codec (depends on :png) | giflib |
| `:image_converter` | Format conversion orchestrator | all codecs |
| `:frame_processing` | Animation support | none |
| `:image_analysis` | Quality analysis | jpeg |
| `:image_resizer` | Image scaling | none |
| `:image_optimizer` | High-level optimization API | all |

```bash
# Build specific image codec (faster iteration)
bazel build --config=clang-libstdcxx13 //pagespeed/kernel/image:jpeg
bazel build --config=clang-libstdcxx13 //pagespeed/kernel/image:png
bazel build --config=clang-libstdcxx13 //pagespeed/kernel/image:webp

# Test specific codec
bazel test --config=clang-libstdcxx13 //test/pagespeed/kernel/image:jpeg_test
bazel test --config=clang-libstdcxx13 //test/pagespeed/kernel/image:png_test
bazel test --config=clang-libstdcxx13 //test/pagespeed/kernel/image:webp_test
bazel test --config=clang-libstdcxx13 //test/pagespeed/kernel/image:gif_test
```

## Code Style

- C++20 standard (required by Envoy dependencies)
- Google C++ style (see `.clang-format`)
- 80-column line limit

## Envoy HTTP Fetcher

For Envoy deployments, resource fetching uses libcurl (`CurlUrlAsyncFetcher`).
This operates independently of Envoy's ClusterManager infrastructure, providing
reliable HTTP/HTTPS fetching without TLS initialization issues.

**Features:**
- Full HTTP method support (GET, POST, PUT, DELETE, PATCH, PURGE, etc.)
- Automatic gzip inflation for compressed responses
- Configurable timeout (uses `FetcherTimeoutMs` setting)
- HTTPS support with configurable SSL certificates

**Code locations:**
- `pagespeed/envoy/envoy_rewrite_driver_factory.cc` - AllocateFetcher() creates CurlUrlAsyncFetcher
- `pagespeed/system/curl_url_async_fetcher.cc` - libcurl-based fetcher implementation

## Envoy Build Artifacts

Two build targets are available for the Envoy filter:

| Target | Size | Description |
|--------|------|-------------|
| `//pagespeed/envoy:envoy_pagespeed` | ~212 MB | Standalone Envoy binary with PageSpeed filter |
| `//pagespeed/envoy:pagespeed_filter.so` | ~113 MB (stripped) | Shared library for use with standard Envoy |

The shared library registers itself via static initialization when loaded. It can be used with a standard Envoy binary via `LD_PRELOAD` or extension loading mechanisms.

**Optimized dependencies:** The filter uses `filter_config_interface` instead of `main_common_lib`, avoiding QUIC, HTTP/3, and gRPC implementations. This reduces Envoy package dependencies from 137 to 52 packages.

## Envoy Filter Status

The Envoy filter supports both **IPRO (In-Place Resource Optimization)** for CSS/JS/images and **HTML rewriting** for HTML responses.

### HTML Rewriting

HTML rewriting uses **ProxyFetch** (`pagespeed/automatic/proxy_fetch.h`), the same battle-tested engine used by Apache and IIS. This ensures consistent behavior across all deployment modes.

#### Architecture

The Envoy implementation uses `CreateNewProxyFetch()` (not `StartNewProxyFetch()`) because Envoy already has the response body from the upstream filter chain - it doesn't need ProxyFetch to fetch from origin.

```
Request Flow:
  Client Request
       │
       ▼
  decodeHeaders() ──► Setup request context, options
       │
       ▼
  Origin Server Response (via Envoy upstream)
       │
       ▼
  encodeHeaders() ──► Detect HTML via Content-Type
       │              ├──► Create EnvoyAsyncFetch (receives output)
       │              ├──► Create RewriteDriver
       │              ├──► InitiatePropertyCacheLookup()
       │              └──► proxy_fetch_ = CreateNewProxyFetch()
       │                   └──► proxy_fetch_->HeadersComplete()
       ▼
  encodeData() ──────► proxy_fetch_->Write(data)
       │              └──► ProxyFetch buffers and parses on worker threads
       ▼
  end_stream ────────► proxy_fetch_->Done(true)
       │              └──► ProxyFetch completes parsing, calls EnvoyAsyncFetch
       ▼
  EnvoyAsyncFetch::HandleDone()
       │              └──► dispatcher_.post() to main thread
       ▼
  sendReply() ───────► Rewritten HTML to client
```

#### Threading Model

ProxyFetch uses worker threads (`QueuedWorkerPool::Sequence`) for HTML parsing while Envoy runs on its dispatcher thread. `EnvoyAsyncFetch` bridges these:

```
Envoy Dispatcher Thread              ProxyFetch Worker Thread
───────────────────────              ────────────────────────
encodeHeaders()
  └─ CreateNewProxyFetch()
encodeData()
  └─ proxy_fetch_->Write()  ───►     ExecuteQueued()
                                       └─ ParseText()
proxy_fetch_->Done()        ───►     FinishParseAsync()
                                       └─ HandleDone()
                            ◄───     dispatcher_.post()
SendFinalResponse()
  └─ filter_->sendReply()
```

`EnvoyAsyncFetch` uses `shared_from_this()` and atomic flags to safely handle the case where the Envoy filter is destroyed while ProxyFetch is still processing.

#### Comparison with Apache and IIS

| Aspect | Apache | IIS | Envoy |
|--------|--------|-----|-------|
| ProxyFetch method | `StartNewProxyFetch()` | `CreateNewProxyFetch()` | `CreateNewProxyFetch()` |
| Data source | Fetcher (origin) | IIS buffer (complete) | Filter chain (streaming) |
| Wait pattern | Blocking | Blocking | Non-blocking (dispatcher.post) |
| Property cache | Full integration | Not used | Full integration |

#### Activation Requirements

HTML rewriting activates by default when **both** conditions are met:
1. PageSpeed is enabled (not disabled via `PageSpeed=off`)
2. `EnableHtmlRewriting` is `on` (default)

This matches Apache behavior where HTML is rewritten automatically. IPRO (In-Place Resource Optimization) for CSS/JS/images goes through a separate code path and does not conflict with HTML rewriting.

You can optionally customize filters via query parameters:

```bash
# HTML is rewritten by default - no query params needed
curl "http://localhost:8080/page.html"

# Override filters via query params
curl "http://localhost:8080/page.html?PageSpeedFilters=collapse_whitespace"

# Multiple filters
curl "http://localhost:8080/page.html?PageSpeedFilters=combine_css,combine_javascript,rewrite_images"
```

#### Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `EnableHtmlRewriting` | `on` | Enable/disable HTML rewriting capability |

Standard PageSpeed options (inherited from Apache) also apply, including filter configuration, domain mapping, and caching settings.

#### Fallback Behavior

The rewriter gracefully degrades in these scenarios:

| Scenario | Behavior |
|----------|----------|
| Content not HTML | Pass through original |
| Non-2xx status | Pass through original |
| ProxyFetch creation fails | Pass through original |
| Write to ProxyFetch fails | Pass through original |

#### Resource Fetching During Rewriting

When HTML rewriting triggers sub-resource fetches (e.g., for inlining CSS), the `CurlUrlAsyncFetcher` handles these requests independently of Envoy's ClusterManager. This is critical because:
- RewriteDriver worker threads cannot access Envoy's ClusterManager (main thread only)
- libcurl operates on its own thread pool, avoiding deadlocks

#### Code Locations

| File | Purpose |
|------|---------|
| `pagespeed/automatic/proxy_fetch.h` | ProxyFetch class (shared with Apache/IIS) |
| `pagespeed/envoy/envoy_async_fetch.h` | Adapter receiving ProxyFetch output |
| `pagespeed/envoy/envoy_async_fetch.cc` | Thread-safe dispatcher posting |
| `pagespeed/envoy/http_filter.cc` | HTML detection, ProxyFetch setup (lines 782-1001) |
| `pagespeed/envoy/envoy_rewrite_options.cc` | Option registration |

#### Testing

```bash
# All Envoy filter tests
bazel test --config=clang-libstdcxx13 //test/pagespeed/envoy/...
```

#### Debugging

Look for these log messages:
- `"Starting HTML rewriting for %s"` - Rewriting initiated
- `"Failed to create ProxyFetch for %s"` - ProxyFetch creation failed
- `"HTML rewriting failed for %s"` - Rewriting error

**Envoy config:** See `pagespeed-envoy.yaml` for the Envoy v3 API configuration template.

### Production Deployment

This section covers production-ready features for deploying the PageSpeed Envoy filter in production environments.

#### Configuration Options

The filter supports several production-oriented configuration options via the `pagespeed.Decoder` proto:

| Option | Type | Description |
|--------|------|-------------|
| `admin_auth` | `AdminAuthConfig` | Token-based authentication for admin endpoints |
| `circuit_breaker` | `CircuitBreakerConfig` | Circuit breaker for resource fetching reliability |
| `redis` | `RedisConfig` | External Redis cache configuration |
| `file_cache_path` | `string` | Path for disk cache (default: `/tmp/envoy_pagespeed_cache/`) |
| `lru_cache_kb_per_process` | `int64` | In-memory LRU cache size in KB (default: 512000 = 500MB) |
| `file_cache_size_kb` | `int64` | Disk cache size limit in KB (default: 10240000 = 10GB) |

**AdminAuthConfig options:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | `bool` | `false` | Enable authentication for admin endpoints |
| `token` | `string` | (required) | Bearer token for `Authorization` header |
| `allowed_ips` | `repeated string` | (all) | IP ranges in CIDR notation (e.g., `10.0.0.0/8`) |
| `rate_limit_rpm` | `int32` | `100` | Rate limit in requests per minute (0 = disabled) |

#### Prometheus Metrics Integration

The PageSpeed filter exports metrics via Envoy's stats system, which are available at:
- `/stats` - All Envoy stats including PageSpeed metrics
- `/stats/prometheus` - Prometheus-compatible format

**PageSpeed-specific metrics:**

| Metric | Type | Description |
|--------|------|-------------|
| `pagespeed.requests_total` | Counter | Total requests processed |
| `pagespeed.html_rewrites_total` | Counter | Successful HTML rewrites |
| `pagespeed.html_rewrites_failed` | Counter | Failed HTML rewrites |
| `pagespeed.html_rewrites_timeout` | Counter | HTML rewrites that timed out |
| `pagespeed.ipro_cache_hits` | Counter | IPRO cache hits |
| `pagespeed.ipro_cache_misses` | Counter | IPRO cache misses |
| `pagespeed.ipro_served_total` | Counter | Resources served via IPRO |
| `pagespeed.ipro_not_rewritable` | Counter | Non-rewritable resources |
| `pagespeed.active_html_rewrites` | Gauge | Currently active HTML rewrites |
| `pagespeed.rewrite_latency_ms` | Histogram | HTML rewrite latency distribution |

**Recommended alerts:**

```yaml
# Alert if HTML rewrite error rate exceeds 5%
- alert: PageSpeedHighErrorRate
  expr: rate(pagespeed_html_rewrites_failed[5m]) / rate(pagespeed_html_rewrites_total[5m]) > 0.05
  for: 5m
  labels:
    severity: warning

# Alert if rewrite latency p99 exceeds deadline
- alert: PageSpeedHighLatency
  expr: histogram_quantile(0.99, rate(pagespeed_rewrite_latency_ms_bucket[5m])) > 2000
  for: 5m
  labels:
    severity: warning

# Alert on cache hit rate drops
- alert: PageSpeedLowCacheHitRate
  expr: rate(pagespeed_ipro_cache_hits[5m]) / (rate(pagespeed_ipro_cache_hits[5m]) + rate(pagespeed_ipro_cache_misses[5m])) < 0.7
  for: 15m
  labels:
    severity: info
```

#### Health Check Endpoint

The filter provides a configurable health check endpoint for load balancer integration:

| Option | Default | Description |
|--------|---------|-------------|
| `HealthPath` | `/pagespeed/health` | Health check endpoint path |

The health endpoint returns JSON with status, version, uptime, and cache statistics:

```json
{
  "status": "healthy",
  "version": "1.15.0.0",
  "uptime_seconds": 3600,
  "cache": {
    "hit_rate": 0.85,
    "hits": 1000,
    "misses": 177,
    "size_bytes": 104857600
  }
}
```

**Note:** The health endpoint is intentionally excluded from admin authentication to allow load balancers to perform health checks without authentication tokens. However, it does receive security headers (X-Frame-Options, etc.) and restrictive Cache-Control to prevent caching of health status.

Configure in PageSpeed options:
```
# Custom health check path
HealthPath /health
```

#### Circuit Breaker

The filter includes a circuit breaker for resource fetching to prevent cascade failures.

**CircuitBreakerConfig options:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | `bool` | `false` | Enable circuit breaker for resource fetching |
| `failure_threshold` | `int32` | `5` | Consecutive failures before opening circuit |
| `success_threshold` | `int32` | `2` | Consecutive successes to close circuit |
| `timeout_ms` | `int64` | `30000` | Time in OPEN state before testing (HALF_OPEN) |

**Circuit breaker states:**
- **CLOSED**: Normal operation, requests allowed
- **OPEN**: Circuit tripped, requests blocked with 503 status
- **HALF_OPEN**: Testing recovery with limited requests

**Graceful degradation:** When the circuit breaker is OPEN, responses include the header `X-PageSpeed-Degraded: circuit-breaker-open` to indicate the system is operating in degraded mode.

**Configuration example:**
```yaml
circuit_breaker:
  enabled: true
  failure_threshold: 5
  success_threshold: 2
  timeout_ms: 30000
```

#### Security Headers

The PageSpeed filter adds security headers to **admin endpoints only**, not to user-facing content (HTML rewrites, IPRO cache hits). This ensures optimized resources maintain proper cacheability.

**Admin endpoints that receive security headers:**
- Health check (`/pagespeed/health`)
- Statistics (`/pagespeed_statistics`)
- Console (`/pagespeed_console`)
- Admin pages (`/pagespeed_admin`)
- Authentication errors (401, 429 responses)

**Security headers added to admin responses:**

| Header | Value | Purpose |
|--------|-------|---------|
| `X-Frame-Options` | `SAMEORIGIN` | Prevents clickjacking |
| `X-Content-Type-Options` | `nosniff` | Prevents MIME sniffing |
| `X-XSS-Protection` | `1; mode=block` | Legacy XSS protection |
| `Content-Security-Policy` | `default-src 'self'; script-src 'self' 'unsafe-inline'` | Restricts resource loading |
| `Cache-Control` | `private, no-store, no-cache, must-revalidate` | Prevents caching of admin data |

**User-facing content (IPRO, HTML rewrites):**
- Receives cache headers set by PageSpeed optimization (e.g., `max-age=31536000, public`)
- Does NOT receive security headers or restrictive cache directives
- This is intentional to ensure CDNs and browsers can properly cache optimized resources

#### Security Best Practices

For production deployments:

1. **Enable admin authentication:**
   ```yaml
   admin_auth:
     enabled: true
     token: "your-secure-token-here"
     allowed_ips:
       - "10.0.0.0/8"      # Internal network
       - "192.168.0.0/16"  # Private network
     rate_limit_rpm: 60
   ```

2. **Restrict admin access by IP:** Use `allowed_ips` to limit admin endpoint access to internal networks only.

3. **Use rate limiting:** Set `rate_limit_rpm` to prevent abuse of admin endpoints.

4. **Secure the Envoy admin interface:** Configure Envoy's admin interface on a separate port and restrict access:
   ```yaml
   admin:
     address:
       socket_address:
         address: 127.0.0.1  # Localhost only
         port_value: 9901
   ```

5. **Configure appropriate cache sizes:** Set `lru_cache_kb_per_process` and `file_cache_size_kb` based on available memory and disk.

6. **Monitor key metrics:** Set up alerts for error rates, latency, and cache hit rates (see Prometheus section above).

#### Example Production Configuration

See `pagespeed-envoy.yaml` for a complete example with production settings including:
- Admin authentication
- Redis cache backend
- Prometheus metrics exposure
- Health check endpoint
- Security-hardened Envoy admin

### Envoy vs Apache: Known Differences

The Envoy filter has some behavioral differences compared to Apache's mod_pagespeed. These are documented here for reference and are covered by skip markers in the test suite.

#### Filter Behavior Differences

| Filter | Issue | Root Cause |
|--------|-------|------------|
| `combine_css` | Times out waiting for CSS combination | Async worker pool coordination in ProxyFetch streaming architecture |
| `flatten_css_imports` | `CssInlineImportToLinkFilter` misses some style elements | HTML parsing stream timing in non-blocking model |
| `extend_cache_css` | Resource fetch times out | Async fetch via CurlUrlAsyncFetcher doesn't complete in time |

#### IPRO Cache Header Preservation

**Issue:** Tests `test_extend_cache_preserves_no_cache` and `test_rewrite_javascript_preserves_no_cache` fail because Envoy's IPRO doesn't preserve `no-cache` from upstream.

**Root Cause:** Apache calls `recorder->SaveCacheControl()` in `mod_instaweb.cc:808-819` before modifying headers for s-maxage. Envoy never calls this method (`http_filter.cc:1181-1193` has a TODO comment).

**Impact:** Cached resources lose their original `no-cache` directive.

#### Statistics Tracking

**Issue:** `resource_404_count` statistic not tracked on Envoy.

**Root Cause:** Apache uses `ApacheServerContext::ReportResourceNotFound()` to increment this counter. Envoy's `EnvoyServerContext` doesn't have an equivalent error handler.

#### Request Header Handling

**Issue:** `X-PSA-Blocking-Rewrite-Mode` header is ignored.

**Root Cause:** Envoy's ProxyFetch model is fundamentally non-blocking. This Apache-specific header has no effect because Envoy doesn't check it in `http_filter.cc`.

#### Version Header

**Issue:** Version header shows `@MAJOR@.@MINOR@...` placeholders instead of actual version.

**Root Cause:** Missing Bazel genrule to process `version.h.in` template. The GYP build system handled this, but Bazel treats `version.h` as a static file.

**Files:**
- `net/instaweb/public/version.h.in` - Template with placeholders
- `net/instaweb/public/VERSION` - Actual version values (1.15.0.0)
- `net/instaweb/public/version.h` - Static file needing substitution

#### Architectural Context

The fundamental difference is architectural:
- **Apache**: Synchronous request/response model - filters can block and wait for completion
- **Envoy**: Non-blocking, streaming architecture - ProxyFetch runs on worker threads with async callbacks

This means some Apache behaviors that depend on blocking/waiting cannot be directly replicated in Envoy without significant changes to the filter coordination model.

### Cyclone Cache Build Rule

The Cyclone cache external dependency build rule is in `bazel/cyclone.bzl`. If the upstream Cyclone repo adds new source directories, the `srcs` and `hdrs` globs must be updated to include them.

### Zero-Copy Cache Integration

The Cyclone cache supports zero-copy reads via memory-mapped I/O. When reading from the disk cache, data can be accessed directly through the mmap'd region without copying into a separate buffer.

**Key classes:**
- `MappedSharedString` (`pagespeed/kernel/base/mapped_shared_string.h`) - Holds either an owned `SharedString` or a borrowed view into mmap'd memory
- `CacheInterface::Callback` (`pagespeed/kernel/base/cache_interface.h`) - Uses `MappedSharedString` as its value storage

**Using cached values:**
```cpp
// In a CacheInterface::Callback subclass:
void Done(KeyState state) override {
  if (state == kAvailable) {
    // Zero-copy access - returns StringPiece pointing to mmap'd memory
    StringPiece data = value().Value();

    // Check if this is a zero-copy path
    if (value().is_mapped()) {
      // Data is mmap'd - avoid holding reference too long
    }

    // If you need owned data (copies if mapped):
    SharedString owned = value().ToOwned();
  }
}
```

**Current limitations:**
- **HTTPValue does not benefit from zero-copy:** The `HTTPValue::Link()` method requires a `SharedString`, so HTTP cache hits call `value().ToOwned()` which copies the data. True zero-copy for HTTP caching would require modifying `HTTPValue` to use `MappedSharedString` internally.
- **RAM cache hits are not zero-copy:** Only disk cache hits provide mmap'd access. RAM cache hits use the traditional copy path.

**Testing with sanitizers:**
```bash
# Run with Thread Sanitizer to verify thread-safety
bazel test --config=clang-tsan //test/pagespeed/kernel/cache:cache
bazel test --config=clang-tsan //test/pagespeed/kernel/base:base_test --test_filter="*MappedSharedString*"
```
