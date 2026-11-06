<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# Lane write sets: CI and the release lane on one commit

The CI run of a commit (`ci.yml`, including a `full=true` dispatch, which
calls `pipeline-pr.yml` and `pipeline-full.yml`), the nightly
(`nightly.yml` -> `pipeline-nightly.yml`) and the module release lane
(`release.yml` -> `pipeline-release-module.yml`, with the gates
`gate-iis-msi-upgrade.yml` and `gate-cpanel-smoke.yml`) share the build
runners. This page lists what each lane writes on a build runner outside the
job's own lifetime, and answers one question: **can a CI run and a release-lane
run of the same commit run at the same time without either one picking up
state the other left?**

The answer is yes, with the two changes listed under "Overlaps and fixes".
`tools/ci/test_lane_write_sets.sh` replays both interleavings with the real
workflow steps; it fails without the changes and passes with them.

Line numbers below refer to the tree this page was added in. Runner instance
means one registered runner (one `RUNNER_NAME`, one `_work` directory); a host
can carry several runner instances under one user account, and a runner
instance runs one job at a time.

## What each lane writes

### Per runner instance (never shared by two jobs at once)

| Path | Release lane | CI and nightly | Notes |
|---|---|---|---|
| `GITHUB_WORKSPACE` (`_work/mod_pagespeed/mod_pagespeed`) | the release tree: `GIT_COMMIT` of the release commit, a version-stamped `VERSION`, `vendor/` (`pipeline-release-module.yml:358`, `:823`) | the CI tree, after `actions/checkout` and the hub extract (`pipeline-pr.yml:1243`, `:1266`) | the **same directory** for both lanes on one runner instance, because both workflows belong to this repository. See overlap 1. |
| `${GITHUB_WORKSPACE}-leg-<distro>` | one copy of the tree per nginx leg (`tools/ci/release_nginx.sh:239`) | not used | the release lane re-owns and clears them (`&reown`) |
| `RUNNER_TEMP` | tree download, packages, `tree-boot-<run>-<attempt>` on Windows (`pipeline-release-module.yml:2692`), keyrings (`mktemp -d`) | vendor temp tarball, bundles, shared-cache legs (`pipeline-nightly.yml:933`) | emptied by the runner for every job |
| `.ci-artifacts/` (in the workspace) | not written (no reference in the release workflow) | the build bundle (`pipeline-pr.yml:1302`) | travels between CI jobs as a run artifact, not on disk |
| `~/.cache/mps-bazel-<runner>` | not used | Bazel disk cache (`pipeline-pr.yml:1290`) | per runner instance |
| `~/.cache/mps-release-bazel-<runner>` | Bazel disk cache (`pipeline-release-module.yml:865`) | not used | per runner instance |
| `~/.cache/mps-envoy-repo-<runner>` | not used | nightly repository cache (`pipeline-nightly.yml:3509`) | per runner instance |
| Windows Bazel output base | under `WIN_BAZEL_OUTPUT_ROOT` (`pipeline-release-module.yml:2672`) | the same root (`pipeline-pr.yml:2526`) | Bazel derives the output base from the workspace path, so each runner instance has its own; the install base is shared and Bazel locks it |

### Shared by the runner instances of one host

