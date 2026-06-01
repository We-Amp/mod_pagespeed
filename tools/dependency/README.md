<!-- SPDX-License-Identifier: BUSL-1.1 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# Vendored C/C++ CVE matcher

This directory holds the CVE scanner for mod_pagespeed 1.1's **vendored
C/C++ Bazel dependencies** — the ~27 `http_archive` / `git_repository` deps
declared in [`bazel/repositories.bzl`](../../bazel/repositories.bzl).

It is the C/C++ half of [the design record](../../../corp/decisions/065-comprehensive-dependency-verification.md).
The npm / Docker-image / lockfile half is handled by the SBOM + grype + OpenVEX
gate (`tools/generate-sbom.py`, `tools/ci/validate-vex.py`, `sbom/`). **These are
separate surfaces** — grype meaningfully matches only the npm rows; it cannot match
source-built C/C++ Bazel deps (see below). This matcher fills that gap.

## Why grype/syft can't do this for vendored C/C++

A Bazel `http_archive` is a tarball pinned by SHA. There is no package manager,
no `Package-URL` ecosystem coordinate, and no CPE recorded anywhere in the build
graph. `syft dir:` finds **zero** C/C++ packages from source, and grype has
nothing to match a CVE against. Injecting *synthetic* CPEs into the SBOM and
letting grype match them is noisy and — critically — has **no per-dependency
release-date filter**, so it floods you with CVEs that were already fixed in the
version you pinned.

## The model (Envoy's `tools/dependency/`)

We adopt Envoy's approach (it is a large C++/Bazel project we already vendor):

1. **Annotate** every vendored dep with a **CPE** and a **release_date**
   ([`cpe-map.yaml`](cpe-map.yaml)). The CPE is the CVE-matchable identifier
   (`cpe:2.3:a:vendor:product`); deps with no applicable CPE are set to `N/A`
   **with a mandatory justification**. Versions are *not* duplicated here — they
   live as `*_VERSION` / `*_COMMIT` constants in `repositories.bzl` and are
   resolved at scan time, so version drift is structurally impossible.

2. **Match** ([`cve_scan.py`](cve_scan.py)): for each CPE, query the NVD 2.0 CVE
   feed by `virtualMatchString` (CPE with a wildcard version), then keep only
   CVEs **published after that dep's `release_date`** and **at/above a severity
   threshold** (default: CVSS ≥ 4.0, i.e. medium+), minus the ignore-list. The
   release-date filter is what makes the output actionable — it drops the CVEs
   that the pinned version already contains the fix for (e.g. 47 of 54 high+ curl
   CVEs are pre-8.20.0 and filtered out).

3. **Suppress** ([`cve-ignore.yaml`](cve-ignore.yaml)): a VEX-with-justification
   ignore-list. Every entry **requires** a `justification` — the scanner rejects
   entries without one. This is the durable record of every "not affected"
   decision for the C++ surface (the analog of `sbom/*.vex.json` for grype).

4. **Gate on completeness** ([`validate-deps.py`](validate-deps.py)): fails if any
   dep in `repositories.bzl` lacks a `cpe` + `release_date` (or an explicit
   `cpe: "N/A"` + justification). A new C++ dep therefore **cannot land
   unscanned**. This is the anti-drift guard, and it runs offline on every
   PR/push.

The dep enumeration and version resolution are **reused from
`tools/generate-sbom.py`** (its `CPP_DEPS` list + `parse_bzl_constants()`), so
the SBOM and this matcher can never disagree about which deps exist or what
version is pinned.

## The reproducible NVD snapshot

Hitting NVD live is slow and non-reproducible: unauthenticated callers are
rate-limited to ~5 requests / 30s, and the same CPE returns different CVE sets
day to day. That is fine for a daily cron, but it cannot run **per-PR** (too
slow) and it cannot be **reproducible** (a re-run drifts). So we keep a
committed, compact **NVD snapshot** that the per-PR job scans **offline in
seconds**:

