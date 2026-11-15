<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# CI pipelines

This repository's CI is split into thin **caller** workflows and
**reusable** workflows. The heavy jobs run on a fleet of build runners that
are reachable only through this repository's reusable workflows at
`refs/heads/main` (and `refs/heads/ci-dev`). A pull request therefore cannot
put its own workflow code on a fleet runner: whatever it changes in
`.github/workflows/`, the jobs that reach the fleet are the ones already on
`main`.

## The shape

| Kind | Triggers | Holds | Example |
|---|---|---|---|
| Caller | `pull_request`, `push`, `schedule`, `workflow_dispatch` | triggers, `concurrency`, `permissions`, jobs that only `uses:` a reusable workflow `@main` or `@ci-dev`, and at most an aggregate gate job on a hosted label | `ci.yml`, `nightly.yml`, `sweep-queued.yml`, `ci-dev.yml` |
| Reusable | `workflow_call` only | every job, with its `runs-on` | `pipeline-pr.yml`, `pipeline-full.yml`, `pipeline-nightly.yml`, `pipeline-sweep.yml` |

Rules that follow from the shape:

- Concurrency groups live in the callers. A group inside a called workflow
  evaluates the caller's context and can collide or deadlock.
- A caller never names a fleet label. The aggregate gate job (`CI gate`)
  runs inline because it reads `needs.*.result`, which a called workflow
  cannot see; it is pinned to a hosted label.
- Reusable workflows declare `permissions: contents: read`. A job that needs
  more asks for it on the job, and anything that writes to GitHub (issues,
  releases, comments) runs on a hosted runner fed by an artifact, never on a
  fleet runner (see "Report jobs" below). A called workflow can narrow the
  permissions its caller grants but never widen them, so a caller whose
  workflow has a report job grants `issues: write` on the job that calls it.
- Jobs pass files to each other with `actions/upload-artifact` and
  `actions/download-artifact`, or `actions/cache`, never through a path on a
  particular machine. A runner-local fast path is allowed only through the
  runner-environment contract (`docs/ci/runner-environment.md`).

## Tiers

| Tier | Workflow | Runs on | Holds |
|---|---|---|---|
| PR | `pipeline-pr.yml` | every pull request and push to `main` | the hosted checks (daemon build and unit-test slice, Apache RAT, license headers, SBOM drift, release-note gate and the chart, probe and entrypoint checks, the public-tree hygiene gate, the blocking and the report-only dependency scans, the ABI check, the JS/TS unit tests, the release packaging container), and the fleet legs: vendor, Linux build, Linux unit tests, lint (clang-format only on a push to `main`; clang-tidy scoped or skipped by the changed files on a pull request, whole tree on a dispatch), the JS and HTML kernel differentials, the release-config test compile, the Windows build and unit tests, the path-filtered CSS and JS fuzz smokes, the path-filtered ETag subset of the production-readiness rig (conditional and range request tests against the prebuilt images, from the Linux build bundle; the whole rig stays in the nightly), and the path-filtered Test::Nginx suite (`t/`, from the Linux build bundle). Later changes add the Linux arm64 build and tests |
| Full | `pipeline-full.yml`, called by `ci.yml` and `nightly.yml` (job `full` in both) | push to `main` (unless `FULL_TIER_TRIGGER` is `nightly`), `workflow_dispatch` with `full: true`, a pull request labelled `ci:full`, the scheduled nightly, or a nightly `workflow_dispatch` with `full_tier: true` | its own vendor and Linux x64 build; sanitizers (ASan, TSan); package smokes (Linux x64, Linux arm64, Windows, with a Windows build of the two shipped binaries to feed it); the daemon privilege rigs (three distros on the fleet, one on hosted arm64); HTTP compliance; the async CSS probe markup check; container cache sharing; the Linux arm64 build and unit tests; the whole-tree lint (`lint-full`, for `ci:full` pull requests, dispatches with `full: true` and the nightly, not on a push). See "When the full tier runs" |
| Nightly | `pipeline-nightly.yml`, called by `nightly.yml` (which also calls the full tier as `full`) | `nightly.yml`: daily at 04:17 UTC and on dispatch | the macOS build, unit tests and package smoke; the periodic vendor and optimized builds (x64 on the fleet, arm64 hosted); the stress, production, real-world and end-to-end lanes on both architectures; the Test::Nginx suite (`t/`, x64); the x64 privilege-rig soak; the rendered async CSS probe; CSS fuzzing (nightly, and a manual shadow lane); the nightly image leg of the dependency scan |
| Sweep | `pipeline-sweep.yml` | `sweep-queued.yml`: hourly and on dispatch | cancels runs that wait for a runner nobody serves (see "The queued-run sweep") |

The `ci:full` label is read when a pull request run starts (open, push,
reopen). After adding the label, push or re-run the workflow to get the
full tier.

The full tier's Linux build, ASan and TSan jobs take their Bazel flags from
the `ci-build`, `ci-asan` and `ci-tsan` configs in `.bazelrc`. Those configs
are the one definition CI and the bisect (`tools/ci/bisect-job.sh`) share.
Only the remote-cache and jobs choice (`BAZEL_CONFIG`),
`--nocache_test_results` and the target patterns stay on the job's command
line; docker flags stay in the workflow.

### When the full tier runs

Two callers name it `full`, so its checks read `full / <job name>` in both:

- `ci.yml`: a `workflow_dispatch` with `full: true` (every release
  candidate), a pull request labelled `ci:full`, and a push to `main`.
- `nightly.yml`: every scheduled nightly, and a nightly `workflow_dispatch`
  with `full_tier: true` (boolean, default `false`) on `main`. That dispatch
  is the make-up path for a missed or late night; a dispatch on any other
  ref never reaches it. It shares the `nightly` concurrency group, so it never
  starts while another nightly is pending or running. A release-candidate
  nightly dispatch leaves `full_tier` off, because the candidate's `CI`
  dispatch with `full: true` covers the full tier. The call grants
  `actions: read` and `contents: read`, the same as the push tier's call.

The repository variable `FULL_TIER_TRIGGER` decides the push: unset, or any
value other than `nightly`, runs the full tier on every push to `main`.
With `nightly`, a push runs it only when the pushed range changes the full
tier's own definition: `.github/workflows/pipeline-full.yml`, `nightly.yml`,
`pipeline-nightly.yml`, `ci.yml` (it holds this trigger, so a change to it
proves itself at once), anything under `.github/actions/`, every
`tools/ci/` file `pipeline-full.yml` names (as `tools/ci/<file>` or
`tools\ci\<file>`), and the `tools/ci/` files those files name in turn
(one level). The hosted `Full tier scope` job (`full-scope` in `ci.yml`)
decides it with `tools/ci/full_tier_scope.sh`, read from `main`, never
from the tree under test, and publishes `full_tier_change`. The range is
not the last commit: it starts at the head of the last push run of `CI` on
`main` in which `Full tier scope` succeeded (`tools/ci/push_range_base.sh
--require-job`, which reads the Actions runs and jobs API with
`actions: read`), because GitHub replaces pending push runs in a merge burst
and a full-tier change in a replaced run must still be seen. The full tier
is skipped on a push only when `full-scope` succeeded and printed `false`;
a failed or undecided scope job, no usable base or an API failure runs it.
`CI gate` accepts `full-scope` and `full` as skipped. Neither `Full tier
scope` nor any `full / ` job is a required check.

