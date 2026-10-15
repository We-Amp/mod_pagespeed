<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# Module release lane

The release of the module packages (Apache, nginx and IIS modules, with the
optimizer packages they are released with) runs as one reusable workflow,
`pipeline-release-module.yml`, called by `release.yml`. This page describes
the lane, how to try it without releasing anything (a dry run), and the
contract of the files it produces. How the lane fits the other pipelines is in
`docs/ci/pipelines.md`; the runner environment is in
`docs/ci/runner-environment.md`.

The lane is built in two parts.

- **Part A (this page's subject)**: build every package, run the package
  smokes, the security gates, the unit tests and the system-test gate, and end
  in one run artifact with the asset set, `SHA256SUMS` and `manifest.json`.
  Part A signs nothing, creates no release, publishes nothing and uses no
  secret.
- **Part B (not present yet)**: in jobs of the protected `release`
  environment on hosted runners, the signing of `SHA256SUMS`, the Windows
  binaries and the Windows installer, and the creation of the release. The
  places where it plugs in are marked `PART B` in the workflow.

## Tags and versions

The lane releases a tag `module-v<version>`:

| Tag | Meaning |
|---|---|
| `module-v1.19.0` | a release |
| `module-v1.19.0-rc.1` | a release candidate: built and validated, flagged `prerelease: true` in the manifest, and never to be taken up by a consumer |

The version has the form `X.Y.Z` with an optional `-prerelease`. Build
metadata (`+r1` and similar) is refused: a new build of the same sources gets
a new tag. The tags of the product line (`v2.4.0`) and of the optimizer
(`optimizer-v1.19.0`) never match `module-v*`, so a tag selects exactly one
lane.

The module version comes from the tag alone. The tree carries the product line
version in `net/instaweb/public/VERSION`; the `vendor` job stamps
`MAJOR`, `MINOR`, `BUILD`, `PRERELEASE` and `LASTCHANGE=1` (the package
revision) into the copy every build starts from, with
`tools/ci/release_version.sh`. The packages therefore carry `1.19.0` (or
`1.19.0~rc.1`, the dpkg and rpm spelling of a pre-release) while the tree says
`2.4.0`. The pinned optimizer release (`install/debian/optimizer-pin.json`)
must be `optimizer-v` plus the same version, and its cache library must be the
one this tree links: the `optimizer-fetch` job refuses a pin that is not.

The tag must exist, and its commit must be reachable from `master`. The ref
guard (`.github/actions/ref-guard`) refuses any other ref and any event but a
tag push or a manual dispatch, for a dry run as well.

## Running it

A push of a tag matching `module-v*` runs the lane with `dry_run` false. A
manual dispatch takes the tag and `dry_run` (default true):

```text
gh workflow run release.yml -f tag=module-v1.19.0 -f dry_run=true
gh run list --workflow release.yml --limit 1
gh run download <run id> -n release-assets -D release-assets
```

In part A the two differ only in the `dry_run` output of the `version` job,
which part B reads: with `dry_run` true nothing is ever created. A dry run
builds and validates everything, so it is the way to find out whether a tag
will go through. The `system-test gate` reports its verdict and lets a dry run
continue; on a real run a red verdict stops the release.

Runs of one tag are serialized by the caller (`release.yml`), and a release
always runs out: it is never cancelled by a newer run.

### What a run needs from the repository settings

- `FLEET_ENABLED` (a repository variable): `true` sends the build jobs to the
  runner classes, anything else sends them to hosted runners (see "Runners").
- The runner group policy lists
  `We-Amp/mod_pagespeed/.github/workflows/pipeline-release-module.yml@refs/heads/master`
  (`docs/ci/pipelines.md`, "Runner group policy entries"). There is no entry
  for `ci-dev`: a release only runs from `master`.
- Part B only: the `release` environment (required reviewer, tag policy for
  `module-v*`) and its secrets. Part A refers to none of them.

## Jobs

All jobs have `permissions: contents: read` (the system-test gate and the
optimizer fetch additionally read Actions data and use the job's own
read-only token); none holds a secret. "Pair" means the job exists as a build
runner job and as a `-hosted` twin; exactly one of the two runs, and a small
hosted `-ready` job joins them for the jobs that depend on the pair.

| Job | Class | What it does |
|---|---|---|
| `ref-guard` | hosted | enforces the ref guard (tag glob `module-v*`) |
| `version` | hosted | derives version, package version, pin tag and `dry_run` from the tag |
| `vendor` | fleet-heavy, pair | vendors dependencies, stamps the module version, rebuilds the admin console with it, makes the `tree` artifact and the source tarball |
| `security-gates` | hosted | SBOM at the module version, notices, VEX, grype with VEX applied; makes the versioned SBOM and VEX files |
| `optimizer-fetch` | hosted | fetches the pinned optimizer release and verifies it (see below) |
| `systest-gate` | hosted | requires a green Apache and Nginx system test of the CI run of the release commit |
| `linux-release-build` | fleet-heavy, pair | x64: Apache module, deb, rpm, nginx module tarball, debug files, old-version packages |
| `linux-arm64-release-build` | hosted arm64 | the same for arm64 |
| `nginx-distro-build` | fleet-heavy, pair | x64 nginx module packages for noble, bookworm, trixie, jammy (deb) and el9 (rpm), with symbol-leak and rpm routing gates |
| `nginx-distro-build-arm64` | hosted arm64 | the same for arm64 |
| `nginx-distro-build-el10` | fleet-heavy, pair | the el10 nginx module rpm, with the gates and a load check into the stock nginx |
| `apache-el8-build` | fleet-heavy, pair | the stock Apache rpm built on el8 (glibc 2.28); input of the cPanel package only |
| `apache-el10-build` | fleet-heavy, pair | the stock Apache rpm built natively on el10 |
| `windows-release-build` | fleet-windows, pair | the IIS module built optimized with debug symbols, plus the pinned optimizer worker, all unsigned; hands over `unsigned-windows` |
| `unit-tests` | fleet-heavy, pair | unit tests of the optimized build with redis and memcached containers |
| `package-smoke-deb`, `package-smoke-rpm` | fleet-heavy, pair | install, upgrade over a synthetic old version, version header, removal, in a Debian and a Rocky container |
| `package-smoke-nginx` | fleet-heavy, pair | the from-source nginx smoke with the module tarball |
| `package-load-check-nginx` | fleet-heavy, pair | loads each x64 package into the stock packaged nginx of its distro |
| `package-load-check-nginx-arm64` | hosted arm64 | the same for arm64 |
| `package-smoke-nginx-packaged` | fleet-heavy, pair | installs each x64 package into its stock nginx and runs the nginx test suite |
| `ea4-build`, `ea4-el10-build` | fleet-heavy, pair | the cPanel (EasyApache 4) packages for el9, el8 and el10, unsigned |
| `vendor-ready`, `linux-ready`, `nginx-ready`, `el8-ready`, `el10-ready` | hosted | join the two legs of a pair |
| `validate` | hosted | builds and checks the asset set, writes `SHA256SUMS` and `manifest.json`, uploads `release-assets` |

Left out of part A on purpose: the Windows installer and its tests (they need
signed binaries and a virtual machine host), the cPanel smoke (a virtual
machine host), the signing of every kind, and the release itself. The
publication of apt and yum repositories does not exist in this repository: the
consumer pulls the release assets (see below).

### Fetched from outside the repository

All of it is public and read only, and needs no credential:

- the canonical product facts file of the optimizer repository, at vendoring
  time, because the admin console build checks its synced copy against it;
- the pinned optimizer release: the commit named in the pin file (its
  `MODULE.bazel`, for the cache library check), and the release assets of the
  pinned tag. The hash of the release's `SHA256SUMS` must equal the pin, and
  every file must match `SHA256SUMS`. The API call carries the job's own
  read-only token, to avoid the rate limit of shared runner addresses; the
  downloads carry none;
- the dependency sources named in the Bazel files.

### The system-test gate

The gate looks for a completed run of `ci.yml` for the release commit in which
the jobs named `Apache System Tests` and `Nginx System Tests` (under any caller
prefix) finished with `success`. A release commit that never ran those lanes
cannot be released: run the CI on it first. Until the pipelines of this
repository contain both lanes the gate cannot pass, so a real run refuses and
a dry run reports it.

### Runners

With `FLEET_ENABLED` true the build jobs run on the runner classes
(`fleet-heavy`, `fleet-windows`); otherwise the `-hosted` twins run the same
steps on GitHub-hosted runners, so the kill switch never blocks a release. The
fleet jobs use the facilities of the runner environment when set and run
without them otherwise. A hosted twin has no remote cache and a small machine
(four cores, 16 GB of memory, limited disk): it is much slower, and the builds
that are the longest on a build runner (the Apache modules and the distro
legs, which are cold builds) are not measured there yet. Their timeouts are
set to the hosted maximum for that reason. Measure with `FLEET_ENABLED` unset
before relying on a hosted release.
The queued-run sweep skips runs of the `Release` workflow (a real run may wait
for an approval), so a release run whose runner class is not served waits until
it is cancelled: set `FLEET_ENABLED` to anything but `true` and run it again. The nginx distro legs of a small runner
run one after the other (`tools/ci/release_nginx.sh` derives the concurrency
from the cores and memory it finds).

## Artifacts

Run artifacts of the lane, as kept for a few days (the file artifacts and
`release-assets` longer):

| Artifact | Content | A release file |
|---|---|---|
| `release-assets` | the validated asset set, `SHA256SUMS`, `manifest.json` | the result |
| `asset-source` | `mod_pagespeed-<version>.tar.gz` | yes |
| `asset-linux-x64`, `asset-linux-arm64` | Apache deb and rpm, `libmod_pagespeed-linux-<arch>.so`, the nginx module tarball | yes |
| `asset-nginx-x64`, `asset-nginx-arm64`, `asset-nginx-el10` | nginx module debs and rpms | yes |
| `asset-apache-el10` | the stock Apache rpm built on el10 | yes |
| `asset-ea4-el9`, `asset-ea4-el8`, `asset-ea4-el10` | unsigned cPanel rpms (el10 and the others as built) | yes |
| `asset-optimizer` | the verified optimizer deb, rpm and Windows zip | yes |
| `asset-sbom` | the versioned SPDX SBOM and VEX file | yes |
| `tree` | the prepared source tree every build starts from | no |
| `stock-el8` | the stock Apache rpm built on el8 | no |
| `testdata-oldpkgs-x64` | synthetic old-version packages for the upgrade smoke | no |
| `debug-linux-x64`, `debug-linux-arm64` | unstripped module and debug symbols | no |
| `unsigned-windows` | the unsigned IIS module, its symbols and the optimizer worker | no |

`validate` builds the asset set from the `asset-*` artifacts only. A directory
it does not know, an empty file, a signature file or a name outside
`[A-Za-z0-9._+-]` stops it, and so does a missing or ambiguous file of the
required set (every package kind, suite and architecture above, the optimizer
pair, the source tarball, the SBOM and the VEX file; the cPanel packages of el9
and el8 on a release, none on a pre-release).

## The asset contract

This is what a consumer of the release (the process that publishes the
packages to the apt and yum repositories) relies on. Part A produces these
files in `release-assets`; part B signs them and attaches them to the release
with the same names.

### Asset names

GitHub replaces a `~` in an uploaded file name with `.`. The packages of a
pre-release and the nginx debs (`...-r1~bookworm_amd64.deb`) have a `~` in
their names, so the lane renames them before upload, and `SHA256SUMS` lists
the uploaded names. `sha256sum -c SHA256SUMS` therefore works on the downloaded
assets as they are, and the original names (which the repository tooling keys
on) are recovered from the manifest. One more rename exists: the stock Apache
rpm of el10 has no dist tag in its name and would have the same name as the
el9 one, so its asset name carries the prefix `el10-`.

### SHA256SUMS

Standard `sha256sum` output (`<hash>  <name>`), one row per file of the
release except itself, in C-locale order, with the uploaded names. It lists
`manifest.json`. Part B adds a detached signature, `SHA256SUMS.asc`, so the
signature covers every file through the sums.

### manifest.json

```json
{
  "schema": 1,
  "tag": "module-v1.19.0",
  "version": "1.19.0",
  "pkg_version": "1.19.0",
  "prerelease": false,
  "source_sha": "<40 hex digits: the release commit>",
  "run_id": "<workflow run id>",
  "repository": "We-Amp/mod_pagespeed",
  "optimizer_tag": "optimizer-v1.19.0",
  "files": [
    {
      "asset": "nginx-module-pagespeed_1.19.0-r1.bookworm_amd64.deb",
      "name": "nginx-module-pagespeed_1.19.0-r1~bookworm_amd64.deb",
      "sha256": "<64 hex digits>",
      "size": 123456,
      "kind": "deb",
      "component": "module",
      "channel": "apt",
      "channel_also": [],
      "distro": "bookworm",
      "arch": "x64"
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `tag`, `version` | the released tag and its version (`1.19.0-rc.1` for a candidate) |
| `pkg_version` | the version as dpkg and rpm spell it (`1.19.0~rc.1`) |
| `prerelease` | true for a release candidate; a consumer must not take one up |
| `source_sha`, `run_id`, `repository` | provenance: the commit, the workflow run and the repository |
| `optimizer_tag` | the optimizer release the packages are paired with |
| `asset` | the uploaded name; what `SHA256SUMS` lists |
| `name` | the original name, with any `~` kept; the name to give the file when it is put in a repository tree |
| `kind` | `deb`, `rpm`, `tarball`, `so`, `zip` or `json` |
| `component` | `module`, `optimizer`, `source` or `sbom` |
| `channel` | the routing hint for repository publication: `apt`, `rpm-el9`, `rpm-el10`, `rpm-ea4`, or `none` (a direct download only) |
| `channel_also` | further channels the same file belongs to: the optimizer rpms are also in `rpm-el10` |
| `distro` | a deb suite (`noble`, `bookworm`, `trixie`, `jammy`), an rpm family (`el8`, `el9`, `el10`) or `any` |
| `arch` | `x64`, `arm64` or `any` |

`channel` values: debs are `apt` and the suite is in `distro` (also in the
`~suite` of the name). Rpms of el9 (the stock Apache rpm, the nginx rpms, the
optimizer rpms) are `rpm-el9`. The stock Apache rpm of el10 (dist-tagless) and
the nginx rpm of el10 are `rpm-el10` and belong in a tree of their own; the
optimizer rpms are `rpm-el9` with `rpm-el10` in `channel_also`. The cPanel
rpms of all families are `rpm-ea4`. The `.so` files, tarballs, the Windows zip
and the SBOM and VEX files are `none`.

A consumer should: check the tag grammar and `prerelease`; check that the
release is not a draft and that its commit is on `master`; download every
asset; run `sha256sum -c SHA256SUMS`; verify the `SHA256SUMS.asc` signature
against a pinned key (part B); compare the set of package assets with the
manifest and refuse extra or missing files; rename each file from `asset` to
`name` as it builds its trees; and check the pair (every module package needs
the optimizer packages of `optimizer_tag` beside it).

## Reading a failed run

- `validate` names every missing or ambiguous file of the set. A new package
  kind needs a line in `tools/ci/release_manifest.py` (the artifact table and
  the required set) in the same change that adds the job.
- `validate` also checks the version of every package against the tag and
  that the Apache packages require the optimizer at exactly the pair version.
- A job that fails on a runner class is re-run alone with "Re-run failed
  jobs"; the `tree` artifact is kept for three days. After that, run the
  whole workflow again.
- The cPanel jobs fail on a release when the stock rpm's name does not match
  the spec: `%global mps_version` in `packaging/cpanel` must be bumped with the
  module version.

## Tooling

| File | Purpose |
|---|---|
| `tools/ci/release_version.sh` | tag grammar, derived versions, VERSION stamping (`--self-test`) |
| `tools/ci/release_manifest.py` | asset set, `SHA256SUMS`, `manifest.json`, verification (`--self-test`) |
| `tools/ci/release_nginx.sh` | the nginx distro legs and their gates (`--self-test`) |
| `tools/ci/release_ea4_build.sh` | the cPanel package from the stock rpm (`--self-test`) |
| `tools/ci/ref_guard.sh`, `tools/ci/test_ref_guard.sh` | the ref guard and its test |

The self-tests run in the `CI tooling self-tests` job of `ci.yml`.
