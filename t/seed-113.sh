#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/113-agent-markdown-headers.t: a stored page with a Markdown
# copy, a stored page without one, and the /llms.txt index.
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

dir=/tmp/psmdtok-113
vol="${dir}/cache.vol"
mkdir -p "$dir"
rm -f "${vol}"* "${vol%.vol}"-*.vol

page() {  # page URL BODY
  "$SEED" "${SITE[@]}" --cache "$vol" --url "$1" --host localhost --scheme http \
    --content_type html --mask 0x08 --origin_cc "max-age=600" --content "$2"
}

# A Markdown copy is served only while its recorded hash matches the URL's
# live content hash, so the copy gets both.
hash=$(printf 'ab%.0s' $(seq 32))
markdown() {  # markdown URL BODY
  page "$1" "<html><body>stored parent</body></html>"
  "$SEED" "${SITE[@]}" --cache "$vol" --url "$1" --host localhost --scheme http \
    --mask 0x3C --content_hex "$hash"
  "$SEED" "${SITE[@]}" --cache "$vol" --url "$1" --host localhost --scheme http \
    --mask 0x7C --content_type other --origin_ct text/markdown \
    --origin_html_hash "$hash" --content "$2"
}

# 26 bytes of Markdown: an estimate of 6 tokens.
markdown /md "# markdown copy for agents"
markdown /signal/md "# markdown copy for agents"
markdown /signal/off/md "# markdown copy for agents"
page /html-only "<html><body>stored html only</body></html>"

# The /llms.txt index (0x8C): 14 bytes, an estimate of 3 tokens.
"$SEED" "${SITE[@]}" --cache "$vol" --url /llms.txt --host localhost --scheme http \
  --content_type other --mask 0x8C --content "# stored index"
