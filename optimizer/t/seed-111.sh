#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/111-cookie-request.t: stored pages and Markdown copies with
# and without a sharing mark (public / s-maxage) in their Cache-Control.
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

dir=/tmp/pscookie-111
vol="${dir}/cache.vol"
mkdir -p "$dir"
rm -f "${vol}"* "${vol%.vol}"-*.vol

page() {  # page URL CACHE_CONTROL BODY
  "$SEED" "${SITE[@]}" --cache "$vol" --url "$1" --host localhost --scheme http \
    --content_type html --mask 0x08 --origin_cc "$2" --content "$3"
}

page /plain "max-age=600" "<html><body>stored plain</body></html>"
page /public "public, max-age=600" "<html><body>stored public</body></html>"
page /smaxage "max-age=600, s-maxage=600" \
  "<html><body>stored smaxage</body></html>"
page /mustrev "max-age=600, must-revalidate" \
  "<html><body>stored mustrev</body></html>"

# A Markdown copy is served only while its recorded hash matches the URL's
# live content hash, so each copy gets both.
hash=$(printf 'ab%.0s' $(seq 32))
markdown() {  # markdown URL PARENT_CACHE_CONTROL
  page "$1" "$2" "<html><body>stored parent</body></html>"
  "$SEED" "${SITE[@]}" --cache "$vol" --url "$1" --host localhost --scheme http \
    --mask 0x3C --content_hex "$hash"
  "$SEED" "${SITE[@]}" --cache "$vol" --url "$1" --host localhost --scheme http \
    --mask 0x7C --content_type other --origin_ct text/markdown \
    --origin_html_hash "$hash" --content "# markdown $1"
}

markdown /md-plain "max-age=600"
markdown /md-public "public, max-age=600"

# Stored pages whose s-maxage is not an opt-in.  (A shared cache revalidates
# them before every use, so the tests see the origin either way; they pin
# that the cookie rule does not count them as marked.)
page /smaxage-zero "max-age=600, s-maxage=0" \
  "<html><body>stored smaxage-zero</body></html>"
page /smaxage-dup "max-age=600, s-maxage=600, s-maxage=600" \
  "<html><body>stored smaxage-dup</body></html>"

# Early Hints sets: one for a URL with no stored page (every request is a
# miss), one for a stored page without a sharing mark.
hints() {  # hints URL
  "$SEED" "${SITE[@]}" --cache "$vol" --url "$1" --host localhost --scheme http \
    --mask 0x1C --content "/hint.css"
}
hints /hints-nopage
page /hints-unmarked "max-age=600" "<html><body>stored hints</body></html>"
hints /hints-unmarked

# The /llms.txt index (0x8C).
"$SEED" "${SITE[@]}" --cache "$vol" --url /llms.txt --host localhost --scheme http \
  --content_type other --mask 0x8C --content "# stored index"
