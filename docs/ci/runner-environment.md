<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# Runner environment contract

Jobs that run on the build runners (see `docs/ci/pipelines.md`) may use
facilities that exist only on a particular runner: a shared artifact hub, a
remote Bazel cache, a fast local vendor directory, a Windows toolchain
directory. None of that is written into a workflow or a script in this
repository. A runner offers it through environment variables in its own
environment (for the GitHub Actions runner, its `.env` file), and the jobs
use each one only when it is set.

**The rule: every variable is optional. With none of them set, a job must
still run, slower.** A job that needs a variable to succeed is a bug. Values
are never echoed to the job log, and no value appears in this repository. This
page lists names and meanings only.

## Artifact hub and caches

| Variable | Platform | What it enables when set | Without it |
|---|---|---|---|
| `CI_HUB_SSH` | Linux, Windows | ssh target (`user@host`) of the shared artifact hub. Together with `CI_HUB_VENDOR_DIR` the vendor job publishes its tarball there and consumers on other runners fetch it | the vendored tree passes between jobs through `actions/cache` |
| `CI_HUB_USER`, `CI_HUB_ADDR`, `CI_HUB_HOST`, `CI_HUB_IP` | Linux, Windows | the older split form of the ssh target: user, then an address, or a name and a fallback address probed by `tools/ci/resolve-cache-host.sh`. `CI_HUB_SSH` wins when both are set | as above |
| `CI_HUB_VENDOR_DIR` | Linux, Windows | directory on the hub for vendor tarballs (needs an ssh target) | as above |
| `CI_HUB_VENDOR_RETENTION_MIN` | Linux | minutes the vendor job keeps vendor tarballs on the hub before pruning them | 1440 |
| `CI_HUB_ARTIFACTS_ROOT` | Linux | directory on the hub for build bundles, read by `tools/ci/stage-artifacts.sh` when a local bundle is missing (needs an ssh target) | bundles pass through `actions/upload-artifact` and `actions/download-artifact` only |
| `CI_VENDOR_DIR` | Linux | runner-local directory for vendor tarballs; consumers on the same host extract from it without a network fetch | no local fast path |
| `CI_WIN_VENDOR_DIR` | Windows | the same, for Windows runners: a directory that may hold the vendor tarball the Linux jobs of the same machine published; the Windows jobs read it and `tools/ci/prune-win-vendor.ps1` never prunes it | no local fast path (a hub fetch goes to a per-profile download directory) |
| `BAZEL_CACHE_HOST` | Linux | address of the remote Bazel cache; jobs add a host mapping to their build containers and build with the cache configuration | builds use the plain container configuration with no remote cache |
| `BAZEL_REMOTE_CACHE_URL` | Windows, macOS | full remote cache URL, passed as `--remote_cache` | no remote cache |
| `CI_DEV_IMAGE_LOCK_DIR` | Linux | directory for the per-host lock files `tools/ci/ensure-dev-image.sh` takes around image builds (useful when several runner instances share one container daemon) | locks go to the temporary directory |
| `CI_IMAGE_LOCK_TIMEOUT` | Linux | seconds a job waits for another build of the same image tag on the host before building anyway | 1200 |

## Windows toolchain and caches

