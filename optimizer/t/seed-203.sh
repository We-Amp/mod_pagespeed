#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# Pre-create EMPTY cache volumes for t/203-vary-request-fields.t.
# The nginx module opens the cache resolve-only and cannot cold-create a
# volume, so these MISS-path tests need volumes that exist but hold no entries.
# Called by tools/run-test-nginx.sh or the test's BEGIN block.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SEED="${SEED_TEST_CACHE:-${REPO_ROOT}/bazel-bin/tools/seed_test_cache}"

if [ ! -x "$SEED" ]; then
  echo "Error: seed_test_cache not found at $SEED"
  echo "  Local: bazel build //tools:seed_test_cache"
  echo "  Docker: set SEED_TEST_CACHE env var"
  exit 1
fi

for n in 1 2 3 4 5 6; do
  vol="/tmp/test-ps-203-${n}.vol"
  rm -f "${vol}"* "${vol%.vol}"-*.vol
  "$SEED" --cache "$vol" --create_only
done
