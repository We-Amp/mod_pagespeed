#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Seed cache for t/302-variant-format.t tests (issue #1335: the serve path
# re-validates the selected variant's image format against the request's
# negotiated capability mask, mirroring the #1334 selection-layer rule).
# Called by tools/run-test-nginx.sh or the test's BEGIN block.
# Supports both local (bazel-bin/) and Docker (SEED_TEST_CACHE env var).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Allow override via environment (used by docker-test-nginx.sh)
SEED="${SEED_TEST_CACHE:-${REPO_ROOT}/bazel-bin/tools/seed_test_cache}"

if [ ! -x "$SEED" ]; then
  echo "Error: seed_test_cache not found at $SEED"
  echo "  Local: bazel build //tools:seed_test_cache"
  echo "  Docker: set SEED_TEST_CACHE env var"
  exit 1
fi

# Entries go to the keyspace of the Test::Nginx server block the test
# requests reach: its listener and its server names.
SITE=(--site_listen "0.0.0.0:${TEST_NGINX_SERVER_PORT:-${TEST_NGINX_PORT:-1984}}"
  --site_names 'localhost')

VOL=/tmp/test-ps-302-1.vol

# Drop leftovers from previous runs (the volume, its .gen stamp, .small
# sibling, and Cyclone's fingerprint-named siblings). The MISS-fallthrough
# tests below record the upstream response at the request's mask; without a
# wipe a repeated prove run would HIT those recorded entries instead of
# falling through, turning the expected MISS into a HIT.
rm -f "${VOL}"* "${VOL%.vol}"-*.vol

# Mask byte layout: bits 0-1 image format (0=Original, 1=WebP, 2=AVIF,
# 3=SVG), bits 2-3 viewport (2=Desktop).  0x08 = Original/Desktop/1x/off/
# identity (the mask a request negotiates when its Accept names no image
# format); 0x09 = WebP, 0x0B = SVG at the same dimensions.

# WebP-only variants (one URL per test so a MISS-side recording for one URL
# can never become another test's fallback variant).
"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/m1.jpg" \
  --host "localhost" \
  --scheme http \
  --content "WEBP-BYTES-1" \
  --content_type image \
  --mask 0x09

"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/m2.jpg" \
  --host "localhost" \
  --scheme http \
  --content "WEBP-BYTES-2" \
  --content_type image \
  --mask 0x09

"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/m3.jpg" \
  --host "localhost" \
  --scheme http \
  --content "WEBP-BYTES-3" \
  --content_type image \
  --mask 0x09

# Original-format image variant: the universal fallback — must serve even to
# a client that negotiated WebP.
"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/m4.jpg" \
  --host "localhost" \
  --scheme http \
  --content "JPEG-BYTES-4" \
  --content_type image \
  --mask 0x08

# SVG variant: universal like Original — must serve to a client that
# negotiated no image format at all.
"$SEED" "${SITE[@]}" \
  --cache "$VOL" \
  --url "/m5.svg" \
  --host "localhost" \
  --scheme http \
  --content "<svg xmlns=\"http://www.w3.org/2000/svg\"/>" \
  --content_type image \
  --mask 0x0B
