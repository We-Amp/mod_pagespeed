# CLAUDE.md

This file provides context and guidance for contributors (human and AI-assisted) working in this repository.
Component-specific details live in sub-CLAUDE.md files: `src/worker/`, `src/nginx/`,
`src/browser/`, `src/crypto/`, `lib/image/`, `lib/html/`, `lib/cache/`, `lib/classify/`,
`lib/css/`, `lib/js/`, `lib/base/`, `lib/pagespeed/` (C API: `libpagespeed.so` Linux / `pagespeed.dll` Windows),
`tools/workbench/`, `tools/e2e/`, `tools/stress/`, `tools/http-compliance/`,
`samples/aspnetcore/`, `website/` (the modpagespeed.com site). Also
`reference/mod_pagespeed/` (gitignored, absent on a fresh clone; original Google
project, not this project's conventions — see Reference Code Locations below).

## Canonical Checkout & Multi-Clone Safety

A development machine may host multiple clones and worktrees of this repo (and of
sibling repos). Before editing, run `git rev-parse --show-toplevel` and `pwd` to
confirm you are in the intended checkout. Never search or edit sibling clones,
worktrees, or vendored copies; anchor every path to the resolved toplevel. Two
in-repo wrong-file traps to watch for: `t/` (Test::Nginx integration) vs `test/`
(GoogleTest unit/e2e), and `reference/mod_pagespeed/` (upstream, gitignored and
absent by default) — code edits never belong under `reference/`.

## Start Here: Task → Entry Point

| If you are changing… | Start at | Sub-doc |
|----------------------|----------|---------|
| Request classification / cache serving (nginx) | `src/nginx/ngx_pagespeed_module.cc` | `src/nginx/CLAUDE.md` |
| Worker, IPC, or management/REST API | `src/worker/main.cc` | `src/worker/CLAUDE.md` |
| Image optimization | `lib/image/` | `lib/image/CLAUDE.md` |
| Cache / metadata wire format | `lib/cache/cache.h`, `lib/classify/alternate_metadata.h` | `lib/cache/CLAUDE.md`, `lib/classify/CLAUDE.md` |
| Outbound fetch / SSRF policy | `lib/net/` (`ssrf_guard`, `fetch_policy`, `curl_fetcher`, `upstream_pin`) | — |
| ASP.NET Core middleware | `samples/aspnetcore/src/WeAmp.PageSpeed.AspNetCore/PageSpeedMiddleware.cs` | `samples/aspnetcore/CLAUDE.md` |
| Web Bot Auth / RSL-CAP verification | `src/crypto/webbotauth/` | `src/crypto/CLAUDE.md` |
| Website (modpagespeed.com) | `website/` | `website/CLAUDE.md` |

Per-component tests are in the "What to Test After Changes" table below.
Load-bearing jargon (PSOL, AlternateId vs SentinelId, capability mask, kIdentity,
the two namespaces, Cyclone stripe/volume_size) is defined in `docs/GLOSSARY.md`.

## Related Repositories

This repo is part of the We-Amp B.V. product family. Cross-repo context:

- **mod_pagespeed** (`We-Amp/mod_pagespeed`) — the multi-port predecessor line
- **Cyclone Cache** (`We-Amp/cyclone-cache`) — shared cache library, fetched via Bazel `git_repository`

## Git & Pre-commit Hooks

When committing, always stage only the intended files explicitly with `git add <file1> <file2>` before committing. Never use `git add .` or `git commit -a`. If pre-commit hooks fail, do NOT retry blindly — check what the hook changed, re-stage only intended files, and retry. Expect hooks to modify files (formatting, linting) and plan for a two-pass commit.

## Docker Compose

Before starting containers, always run `docker compose down --remove-orphans` and check for ghost containers with `docker ps -a | grep <project>`. If port conflicts or stale container IDs appear, use `docker compose -p <new-project-name>` as a workaround. Never assume a clean Docker state. For config changes, prefer runtime volume mounts over image rebuilds when possible.

## Approach Preferences

Always prefer the simplest runtime solution over build-time or proxy-based solutions. When the user has already built something in the repo, search for it before suggesting external alternatives or asking if it exists. Do not propose complex infrastructure (nginx proxies, new services) when a simpler approach (volume mounts, runtime config, env vars) would suffice.

## Bazel / C++ Build

When fixing build warnings, always run the full test suite (`bazel test //...`) before committing. Watch for buildifier lint issues (unused parameters, load statement ordering). When modifying .bazelrc or BUILD files, verify with both `bazel build` and `bazel test`.

## Exploration & Planning

Keep exploration phases concise. When asked for a plan, deliver an actionable outline within 2-3 minutes of exploration, then offer to deep-dive on specific areas. Do not exhaustively explore the entire codebase before producing output — the user may interrupt if it takes too long.

## Project Overview

mod_pagespeed is a modern successor to ngx_pagespeed with five components:
1. **Nginx Interceptor** - C++ nginx module for zero-copy cache serving
2. **Cyclone Cache** - Variant-aware cache with capability-based keys
3. **PSOL Factory Worker** - Lightweight C++ daemon (HtmlParse + ImageOptimizer)
4. **Browser Analysis** - CDP client + Chrome process management for headless analysis
5. **Workbench** - Web console + VS Code extension for cache inspection and config tuning

Key design decision: Skip RewriteDriver entirely (2000+ LOC, 60+ filters). Use
underlying PSOL components directly.

## Build Commands

```bash
bazel build //...                                # Build all (excludes nginx module)
bazel test //...                                 # Run all tests
bazel test //test/lib/base:string_util_test      # Specific test

# Nginx module (requires nginx source headers)
NGINX_PATH=/path/to/nginx bazel build //src/nginx:ngx_pagespeed_module.so

# Regenerate compile_commands.json for clangd (after BUILD file changes)
# //:refresh_all (root BUILD) wraps hedron's default with explicit entries
# for the `manual`-tagged fuzz harnesses, which the `//...` aquery excludes.
bazel run //:refresh_all -- --features=-parse_headers --host_features=-parse_headers
```

### Package-install verification (unprivileged daemon)

Installs the built deb/rpm in throwaway containers of one target distro and
asserts what the packages promise: the `pagespeed` identity and the versioned
cache layout, the file/socket modes and ownership, the cold-start notice over a
pre-existing root-owned cache (which is never chowned, migrated or deleted),
secrets off the daemon command line, who can reach the volume and sockets
through group membership, and that a permanent refusal to start latches
`failed` instead of restarting forever. Needs Docker; nothing is installed on
the host.

```bash
tools/packaging/daemon-install-rig.sh --distro debian12 \
  --binary bazel-bin/src/worker/factory_worker \
  --library bazel-bin/lib/pagespeed/libpagespeed.so
# --distro: debian12 | ubuntu2404 | alma9
```

A leg that cannot run fails, with its blocker printed — including the
booted-systemd legs on a host that cannot run a systemd container. The single
exception is enforcing SELinux, which reports SKIP: a container shares the host
kernel, so no change here can make it runnable. CI runs this as
`Daemon Privilege Rig (<distro>)`.
`tools/packaging/smoke-optimizer-modes.sh` stays the cheap local mode check;
this rig is the packaged-install one.

## Architecture

### Directory Structure
- `lib/` - Curated code from mod_pagespeed (base, HTML parser, image optimizer, CSS)
  - `lib/net/` - Outbound fetch path: SSRF guard, fetch policy, curl fetcher,
    upstream pin (security-sensitive)
- `src/` - mod_pagespeed code (cache, worker, nginx module, browser, crypto)
  - `src/cache/` - cache integration (BUILD glue over `lib/cache/`)
  - `src/product_version/` - Single-source build version (`version.h`)
- `test/` - GoogleTest unit + e2e tests, run via Bazel (`bazel test //test/...`)
- `t/` - Test::Nginx::Socket `.t` integration tests for the nginx module
  (run via `./tools/run-test-nginx.sh`; distinct from `test/`)
- `bazel/` - Repository rules and BUILD files for external deps
- `third_party/` - BUILD files for external dependencies
- `tools/` - Docker, sanitizer, formatting, and E2E test scripts
  - `tools/workbench/` - Workbench monorepo (pnpm, SvelteKit, VS Code extension)
- `samples/` - Integration demos (ASP.NET Core middleware)
- `docs/` - Design docs and notes (see `docs/INDEX.md`)
- `deploy/` - Deployment packaging (Helm chart)
- `docker/` - Dockerfiles for the dev and lint images
- `sbom/` - Generated software bill of materials (`mod_pagespeed-2.1.spdx.json`)
- `reference/` - Reference repositories (gitignored, absent on a fresh clone)
  - `reference/mod_pagespeed/` - Source for code curation (not part of build)
  - **Note**: `reference/mod_pagespeed/` CLAUDE.md describes the *original* project,
    not this project's conventions.

### Reference Code Locations (mod_pagespeed)

`reference/mod_pagespeed/` is **gitignored** (`.gitignore`) and **absent on a fresh
clone** (also `.bazelignore`d — never a build input). The paths below are only valid
once you populate it:

```bash
git clone https://github.com/apache/incubator-pagespeed-mod reference/mod_pagespeed
```

**If `reference/mod_pagespeed/` is absent, the porting tables below are inapplicable —
do NOT fabricate a port from memory.** Verify the directory exists first, then read
the actual source.

| Component | Path | Priority |
|-----------|------|----------|
| HTML Parser | `reference/mod_pagespeed/pagespeed/kernel/html/` | P0 |
| Image Optimizer | `reference/mod_pagespeed/pagespeed/kernel/image/` | P1 |
| Base Utilities | `reference/mod_pagespeed/pagespeed/kernel/base/` | P0 |

### Dependencies

| Dependency | Source | Purpose |
|------------|--------|---------|
| Cyclone Cache | `@cyclone//:cyclone` | Disk cache with C API, zero-copy mmap |
| Abseil | `@com_google_absl//...` | String utilities, containers |
| RE2 | `@com_google_re2//:re2` | CSS selector matching |
| GoogleTest | `@com_google_googletest//:gtest_main` | Testing |
| libjpeg-turbo | `@libjpeg_turbo` | JPEG decoding |
| libpng | `@libpng` | PNG decoding |
| libwebp | `@libwebp` | WebP encoding/decoding |
| giflib | `@giflib` | GIF decoding (static + animated) |
| libuv | `@libuv` | Async I/O for worker |
| optipng | `@optipng` | Lossless PNG reduction (opngreduc) |
| libaom | `@libaom` | AV1 encoder (via rules_foreign_cc cmake) |
| libavif | `@libavif` | AVIF encoding/decoding |
| libyuv | `@libyuv` | YUV conversion (used by libavif) |
| skcms | `@skcms` | Color management (used by SSIMULACRA2) |
| nlohmann/json | `@nlohmann_json//:json` | CDP JSON-RPC serialization (header-only, MIT) |
| vtracer | `@crates//:vtracer` | Raster-to-SVG vectorization (via Rust FFI) |
| rules_rust | `bazel_dep` | Rust toolchain for VTracer FFI |
| jpegli | `@jpegli` | Advanced JPEG encoder (high quality, SIMD) |
| highway | `@highway` | SIMD abstraction library (used by jpegli) |
| zlib | `@zlib` | Compression (used by libpng, gzip variants) |
| brotli | `@brotli` | Brotli compression/decompression |
| llhttp | `@llhttp` | HTTP parser for worker management API |
| ed25519 | `@ed25519` | Ed25519 signature verification for Web Bot Auth / RSL-CAP (RFC 9421) |
| nanosvg | `@nanosvg` | SVG parser/rasterizer for fidelity verification |

### C++ Standard

C++23 required (for Cyclone). Configured in `.bazelrc`.

### Code Curation Guidelines

When porting from mod_pagespeed:
- Use `std::string` and `std::string_view` directly (not GoogleString/StringPiece)
- Use `std::unique_ptr` (not scoped_ptr)
- Use abseil directly (`absl::StrCat`, `absl::StrFormat`, etc.)
- Use C++20/23 features (`starts_with()`, `ends_with()`, etc.)
- Only add to `lib/base/string_util.h` if not available in C++ or abseil

**Namespaces:**
- New code: `pagespeed::`
- Legacy HTML parser: `net_instaweb::` (HtmlParse, HtmlElement, HtmlNode, etc.)
- Legacy image processing: `pagespeed::image_compression::` (ScanlineReader, etc.)

### 32-bit Capability Bitmask

- Bits 0-1: Image Format (00: Original, 01: WebP, 10: AVIF, 11: SVG)
- Bits 2-3: Viewport Class (00: Mobile, 01: Tablet, 10: Desktop)
- Bit 4: Pixel Density (0: 1x, 1: 2x+)
- Bit 5: Save-Data (0: off, 1: on)
- Bits 6-7: Transfer Encoding (00: Identity, 01: Gzip, 10: Brotli, 11: Reserved)

Viewport pixel ranges: Mobile 0-479px, Tablet 480-1023px, Desktop 1024-65535px.

**Default values:** `CapabilityMask()` = Desktop/Identity = **0x08** (not 0x00).
`CapabilityMask::Decode(0)` = Mobile/Identity = **0x00**.

### Cache Key Format

Cache keys are `SHA-256(URL, hostname)` (Cyclone native). Multiple variants per URL
stored as alternates, each identified by an `AlternateId` (low 8 bits of the
capability mask). Managed by `PageSpeedCache` in `lib/cache/cache.h`.

**Metadata wire format — single source of truth is `lib/classify/alternate_metadata.h`.**
Per-alternate metadata is a version-tagged, little-endian blob carrying the full
32-bit mask, content type, flags, origin cache-control fields, SSIMULACRA2 score,
content class, ETag, Last-Modified, origin content length, and (current version)
the v7 content-binding hashes (`origin_html_hash` / `render_source_hash`, 32 bytes
each). Older versions are accepted on read with missing fields
defaulted. Read the header for the current `kCurrentVersion`, exact byte offsets,
flag bits, and field semantics before changing the format — do NOT re-transcribe
the byte layout here. `lib/classify/CLAUDE.md` summarizes the field set.

**Endianness**: All metadata fields are little-endian (`memcpy` from host `uint32_t`/`uint16_t`, valid on x86 and arm64). The IPC wire format (`src/proto/worker_ipc.h`) uses explicit big-endian encoding (`WriteBE32`/`ReadBE32`). Do not conflate the two formats when adding new fields.

### Cache Write Invariant

Nginx writes only the default-mask alternate (original content, `CapabilityMask()` =
0x08); the worker writes all other alternates. No write contention by design (also in
`lib/cache/CLAUDE.md`).

**Worker identity stripping**: When the worker receives a notification with non-identity
encoding bits (e.g., 0x88 = Desktop/Brotli), it strips encoding to kIdentity before
writing the uncompressed variant (→ 0x08 identity AlternateId). Compressed variants
(gzip, brotli) are then written at their respective encoding AlternateIds (0x48, 0x88).
This prevents identity/compressed collisions.

### Cross-Process Cache Sharing (CRITICAL)

> **WARNING**: Both nginx and worker MUST open cache with
> `enable_mmap_directory = true`. Without it, worker writes are invisible
> to nginx. Symptom: X-PageSpeed: HIT but content is original (not optimized).

Cache file chmod 660, notification socket 0660 owner+group (daemon user
`pagespeed`; web-server workers join group `pagespeed`). All modes are set
with explicit chmod/fchmod after create — never umask-derived; the unit's
`UMask=0007` is a backstop only.

**One host; one PID namespace during upgrades**: every process sharing the
volume (nginx, worker, any `libpagespeed` client) must run on one host. Cyclone
(since 2194698) proves a write-lock holder dead by a kernel-held byte-range
"liveness slot" on the volume file, which is safe across PID namespaces; older
builds use `kill(pid, 0)`, where a live holder in another PID namespace looks
gone and two writes overlap undetected. So keep one PID namespace while old and
new builds can share a volume: the Helm pod sets `shareProcessNamespace: true`
and the deploy compose files join worker and nginx to a `pidns` anchor service
(`pid: "service:pidns"`) running the digest-pinned `registry.k8s.io/pause`
image, so that restarting either never kills the other.
`tools/ci/check_helm.sh` verifies the Helm setting; the compose files are
kept consistent by hand. The anchor's image must never follow the release tag: a
recreated anchor kills the namespace mid-`up -d` (the upgrade then fails with
"cannot stop container ... is not running"). Stopping `pidns` stops both. Open the cache after any daemonize
step and never close its fds behind its back (that drops the slot lock).

**Volume size must match**: nginx MUST open the cache with `config.volume_size = 0` (auto-detect from file). The worker sets its own size via `--cache-size`. Cyclone derives stripe count from volume size — a mismatch makes the same SHA-256 key resolve to different stripes in each process, making all cross-process reads and writes invisible. Symptom is identical to the `enable_mmap_directory` omission: `X-PageSpeed: HIT` but content is unoptimized. `config.volume_size = 0` is set once in the shared helper `MakeNginxCacheConfig` (`src/nginx/ngx_pagespeed_module.cc`), used by both `GetCache` and `CheckGenerationAndGetCache`.

**Serve-time bandwidth stats** (`src/worker/serve_stats.h/.cc`): Shared 128-byte
mmap file at `{cache_parent}/.pagespeed-serve-stats`. Worker creates on startup,
nginx opens lazily (~1s poll). On cache HIT of a worker-processed variant
(`kFlagWorkerProcessed` && `origin_content_length > 0`), nginx atomically
increments per-type counters (original bytes, optimized bytes, hit count).
Worker reads counters and exposes them via `serve_savings` in `/v1/stats`,
`/v1/metrics`, management socket STATS, and WebSocket `/v1/ws/stats`.

### Zero-Copy Flow
1. Nginx classifies request → 32-bit capability mask
2. `ReadBestAlternate(url, hostname, mask)` — single-pass alternate selection
3. HIT: mmap'd data → `ngx_buf_t` (zero-copy), Cache-Control set per content type
4. MISS: proxy to origin, record as default alternate, notify worker
5. Worker reads original, optimizes, writes variant alternate
6. For HTML: stores preload hints at `SentinelId::kEarlyHints`
   (stylesheet URLs — including the ones async-CSS defers, since the
   deferral primitive is itself a preload; `media="print"` always omitted
   — + LCP image with `image:` prefix — omitted for an
   `<img>` inside `<picture>` — + third-party origins with `preconnect:`
   or `preconnect-cors:` prefix, newline-separated)
7. HIT/MISS: nginx sends Link preload/preconnect headers (HIT path adds to
   response headers; MISS path sends 103 Early Hints before proxying)

### Cache Variant Fallback

`PageSpeedSelector::select()` scores alternates by mask similarity (format +1000,
Original fallback +100, viewport +80, encoding +60 exact / +5 identity fallback,
density +40, save-data +20). Mismatched non-identity encoding is a hard
disqualification (score 0). Original content always serves as reliable fallback.
SVG alternates receive a universal format bonus (+1200) and skip viewport/density
penalties (resolution-independent). Save-Data adds +50 to SVG scores. SVG bonus
is suppressed when the client explicitly requests raster-only formats.

### Learned Quality Prediction

Per-format ML models (LightGBM compiled to C via TL2cgen) predict optimal encoder
quality for the target SSIMULACRA2 score. ~5us inference, zero runtime dependencies.
SSIMULACRA2 verification runs as safety net with asymmetric tolerance `[target - 0.6*tol, target + 1.6*tol]`.

**CLI flags:**
- `--no-learned-quality` — disable all learned quality prediction
- `--no-learned-quality-jpeg` — disable for JPEG only
- `--no-learned-quality-webp` — disable for WebP only
- `--no-learned-quality-avif` — disable for AVIF only
- `--savedata-score-reduction N` — SSIMULACRA2 points to subtract for Save-Data (default: 15)

**Config fields** (`ImageTranscoderConfig`): `learned_quality`, `learned_quality_jpeg`,
`learned_quality_webp`, `learned_quality_avif`, `savedata_score_reduction`.

**Stats counters** (`WorkerStats`): `learned_quality_predictions`, `learned_quality_fallbacks`.

### Windows Port Gotchas

The worker and build system compile on Windows. Key rules when writing cross-platform code:

- **Socket paths**: On Windows, Unix socket paths must be converted to Named Pipe paths (`\\.\pipe\<path>`). `Worker::Initialize()` does this automatically. Do not assume Unix socket semantics on Windows.
- **POSIX compat**: Include `src/worker/posix_compat.h` instead of raw POSIX headers. Do NOT use `#define unlink _unlink` macros — they conflict with MSVC STL internals (e.g., `std::filebuf::open`). Use `_unlink`, `_open`, etc. directly inside `#ifdef _WIN32` blocks.
- **Atomic rename**: Use `ps_rename(src, dst)` not `std::rename()`. On Windows, `std::rename()` fails if the destination exists; `ps_rename()` uses `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`.
- **Global defines**: `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `_USE_MATH_DEFINES` are set in `.bazelrc` for all Windows targets. Never include `<windows.h>` before these are defined.
- **libaom build**: Requires cmake + Perl + Ninja + MSYS2 coreutils on PATH on Windows. See `third_party/libaom.BUILD` for MSVC path auto-detection logic.
- **Windows ASan**: `--config=windows-asan` is informational in CI; MSVC ASan may produce false positives. Do not treat windows-asan CI failures as blocking without verification on Linux.

### Windows Build System Gotchas

- **PDB contention**: Parallel MSVC compilation can produce PDB file contention errors. Use `/Fd` with a unique path per-target if you see "could not open PDB" errors during parallel builds.
- **Stale Bazel cache on Windows**: Windows file locking can prevent Bazel from clearing stale artifacts. If builds produce mysterious failures after switching branches or configs, try `bazel clean --expunge` or move `output_base` to a ReFS/Dev Drive (which avoids minifilter overhead).
- **Handle truncation**: Windows `HANDLE` is a pointer-sized type (8 bytes on x64). Casting to `int` (4 bytes) truncates it silently. Use `intptr_t` or `HANDLE` directly. The `posix_compat.h` wrappers handle this correctly — use them.
- **ASan flag scoping for clang-cl**: Sanitizer instrumentation flags must be scoped to user code, not third-party deps. Use `per_file_copt` or `copts` on specific targets rather than global `--copt` flags that instrument everything (including libaom, abseil, etc.), which causes false positives and OOM.
- **BSD tar vs GNU tar vs Windows tar**: macOS uses BSD tar, Linux uses GNU tar, Windows has its own tar. They differ in path separator handling, symlink support, and long filename handling. CI scripts that use `tar` must account for these differences. Prefer `zstd` for compression (available on all platforms via Bazel toolchain).

## Code Quality

### Before Committing

**Pre-commit hooks** run automatically (formatting/linting only):
clang-format, buildifier, trailing whitespace, end-of-file fixer.

**Before staging**, format with the single CI-faithful entry point so the hook
does not modify files mid-commit:
```bash
tools/format.sh              # format in place (whole CI scope)
tools/format.sh --check      # verify only, like CI
```
`tools/format.sh` resolves a clang-format **20.x** binary itself — a local
`clang-format-20`, else a pinned `clang-format==20.1.8` pip wheel it bootstraps
into a shared cache venv (the same wheel `mirrors-clang-format` installs;
verified byte-identical to the CI `clang-format-20`). **Do not run a bare
`clang-format`** (a Homebrew `llvm` ships a newer major that reformats
differently) — `tools/format.sh` is the one command that always matches CI, no
remote formatter box needed.

BUILD files are auto-formatted by the pre-commit hook (buildifier).
Do NOT run buildifier manually — it's not in PATH.

**Before committing significant C/C++ changes**, run sanitizers:
```bash
./tools/docker-sanitizers.sh
```

**clang-format version**: The pre-commit hook pins clang-format via the
`mirrors-clang-format` rev in `.pre-commit-config.yaml` (currently **v20.1.8**), and
CI runs `clang-format-20`. Always match the rev in `.pre-commit-config.yaml` rather
than trusting this literal — it is the source of truth on a version bump. Verify the
local version before formatting:

```bash
/opt/homebrew/opt/llvm/bin/clang-format --version  # major must match the .pre-commit-config.yaml rev (20.x)
```

If the local clang-format major differs from the pinned rev, format output will
differ from the hook and CI, causing failures — which is exactly why
`tools/format.sh` exists: it pins the version for you. `pre-commit install` also
wires a **pre-push** gate (`clang-format-push-gate`) that runs
`tools/format.sh --check --changed` on the files being pushed, so divergent
formatting is caught before it leaves your machine even if a commit used
`--no-verify`.

### clang-tidy

`.clang-tidy` is active for `(lib|src)/.*\.h$` files with these enforced checks:
- `bugprone-*` (except `bugprone-easily-swappable-parameters`) — catches common bug patterns
- `modernize-use-nullptr` — enforces `nullptr` over `NULL`/`0`
- `modernize-use-override` — all virtual function overrides must have `override` keyword
- `performance-*` — catches unnecessary copies, `string_view` opportunities

Violations appear in clangd IDE warnings. To reproduce the **CI lint gate locally**,
run:

```bash
./tools/run-clang-tidy.sh          # builds pagespeed2-lint, generates compile_commands.json, runs run-clang-tidy-20 (libc++)
./tools/run-clang-tidy.sh --fix    # same, applying auto-fixes in place
```

A bare `clang-tidy --fix <file>` on the host is **NOT equivalent**: it uses the
host's clang-tidy version, lacks `compile_commands.json`, and falls back to a
libstdc++ that cannot parse the C++23 `<expected>` headers. Use the script to match
CI.

### Developer Setup

```bash
brew install pre-commit && pre-commit install
./tools/docker-build.sh   # Docker image for sanitizer runs

# Language servers (optional, for semantic code navigation in your editor)
npm install -g typescript-language-server typescript   # TypeScript/JS (workbench)
pipx install python-lsp-server                         # Python (tools/)
# clangd ships with Xcode / LLVM — no separate install needed
```

### License Headers

All source files MUST include the Apache-2.0 license header as the first content
(after any shebang line). Use SPDX format:

**New code (src/, new files in lib/, tools/workbench/):**
```
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
```

**Ported code (lib/ files derived from mod_pagespeed):**
```
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.
```

**Python/Shell/BUILD files:** Use `#` comment style instead of `//`.

**Excluded**: vendored third-party sources (the repo-authored `third_party/*.BUILD` files
ARE checked), test fixtures, config files, generated files. `tools/check-license-headers.sh`
is the per-file-type gate; Apache RAT (`tools/ci/rat.sh`, exclusions with reasons in
`.rat-excludes`) sweeps the whole tree and must report 0 Unknown Licenses.

**When adding dependencies**: Update THIRD-PARTY-NOTICES and re-run SBOM
generation (`tools/generate-sbom.py`). Verify the new dependency's license is
permissive (MIT, BSD, Apache-2.0, Zlib, ISC). No GPL/AGPL/LGPL dependencies.

### Docker Commands

```bash
./tools/docker-build.sh              # Build Docker image
./tools/docker-test.sh test //...    # Run bazel in Docker
./tools/docker-sanitizers.sh          # All sanitizers (ASan+UBSan+LeakSan+TSan)
```

### Native Sanitizer Runs

Prefer `./tools/native-sanitizers.sh` over bare `bazel test --config=asan //...` — it applies required target exclusions.

```bash
./tools/native-sanitizers.sh         # ASan+UBSan+LSan + TSan (all)
./tools/native-sanitizers.sh asan    # ASan+UBSan+LSan only
./tools/native-sanitizers.sh tsan    # TSan only
```

**Excluded targets** (libaom/GIF OOM under sanitizer memory overhead):
- `//test/src/worker:image_transcoder_test`
- `//test/src/worker:worker_test`
- `//test/e2e:pipeline_test`
- `//test/lib/image:gif_reader_test`

**Additional TSan-only exclusion:**
- `//test/src/worker:static_file_handler_test` (UV loop timing test, flaky under TSan)

**Suppression files:**
- `tools/lsan_suppressions.txt` — Arena-allocated HTML nodes (HtmlElement, HtmlLeafNode, HtmlCharactersNode, HtmlParse factories)
- `tools/tsan_suppressions.txt` — Cyclone HitTracker benign counter race

**Test tags**: New multi-threaded tests involving image encoding (libaom, libavif, libwebp), VTracer FFI, or worker thread pools must be tagged `tags = ["no_tsan"]` in their BUILD rule. The `test:tsan` config in `.bazelrc` automatically filters them via `--test_tag_filters=-no_tsan`.

### Test Coverage

```bash
./tools/coverage.sh //test/lib/base/...  # Local (requires lcov: brew install lcov)
./tools/docker-coverage.sh               # Docker (all tests, lcov pre-installed)
./tools/docker-coverage.sh //test/lib/... # Docker (specific targets)
```

Report generated at `coverage-html/index.html`. Instruments `lib/` and `src/` only.
Browser tests that require Chrome will fail in the dev container but coverage is
still collected from all passing tests.

### Workbench (Web Console + VS Code Extension)

```bash
cd tools/workbench && pnpm install    # Install dependencies
cd tools/workbench && pnpm test       # Run all 478 TypeScript tests
cd tools/workbench && pnpm -C packages/web-shell dev  # Dev server (Vite hot reload)
```

SvelteKit web console served at `/console/*` by the worker's HTTP API (port 9880).
C++ HTTP server in `src/worker/http_server.h/.cc`. The endpoint table (REST +
WebSocket) lives in `src/worker/CLAUDE.md`; console/test details in
`tools/workbench/CLAUDE.md`.

### E2E Tests

```bash
./tools/e2e/run_e2e.sh                                   # Full stack
cd tools/e2e && pytest test_user_stories.py -v -k "css"   # Specific
```

3 Docker Compose containers (origin, worker, nginx) sharing a named volume.

### Real-World Proxy Tests

```bash
./tools/realworld-tests/run_realworld.sh              # Full stack (up → test → down)
./tools/realworld-tests/run_realworld.sh -k privacy   # Specific category
cd tools/realworld-tests && pytest -v                  # Manual (needs running stack)
```

2 Docker Compose containers (nginx + worker) proxying directly to real internet
sites (example.com, httpbin.org). 30 pytest tests covering proxy connectivity,
MISS→HIT caching, header preservation, privacy (no-store/private never cached),
image format negotiation (WebP/AVIF), and worker notifications.

Nginx :8083, worker API :9882. Requires: `pip install pytest requests`.
Configurable via env vars: `NGINX_URL`, `WORKER_API_URL` (see `conftest.py`).

**Gotchas:**
- httpbin uses HTTP (not HTTPS) — httpbin.org returns Swagger HTML via HTTPS proxy
- `proxy_pass` with nginx variables needs explicit `rewrite` to strip location prefix
- Module only caches `text/html` and `image/*`, not `application/json`

### ASP.NET Core Demo Stack

`./run-demo.sh` in `samples/aspnetcore/samples/DemoSite` (full stack, `--standalone`,
`--build-only`). Ports, env vars (`PAGESPEED_CACHE_PATH`, `PAGESPEED_SOCKET_PATH`),
and the multi-stage Dockerfile are documented in `samples/aspnetcore/CLAUDE.md`.

### Known Issues

- **JXL format slot repurposed as SVG**: kJxl renamed to kSvg; `image/jxl` in Accept headers maps to kOriginal
- **Cache keys include query strings**: `ngx_http_pagespeed_cache_url()` joins `r->uri` + `r->args`
- **PURGE unreliable under cache pressure**: Cyclone library behavior

### Common Failure Modes

| Symptom | Cause | Fix |
|---------|-------|-----|
| `clang-format: command not found` (macOS) | Not in PATH | Use `/opt/homebrew/opt/llvm/bin/clang-format` |
| `buildifier: command not found` | Hook-managed, not in PATH | Don't run manually; pre-commit hook handles it |
| Cache HIT returns original content | `enable_mmap_directory` not set | Both nginx and worker must set `enable_mmap_directory = true` |
| Worker writes invisible to nginx | Same as above, or permissions | Volume/socket are 0660 `pagespeed:pagespeed` — the web-server user must be in group `pagespeed`; a permission-denied degrade is logged loudly |
| Build fails with C++20 errors | Cyclone requires C++23 | Check `.bazelrc` has `--cxxopt=-std=c++23` |
| Image tests fail under TSan | Known false positives | Use `--test_tag_filters=-no_tsan` or skip image tests |
| Nginx module build fails | Missing nginx headers | Set `NGINX_PATH` or use Docker build |
| LeakSanitizer fails on macOS | Linux-only feature | Use `--config=asan` not `--config=asan-leaks` |
| All responses MISS, cache never populates | `emit_vary()` called before `vary_uncacheable()` in `src/nginx/ngx_pagespeed_module.cc` — module's own User-Agent Vary token poisons cacheability check | Ensure `vary_uncacheable()` runs before `emit_vary()` in header filter |
| Worker logs "Original not found in cache" on first request | Cold-start race: nginx sends notification before mmap write is visible to worker | Benign — worker retries via `ReadOriginalWithRetry()` (10ms initial delay, up to 3 retries). If still missing, nginx fallback-hit re-notifies on next request. |
| Tests fail with socket path too long | macOS `sun_path` limit is 104 bytes (vs 108 on Linux) | Hardcoded limit is 107 in `ParseSharedConfig`. Use shorter paths in tests on macOS. |
| Browser analysis refuses and the message names SIGSYS | A syscall filter denied the sandbox probe -- not the kernel's user-namespace policy. In a container this is the runtime's default seccomp profile (which every stock container has); on a host it is whatever syscall filter the service runs under. | Run the container with a Chrome-compatible seccomp profile, or relax the syscall-filter/namespace restriction the service runs under. `GET /v1/health` reports `syscall_filter` so you can see whether a filter is attached at all -- note `"filtered"` means *a* filter, not necessarily ours. |
| Browser analysis refuses to start; log says "REFUSING to start browser analysis" | The headless-Chrome sandbox is unavailable and `--browser-sandbox` is `require` (the default). Most often: the process is root (Chrome will not sandbox as uid 0 — this is every container image today, see #1429), or the kernel/AppArmor denies unprivileged user namespaces. | Intended, not a bug: the daemon keeps serving and never falls back to an unsandboxed browser. Run unprivileged, or take the documented opt-out with `--browser-sandbox=off` / `PAGESPEED_BROWSER_SANDBOX=off`. `GET /v1/health` reports `browser_sandbox`. |
| Worker exits at startup with "refusing to start -- the management API ..." | The H5 invariant: remote is never unauthenticated, unauthenticated is never remote | The message names the one flag that makes the requested posture legal (`--api-allow-remote`, `--api-no-auth`) or tells you to set `PAGESPEED_API_TOKEN`. See `deploy/pagespeed-optimizer.default`. |

### What to Test After Changes

| Changed component | Run these tests |
|-------------------|-----------------|
| `lib/base/` | `bazel test //test/lib/base/...` |
| `lib/html/` | `bazel test //test/lib/html/...` |
| `lib/image/` | `bazel test //test/lib/image/...` (tag: `no_tsan`) |
| `lib/cache/` | `bazel test //test/lib/cache/...` |
| `lib/classify/` | `bazel test //test/lib/classify/...` |
| `lib/css/` | `bazel test //test/lib/css/...` |
| `lib/js/` | `bazel test //test/lib/js/...` |
| `src/worker/` | `bazel test //test/src/worker/...` (tag: `no_tsan`) |
| `src/nginx/` | `bazel test //test/src/nginx/...` (unit, tag: `manual`, needs `NGINX_PATH`) |
|              | `./tools/run-test-nginx.sh` (module integration, needs nginx + Test::Nginx) |
| `src/browser/` | `bazel test //test/src/browser/...` |
| `src/crypto/` | `bazel test //test/src/crypto/...` |
| Cross-component | `bazel test //test/e2e/...` (tag: `no_tsan`) |
| `tools/realworld-tests/` | `./tools/realworld-tests/run_realworld.sh` (needs internet) |
| `src/nginx/` Cache-Control logic | `./tools/http-compliance/run_compliance.sh` (RFC 9111 validation) |
| Any cache behavior change | `cd tools/production-tests && docker compose up --build --abort-on-container-exit` |
| Full validation | `./tools/docker-sanitizers.sh` |

### Deployment

Production configs in `deploy/`:

Docker release images via `docker/build-release.sh`.

## Git Workflow Rules

- **A change to `src/` or `lib/` needs a changelog fragment in the same PR:**
  `changelog.d/<branch-slug>.<category>.md` (see `changelog.d/README.md`).
  Do not edit `CHANGELOG.md` directly; the release cut folds the fragments in.
  Enforced by `tools/ci/check_release_note.sh` in CI. Where the change is
  genuinely invisible to users, record that instead — `Release-Note: none —
  <reason>` in a commit message — rather than leaving the question unanswered.
  Check it before pushing:
  `bash tools/ci/check_release_note.sh --base origin/main --head HEAD`.
  Details in RELEASING.md.
- Security fixes are not opened as public pull requests or issues; follow `SECURITY.md`.
- Always verify current branch with `git branch --show-current` before committing.
- Never use `git commit --amend` unless the user explicitly requests it.
- Always `git push` after committing unless told otherwise.
- When working across branches, confirm the target branch with the user before committing.

## Debugging Methodology

- Read the full error output before hypothesizing a cause.
- Identify the exact compiler error before attempting fixes.
- When a fix fails in CI, analyze WHY it failed before trying another approach — do not iterate blindly.
- For crash debugging, use proper tools (core dumps, sanitizer output) rather than indirect breadcrumb approaches.
- Verify hypotheses with evidence before acting on them.
