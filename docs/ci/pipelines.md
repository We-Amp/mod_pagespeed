<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# CI pipelines

This repository's CI is split into thin **caller** workflows and
**reusable** workflows. Jobs that need a lot of CPU run on a pool of build
runners that are reachable only through this repository's reusable workflows
at `refs/heads/master` (and `refs/heads/ci-dev`). A pull request therefore
cannot put its own workflow code on one of those runners: whatever it
changes in `.github/workflows/`, the jobs that reach them are the ones
already on `master`.

The pipeline is being moved over in steps. This document describes the
shape and the tooling; the sections on tiers and required checks say which
jobs exist today.

## The shape

| Kind | Triggers | Holds | Example |
|---|---|---|---|
| Caller | `pull_request`, `push`, `schedule`, `workflow_dispatch` | triggers, `concurrency`, `permissions`, jobs that only `uses:` a reusable workflow `@master` or `@ci-dev`, and at most an aggregate gate job on a hosted label | `ci.yml`, `ci-dev.yml`, `sweep-queued.yml` |
| Reusable | `workflow_call` only | every job, with its `runs-on` | `pipeline-pr.yml`, `pipeline-sweep.yml` |

Rules that follow from the shape:

- Concurrency groups live in the callers. A group inside a called workflow
  evaluates the caller's context and can collide or deadlock.
- A caller never names a build-runner label. The aggregate gate job
  (`CI gate`) runs inline because it reads `needs.*.result`, which a called
  workflow cannot see; it is pinned to a hosted label.
- Reusable workflows declare `permissions: contents: read`. A job that needs
  more asks for it on the job, and anything that writes to GitHub (issues,
  releases, comments) runs on a hosted runner fed by an artifact, never on a
  build runner (see "Report jobs" below). A called workflow can narrow the
  permissions its caller grants but never widen them, so a caller whose
  workflow has a report job grants `issues: write` on the job that calls it.
- No secret and no write token is ever available to a job that can run on a
  build runner.
- Jobs pass files to each other with `actions/upload-artifact` and
  `actions/download-artifact`, or `actions/cache`, never through a path on a
  particular machine. A runner-local fast path is allowed only through the
  runner-environment contract (`docs/ci/runner-environment.md`).

## Runner routing

A job that is meant for the build runners chooses its runner from the event,
never from inputs. A pull request from a fork, or from Dependabot, runs on a
GitHub-hosted runner; everything else runs on a runner class label. The
expression, copied verbatim into every such job:

```yaml
runs-on: ${{ (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) && 'ubuntu-latest' || 'fleet-heavy' }}
```

The class is one of `fleet-heavy`, `fleet-light`, `fleet-windows`,
`fleet-iis`. Windows jobs use `windows-2022` as the hosted side. A job
that needs a full IIS installation or a Hyper-V guest has no hosted side
and uses the "only" guard below.

### The guard and the kill switch

Every job routed by that expression also carries this guard, verbatim, as
its `if:`:

```yaml
if: ${{ vars.FLEET_ENABLED == 'true' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}
```

`FLEET_ENABLED` is a repository variable. While it is unset (or anything but
`true`), these jobs are skipped for same-repository events and still run on
hosted runners for fork and Dependabot pull requests, which need no build
runner. Setting it to `true` turns the build-runner legs on; clearing it is
the kill switch that takes every such job off the build runners at once, with
no workflow change.

Two more guards exist for jobs that cannot fall back to a hosted runner, and
the gate accepts exactly these three. **Only** is for a job that mounts the
container socket, runs privileged containers or needs a real Linux or Windows
host: a fork or Dependabot pull request skips it.

```yaml
if: ${{ vars.FLEET_ENABLED == 'true' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}
```

**Dispatch only** is for a manual lane; a dispatch is never a fork pull
request.

```yaml
if: ${{ github.event_name == 'workflow_dispatch' && vars.FLEET_ENABLED == 'true' }}
```

A job that needs more conditions (for example a path filter) appends them
together with a matching gate rule; the gate rejects any routed job whose
`if:` is not exactly one of the three guards, or the first one narrowed by
`&& needs.changes.outputs.<name> == 'true'`. A job that runs on a hosted label
only (hosted arm64, report jobs, the checks that need nothing special)
carries no guard.

### Degraded mode on hosted runners

A job routed by the expression must also work on a hosted runner: no hub, no
remote cache, no host paths. Anything host-specific is read from the runner
environment (`docs/ci/runner-environment.md`) and used only when set. When
none of it is set the job runs slower, and never fails for that reason.

## Artifacts

Jobs pass files through `actions/upload-artifact` and
`actions/download-artifact`. Artifact names are unique within a workflow run,
and the caller runs several tiers in one run, so each tier prefixes its
names: the PR tier uses none, the full tier `full-`, the nightly tier
`nightly-`. A build bundle is the directory `.ci-artifacts` (binaries,
`metadata.json`, `checksums.sha256`); a consumer downloads it to a directory
and hands the parent to `tools/ci/stage-artifacts.sh`, which validates it
(`tools/ci/validate-local-bundle.sh`). The vendored dependency tarball is
published with `tools/ci/publish-vendor-tarball.sh` and fetched with
`tools/ci/extract-vendor-tarball.sh`, which verifies it and fails loudly with
the `[vendor-tarball-unavailable]` marker when it is gone. Retention is three
days for bundles and evidence.

