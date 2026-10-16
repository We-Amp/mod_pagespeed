<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# CI pipelines

This repository's CI is split into thin **caller** workflows and
**reusable** workflows. Jobs that need a lot of CPU run on a fleet of build
runners that are reachable only through this repository's reusable workflows
at `refs/heads/master` (and `refs/heads/ci-dev`). A pull request therefore
cannot put its own workflow code on one of those runners: whatever it
changes in `.github/workflows/`, the jobs that reach them are the ones
already on `master`.

The pipeline is being moved over in steps. This document describes the
shape and the tooling; the sections on tiers and required checks say which
jobs exist today.

## Tiers

| Tier | Workflow | Runs on | Holds |
|---|---|---|---|
| PR | `pipeline-pr.yml`, called by `ci.yml` | every pull request and push to `master` | the hosted checks: the guards group (test placement, pytest exit status, bazelrc language flags, `git_repository` vendoring, the release-note gate, legacy docs, console drift, the SBOM, notices and VEX gates with a grype scan), the blocking and the report-only dependency scans, the browser and console tests, the repository signing key guard, the serf removal guard, the C++ CVE completeness gate and per-pull-request snapshot scan, and the plan half of the upgrade rehearsal; and the build runner legs listed under "Jobs of the PR tier" below. |
| Full | `pipeline-full.yml`, called by `ci.yml` | push to `master`, `workflow_dispatch` with `full: true`, or a pull request labelled `ci:full` | its own vendor and Linux x64 build; the sanitizers (ASan with UBSan, TSan); the Apache and nginx system tests; the Windows build of the IIS module, the Windows ASan unit tests and the Windows Application Verifier run; the Linux arm64 build, unit tests and system tests on hosted arm64 runners. See "The full tier" |
| Nightly | `pipeline-nightly.yml`, called by `nightly.yml` | 03:30 UTC and `workflow_dispatch` | the sanitizer stress rigs (Apache, nginx, IIS), the Apache daemon lane, the nginx protocol matrix, the full-tree lint, the four fuzz lanes, the IIS Application Verifier matrix, the NVD scan and the nightly dependency scan (see "The nightly tier") |
| Release | `pipeline-release-module.yml`, called by `release.yml` | a pushed tag `module-v*`, or `workflow_dispatch` with a tag and `dry_run` | the module release lane: build, package, smoke, the Windows installer, the release gates and validation, ending in one run artifact with the asset set, `SHA256SUMS` and `manifest.json`; a real run also signs and creates the release, in hosted jobs of the `release` environment. A dry run signs nothing and uses no secret. See `docs/ci/release.md` |
| Sweep | `pipeline-sweep.yml`, called by `sweep-queued.yml` | hourly and on dispatch | cancels runs that wait for a runner nobody serves (see "The queued-run sweep") |

The license audit (Apache RAT), the public-tree hygiene gate, the workflow-shape
gate and the CI tooling self-tests run inline in `ci.yml`, on hosted runners.

The parity guard of the former pipeline is not carried over: the corpus it
compared against is not part of this repository.

The `ci:full` label is read when a pull request run starts (open, push,
reopen). After adding the label, push or re-run the workflow to get the full
tier.

### Jobs of the PR tier

