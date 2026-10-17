#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test (no network) for the supply-chain checks in the user-facing
# build scripts:
#   1. scripts/build_nginx_with_pagespeed.sh verifies the nginx tarball digest
#      (wrong digest fails, right digest passes, unknown version fails closed
#      unless NGINX_SHA256 is given).
#   2. install/nginx/build_module_in_container.sh no longer downloads and
#      executes a remote installer script; the LLVM apt repository helper
#      writes a keyring-scoped source entry instead.
#
# Run: tools/ci/test_build_nginx_verify.sh [path-to-build_nginx_with_pagespeed.sh]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_NGINX="${1:-${ROOT}/scripts/build_nginx_with_pagespeed.sh}"
CONTAINER="${ROOT}/install/nginx/build_module_in_container.sh"

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
FAILS=0
check() { # name, rc-expected(0|1), actual rc
  if { [ "$2" = 0 ] && [ "$3" = 0 ]; } || { [ "$2" = 1 ] && [ "$3" != 0 ]; }; then
    echo "ok   - $1"
  else
    echo "FAIL - $1"; FAILS=$((FAILS + 1))
  fi
}

printf 'fake nginx tarball\n' > "$T/nginx.tar.gz"
GOOD="$(sha256sum "$T/nginx.tar.gz" 2>/dev/null | awk '{print $1}')"
[ -n "$GOOD" ] || GOOD="$(shasum -a 256 "$T/nginx.tar.gz" | awk '{print $1}')"
BAD="0000000000000000000000000000000000000000000000000000000000000000"

# Sourcing must not run the build: refuse unless the script guards main().
if ! grep -q 'BASH_SOURCE\[0\]}" = "\$0"' "$BUILD_NGINX"; then
  echo "FAIL - build script does not guard main() for sourcing"; exit 1
fi
set --
# shellcheck disable=SC1090
source "$BUILD_NGINX" 2>/dev/null
if ! declare -F verify_nginx_tarball >/dev/null; then
  echo "FAIL - verify_nginx_tarball is not defined"; exit 1
fi
set +e

(NGINX_SHA256="$BAD" verify_nginx_tarball 1.30.5 "$T/nginx.tar.gz") 2>/dev/null
check "wrong digest is rejected" 1 $?
(NGINX_SHA256="$GOOD" verify_nginx_tarball 1.30.5 "$T/nginx.tar.gz") 2>/dev/null
check "right digest is accepted" 0 $?
(unset NGINX_SHA256; verify_nginx_tarball 1.30.5 "$T/nginx.tar.gz") 2>/dev/null
check "pinned digest rejects a different file" 1 $?
(unset NGINX_SHA256; verify_nginx_tarball 9.9.9 "$T/nginx.tar.gz") 2>/dev/null
check "unknown version fails closed" 1 $?
(NGINX_SHA256="$GOOD" verify_nginx_tarball 9.9.9 "$T/nginx.tar.gz") 2>/dev/null
check "unknown version with explicit digest is accepted" 0 $?
(unset NGINX_SHA256; nginx_pinned_sha256 1.30.5 >/dev/null)
check "default version is pinned" 0 $?

# Container build script: no remote installer execution.
grep -q 'apt.llvm.org/llvm.sh' "$CONTAINER"
check "container script does not fetch llvm.sh" 1 $?

# The helper writes a signed-by source entry (key download mocked).
eval "$(sed -n '/^add_llvm_apt_repo()/,/^}/p' "$CONTAINER")"
if declare -F add_llvm_apt_repo >/dev/null; then
  mkdir -p "$T/bin" "$T/keyrings" "$T/sources"
  printf '#!/bin/sh\nwhile [ $# -gt 0 ]; do [ "$1" = -qO ] && out="$2"; shift; done\necho KEY > "$out"\n' > "$T/bin/wget"
  printf '#!/bin/sh\nexit 0\n' > "$T/bin/apt-get"
  chmod +x "$T/bin/wget" "$T/bin/apt-get"
  (PATH="$T/bin:$PATH" LLVM_KEYRING_DIR="$T/keyrings" LLVM_SOURCES_DIR="$T/sources" \
     LLVM_CODENAME=focal add_llvm_apt_repo 18) >/dev/null 2>&1
  check "add_llvm_apt_repo succeeds" 0 $?
  grep -q 'signed-by=.*llvm-toolchain-focal-18' "$T/sources/llvm-18.list" 2>/dev/null
  check "source entry is key-scoped for the right suite" 0 $?
else
  check "add_llvm_apt_repo is defined" 0 1
fi

[ "$FAILS" = 0 ] && echo "all passed" || { echo "$FAILS failed"; exit 1; }