### Bisecting a red full-tier job

`tools/ci/bisect-job.sh` bisects a red `linux-asan`, `linux-tsan` (test
bisect) or `linux-build` (build bisect) on one Linux build host. Other
full-tier jobs are out of its scope. Per-repository values (job names,
container flags and environment, services, the vendor command) are in
`tools/ci/bisect-job-table.sh`; `tools/ci/test_bisect_job.sh` checks them
against `pipeline-full.yml`, so a workflow change that is not mirrored there
fails CI.

```bash
export BISECT_ROOT=/absolute/path/for/bisect/runs   # required, never defaulted
tools/ci/bisect-job.sh start --job linux-asan --bad <sha> --good <sha> \
  --run <run-id> --repo <clone url or path>         # prints <id>, returns at once
tools/ci/bisect-job.sh status <id>                  # state, last log lines, exit
tools/ci/bisect-job.sh resume <id>                  # continue an interrupted run
```

Host prerequisites: `docker`, `git`, `gh` (authenticated), `flock` and
`setsid` (util-linux), `nice` (and `ionice` where present), and `bazel`
(bazelisk) for vendoring.

`--targets-file <file>` (one label per line) replaces `--run`. `--cpus`,
`--memory-gb`, `--runs-per-test` and `--remote-cache-url` (read-only use)
are optional. The run writes `BISECT_ROOT/<id>/{state,log,result.json,exit}`.
`result.json` has `status` (`culprit`, `inconclusive`, `env-dependent`,
`external-drift-or-stale-good`, `no-targets`, `too-many-targets`),
`first_bad`, `skips`, `targets`, `timeout_only`, `rebuilt_images`, `steps`,
`bad`, `good` and `job`. `start` and `resume` exit 0 when launched, 2 when
refused, 3 when another bisect holds the lock.

How it decides:

