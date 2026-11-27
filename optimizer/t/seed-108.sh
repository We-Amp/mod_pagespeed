#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/108-access-rules.t tests.
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

# Start every run from fresh volumes so the MISS/HIT assertions stay
# deterministic on repeated prove invocations.
for vol in /tmp/test-ps-108-1.vol /tmp/test-ps-108-6.vol; do
  rm -f "${vol}"* "${vol%.vol}"-*.vol
done

# A stored CSS entry at the default mask (Desktop/Identity = 0x08), keyed for
# the plain-http requests Test::Nginx sends.
"$SEED" "${SITE[@]}" \
  --cache /tmp/test-ps-108-1.vol \
  --url "/style.css" \
  --host "localhost" \
  --scheme http \
  --content "p{color:blue}" \
  --content_type css \
  --mask 0x08

# An empty volume: the test stores its entry through the MISS path.
"$SEED" "${SITE[@]}" --cache /tmp/test-ps-108-6.vol --create_only
