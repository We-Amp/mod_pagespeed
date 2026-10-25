#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# envoy-filter-build.sh - Build the Envoy filter (against Envoy 1.40 from
# MODULE.bazel) and run its unit tests in the dev image. Used by the
# envoy-filter jobs of the full and nightly tiers.
#
# Environment (set by the calling job):
#   DOCKER_IMAGE      the dev image (tools/ci/ensure-dev-image.sh)
#   REPO_CACHE_DIR    a per-runner Bazel repository cache directory
#   RUNNER_TEMP       holds ci-passwd (the container identity)
#   BAZEL_CACHE_HOST  optional: the remote cache host
#   CI_SLOT_CPUS / CI_SLOT_MEM_GB  optional: the slot budget
#
# Every target in pagespeed/envoy and test/pagespeed/envoy is tagged manual,
# so the filter's deliverables and tests are named here.

set -euo pipefail

: "${DOCKER_IMAGE:?}" "${REPO_CACHE_DIR:?}" "${RUNNER_TEMP:?}"
WS="${GITHUB_WORKSPACE:-$(pwd)}"

# Per-slot budget (CI_SLOT_CPUS / CI_SLOT_MEM_GB); legacy values when unset.
# shellcheck source=tools/ci/slot-budget.sh
source "${WS}/tools/ci/slot-budget.sh"
slot_budget 55g || exit 1

EXTRA_DOCKER=()
BAZEL_CONFIG="--config=envoy-filter --config=remote-cache --nostamp"
if [ -n "${BAZEL_CACHE_HOST:-}" ]; then
  EXTRA_DOCKER+=(--add-host="bazel-remote-cache:${BAZEL_CACHE_HOST}")
else
  BAZEL_CONFIG="--config=envoy-filter --nostamp --jobs=${SLOT_JOBS}"
  echo "::notice::BAZEL_CACHE_HOST not set: building without the remote cache (docs/ci/runner-environment.md)"
fi

docker run --rm --init "${EXTRA_DOCKER[@]}" \
  ${SLOT_CPUS:+--cpus="${SLOT_CPUS}"} \
  --memory="${SLOT_MEM}" --memory-swap="${SLOT_MEM}" \
  --user "$(id -u):$(id -g)" \
  -v "${RUNNER_TEMP}/ci-passwd:/etc/passwd:ro" \
  -e HOME=/workspace -e USER=dev \
  -e PATH=/opt/llvm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  -e BAZEL_CONFIG="${BAZEL_CONFIG}${SLOT_CPUS:+ --jobs=${SLOT_CPUS}}" \
  -v "${WS}:/workspace" \
  -v "${REPO_CACHE_DIR}:/repo-cache" \
  -w /workspace \
  "${DOCKER_IMAGE}" \
  bash -c '
    set -e
    bash tools/ci/check-bazel-version.sh
    # The output root is inside the container: every run re-analyses Envoy
    # (about a minute); the remote cache carries the actions.
    # shellcheck disable=SC2086
    bazel --output_user_root=/tmp/bazel-out-envoy \
      test ${BAZEL_CONFIG} \
      --repository_cache=/repo-cache \
      --action_env=CC=clang --action_env=CXX=clang++ \
      --test_output=errors --nocache_test_results \
      -- //pagespeed/envoy:envoy_pagespeed \
      //pagespeed/envoy:pagespeed_filter.so \
      //test/pagespeed/envoy:envoy_vhost_config_test \
      //test/pagespeed/envoy:envoy_html_rewriter_test \
      //test/pagespeed/envoy:envoy_server_context_test \
      //test/pagespeed/envoy:http_filter_test
  '
