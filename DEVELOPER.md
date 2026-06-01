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
- **CI Toolchain**: `clang-format-20` (must match exactly to avoid CI churn)

Reformat everything in scope in one shot:
```bash
tools/fix-format.sh
```

Or reformat a single file:
```bash
clang-format-20 -i path/to/file.cc
```

### Optional: pre-commit hook

Install the in-tree pre-commit hook to catch formatting violations locally
before CI does. Run once per clone:

```bash
tools/install-hooks.sh
```

This sets `core.hooksPath` to `.githooks/`, which runs `clang-format-20
--dry-run --Werror` on staged `pagespeed/` and `net/` sources (excluding
`pagespeed/iis/`) on every commit. Bypass once with `git commit --no-verify`.
If `clang-format-20` is not on PATH the hook prints a warning and skips.

## Common Issues

### Host environment leaking into Docker builds (M4/BISON_PKGDATADIR)

If you have `m4` or `bison` installed locally (e.g., in `~/.local/bin`), their environment variables (`M4`, `BISON_PKGDATADIR`) can leak into the Bazel sandbox via `rules_foreign_cc`, causing the `libmemcached` CMake build to fail with errors like:

```
/usr/bin/flex: fatal internal error, exec of /home/user/.local/bin/m4 failed
```

**Fix:** Create a `user.bazelrc` (gitignored) to override these:

```bash
cat > user.bazelrc << 'EOF'
# Fix for Docker container builds: override host-leaked env vars
build --action_env=M4=/usr/bin/m4
build --action_env=BISON_PKGDATADIR=/usr/share/bison
build --action_env=PATH=/opt/llvm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
EOF
```

### Accessing Apache from the host (port publishing)

By default the dev container has no published ports, so Apache running inside it isn't reachable from the host browser. Create a `docker-compose.override.yml` (gitignored) to publish ports:

```yaml
# Publish Apache ports for browser access
# Note: network_mode: host does NOT work on Docker Desktop (WSL2/macOS) —
# it maps to the Docker VM's network, not the actual host.
services:
  dev:
    ports:
      - "8081:8081"   # Apache secondary vhost (use if port 80 is taken by IIS)
      - "8443:8443"   # Apache HTTPS
```

Then `docker compose up -d dev` and access the admin console at `http://localhost:8081/pagespeed_admin/`.

### SSH agent forwarding for Docker builds

The dev container clones its dependencies (e.g., the Cyclone cache library) from their public repositories over HTTPS, so no credentials are needed. If you build against private forks over SSH, start an SSH agent before `docker compose up`:

```bash
eval "$(ssh-agent -s)"
ssh-add ~/.ssh/id_ed25519
export SSH_AUTH_SOCK
docker compose up -d dev
```

If you see "Host key verification failed" inside the container, add GitHub's key:

```bash
docker compose exec dev bash -c "mkdir -p ~/.ssh && ssh-keyscan github.com >> ~/.ssh/known_hosts"
```

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