## Report jobs

A job that files or closes an issue never runs on the build runners. The
build job uploads a small artifact (the findings text, plus a `lane-ran`
marker file that says the lane reached a verdict) and a hosted `report` job
with `issues: write` downloads it and posts the result. Without the marker,
or without the artifact, the report job leaves the issue alone, so a failure
before the lane ran cannot close a live issue. Only a run on the default
branch changes the issue; a branch dispatch is a dry run. The gate enforces
the split (rule f): every reusable workflow declares a top-level
`permissions:` block with no write access, and a write permission (quoted or
not) is allowed only on a job with a hosted `runs-on`. Report jobs check out
`master` for the tools they run, because they hold a write token.

## The release ref guard

Release pipelines start with the `ref-guard` action
(`.github/actions/ref-guard`, logic in `tools/ci/ref_guard.sh`). It refuses
any ref but `master` and release tags whose commit is on `master`, and any
event outside the allowed list; a manually dispatched release may only name a
tag that already exists and is reachable from `master`. The allowed tag glob
is a parameter. The action runs after a checkout of `master` with full
history and tags, so the guard code is never the tree being released.
`tools/ci/test_ref_guard.sh` is its self-test.

## The queued-run sweep

A job whose runner label nothing serves (a retired label, or a runner class
that is not registered while `FLEET_ENABLED` is on) queues for days instead
of failing. With `FLEET_ENABLED` off the guard skips the job, so nothing
queues. `sweep-queued.yml` runs hourly on a hosted runner and calls
`pipeline-sweep.yml`, which runs `tools/ci/sweep-queued-runs.sh`: it lists
runs in `queued` or `waiting` status whose latest attempt started more than
30 minutes ago (`run_started_at`, so a re-run of an old run is not stale; the
script falls back to `created_at` only when that field is empty), keeps any
run with a job that has a runner and any run of a workflow on the skip list
(`Release` by default, because a run that waits for a manual approval looks
the same), and cancels the rest with one log line each. `--dry-run` only logs.

The threshold is a repository variable: set `SWEEP_MAX_AGE_MINUTES` (a whole
number of minutes) to override the 30 minute default. The logic is tested
against a fake `gh` by `tools/ci/test_sweep_queued_runs.sh`, which the sweep
job runs first.

## Trying a pipeline change on the build runners: `ci-dev`

A change to a reusable workflow does not reach the build runners from its own
pull request (the callers pin `@master`). To exercise it before merge:

1. Push the change to the `ci-dev` branch:
   `git push --force origin HEAD:ci-dev`.
2. Dispatch the `CI (ci-dev)` workflow (`ci-dev.yml`) from the Actions tab or
   with `gh workflow run ci-dev.yml`. It calls `pipeline-pr.yml@ci-dev`.
3. When it is green, merge the pull request as usual.

The `ci-dev` branch is shared and disposable; whoever pushes it last owns the
next dispatch.

## The workflow-shape gate

`tools/ci/check-workflow-shape.sh` runs in the `Public-tree hygiene gate` job
and fails the build when the shape breaks:

- **(a)** no workflow uses `pull_request_target`;
- **(b)** a workflow with any trigger other than `workflow_call` is a caller:
  its jobs only `uses:`
  `We-Amp/mod_pagespeed/.github/workflows/<file>.yml@master` or `@ci-dev`,
  with no `runs-on` or `steps`, except an aggregate or hosted-only job on a
  hosted label. Files listed in `tools/ci/workflow-shape-allowlist.txt` (with
  a reason each) are exempt from this rule only;
- **(c)** no local reusable-workflow reference (`uses: ./.github/workflows/...`);
- **(d)** in a reusable workflow every `runs-on` is a hosted label
  (`ubuntu-latest`, `ubuntu-24.04`, `ubuntu-24.04-arm`, `windows-2022`,
  `macos-15`, `macos-latest`) or exactly the routing expression with a
  runner class, and a job routed by the expression carries one of the three
  guards verbatim (the first one optionally narrowed by a `changes` path
  filter);
- **(f)** a reusable workflow has a top-level `permissions:` block with no
  write access, and a `write` permission at job level is allowed only on a
  job with a hosted `runs-on`;
- **(e)** `.github/` and `tools/ci/` carry no LAN address literals, no
  `.home` names, no `:9090`, no `self-hosted` and none of the retired runner
  label words (the bare word `pool` is only checked under `.github/`, because
  IIS application pools are named in the Windows scripts).

The gate is line-oriented: `runs-on` and the guard must each fit on one line.
Fixtures live in `tools/ci/testdata/workflow-shape/`; run
`tools/ci/test_check_workflow_shape.sh` after changing the gate.

## Runner group policy entries

The runner group that serves the build runners admits only the workflows
listed in its "selected workflows" policy; a job from any other workflow file
or ref queues and never starts. One entry per reusable workflow that can
reach the build runners, added in the change that gives the workflow its
first such job:

```text
We-Amp/mod_pagespeed/.github/workflows/pipeline-pr.yml@refs/heads/master
We-Amp/mod_pagespeed/.github/workflows/pipeline-pr.yml@refs/heads/ci-dev
```

Callers (`ci.yml`, `ci-dev.yml`, `sweep-queued.yml`) never get an entry.