| Job | Class | Needs | What it does |
|---|---|---|---|
| `changes` | hosted | | path filters (see below) |
| `guards`, `blocking-dep-scan`, `dep-scan`, `browser-js-tests`, `console-tests` | hosted | | the hosted checks |
| `signing-key`, `serf-removed`, `cpp-cve-completeness`, `cpp-cve-snapshot-scan`, `upgrade-rehearsal-plan` | hosted | `changes` | the path-filtered hosted checks |
| `upgrade-rehearsal` | hosted | `upgrade-rehearsal-plan` | previous release to the version under test, per distro, in a booted systemd container (hosted because it needs a privileged container, which a build runner never runs); reads the public release assets only |
| `vendor` | `fleet-heavy` | | vendors the dependencies; publishes the tarball (hub channel) or caches `vendor/` (cache channel) |
| `linux-build` | `fleet-heavy` | `vendor` | builds both server modules and the test targets in the dev image, checks the modules' symbols, uploads the `ci-artifacts-linux-x64` bundle |
| `linux-unit-tests` | `fleet-heavy` | `vendor`, `linux-build` | unit tests against throw-away Redis and memcached containers |
| `lint` | `fleet-heavy` | `vendor` | clang-tidy (scoped to the pull request) and clang-format |
| `js-minify-corpus` | `fleet-heavy` | `vendor` | JS minifier oracles and goldens freshness gate |
| `html-parse-corpus` | `fleet-heavy` | `vendor` | HTML parse goldens freshness gate |
| `fuzz-smoke` | `fleet-heavy` | `changes`, `vendor` | CSS, HTML, rule-pack and JS fuzz harnesses: deterministic replay plus 60 s libFuzzer runs (path-filtered) |
| `windows-build` | `fleet-windows` (`windows-2022` for forks) | `vendor` | builds the IIS module with clang-cl, uploads the `ci-artifacts-win-x64` bundle |
| `windows-unit-tests` | `fleet-windows` (`windows-2022` for forks) | `vendor`, `windows-build` | Windows unit tests |
| `iis-sys-tests` | `fleet-iis`, no hosted side | `windows-build`, `windows-unit-tests` | the IIS system tests against the module from the `windows-build` bundle, on the host's full IIS. Skipped (not failed) for fork and Dependabot pull requests and while `FLEET_ENABLED` is not `true`; carries the "only" guard below |

Application Verifier and the other IIS lanes that run longer belong to the
later tiers. `iis-sys-tests` clears stale verifier settings before the run and
again afterwards, arms Windows Error Reporting full dumps for the worker, and
on a failure ships an evidence artifact that includes this run's staged
crash dumps. It blanks the guest password for the whole job, because it runs
pull request code with administrator rights.

### Release gates on the `fleet-iis` class

Two reusable workflows hold the jobs that need a Hyper-V guest. They are not
part of the pull request tier: the release lane calls them after the hosted
job that builds the installer and the cPanel packages, and a gate failure
stops the release.

| Workflow | Job | Inputs | What it does |
|---|---|---|---|
| `gate-iis-msi-upgrade.yml` | `iis-msi-upgrade-test` | `msi_artifact`, `old_msi_artifact` (optional), `release_tag` | restores a clean IIS guest checkpoint, installs the MSI (a clean install, or an upgrade over the older MSI), smoke-tests and uninstalls it. A missing guest, name or password fails the gate |
| `gate-cpanel-smoke.yml` | `cpanel-smoke` | `os`, `rpm_artifact`, `release_tag` | restores a clean cPanel guest checkpoint for one operating system, installs the package and smoke-tests it over ssh. Skips with a warning when that operating system has no guest yet, and on prerelease tags |

Both declare one output, `ran`: the string `true` only when the test step
actually executed (it passed or failed), and `false` on every other path (a
missing guest or checkpoint, a prerelease tag, an early exit, a cancelled or
failed earlier step). The release lane requires `ran == 'true'` from each gate
in a real run and treats a missing output as `false`, so a skip can never
count as a pass.

Both use the "only" guard and read their guest names and the guest password
from the runner environment (`docs/ci/runner-environment.md`); neither holds a
secret or a write token. The MSI is built on a hosted runner and handed over
as an artifact, so the fleet host never holds a signing key.

The `CI gate` in `ci.yml` consumes only the result of the whole `pr` call, so a
job added here is covered without touching the gate.

On the Linux legs the Bazel disk cache lives in a per-runner directory under
the runner's home, mounted at `/cache` (empty on a hosted runner). Containers
run as the runner's own user with `HOME` and `USER` set, so nothing root-owned
lands in the checkout (the system-test jobs use uid 1000 and hand it the
workspace when the runner itself is root); no job logs in to a registry. Every Linux job that
checks out a tree starts with a step that makes `vendor/` writable again,
because a previous run's vendored toolchain files are read-only and would stop
the checkout's cleanup on a persistent runner.

### Path-filtered hosted guards

