---
title: 'mod_pagespeed 1.15: what six years of stewardship look like'
description: 'mod_pagespeed 1.15 ships 765 merges since 1.13.35.2: image-decoder hardening, a full sanitizer matrix, current dependencies and the IIS port in the tree.'
date: 2026-05-17
lastUpdated: 2026-07-04
tags: [release, security, dependencies]
product: '1.1'
---

mod_pagespeed is a server-side module that rewrites HTML, transcodes images, inlines critical CSS, and minifies CSS and JavaScript at request time. It runs inside Apache, nginx, Envoy, or IIS, so optimization happens on the server you already operate rather than via a CDN or a sidecar service.

The last upstream mod_pagespeed release shipped in mid-2020. Since then, the libpng, libwebp, gRPC, nginx, and libcurl CVE streams have all kept landing against a tree that nobody was rebuilding. Distributions moved on. Compilers got stricter. Chromium itself deprecated the build system mod_pagespeed depended on (`gyp_chromium`).

**mod_pagespeed 1.15** is what the codebase looks like after six years of We-Amp stewardship. We-Amp, founded in 2012, helped build ngx_pagespeed and maintained mod_pagespeed, and has carried the line forward ever since it went quiet — contributing substantively to ngx_pagespeed, helping drive the Apache incubation, and staying engaged through the years of upstream silence that followed. 1.15 is the same maintenance, back at full velocity. 765 first-parent merges, a current dependency graph, sanitizer coverage in CI, and four ports — Apache, nginx, Envoy, IIS — building from one source tree. Apache and IIS are stable. nginx is in early access; Envoy is experimental.

After Apache incubation wound down, the codebase saw twelve commits across the next five years; the five months that produced 1.15 saw more than eight hundred.

## Defense-in-depth on a six-year-stale tree

The threat model has moved since 2018, and a maintained tree moves with it.

A large slice of this release is defense-in-depth work. The strategy across all of it was conservative and consistent: validate untrusted input early, fail closed rather than abort the worker, treat concurrency hazards as bugs to be eliminated rather than tolerated, and let continuous validation in CI catch what code review misses.

Continuous validation in this release means the CI matrix on every PR exercises:

- AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer on Linux x64
- ThreadSanitizer on Linux x64
- AddressSanitizer on Windows via clang-cl
- AppVerifier on the IIS port, with timeout multipliers and evidence collection in CI

Linux arm64 builds, unit tests, and Apache/nginx sys-tests run on every PR as well — without a sanitizer pass on that target yet.

Any class of memory or concurrency error these tools catch fails CI before merge. They surfaced what you would expect across a six-year-stale tree, and they keep catching it in code under active development.

### The IIS port: a divergent fork, normalized

mod_pagespeed's IIS port did not sit idle during the upstream dormancy. The standalone IISpeed product was a divergent fork that We-Amp continued to ship, maintain, and apply security work to while mainline mod_pagespeed sat still. By 2026, IISpeed and the C++ tree had years of fixes apart from each other.

1.15 normalizes that fork. IISpeed's hardened runtime patterns are now in the main C++ tree, so the IIS port and the Linux ports share one source of truth — one filter set, one fetcher, one admin console. The carry-over folds in IIS-specific lifecycle and concurrency hardening that IISpeed had absorbed over the years, admin-handler findings closed during pre-release review, and HTTP-plumbing corrections on the admin API.

## The dependency graph in 2026

The other half of the security story is the supply chain. The upstream 1.13.35.2 tree carried libraries pinned to 2018-vintage versions, several of them Chromium forks pinned for years. 1.15 rebases the graph:

| Component     | 1.15        | Notable                                                 |
| ------------- | ------------ | ------------------------------------------------------- |
| Envoy         | v1.37.2      | May 2026 CVE cluster (CVE-2026-26308/09/10/11/30)       |
| BoringSSL     | 0.20260508.0 | Replaces in-tree NSS; libcurl on Linux links against it |
| gRPC          | 1.78.1       | Upstream baseline carried 1.6.0                         |
| nginx         | 1.30.2       | May 2026 CVE cluster                                    |
| libcurl       | 8.20.0       | Built from source via `rules_foreign_cc 0.11.1`         |
| libpng        | 1.6.58       | Four parser-hardening point releases                    |
| libwebp       | 1.5.0        | Post-CVE-2023-4863                                      |
| libjpeg-turbo | 3.1.4.1      | Switched from Chromium fork to upstream                 |
| zlib-ng       | 2.3.2        | Replaces zlib                                           |
| Abseil        | 20260107.1   | Dual-aliased for strict-deps on Windows                 |

