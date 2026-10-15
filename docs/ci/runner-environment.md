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
| `CI_WIN_CACHE_MAX_SIZE` | size cap for that cache (Bazel's size syntax); the cache has no built-in eviction, so a runner that keeps it sets this | no cap is applied by the job |
| `CI_WIN_LLVM_DIR` | install directory of LLVM 19 (`clang-cl` and the AddressSanitizer runtime library directory) | `clang-cl` from `PATH`; sanitizer jobs skip with a notice when the runtime is not found |
| `CI_WIN_PYTHON_DIR` | directory of Python 3.12 with pip, placed ahead of Git for Windows' python stub | `python` from `PATH` |
| `CI_WIN_DUMP_DIR` | directory for crash dumps (Windows Error Reporting local dumps); the scripts read it as `PAGESPEED_DUMP_DIR` | a directory under the job's temporary directory |

Tool expectations that have no variable: NASM is pinned to 2.16.03 (the
image-codec dependency rejects 3.x), so the Windows jobs install that version
into the runner's temporary directory when `PATH` does not provide it; Visual
Studio 2022 with the C++ workload; `zstd` and `scp` for the hub channel only.
Signing tools (`signtool`, the trusted-signing client) are a hosted-job
concern and are not required of a build runner. ProcDump is expected on
`PATH` by the IIS AddressSanitizer rig; Application Verifier (`appverif`)
comes with the Windows SDK and is expected on `PATH` by the AppVerifier lanes.

## IIS and virtual machine runners (class `fleet-iis`)

Jobs of this class have no hosted twin. The class means a Windows host with
these facts, which are not variables:

- the IIS role is installed and the job manages it (sites, application pools,
  `w3wp`, `appcmd`);
- the runner service account is a local administrator;
- one runner instance per host: the tests own ports 80 and 443 and the default
  web site, and never run concurrently with another IIS job.

| Variable | What it provides | Without it |
|---|---|---|
| `CI_IIS_VM_NAME` | name of the Hyper-V guest that holds a clean IIS installation, for the installer upgrade test | the test fails loudly (never a false green) |
| `CI_IIS_VM_SNAPSHOT` | name of the clean checkpoint the test restores before each scenario | as above |
| `CI_IIS_VM_CREDENTIAL_FILE` | path of a host-side file holding the guest administrator credential; read by the script and never echoed | as above |
| `CI_EA4_VM_<OS>` | name of the cPanel guest for one target OS (for example `CI_EA4_VM_EL9`), with a per-OS clean checkpoint by the same naming convention; ssh keys stay on the host | the cPanel smoke for that OS skips with an annotation |

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

## Adding a variable

1. Give it a neutral name that says what it is, not where it lives.
2. Read it with a default (`${NAME:-}`) and branch on it being set; state the
   degraded behaviour in the job or the script next to the branch.
3. Never print its value; print what it enables ("using the remote cache").
4. Add a row to the table above.