The `changes` job diffs a pull request against its base and publishes one
boolean per filter (`signing_key`, `serf`, `cve`, `upgrade`, `fuzz`). The jobs that only
matter when their inputs change run when their filter is true, and are skipped
otherwise; outside a pull request every filter is true. The aggregate gate
treats a skipped job as fine. A build runner job that depends on a filter
narrows its guard as described under "The guard and the kill switch".

## The shape

| Kind | Triggers | Holds | Example |
|---|---|---|---|
| Caller | `pull_request`, `push`, `schedule`, `workflow_dispatch` | triggers, `concurrency`, `permissions`, jobs that only `uses:` a reusable workflow `@master` or `@ci-dev`, and at most an aggregate gate job on a hosted label | `ci.yml`, `ci-dev.yml`, `nightly.yml`, `sweep-queued.yml`, `release.yml` |
| Reusable | `workflow_call` only | every job, with its `runs-on` | `pipeline-pr.yml`, `pipeline-nightly.yml`, `pipeline-sweep.yml`, `pipeline-release-module.yml` |

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

`FLEET_IIS_ENABLED` is a second repository variable for the jobs of the
`fleet-iis` class, which have no hosted twin. Each of them also requires it to
be `true`, so they are skipped (not queued) until a runner of the class is
registered; the `CI gate` accepts the skipped result, and a real module
release fails closed at `validate` while the gates are skipped.

Three more guards exist for jobs that cannot fall back to a hosted runner, and
the gate accepts exactly these four. **Only** is for a job that mounts the
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

**fleet-iis** is for a job routed to the `fleet-iis` class, which has no
hosted twin: the fleet-only guard plus the `FLEET_IIS_ENABLED` term. The gate
requires it, and no other guard, on every job routed to that class.

```yaml
if: ${{ vars.FLEET_ENABLED == 'true' && vars.FLEET_IIS_ENABLED == 'true' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}
```

A job that needs more conditions (for example a path filter) appends them
together with a matching gate rule; the gate rejects any routed job whose
`if:` is not exactly one of the four guards, or the first one narrowed by
`&& needs.changes.outputs.<name> == 'true'`. A job that runs on a hosted label
only (hosted arm64, report jobs, the checks that need nothing special)
carries no guard.

### Degraded mode on hosted runners

A job routed by the expression must also work on a hosted runner: no hub, no
remote cache, no host paths. Anything host-specific is read from the runner
environment (`docs/ci/runner-environment.md`) and used only when set. When
none of it is set the job runs slower, and never fails for that reason.

### Hosted twins and joins

A release lane must not stop when the kill switch is off, so each of its build
runner jobs has a hosted twin with the same steps (`<job>-hosted`, carrying
`if: ${{ vars.FLEET_ENABLED != 'true' }}`). Exactly one of the pair runs. The
`if:` of a build runner job is the guard alone, so such a job cannot name the
result of the job it depends on; a small hosted `<name>-ready` job joins the
pair (it succeeds when one of the two legs did) and the dependent jobs `need`
the join. GitHub skips a job when any ancestor job was skipped, so those
dependents state their status check themselves: a build runner job's `if:` is
`${{ !cancelled() && needs.<job>.result == 'success' && (<fleet guard>) }}`
(the workflow-shape gate accepts that form), a hosted one omits the guard. Steps shared by the two jobs, and by an arm64 job, are written once
with a YAML anchor.

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
days for bundles and evidence. The release lane names its artifacts for what
they are instead of by tier: `asset-*` hold release files, and the others
(`tree`, `stock-el8`, `testdata-*`, `debug-*`, `unsigned-windows`) are inputs of
later jobs (`docs/ci/release.md`).

## The nightly tier

`nightly.yml` holds the schedule (03:30 UTC) and `workflow_dispatch`, takes
the run-wide `nightly` concurrency group (one nightly at a time, never
cancelled) and calls `pipeline-nightly.yml@master`. Its inputs reach the
jobs as strings and every one is validated before use: `fuzz_seconds` and
`duration` (1 to 1800 seconds), `concurrency`, `protocols`, `arms` and
`severity`.

