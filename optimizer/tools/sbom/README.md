# Dependency verification

Three layers:

| Layer | Mechanism | Status | Blocks build? |
|---|---|---|---|
| **Curated SBOM gate** | `tools/generate-sbom.py` → grype on `sbom/mod_pagespeed-2.1.spdx.json` | shipped | **yes** (release + per-PR hygiene) — the published license SBOM |
| **Ground-truth blocking gate** | `tools/sbom/dep-scan.sh --fail-on medium --surfaces npm,cargo` | shipped | **yes** (per-PR + push, pinned DB) — npm + cargo |
| **Image blocking gate** | `tools/sbom/dep-scan.sh --images --fail-on medium --surfaces npm,cargo,image` | shipped | **yes** (daily + dispatch) — where built images exist |
| **Comprehensive scan + notify** | `tools/sbom/dep-scan.sh` (no `--fail-on`) + tracking issue | this dir | **no** on push/PR (live-DB sweep + files an issue on medium+) |

The two blocking gates are **not double-gating**, they cover different package
sets:

- The **curated gate** scans the hand-curated `generate-sbom.py` SPDX — the
  C++/Cyclone provenance rows plus the summarized `JS_DEPS`/`RUST_DEPS` rows.
  It is the **published, license-annotated** SBOM (customer security
  questionnaires, attestation). It can still drift from what ships.
- The **ground-truth blocking gate** scans the FULL transitive closure of the
  real committed lockfiles (workbench `pnpm-lock.yaml` + vtracer `Cargo.lock`)
  — the actual ground truth those curated rows summarize, with **zero drift**
  (re-derived each run). A medium+ CVE in any of those lockfiles fails the PR.

## `dep-scan.sh` — ground-truth scan (report-only + blocking)

Generates SBOMs from **committed, deterministic inputs** and scans with grype.
Per-surface ratchet status (blocking vs report-only):

| Surface | Input (deterministic) | Coverage | Gate status |
|---|---|---|---|
| **npm** | `tools/workbench/pnpm-lock.yaml` (committed) | covered | **BLOCKING** at medium+ (the per-PR blocking job) |
| **cargo** | `lib/image/vtracer_ffi/Cargo.lock` (committed; the Bazel build consumes it via `crate_universe` `from_cargo`, so build == scan — genuine zero-drift) | covered | **BLOCKING** at medium+ |
| **images** (`--images`) | the **built product images** from the local docker cache — `modpagespeed/worker`, `modpagespeed/nginx`, `pagespeed2-base-runtime`, `pagespeed2-dev` (apt + Chromium + `.so` layers) | covered, build-machine only | **BLOCKING** (the scheduled/dispatch dependency-scan run — where the images exist; cold cache degrades to non-gating "not built"): medium-band findings gate when **fixable**, High/Critical gate **always** — see "Image gate shape" below. Exception: `pagespeed2-dev*` is **report-only** (owner decision 2026-06-11) — it is the CI build toolchain, never shipped to customers; its rows/findings stay in the severity table (marked ⚠) and the daily issue but never bump the gate. |
| **.NET** | the shipped **`.nupkg`** (NOT a dev-time `dotnet restore` — that undercounts and the NativeAssets are injected by the release pipeline) | **deferred** to a post-build scan | report-only (when added) |
| **transitive C/C++** | Bazel external repos — Envoy's `cpe`+`release_date`+NVD model | **deferred**, lands in `mod_pagespeed` (the Envoy tree) | report-only (when added) |

The optimizer has no shipped Go or Python surface, so those ecosystems are intentionally absent.

### Image gate shape (2026-06-11)

For **image** surfaces only, the blocking path counts medium-band findings only
when `fix.state == "fixed"` — ones a rebuild (`dist-upgrade` picks up the patched
deb) or a base bump actually resolves — while **High/Critical findings gate
regardless of fixability** (a Critical in a shipped image never rides silently;
the out is an evidence-backed VEX statement or structural removal, like the
2026-06-11 trixie→noble nginx re-base). Rationale: after the 2026-06 remediation
pass, 100% of the **shipped** images' remaining mediums are `wont-fix`/`not-fixed`
upstream — nothing we ship can act on them — and "suppressing" them with
`not_affected` VEX claims would be dishonest (the analysis Chromium genuinely
links the codec libraries, and it runs `--no-sandbox` in containers, so neither
`vulnerable_code_not_present` nor a sandbox-mitigation justification holds).
The **npm/cargo gates are unchanged**: any medium+ finding blocks, fixable or
not. The full population always appears in every report and the daily tracking
issue, and evidence-backed VEX suppression remains available.

