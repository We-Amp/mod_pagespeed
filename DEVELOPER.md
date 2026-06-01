# Developer Guide

This guide helps developers set up their environment and contribute to mod_pagespeed.

## Quick Start with Docker (Recommended)

The Docker-based development environment provides a consistent build experience across platforms, including macOS (Apple Silicon) and Linux.

### Prerequisites

- Docker Desktop (macOS/Windows) or Docker Engine (Linux)
- Docker Compose v2+

### Starting the Environment

```bash
# Build and start all services (dev container + Redis + Memcached)
docker compose up -d

# Enter the development container
docker compose exec dev bash

# Verify services are accessible (inside container)
check-services
```

### Building

Inside the container:

```bash
# Build everything
bazel build //...

# Build specific components
bazel build //pagespeed/kernel/...
bazel build //pagespeed/apache/...
bazel build //pagespeed/envoy/...

# Build with specific compiler
bazel build --config=clang //...
bazel build --config=gcc //...
```

### Running Tests

```bash
# Run all tests
bazel test //test/...

# Run specific test suites
bazel test //test/pagespeed/kernel/...
bazel test //test/net/instaweb/...

# Run a single test with verbose output
bazel test --test_output=streamed //test/pagespeed/kernel/base:base_test

# Run tests with cache backends (Redis/Memcached)
bazel test --test_env=REDIS_PORT=6379 --test_env=MEMCACHED_PORT=11211 //test/...
```

### Container Shell Aliases

The dev container provides convenient aliases:

| Alias | Description |
|-------|-------------|
| `build` | Build all targets |
| `test` | Run all tests |
| `test-debug` | Run tests with debug output |
| `build-kernel` | Build kernel components |
| `build-apache` | Build Apache module |
| `build-envoy` | Build Envoy filter |
| `test-kernel` | Test kernel components |
| `check-services` | Verify Redis/Memcached connectivity |
| `clean` | Clean bazel cache |

### Stopping the Environment

```bash
docker compose down        # Stop containers
docker compose down -v     # Stop and remove volumes (clears bazel cache)
```

## Native Build (Linux)

For native Linux builds without Docker:

### Dependencies

Ubuntu/Debian:
```bash
sudo apt-get install -y \
    bazel \
    clang \
    lld \
    python3 \
    default-jre-headless \
    apache2-dev \
    libmemcached-dev \
    redis-tools \
    gperf \
    uuid-dev
```

### Cache Services

Tests require Redis and Memcached:
```bash
# Start services
sudo systemctl start redis-server
sudo systemctl start memcached

# Or run in Docker
docker run -d -p 6379:6379 redis:7-alpine
docker run -d -p 11211:11211 memcached:1.6-alpine
```

### Build Commands

Same as inside Docker container - see [Building](#building) section above.

## Project Structure

```
mod_pagespeed/
├── pagespeed/              # Main source code
│   ├── kernel/             # Core utilities and abstractions
│   │   ├── base/           # Strings, threading, statistics
│   │   ├── cache/          # Cache backends (LRU, file, memcached, redis)
│   │   ├── html/           # HTML parsing and filters
│   │   ├── http/           # HTTP protocol handling
│   │   ├── image/          # Image optimization
│   │   └── thread/         # Threading primitives
│   ├── apache/             # Apache module (mod_instaweb)
│   ├── envoy/              # Envoy filter
│   ├── system/             # System-level abstractions
│   └── controller/         # gRPC coordination service
├── test/                   # Tests (mirrors source structure)
├── bazel/                  # Bazel build rules and dependencies
├── third_party/            # Third-party code and patches
└── docker-compose.yml      # Docker development environment
```

## Build Configurations

Available Bazel configurations (use with `--config=<name>`):

| Config | Description |
|--------|-------------|
| `clang` | Build with Clang compiler |
| `gcc` | Build with GCC compiler |
| `clang-asan` | Clang with AddressSanitizer |
| `clang-tsan` | Clang with ThreadSanitizer |
| `clang-msan` | Clang with MemorySanitizer |

Example:
```bash
bazel build --config=clang-asan //...
bazel test --config=clang-asan //test/...
```

## Code Style

- **C++ Standard**: C++17
- **Style Guide**: Google C++ Style Guide
- **Line Limit**: 80 columns
- **Formatting**: Use `.clang-format` in repository root

Format code before committing:
```bash
clang-format -i path/to/file.cc
```

## Common Issues

### libjpeg_turbo checksum errors

The Chromium libjpeg_turbo archive has non-deterministic checksums. The sha256 check is disabled in `bazel/repositories.bzl`. If you see checksum errors, this is expected behavior from upstream.

### ARM NEON linking errors (ARM64/Apple Silicon)

libpng NEON optimizations are disabled via `PNG_ARM_NEON_OPT=0` in `bazel/libpng.bzl` since the NEON assembly files aren't compiled.

### Python 3 required

The `drp` (domain registry provider) uses Python 3 scripts. Ensure `python3` is installed.

### Java required

The Closure Compiler requires Java. Ensure `default-jre-headless` or equivalent is installed.

### SerfUrlAsyncFetcherTest failures

These tests require access to `selfsigned.modpagespeed.com` and may fail if the server is unavailable or returning unexpected responses. These are integration tests, not unit tests.

### Memcached/Redis connection refused in Docker

If tests can't connect to cache services, ensure port forwarding is set up:
```bash
# Inside dev container, run socat forwarders
socat TCP-LISTEN:11211,fork,reuseaddr TCP:memcached:11211 &
socat TCP-LISTEN:6379,fork,reuseaddr TCP:redis:6379 &
```

## Debugging

### Debug builds

```bash
bazel build -c dbg //...
bazel test -c dbg --test_output=streamed //test/path:test_name
```

### Running under GDB

```bash
bazel build -c dbg //test/pagespeed/kernel/base:base_test
gdb bazel-bin/test/pagespeed/kernel/base/base_test
```

### Verbose test output

```bash
bazel test --test_output=all //test/path:test_name
bazel test --test_output=streamed //test/path:test_name  # Real-time output
```

## Contributing

1. Create a feature branch from `master`
2. Make changes following the code style guidelines
3. Ensure all tests pass: `bazel test //test/...`
4. Submit a pull request

## Additional Resources

- [CLAUDE.md](CLAUDE.md) - AI assistant guidance and architecture overview
- [Bazel Documentation](https://bazel.build/docs)
- [Envoy Documentation](https://www.envoyproxy.io/docs)
- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
