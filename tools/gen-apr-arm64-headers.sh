#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Regenerate the linux/arm64 APR + apr-util pre-generated configure headers by
# running the real ./configure on aarch64 Linux, in the same base image the
# arm64 CI lane compiles in. Requires arm64 Docker (native on Apple Silicon).
#
# Reads the pinned APR/apr-util commits from bazel/repositories.bzl and the CI
# base image from docker/Dockerfile, so it never drifts from what CI builds.
# See third_party/apr/gen/README.md for background.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

APR_COMMIT="$(sed -nE 's/^APR_COMMIT = "([0-9a-f]+)".*/\1/p' bazel/repositories.bzl)"
APRUTIL_COMMIT="$(sed -nE 's/^APRUTIL_COMMIT = "([0-9a-f]+)".*/\1/p' bazel/repositories.bzl)"
BASE_IMAGE="$(sed -nE 's/^FROM (envoyproxy\/envoy-build-ubuntu@sha256:[0-9a-f]+).*/\1/p' docker/Dockerfile | head -1)"
[ -n "$APR_COMMIT" ] && [ -n "$APRUTIL_COMMIT" ] && [ -n "$BASE_IMAGE" ] || {
  echo "Could not resolve pinned commits/image" >&2; exit 1; }
echo "APR=$APR_COMMIT APRUTIL=$APRUTIL_COMMIT"
echo "IMAGE=$BASE_IMAGE"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
curl -fsSL -o "$WORK/apr.tar.gz"     "https://github.com/apache/apr/archive/${APR_COMMIT}.tar.gz"
curl -fsSL -o "$WORK/aprutil.tar.gz" "https://github.com/apache/apr-util/archive/${APRUTIL_COMMIT}.tar.gz"

cat > "$WORK/run.sh" <<'INNER'
set -euo pipefail
cd /work
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  autoconf libtool libtool-bin make gcc python3 \
  libexpat1-dev libsqlite3-dev libpq-dev uuid-dev >/dev/null
uname -m; ldd --version | head -1
rm -rf apr aprutil; mkdir apr aprutil
tar xf apr.tar.gz     -C apr     --strip-components=1
tar xf aprutil.tar.gz -C aprutil --strip-components=1
( cd apr     && ./buildconf && CC=gcc ./configure >/tmp/apr.log 2>&1 )
( cd aprutil && ./buildconf --with-apr=/work/apr && CC=gcc ./configure --with-apr=/work/apr >/tmp/aprutil.log 2>&1 )
O=/work/tree
rm -rf "$O"
mkdir -p "$O/apr/include" "$O/aprutil/include/private"
cp apr/include/apr.h                        "$O/apr/include/apr.h"
cp apr/include/arch/unix/apr_private.h      "$O/apr/include/apr_private.h"
cp aprutil/include/apu.h                    "$O/aprutil/include/apu.h"
cp aprutil/include/apu_want.h               "$O/aprutil/include/apu_want.h"
cp aprutil/include/apr_ldap.h               "$O/aprutil/include/apr_ldap.h"
cp aprutil/include/private/apu_config.h     "$O/aprutil/include/private/apu_config.h"
cp aprutil/include/private/apu_select_dbm.h "$O/aprutil/include/private/apu_select_dbm.h"
INNER

docker run --rm --platform linux/arm64 -v "$WORK":/work "$BASE_IMAGE" bash /work/run.sh

DST_APR="third_party/apr/gen/arch/linux/arm64/include"
DST_APU="third_party/aprutil/gen/arch/linux/arm64/include"
mkdir -p "$DST_APR" "$DST_APU/private"
cp "$WORK/tree/apr/include/"* "$DST_APR/"
cp "$WORK/tree/aprutil/include/apu.h" "$WORK/tree/aprutil/include/apu_want.h" \
   "$WORK/tree/aprutil/include/apr_ldap.h" "$DST_APU/"
cp "$WORK/tree/aprutil/include/private/"* "$DST_APU/private/"
echo "Wrote:"
find "$DST_APR" "$DST_APU" -type f | sort
