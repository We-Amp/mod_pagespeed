#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Build a distributable Envoy PageSpeed package.
#
# Usage:
#   ./install/envoy/build_envoy_package.sh [-o output_dir]
#
# Prerequisites:
#   - Bazel build of //pagespeed/envoy:envoy_pagespeed and
#     //pagespeed/envoy:pagespeed_filter.so completed

set -euo pipefail

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "${SCRIPTDIR}/../.." && pwd)"
OUTPUTDIR="${PWD}"

while getopts ":o:h" opt; do
  case $opt in
    o) OUTPUTDIR="$OPTARG"; mkdir -p "${OUTPUTDIR}" ;;
    h) echo "Usage: $(basename "$0") [-o output_dir]"; exit 0 ;;
    *) echo "Invalid option: -$OPTARG"; exit 1 ;;
  esac
done

# Read version
source "${SRCDIR}/net/instaweb/public/VERSION"
if [ -n "${PRERELEASE:-}" ]; then
  VERSION="${MAJOR}.${MINOR}.${BUILD}-${PRERELEASE}"
else
  VERSION="${MAJOR}.${MINOR}.${BUILD}"
fi

PKGNAME="envoy-pagespeed-${VERSION}"
PKGDIR="${OUTPUTDIR}/${PKGNAME}"
BINARY_PATH="${SRCDIR}/bazel-bin/pagespeed/envoy/envoy_pagespeed"
SO_PATH="${SRCDIR}/bazel-bin/pagespeed/envoy/pagespeed_filter.so"

# Check at least one artifact exists
if [ ! -f "${BINARY_PATH}" ] && [ ! -f "${SO_PATH}" ]; then
  echo "ERROR: No Envoy PageSpeed artifacts found."
  echo "Build them first:"
  echo "  bazel build --config=clang-libstdcxx13 //pagespeed/envoy:envoy_pagespeed //pagespeed/envoy:pagespeed_filter.so"
  exit 1
fi

echo "Creating Envoy PageSpeed package: ${PKGNAME}"

rm -rf "${PKGDIR}"
mkdir -p "${PKGDIR}"

# Copy binaries
if [ -f "${BINARY_PATH}" ]; then
  cp "${BINARY_PATH}" "${PKGDIR}/envoy_pagespeed"
fi
if [ -f "${SO_PATH}" ]; then
  cp "${SO_PATH}" "${PKGDIR}/pagespeed_filter.so"
fi

# Copy sample config
if [ -f "${SRCDIR}/pagespeed-envoy.yaml" ]; then
  cp "${SRCDIR}/pagespeed-envoy.yaml" "${PKGDIR}/pagespeed-envoy.yaml.sample"
fi

# Create README
cat > "${PKGDIR}/README.md" << EOF
# Envoy PageSpeed ${VERSION}

Envoy proxy with integrated PageSpeed web optimization filter.

**Status: Experimental**

## Contents

- \`envoy_pagespeed\` - Standalone Envoy binary with PageSpeed (~212MB)
- \`pagespeed_filter.so\` - Shared library for existing Envoy (~113MB)
- \`pagespeed-envoy.yaml.sample\` - Sample configuration

## Quick Start

### Standalone binary

\`\`\`bash
./envoy_pagespeed -c pagespeed-envoy.yaml.sample
\`\`\`

### Shared library with existing Envoy

Start Envoy with the PageSpeed filter loaded:
\`\`\`bash
envoy -c your-config.yaml --extension-allowlist pagespeed_filter.so
\`\`\`

## Configuration

See \`pagespeed-envoy.yaml.sample\` for a complete configuration example
including admin authentication, Redis cache, and Prometheus metrics.

## Known Limitations

- IPRO cache lifetime differs from Apache (returns upstream max-age minus cache time)
- X-PSA-Blocking-Rewrite header is not supported (Envoy is non-blocking)
- combine_css may timeout in some configurations
- See docs/envoy-limitations.md for full details

## More Information

- https://github.com/we-amp/mod_pagespeed
EOF

# Create tarball
cd "${OUTPUTDIR}"
tar czf "${PKGNAME}-linux-x86_64.tar.gz" "${PKGNAME}/"
rm -rf "${PKGDIR}"

echo "Package created: ${OUTPUTDIR}/${PKGNAME}-linux-x86_64.tar.gz"
