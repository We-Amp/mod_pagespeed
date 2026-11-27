#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for the lint pipeline.
#
# Runs the `lint-compile-commands.sh` script the CI Lint job uses and
# asserts that the resulting compile_commands.json is fit for
# clang-tidy. Catches regressions that would otherwise surface as
# FileNotFoundError minutes into a CI Lint job.
#
# This is a smoke test, not a unit test — it requires the full lint
# Docker image and a vendored workspace. Invoke from inside the
# pagespeed2-lint container (as the CI Lint job does):
#
#     docker run --rm \
#       -v $(pwd):/workspace \
#       -v $HOME/.cache/bazel:/root/.cache/bazel \
#       -v $HOME/bazel-repo-cache:/root/bazel-repo-cache \
#       -v $HOME/.ssh/<ci-key>:/tmp/ssh_key:ro \
#       <remote-cache flags from tools/ci/remote-cache-docker-args.sh> \
#       pagespeed2-lint:latest \
#       bash -c 'cp /tmp/ssh_key /root/.ssh/ci_key && chmod 600 /root/.ssh/ci_key \
#         && ssh-keyscan -H github.com >> /root/.ssh/known_hosts 2>/dev/null \
#         && export GIT_SSH_COMMAND="ssh -i /root/.ssh/ci_key" \
#         && /workspace/tools/ci/lint-self-test.sh'
#
# GIT_SSH_COMMAND is required: ssh only auto-loads default identity names
# (id_rsa, id_ed25519, ...), so a key copied to ci_key is otherwise ignored and
# the git fetches this test triggers fall back to no key and fail.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="${BUILD_WORKSPACE_DIRECTORY:-/workspace}"
COMPDB="${WORKSPACE}/compile_commands.json"

cd "${WORKSPACE}"

# Ensure a clean starting state: a leftover compile_commands.json from a
# prior run could mask a regression where the new run produces nothing.
rm -f "${COMPDB}"

if ! "${HERE}/lint-compile-commands.sh"; then
  echo "FAIL: lint-compile-commands.sh exited non-zero" >&2
  exit 1
fi

# Re-check from the test's perspective (the helper script also validates,
# but a self-test should be paranoid; if a future refactor moves the
# validation out of the helper, the test still catches the regression).
if [ ! -s "${COMPDB}" ]; then
  echo "FAIL: ${COMPDB} missing or empty after lint-compile-commands.sh" >&2
  exit 1
fi

python3 - "${COMPDB}" <<'PY'
import json
import sys
with open(sys.argv[1]) as f:
    db = json.load(f)
assert isinstance(db, list) and db, "compile_commands.json is empty or not a list"
local = [e for e in db if "/lib/" in e.get("file", "") or "/src/" in e.get("file", "")]
assert local, f"no lib/ or src/ entries in {len(db)}-entry compile_commands.json"
# At least one canonical source file should be present.
needles = ("lib/base/atomic_file_writer.cc", "src/worker", "lib/cache")
hits = [n for n in needles if any(n in e.get("file", "") for e in db)]
assert hits, f"none of {needles!r} found in compile_commands.json"
print(f"PASS: {len(db)} entries, {len(local)} local, canonical hits: {hits}")
PY
