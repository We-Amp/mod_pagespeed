# CLAUDE.md

This file provides context and guidance for contributors (human and AI-assisted) working in this repository.

## Project Overview

mod_pagespeed is an open-source web performance optimization middleware originally created by Google. It automatically applies 40+ optimization filters (image compression/resizing, CSS/JS minification, cache extension, etc.) to web pages without requiring content modifications.

The project supports three deployment modes:
- **Apache module** (`mod_instaweb`) - Traditional Apache HTTP Server integration
- **Envoy filter** - Modern proxy-based deployment using Envoy
- **Windows/IIS** (experimental) - IIS native module via C API

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

# Inside container - run tests (with cache backends)
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/...

# Stop when done
docker compose down
```

**Preferred compiler config: `--config=clang-libstdcxx13`** (Clang with GCC 13's libstdc++). This provides the best combination of build diagnostics and C++23 support.

Other build configs (less preferred):
- `--config=gcc` - GCC 13 with native C++23 support (works but slower diagnostics)
- `--config=clang` - **Will fail** to build Cyclone (Clang 17's `__cpp_concepts` macro doesn't meet libstdc++ 13's requirement for `std::expected`)

### Windows Builds (dockur/windows)

Windows builds use **dockur/windows** containers to run Windows VMs directly from a Linux workstation with KVM support. This provides automated provisioning and seamless integration with the existing docker-compose workflow.

**Requirements:**
- Linux workstation with KVM support (`/dev/kvm` accessible)
- Docker and Docker Compose
- ~100GB SSD space at `/media/usbcssd/windows-vm/`
- 16GB+ RAM available for the Windows VM

**Quick Start:**
```bash
# Start Windows x64 VM (first boot takes 15-30 min for installation + provisioning)
./windows-dev/start-windows-dev.sh

# Watch installation progress via Web VNC
# Open in browser: http://localhost:8006

# Wait for SSH to become available
./windows-dev/wait-for-windows.sh

# Build on Windows
./windows-dev/build-on-windows.sh //pagespeed/kernel/base:pagespeed_base
./windows-dev/build-on-windows.sh //pagespeed/system:system_windows
./windows-dev/build-on-windows.sh //pagespeed/windows:pagespeed_c_api

# Run tests
./windows-dev/build-on-windows.sh --test //test/pagespeed/kernel/base:string_util_test

# Interactive PowerShell session
./windows-dev/build-on-windows.sh --shell

# Stop the VM when done
docker compose --profile windows-x64 down
```

**Connection Details:**
| VM | SSH | Web VNC | RDP |
|----|-----|---------|-----|
| Windows x64 | `ssh -p 2222 Developer@localhost` | http://localhost:8006 | localhost:33389 |
| Windows ARM64 | `ssh -p 2223 Developer@localhost` | http://localhost:8007 | localhost:33390 |

**Credentials:** Username `Developer`, Password `ChangeMe1`

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

**Current Build Status (January 2026):**
| Target | Status | Notes |
|--------|--------|-------|
| `//pagespeed/kernel/base:pagespeed_base` | ✅ Builds | Core utilities, threading, file system |
| `//pagespeed/kernel/http` | ✅ Builds | Requires clang-cl for googleurl |
| `//pagespeed/system:system_windows` | ✅ Builds | Requires clang-cl |
| `//pagespeed/windows:pagespeed_c_api` | ✅ Builds | Requires clang-cl |

**Known Issues:**
1. **googleurl requires clang-cl** - The provisioning script installs LLVM automatically.
2. **yq junction bug** - Bazel creates broken directory junctions for yq. Workaround: copy yq.exe manually after `bazel fetch @yq_windows_amd64//:yq_toolchain`.
3. **Tests not yet ported** - Unit tests require additional Windows compatibility work.

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

# Run all tests
bazel test --config=clang-libstdcxx13 //test/...

# Run a specific test
bazel test --config=clang-libstdcxx13 //test/pagespeed/kernel/base:string_util_test

# Run tests with debug output (as used in CI)
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  -c dbg --test_output=streamed //test/...

# Other build configurations (less preferred)
bazel build --config=gcc //...               # GCC 13
bazel build --config=clang-asan //...        # ASAN sanitizer
bazel build --config=clang-tsan //...        # TSAN sanitizer

# Standard test exclusions (tests that require special setup or are broken)
EXCL="-//test/pagespeed/apache:apache -//test/pagespeed/apache:apache_apr -//test/pagespeed/system:system -//test/system/..."

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
  - `mod_instaweb.cc` - Main Apache module hooks
  - `instaweb_handler.cc` - Request/response handling

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
  - `iis_rewrite_driver_factory` - IIS-specific factory

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

