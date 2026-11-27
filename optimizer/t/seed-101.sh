#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Pre-create EMPTY cache volumes for t/101-*.t tests.
# Since #1319 the nginx module opens the cache resolve-only (volume_size=0)
# and cannot cold-create a volume, so tests need volumes that exist but hold
# no entries.
# Called by tools/run-test-nginx.sh or the test's BEGIN block.
# Supports both local (bazel-bin/) and Docker (SEED_TEST_CACHE env var).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Allow override via environment (used by docker-test-nginx.sh)
SEED="${SEED_TEST_CACHE:-${REPO_ROOT}/bazel-bin/tools/seed_test_cache}"

if [ ! -x "$SEED" ]; then
  echo "Error: seed_test_cache not found at $SEED"
  echo "  Local: bazel build //tools:seed_test_cache"
  echo "  Docker: set SEED_TEST_CACHE env var"
  exit 1
fi

# Entries go to the keyspace of the Test::Nginx server block the test
# requests reach: its listener and its server names.
SITE=(--site_listen "0.0.0.0:${TEST_NGINX_SERVER_PORT:-${TEST_NGINX_PORT:-1984}}"
  --site_names 'localhost')

for vol in /tmp/test-ps-101-1.vol /tmp/test-ps-101-2.vol /tmp/test-ps-101-3.vol; do
  # Drop leftovers from previous runs (the volume, its .gen stamp, .small
  # sibling, and Cyclone's fingerprint-named siblings) so every run starts
  # from a genuinely empty volume.
  rm -f "${vol}"* "${vol%.vol}"-*.vol
  "$SEED" "${SITE[@]}" --cache "$vol" --create_only
done
