# Dependency verification

Three layers, deliberately separate:

| Layer | Mechanism | Status | Blocks build? |
|---|---|---|---|
| **Curated SBOM gate** | `tools/generate-sbom.py` → grype on `sbom/pagespeed-1.1.spdx.json` | shipped | **yes** (per-PR `security-gates` + release backstop) |
| **Comprehensive scan** | `tools/sbom/dep-scan.sh` → syft + grype | this dir | **npm: yes** (`blocking-dep-scan`); images: no |
| **Transitive C/C++ matcher** | per-dep `cpe` + `release_date` + cached NVD feed | separate work (`cpp-cve-matcher`) | tracked elsewhere |

> The curated `generate-sbom.py` gate remains the blocking check for the
> **C/C++** surface. It verifies the committed `sbom/pagespeed-1.1.spdx.json`
> matches what `bazel/repositories.bzl` (+ the bundled admin-console deps)
> produce today, and fails the build on medium+ grype findings (with
> `sbom/pagespeed-1.1.vex.json` suppressions). This comprehensive scan is now
> **blocking for npm** (`blocking-dep-scan`, medium+, pinned DB) and
> report-only for images. The "no drift" payoff lands later, when these
> ground-truth surfaces are ratcheted in to *replace* the hand-curated SBOM rows
>. The transitive vendored C/C++ surface (Envoy / libwebp / optipng /
> …) is **not** covered here at all — it is handled by the separate
> `cpp-cve-matcher` work (an Envoy-style per-dep `cpe`+`release_date`+NVD model),
> because grype rarely matches CVEs off the `pkg:github` PURLs those deps carry.

## `dep-scan.sh` — report-only comprehensive scan

Generates SBOMs from **committed, deterministic inputs** and scans with grype:

| Surface | Input (deterministic) | Status |
|---|---|---|
| **npm** | **every** committed lockfile — `pagespeed/system/console/pnpm-lock.yaml` (shipped admin-console SPA), `test/browser/`, `devel/loadtest_collect/`, `tools/js-minify-corpus/bundle-fixtures/` (`package-lock.json`) | covered, **blocking at medium+** |
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

### npm scope and its two silent-hole guards

The npm surface list lives in `NPM_LOCKFILES` in `dep-scan.sh`. Two guards exist
because both holes were real:

- **Coverage** (`assert_npm_coverage`): the registered list is cross-checked
  against `git ls-files`. The scan originally covered only the admin-console
  lockfile, so three committed `package-lock.json` files went unscanned for the
  whole report-only life of the gate while the summary table looked complete.
  An unregistered lockfile is now a loud row and a gate hit.
- **Dev dependencies** (`SYFT_JAVASCRIPT_INCLUDE_DEV_DEPENDENCIES=true`): syft's
  `package-lock.json` cataloger skips `"dev": true` entries by default. That
  reduced the js-minify corpus bundler to 1 of its 163 locked packages and the
  browser tests to 1 of 4 — both reporting a false `0/0/0/0`. Dev deps are in
  scope everywhere: a compromised dev/test toolchain runs on CI runners with
  repo credentials even though it ships nothing. `run_scan` additionally treats
  an empty catalog on a lockfile surface as a scan failure, not a clean result.

```sh
bash tools/sbom/dep-scan.sh                              # report-only, all surfaces
bash tools/sbom/dep-scan.sh --images                     # + built package-test images (build machine)
bash tools/sbom/dep-scan.sh --fail-on medium --surfaces npm   # the blocking gate (exits non-zero on findings)
# outputs: sbom/scan/<source>.spdx.json, <source>.grype.json, SUMMARY.md
# sbom/scan/ is gitignored — generated, never committed.
```

`--fail-on <sev>` switches to blocking mode; `--surfaces <csv>` restricts the
scanned set so still-report-only surfaces stay out of a gate. Blocking runs
**refuse to start** on an unpinned syft/grype — the gate reads grype's
`fix.state` enum and depends on syft's dev-dependency cataloging, so an
unpinned tool could fail it open.

