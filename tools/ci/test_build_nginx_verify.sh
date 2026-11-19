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

(NGINX_SHA256="$BAD" verify_nginx_tarball 1.30.7 "$T/nginx.tar.gz") 2>/dev/null
check "wrong digest is rejected" 1 $?
(NGINX_SHA256="$GOOD" verify_nginx_tarball 1.30.7 "$T/nginx.tar.gz") 2>/dev/null
check "right digest is accepted" 0 $?
(unset NGINX_SHA256; verify_nginx_tarball 1.30.7 "$T/nginx.tar.gz") 2>/dev/null
check "pinned digest rejects a different file" 1 $?
(unset NGINX_SHA256; verify_nginx_tarball 9.9.9 "$T/nginx.tar.gz") 2>/dev/null
check "unknown version fails closed" 1 $?
(NGINX_SHA256="$GOOD" verify_nginx_tarball 9.9.9 "$T/nginx.tar.gz") 2>/dev/null
check "unknown version with explicit digest is accepted" 0 $?
(unset NGINX_SHA256; nginx_pinned_sha256 1.30.7 >/dev/null)
check "default version is pinned" 0 $?

# Container build script: no remote installer execution.
grep -q 'apt.llvm.org/llvm.sh' "$CONTAINER"
check "container script does not fetch llvm.sh" 1 $?

# The helpers write a signed-by source entry scoped to the pinned key only
# (key download mocked with a throwaway key; a second key in the file must not
# reach the keyring, and a file without the pinned key is refused).
eval "$(sed -n '/^pinned_keyring()/,/^}/p' "$CONTAINER")"
eval "$(sed -n '/^add_llvm_apt_repo()/,/^}/p' "$CONTAINER")"
eval "$(sed -n '/^add_toolchain_ppa()/,/^}/p' "$CONTAINER")"
if declare -F add_llvm_apt_repo >/dev/null && declare -F add_toolchain_ppa >/dev/null && command -v gpg >/dev/null 2>&1; then
  mkdir -p "$T/bin" "$T/keyrings" "$T/sources" "$T/gpg"
  chmod 700 "$T/gpg"
  for who in pinned other; do
    GNUPGHOME="$T/gpg" gpg --batch --quiet --pinentry-mode loopback --passphrase '' \
      --quick-gen-key "test-$who@example.invalid" ed25519 sign never 2>/dev/null
  done
  PINNED="$(GNUPGHOME="$T/gpg" gpg --batch --with-colons --list-keys test-pinned@example.invalid 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}')"
  GNUPGHOME="$T/gpg" gpg --batch --armor --export > "$T/both.asc"
  # shellcheck disable=SC2016 # the mock's own script text
  printf '#!/bin/sh\nwhile [ $# -gt 0 ]; do [ "$1" = -qO ] && out="$2"; shift; done\ncp "%s" "$out"\n' "$T/both.asc" > "$T/bin/wget"
  printf '#!/bin/sh\nexit 0\n' > "$T/bin/apt-get"
  chmod +x "$T/bin/wget" "$T/bin/apt-get"
  (PATH="$T/bin:$PATH" LLVM_KEYRING_DIR="$T/keyrings" LLVM_SOURCES_DIR="$T/sources" \
     LLVM_CODENAME=focal LLVM_APT_KEY_FPR="$PINNED" add_llvm_apt_repo 18) >/dev/null 2>&1
  check "add_llvm_apt_repo succeeds" 0 $?
  grep -q 'signed-by=.*llvm-snapshot.gpg.*llvm-toolchain-focal-18' "$T/sources/llvm-18.list" 2>/dev/null
  check "source entry is key-scoped for the right suite" 0 $?
  n="$(GNUPGHOME="$T/gpg" gpg --batch --with-colons --show-keys "$T/keyrings/llvm-snapshot.gpg" 2>/dev/null | grep -c '^pub:')"
  check "the keyring holds only the pinned key" 1 "$n"
  (PATH="$T/bin:$PATH" LLVM_KEYRING_DIR="$T/keyrings" LLVM_SOURCES_DIR="$T/sources" \
     LLVM_CODENAME=focal TOOLCHAIN_PPA_KEY_FPR="$PINNED" add_toolchain_ppa) >/dev/null 2>&1
  check "add_toolchain_ppa succeeds" 0 $?
  grep -q 'signed-by=.*ubuntu-toolchain-r.gpg.*ubuntu-toolchain-r/test/ubuntu focal main' "$T/sources/ubuntu-toolchain-r.list" 2>/dev/null
  check "PPA entry is key-scoped for the right suite" 0 $?
  (PATH="$T/bin:$PATH" LLVM_KEYRING_DIR="$T/keyrings" LLVM_SOURCES_DIR="$T/sources" \
     LLVM_CODENAME=focal LLVM_APT_KEY_FPR=0123456789ABCDEF0123456789ABCDEF01234567 add_llvm_apt_repo 18) >/dev/null 2>&1
  check "a key file without the pinned key is refused" 1 $?
  grep -q 'add-apt-repository' <(grep -v '^[[:space:]]*#' "$CONTAINER")
  check "container script does not use add-apt-repository" 1 $?