Nothing in this tier writes to GitHub and nothing reads a secret. Both the
caller and the called workflow hold `contents: read` only, which is why the
tier has no report jobs: a lane that finds something goes red, writes the
job summary and uploads its evidence (kept three days, seven for the IIS
matrix). Opening a tracking issue for a finding, or proposing the refreshed
vulnerability snapshot as a pull request, needs a write token and belongs to
a separate hosted bot job with its own token in a protected environment; it
is not part of this tier.

Fuzz crash reproducers and corpora are uploaded as workflow artifacts of a public repository, so treat them as public evidence for three days: anyone who can view the run can download them. A crash that is also a security problem should be reported privately and its evidence not left to the schedule; delete the artifact from the run if needed.

| Job | Class | What it does |
|---|---|---|
| `nightly-vendor` | fleet-heavy | vendors the dependencies once (`tools/vendor-deps.sh`) and hands the tree to every other job through a vendor tarball or `actions/cache`, as described under "Artifacts" |
| `apache-asan-stress` | fleet-heavy, fleet only | ASan module under graceful reload and restart load in the Apache rig; the sweep step is the gate |
| `apache-daemon-lane` | fleet-heavy | the Apache system tests with the daemon lane required |
| `nginx-asan-stress` | fleet-heavy, fleet only | the same for the nginx module |
| `nginx-protocol-matrix` | fleet-heavy, fleet only | the proxy-cache race smoke over HTTP/1.1 and HTTP/2 (`protocols` input to change the list) |
| `full-tree-lint` | fleet-heavy, three shards | clang-tidy over the whole tree, split into three matrix shards by a hash of the file path, the backstop for what the pull request lint's include graph cannot see |
| `fuzz-css`, `fuzz-html`, `fuzz-packs`, `fuzz-js` | fleet-heavy | an ASan replay floor plus a bounded libFuzzer campaign per surface; corpora persist across nights in `actions/cache`; the JS lane classifies its artifacts and fails only on needs-triage ones |
| `iis-asan-systest` | fleet-iis | ASan build of the IIS module under recycle-under-load stress |
| `iis-appverif-matrix` | fleet-iis | the zero-copy merge gate, then the Application Verifier provider matrix (`arms` input) |
| `nvd-scan` | hosted | rebuilds the vendored-dependency CVE snapshot from the live feed and uploads it with the report; no API key, so a rate-limited run skips instead of failing |
| `dep-scan-nightly` | hosted | the live-database dependency scan with the image scan mode, and the pin staleness check |

**Where the pin staleness check lives.** The blocking dependency gate of the
pull request tier scans against the pinned grype database in
`tools/sbom/grype-db-pin.json`. `dep-scan-nightly` runs
`tools/sbom/provision-grype-db.sh --enforce-age`, which fails the job once the
pin is older than its `max_age_days`. It is deliberately a scheduled check: a
forgotten pin reddens the nightly and never someone's pull request. Bumping
the pin is a reviewed change to the pin file.

**The image scan mode.** `dep-scan-nightly` runs `tools/sbom/dep-scan.sh
--images`, which scans the package-test images from the local container
cache. A hosted runner has none, so on its own those rows read "not built":
the scan is honest about it, and "no image findings" there means "no
images", not "no vulnerabilities". Findings never fail that step; the pin
age does.

The Linux jobs run in the dev image as the host user. Each starts one named
container (`nightly-<job>-<run id>-<attempt>`), runs its steps in it with
`docker exec` so a test rig's processes survive from step to step, and
removes it by that name in its last step. The container never mounts the
container socket and is never privileged. The IIS jobs run on one Windows
host at a time because the fleet-iis class has one runner instance per host.

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

## The full tier

`pipeline-full.yml` is self-contained: it vendors and builds for itself
instead of waiting for the PR tier, so the two run side by side. Its
artifacts carry the `full-` prefix. The IIS system tests (the PR tier's IIS
legs) are not part of it.

