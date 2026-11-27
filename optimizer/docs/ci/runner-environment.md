<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

# Runner environment contract

Fleet jobs (see `docs/ci/pipelines.md`) may use facilities that exist only on
a particular runner: a shared artifact hub, a remote Bazel cache, a fast
local vendor directory. None of that is written into a workflow or a script
in this repository. A runner offers it through environment variables in its
own environment (for the GitHub Actions runner, its `.env` file), and the
jobs use each one only when it is set.

**The rule: every variable is optional. With none of them set, a job must
still run, slower.** A job that needs a variable to succeed is a bug. Values
are never echoed to the job log, and no value appears in this repository.

| Variable | Platform | What it enables when set | Without it |
|---|---|---|---|
| `CI_HUB_SSH` | Linux, Windows | ssh target (`user@host`) of the shared artifact hub. Together with `CI_HUB_VENDOR_DIR` the vendor job publishes its tarball there and consumers on other runners fetch it | the vendored tree passes between jobs through `actions/cache` |
| `CI_HUB_VENDOR_DIR` | Linux, Windows | directory on the hub for vendor tarballs (needs `CI_HUB_SSH`) | as above |
| `CI_HUB_ARTIFACTS_ROOT` | Linux | directory on the hub for build bundles, read by `tools/ci/stage-artifacts.sh` when a local bundle is missing (needs `CI_HUB_SSH`) | bundles pass through `actions/upload-artifact` / `download-artifact` only |
| `CI_VENDOR_DIR` | Linux | runner-local directory for vendor tarballs; consumers on the same host extract from it without a network fetch | no local fast path |
| `CI_WIN_VENDOR_DIR` | Windows | the same, for Windows runners: a directory that may hold the vendor tarball the Linux jobs of the same machine published; the Windows jobs read it and never prune it | no local fast path (a hub fetch goes to a per-profile download directory) |
| `CI_WIN_TOOLS_DIR` | Windows | `;`-separated list of directories added to `PATH` ahead of everything else (for example one for `bazelisk`, one for the CMake `bin`, one for the MSYS2 `usr/bin`); together they may provide `bazelisk` (or `bazel`), `bash.exe`, `cmake`, `ninja` and `zstd`. Visual Studio is found with `vswhere` | tools come from `PATH`, and bash from MSYS2 or Git for Windows in their usual locations |
| `BAZEL_CACHE_HOST` | Linux | address of the remote Bazel cache; jobs add `--add-host=bazel-remote-cache:<value>` to their build containers and build with `--config=ci` | builds use `--config=docker` with no remote cache |
| `BAZEL_CACHE_AUTH_RC` | Linux | path to a bazelrc fragment with optional cache credentials; jobs that map the cache (`BAZEL_CACHE_HOST`) mount it read-only at `/etc/bazel.bazelrc` in their build containers, where Bazel reads it as the system rc (`tools/ci/remote-cache-docker-args.sh`). Ignored unless it names a readable file | the cache is used without credentials |
| `BAZEL_REMOTE_CACHE_URL` | Windows, macOS | full remote cache URL, passed as `--remote_cache` | no remote cache |
| `BAZEL_SH` | Windows | the bash Bazel runs genrules with (release lane Windows jobs) | MSYS2's bash, else Git for Windows' bash |
| `CI_WIN_BAZEL_OUTPUT_ROOT` | Windows | Bazel `--output_user_root` for every Windows job (a short path on a volume with room) | `C:/b` |
| `CI_DEV_IMAGE_LOCK_DIR` | Linux | directory for the per-host lock files `tools/ci/ensure-dev-image.sh` takes around image builds (useful when several runner instances share one Docker daemon) | locks go to the temporary directory |

Windows jobs build natively and need no variable for cache credentials: Bazel
on Windows honours the machine-wide system bazelrc, so a runner can supply
optional cache credentials there.

## Host ports per runner slot

Several runner slots can share one host, and the compose stacks the pipelines
bring up (`tools/e2e`, `tools/realworld-tests`, `tools/stress`,
`tools/production-tests`, `tools/http-compliance`, `tools/async-css-probe`,
`tools/workbench-demo`) publish host ports. The first step of each such job
sources `tools/ci/runner-ports.sh`, which reads the trailing digits of
`RUNNER_NAME` (`fleet-heavy-07` gives 7; only the last two digits count, taken
modulo 20), multiplies them by 1000 and exports every published port as its
default plus that offset (`E2E_NGINX_PORT`, `STRESS_NGINX_PORT` and so on; the
script lists them all). The highest port is 19 * 1000 + 9882 = 28882, below
the Linux ephemeral range. A name without trailing digits, an unset name, a
GitHub-hosted runner (`RUNNER_ENVIRONMENT=github-hosted`), and any run by hand
get the defaults, so a developer running a stack locally sees the ports the
stack's README names.