- The targets come from the failed job's log (`gh run view --job <id>
  --log-failed`, or when that gives nothing the job logs API with `gh api
  --allow-escape-sequences`) through `tools/ci/failing_targets.sh`. No target,
  or more than 10 (`BISECT_MAX_TARGETS`), means no bisect.
- The bad endpoint must fail and the good one pass first (otherwise
  `env-dependent` or `external-drift-or-stale-good`). Then `git bisect start
  --first-parent` and `git bisect run`.
- A step is bad when a recorded target fails the same way (a compile failure
  of a recorded target counts in a test bisect too), good when all recorded
  targets pass or build, and skipped for anything else: a commit whose
  `.bazelrc` predates the job's `ci-*` config, a vendoring or image failure, a
  build error in another target.
- Each step uses the commit's own `.bazelrc` config, vendored tree (kept per
  vendor key under `BISECT_ROOT/vendor/`) and content-hashed dev image (built
  only when the host lacks it; a rebuild is listed in `rebuilt_images`), with
  `--nocache_test_results` and a private `--disk_cache`. MOD's redis and
  memcached run on the run's own network under their short names as network
  aliases.
- Disk: each step copies the vendored tree into the work tree, and the
  script never prunes `BISECT_ROOT`; operators prune it.

Host safety contract:

- Every container and network name starts with the run id
  (`bisect-<repo>-<job>-<utc>-<pid>`) and is recorded before it is created.
  Cleanup removes only those names. Nothing else is stopped, removed or
  pruned, no image is retagged or removed, and no process is signalled.
- `BISECT_ROOT` must be absolute and outside any `actions-runner*/_work`
  tree (checked on the resolved path before anything is created). One bisect runs per host (`flock` on `BISECT_ROOT/.lock`).
- A step container runs with `--cpu-shares 128` and `--cpus`/`--memory` caps
  (defaults: a quarter of the host's CPUs, at most 16; 2 GiB per CPU, at
  least 16 GiB), and Bazel `--jobs` equals the CPU cap. Service containers
  get a small fixed cap. Host-side work (the docker client, vendoring, image
  builds) runs under `nice` (and `ionice` where present); that does not carry
  into a container, so the share inside comes from the caps alone.
- Host Bazel (vendoring) runs with `HOME` and `--output_user_root` (the
  install base) under `BISECT_ROOT/<id>`. A `PATH` shim keeps the vendor
  script's own `--output_base`, records it and shuts that server down when
  each call returns; the exit trap shuts down any recorded one that is left
  and removes only the trees under `BISECT_ROOT/<id>`.
- `resume` first removes the containers the earlier worker recorded.

## Runner routing

Every fleet job chooses its runner from the event, never from inputs. A pull
request from a fork, or from Dependabot, runs on a GitHub-hosted runner, and
so does a merge group that holds one (see "Merge-queue runs"); everything else
runs on a fleet class label. The expression, copied verbatim into every fleet
job:

```yaml
runs-on: ${{ (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) && 'ubuntu-latest' || 'fleet-heavy' }}
```

The fleet class is one of `fleet-heavy`, `fleet-light`, `fleet-windows`,
`fleet-iis`. Windows jobs use `windows-2022` as the hosted side.

### The kill switch

`FLEET_ENABLED` is a repository variable. Unset or `true` means the fleet is
in use; `false` is the kill switch.

The PR, full and nightly tiers use the switch routing expression, copied
verbatim into every fleet job that has a hosted twin. It sends the job to the
hosted side for a fork or Dependabot pull request and also while the switch
is off:

```yaml
runs-on: ${{ (vars.FLEET_ENABLED == 'false' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]'))) && 'ubuntu-latest' || 'fleet-heavy' }}
```

In `pipeline-pr.yml` the expression has one more hosted term, the route the
`changes` job decided for the whole run (a merge group with a fork or
Dependabot pull request is `hosted`; a missing or empty route also means
hosted), and the job lists `changes` in its `needs` so it reads the route
directly:

```yaml
runs-on: ${{ (vars.FLEET_ENABLED == 'false' || needs.changes.outputs.route != 'fleet' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]'))) && 'ubuntu-latest' || 'fleet-heavy' }}
```

Such a job carries no `if:` guard (it always runs, on one side or the other),
except a path filter alone (`needs.changes.outputs.<name> == 'true'`) or, on
`vendor` only, the fleet-scope guard `needs.changes.outputs.fleet == 'true'`. Jobs
with no hosted twin keep the plain routing expression and a fleet-only guard
whose `FLEET_ENABLED` term is `!= 'false'`: they are skipped, not failed,
while the switch is off, and a `fleet-off-notice` job in each tier says so in
an annotation and the job summary.

To flip it: Settings, Secrets and variables, Actions, Variables, set
`FLEET_ENABLED` to `false` (or `gh variable set FLEET_ENABLED --body false
-R We-Amp/pagespeed-optimizer`); set it back to `true` to go back (do not delete it: the release lanes still test for `true`). It
is read when a run starts, so re-run a run that began before the change. The
hosted legs run without the hub and the remote cache and are slower; the
fleet-only legs are skipped, and the CI gate accepts those skips.

The release lanes do not use the switch routing expression. They keep the
plain routing expression, the original guards (`FLEET_ENABLED == 'true'`) and
their hosted twins; see "Release lanes: hosted twins".

#### The plain guards

A fleet job routed by the plain expression (the release lanes, and the jobs
without a hosted twin) carries one of these guards, verbatim, as its `if:`:

```yaml
if: ${{ vars.FLEET_ENABLED == 'true' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}
```

While the variable is unset (or anything but `true`), these jobs are skipped
for same-repository events and still run on hosted runners for fork and
Dependabot pull requests, which need no fleet. Clearing it is the kill switch
for the release lanes' fleet jobs, which then run their hosted twins.

Two more guards exist for jobs that cannot fall back to a hosted runner, and
the gate accepts exactly these three. **Fleet only** is for a job that mounts
the container socket, runs privileged containers or needs a real Linux
kernel: a fork or Dependabot pull request skips it.

```yaml
if: ${{ vars.FLEET_ENABLED == 'true' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}
```

The manual shadow lanes keep the dispatch-only guard below with `== 'true'`, so they
are skipped while the variable is unset or `false`.

**Dispatch only** is for a manual lane; a dispatch is never a fork pull
request.

```yaml
if: ${{ github.event_name == 'workflow_dispatch' && vars.FLEET_ENABLED == 'true' }}
```

The PR, full and nightly tiers write the fleet-only guard with the first term
`vars.FLEET_ENABLED != 'false'` (unset means on); the gate accepts both
spellings. A job that needs more conditions (for example a path filter) appends them in
a later change together with a matching gate rule; the gate rejects any
fleet job whose `if:` is not exactly one of the three guards. A job that runs
on a hosted label only (hosted arm64, macOS, report jobs) carries no guard.

### Hosted side: degraded mode

Every fleet job must also run on a hosted runner: no hub, no remote cache,
no host paths. Anything host-specific is read from the runner environment
(`docs/ci/runner-environment.md`) and used only when set. When none of it is
set the job runs slower, never fails for that reason. In `pipeline-pr.yml`:

- **vendor** decides a distribution channel: `hub` when the runner provides
  a local vendor directory or a hub, `cache` otherwise. On the cache channel
  `vendor/` passes to the consumer jobs through `actions/cache`, keyed on the
  vendor manifest (`MODULE.bazel`, `.bazelversion`, `bazel/**`,
  `third_party/**`); a dependency bump re-vendors once.
- **Linux Build** and **Linux Unit Tests** use the remote Bazel cache only
  when `BAZEL_CACHE_HOST` is set (`--config=ci`); otherwise they build with
  `--config=docker` and no cache. The build bundle always goes to
  `actions/upload-artifact` (retention 3 days) and the unit tests download it.
- **Kernel differentials**, the **release-config test compile** and the
  **fuzz smokes** follow the same two rules: the remote cache only when
  `BAZEL_CACHE_HOST` is set, and the dev image built in the job when the
  runner has none. The differentials' floor ratchet reads the public
  `We-Amp/mod_pagespeed` repository, and only when a pin floor moves.
- **Windows Build** and **Windows Unit Tests** take their tools from `PATH`
  or from `CI_WIN_TOOLS_DIR`. On the hub channel a runner with a vendor
  tarball (`CI_WIN_VENDOR_DIR`, or a fetch from `CI_HUB_SSH`) extracts it
  over the checkout; otherwise Bazel fetches the dependencies itself. The
  remote cache is used only when `BAZEL_REMOTE_CACHE_URL` is set. The build
  bundle goes to `actions/upload-artifact` (`ci-artifacts-win-x64`). The
  hosted Windows build time has not been measured; if it proves too slow, the
  fallback is to run these two jobs on the fleet only.
- **Lint** keeps its Bazel state in a per-runner cache directory under the
  runner's home, which persists on a fleet runner and is empty on a hosted
  one. No credential enters the lint containers.
- **Lint scope on a pull request.** A changed `.cc` file is linted; a changed
  header lints the `.cc` files that include it, directly or through other
  headers (`tools/ci/lint-scope-expand.sh`, run inside the lint container and
  limited to the files in `compile_commands.json`). A changed `.cc` file with
  no compilation-database entry fails the job; a dependent of a header that is
  not in the database is listed in the log and skipped. When the changed files
  reach no translation unit at all, clang-tidy is skipped with a log line; it
  never falls back to the whole tree. A change to the clang-tidy configuration
  (`.clang-tidy`, `test/.clang-tidy`, `lib/image/generated/.clang-tidy`), the
  lint image (`docker/Dockerfile`, `docker/Dockerfile.lint`), the
  compile-database recipe (`tools/ci/lint-compile-commands.sh`, the root
  `BUILD`), the expander, `MODULE.bazel` or `.bazelrc` lints the whole tree,
  because only the whole tree can show the effect of such a change. The path
  list is `LINT_TRIGGER_PATHS` in the "Compute lint scope" step. The
  whole-tree lint runs in the nightly and on `workflow_dispatch`; a pull
  request that changes a lint trigger file also lints the whole tree. The self-test is
  `tools/ci/test_lint_scope_expand.sh`.
- **Static analyzer scope.** The clang static analyzer (`clang-analyzer-*`)
  runs on `lib/` and `src/` only and is turned off for `test/` in
  `test/.clang-tidy`. Measured reason: it accounts for about 96% of clang-tidy
  CPU on the test tree (the largest test unit took 33.6 minutes
  single-threaded with it and 100 seconds without it), and the test tree is
  88% of a whole-tree lint. `test/.clang-tidy` is itself a lint trigger file
  (listed above), so a change to it lints the whole tree.

### Release lanes: hosted twins

A release cannot wait for the fleet. In the release pipelines
(`pipeline-release-optimizer.yml`, `pipeline-release-nuget.yml`) every
fleet job has a hosted twin: the same steps (shared through a YAML anchor),
a hosted label, and `if: ${{ vars.FLEET_ENABLED != 'true' }}`. Exactly one
of each pair runs, so clearing `FLEET_ENABLED` moves a release to hosted
runners instead of skipping it. A job that consumes the output of a pair
names both in `needs:` and runs when either succeeded. Release lanes pass
every file between jobs as workflow artifacts; they use no hub.

### Path-filtered jobs

The `changes` job (hosted) diffs a pull request against its base and
publishes one boolean per filter (`css`, `js`). A fleet job that depends on a
filter narrows the guard with `&& needs.changes.outputs.<name> == 'true'`:

```yaml
if: ${{ (vars.FLEET_ENABLED == 'true' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]'))) && needs.changes.outputs.css == 'true' }}
```

Outside a pull request every filter is false. The workflow-shape gate accepts
the guard either verbatim or in exactly this narrowed form.

## Artifacts

Jobs pass files through `actions/upload-artifact` and
`actions/download-artifact`. Artifact names are unique within a workflow run,
and the caller runs the PR tier and the full tier in one run, so each tier
prefixes its names: the PR tier uses none, the full tier `full-`, the nightly
tier `nightly-`. A build bundle is the directory `.ci-artifacts` (binaries,
`metadata.json`, `checksums.sha256`); a consumer downloads it to a directory
and hands the parent to `tools/ci/stage-artifacts.sh`, which validates it.
Retention is three days for bundles and evidence.

## Report jobs

A job that files or closes an issue never runs on the fleet. The fleet job
uploads a small artifact (the findings text, plus a `lane-ran` marker file
that says the lane reached a verdict) and a hosted `report` job with
`issues: write` downloads it and runs `tools/sbom/post-findings-issue.sh`.
Without the marker, or without the artifact, the report job leaves the issue
alone, so a failure before the lane ran cannot close a live issue. Only a run
on the default branch changes the issue; a branch dispatch is a dry run.
The gate enforces the split (rule f): every reusable workflow declares a
top-level `permissions:` block with no write access, and a write permission
(quoted or not) is allowed only on a job with a hosted `runs-on`. The report
jobs check out `main` for the tools they run, because they hold a write token.

### The schedule guard

GitHub's cron is best effort, so an external scheduler also dispatches
`nightly.yml` every night (with `full_tier` set, which reproduces what a
scheduled night runs). The hosted `guard` job makes a first-attempt scheduled
run skip all its work, and cancel itself, when a real night started in the
previous 20 hours. A real night is a run on the default branch that is not
cancelled and is either a scheduled run or a full-tier dispatch; the run
title (`run-name`) marks the latter `Nightly (full)`. Other dispatches and
skipped (cancelled) runs never count, so they cannot suppress a night. A
dispatch and a re-run never skip. The guard runs inside the `nightly`
concurrency group, so a late cron run that waited for a running dispatch sees
it and skips. A queued dispatch can be replaced by a newer queued run of the
group; it then reads `cancelled`, is not counted, and the cron run proceeds.
If the guard cannot read the run list it lets the run proceed, and if the
guard job itself fails the jobs still run. A skipped run has no `full` job,
so `nightly-red` stays silent for it and still reports a real red.
One edge remains: a queued full-tier dispatch that the guard counted and a non-full hand dispatch then replaces in the group is a lost night, so avoid hand dispatches during the nightly window.

The manual CSS oracle shadow lane (`css-fuzz-shadow`) runs only on a dispatch with `css_fuzz_shadow` set (default false), so the nightly dispatch does not add it.

### The nightly-red issue

`nightly.yml` ends with the hosted job `nightly-red` (`Nightly full-tier
report`): `needs: [full]`, runs whenever `full` was not skipped,
`actions: read`, `contents: read` and `issues: write`, no secret. It runs
`tools/ci/nightly_red_report.sh` from `main`. The caller sees one result
for `full`, and a `continue-on-error` job never turns it red, so the script
reads the run's own job list and judges each `full / <job>` itself:

- **skipped** never counts as green, and is never listed as failed either;
  the issue names skipped jobs on a line of their own.
- **green** is `success`, plus a success of every step named for the job in
  `RUNTIME_STEP_CHECKS` (`<job name>=<step name>` lines). This repository
  leaves it empty.
- **failed** is everything else (failure, cancelled, timed out, a green job
  whose checked step did not succeed).

When a job failed, the script opens one issue titled `Nightly full tier red`
with the label `nightly-red` (created on first use), or comments on the open
one and replaces its body. Per failed job the body lists the job, the SHA,
the failing targets from its log (`tools/ci/failing_targets.sh`; `none
extracted` when the log names none, `timeout-only` when every one timed
out, `from the whole job log` when the failed-steps log was not available
and the whole job log was read instead, so a line from a retried step can
appear), the last completed run in which that job was green and the commit
count since, and a marker line `<!-- nightly-red-job: <job name> -->`. A
`Skipped` line lists the skipped jobs, or `none`. The last green is searched
in the completed runs on `main` of `nightly.yml`, of `ci.yml`'s
`workflow_dispatch` and of `ci.yml`'s push, created before this run (the
run's own creation time, which a re-run keeps), newest first, whatever their
event (a scheduled night, a `full_tier` or `full: true` dispatch, a push), so
a missed night does not hide it. `REPORT_MAX_LOOKUPS` (12) counts only runs
that ran a `full / ` job, so a day of push runs without the full tier does
not use it up; `REPORT_MAX_API_CALLS` (40) caps the jobs calls of the search.
When none is found the body says `no green within <N> completed full-tier
runs`, with `(API call ceiling reached)` when the ceiling ended the search.
The body names jobs, SHAs, targets and run links only.

When no job failed, the open issue is closed, with a comment, once every job
named by a marker line is green in this run. **A re-run never closes it:** on
`GITHUB_RUN_ATTEMPT` 2 or later the script only comments, because a re-run
that goes green is a flake signal and not evidence of a fix. An API failure
fails the job, so a reporter that could not report is visible. A run on any
ref other than `main` is a dry run that prints what it would write.
`tools/ci/test_nightly_red_report.sh` and `tools/ci/test_failing_targets.sh`
(in the `Public-tree hygiene gate` job) pin this behaviour.

## The queued-run sweep

A job whose runner label nothing serves (a retired label, or a fleet class
whose runners are not registered while `FLEET_ENABLED` is on) queues for days
instead of failing. With `FLEET_ENABLED` off the fleet guard skips the job, so
nothing queues. `sweep-queued.yml` runs hourly on a hosted runner and calls
`pipeline-sweep.yml`, which runs `tools/ci/sweep-queued-runs.sh`: it lists runs
in `queued` or `waiting` status whose latest attempt started more than 30
minutes ago (`run_started_at`, so a re-run of an old run is not stale; the
script falls back to `created_at` only when that field is empty), keeps any
run with a job that has a runner and any run of a workflow on the skip list
(`Release` by default, because a run that waits for a manual approval looks the
same), and cancels the rest with one log line each. `--dry-run` only logs.

The threshold is a repository variable: set `SWEEP_MAX_AGE_MINUTES` (a whole
number of minutes) to override the 30 minute default. Raise it when a busy
runner class legitimately keeps whole runs queued for longer, for example
behind the long nightly stress legs. The logic is tested against a fake `gh` by
`tools/ci/test_sweep_queued_runs.sh`, which the sweep job runs first.

## Merge-queue runs

A `merge_group` run is classified, not refused. The `changes` job (hosted)
has a step `Classify the merge group`, which runs only on `merge_group`. It
fetches the default branch afresh, takes the group to be every commit between
it and the group head (a stacked group's `base_sha` is the previous entry's
queue commit, so it is not used; the default branch must be an ancestor of the
head, and the range may hold at most 10 commits), resolves every pull request
in it from the last `(#N)` of each commit subject (the head commit's number
must equal the one in the group's `head_ref`), and looks each one up with `gh api` (5 tries, 2, 4, 8 and 16 s
apart). A group is **trusted** when every pull request comes from this
repository and none is authored by `dependabot[bot]`; it is **untrusted** when
it holds a fork pull request, a pull request whose head repository is another
one (compared case-insensitively) or a Dependabot pull request. The step turns
the classifier's exit status into the `route` output: `0` is `fleet`, `3` is
`hosted`, and any other status fails the step. A trusted group runs on the
fleet as before. An untrusted group runs every leg on GitHub-hosted runners:
slower (a Windows unit-test leg on the critical path), without the shared
caches, but with the same checks, and the fork pull request still merges
through the queue.

The classifier refuses (exit `1`, the job fails, `CI gate` goes red) when it
cannot decide: a pull request has lost its head repository, the fork flag is
unreadable, the range is empty or holds more than 10 commits, the default branch is not an ancestor of the head, a commit subject has no `(#N)`, the `head_ref`
cannot be parsed, a pull request is not open (one in a merge group is, unless it merged earlier in the same queue run as the commit the group attributes to it), or a lookup still fails after the last try. It also refuses
a fork pull request (not a Dependabot one) whose squash commit changes a path
under `.github/` or `tools/ci/`: such a change is re-landed by a maintainer
from a branch.

A separate step, `Decide the runner route`, publishes the one `route` output of
the `changes` job for every event: for a merge group it requires exactly
`fleet` or `hosted` from the classification step; a pull request from a fork
or from Dependabot is `hosted`, and any other pull request, push or dispatch
is `fleet`; any other event fails the step. Every fleet job lists `changes` in
its `needs` and reads `needs.changes.outputs.route` directly; the value is
never passed through `vendor`, which runs the queued tree's code. A missing or
empty route means hosted.

The classifier is `tools/ci/queue_fork_guard.sh`. The step writes it from
`origin/main` to a file in the runner's temp directory and runs that file; it never runs the copy in the
queued tree, and a script missing on the default branch fails the step. The
workflow-shape gate (`queue-guard-reads-default-branch`) requires the step in
the `changes` job, restricts its `run:` block to that read, the run of the file and the exit-status
mapping (plus `set -euo pipefail` and the default-branch fetch), pins the route
step and rejects any other read of the script in a workflow file.
`tools/ci/test_queue_fork_guard.sh` covers the classifier against a fake `gh`
and a temporary git repository.

The PR-number rule depends on the queue's **squash** merge method: one squash
commit per pull request, its subject ending in `(#N)`. A different method
(merge commits or rebase) must change the classifier first; a group without
that shape is refused.

**Event guard.** A queued tree can change the caller `ci.yml` to call any
other allow-listed fleet pipeline, and it can also produce `push` and
`workflow_dispatch` events of its own (the queue pushes a temporary
`gh-readonly-queue/` branch; a queued job with a write token can dispatch a
workflow). Every one of those pipelines therefore opens with a hosted job
`event-guard` that refuses: any event outside the pipeline's allowed list
(`merge_group` is allowed nowhere but `pipeline-pr.yml`); a ref under
`refs/heads/gh-readonly-queue/` or outside the pipeline's allowed refs; a
`workflow_dispatch` made by `github-actions[bot]` (a dispatch with a workflow
token); and a pull request from a fork or from another repository. The allowed
events and refs are `pipeline-full.yml` (events `push`, `workflow_dispatch`, `pull_request`, `schedule`; refs `refs/heads/main`, `refs/heads/ci-dev`, `refs/pull/*/merge`: `ci.yml` runs it for a push to `main`, a dispatch with `full: true` and the `ci:full` label, and `nightly.yml` for the scheduled nightly and a `full_tier` dispatch), `pipeline-nightly.yml` (`schedule`, `workflow_dispatch`; `refs/heads/main`), and `pipeline-release-optimizer.yml` and `pipeline-release-nuget.yml` (`push`, `workflow_dispatch`; `refs/tags/*` and `refs/heads/main` for dry runs). Every fleet job reaches `event-guard` through `needs`,
so a refusal skips the whole fleet. The workflow-shape gate (`event-guard`)
pins the job text, the per-pipeline lists and the edges, scans every workflow file that has a fleet `runs-on` (not a fixed list), and rejects a job between the guard and a fleet job that can still run after the guard failed. Job keys (`if`, `needs`, `runs-on`, `continue-on-error`, `uses`) are matched quoted or with whitespace before the colon. Each job's `if:` is normalised first (folded, literal and plain multi-line forms joined, where a plain value continues on every line indented more than its key and a `#`-led line inside a block scalar is content; lowercased, whitespace collapsed, the space between a function name and its `(` removed). Then, for every job on a `needs` path between `event-guard` and a fleet job (and the fleet jobs themselves, unless the routing-guard rule pins their `if:`), an `if:` that contains `always(`, `failure(`, `cancelled(`, `success(` (unless the whole `if:` is a bare `success()`), `||`, `!` (a negation anywhere, so `!=` and `!= true` too) or `== false` must be exactly `needs.event-guard.result == 'success' && (<inner>)`, optionally inside `${{ }}`, where the parentheses around `<inner>` are one balanced group (parentheses inside string literals do not count) and `<inner>` does not mention `event-guard`; the job must also need `event-guard`, and a block scalar holding the condition must strip its final newline (`|-` or `>-`), since a kept newline is text around `${{ }}`. Otherwise the gate rejects the job by name. Because `!` covers `!=`, an `if:` such as `${{ needs.event-guard.result != 'failure' }}` or `${{ vars.FLEET_ENABLED != 'true' }}` on such a job is rejected as well, although it would be safe; write it as `${{ needs.event-guard.result == 'success' && (vars.FLEET_ENABLED != 'true') }}`. Any other `if:` is accepted as today (GitHub adds `success()` implicitly unless the expression names a status function). The gate also refuses, in every workflow's `jobs:` section, a YAML anchor or alias on a job or on a job-level key other than `steps:` and `env:`, and any `<<:` merge key, because it reads the text and cannot resolve them (anchored step lists and env maps stay allowed).

The `changes` job of `pipeline-pr.yml` refuses the same two cases for itself,
because a queued tree's caller can also reach that pipeline with a `push` on the
queue's temporary branch: before the fork guard step, `Refuse pushes that are
not on the default branch` accepts a push only on `refs/heads/<default>` or
`refs/heads/ci-dev`, and `Refuse dispatches made with a workflow token` refuses
a dispatch made by `github-actions[bot]`. The shape gate
(`queue-guard-reads-default-branch`) requires these two steps and the fork guard
step, in this order, as the first steps after the checkout, each in exactly
this text.

These in-tree guards bind runs whose caller workflow is unmodified: a queued
tree controls its own caller `ci.yml`, so maintainers review `.github/` changes
before queueing. Contributors cannot add their own pull requests to the merge
queue. A maintainer reads the full diff of a pull request from a fork,
including any `.github/` or `tools/ci/` changes, before adding it to the queue;
fork pull requests that change those paths are re-landed from a branch
instead. What keeps
a fork pull request's code off the shared build runners is the runner group
that serves them, which accepts only the pipeline workflows at the default
branch (and `ci-dev`), together with the classifier in the default branch's
pipeline. The
`@ci-dev` allow-list entries stay. Pushing `ci-dev` from a `main`-based branch
gives its copies of `pipeline-pr.yml` and `pipeline-full.yml` the classifier
and the guards above, so after a change to them lands on `main` a maintainer
pushes `ci-dev` again from `main`.

## CI_SCOPE: the fleet scope of the PR tier

The `changes` job of `pipeline-pr.yml` decides, in its step `Decide the fleet
scope`, whether the fleet legs of the PR tier run. It publishes two outputs,
`fleet` and `heavy` (equal for now); `vendor` runs only when `fleet` is
`true`, and every other fleet leg needs `vendor` (the fuzz smokes, which need
only `changes`, never run outside a pull request or a merge group), so
`fleet=false` skips them all. The hosted jobs of the PR tier and the whole
full tier run as before.

- `workflow_dispatch` always runs the whole PR tier. The step decides it
  inline, whatever `CI_SCOPE` says, so the release candidate dispatch
  (`full: true`) never skips a fleet job. A merge group runs the whole PR
  tier too, except a docs-only merge group under an ALLGREEN queue once
  `DOCS_LANE` is `on` (see "DOCS_LANE: the docs-only lane").
- For every other event the step reads `tools/ci/pr_tier_scope.sh` from
  `origin/main` with `git show`, never from the tree under test, and runs
  that copy. A missing script, a failing script or any output other than
  exactly the two lines `fleet=<true|false>` and `heavy=<true|false>` runs the
  whole tier and leaves a warning.
- The repository variable `CI_SCOPE`, lowercased, sets the level. Unset,
  `full` or any value the script does not know runs everything. `push`,
  `pr-build` and `pr` turn on the push rule.
- **The push rule.** A push to `main` skips the fleet legs when the merge
  queue already proved the same commit and the commit does not change the PR
  tier itself. Proof: the commit has a completed, successful GitHub Actions
  check suite on a `gh-readonly-queue/` branch
  (`gh api repos/<repo>/commits/<sha>/check-suites?app_id=15368`, up to 3
  tries, 2 and 4 s apart; an empty or unparseable answer after the last try
  counts as no proof). The suite is not enough by itself: the proof covers the whole queue group,
  which is every commit between the default branch and the group head. A
  stacked group's base is the previous entry's queue commit, so the group is
  not read from the suite's branch name. Instead the pull request of each
  commit in the union of `BEFORE_SHA..SHA` and the last 10 first-parent commits
  ending at `SHA` (the last `(#N)` of each subject, looked up with `gh api`, 3
  tries) must be from this repository and not authored by `dependabot[bot]`: a
  group with a fork or Dependabot pull request ran on hosted runners, and a
  merge group can land several entries over more than one push. A missing or
  zero `BEFORE_SHA` or one that is not an ancestor of `SHA`, an empty or
  unreadable range, a merge commit, more than 10 commits in `BEFORE_SHA..SHA`, a
  subject without a number or a failed lookup counts as no proof, and the full
  tier runs. A change to the PR tier: any path under
  `.github/actions/` or `tools/ci/`, or `.github/workflows/pipeline-pr.yml` or
  `.github/workflows/ci.yml`, in the pushed commit; a commit without a parent
  counts as one. The callers grant `checks: read` to the `pr` job for the
  lookup; the pull request lookup uses the `pull-requests: read` the `changes`
  job already has.
- **Log lines.** The script writes one line
  `fleet scope: event=<event> scope=<level> fleet=<bool> heavy=<bool> reason=<text>`
  and, on every push whatever the level,
  `shadow: would_skip=<bool> proof=<true|false|error> pipeline_change=<bool>`:
  what the push rule decides for this commit, so the rule can be measured
  with `CI_SCOPE` unset before it acts. When the commit changes the PR tier,
  the proof is not looked up and the line says `proof=false`.
- **Rollback.** Set the variable back:
  `gh variable set CI_SCOPE --body full --repo We-Amp/pagespeed-optimizer` (or
  delete it). The next run takes it; nothing else changes.
- **Coverage on a skipped push.** The fleet legs ran on the same commit in the
  merge queue, so a skipped push loses no check of the merged tree, with one
  difference: a push to `main` no longer runs a whole-tree clang-tidy. The PR
  tier's `lint` computes its scope per event (scoped or none by the changed
  files on a pull request or in the merge queue, with the dependents of a
  changed header included, and the whole tree on a `workflow_dispatch`) and on a push runs
  clang-format only. The full tier's `lint-full` runs for a `ci:full` pull
  request, for a dispatch with `full: true` and in the nightly, and is
  skipped on a push. The whole-tree lint for `main` is that job in the
  scheduled nightly's call of the full tier, `full / Lint (whole tree)`
  (see "When the full tier runs"); the nightly tier has no twin of it. TSan,
  ASan, the privilege rigs and the arm64 legs run with the full tier (see
  "When the full tier runs" for `FULL_TIER_TRIGGER`).
  The hosted checks of the PR tier (Apache
  RAT, license headers and SBOM drift, the blocking and the report-only
  dependency scans and the others) are not fleet legs and still run on every
  push. `tools/ci/test_pr_tier_scope.sh` covers the script against a fake
  `gh`, and the workflow-shape gate (`fleet-scope`) pins the step.

## DOCS_LANE: the docs-only lane

A pull request or merge group that changes documentation only does not need
the fleet legs: no build, test or fleet job reads those paths. The
`Decide the fleet scope` step hands a pull request and a merge group to
`tools/ci/pr_tier_scope.sh` with `BASE` (the pull request's or merge group's
base commit) and the repository variable `DOCS_LANE`.

- **The classifier.** `tools/ci/docs_only.sh BASE HEAD` prints exactly
  `docs_only=true` or `docs_only=false`. A change is docs-only when every path
  of `git diff --no-renames --name-only BASE...HEAD` matches its allowlist:
  `website/`, `docs/`, a Markdown file at the top level, `.github/ISSUE_TEMPLATE/` and `.github/pull_request_template.md` (`shared/` and the sources of the generated website references stay code). Anything else is false: a path off the list (`tools/ci/` and
  every workflow included), a rename out of a code path (renames are not
  detected, so the old path is listed), a missing or unknown base, a failed
  or empty diff. `pr_tier_scope.sh` reads the classifier from
  `origin/main` with `git show`, like itself, so a pull request cannot
  change its own lane; an unreadable classifier or any other output is false.
  No workflow names the classifier (the workflow-shape gate's `docs-lane`
  rule).
- **Merge groups.** A docs-only merge group counts only while the merge queue
  of `main` groups ALLGREEN, read from
  `gh api repos/<repo>/rules/branches/main` (3 tries). Any other grouping,
  no queue rule, more than one, or a failed read is the full tier. Under
  ALLGREEN a code entry queued ahead of a docs entry merges only on its own
  result, and the docs entry's range (`base_sha...head_sha`) is its own change.
- **`DOCS_LANE`.** Unset, or any value other than `on` (lowercased), is shadow
  mode: the decision is logged and the PR tier runs in full. `on` skips the
  fleet legs (`fleet=false heavy=false reason=docs only`). The `ci:pr-full`
  label always runs the full tier; `workflow_dispatch` and `push` never take
  the lane.
- **Log lines.** On every pull request and merge group:
  `docs lane: mode=<on|shadow> would_skip=<bool> docs_only=<bool> queue=<grouping|none|mixed|error|n/a> pr_full=<bool>`,
  the classifier's own `docs lane: docs_only=<bool> (<reason>)` line, and,
  when the lane would skip, one line saying whether it did (`skipping vendor
  and every fleet job that needs it`) or would have (`shadow: would skip
  ...`).
- **Push burst guard.** Once docs entries can pass the queue without the
  fleet, the queue proof of a docs-only commit covers only that commit. With
  `DOCS_LANE` on, a push whose tip commit is docs-only but whose
  `BEFORE_SHA..SHA` range holds code drops the queue proof and runs the fleet
  legs (`docs lane: burst guard: ...`); in shadow mode the line says what it
  would do.
- **Rollout and rollback.** Read the shadow lines on docs and code pull
  requests first, then `gh variable set DOCS_LANE --body on --repo We-Amp/pagespeed-optimizer`.
  Roll back with `gh variable delete DOCS_LANE --repo We-Amp/pagespeed-optimizer`; the next run
  takes it.
- **The website gate (`WEBSITE_GATE`).** The docs lane leaves a website
  change with the hosted gates only, so the site's own checks can join the
  PR tier: the `changes` job publishes `website` (true when a pull request or
  merge group changes `website/`, `shared/product-facts.mjs`,
  `.github/workflows/website.yml`, `src/worker/main.cc`,
  `src/nginx/ngx_pagespeed_module.cc` or `docker/nginx-version.txt`), and the
  `website` job calls `website.yml@main`. Source checks (`npm run format:check`,
  `npm run lint`), build, unit tests, content lint and reference drift run on
  both events; the Playwright suite and the Lighthouse
  budgets run on the pull request only. A failure fails `pr` and `CI gate`; a
  run without website changes skips the job, which `CI gate` accepts. This
  part does not depend on `DOCS_LANE`.

## WEBSITE_GATE: the website checks in `CI gate`

The `website` job of `pipeline-pr.yml` runs only when the repository variable
`WEBSITE_GATE` is `on`. Unset (the default) or any other value keeps today's
behaviour: `website.yml` runs from its own `pull_request` trigger as an
advisory check, outside `CI gate` and outside the merge queue. With the
variable `on`, the CI pipeline calls `website.yml` and every job of a direct
`pull_request` run of `website.yml` skips, so the checks run once, inside
`CI gate`; flipping the variable is the only step
(`gh variable set WEBSITE_GATE --body on --repo We-Amp/pagespeed-optimizer`,
rollback `gh variable delete WEBSITE_GATE --repo We-Amp/pagespeed-optimizer`).
**Release windows.** The build job's release-state guards are release-window
tolerant, so turning the gate on does not block the pull requests that
carry a release. A run is a release window when it is a pull request (or a
merge-queue entry, resolved through its pull request) whose head branch is
`auto/sync-release-manifests` or `release/*` **and** lives in this repository
(a fork cannot earn the tolerance by naming its branch); the step
`Detect a release window` (`website/scripts/release-window.sh`) sets
`RELEASE_WINDOW=1` for the rest of the job (and `RELEASE_SYNC=1` for the sync
branch alone). Main pushes, every other pull request and any failed lookup are
not a window; a failed merge-queue lookup prints a `::warning::` (the job token
needs `pull-requests: read`, granted to the `website` job and to the
`website.yml` build job). A merge-queue group built for a release pull request
also holds the pull requests queued ahead of it, and they get the window too in
that group.

| Guard | Reads | Outside a window | In a window |
| --- | --- | --- | --- |
| 2.1 manifest lags the latest stable tag (`check-version-drift.sh`) | tag of the public module repo | warning for 6 h after the tag, then error | warning at any tag age |
| Module-only release: a newer tag on the manifest's line with no optimizer `v*` tag (same script) | tags of both repositories, `MODULE_PACKAGE_VERSION` | passes when `MODULE_PACKAGE_VERSION` names the tag; otherwise the lag rule above | same |
| Stale hand-pinned image tags (same script) | committed content vs the manifest | error | warning on `auto/sync-release-manifests` only (the sync bot edits `release.yaml` alone, so it cannot fix them in its commit); error on `release/*` |
| `MODULE_PACKAGE_VERSION` equals the live package index (`module-package-version.test.ts`) | package index | error | warning |
| Version map row matches the Helm chart (`version-map.test.ts`) | `deploy/helm/pagespeed/Chart.yaml`, bumped by the release lane first | error | warning |
| Version map row matches `MODULE_PACKAGE_VERSION` (same test) | two committed files | error | error |
| Built-output content lint (`npm run check:content`) and the `llms*.txt` drift test | committed content | error | error |

Because the sync pull request may merge with stale image tags, a follow-up
pull request that moves them is then required: the next push to `main` fails
the drift guard until it lands.

The content lint, the llms drift test and the two committed-file rows above are not live state: they read only
what the pull request commits, so a failure on a release blog post (a
retired "2.1" label in its teaser, a `public/llms*.txt` not regenerated with
`npm run gen:llms`) is a real content defect and stays a failure in a window.
The warnings are `::warning::` annotations that say they are release-window
warnings. A `release/*` or sync branch that lingers after its release keeps
the tolerance until it is merged or deleted, so keep those branches short
lived.
The call is pinned to
`website.yml@main`, like every reusable workflow here: a `ci-dev` dispatch
that changes `website.yml` runs the copy on `main`, and until `main` has the
`workflow_call` trigger the `website` job cannot run at all. To try a change
to the callee on `ci-dev`, point that `uses:` line at `@ci-dev` on the
`ci-dev` branch only, and never merge that edit.

## Required checks

`ci.yml` is a thin caller, so a check from a called workflow is named
`<caller job> / <job name>`, and the required checks are
`pr / Build daemon & unit-test slice (ubuntu)`,
`pr / License audit (Apache RAT)`, `pr / License Headers + SBOM Drift`,
`pr / Public-tree hygiene gate`, `pr / Blocking Dep Scan (npm + cargo)`,
`pr / ABI Compatibility (lib/pagespeed C API)`, `pr / JS/TS Unit Tests`,
`pr / Release Packaging Container`, the fleet legs once `FLEET_ENABLED` is on
(`pr / Linux Build`, `pr / Linux Unit Tests`, `pr / Lint (clang-tidy +
clang-format)`, `pr / JS Kernel Differential (vendored goldens)`,
`pr / HTML Parse Differential (vendored goldens)`, `pr / Release-config test
compile`, `pr / Windows Build`, `pr / Windows Unit Tests`), and `CI gate`.

Not listed individually: `pr / Dependency Scan (report-only)` (it never
fails the build), the two fuzz smokes (`pr / CSS fuzz smoke (deterministic
oracle replay)`, `pr / JS fuzz smoke (deterministic oracle replay)`),
`pr / Production readiness (ETag subset)` and `pr / nginx integration
(Test::Nginx t/)`, which are skipped on a pull request that does not touch
their paths. The ETag subset runs when a pull request or
a merge group changes `src/nginx/etag_util.*`,
`src/nginx/ngx_pagespeed_module.cc`, `src/nginx/header_key_util.*`,
`src/worker/worker.cc`, `src/worker/worker.h`, `src/worker/api_handlers.*`,
`src/worker/http_server.*`, `src/nginx/headers_sidecar_replay.h`,
`src/nginx/revalidation_storability.h`, `lib/cache/headers_sidecar.*`,
`lib/classify/alternate_metadata.*`, `tools/e2e/Dockerfile.prebuilt`,
`tools/common/` or anything under `tools/production-tests/`, and on a `workflow_dispatch`
(the path filters are not computed for a dispatch, so this is what lets a
`ci-dev` dispatch exercise it). It stages the Linux Build bundle, starts the
production-readiness stack and runs `tests/test_conditional_requests.py` and
`tests/test_range_requests.py` only. The aggregate `CI gate` treats `skipped`
as fine and fails on any other non-success result; a skipped job leaves `pr`
successful, which is what `CI gate` reads. The job runs under a compose project
named for the run and attempt, so two runs on one host never share images,
containers or the volume.

The Test::Nginx job runs when a pull request or a merge group changes
anything under `src/nginx/`, `lib/cache/`, `lib/classify/`, `lib/base/`,
`src/proto/`, `src/crypto/webbotauth/` or `t/`, `src/worker/shared_config.*`,
`src/worker/serve_stats.*`, `src/worker/async_css_loader.*`,
`tools/seed-test-cache.cc`, `tools/docker-test-nginx.sh`, `docker/Dockerfile`,
`docker/nginx-version.txt` or `pipeline-pr.yml`, and on a
`workflow_dispatch`. The run step uses `shell: bash` (`-eo pipefail`) so a
failing suite is not hidden by the pipe into `tee`. It builds nothing: it
downloads the Linux Build bundle (module, `seed_test_cache`,
`webbotauth_sign_tool`) and runs `tools/docker-test-nginx.sh` in its prebuilt
mode (`PREBUILT_DIR`) inside the dev image, whose nginx is the version the
module was built against. The container runs as root with the checkout
mounted read-only and copied in, so nothing root-owned lands in the runner's
workspace. The nightly runs the same suite on its optimized x64 bundle
(`test-nginx-x64`).

## Trying a pipeline change on the fleet: `ci-dev`

A change to a reusable workflow does not reach the fleet from its own pull
request (the callers pin `@main`). To exercise it before merge:

1. Push the change to the `ci-dev` branch:
   `git push --force origin HEAD:ci-dev`. Push `ci-dev` only from a branch
   based on the current `main`, so it carries the queue and event guards.
2. Dispatch the `CI (ci-dev)` workflow (`ci-dev.yml`) from the Actions tab
   or with `gh workflow run ci-dev.yml --ref ci-dev`. It calls
   `pipeline-pr.yml@ci-dev`. Use `--ref ci-dev`: the event guard of the
   full-tier pipeline accepts a dispatch only on `main` or `ci-dev`.
   To run the full tier as well, set the `full` input:
   `gh workflow run ci-dev.yml --ref ci-dev -f full=true` (or tick `full`
   in the Actions tab). The `full` job then calls `pipeline-full.yml@ci-dev`.
3. When it is green, merge the pull request as usual.

The `ci-dev` workflow is latest-wins: its concurrency group cancels the run in
flight when a newer one is dispatched, so only the newest dispatch finishes.
The `ci-dev` branch is shared and disposable; whoever pushes it last owns
the next dispatch.

## The workflow-shape gate

`tools/ci/check-workflow-shape.sh` runs in the `Public-tree hygiene gate`
job and fails the build when the shape breaks:

- **(a)** no workflow uses `pull_request_target`;
- **(b)** a workflow with any trigger other than `workflow_call` is a caller:
  its jobs only `uses:` `We-Amp/pagespeed-optimizer/.github/workflows/<file>.yml@main`
  or `@ci-dev`, with no `runs-on` or `steps`, except an aggregate gate job
  on a hosted label. Files listed in `tools/ci/workflow-shape-allowlist.txt`
  (with a reason each) are exempt from this rule only;
- **(c)** no local reusable-workflow reference (`uses: ./.github/workflows/...`);
- **(d)** in a reusable workflow every `runs-on` is a hosted label
  (`ubuntu-latest`, `ubuntu-24.04`, `ubuntu-24.04-arm`, `windows-2022`,
  `macos-15`, `macos-latest`), exactly the plain routing expression with a
  fleet class, or the switch routing expression; a job routed by the plain
  expression carries one of the three fleet guards verbatim (the general form
  optionally narrowed by a `changes` path filter, see above), and a job routed
  by the switch expression carries none. In `pipeline-pr.yml` the switch
  expression must be the queue form (with the `needs.changes.outputs.route !=
  'fleet'` term; the plain switch form is rejected there), and a fleet-only
  job (plain route) carries a fleet-only guard ending in `&& needs.changes.outputs.route
  == 'fleet'`; every such job lists `changes` in its `needs`;
- **(e)** `.github/` and `tools/ci/` carry no LAN address literals, no
  `.home` names, no `:9090`, no `self-hosted` and none of the retired runner
  label words;
- **(f)** in a reusable workflow a `write` permission (workflow or job level)
  is allowed only on a job with a hosted `runs-on`; in `pipeline-pr.yml` also
  only on a job whose `if:` keeps it off `merge_group`, so a job that can run
  on a merge group stays read-only;
- **(queue-guard-reads-default-branch)** in `pipeline-pr.yml`: the `changes`
  job holds the merge-queue guard step as the third of three fixed steps
  after the checkout step (after the push-ref and token-dispatch refusal
  steps; the checkout sets no `env:`), with fixed keys (compared unquoted), `if:`,
  `run:` block (the classifier is read from `origin/${DEFAULT_BRANCH}` with
  `git show` and its exit status mapped to `route=fleet` or `route=hosted`),
  an `id: queue` and only the guard's own env keys; the job also holds the
  pinned step `Decide the runner route` and publishes `route` from it; no other line names the
  script in any spelling, and the job has no glob, `find`, `source` or
  `GITHUB_ENV` write under `tools/ci/`; `BASH_ENV`, `PATH`, `GIT_DIR`, `GIT_WORK_TREE` and `GIT_CONFIG*` are not
  set at workflow level or in that job; `vendor` has `needs: changes` and
  either no `if:` or exactly `if: ${{ needs.changes.outputs.fleet == 'true' }}`
  (see "Merge-queue runs");
- **(fleet-scope)** in `pipeline-pr.yml`: the `changes` job holds the step
  `Decide the fleet scope` right after the merge-queue guard step (so before
  the path-filter diff), in exactly its spec text (comment and blank lines
  aside): it decides `merge_group` and `workflow_dispatch` inline and reads
  `tools/ci/pr_tier_scope.sh` from `origin/${DEFAULT_BRANCH}`. The job
  publishes `fleet` and `heavy` from that step, and no other line names the
  script (see "CI_SCOPE: the fleet scope of the PR tier");
- **(event-guard)** a reusable workflow with a fleet job holds the hosted
  `event-guard` job in its spec form, with the per-pipeline `ALLOWED` and
  `ALLOWED_REFS` lists (for `pipeline-full.yml`: `push workflow_dispatch
  pull_request schedule`, and no other event), and every fleet job reaches it
  through `needs` (see "Merge-queue runs");
- **(yaml-alias)** no YAML anchor, alias or `<<:` merge key on a job or a job-level
  key other than `steps:` and `env:`.

The gate is line-oriented: `runs-on` and the guard must each fit on one
line. Fixtures live in `tools/ci/testdata/workflow-shape/`; run
`tools/ci/test_check_workflow_shape.sh` after changing the gate.

## Runner group policy entries

The fleet runner group admits only the workflows listed in its
"selected workflows" policy; a job from any other workflow file or ref
queues and never starts. The entries, one per reusable workflow that can
reach the fleet:

```text
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-pr.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-full.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-full.yml@refs/heads/ci-dev
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-nightly.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-optimizer.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-nuget.yml@refs/heads/main
We-Amp/pagespeed-optimizer/.github/workflows/pipeline-pr.yml@refs/heads/ci-dev
```

`pipeline-release-smoke.yml` runs on hosted runners only and needs no
entry. Callers (`ci.yml`, `nightly.yml`, `ci-dev.yml`, `release.yml`,
`release-smoke.yml`) never get one. Add an entry in the same change that
gives a reusable workflow its first fleet job.