| Job | Class | Guard | Notes |
|---|---|---|---|
| `vendor` | `fleet-heavy` | general | vendors the dependency tree; publishes it through the hub when the runner environment provides one, else `actions/cache` |
| `linux-build` | `fleet-heavy` | general | builds both server modules and the test targets, runs the module symbol checks, uploads the module bundle |
| `linux-asan` | `fleet-heavy` | general | ASan, UBSan and LeakSan over the unit tests, with redis and memcached on a per-run network |
| `linux-tsan` | `fleet-heavy` | general | TSan over the same tests (`seccomp=unconfined` for TSan's address-space layout, all capabilities dropped) |
| `apache-sys-tests` | `fleet-light` | general | runs the built module in the dev image with only the low-port capability; failure diagnostics are uploaded |
| `nginx-sys-tests` | `fleet-light` | general | the same for the nginx module |
| `windows-build` | `fleet-windows` | general | release build of the IIS module; its bundle feeds `win-appverif` |
| `win-asan` | `fleet-windows` | general | Windows ASan build and unit tests; skips with a notice when LLVM 19 with its sanitizer runtime is not found |
| `win-appverif` | `fleet-iis` | only | Application Verifier, full page heap and concurrency fuzzing on the IIS worker process. Needs the IIS role, administrator rights and the Windows SDK debugging tools, so it has no hosted side and a fork or Dependabot pull request skips it |
| `linux-arm64-build`, `linux-arm64-unit-tests`, `linux-arm64-apache-sys-tests`, `linux-arm64-nginx-sys-tests` | hosted (`ubuntu-24.04-arm`) | none | the arm64 build vendors for itself into `actions/cache` and uploads its module bundle; the other three reuse the vendored tree, the Bazel disk cache and the bundle |

Notes on the full tier:

- Sanitizer jobs wait for `linux-build` (not only for `vendor`), so they never
  compete with the build for a heavy slot.
- Containers run as the host user. The job supplies a passwd file that names
  that user `dev`, the name the dev image's sudo rules know. No job mounts the
  container socket or runs a privileged container.
- With none of the runner-environment variables set (a fork or Dependabot pull
  request, or a runner without a cache), every general-guard job runs on a
  hosted runner without the remote cache and without host paths.
- The hosted arm64 jobs are in this tier and not in the PR tier because a cold
  hosted arm64 build has no warm cache. Promote them after a timing run.
- A failed system-test run uploads one diagnostics tarball as an artifact;
  that collection and upload are best effort and cannot change the job result.
- Stale Windows runner state matters to `win-appverif`: Application Verifier
  and page heap settings for the worker process are machine-global, so the job
  clears them before arming and again at the end, even on failure.

## The release ref guard

Release pipelines start with the `ref-guard` action
(`.github/actions/ref-guard`, logic in `tools/ci/ref_guard.sh`). It refuses
any ref but `master` and release tags whose commit is on `master`, and any
event outside the allowed list; a manually dispatched release may only name a
tag that already exists and is reachable from `master`. The allowed tag glob
is a parameter (`module-v*` for the module lane). The action runs after a
checkout of `master` with full history and tags, so the guard code is never the
tree being released.
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

## Required checks

A check from a called workflow is named `<caller job> / <job name>`, so after
the caller switch the required checks are: `pr / Guards (security gates,
console drift, placement, pytest exit, bazelrc, git_repository, PR policy)`,
`pr / Blocking Dep Scan (npm)`, `pr / Browser client-JS tests`,
`pr / Console SPA tests`, the inline `License audit (Apache RAT)`,
`Public-tree hygiene gate` and `CI tooling self-tests`, and `CI gate`.

The full tier's jobs are named `full / <job name>` (for example
`full / Linux Build`). They run only on a push to `master`, a dispatch with
`full: true` or a labelled pull request, so none of them is a required check;
`CI gate` covers them whenever they run.

Not listed individually: `pr / Dependency Scan (report-only)` (it never fails
the build) and the path-filtered jobs (`pr / Repo signing key embedded`,
`pr / Serf stays removed`, `pr / Completeness gate (every C++ dep annotated)`,
`pr / CVE scan vs committed snapshot (per-PR, offline)`,
`pr / Upgrade rehearsal plan (version + distro matrix)`, `pr / Fuzz smoke (deterministic replay plus 60s libFuzzer)`), which are skipped on
a pull request that does not touch their paths. `CI gate` treats `skipped` as
fine and fails on any other non-success result, so it covers all of them.
Switch the required checks in the repository ruleset in the same change that
merges the caller.

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