The report-only `pagespeed2-dev` toolchain image is the one surface that does
carry fixable findings (including Highs — e.g. netty/openjdk in its 2.6k-package
population); they are visible in every summary (⚠ row) and the daily issue, and
a periodic dev-image rebuild sweep is the cheap way to keep that pile down.

### Published images (nightly)

The nightly also scans the images customers actually pull, so a CVE disclosed after a
release surfaces within a day. The `dep-scan-published` job resolves the newest published
(non-draft, non-prerelease) `v*` release, then runs
`dep-scan.sh --published-images <version> --fail-on medium --surfaces image` over
`ghcr.io/we-amp/pagespeed-worker`, `pagespeed-nginx` and `pagespeed-combined` at that
version, for both `linux/amd64` and `linux/arm64`. It uses the pinned grype database, the
same VEX files and the image gate shape above, and fails when the gate trips. Findings are
tracked in one issue titled "Published images: dependency findings", separate from the
nightly image scan issue.

### Two modes

```sh
# Report-only (default) — daily posture, always exits 0:
bash tools/sbom/dep-scan.sh             # npm + cargo (committed lockfiles)
bash tools/sbom/dep-scan.sh --images    # + built product images (build machine)

# Blocking — exits NON-ZERO on any finding at/above <sev> (after VEX
# suppression) on the requested surfaces:
bash tools/sbom/dep-scan.sh --fail-on medium --surfaces npm,cargo        # per-PR gate
bash tools/sbom/dep-scan.sh --images --fail-on medium --surfaces npm,cargo,image  # +images (the scheduled gate)
# outputs: sbom/scan/<source>.spdx.json, <source>.grype.json, SUMMARY.md
```

`--surfaces` restricts the scanned set (csv of `npm`,`cargo`,`image`); images
gate only when both `--images` is passed AND `image` is in `--surfaces`, so the
per-PR gate (`npm,cargo`) can't fail on images and a cold image cache degrades
to a non-gating "not built" row. Both flags accept
`--flag value` and `--flag=value`. The script is bash-3.2-safe (no associative
arrays / `${var,,}`) so it runs unchanged on macOS and the Linux CI runners.

### Grype DB: pinned for the blocking path, live for report-only

The **blocking** gate pins the grype DB so a newly-published CVE can't
non-deterministically break an in-flight PR/release:

- `tools/sbom/grype-db-pin.json` records the exact dated DB archive (`path` +
  `checksum` + `built`).
- `tools/sbom/provision-grype-db.sh` imports it (`grype db import <url>?checksum=…`)
  into an isolated `GRYPE_DB_CACHE_DIR`, and the gate runs with
  `GRYPE_DB_AUTO_UPDATE=false`. Bumping the pin is a deliberate, reviewed change
  (get the current archive from `grype db list -o raw`, update the JSON, re-run
  the gate).

The **report-only** scan (the daily scheduled run) uses the **live** DB so newly disclosed
CVEs surface promptly on the daily run; its results are therefore not byte-
reproducible run-to-run, which is fine for an alerting (non-gating) job.

### Determinism

It scans **only** each committed lockfile (copied into an isolated temp dir),
never a project tree — a `dir:` scan pulls in whatever `node_modules` / `obj` /
`bin` happen to be present (a .NET project dir was observed cataloging 744 npm
pkgs) — and it never generates a lockfile at scan time (a generated lockfile can
resolve differently than what the build pins). Images are scanned from the local
docker cache, so `--images` only produces results on the build machine where the
images exist; absent images show an explicit **not built** row. syft and grype
are **pinned** (`SYFT_PIN` / `GRYPE_PIN`). Any committed `sbom/*.vex.json`
suppressions are applied in both modes, so the report reflects the same triaged
reality as the gate.

