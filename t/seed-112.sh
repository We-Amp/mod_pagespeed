#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/112-cache-per-server-block.t.  Tests 1-2 share an empty
# volume between server blocks and store through the MISS path.  For the
# other tests, /tmp/ps-112-own holds entries of the Test::Nginx server block
# and /tmp/ps-112-other the same entries for the other block in the test
# (same server name, another listener): a stored page with its Markdown copy,
# a /llms.txt index, an Early Hints set and an expired stylesheet.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SEED="${SEED_TEST_CACHE:-${REPO_ROOT}/bazel-bin/tools/seed_test_cache}"

if [ ! -x "$SEED" ]; then
  echo "Error: seed_test_cache not found at $SEED"
  echo "  Local: bazel build //tools:seed_test_cache"
  echo "  Docker: set SEED_TEST_CACHE env var"
  exit 1
fi

port="${TEST_NGINX_SERVER_PORT:-${TEST_NGINX_PORT:-1984}}"

for vol in /tmp/test-ps-112-1.vol /tmp/test-ps-112-2.vol /tmp/test-ps-112-3.vol; do
  rm -f "${vol}"* "${vol%.vol}"-*.vol
  "$SEED" --cache "${vol}" --create_only
done

hash=$(printf 'cd%.0s' $(seq 32))
seed_block() {  # dir listen
  local vol="$1/cache.vol"
  local site=(--site_listen "$2" --site_names localhost)
  local key=(--cache "$vol" --host localhost --scheme http)
  mkdir -p "$1"
  rm -f "${vol}"* "${vol%.vol}"-*.vol
  "$SEED" "${site[@]}" "${key[@]}" --url /md --content_type html --mask 0x08 \
    --origin_cc "public, max-age=600" --content "<html><body>page</body></html>"
  "$SEED" "${site[@]}" "${key[@]}" --url /md --mask 0x3C --content_hex "$hash"
  "$SEED" "${site[@]}" "${key[@]}" --url /md --mask 0x7C --content_type other \
    --origin_ct text/markdown --origin_html_hash "$hash" --content "# markdown"
  "$SEED" "${site[@]}" "${key[@]}" --url /llms.txt --content_type other \
    --mask 0x8C --content "# stored index"
  "$SEED" "${site[@]}" "${key[@]}" --url /hints --mask 0x1C \
    --content "/hint.css"
  "$SEED" "${site[@]}" "${key[@]}" --url /stale.css --content_type css \
    --mask 0x08 --origin_cc "max-age=0" --content "p{color:gray}"
}
seed_block /tmp/ps-112-own "0.0.0.0:${port}"
seed_block /tmp/ps-112-other "127.0.0.1:$((port + 31))"
