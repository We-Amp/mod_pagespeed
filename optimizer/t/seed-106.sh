#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache volumes for t/106-cache-generation.t.
# Called by tools/run-test-nginx.sh or the test's BEGIN block.
# Supports both local (bazel-bin/) and Docker (SEED_TEST_CACHE env var).
#
# Each case gets its own cache-parent directory, because the cache-directory
# generation is read from pagespeed-shared.conf BESIDE the volume: the .t
# prologue writes a different one into each directory, playing an optimizer
# that publishes generation 2, generation 1, generation 3, or none at all.
# Every volume holds /style.css at the default mask, so a request for it is a
# HIT exactly when the module uses the cache.
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

for name in match older newer none recover; do
  dir="/tmp/psgen-106-${name}"
  vol="${dir}/cache.vol"
  mkdir -p "$dir"
  # Drop leftovers from previous runs (the volume, its .gen stamp, .small
  # sibling, and Cyclone's fingerprint-named siblings) so every run starts
  # from a volume holding only the seeded entry.
  rm -f "${vol}"* "${vol%.vol}"-*.vol
  "$SEED" "${SITE[@]}" \
    --cache "$vol" \
    --url "/style.css" \
    --host "localhost" \
    --scheme http \
    --content "h1{color:red}" \
    --content_type css \
    --mask 0x08
done
