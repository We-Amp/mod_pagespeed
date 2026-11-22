#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Packs the `pagespeed lint` binary as a release file:
#   pagespeed-lint-<version>-linux-<arch>.tar.gz
# holding pagespeed-lint-<version>/{pagespeed, LICENSE, NOTICE,
# pagespeed-lint.schema.json}. <version> is the module version of the source
# tree (MAJOR.MINOR.BUILD[-PRERELEASE] from net/instaweb/public/VERSION), the
# same spelling as the nginx module tarball. The binary is stripped; the
# archive has fixed owners, modes and times, so the same binary gives the same
# bytes.
#
# usage: package_pagespeed_lint.sh <binary> <arch label, e.g. x86_64> <out dir>
set -euo pipefail

BIN="$1"
ARCH_LABEL="$2"
OUT="$3"
SRCDIR="$(cd "$(dirname "$0")/../.." && pwd)"

# shellcheck disable=SC1091
source "${SRCDIR}/net/instaweb/public/VERSION"
if [ -n "${PRERELEASE:-}" ]; then
  VERSION="${MAJOR}.${MINOR}.${BUILD}-${PRERELEASE}"
else
  VERSION="${MAJOR}.${MINOR}.${BUILD}"
fi
NAME="pagespeed-lint-${VERSION}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/${NAME}" "${OUT}"
chmod 755 "${WORK}/${NAME}"
install -m 755 "${BIN}" "${WORK}/${NAME}/pagespeed"
strip --strip-unneeded "${WORK}/${NAME}/pagespeed"
install -m 644 "${SRCDIR}/LICENSE" "${SRCDIR}/NOTICE" \
  "${SRCDIR}/pagespeed/tools/cli/pagespeed-lint.schema.json" "${WORK}/${NAME}/"

TARBALL="${OUT}/${NAME}-linux-${ARCH_LABEL}.tar.gz"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
  -C "${WORK}" -cf - "${NAME}" | gzip -n -9 > "${TARBALL}"
echo "${TARBALL}"
