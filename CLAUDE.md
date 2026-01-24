# CLAUDE.md

This file provides context and guidance for contributors (human and AI-assisted) working in this repository.

## Project Overview

mod_pagespeed is an open-source web performance optimization middleware originally created by Google. It automatically applies 40+ optimization filters (image compression/resizing, CSS/JS minification, cache extension, etc.) to web pages without requiring content modifications.

The project supports two deployment modes:
- **Apache module** (`mod_instaweb`) - Traditional Apache HTTP Server integration
- **Envoy filter** - Modern proxy-based deployment using Envoy

## Build System

**Bazel 7.x** is the build system. The project uses WORKSPACE-based dependency management (bzlmod is disabled via `.bazelrc`).

### Docker Development Environment (Recommended)

The easiest way to build is using the Docker development environment:

```bash
# Start the environment (includes Redis and Memcached)
docker compose up -d

# Enter the dev container
docker compose exec dev bash

# Inside container - build everything
bazel build //...

# Inside container - run tests (with cache backends)
bazel test --test_env=REDIS_PORT=6379 --test_env=MEMCACHED_PORT=11211 //test/...

# Stop when done
docker compose down
```

### Key Commands

```bash
# Build everything
bazel build //...

# Run all tests
bazel test //test/...

# Run a specific test
bazel test //test/pagespeed/kernel/base:string_util_test

# Run tests with debug output (as used in CI)
bazel test --test_env=REDIS_PORT=6379 --test_env=MEMCACHED_PORT=11211 -c dbg --test_output=streamed //test/...

# Build configurations
bazel build --config=clang //...     # Clang compiler
bazel build --config=gcc //...       # GCC compiler
bazel build --config=clang-asan //...# ASAN sanitizer
bazel build --config=clang-tsan //...# TSAN sanitizer
```

Tests require Redis (port 6379) and Memcached (port 11211) for full coverage. In the Docker environment, pass `--test_env=REDIS_PORT=6379 --test_env=MEMCACHED_PORT=11211` to connect to the containerized services. Tests use default sharding of 10 shards.

**Note on memory requirements**: Building Envoy test libraries (especially integration tests) requires significant memory. With constrained memory (e.g., Docker containers with limited RAM), use `--jobs=1` or `--jobs=2` and exclude the heavy integration test:
```bash
bazel build --jobs=1 //... -- -//pagespeed/envoy:http_filter_integration_test
```

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

- **system/** - System-level abstractions (admin UI, controller management)

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

### Key Dependencies

The project builds on Envoy's infrastructure and uses:
- Envoy HTTP proxy libraries
- APR/APRUtil/Serf (Apache portable runtime)
- Protocol Buffers / gRPC
- libjpeg-turbo, libpng, libwebp (image optimization)
- Brotli (compression)
- Google Test (testing)
- DRP (Domain Registry Provider) - public suffix validation

**Build-time requirements:**
- Python 3 - Required for DRP registry tables code generation
- Java JRE - Required for Closure Compiler

## Code Style

- C++20 standard (required by Envoy dependencies)
- Google C++ style (see `.clang-format`)
- 80-column line limit
