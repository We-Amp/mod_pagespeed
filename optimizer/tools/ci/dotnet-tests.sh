#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# Runs the two .NET test projects under samples/aspnetcore/test
# (WeAmp.PageSpeed.Tests and WeAmp.PageSpeed.AspNetCore.Tests) on net8.0 and
# net10.0 against a Linux Build bundle's libpagespeed.so and factory_worker.
# Used by the dotnet-tests job of pipeline-pr.yml and the dotnet-tests-x64
# job of pipeline-nightly.yml. Builds no native code.
#
# Usage: tools/ci/dotnet-tests.sh BUNDLE_DIR RESULTS_DIR
#
#   BUNDLE_DIR   a downloaded Linux x64 build bundle (libpagespeed.so and
#                factory_worker; its checksums are verified by the caller)
#   RESULTS_DIR  an empty per-job directory; receives the .trx files and
#                summary.md
#
# Everything runs in containers as the runner's own user (container root on
# a rootless daemon, see tools/ci/container-identity.sh), so a shared host
# keeps no SDK, package cache or root-owned file from this job:
#   1. the workbench SPA that WeAmp.PageSpeed.AspNetCore embeds (/console/)
#      is built with pnpm in node:22, so the console tests run against the
#      real build;
#   2. one image holds the .NET 10 SDK plus the .NET 8 runtimes (the
#      projects target both frameworks, and only the 10 SDK restores both);
#      the tests run in it with CI=true, so the native-dependent tests fail
#      instead of skipping when the library does not load.
# The job fails when any test fails, when a test project reports no tests,
# or when CookieRequestRecordTests (native cache, no worker) did not run and
# pass on both frameworks.

set -euo pipefail

BUNDLE="${1:?usage: dotnet-tests.sh BUNDLE_DIR RESULTS_DIR}"
RESULTS="${2:?usage: dotnet-tests.sh BUNDLE_DIR RESULTS_DIR}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# Pinned by digest: a test run whose SDK or runtime moves under it reports
# a toolchain change as a product failure. Bump deliberately.
SDK_IMAGE="mcr.microsoft.com/dotnet/sdk:10.0-noble@sha256:fe0fd4e93bd2fbb47c883b7a6369e5c8d3db8bf01cdc5981f6ee35fbdf3ff4fb"
RUNTIME8_IMAGE="mcr.microsoft.com/dotnet/aspnet:8.0-noble@sha256:fb50a3d0efad014b4fa8ee85fa498818172e38216cc73742821319f9db827bb2"
NODE_IMAGE="node:22"

for f in libpagespeed.so factory_worker; do
  [ -s "${BUNDLE}/${f}" ] || { echo "::error::${BUNDLE}/${f} is missing from the bundle"; exit 1; }
done
mkdir -p "${RESULTS}"

# Only the two binaries the tests load: the test project copies everything
# in PageSpeedNativeAssetsDir into its output.
NATIVE="${RESULTS}/native"
mkdir -p "${NATIVE}"
install -m 755 "${BUNDLE}/libpagespeed.so" "${BUNDLE}/factory_worker" "${NATIVE}/"

# The container user: the runner's uid on a rootful daemon, so what the
# containers write into the bind mounts stays the runner's; container root on
# a rootless daemon, where root is the runner user.
eval "$("${ROOT}/tools/ci/container-identity.sh")"

# Per-slot budget (CI_SLOT_CPUS / CI_SLOT_MEM_GB); legacy values when unset.
# shellcheck source=tools/ci/slot-budget.sh
source "${ROOT}/tools/ci/slot-budget.sh"
slot_budget 8g || exit 1
LIMITS=(${SLOT_CPUS:+--cpus="${SLOT_CPUS}"})
[ -z "${SLOT_MEM}" ] || LIMITS+=(--memory="${SLOT_MEM}" --memory-swap="${SLOT_MEM}")

echo "=== Building the workbench SPA (embedded /console/) ==="
docker run --rm --init "${LIMITS[@]}" \
  --user "${CONTAINER_USER}" -e HOME=/tmp -e COREPACK_HOME=/tmp/.corepack \
  -v "${ROOT}:/repo" -w /repo/tools/workbench \
  "${NODE_IMAGE}" \
  bash -c 'set -euo pipefail
    mkdir -p /tmp/bin && corepack enable --install-directory /tmp/bin
    export PATH=/tmp/bin:$PATH
    pnpm install --frozen-lockfile
    pnpm -C packages/api-client build
    pnpm -C packages/web-shell build
    test -s packages/web-shell/dist/index.html'

# The image name carries a hash of its definition, so a host keeps one copy
# per pin and concurrent jobs build the same content.
DOCKERFILE="FROM ${RUNTIME8_IMAGE} AS runtime8
FROM ${SDK_IMAGE}
COPY --from=runtime8 /usr/share/dotnet/shared/ /usr/share/dotnet/shared/
"
TEST_IMAGE="pagespeed-dotnet-tests:$(printf '%s' "${DOCKERFILE}" | sha256sum | cut -c1-16)"
if ! docker image inspect "${TEST_IMAGE}" >/dev/null 2>&1; then
  echo "=== Building ${TEST_IMAGE} (.NET 10 SDK + .NET 8 runtimes) ==="
  printf '%s' "${DOCKERFILE}" | docker build -q -t "${TEST_IMAGE}" -