## The public-tree hygiene gate

`tools/ci/check-public-hygiene.sh` runs in the `Public-tree hygiene gate` job
in two passes over the tracked tree. The first pass applies
`.github/hygiene-rules.txt` (private vocabulary and identifiers). The second
pass applies `.github/hygiene-credential-rules.txt` and refuses credential
literals:

- **securestring-literal**: PowerShell `ConvertTo-SecureString` with a quoted
  string literal and `-AsPlainText` (any argument order, or via the pipeline).
  A variable or `$env:` argument passes.
- **pscredential-literal**: `PSCredential(` or `New-Object ... PSCredential
  -ArgumentList` with a quoted literal password argument, and the `-Password`
  cmdlet parameter with a quoted literal (PowerShell files).
- **secret-assignment**: a name ending in `password`, `passwd`, `secret`,
  `token`, `api_key` or `apikey` (any case; `ADMIN_PASSWORD`, `$AdminPassword`
  and `GITHUB_TOKEN` count, `password_file` does not) assigned a quoted string
  of 8 or more characters, in shell, PowerShell, YAML, Python and JSON files.
  Obvious placeholders pass; they are anchored `placeholder` entries in the
  rules file (`changeme`, `example`, `placeholder`, `dummy`, `test`,
  `replace` optionally followed by a separator and more text, runs of `x`,
  `<...>`, `%...%`, `your-...`, `secret` itself, `${{ ... }}`) plus variable
  references in double quotes (`${NAME}`, `$env:NAME`, `$NAME`). The gate
  refuses empty entries and fails if the set matches a canary value.
- **private-key-header**: `-----BEGIN ... PRIVATE KEY-----` headers.
- **token-shape**: GitHub (`gh[pousr]_` and `github_pat_`), AWS (`AKIA`),
  Slack (`xox[baprs]-`) and Google (`AIza`) token shapes.

The report names `path:line` and the rule class, never the matched text.

Allowlisting, for fixtures that intentionally hold fake secrets only: add an
`allow-path` entry (with a comment saying why) to the
credential rules file (the exact path of one tracked file: no glob
characters, no directory; the gate refuses anything else and lists the active
entries in its output), or put the marker `hygiene-allow-credential` on the
line itself, for example in a trailing comment.

Known gaps: unquoted YAML values, `.env` files and Dockerfile `ENV` lines are
not scanned, and a secret's NAME in a Kubernetes or Helm value (for example
`secret: "some-secret-name"`) can be a false positive; the line marker is the
escape hatch. File names containing a colon break the `path:line` report. The
self-test for this pass is
`tools/ci/test_check_public_hygiene.sh`; it builds its fixtures at run time.

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
  runner class, and a job routed by the expression carries one of the four
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
We-Amp/mod_pagespeed/.github/workflows/pipeline-full.yml@refs/heads/master
We-Amp/mod_pagespeed/.github/workflows/pipeline-nightly.yml@refs/heads/master
We-Amp/mod_pagespeed/.github/workflows/pipeline-release-module.yml@refs/heads/master
```

The release lane has no `ci-dev` entry: it only runs from `master`.

The two gates above need an entry each, in the same form, in the change that
first calls them from a release workflow:

```text
We-Amp/mod_pagespeed/.github/workflows/gate-iis-msi-upgrade.yml@refs/heads/master
We-Amp/mod_pagespeed/.github/workflows/gate-iis-msi-upgrade.yml@refs/heads/ci-dev
We-Amp/mod_pagespeed/.github/workflows/gate-cpanel-smoke.yml@refs/heads/master
We-Amp/mod_pagespeed/.github/workflows/gate-cpanel-smoke.yml@refs/heads/ci-dev
```

Callers (`ci.yml`, `ci-dev.yml`, `nightly.yml`, `sweep-queued.yml`, `release.yml`) never get an entry.