## The ratchet

A raw scan surfaces a backlog, much of it not-applicable (vulnerable code not on
an execute path, dev/test-only deps). Failing on all of it immediately would
wall every PR. So each surface lands report-only, is triaged, then ratchets to
blocking:

1. Land report-only; the severity table appears on each run's Summary tab.
2. Triage findings into `sbom/*.vex.json` — `not_affected` + justification, or
   bump the real ones. A `not_affected` statement for a finding that has no
   fix yet should be time-boxed: put an `Expires: YYYY-MM-DD` marker at the
   start of its `impact_statement` (OpenVEX has no expiry field), and
   `tools/ci/validate-vex.py` — run before every grype invocation — fails the
   file once that date has passed, so the suppression gets re-evaluated
   instead of quietly outliving its evidence.
3. Once a surface is clean at medium+, add it to the blocking gate's
   `--surfaces` in the per-PR blocking job.

**Status:** **npm + cargo + images are now BLOCKING** at medium+.
- **npm + cargo** gate per-PR/push (the per-PR blocking job, pinned DB) and
  are clean at medium+ (only sub-threshold Low findings remain). A future
  medium+ CVE in those committed lockfiles fails the PR. `vitest` was bumped
  3.2.4 → 4.x to clear `CVE-2026-47429` (GHSA-5xrq-8626-4rwp, Critical) so npm
  is clean for the gate.
- **images** gate on the **scheduled / dispatch** dependency-scan run (the
  dedicated runner where the built `:latest` images live); a medium+ image CVE
  reddens that run and opens the `dependencies-cve` tracking issue. They are not
  gated per-PR because PRs never rebuild images.
- **.NET stays report-only** (when added) until triaged clean.

Every run on the default branch (push / schedule / dispatch on `main`) also
files-or-closes a single `dependencies-cve` tracking issue on medium+ findings,
so a newly disclosed CVE notifies immediately even on the non-blocking push
sweep. A pull_request run is a **dry run** of that step: it prints what the
live run would do (create / update / close / nothing, with the issue number)
to the log and the step summary and touches nothing — a PR's scan, clean only
because of a suppression the PR carries, must not close the tracker before it
   merges.

### Retiring the curated `generate-sbom.py` rows — deliberately deferred

The end-state is to retire the hand-maintained `JS_DEPS`/`RUST_DEPS` rows
in `tools/generate-sbom.py` for surfaces now covered by ground-truth scanning.
We verified the ground-truth scan **strictly supersedes** them for CVE-gating:
all 13 `JS_DEPS` are in the workbench `pnpm-lock.yaml` and all 34 `RUST_DEPS` are
in the vtracer `Cargo.lock` (which carries 50 crates total — 16 more than the
curated list). So the CVE-coverage redundancy is resolved by this PR's blocking
gate.

We **did not** delete those rows here, on purpose: `sbom/mod_pagespeed-2.1.spdx.json`
is the **published, license-annotated** SBOM (SPDX license expressions for
attestation / security questionnaires), and the ground-truth `dep-scan.sh` SBOMs
are gitignored (`sbom/scan/`), not published. Removing the rows would shrink the
customer SBOM and drop license provenance for those packages without a committed
replacement. Migrating the **published** SBOM to a ground-truth generator is the
larger "no drift" step — tracked below.

## Roadmap

- **Done (this PR):** npm + cargo ratcheted to **blocking** at medium+ with a
  pinned grype DB; `anchore/scan-action` SHA-pinned.
- **Next:** scan the shipped **`.nupkg`** for the .NET surface (report-only, then
  ratchet); port this scan to **`mod_pagespeed`** (whose dominant surface
  is the vendored Envoy C++ tree).
- **Then:** the Envoy-style C/C++ matcher (per-dep `cpe`+`release_date`, cached
  NVD feed, `validate.py` completeness gate) in `mod_pagespeed`.
- **Then:** migrate the **published** `generate-sbom.py` SBOM off its hand-curated
  `JS_DEPS`/`RUST_DEPS` rows onto a ground-truth generator (the load-bearing "no
  drift" step — see "Retiring the curated rows" above for why it's deferred).