fi

echo "=== Building and running the tests ==="
rc=0
docker run --rm --init "${LIMITS[@]}" \
  --user "${CONTAINER_USER}" \
  -e HOME=/tmp -e DOTNET_CLI_HOME=/tmp -e NUGET_PACKAGES=/tmp/nuget \
  -e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e DOTNET_NOLOGO=1 -e DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
  -e CI=true \
  -e PAGESPEED_E2E_NATIVE_DIR=/native \
  -v "${ROOT}:/repo" -v "${NATIVE}:/native:ro" -v "${RESULTS}:/results" \
  -w /repo/samples/aspnetcore \
  "${TEST_IMAGE}" \
  bash -c 'set -euo pipefail
    dotnet --list-runtimes
    rc=0
    for proj in WeAmp.PageSpeed.Tests WeAmp.PageSpeed.AspNetCore.Tests; do
      csproj="test/${proj}/${proj}.csproj"
      # ContinuousIntegrationBuild (Directory.Build.props sets it when CI is
      # true) maps source paths to /_/, and VersionInvariantTests find the
      # tree through [CallerFilePath]; these assemblies are never packed.
      dotnet build "${csproj}" -c Release -p:ContinuousIntegrationBuild=false \
        -p:WebShellSkipBuild=true -p:PageSpeedNativeAssetsDir=/native
      for fw in net8.0 net10.0; do
        # WeAmp.PageSpeed.Tests has no copy target of its own; the library
        # resolves runtimes/<rid>/native next to the test assembly first.
        dest="test/${proj}/bin/Release/${fw}/runtimes/linux-x64/native"
        mkdir -p "${dest}"
        cp /native/libpagespeed.so /native/factory_worker "${dest}/"
        dotnet test "${csproj}" -c Release -f "${fw}" --no-build \
          --results-directory /results \
          --logger "trx;LogFileName=${proj}-${fw}.trx" \
          --logger "console;verbosity=normal" || rc=1
      done
    done
    exit "${rc}"' || rc=$?

echo "=== Results ==="
python3 -I - "${RESULTS}" "${rc}" <<'PY'
import glob, os, sys
import xml.etree.ElementTree as ET

results, rc = sys.argv[1], int(sys.argv[2])
problems = []
lines = ["| Project | Framework | Total | Passed | Failed | Skipped |",
         "|---|---|---:|---:|---:|---:|"]
want = {("WeAmp.PageSpeed.Tests", fw) for fw in ("net8.0", "net10.0")}
want |= {("WeAmp.PageSpeed.AspNetCore.Tests", fw) for fw in ("net8.0", "net10.0")}
seen = set()
for path in sorted(glob.glob(os.path.join(results, "*.trx"))):
    proj, fw = os.path.basename(path)[:-4].rsplit("-", 1)
    seen.add((proj, fw))
    root = ET.parse(path).getroot()
    c = next(e for e in root.iter() if e.tag.endswith("}Counters"))
    total, passed, failed = (int(c.get(k, 0)) for k in ("total", "passed", "failed"))
    skipped = []
    failed_names = []
    cookie_run = cookie_bad = 0
    for r in root.iter():
        if not r.tag.endswith("}UnitTestResult"):
            continue
        name, outcome = r.get("testName", ""), r.get("outcome")
        if outcome == "NotExecuted":
            skipped.append(name)
        elif outcome != "Passed":
            failed_names.append(name)
        if ".CookieRequestRecordTests." in name:
            if outcome == "Passed":
                cookie_run += 1
            else:
                cookie_bad += 1
    lines.append("| %s | %s | %d | %d | %d | %d |" % (proj, fw, total, passed, len(failed_names), len(skipped)))
    print("%s %s: %d tests, %d passed, %d failed, %d skipped" % (proj, fw, total, passed, len(failed_names), len(skipped)))
    for n in skipped:
        print("  skipped: " + n)
    for n in failed_names:
        print("  FAILED: " + n)
    if total == 0:
        problems.append("%s %s reported no tests" % (proj, fw))
    if failed_names or failed:
        problems.append("%s %s: %d failed" % (proj, fw, max(failed, len(failed_names))))
    if proj == "WeAmp.PageSpeed.AspNetCore.Tests":
        print("  CookieRequestRecordTests: %d passed, %d not passed" % (cookie_run, cookie_bad))
        if cookie_run == 0 or cookie_bad:
            problems.append("%s %s: CookieRequestRecordTests did not all run and pass (%d passed, %d not)" % (proj, fw, cookie_run, cookie_bad))
for p in sorted(want - seen):
    problems.append("no results for %s %s" % p)
if rc and not problems:
    problems.append("the test container exited with %d" % rc)
with open(os.path.join(results, "summary.md"), "w") as f:
    f.write("\n".join(lines) + "\n")
    if problems:
        f.write("\n" + "\n".join("- " + p for p in problems) + "\n")
for p in problems:
    print("::error::" + p)
sys.exit(1 if problems else 0)
PY
