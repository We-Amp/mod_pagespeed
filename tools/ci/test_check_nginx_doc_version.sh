#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for check_nginx_doc_version.sh: pass and fail fixtures, then the
# real tree.
#   bash tools/ci/test_check_nginx_doc_version.sh

# The fixtures are literal Markdown and shell source, never expanded here.
# shellcheck disable=SC2016

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/check_nginx_doc_version.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

printf '      local ngx="${NGINXORG_VERSION:-1.30.7}"\n' > "$T/lane.sh"

fail=0
expect() {
  local want="$1" name="$2" doc="$3" rc=0
  bash "$SCRIPT" "$T/lane.sh" "$doc" >"$T/out" 2>&1 || rc=$?
  if { [ "$want" = pass ] && [ "$rc" -ne 0 ]; } || { [ "$want" = fail ] && [ "$rc" -ne 1 ]; }; then
    echo "FAIL: ${name}: want ${want}, exit ${rc}"
    cat "$T/out"
    fail=1
  else
    echo "ok: ${name}"
  fi
}

printf '(`<v>+ngx1.30.7~trixie`, `<v>-0.ngx1.30.7.el9`)\nFROM nginx:1.30.7\n' > "$T/good.md"
expect pass "doc names the lane version" "$T/good.md"

printf '(`<v>+ngx1.30.7~trixie`, `<v>-0.ngx1.30.6.el9`)\nFROM nginx:1.30.7\n' > "$T/stale_pkg.md"
expect fail "a stale package example" "$T/stale_pkg.md"

printf '(`<v>+ngx1.30.7~trixie`)\nFROM nginx:1.30.6\n' > "$T/stale_from.md"
expect fail "a stale FROM line" "$T/stale_from.md"

printf 'FROM nginx:1.30.7\n' > "$T/no_pkg.md"
expect fail "no package example at all" "$T/no_pkg.md"

if bash "$SCRIPT" >"$T/out" 2>&1; then
  echo "ok: docs/install-nginx.md"
else
  echo "FAIL: docs/install-nginx.md"
  cat "$T/out"
  fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "test_check_nginx_doc_version: all passed"
