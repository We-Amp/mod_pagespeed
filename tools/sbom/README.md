# Dependency verification

Three layers, deliberately separate:

| Layer | Mechanism | Status | Blocks build? |
|---|---|---|---|
| **Curated SBOM gate** | `tools/generate-sbom.py` → grype on `sbom/pagespeed-1.1.spdx.json` | shipped | **yes** (per-PR `security-gates` + release backstop) |
| **Comprehensive scan** | `tools/sbom/dep-scan.sh` → syft + grype | this dir | **no** (report-only) |
| **Transitive C/C++ matcher** | per-dep `cpe` + `release_date` + cached NVD feed | separate work (`cpp-cve-matcher`) | tracked elsewhere |

> The curated `generate-sbom.py` gate remains the **only blocking** dependency
> check. It verifies the committed `sbom/pagespeed-1.1.spdx.json` matches what
> `bazel/repositories.bzl` (+ the bundled admin-console deps) produce today, and
> fails the build on medium+ grype findings (with `sbom/pagespeed-1.1.vex.json`
> suppressions). This comprehensive scan is **additive and report-only** — it
> decides nothing yet. The "no drift" payoff lands later, when these
> ground-truth surfaces are ratcheted in to *replace* the hand-curated SBOM rows
>. The transitive vendored C/C++ surface (Envoy / libwebp / optipng /
> …) is **not** covered here at all — it is handled by the separate
> `cpp-cve-matcher` work (an Envoy-style per-dep `cpe`+`release_date`+NVD model),
> because grype rarely matches CVEs off the `pkg:github` PURLs those deps carry.

## `dep-scan.sh` — report-only comprehensive scan

Generates SBOMs from **committed, deterministic inputs** and scans with grype:

| Surface | Input (deterministic) | Status |
|---|---|---|
| **npm** | `pagespeed/system/console/pnpm-lock.yaml` (committed; the admin-console SPA, the only shipped JS surface) | covered |
| **images** (`--images`) | the **built package-test images** from the local docker cache — `mps-smoke-nginx`, `package-test-test-ubuntu-apache`, `package-test-test-rocky-apache` (a production-representative base OS + Apache/nginx with the actual shipped `.deb` / `.rpm` / `ngx_pagespeed` `.so` installed on top — `test/package-test/Dockerfile.*`) | covered, build-machine only |
| **cargo** | — | **n/a** — 1.1 ships no Rust (no `Cargo.toml` / `Cargo.lock` in the tree) |
| **.NET** | the shipped **`.nupkg`** / publish output (the `aspnetcore` middleware + `WeAmpSite`), **NOT** a dev-time `dotnet restore` (that undercounts and the NativeAssets are injected by the build) | **deferred** to a post-build scan |
| **transitive C/C++** | Bazel external repos — Envoy's `cpe`+`release_date`+NVD model | **out of scope here**, handled by the separate `cpp-cve-matcher` work |

It scans **only** each committed lockfile (copied into an isolated temp dir),
never a project tree — a `dir:` scan pulls in whatever `node_modules` / `obj` /
`bin` happen to be present (a .NET project dir was observed cataloging 744 npm
pkgs) — and it never generates a lockfile at scan time (a generated lockfile can
resolve differently than what the build pins).

**Images are the real shipped C/C++ surface, not a base-image proxy.** We scan
the package-test images *after the actual `.deb`/`.rpm`/`.so` is installed* (so
the scan sees the apt/dnf + built-module layer the product adds), and we
deliberately do **not** scan the `pagespeed1.1-dev` build image or the bare
`ubuntu:22.04` / `rockylinux:9` base layers — a base/build image is false
confidence. The images are scanned from the local docker cache, so `--images`
only produces results on the build machine where they exist (after a release
build); absent images show an explicit **not built** row.

```sh
bash tools/sbom/dep-scan.sh           # npm (committed lockfile)
bash tools/sbom/dep-scan.sh --images  # + built package-test images (build machine)
# outputs: sbom/scan/<source>.spdx.json, <source>.grype.json, SUMMARY.md
# sbom/scan/ is gitignored — generated, never committed.
```

syft and grype are **pinned** (`SYFT_PIN` / `GRYPE_PIN` in the script; the
`dep-scan.yml` workflow installs the same pins). The grype **DB is live**
(refreshed each run) — intentional for a report-only daily scan, so newly
disclosed CVEs surface promptly; results are therefore not byte-reproducible
run-to-run. the design record calls for a snapshotted feed *for the blocking flip*. Any
committed `sbom/*.vex.json` suppressions (the gate's
`sbom/pagespeed-1.1.vex.json`) are applied here too, so the report reflects the
same triaged reality as the blocking gate. The scan always exits 0.

## Workflow

The scheduled dependency-scan lane runs the npm surface on every push/PR to
`master` (fast), and the heavier image scan on the daily schedule +
`workflow_dispatch` (which also refreshes the grype DB). It runs on a dedicated Linux x64 CI runner,
self-installs the pinned syft/grype (no third-party scanner action — same
supply-chain-conservative choice as the 2.0 optimizer line source), writes the severity table
to the run's Summary tab, and uploads the SBOMs + grype reports as artifacts. It
**never** fails the build.

## Why report-only (the ratchet — the design record)

A raw scan surfaces a backlog, much of it not-applicable (vulnerable code not on
an execute path, dev/test-only deps). Failing on it immediately would wall every
PR. Instead:

1. Land report-only; the severity table appears on each run's Summary tab.
2. Triage findings into `sbom/*.vex.json` — `not_affected` + justification, or
   bump the real ones.
3. Once a surface is clean at medium+, flip it to blocking. Note this is **not**
   a one-line toggle today: the blocking gate scans the curated
   `generate-sbom.py` SBOM, a different package set than these per-source syft
   SBOMs. "Flipping" means moving a surface's source-of-truth from the curated
   list to the generated SBOM (and retiring the corresponding hand-maintained
   rows). That migration is the actual "no drift" win and is tracked as a later
   phase.

## Roadmap

- **Next:** scan the shipped **`.nupkg`** for the .NET surface (`aspnetcore`).
- **Then:** the transitive C/C++ matcher (`cpp-cve-matcher`) — per-dep
  `cpe`+`release_date`, cached NVD feed, completeness gate — for the vendored
  Envoy/libwebp/optipng/… tree, which grype's SBOM matcher cannot see.
- **Then:** migrate clean surfaces off the curated SBOM (the load-bearing "no
  drift" step).