| Path or object | Release lane | CI and nightly | Verdict |
|---|---|---|---|
| `~/.cache/mps-release-nginx`, `-nginx-el10`, `-<el>` | mounted at `/cache` into every nginx distro leg and the Apache EL builds (`pipeline-release-module.yml:1149`, `:1431`, `:1623`; `tools/ci/release_nginx.sh:284`) | not used (the nightly's local package build works in `RUNNER_TEMP`, `pipeline-nightly.yml:933`) | **shared per host user before this change.** See overlap 2. |
| CI vendor dir and hub (`CI_VENDOR_DIR`, `CI_HUB_*`) | not written (no reference in the release workflow) | the vendor tarball, create-if-absent (`pipeline-pr.yml:1128`, `:1193`) | disjoint |
| Windows disk cache (`CI_WIN_CACHE_DIR`) | `--disk_cache` (`pipeline-release-module.yml:2673`) | the same directory (`pipeline-pr.yml:2530`); the nightly uses `-asan` and `-appverif` suffixes | shared, content-addressed. CI jobs already share it at the same time today; the release lane adds no new kind of writer |
| Remote Bazel cache | read and write | read and write | shared, content-addressed |
| Dev image tags | `ensure-dev-image.sh --tag-latest` (`pipeline-release-module.yml:843`) | the same | content-hashed tag, per-host `flock` in `CI_DEV_IMAGE_LOCK_DIR` (`tools/ci/ensure-dev-image.sh:129`). Both lanes run the content tag their call printed; `:latest` is retagged by both (the same image for one commit, a different one across commits) and is not what either lane runs |
| `mps-smoke-deb-base`, `mps-smoke-apache-deb`, `mps-smoke-rpm-base`, `mps-smoke-apache-rpm`, `mps-smoke-nginx` | built and run by the package smokes under a run-scoped tag `<name>:<run>-<attempt>` (`pipeline-release-module.yml:1904`, `:2044`, `:2127`, `:2264`, `:2320`); `mps-smoke-nginx` is also tagged plain for the local image scan, which the lane never runs | not used | release only. The release concurrency group is per tag, so release runs of two tags (a dry run, a real tag) can share a host; the run-scoped tag keeps each run on the image it built |
| Containers, networks, volumes | named `ci-*-<job>-<run>-<attempt>` (`pipeline-release-module.yml:1729`) or unnamed `--rm` | named with the job, run id and attempt (`pipeline-pr.yml:1493`, `pipeline-nightly.yml:993`) | disjoint across runs; every `docker rm` names its own containers. No job publishes a host port or uses the host network |
| `/tmp` on the host | `mktemp -d` only; `/tmp/bazel-out`, `/tmp/pytest-smoke.out` and `/tmp/nginx_test` are inside containers | `mktemp`, the dev image lock files | disjoint |
| `fleet-hyperv` guests | installer upgrade and cPanel gates | not used | disjoint by runner class |
| `fleet-iis` | not used | IIS system tests | disjoint by runner class |

Hosted runners (`ubuntu-latest`, `ubuntu-24.04-arm`, `windows-2022`) start
empty for every job and are out of scope.

## Overlaps and fixes

### 1. A release tree with the CI commit stamp

The release lane stamps `GIT_COMMIT` with the release commit in a tree that
also carries the release `VERSION` and the lane's own `vendor/`, and leaves
that tree in `GITHUB_WORKSPACE`: the vendor job in its checkout, every build
job after it extracts the tree. On the same commit, a CI step that trusts a
warm workspace by its stamp would take that tree as its own.

The CI jobs of `pipeline-pr.yml`, `pipeline-full.yml` and
`pipeline-nightly.yml` do not trust it today: every job on a build runner
runs `actions/checkout` of the workspace root (clean, never `clean: false`)
before it reads the workspace, which removes untracked files and resets
tracked ones, then extracts the CI tarball and checks the stamp. `tools/ci/ensure-workspace.sh`,
the helper for a job that reuses a warm workspace, does trust it: it reused any
tree whose stamp matched.

Fix: the release lane writes a marker, `.release-lane-tree`, next to
`GIT_COMMIT`, and before it, so a job killed between the two writes never
leaves a stamped tree without the marker (`pipeline-release-module.yml:358`). The tree tarball takes it to
every job that extracts it, Linux and Windows. `ensure-workspace.sh` treats a
workspace that holds the marker as stale and re-extracts it from the CI hub.
A marker rather than a cleanup on exit: a cancelled job runs no cleanup. The
source tarball of the release is built from `git archive` and does not carry
the marker.

### 2. One nginx cache directory per host user

`~/.cache/mps-release-nginx` (and `-nginx-el10`, `-<el>`) had no runner
suffix, so every release job on one host mounted the same directory
read-write into its containers. The x64 nginx legs run as one job per distro, up
to three at once (`max-parallel: 3`), possibly on runner instances of one
host. Today the
build scripts never write it: the distro builds deliberately do not use a disk
cache (`install/nginx/build_module_in_container.sh:840`,
`install/rpm/build_apache_el8_in_container.sh:186`), so the overlap is latent.
It becomes real the moment a cache is used there.

Fix: the same per-runner suffix the Bazel cache directories already have
(`mps-release-nginx-<runner>`, `mps-release-nginx-el10-<runner>`,
`mps-release-<el>-<runner>`). `release_nginx.sh` keeps no state across legs or
jobs in that directory, so nothing needs it shared. The test evaluates every
`CACHE_ROOT` under `~/.cache` in every workflow for two runner instances of
one home, so a new shared one fails it; distinct directories are what keeps
concurrent legs on one host from writing into each other's cache.

## Proof

- `tools/ci/test_lane_write_sets.sh` (in `ci.yml`, CI tooling self-tests)
  runs the real stamp, tree tarball and tree extract steps of the release
  lane: the tarball carries the marker, a release vendor checkout and an
  extracted release tree of the same commit are re-extracted by the CI reuse
  step, a CI tree of that commit is still reused, and every host cache
  directory differs between two runner instances of one home.
- After merge, on the build runners: one release-lane job leaves its tree on a
  runner instance, then a CI reuse job of the same commit runs on that same
  instance and logs the re-extract; a release run shows the per-runner nginx
  cache directories.
