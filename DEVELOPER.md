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
# Build everything (preferred config — needed for the Cyclone cache files)
bazel build --config=clang-libstdcxx13 //...

# Build specific components
bazel build --config=clang-libstdcxx13 //pagespeed/kernel/...
bazel build --config=clang-libstdcxx13 //pagespeed/apache/...

# The Envoy filter (Envoy 1.40, from MODULE.bazel). Its targets and tests are
# tagged manual (//... leaves them out: they compile all of Envoy), so name
# them:
bazel build --config=envoy-filter //pagespeed/envoy:envoy_pagespeed //pagespeed/envoy:pagespeed_filter.so
bazel test --config=envoy-filter //test/pagespeed/envoy:http_filter_test

# Build with GCC instead of Clang
bazel build --config=gcc //...
```

### Compiler and linker settings inside the container

The compiler the build should use is the LLVM 20 clang the image links into
`/opt/llvm/bin` (`docker/Dockerfile`), and the container puts `/opt/llvm/bin`
first on `PATH` (`docker-compose.yml`). Two settings make a local Bazel run
use it end to end.

**The toolchain compiler.** Bazel's auto-configured C++ toolchain (the
`local_config_cc` repository) picks the compiler when that repository is
generated: it reads `CC`, and with no `CC` anywhere it takes `gcc` from
`PATH`. The container exports no `CC`. A command line that carries
`--config=clang-libstdcxx13` (or one of the configs built on it, such as
`clang-asan` and `clang-tsan`) still ends up on `/opt/llvm/bin/clang`: the
`--config=clang` it chains to contributes `--action_env=CC=clang`, and that
is what the detection sees. A command with no clang config at all silently
builds with the system GCC instead. To pin the compiler whatever configs you
pass, add the repository-rule counterpart of `--action_env`:

```bash
bazel test --config=clang-libstdcxx13 \
    --repo_env=CC=clang --repo_env=CXX=clang++ \
    //third_party/css_parser:css_parser_test
```

**The linker.** The clang configs link with `-fuse-ld=lld` (`.bazelrc`,
`build:clang`), and the clang driver then looks for `ld.lld` — not `lld`.
`/usr/bin` ships only the versioned name (`ld.lld-20`), so the image links
`ld.lld` into `/opt/llvm/bin` next to clang (`docker/Dockerfile`). Bazel
compiles with `-no-canonical-prefixes`, so a symlinked clang resolves tools
in the symlink's directory rather than its real install dir, and falls back
to `PATH` from there — keep `/opt/llvm/bin` on it (the `user.bazelrc` PATH
override under
[Common Issues](#host-environment-leaking-into-docker-builds-m4bison_pkgdatadir)
does). Without a reachable `ld.lld`, linking fails with:

```
clang++: error: invalid linker name in argument '-fuse-ld=lld'
```

### What `--config=ci` expands to

`build:ci` (`pagespeed.bazelrc`) is what the CI Linux jobs pass, and it is a
**vendored** build. It expands to:

- `--config=vendored` — dependencies resolve from the repository cache under
  `vendor/repo-cache` and the two vendored source trees
  (`--override_repository` for the Cyclone cache library and aom), with the
  repository contents cache switched off (`--repo_contents_cache=`; the
  repository cache lives inside the source tree, where Bazel refuses to keep
  a contents cache). A plain clone has no `vendor/`: create it with
  `tools/vendor-deps.sh` or build from a source tarball (see
  [Offline Build](#offline-build)).
- `--config=clang-libstdcxx13` — the config above.
- `--config=remote-cache` — `--remote_cache=http://bazel-remote-cache:9090`.
  That address is an alias that resolves only on the CI runners (the Linux
  jobs map it with `--add-host`; a hosted job passes `--remote_cache=` to
  switch it off). Locally, do not pass `--config=ci`: name the configs you
  want (`--config=clang-libstdcxx13`, plus `--config=vendored` when a
  vendored tree is what you have).
- `--jobs=32`, `--nostamp`, `--define=go_build=disabled` and `-std=c++20`
  forced for host and target; `.bazelrc` adds `--noshow_progress
  --noshow_loading_progress`, and `test:ci` sets `--test_output=errors`.

CI layers the compiler environment on top of that: the full-tier jobs add
`--config=ci-build` (`.bazelrc`: `--action_env=CC=clang
--action_env=CXX=clang++`), the PR-tier jobs pass `--config=ci
--config=ci_lock` (`--lockfile_mode=error`), and the hosted arm64 job spells
the two `--action_env` flags out on its command line. For a local run the
flags that matter are the ones in the section above: with
`--config=clang-libstdcxx13` the toolchain detection picks the image's clang
either way, and the `--repo_env` pair pins it for command lines that carry no
clang config.

### Running Tests

```bash
# Run all tests
bazel test --config=clang-libstdcxx13 //test/...

# Run specific test suites
bazel test --config=clang-libstdcxx13 //test/pagespeed/kernel/...
bazel test --config=clang-libstdcxx13 //test/net/instaweb/...

# Run a single test with verbose output
bazel test --config=clang-libstdcxx13 --test_output=streamed //test/pagespeed/kernel/base:base_test

# Run tests with cache backends (Redis/Memcached)
bazel test --config=clang-libstdcxx13 --test_env=REDIS_PORT=6379 --test_env=MEMCACHED_PORT=11211 //test/...
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
    apache2-dev \
    libmemcached-dev \
    redis-tools \
    gperf \
    uuid-dev \
    make \
    m4
```