> **[`nvd-snapshot.json`](nvd-snapshot.json)** — the only fields the matcher
> reads, per CPE: each CVE's `id`, `published`, `vulnStatus` (only when
> `Rejected`), the English description, and the single highest-priority CVSS
> base score + severity. ~300 KB for the 19 CPE'd deps / ~380 CVEs. Deterministic
> (sorted CPE keys, CVEs sorted by id, `sort_keys`, trailing newline) so the
> daily refresh produces a clean diff and a re-run reproduces byte-for-byte.

**Why a committed snapshot (vs `actions/cache` or GCS)?**

- **Reproducible by construction.** The data the PR scans is a git-tracked file.
  A PR scanned today and re-run tomorrow gives the *same* result unless the
  committed snapshot changed — and when it changes, the change is a reviewable
  diff. `actions/cache` keyed by date is restorable but its content is neither
  diffable nor durable (7-day LRU eviction breaks reproducibility for older PRs).
- **No extra auth / no network on the PR path.** GCS (Envoy-style) would need a
  service-account key that lives on one machine only, *not* on the CI runner
  runner that runs this workflow — so GCS was rejected. The snapshot needs
  nothing but `git checkout`.
- **Fast.** Local file read → the whole 19-CPE scan runs in ~0.05 s.

### How it refreshes

The daily `nvd-scan` job (08:00 UTC + manual dispatch) hits NVD live with
`--refresh`, rebuilds the snapshot with `--build-snapshot`, and **commits it back
to `master`**. Because the output is deterministic, an unchanged feed is a no-op
commit (skipped). It also still files/closes the findings issue and uploads the
report — exactly as before. So the committed snapshot is at most ~24 h stale,
and the per-PR job always scans current data.

### How the PR job uses it

The `pr-snapshot-scan` job runs on every PR/push touching the C++ dep surface:

```sh
tools/dependency/cve_scan.py --snapshot tools/dependency/nvd-snapshot.json
```

This is **offline, fast, reproducible, and report-only** today. The matching
logic is identical to a live run — the snapshot feeds the same
`scan()`/`cve_severity()`/`cve_published()` code path, so the per-PR findings
equal what a (slow) live scan would have produced from the same snapshot date.

## Files

| File | Role | When it runs |
|---|---|---|
| `cpe-map.yaml` | curated `name → {cpe, release_date, justification?}` | committed; reviewed like code |
| `cve_scan.py` | NVD matcher (fetch + cache + snapshot + date-filter + report) | per-PR (snapshot), **daily** (live refresh) |
| `nvd-snapshot.json` | committed compact NVD mirror (per-CPE minimal CVE records) | **committed**; refreshed daily, scanned per-PR |
| `cve-ignore.yaml` | VEX-style suppression list (justification mandatory) | read by `cve_scan.py` |
| `validate-deps.py` | completeness / anti-drift gate | **every PR/push** (fast, offline) |
| `.cve-cache/` | cached raw NVD responses + JSON report | gitignored, never committed |

## Usage

```sh
# Completeness gate (fast, offline) — fails if a dep is un-annotated.
tools/dependency/validate-deps.py

# Per-PR scan against the committed snapshot (offline, ~0.05 s, reproducible).
tools/dependency/cve_scan.py --snapshot tools/dependency/nvd-snapshot.json
tools/dependency/cve_scan.py --snapshot ... --fail-on high   # blocking variant

# Live CVE scan against NVD (caches raw responses under .cve-cache/).
tools/dependency/cve_scan.py                       # all deps, medium+
tools/dependency/cve_scan.py --severity high       # high+ only
tools/dependency/cve_scan.py --dep curl --dep grpc # subset
tools/dependency/cve_scan.py --offline             # .cve-cache only, no network
tools/dependency/cve_scan.py --refresh             # ignore cache, re-fetch

# Daily-cron refresh: live scan + rebuild the committed snapshot.
tools/dependency/cve_scan.py --refresh \
    --build-snapshot tools/dependency/nvd-snapshot.json
```

