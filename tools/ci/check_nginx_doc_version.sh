#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Docs check: docs/install-nginx.md names the nginx.org nginx version the
# nginx.org packages are built for.
#
#   check_nginx_doc_version.sh [<release_nginx.sh> <install-nginx.md>]
#
# The version is the NGINXORG_VERSION default in tools/ci/release_nginx.sh.
# Every "ngxX.Y.Z" package-version example and every "FROM nginx:X.Y.Z" line
# in the doc must name it, and there must be at least one of each. Exits 1 and
# prints path:line per stale mention. Self-test:
# tools/ci/test_check_nginx_doc_version.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LANE="${1:-${ROOT}/tools/ci/release_nginx.sh}"
DOC="${2:-${ROOT}/docs/install-nginx.md}"
for f in "$LANE" "$DOC"; do
  [ -f "$f" ] || { echo "error: no such file: $f" >&2; exit 1; }
done

mapfile -t pins < <(sed -n 's/.*NGINXORG_VERSION:-\([0-9][0-9.]*[0-9]\)}.*/\1/p' "$LANE" | sort -u)
if [ "${#pins[@]}" -ne 1 ]; then
  echo "error: ${LANE} must have one NGINXORG_VERSION default, got '${pins[*]-}'" >&2
  exit 1
fi
WANT="${pins[0]}"

bad=0
pkg=0
from=0
n=0
while IFS= read -r line || [ -n "$line" ]; do
  n=$((n + 1))
  rest="$line"
  while [[ "$rest" =~ ngx([0-9]+\.[0-9]+\.[0-9]+) ]]; do
    pkg=$((pkg + 1))
    if [ "${BASH_REMATCH[1]}" != "$WANT" ]; then
      echo "${DOC}:${n}: names nginx ${BASH_REMATCH[1]}, the nginx.org packages are built for ${WANT}"
      bad=1
    fi
    rest="${rest#*"${BASH_REMATCH[0]}"}"
  done
  if [[ "$line" =~ ^FROM[[:space:]]+nginx:([0-9]+\.[0-9]+\.[0-9]+) ]]; then
    from=$((from + 1))
    if [ "${BASH_REMATCH[1]}" != "$WANT" ]; then
      echo "${DOC}:${n}: FROM nginx:${BASH_REMATCH[1]}, the nginx.org packages are built for ${WANT}"
      bad=1
    fi
  fi
done < "$DOC"

if [ "$pkg" -eq 0 ] || [ "$from" -eq 0 ]; then
  echo "error: ${DOC} has ${pkg} ngxX.Y.Z example(s) and ${from} FROM nginx:X.Y.Z line(s); expected both" >&2
  exit 1
fi
[ "$bad" -eq 0 ] || exit 1
echo "check_nginx_doc_version: ok (nginx ${WANT}, ${pkg} package example(s), ${from} FROM line(s))"