`make` and `m4` are used from the host by the rules_foreign_cc builds
(curl, libaom, libavif, libmemcached, libevent) on Linux.

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
The toolchain selection of [Compiler and linker settings inside the
container](#compiler-and-linker-settings-inside-the-container) applies here
too: a host that exports no `CC` builds with its system GCC, and
`--repo_env=CC=clang --repo_env=CXX=clang++` (or an exported `CC`/`CXX`)
selects the host's clang instead.

## Project Structure

The engine spans two top-level source trees. `net/instaweb/` holds the ~57
rewriter filters (`net/instaweb/rewriter/*_filter.cc`) with `RewriteDriver`
and `RewriteOptions`, in the legacy `net_instaweb::` namespace: to add or edit
a filter, work there. `pagespeed/` holds the modern subsystems: the server
adapters (`apache/`, `nginx/`, `envoy/`, `iis/`), `kernel/`, `system/` and
`automatic/`. There is no `rewriter/` directory under `pagespeed/`.
`net/instaweb/rewriter/generated/` is generated; edit the source `.js` and run
`tools/regenerate-rewriter-js.sh`.

```
mod_pagespeed/
├── net/instaweb/           # Rewriter filters + RewriteDriver/RewriteOptions
│   ├── rewriter/           # ~57 rewriter filters + RewriteDriver/RewriteOptions + support code
│   ├── htmlparse/          # Legacy HTML-parse support
│   ├── http/               # Legacy HTTP support
│   ├── util/               # Legacy util support
│   └── rewriter/generated/ # Generated client JS outputs (do NOT hand-edit)
├── pagespeed/              # Modern subsystems
│   ├── kernel/             # Core utilities and abstractions
│   │   ├── base/           # Strings, threading, statistics
│   │   ├── cache/          # Cache backends (LRU, file, memcached, redis, Cyclone)
│   │   ├── html/           # HTML parsing (HtmlParse) and filters
│   │   ├── http/           # HTTP protocol handling
│   │   ├── image/          # Image optimization
│   │   └── thread/         # Threading primitives
│   ├── apache/             # Apache module (mod_instaweb)
│   ├── nginx/              # Nginx module
│   ├── envoy/              # Envoy filter
│   ├── iis/                # IIS native module (live factory)
│   ├── automatic/          # ProxyFetch — shared rewriting engine
│   └── system/             # System-level abstractions (admin UI, cache backends)
├── base/                   # glog-compatible logging (base/logging.h)
├── test/                   # Tests (mirrors source structure)
├── bazel/                  # Bazel build rules and dependencies
├── third_party/            # Third-party code and patches
└── docker-compose.yml      # Docker development environment
```

## Build Configurations

Available Bazel configurations (use with `--config=<name>`):

| Config | Description |
|--------|-------------|
| `clang-libstdcxx13` | **Preferred** — Clang with GCC 13's libstdc++; required to compile the Cyclone cache files. Bare `--config=clang` fails on Cyclone (Clang's `__cpp_concepts` is below the value GCC 13's libstdc++ requires for `std::expected`); `--config=gcc` builds Cyclone natively. |
| `clang` | Clang compiler (base config; `clang-libstdcxx13` extends it) |
| `gcc` | GCC 13 native — also builds Cyclone (C++23), slower diagnostics |
| `clang-asan` | Clang with AddressSanitizer |
| `clang-tsan` | Clang with ThreadSanitizer |
| `clang-msan` | Clang with MemorySanitizer |

Example:
```bash
bazel build --config=clang-libstdcxx13 //...
bazel test --config=clang-asan //test/...
```

## Offline Build

A source tarball built by `tools/vendor-deps.sh` carries what a build needs, so it can run with the network cut:

- `vendor/repo-cache`: the Bazel repository cache, with every archive the build downloads and every registry file (`MODULE.bazel`, `source.json`, `bazel_registry.json`) that `MODULE.bazel.lock` pins, stored by sha256. The vendor step fails if a pinned registry file is missing.
- `vendor/cyclone` and `vendor/aom`: the two `git_repository` sources.

Build with:

```bash
bazel build --config=vendored --config=clang-libstdcxx13 \
    --repository_disable_download --lockfile_mode=error //:libmod_pagespeed.so
```

- `--lockfile_mode=error` fails the build if resolution would change `MODULE.bazel.lock`. Keep `--registry` as committed: it is part of the lockfile keys.
- Changing `MODULE.bazel` or the lockfile means vendoring again.
- The Bazel binary itself is not vendored; provide it (for example a pre-seeded Bazelisk home).
- Downloads made with `repository_ctx.download` (for example `yq_windows_amd64`) bypass the repository cache and are not vendored.
- Cutting the network (for example `docker run --network none`) is the proof: with `--repository_disable_download`, anything missing from the tarball fails the build.

## Code Style

- **C++ Standard**: C++20 (C++23 for Cyclone cache files via `per_file_copt`)
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

See [CONTRIBUTING.md](CONTRIBUTING.md) for bug reports, the release-note
requirement and pull request expectations.

## Additional Resources

- [CLAUDE.md](CLAUDE.md) - guidance for AI coding agents (detailed architecture notes)
- [Bazel Documentation](https://bazel.build/docs)
- [Envoy Documentation](https://www.envoyproxy.io/docs)
- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