libjpeg-turbo moved from a Chromium fork — which hadn't seen an upstream merge in years — to upstream v3.1.4.1. The switch meant writing a native Bazel BUILD file that emulates `configure_file()` via genrules. `getenv`/`putenv` are disabled at build time for defense in depth on the worker process.

The switch from zlib to **zlib-ng** is the supply-chain change with the most visible downstream effect on what mod_pagespeed actually outputs. zlib-ng's SIMD-accelerated longest-match and CRC32 paths cut image-output time, and at the compression levels the optimizer uses it produces smaller output. The project's own 128-image PNG reference fixture saw a net **~4.8% reduction in total bytes** from the zlib-ng switch alone (most images smaller, a handful larger by a few bytes).

libcurl on Linux now links against the same BoringSSL that Envoy links against. One TLS library, one set of symbols, no version skew. On Windows libcurl uses SChannel so there's no second TLS implementation on the box. The Apache port hides BoringSSL's symbols via a version script so they don't conflict with whatever the host process already loaded.

Security hygiene on the upstream library set has been continuous since the Apache incubation ended — ongoing on the closed-source We-Amp fork that carried the maintenance through the years of public dormancy. What 1.15 changes is scope. AI in the loop let the same discipline stretch wider: four HTTP front-ends, both x86_64 and arm64, a unified codebase, and a shared Python test surface that catches port-specific regressions before merge. The May 2026 coordinated bump (#107, #109, #110, #115) — BoringSSL, libpng, re2, gRPC, curl, Envoy, and nginx all moved together to clear the Q2 CVE clusters — is one cut of that ongoing work, now landing on the public release line.

The graph is auditable, and the build is hermetic. Every dependency is declared in `bazel/repositories.bzl` with a SHA. Each release is built from a vendored-dependency tree so the build is reproducible offline. No customer-facing source distribution — installs come from the signed apt/yum repo, .deb/.rpm packages, or the dynamic nginx module.

Build environment: Bazel 7.x on a classic WORKSPACE (`MODULE.bazel` is staged but bzlmod is off), Clang 20 as the supported compiler (GCC 13 also builds Linux; MSVC + clang-cl on Windows), C++20 by default with C++23 for the Cyclone wrapper.

## Cyclone Cache

1.15 replaces the legacy `FileCache` with **Cyclone**, a C++23 disk cache shared with the optimizer worker. The headline operational change: no more periodic pruning scans across millions of small files. The in-tree benchmark records the rest — versus `FileCache`, writes ~19× faster, deletes ~27× faster, large reads ~4.5× faster, small reads slightly slower. Code at `test/pagespeed/kernel/cache/disk_cache_speed_test.cc`; the [Cyclone versus file-cache benchmark](/blog/cyclone-cache-vs-file-cache-benchmark/) walks through the methodology and the full numbers.

On upgrade: existing `FileCache` directories aren't read; `FileCacheInodeLimit` parses as a deprecated no-op so Apache configurations keep loading.

## A modernized admin console

The admin console was the other 2010-vintage subsystem. The original handler rendered server-side HTML — functional then, awkward now. 1.15 replaces it with a Svelte 5 + Vite single-page application, bundled as a single file so it ships inside the module itself with no extra static-asset deployment step, talking to the same admin endpoints across all four ports.

The console is the operator UI for license state, cache configuration, and filter configuration. It is stamped with the release tag at build time, so the running build is unambiguous from the UI itself. The same SPA renders on all four ports.

## A shared system test platform

The upstream test infrastructure was bash and Perl, Apache-shaped. It worked, but adding ports meant per-port bespoke harnesses, and Windows coverage in particular needed its own scaffolding.

1.15 ships a Python pytest framework — `test/system/pagespeed_test_framework/` — shared across Apache, nginx, Envoy, and IIS. Every port runs the same behavioral test surface against its own server runtime: HTML rewriting, in-place resource optimization, image transcoding, JavaScript inlining, beacon handling, license-enforcement paths. Pytest markers (`@pytest.mark.ipro`, `@pytest.mark.slow`, `@pytest.mark.license`, and so on) let CI select subsets per matrix leg without forking the test files.

The practical effect: when a fix lands on the Apache port, the same test catches a regression on nginx or Envoy or IIS without anyone writing a port-specific test.

## How 1.15 actually got built

Two investments shaped how this release came together.

**AI in the loop.** Autonomous AI workflows ran continuously on this codebase through the 1.15 cycle — for triage, planning, code review, dependency audits, and large-scale refactors. AI agents proposed patches and ran review passes; every commit was signed off by a maintainer and exercised by the sanitizer matrix before merge. Security audits and dependency-graph sweeps benefited the most: audit sweeps across thousands of files for outdated patterns and latent concurrency hazards, applied systematically against a six-year-stale tree.

**A CI lab that keeps up.** We've invested in dedicated build hardware over the past two years — multi-machine, multi-platform, multi-architecture — so that every PR runs across Linux x64 + arm64, Windows x64, and macOS arm64, with full sanitizer coverage, in minutes rather than hours. The release pipeline runs end-to-end on this lab.

AI generates the candidate change, the CI matrix proves it does not regress, a human reviews the resulting diff. That loop ran thousands of times across the 1.15 cycle.

## Apache, nginx, Envoy, IIS — one tree, four front-ends

mod_pagespeed historically meant "Apache, plus a separately-maintained nginx port that drifted." 1.15 produces optimization modules for the four most common production HTTP front-ends from a single tree:

**Apache 2.4+** is the stable headline port. The APR-coupling that ran through the whole upstream tree has been confined to `apache/`; everything else uses cross-platform substitutes (`StdThreadSystem`, `StdCondvar`, `LibeventDispatcher`, `WindowsSharedMem`). Apache 2.2 support is dropped — the `_ap24` suffix is gone from the package name. The Apache admin JSON endpoints now handle POST bodies correctly; the prior implementation silently dropped them.

**nginx 1.30.x** (early access) builds as `ngx_pagespeed_module.so` — dynamic, no recompilation against the nginx source tree. The HTTP fetcher is no longer the Apache-coupled Serf library; every port uses the new `CurlUrlAsyncFetcher` over libcurl + BoringSSL. The iterator-invalidation bug in `CancelActiveFetches` is gone. Backoff on broken pipe writes is exponential rather than busy-looped.

**Envoy** (experimental) builds against v1.37.2 with an independent outbound fetcher, IPRO, admin endpoints, query-parameter config, and a sharded `AdminRateLimiter` mutex. Two artifact shapes: a standalone binary at ~212 MB and a shared library at ~113 MB.

**IIS 10+** ships as `pagespeed_iis.dll`, signed via Azure Trusted Signing, installed via a WiX MSI pinned to WiX v5.0.2. Day-2 upgrades — installing 1.15 on top of an existing PageSpeed install with w3wp running — go through a WAS stop and a w3wp force-kill so the locked DLL is released before the new bits drop. The CI matrix exercises this path on a dedicated Windows VM kept intentionally in a day-2 state with a pre-installed older PageSpeed, because that's the upgrade real operators hit.

**Linux on arm64** is also new. The upstream tree had no aarch64 build at all; 1.15 ships arm64 builds for Apache and nginx alongside x86_64. AWS Graviton and Ampere servers can deploy mod_pagespeed without an out-of-tree port. macOS arm64 is supported as a development platform, not a shipping target. i386 is dropped from the package list.

The `X-Page-Speed` response header is unified across ports — no more port-specific overrides, no special expired-state header that broke header-based assertions in operator monitoring.

## Where to get it, what's next

The [product overview](/) covers the four ports, support status, and licensing. Full release documentation — installer downloads, configuration, port-specific notes — lives in [the 1.15 docs](/docs/), with a thematic [what's-new summary](/docs/release-notes/#115-history) of everything that shipped. If you're comparing the paths, [the alternatives post](/blog/mod-pagespeed-alternatives/) lays out the trade-offs; if you're coming off a Google-era 1.13.x or ngx_pagespeed install, [the migration guide](/blog/migrating-from-1x/) walks through the config mapping and verification steps. mod_pagespeed is licensed under the Apache License 2.0; support subscriptions are on the [pricing page](/pricing/).

If you've been running an [unpatched mod_pagespeed](/blog/mod-pagespeed-deprecated-2026/) against a 2020-vintage dependency graph: 1.15 ships on current toolchains, against libraries that have been patched.
