#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# Seed the cache for t/109-host-vouch-flag.t: a stored stylesheet under a
# listed and an unlisted hostname, for the hit-path cases.
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
  --site_names 'localhost,site.test')

dir=/tmp/psvouch-109
vol="${dir}/cache.vol"
mkdir -p "$dir"
rm -f "${vol}"* "${vol%.vol}"-*.vol

for host in site.test unlisted.test; do
  "$SEED" "${SITE[@]}" \
    --cache "$vol" \
    --url "/hit.css" \
    --host "$host" \
    --scheme http \
    --content "h1{color:red}" \
    --content_type css \
    --origin_ct text/css \
    --mask 0x08
done