### What is NOT a scan surface

`vendor/` is `.gitignored` and **generated** by `tools/vendor-deps.sh` (a Bazel
repo-cache plus a Cyclone checkout). Nothing in it is committed, so it does not
exist in a CI checkout. A `vendor/modpagespeed2/` tree may linger on a developer
box as a leftover of the pre-the design record `@modpagespeed2` Bazel `git_repository`
(1.1 no longer consumes 2.0 that way). It is a stale
copy of a *different repo's* source; its CVEs are pagespeed-optimizer's to fix and
are not actionable from this repo. Do not add it to `NPM_LOCKFILES`.

syft and grype are **pinned** (`SYFT_PIN` / `GRYPE_PIN` in the script; both
workflows install the same pins). The grype **DB posture differs by mode**:

- **report-only** (`dep-scan.yml`) uses the **live** DB, refreshed each run, so
  newly disclosed CVEs surface within a day. Results are therefore not
  byte-reproducible run-to-run — fine, because this path never fails a build.
- **blocking** (`blocking-dep-scan`) uses the **pinned** DB in
  `tools/sbom/grype-db-pin.json`, imported by `tools/sbom/provision-grype-db.sh`
  into a job-local `GRYPE_DB_CACHE_DIR` with `GRYPE_DB_AUTO_UPDATE=false` — the
  snapshotted feed the design record calls for. A gate whose verdict depends on the day
  would redden in-flight PRs that changed nothing. To bump the pin: take the
  current archive from `grype db list -o raw`, update `path`/`checksum`/`built`,
  and review it like any dependency bump.

Any
committed `sbom/*.vex.json` suppressions (the gate's
`sbom/pagespeed-1.1.vex.json`) are applied here too, so the report reflects the
same triaged reality as the blocking gate. The scan always exits 0.

## Workflow

Two workflows, mirroring pagespeed-optimizer:

The scheduled dependency-scan lane — the **report-only** live-DB sweep. Runs the
npm surface on every push/PR to `master` (fast) and the heavier image scan on
the daily schedule + `workflow_dispatch`. Runs on a dedicated Linux x64 CI runner, self-installs the pinned
syft/grype (no third-party scanner action — same supply-chain-conservative
choice as the 2.0 optimizer line source), writes the severity table to the Summary tab,
uploads SBOMs + grype reports, and files/closes a tracking issue. It **never**
fails the build.

`blocking-dep-scan` ("Blocking Dep Scan (npm)") —
the **blocking** npm gate: `dep-scan.sh --fail-on medium --surfaces npm` against
the pinned DB. Box-independent (any Linux x64 CI runner), no vendor tarball
or Docker.

> **Image-surface caveat.** The package-test images are scanned from the local
> docker cache and only exist there *after a release build*. The scheduled scan
> does not build them, so on a typical run every image row reads
> **not built** — that surface silently covers nothing. The rows are honest
> (never a `0/0/0/0`), but "no image findings" on a scheduled run means "no
> images", not "no CVEs". This is why images are **not** in the blocking set.

## Why report-only (the ratchet — the design record)

A raw scan surfaces a backlog, much of it not-applicable (vulnerable code not on
an execute path, dev/test-only deps). Failing on it immediately would wall every
PR. Instead:

1. Land report-only; the severity table appears on each run's Summary tab.
2. Triage findings into `sbom/*.vex.json` — `not_affected` + justification, or
   bump the real ones.
3. Once a surface is clean at medium+, flip it to blocking.

**npm has completed this ratchet.** Precondition checked before the flip: the
*widened* scan (all four committed lockfiles, dev deps included, pinned syft
1.44.0 / grype 0.112.0) reports `0/0/0/0` on every surface **without any VEX
suppression** — nothing was triaged away to make it green, and the two guards
above make a false green a failure rather than a pass. Images stay report-only
(see the caveat above). Note the surface-migration below is **not** a one-line
toggle: the curated gate scans the curated
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
