#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# bake-toolchain.sh RECIPE: runs INSIDE a toolchain image build (see
# Dockerfile.focal and Dockerfile.jammy in this directory). Installs the same
# compiler set the nginx build recipe installs for focal and jammy today:
#   - g++-13, gcc-13 and libstdc++-13-dev from the Ubuntu toolchain PPA, and
#   - clang, lld and llvm 18 from apt.llvm.org.
#
# The recipe (install/nginx/build_module_in_container.sh, passed as RECIPE) is
# the single home of the repository keys and their pinned fingerprints, so this
# script does not copy them: it takes the key variables and the four functions
# out of the recipe and calls them as they are, the way
# tools/ci/test_build_nginx_verify.sh does. Every key is therefore still
# verified against its pinned fingerprint, and a recipe change reaches the
# images on their next build.
#
# At the end the third-party apt sources are removed again, so that a later
# `apt-get update` in a container made from the image does not contact the PPA
# or apt.llvm.org. The installed packages and /etc/pagespeed-toolchain-baked
# (the versions that went in) stay.

set -euo pipefail

RECIPE="${1:?usage: bake-toolchain.sh RECIPE}"
CLANG_VER=18

export DEBIAN_FRONTEND=noninteractive

eval "$(grep -E '^(LLVM_APT_KEY_FPR|TOOLCHAIN_PPA_KEY_FPR)=' "${RECIPE}")"
for fn in pinned_keyring add_llvm_apt_repo add_toolchain_ppa install_llvm_clang; do
  body="$(sed -n "/^${fn}()/,/^}/p" "${RECIPE}")"
  [ -n "${body}" ] || { echo "ERROR: ${fn} not found in ${RECIPE}" >&2; exit 1; }
  eval "${body}"
done
[ -n "${LLVM_APT_KEY_FPR:-}" ] && [ -n "${TOOLCHAIN_PPA_KEY_FPR:-}" ] \
  || { echo "ERROR: the pinned key fingerprints are missing from ${RECIPE}" >&2; exit 1; }

echo 'Acquire::Retries "3";' > /etc/apt/apt.conf.d/99-toolchain-retries

apt-get update -qq
apt-get install -y --no-install-recommends ca-certificates wget gnupg lsb-release >/dev/null

# The PPA's mirror answers 503 for minutes at a time, for package files as well
# as for the index (add_toolchain_ppa already retries the index), so the installs
# are retried here too. A failed attempt leaves nothing in the image: the build
# only keeps the result of a script that succeeded.
retry() {
  local attempt
  for attempt in 1 2 3 4 5 6; do
    "$@" && return 0
    echo "'$*' failed (attempt ${attempt} of 6)" >&2
    [ "$attempt" -lt 6 ] && sleep $((attempt * 30))
  done
  return 1
}

# Same order as install_deps_jammy and install_deps_focal.
add_toolchain_ppa
retry apt-get install -y --no-install-recommends g++-13 gcc-13 libstdc++-13-dev >/dev/null
install_llvm_clang "${CLANG_VER}"

g++-13 --version | head -n 1
clang-"${CLANG_VER}" --version | head -n 1

{
  echo "# Written by tools/ci/toolchain-images/bake-toolchain.sh"
  # shellcheck disable=SC1091
  . /etc/os-release
  echo "suite=${VERSION_CODENAME}"
  echo "toolchain_ppa_key_fpr=${TOOLCHAIN_PPA_KEY_FPR}"
  echo "llvm_key_fpr=${LLVM_APT_KEY_FPR}"
  dpkg-query -W -f='${Package}=${Version}\n' \
    g++-13 gcc-13 libstdc++-13-dev libstdc++6 libgcc-s1 libgomp1 gcc-16-base "clang-${CLANG_VER}" "lld-${CLANG_VER}" "llvm-${CLANG_VER}" \
    | sed 's/^/pkg:/'
} > /etc/pagespeed-toolchain-baked

rm -f /etc/apt/sources.list.d/ubuntu-toolchain-r.list "/etc/apt/sources.list.d/llvm-${CLANG_VER}.list"
apt-get clean
rm -rf /var/lib/apt/lists/*