else
  check "the repository helpers are defined and gpg is available" 0 1
fi

# The toolchain image check (use_baked_toolchain): a marker of the right suite
# whose package versions are installed passes; no marker stops the build unless
# TOOLCHAIN_PPA_FALLBACK=1 asks for the PPA path; another suite, a version that
# differs, a package the marker lacks and a missing compiler stop it. dpkg-query
# and the compilers are mocks.
eval "$(sed -n '/^use_baked_toolchain()/,/^}/p' "$CONTAINER")"
if declare -F use_baked_toolchain >/dev/null; then
  B="$T/baked"
  mkdir -p "$B/bin"
  for tool in gcc-13 g++-13 clang clang++ ld.lld; do
    printf '#!/bin/sh\necho "%s (mock) 1.0"\n' "$tool" > "$B/bin/$tool"
  done
  # dpkg-query -W -f='${Version}' PKG: the version from $B/installed (PKG=VERSION lines).
  # shellcheck disable=SC2016 # the mock's own script text
  printf '#!/bin/sh\nfor a; do p="$a"; done\nv="$(sed -n "s/^$p=//p" "%s/installed")"\n[ -n "$v" ] || exit 1\nprintf "%%s" "$v"\n' "$B" > "$B/bin/dpkg-query"
  chmod +x "$B/bin/"*
  PKGS='g++-13=13.1.0-8 gcc-13=13.1.0-8 libstdc++-13-dev=13.1.0-8 clang-18=1:18.1.8 lld-18=1:18.1.8 llvm-18=1:18.1.8 libstdc++6=16-1'
  printf '%s\n' $PKGS > "$B/installed"
  { echo "# marker"; echo "suite=focal"; echo "toolchain_ppa_key_fpr=X"; printf 'pkg:%s\n' $PKGS; } > "$B/marker"
  baked() { (PATH="$B/bin:$PATH" TOOLCHAIN_MARKER="$B/marker" use_baked_toolchain "$@") >/dev/null 2>&1; }
  baked focal; check "toolchain image: a matching marker is used" 0 $?
  baked jammy; check "toolchain image: the marker of another suite stops the build" 1 $?
  (PATH="$B/bin:$PATH" TOOLCHAIN_MARKER="$B/none" use_baked_toolchain focal) >/dev/null 2>&1
  check "toolchain image: no marker and no fallback stops the build" 1 $?
  rc=0; (PATH="$B/bin:$PATH" TOOLCHAIN_MARKER="$B/none" TOOLCHAIN_PPA_FALLBACK=1 use_baked_toolchain focal) >/dev/null 2>&1 || rc=$?
  [ "$rc" = 1 ]; check "toolchain image: no marker with TOOLCHAIN_PPA_FALLBACK=1 takes the PPA path" 0 $?
  sed 's/^g++-13=.*/g++-13=13.2.0-1/' "$B/installed" > "$B/i2" && mv "$B/i2" "$B/installed"
  baked focal; check "toolchain image: a version that differs from the marker stops the build" 1 $?
  printf '%s\n' $PKGS > "$B/installed"
  grep -v '^pkg:lld-18=' "$B/marker" > "$B/m2" && mv "$B/m2" "$B/marker"
  baked focal; check "toolchain image: a marker without lld-18 stops the build" 1 $?
  { echo "suite=focal"; printf 'pkg:%s\n' $PKGS; } > "$B/marker"
  rm "$B/bin/ld.lld"
  baked focal; check "toolchain image: a missing compiler stops the build" 1 $?
  # The focal and jammy recipes reach the PPA only through the fallback branch.
  for d in focal jammy; do
    body="$(sed -n "/^install_deps_${d}()/,/^}/p" "$CONTAINER" | grep -v '^[[:space:]]*#')"
    printf '%s\n' "$body" | awk '/use_baked_toolchain/ { g = 1 } /add_toolchain_ppa|add_llvm_apt_repo|install_llvm_clang/ && !g { bad = 1 } END { exit bad }'
    check "install_deps_${d}: the PPA and apt.llvm.org only after the toolchain image check" 0 $?
    printf '%s\n' "$body" | grep -q "if ! use_baked_toolchain ${d}; then"
    check "install_deps_${d}: checks for its own toolchain image" 0 $?
  done
else
  check "use_baked_toolchain is defined" 0 1
fi

[ "$FAILS" = 0 ] && echo "all passed" || { echo "$FAILS failed"; exit 1; }