Tools the Windows jobs expect (on `PATH` or in `CI_WIN_TOOLS_DIR`): `bazelisk`
or `bazel`, `zstd` (hub channel only), `scp` (hub fetch only), Visual Studio
2022 with the C++ workload, and MSYS2 or Git for Windows for Bazel's shell.
Hosted `windows-2022` runners provide all of them except the hub tools.
The hosted Windows jobs add NASM 2.16.03 and gperf (installed with `pacman` into
the image's MSYS2) before the build.

Tools the Linux fleet jobs expect on `PATH`: `docker`, `bash`, `git`, `tar` with
zstd support, `rsync` and `ssh` (hub only), and `bazel` or `npm` (the vendor
job installs bazelisk through npm when `bazel` is missing). Hosted runners
provide all of them.

## Per-slot resource budget

A host runs several heavy jobs at once, so each runner states its own share in
its environment file:

| Variable | Meaning | Used for |
|---|---|---|
| `CI_SLOT_CPUS` | positive integer | Bazel `--jobs`, `run-clang-tidy -j`, Windows Bazel `--jobs`, cmake rule cap and JS fuzz build jobs |
| `CI_SLOT_MEM_GB` | positive integer (GiB) | container `--memory` and `--memory-swap` |

Both are optional. Unset (hosted runners) keeps the previous behaviour: Bazel
sizes itself from the host CPU count and each container keeps its fixed cap
(50g or 55g; the lint container has none). With both set, the lint step also
limits `run-clang-tidy` to one process per 2 GiB. A set but invalid value fails the
job with a clear message. The logic lives in `tools/ci/slot-budget.sh`
(self-test: `tools/ci/test_slot_budget.sh`). Size the values so that all slots
of a host at full use fit in the memory available to the runner services.

Anything that chooses a parallelism inside a job container must read the slot
budget rather than the host CPU count, because `nproc` inside a container still
reports every CPU of the host. Read `${CI_SLOT_CPUS:-$(nproc)}` and pass it as
`make -j`, `cmake --build -j`, `--parallel`, `xargs -P` or the tool's own
worker flag; the `nproc` fallback keeps developer machines unchanged. The
job's `docker run` must pass `-e CI_SLOT_CPUS` for a script to see it. The
cmake rule `bazel/cmake_build.bzl` cannot read the variable (a Bazel action
would need it in every cache key), so it uses a fixed cap of 8 jobs; the JS fuzz
build reads the slot.

## Windows runner: git and PATH requirements

On a self-hosted Windows runner the jobs assume the following.

1. **Git configuration.** The git the runner service uses (the first `git.exe`
   on the machine `PATH`, as seen by the service account) has
   `core.autocrlf=false` and `core.longPaths=true` in its own system
   configuration. Release jobs check out a tag's tree as it was at the time, so
   repository attributes added later do not apply; the gperf generator input
   and the bash scripts must be LF, and the vendored tree nests deeply enough
   to need long paths. Set both with the full path to that binary:

       "C:\Program Files\Git\cmd\git.exe" config --system core.autocrlf false
       "C:\Program Files\Git\cmd\git.exe" config --system core.longPaths true

   Plain `git config --system` from an interactive shell may configure a
   different install than the one the service uses. Adjust the path to the
   install the service resolves.
2. **Machine `PATH`.** The tools are on the machine `PATH`, not a user's
   `PATH`: Git for Windows `cmd` and `bin` (so `shell: bash` resolves to Git
   bash or MSYS2 bash, never the WSL launcher in System32), MSYS2 `usr\bin`
   before System32 when MSYS2 provides bash, and the bazelisk, nasm and CMake
   directories. Alternatively list those tool directories in `CI_WIN_TOOLS_DIR`;
   only the Bazel build steps honour it. Bash steps and the release jobs' tool
   checks read `PATH` alone.
3. **No user-profile path on `PATH`.** No entry of the form
   `C:\Users\<name>\...` may appear on the machine `PATH`. Job logs print
   `PATH`, and a public log must not carry a user name.
4. **Bazel output root.** Set `CI_WIN_BAZEL_OUTPUT_ROOT` (see the table above)
   to a short path on a volume with room.

## Adding a variable

1. Give it a neutral name that says what it is, not where it lives.
2. Read it with a default (`${NAME:-}`) and branch on it being set; state the
   degraded behaviour in the job or the script next to the branch.
3. Never print its value; print what it enables ("using the remote cache").
4. Add a row to the table above.
