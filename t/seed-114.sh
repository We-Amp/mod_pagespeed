#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/114-agent-vary-ordering.t: stored pages that must be
# revalidated (no-cache with an ETag) or refetched (stale, no validator).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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

dir=/tmp/psvaryord-114
vol="${dir}/cache.vol"
mkdir -p "$dir"
rm -f "${vol}"* "${vol%.vol}"-*.vol

page() {  # page URL CACHE_CONTROL ETAG
  "$SEED" "${SITE[@]}" --cache "$vol" --url "$1" --host localhost --scheme http \
    --content_type html --mask 0x08 --origin_cc "$2" --origin_etag "$3" \
    --content "<html><body>stored $1</body></html>"
}

page /reval "no-cache" '"v1"'
page /reval-vary "no-cache" '"v1"'
page /refetch "max-age=0" ""
