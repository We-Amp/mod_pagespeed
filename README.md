# mod_pagespeed — pagespeed-optimizer

[![CI](https://github.com/We-Amp/pagespeed-optimizer/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/We-Amp/pagespeed-optimizer/actions/workflows/ci.yml?query=branch%3Amain)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/We-Amp/pagespeed-optimizer/badge)](https://scorecard.dev/viewer/?uri=github.com/We-Amp/pagespeed-optimizer)
[![Release](https://img.shields.io/github/v/release/We-Amp/pagespeed-optimizer?filter=optimizer-v*&label=release)](https://github.com/We-Amp/pagespeed-optimizer/releases)
[![NuGet](https://img.shields.io/nuget/v/WeAmp.PageSpeed)](https://www.nuget.org/packages/WeAmp.PageSpeed)
[![License](https://img.shields.io/github/license/We-Amp/pagespeed-optimizer)](LICENSE)

The optimization worker, nginx serving module and management console of
mod_pagespeed. The same worker also backs the Apache module
(shipped from the [We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed)
repository): one optimizer serves both web servers, with zero-copy cache
serving and asynchronous content optimization.

## Architecture

mod_pagespeed is built from three cooperating components. The serving
module is thin and lives in the web server; all optimization work happens in
the worker; the cache is shared between them.

```
                  ┌───────────────────────────┐
   HTTP Request──>│    Web server + module    │
                  │ nginx module — this repo  │
                  │ Apache module —           │
                  │   We-Amp/mod_pagespeed    │
                  └─────────────┬─────────────┘
                                │
                  ┌─────────────▼─────────────┐     ┌──────────────────────┐
                  │       Cyclone cache       │◄───►│ pagespeed-optimizer  │
                  │  (We-Amp/cyclone-cache)   │     │ worker — this repo   │
                  └───────────────────────────┘     └──────────────────────┘
```

**Serving module** — Thin C++ web-server module that classifies requests into
capability-based cache keys, serves optimized variants via zero-copy mmap, and
records cache misses for asynchronous processing. This repository builds the
nginx module; the Apache module (the 1.x lineage, shipped as 2.1.0 and later) lives in
[We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed). Both modules
attach to the same worker and the same cache.

**pagespeed-optimizer** — The optimization worker: a lightweight C++ process
that reads original content from cache, applies optimizations (image
transcoding, CSS/JS minification, critical CSS extraction), and writes
optimized variants back. It also serves the management console at `/console/`
and the HTTP management API.

**Cyclone cache** — Variant-aware disk cache with memory-mapped directory
sharing. Both the web-server module and the worker mmap the same volume file
for instant cross-process visibility. Developed in
[We-Amp/cyclone-cache](https://github.com/We-Amp/cyclone-cache) and fetched
here via Bazel.

## Packages

mod_pagespeed ships as deb/rpm package pairs:

- **`pagespeed-optimizer`** — the optimization worker (this repository)
- **`mod-pagespeed`** — the Apache module, which depends on the optimizer
  package at the exact same version, so `apt`/`yum` pull it in automatically
  Both install from the signed repository: `curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh`

Container images and a Helm chart cover the nginx deployment side; see
[modpagespeed.com](https://modpagespeed.com/). Upgrading between releases —
including from 1.x — is an in-place package upgrade; see
[UPGRADING.md](UPGRADING.md).

## Install with a coding agent

Machine-readable install recipes, one per server, are served at
`https://modpagespeed.com/recipes/<surface>.md` (index:
[recipes/README.md](https://modpagespeed.com/recipes/README.md); source:
`website/public/recipes/`). The `install-modpagespeed` skill in
`.claude/skills/` picks the recipe for the detected server, runs it and
verifies the result. Install it in Claude Code:

```bash
claude plugin marketplace add We-Amp/pagespeed-optimizer
claude plugin install modpagespeed@modpagespeed
```

or into another agent's skill directory with the `skills` CLI:

```bash
npx skills add We-Amp/pagespeed-optimizer --skill install-modpagespeed
```

See [Install with a coding agent](https://modpagespeed.com/docs/agent-install/).

## Features

- **Image Optimization** — JPEG/PNG/GIF to WebP and AVIF transcoding,
  lossless PNG reduction, quality-aware compression
- **CSS Minification** — Whitespace, comment, and redundant syntax removal
- **JavaScript Minification** — Comment and whitespace removal
- **Critical CSS Injection** — Extracts above-the-fold CSS and injects it
  inline into HTML
- **Early Hints (103)** — Sends preload hints for stylesheets while waiting
  for upstream
- **Capability-Based Variants** — Different optimized versions for WebP vs
  AVIF clients, mobile vs desktop, Save-Data, connection quality
- **Zero-Copy Serving** — Cache hits served via mmap without copying data
- **Graceful Degradation** — If the worker is down, the web server continues
  serving original content
- **Notification Retry** — Exponential backoff for worker notifications
- **Per-URL Policies** — `pagespeed_disallow` directive for excluding paths
- **Structured Logging** — JSON log output with configurable log levels
- **Security Limits** — Configurable max URL length, content size limits,
  connection limits

## Quick Start

### Build

Needs clang-20 with libc++-20 (see [Prerequisites](#prerequisites)); a build
with the distribution compiler fails. Every Bazel command takes the same
compiler and `--config=libc++`:

```bash
export CC=clang-20 CXX=clang++-20

# Build all (excludes nginx module)
bazel build --config=libc++ //...

# Run all tests
bazel test --config=libc++ //...

# Build nginx module (requires nginx source headers)
NGINX_PATH=/path/to/nginx bazel build --config=libc++ //src/nginx:ngx_pagespeed_module.so
```

### Deploy

```bash
# Docker Compose: pulls the published worker and nginx images (see deploy/README.md)
cd deploy
cp .env.example .env
# Edit .env: set BACKEND_HOST and BACKEND_PORT to your origin
# (default host.docker.internal:8081, a server on the Docker host)
docker compose up -d
```

```bash
# Or systemd, from the repository root (see deploy/pagespeed-optimizer.service)
sudo cp deploy/pagespeed-optimizer.service /etc/systemd/system/
sudo systemctl enable --now pagespeed-optimizer
```

### Verify

Use GET, not `curl -I`: the module does not cache HEAD requests, so a HEAD
never turns into a HIT.

```bash
# First request — cache miss, proxied to origin
curl -s -o /dev/null -D - http://localhost/style.css | grep -i x-pagespeed
# X-PageSpeed: MISS

# Subsequent request — served from cache
curl -s -o /dev/null -D - http://localhost/style.css | grep -i x-pagespeed
# X-PageSpeed: HIT
```

## Configuration

### Nginx Directives

| Directive | Default | Description |
|-----------|---------|-------------|
| `pagespeed on\|off` | `off` | Enable/disable the module |
| `pagespeed_cache_path PATH` | (required) | Path to Cyclone cache file |
| `pagespeed_disallow PATTERN` | (none) | Exclude URL patterns from optimization |

These three are the minimum. The module also takes `pagespeed_cache_mode`, the max-age family (`pagespeed_max_age`, `pagespeed_html_max_age`, `pagespeed_css_max_age`, `pagespeed_image_max_age`, `pagespeed_immutable_max_age`, `pagespeed_stale_if_error_max_age`), `pagespeed_synthesize_swr`, `pagespeed_conditional_revalidation`, `pagespeed_hot_threshold`, `pagespeed_force_refresh`, `pagespeed_force_refresh_html`, `pagespeed_trust_x_forwarded_proto` and `pagespeed_agent_content_signal`; see [cache modes](https://modpagespeed.com/docs/cache-modes/) and [cache control](https://modpagespeed.com/docs/cache-control/).

The worker socket path and HTML processing toggle are configured on
the worker side and shared with nginx automatically via `pagespeed-shared.conf`
(written next to the cache file). See the
[configuration reference](https://modpagespeed.com/docs/configuration/) and
[deploy/README.md](deploy/README.md).

### Worker Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--cache-dir DIR` | `/var/cache/pagespeed-optimizer/v3` | Directory holding the cache volume, shared config and serve-stats |
| `--cache-path PATH` | (derived from `--cache-dir`) | Expert override: full cache volume file stem |
| `--socket PATH` | `/run/pagespeed-optimizer/notify.sock` | Unix socket path |
| `--cache-size BYTES` | `1073741824` | Cache size (1 GiB) |
| `--max-connections N` | `128` | Max simultaneous connections |
| `--max-buffer-size BYTES` | `1048576` | Max per-client buffer (1MB) |
| `--connection-timeout MS` | `30000` | Idle connection timeout |
| `--shutdown-timeout MS` | `5000` | Graceful shutdown timeout |
| `--disable-html` | `false` | Disable HTML optimization |
| `--disable-css` | `false` | Disable CSS minification |
| `--disable-js` | `false` | Disable JS minification |
| `--disable-image` | `false` | Disable image transcoding |
| `--max-url-length BYTES` | `8192` | Max URL length |
| `--max-html-size BYTES` | `5242880` | Max HTML size (5MB) |
| `--max-css-size BYTES` | `2097152` | Max CSS size (2MB) |
| `--max-js-size BYTES` | `2097152` | Max JS size (2MB) |
| `--max-image-size BYTES` | `10485760` | Max image size (10MB) |
| `--log-level LEVEL` | `info` | debug\|info\|warning\|error |
| `--log-format FORMAT` | `text` | text\|json |

## Development

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to report bugs and send
pull requests.

### Prerequisites

- [Bazelisk](https://github.com/bazelbuild/bazelisk) — the repository pins
  the Bazel version in `.bazelversion` (9.0.0); bazelisk installs exactly
  that
- clang-20 with libc++-20 — `clang-20 libc++-20-dev libc++abi-20-dev` from
  [apt.llvm.org](https://apt.llvm.org/) (not the distribution defaults:
  libc++-18 and older lack `std::atomic_ref`, which the cache library
  requires, and GCC trips constexpr bugs in abseil). Build with
  `CC=clang-20 CXX=clang++-20 bazel build --config=libc++ //...`
- Docker (for sanitizer tests and E2E)
- pre-commit (`pip install pre-commit && pre-commit install`)

### Testing

```bash
# Unit tests
export CC=clang-20 CXX=clang++-20
bazel test --config=libc++ //...

# Sanitizers (ASan + UBSan + LeakSan + TSan)
./tools/docker-sanitizers.sh

# E2E tests (full stack in Docker Compose)
./tools/e2e/run_e2e.sh
```

### Code Style

- C++23, Google style, 80-character line limit
- Formatting enforced by pre-commit hooks (clang-format, buildifier)
- Run `clang-format -i <files>` before staging C++ changes

## Project Structure

```
lib/           Curated code from mod_pagespeed
  base/        Base utilities (string_util, message_handler, arena)
  html/        HTML parser
  css/         CSS minifier
  js/          JS minifier
  image/       Image codecs and optimization
  cache/       Cyclone cache C++ wrapper
src/           pagespeed-optimizer worker and serving modules
  cache/       Capability mask, variant keys
  nginx/       Nginx serving module
  proto/       Worker IPC protocol
  worker/      pagespeed-optimizer (the factory worker)
  browser/     Headless browser analysis (CDP client, Chrome management)
  crypto/      Web Bot Auth / RSL-CAP verification
samples/       ASP.NET Core middleware (WeAmp.PageSpeed.AspNetCore)
test/          Unit and integration tests
tools/         Docker, sanitizers, E2E, packaging, formatting scripts
deploy/        Production deployment configs
```

## Related Repositories

- [We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed) — the
  Apache module (the 1.x lineage, shipped as 2.1.0 and later)
- [We-Amp/cyclone-cache](https://github.com/We-Amp/cyclone-cache) — the
  Cyclone cache library

## Support

See [SUPPORT.md](SUPPORT.md) — GitHub Issues for bugs, [SECURITY.md](SECURITY.md)
for vulnerabilities, commercial support from We-Amp.
Release history: [CHANGELOG.md](CHANGELOG.md) and the
[release notes](https://modpagespeed.com/docs/release-notes/).

## License

Apache License 2.0, see [LICENSE](LICENSE).

Developed by [We-Amp B.V.](https://we-amp.com)