| Variable | What it provides | Without it |
|---|---|---|
| `CI_WIN_TOOLS_DIR` | `;`-separated list of directories added to `PATH` ahead of everything else. Together they may provide `bazelisk` (or `bazel`), `bash.exe`, `cmake`, `ninja`, `zstd`, the pinned NASM and, when the runner is not a hosted image, WiX and the .NET SDK. Visual Studio is found with `vswhere` | tools come from `PATH`; bash from MSYS2 or Git for Windows in their usual locations |
| `BAZEL_SH` | the bash Bazel runs genrules with | MSYS2's bash, else Git for Windows' bash |
| `CI_WIN_BAZEL_OUTPUT_ROOT` | Bazel `--output_user_root` for every Windows job (a short path on a volume with room) | `C:/b` |
| `CI_WIN_CACHE_DIR` | directory for the Bazel disk cache of Windows jobs | the cache lives under the job's temporary directory and is lost with it |
| `CI_WIN_CACHE_MAX_SIZE` | size cap for that cache (`N[K|M|G|T]`, Bazel's size syntax); a runner that keeps the cache sets this, and each job enforces it at its start (see below) | no cap is applied by the job |
| `CI_WIN_LLVM_DIR` | install directory of LLVM 19 (`clang-cl` and the AddressSanitizer runtime library directory) | `clang-cl` from `PATH`; sanitizer jobs skip with a notice when the runtime is not found |
| `CI_WIN_PYTHON_DIR` | directory of Python 3.12 with pip, placed ahead of Git for Windows' python stub | `python` from `PATH` |
| `CI_WIN_DUMP_DIR` | directory for crash dumps (Windows Error Reporting local dumps); the scripts read it as `PAGESPEED_DUMP_DIR` | a directory under the job's temporary directory |

The size cap is enforced by the job itself, not by Bazel. Bazel collects a
disk cache (`--experimental_disk_cache_gc_max_size`) only from its server,
only after the server has been idle for a while, and a `bazel shutdown` or a
server exit cancels the collection; the next Windows job on a runner shuts
the previous server down first, and Bazel 8 cannot run the collection
synchronously, so the flag alone never frees anything. Each Windows job
therefore runs `tools/ci/Prune-WinDiskCache.ps1` right after that shutdown,
when nothing holds the directory. It removes the files under the cache's
`ac/` and `cas/` directories with the oldest last-write time first (an
action-cache entry before a content blob of the same time) until the rest
fits `CI_WIN_CACHE_MAX_SIZE`, never follows a reparse point, never touches
anything else under `CI_WIN_CACHE_DIR`, and logs the size before and after.
Without the variable nothing is removed. A file that cannot be removed is a
warning, never a job failure. The nightly ASan and Application Verifier
builds keep their own caches next to `CI_WIN_CACHE_DIR` (`-asan` and
`-appverif` suffixes) and prune those the same way. The cap applies to each
directory separately. `tools/ci/Prune-WinDiskCache_test.ps1` is the script's
hermetic self-test; the pull-request Windows jobs run it.

Tool expectations that have no variable: NASM is pinned to 2.16.03 (the
image-codec dependency rejects 3.x), so the Windows jobs install that version
into the runner's temporary directory when `PATH` does not provide it; Visual
Studio 2022 with the C++ workload; `zstd` and `scp` for the hub channel only.
Signing tools (`signtool`, the trusted-signing client) are a hosted-job
concern and are not required of a build runner. ProcDump is expected on
`PATH` by the IIS AddressSanitizer rig; Application Verifier (`appverif`)
comes with the Windows SDK and is expected on `PATH` by the AppVerifier lanes.
On a hosted Windows runner gperf comes from the image's MSYS2, installed with `pacman`.

## IIS and virtual machine runners (classes `fleet-iis` and `fleet-hyperv`)

Jobs of these classes have no hosted twin. They run only for events of this
repository: a fork or Dependabot pull request skips them (the jobs show as
skipped, not failed), and so does every run while `FLEET_ENABLED` or
`FLEET_IIS_ENABLED` is not `true`. `FLEET_IIS_ENABLED` is a second repository
variable: set it to `true` once a runner of each class is registered, so the
jobs are skipped rather than queued while a class does not exist. A job
of either class builds and runs the pull request's own code on a host with
administrator rights. The `fleet-iis` class means a Windows host with these
facts, which are not variables:

- the IIS role is installed and the job manages it (sites, application pools,
  `w3wp`, `appcmd`); the runner service account is a local administrator;
- the tests own the test ports and the default web site on the host, so two
  jobs of this class must not run on one host at the same time;
- the Windows toolchain of the `fleet-windows` class is present as far as the
  job needs it: Visual Studio 2022 (found with `vswhere`; a host without it
  only warns on the IIS system tests, which run a module that is already
  built), Python 3.12 with pip, and `PATH` entries from `CI_WIN_TOOLS_DIR`;
- Application Verifier (`appverif`, from the Windows SDK) and `gflags` are
  available; the jobs call them only to clear stale settings and tolerate
  their absence.

The installer-upgrade and cPanel release gates run on a separate class,
`fleet-hyperv`: a Windows host with the Hyper-V role and the test guests with
their clean checkpoints. It follows the same rules as this class (events of
this repository only, the same two repository variables; two jobs of this
class must not run on one host at the same time) and needs:

- for the installer gate: the Hyper-V role and PowerShell Direct to the
  guests (the installer itself is built on a hosted runner, so WiX is not
  needed);
- for the cPanel gate: guests per target operating system, reachable by ssh
  from the host with keys that stay on the host.

| Variable | What it provides | Without it |
|---|---|---|
| `CI_WIN_TOOLS_DIR`, `CI_WIN_PYTHON_DIR` | as in the Windows section above | as above |
| `CI_WIN_DUMP_DIR` | directory for crash dumps of the IIS worker; the IIS system tests arm Windows Error Reporting full dumps into it, stage this run's dumps into the failure evidence, and remove them afterwards. The scripts read it as `PAGESPEED_DUMP_DIR` | a directory under the job's temporary directory |
| `CI_IIS_VM_NAME` | name of the Hyper-V guest that holds a clean IIS installation, for the installer upgrade gate | the gate fails loudly (never a false green) |
| `CI_IIS_VM_SNAPSHOT` | name of the clean checkpoint the gate restores | the script's default checkpoint name is used |
| `CI_IIS_VM_ADMIN_PASSWORD` | the guest Administrator password, read by `test/system/iis/ci-msi-upgrade-test.ps1` from the runner's environment, never a parameter, never written in the repository and never printed. Set it in the runner's `.env` file. | the gate fails with a message that names the variable |
| `CI_EA4_VM_<OS>` | name of the cPanel guest for one target operating system (for example `CI_EA4_VM_EL9`); the clean checkpoint follows a fixed naming convention per operating system | a conventional guest name is tried; when no guest exists the smoke for that operating system skips with a warning annotation |

The guest variables above (`CI_IIS_VM_NAME`, `CI_IIS_VM_SNAPSHOT`,
`CI_IIS_VM_ADMIN_PASSWORD` and `CI_EA4_VM_<OS>`) belong only to a `fleet-hyperv`
host; an IIS-only host in the `fleet-iis` class must not carry them.

The IIS system tests (a pull request job) need none of the guest variables.
The two guest gates are release-lane building blocks (see
`docs/ci/pipelines.md`).

## Linux additions

- A registry mirror is configured in the container daemon of the host. No
  workflow logs in to a registry, and no registry credential is available to
  a job.
- `CI_BUILD_RAM_BUDGET_GB` (and a core count variable where a lane needs one)
  tells a memory-hungry leg how much of the host it may use; without it the
  leg uses a conservative default.
- Runners for the heavy and light classes run containers as root inside an
  unprivileged daemon namespace. No job mounts the container socket, and none
  runs `--privileged`; the system tests add only the capabilities they name.
- Native arm64 jobs run on hosted arm64 runners; there is no arm64 class.

Tools the Linux jobs expect on `PATH`: `docker`, `bash`, `git`, `tar` with
zstd support, `rsync` and `ssh` (hub only), and `bazel` or `npm` (the vendor
job installs bazelisk through npm when `bazel` is missing). Hosted runners
provide all of them.

A `bazel` or `bazelisk` launcher on `PATH` (Linux or Windows, including one
provided through `CI_WIN_TOOLS_DIR`) must run the version in
`.bazelversion`, currently 8.8.1. Bazelisk must be 1.19.0 or newer: older
releases find `.bazelversion` only through a WORKSPACE file and, without
one, silently run the latest Bazel instead. A plain Bazel
binary must be exactly that version. Every job that uses the host's launcher
checks this first (`tools/ci/check-bazel-version.sh`,
`tools/ci/Check-BazelVersion.ps1`) and fails naming the runner. The dev
images install their own Bazelisk (`docker/Dockerfile`).

## Per-slot resource budget

A host runs several heavy jobs at once, so each runner states its own share in
its environment file:

| Variable | Meaning | Used for |
|---|---|---|
| `CI_SLOT_CPUS` | positive integer | Bazel `--jobs`, `run-clang-tidy -j`, nginx release leg fan-out, `make` jobs in the module and toolchain builds |
| `CI_SLOT_MEM_GB` | positive integer (GiB) | container `--memory` and `--memory-swap` |

Both are optional. Unset (hosted runners) keeps the previous behaviour: Bazel
sizes itself from the host CPU count and each container keeps its fixed cap
(`CI_BUILD_RAM_BUDGET_GB` or 16 for the nightly lanes, 50g to 80g elsewhere;
the lint container has none). With both set, `run-clang-tidy` is limited to one
process per 2 GiB as well. A set but invalid value fails the job
with a clear message. The logic lives in `tools/ci/slot-budget.sh` (self-test:
`tools/ci/test_slot_budget.sh`). Size the values so that all slots of a host at
full use fit in the memory available to the runner services.

Anything that chooses a parallelism inside a job container must read the slot
budget rather than the host CPU count, because `nproc` inside a container still
reports every CPU of the host. Read `${CI_SLOT_CPUS:-$(nproc)}` and pass it as
`make -j`, `cmake --build -j`, `--parallel`, `xargs -P` or the tool's own worker
flag; the `nproc` fallback keeps developer machines unchanged. The job's `docker
run` must pass `-e CI_SLOT_CPUS` (or `MAKE_JOBS`) for the script to see it.

## Adding a variable

1. Give it a neutral name that says what it is, not where it lives.
2. Read it with a default (`${NAME:-}`) and branch on it being set; state the
   degraded behaviour in the job or the script next to the branch.
3. Never print its value; print what it enables ("using the remote cache").
4. Add a row to the table above.
