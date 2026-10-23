#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Fail when the bazel launcher on this runner does not run the Bazel version
# pinned in .bazelversion.
#
# Bazelisk releases before 1.19.0 find the workspace root, and with it
# .bazelversion, only through a WORKSPACE file. Without one such a launcher
# silently runs the latest Bazel instead. A plain Bazel binary on PATH
# must be exactly the pinned version. Windows legs use the PowerShell twin,
# tools/ci/Check-BazelVersion.ps1.
#
# Usage: tools/ci/check-bazel-version.sh [launcher]   (default: bazel)

set -euo pipefail

launcher="${1:-bazel}"
root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
want="bazel $(tr -d '[:space:]' < "${root}/.bazelversion")"
rc=0
out="$(cd "${root}" && "${launcher}" --version 2>/dev/null)" || rc=$?
if [ "${rc}" -ne 0 ]; then
  echo "::error::'${launcher} --version' exited with ${rc} on runner ${RUNNER_NAME:-unknown} (${RUNNER_OS:-unknown}, ${RUNNER_ENVIRONMENT:-unknown}). See docs/ci/runner-environment.md."
  exit 1
fi
got="$(printf '%s\n' "${out}" | head -n 1 | tr -d '\r')"
if [ "${got}" != "${want}" ]; then
  echo "::error::'${launcher}' on runner ${RUNNER_NAME:-unknown} (${RUNNER_OS:-unknown}, ${RUNNER_ENVIRONMENT:-unknown}) reports '${got}', not '${want}' from .bazelversion. A Bazelisk launcher must be 1.19.0 or newer; a plain Bazel must be exactly the pinned version. See docs/ci/runner-environment.md."
  exit 1
fi
echo "bazel launcher ok: ${got} (${launcher})"