NVD rate-limits unauthenticated callers (~5 requests / 30s); the live scanner
caches per-CPE and sleeps ~7s between requests. Set `NVD_API_KEY` to raise the
limit. The scanner is **report-only by default** (exits 0); pass `--fail-on
<severity>` to make findings fatal. The completeness gate is always blocking.

## Report-only → ratchet (the flip-to-blocking step)

Per the design record, both NVD-touching paths land **report-only**: the matcher prints a
per-dep findings table and writes a JSON report, but never fails CI by default.

The per-PR `pr-snapshot-scan` job is structured so flipping it to **blocking is a
one-line change**: in
the CVE-scan CI lane,
set the `FAIL_ON` env var on that job (empty = report-only; `high` /
`critical` / `medium` = fail the PR on a finding at/above that severity). The job
already forwards a non-empty `FAIL_ON` to `cve_scan.py --fail-on`.

The intended ratchet: first triage the existing snapshot findings — bump the dep
where upstream has a fix, or record a justified suppression in `cve-ignore.yaml`
(or a `sbom/*.vex.json`) where not applicable — until the snapshot scan is clean
at the target severity, *then* set `FAIL_ON: high` so any **new** high+ CVE that
lands (via a daily snapshot refresh) blocks the next PR that touches the dep
surface. Because the snapshot is reproducible, a PR can never flap between pass
and fail without a reviewable snapshot/dep/ignore-list change.

## Honest limitations

- **Sub-deps a vendored library bundles internally are invisible.** We annotate
  what we *declare* in `repositories.bzl`. If, say, Envoy or gRPC statically
  bundles a third library that we never declare, that library's CVEs are not
  matched here. Backstops: the runtime Docker image / built `.deb`+`.rpm`
  scan (catches dynamically-linked `.so`s via grype's reliable OS-package path),
  syft's binary classifier (some statically-linked libs in the ELF), and the
  hand-maintained CVE comments in `repositories.bzl`.
- **A wrong CPE is a silent false-negative.** Where the NVD dictionary had no
  authoritative entry for the exact upstream project (e.g. Google `re2`,
  `gflags`, `sparsehash`, the zlib-ng *library*), we set `N/A` with a
  justification rather than assert a plausible-but-wrong CPE.
- **CPE-level matching is product-wide, not version-precise.** NVD's
  `virtualMatchString` returns all CVEs for the product; the release_date filter
  approximates version applicability but is not a substitute for reading each
  advisory's affected-version range. A finding is a *candidate* to triage, not a
  confirmed exposure. (Example: a gRPC-Go CVE surfaces under the `grpc:grpc` CPE
  even though we build gRPC C++ — exactly what the ignore-list/VEX is for.)
- **NVD CVSS coverage is incomplete.** A CVE with no CVSS score is not matched by
  the severity threshold; such CVEs are skipped (rare for the products here).
- **The per-PR snapshot is up to ~24 h stale.** The PR job scans the committed
  snapshot, not live NVD, so a CVE published *after* the last daily refresh is
  not seen on a PR until the next refresh commits. This is an intentional
  speed/reproducibility trade-off; the daily live `nvd-scan` (and its findings
  issue) is the always-current backstop. If the daily refresh stops committing
  (runner down for days), the snapshot silently ages — the `generated` field in
  `nvd-snapshot.json` records when it was last refreshed.
- **Snapshot integrity rides on branch protection, not signing.** Only the
  `nvd-scan` job (with `contents: write`) commits the snapshot to `master`; a
  poisoned snapshot would require write access to `master`, the same trust
  boundary as any other committed CI input. The snapshot only ever *suppresses*
  findings by omission — it can never inject a false dep version (versions come
  from `repositories.bzl`, not the snapshot). Re-running the daily live scan
  regenerates it deterministically and would surface any tampering as a diff.