# Exclude Apache tests if APR is problematic
bazel test --config=clang-libstdcxx13 //test/pagespeed/... -- -//test/pagespeed/apache:apache
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

HTML rewriting is implemented via `EnvoyHtmlRewriter`, which runs entirely on Envoy's dispatcher thread using async callbacks. This avoids the threading issues that made `ProxyFetch` incompatible with Envoy.

#### Architecture

```
Request Flow:
  Client Request
       │
       ▼
  decodeHeaders() ──► IPRO lookup (FetchInPlaceResource)
       │
       ▼
  Origin Server Response
       │
       ▼
  encodeHeaders() ──► Detect HTML via Content-Type
       │              └──► Create EnvoyHtmlRewriter if HTML
       ▼
  encodeData() ──────► Buffer chunks via ProcessChunk()
       │              └──► HtmlDetector confirms content is HTML
       ▼
  end_stream ────────► StartParsing() + FinishParseAsync()
       │              └──► RewriteDriver applies filters
       ▼
  sendReply() ───────► Rewritten HTML to client
```

#### State Machine

The `EnvoyHtmlRewriter` uses a state machine to track progress:

| State | Description |
|-------|-------------|
| `kDetecting` | Buffering initial bytes to detect if content is actually HTML |
| `kBuffering` | Confirmed HTML, accumulating body chunks |
| `kParsing` | Parsing HTML through RewriteDriver |
| `kFinishing` | FinishParseAsync in progress |
| `kDone` | Rewriting complete, response sent |
| `kPassThrough` | Fell back to original (not HTML, buffer overflow, or error) |

#### Activation Requirements

HTML rewriting is only activated when **both** conditions are met:
1. `EnableHtmlRewriting` is `on` (default)
2. Custom options are present (via query params like `?PageSpeedFilters=...`)

This design prevents IPRO warm-up requests from triggering HTML rewriting, which avoids threading issues. To rewrite HTML, include PageSpeed query parameters:

```bash
# Triggers HTML rewriting with collapse_whitespace filter
curl "http://localhost:8080/page.html?PageSpeedFilters=collapse_whitespace"

# Multiple filters
curl "http://localhost:8080/page.html?PageSpeedFilters=combine_css,combine_javascript,rewrite_images"
```

#### Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `EnableHtmlRewriting` | `on` | Enable/disable HTML rewriting capability |
| `HtmlRewriteDeadlineMs` | `2000` | Maximum time (ms) to wait for rewriting to complete |
| `MaxHtmlBufferBytes` | `2097152` (2MB) | Maximum HTML response size to rewrite |

These can be set via the PageSpeed configuration in your Envoy setup.

#### Fallback Behavior

The rewriter gracefully degrades in these scenarios:

| Scenario | Behavior | Response Header |
|----------|----------|-----------------|
| Content not HTML | Pass through original | None |
| Buffer overflow (>2MB) | Pass through original | None |
| Deadline exceeded | Send original HTML | `X-PageSpeed-Timeout: 1` |
| Parse failure | Send original HTML | Warning logged |

#### Resource Fetching During Rewriting

When HTML rewriting triggers sub-resource fetches (e.g., for inlining CSS), the `CurlUrlAsyncFetcher` handles these requests independently of Envoy's ClusterManager. This is critical because:
- RewriteDriver worker threads cannot access Envoy's ClusterManager (main thread only)
- libcurl operates on its own thread pool, avoiding deadlocks

#### Code Locations

| File | Purpose |
|------|---------|
| `pagespeed/envoy/envoy_html_rewriter.h` | Class declaration and state machine |
| `pagespeed/envoy/envoy_html_rewriter.cc` | Implementation, parsing, timer handling |
| `pagespeed/envoy/http_filter.cc` | HTML detection and filter integration |
| `pagespeed/envoy/envoy_rewrite_options.cc` | Option registration |

#### Testing

```bash
# Unit tests for HTML rewriting options
bazel test --config=clang-libstdcxx13 //test/pagespeed/envoy:envoy_html_rewriter_test

# All Envoy filter tests
bazel test --config=clang-libstdcxx13 //test/pagespeed/envoy/...
```

#### Debugging

Enable verbose logging to trace HTML rewriting:
```yaml
# In Envoy config, set PageSpeed log level
typed_config:
  "@type": type.googleapis.com/pagespeed.Decoder
  # Add MessageBufferSize for debug messages
```

Look for these log messages:
- `"Starting HTML rewriting for %s"` - Rewriting initiated
- `"HTML rewriting fell back to pass-through for %s"` - Fallback triggered
- `"HTML rewrite deadline exceeded for %s"` - Timeout occurred

**Envoy config:** See `pagespeed-envoy.yaml` for the Envoy v3 API configuration template.

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
