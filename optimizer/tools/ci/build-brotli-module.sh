#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build ngx_http_brotli_filter_module.so and stage it into <dir>/.ci-artifacts/.
#
# Usage: tools/ci/build-brotli-module.sh <dir>
#   <dir> is a checkout root that already contains:
#     .ci-artifacts/nginx-version   (recorded by the Linux Build "Build all
#                                    targets" step, or derived by
#                                    stage-artifacts.sh)
#     tools/e2e/Dockerfile.brotli
#
# The brotli module is a pure function of (nginx version, pinned ngx_brotli
# commit), so the Docker layer cache — keyed on the nginx-version COPY'd first
# in Dockerfile.brotli — makes this a no-op unless nginx.org ships a new point
# release. Pre-building it here on the warm-cache producer host (the x64 Linux
# Build job) and shipping the .so as a CI artifact means the HTTP Compliance
# job, which can land on any light runner, never recompiles brotli on a
# cold-cache host. See
# Dockerfile.brotli, Dockerfile.prebuilt, and stage-artifacts.sh.
set -euo pipefail

DIR="${1:?Usage: build-brotli-module.sh <dir-with-.ci-artifacts/nginx-version>}"
cd "$DIR"

[ -s .ci-artifacts/nginx-version ] || {
  echo "::error::.ci-artifacts/nginx-version missing/empty in ${DIR} — run after the module build records it." >&2
  exit 1
}
NV="$(cat .ci-artifacts/nginx-version)"

IMAGE="pagespeed2-brotli:${NV}"
echo "Building brotli module for nginx ${NV} (image ${IMAGE})..."
docker build -t "$IMAGE" -f tools/e2e/Dockerfile.brotli .

cid="$(docker create "$IMAGE")"
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
docker cp "$cid:/out/ngx_http_brotli_filter_module.so" \
  .ci-artifacts/ngx_http_brotli_filter_module.so

echo "Staged brotli module:"
ls -lh .ci-artifacts/ngx_http_brotli_filter_module.so
