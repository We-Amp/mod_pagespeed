#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for assert_nginx_tarball_version.sh. Fake tarballs carry a
# README.md and a small ngx_pagespeed_module.so made of NUL-separated
# strings, like the real module's string table.
#   bash tools/ci/test_assert_nginx_tarball_version.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/assert_nginx_tarball_version.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

printf 'FROM scratch\nENV NGINX_VERSION=1.30.7\n' > "$T/Dockerfile"

# mk <dir> <arch> <readme-line> <so-version>
mk() {
  local dir="$1" arch="$2" line="$3" sover="$4" p="ngx_pagespeed-9.9.9"
  local w="${T}/w"
  rm -rf "$w"; mkdir -p "${w}/${p}" "$dir"
  printf '# ngx_pagespeed\n\n%s\n\nRequires Linux %s\n' "$line" "$arch" > "${w}/${p}/README.md"
  printf '\177ELF\000libpng 1.6.58\000%s\000FileCachePath must be set\000' "$sover" \
    > "${w}/${p}/ngx_pagespeed_module.so"
  tar czf "${dir}/${p}-linux-${arch}.tar.gz" -C "$w" "$p"
}

fail=0
expect() {
  local want="$1" name="$2" rc=0
  shift 2
  bash "$SCRIPT" "$@" >"$T/out" 2>&1 || rc=$?
  if { [ "$want" = pass ] && [ "$rc" -ne 0 ]; } || { [ "$want" = fail ] && [ "$rc" -ne 1 ]; }; then
    echo "FAIL: ${name}: want ${want}, exit ${rc}"
    cat "$T/out"
    fail=1
  else
    echo "ok: ${name}"
  fi
}

mk "$T/good" x86_64 "Built for nginx: 1.30.7" 1.30.7
mk "$T/good" aarch64 "Built for nginx: 1.30.7" 1.30.7
expect pass "both arches agree with the build image" --dockerfile "$T/Dockerfile" "$T/good"

mk "$T/stale" x86_64 "Built for nginx: 1.30.7" 1.30.7
mk "$T/stale" aarch64 "Built for nginx: 1.30.7" 1.30.7
legacy="This tarball ships the module .so built against the dev nginx (currently
1.30.3). nginx dynamic modules are pinned"
mk "$T/stale" aarch64 "$legacy" 1.30.7
expect fail "an old README naming another nginx" --dockerfile "$T/Dockerfile" "$T/stale"
grep -q "nginx 1.30.3" "$T/out" || { echo "FAIL: stale value not named"; cat "$T/out"; fail=1; }

mk "$T/readme" x86_64 "Built for nginx: 1.30.3" 1.30.7
mk "$T/readme" aarch64 "Built for nginx: 1.30.7" 1.30.7
expect fail "README version differs" --dockerfile "$T/Dockerfile" "$T/readme"

mk "$T/so" x86_64 "Built for nginx: 1.30.7" 1.30.7
mk "$T/so" aarch64 "Built for nginx: 1.30.7" 1.30.6
expect fail "module built for another nginx" --dockerfile "$T/Dockerfile" "$T/so"

mk "$T/substr" x86_64 "Built for nginx: 1.30.7" 1.30.7
mk "$T/substr" aarch64 "Built for nginx: 1.30.7" "nginx/1.30.71"
expect fail "a longer string containing the version does not count" --dockerfile "$T/Dockerfile" "$T/substr"

mk "$T/onearch" x86_64 "Built for nginx: 1.30.7" 1.30.7
expect fail "an arch missing" --dockerfile "$T/Dockerfile" "$T/onearch"

printf 'FROM scratch\nENV NGINX_VERSION=1.30.7\nENV NGINX_VERSION=1.30.8\n' > "$T/Dockerfile.two"
expect fail "the Dockerfile names two versions" --dockerfile "$T/Dockerfile.two" "$T/good"

printf 'FROM scratch\n' > "$T/Dockerfile.none"
expect fail "the Dockerfile names no version" --dockerfile "$T/Dockerfile.none" "$T/good"

[ "$fail" -eq 0 ] || exit 1
echo "test_assert_nginx_tarball_version: all passed"
