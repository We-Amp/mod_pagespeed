#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Release assertion: the portable nginx module tarballs name the nginx they
# are built for, and it is the nginx the build image ships.
#
#   assert_nginx_tarball_version.sh [--dockerfile PATH] <assets-dir>
#
# An nginx dynamic module loads only into the exact nginx version it was built
# against, so for each tarball ngx_pagespeed-<ver>-linux-<arch>.tar.gz
# (x86_64 and aarch64, both required) three versions must be equal:
#   * NGINX_VERSION in docker/Dockerfile (the nginx source the build image
#     ships and Bazel builds the module against; --dockerfile overrides the
#     path, default: this checkout's docker/Dockerfile);
#   * the "Built for nginx: X.Y.Z" line of the tarball's README.md;
#   * the nginx version string compiled into ngx_pagespeed_module.so (the
#     module embeds NGINX_VERSION as a string of its own).
# Prints "nginx version ok: <tarball> <version>" per tarball; exits 1 on any
# mismatch, on a missing tarball, README line or module, and when the
# Dockerfile does not name exactly one version (fail closed). Self-test:
# tools/ci/test_assert_nginx_tarball_version.sh

set -euo pipefail
# The module is binary: byte semantics for tr and grep.
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DOCKERFILE="${ROOT}/docker/Dockerfile"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --dockerfile)
      [ "$#" -ge 2 ] || { echo "error: --dockerfile needs a value" >&2; exit 2; }
      DOCKERFILE="$2"; shift 2 ;;
    *) break ;;
  esac
done
if [ "$#" -ne 1 ]; then
  echo "usage: $0 [--dockerfile PATH] <assets-dir>" >&2
  exit 2
fi
DIR="$1"
[ -d "$DIR" ] || { echo "error: no such directory: ${DIR}" >&2; exit 1; }
[ -f "$DOCKERFILE" ] || { echo "error: no such file: ${DOCKERFILE}" >&2; exit 1; }

VER_RE='^[0-9]+\.[0-9]+\.[0-9]+$'

mapfile -t pins < <(sed -n 's/^ENV[[:space:]]\{1,\}NGINX_VERSION=\([^[:space:]]*\).*/\1/p' "$DOCKERFILE")
if [ "${#pins[@]}" -ne 1 ] || ! [[ "${pins[0]}" =~ $VER_RE ]]; then
  echo "error: ${DOCKERFILE} must set exactly one ENV NGINX_VERSION=X.Y.Z, got '${pins[*]-}'" >&2
  exit 1
fi
WANT="${pins[0]}"
echo "build image nginx: ${WANT} (${DOCKERFILE})"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

bad=0
for arch in x86_64 aarch64; do
  shopt -s nullglob
  tars=("$DIR"/ngx_pagespeed-*-linux-"${arch}".tar.gz)
  shopt -u nullglob
  if [ "${#tars[@]}" -ne 1 ]; then
    echo "missing: expected one ngx_pagespeed-*-linux-${arch}.tar.gz in ${DIR}, found ${#tars[@]}"
    bad=1
    continue
  fi
  tarball="${tars[0]}"
  name="$(basename "$tarball")"
  x="${TMP}/${arch}"
  mkdir -p "$x"
  tar xzf "$tarball" -C "$x"
  readme="$(find "$x" -mindepth 2 -maxdepth 2 -name README.md | head -n 1)"
  so="$(find "$x" -mindepth 2 -maxdepth 2 -name ngx_pagespeed_module.so | head -n 1)"
  if [ -z "$readme" ] || [ -z "$so" ]; then
    echo "missing: ${name} has no top-level README.md or ngx_pagespeed_module.so"
    bad=1
    continue
  fi

  # README: the one "Built for nginx: X.Y.Z" line. Older READMEs carried the
  # version in prose ("built against the dev nginx (currently X.Y.Z)"); it is
  # read only to name the stale value, and still fails.
  mapfile -t rv < <(sed -n 's/^Built for nginx:[[:space:]]*\([^[:space:]]*\)[[:space:]]*$/\1/p' "$readme")
  if [ "${#rv[@]}" -ne 1 ]; then
    legacy="$(tr '\n' ' ' < "$readme" | sed -n 's/.*built against the dev nginx (currently[[:space:]]*\([0-9][0-9.]*[0-9]\)).*/\1/p')"
    echo "mismatch: ${name} README.md has ${#rv[@]} 'Built for nginx:' line(s)${legacy:+; it says the module is for nginx ${legacy}}, build image nginx is ${WANT}"
    bad=1
    continue
  fi
  readme_ver="${rv[0]}"

  # Module: NUL-separated strings, so an exact line match is an exact string.
  # (grep -c, not -q: -q stops reading early and pipefail then fails on tr.)
  if [ "$(tr '\0' '\n' < "$so" | grep -cFx -- "$WANT" || true)" -gt 0 ]; then
    so_ver="$WANT"
  else
    so_ver="none of: $(tr '\0' '\n' < "$so" | grep -E "$VER_RE" | sort -u | paste -sd, - || true)"
  fi

  if [ "$readme_ver" != "$WANT" ] || [ "$so_ver" != "$WANT" ]; then
    echo "mismatch: ${name}: README.md nginx ${readme_ver}, module nginx ${so_ver}, build image nginx ${WANT}"
    bad=1
    continue
  fi
  echo "nginx version ok: ${name} ${WANT}"
done

if [ "$bad" -ne 0 ]; then
  echo "error: the nginx module tarballs do not all name the nginx they are built for" >&2
  exit 1
fi
