#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/301-etag.t tests (content-identity weak ETags).
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

VOL=/tmp/test-ps-301-1.vol

# Two entries with IDENTICAL byte length (13) and mask but DIFFERENT
# content-identity hashes — must yield different ETags.
"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/hash-a.css" \
  --host "localhost" \
  --scheme http \
  --content "h1{color:red}" \
  --content_type css \
  --mask 0x08 \
  --origin_html_hash "$(printf 'aa%.0s' $(seq 32))"

"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/hash-b.css" \
  --host "localhost" \
  --scheme http \
  --content "h1{color:blu}" \
  --content_type css \
  --mask 0x08 \
  --origin_html_hash "$(printf 'bb%.0s' $(seq 32))"

# Legacy entry: no content hash, no origin validators (pre-v7 shape) —
# must emit no ETag at all (never a length-only tag).
"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/legacy.css" \
  --host "localhost" \
  --scheme http \
  --content "h1{color:red}" \
  --content_type css \
  --mask 0x08

# Tier-2 entry: no content hash but a stored origin ETag — identity is
# derived from the origin validators (value pinned in etag_util_test.cc).
"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/val.css" \
  --host "localhost" \
  --scheme http \
  --content "h1{color:red}" \
  --content_type css \
  --mask 0x08 \
  --origin_etag '"v1"'
