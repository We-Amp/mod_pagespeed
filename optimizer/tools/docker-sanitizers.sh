#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run all sanitizer tests in Docker (for pre-commit hook).
# This ensures leak detection works regardless of host platform.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== Running tests in Docker ==="
# Split into two steps: libaom's cmake build uses nproc parallelism internally.
# The cmake_build rule's resource_set prevents Bazel from co-scheduling heavy
# actions, but --jobs=2 in phase 2 adds an extra guard for Docker memory limits.
# First, run all tests except those that transitively depend on libaom.
"$SCRIPT_DIR/docker-test.sh" test --local_test_jobs=4 \
  -- //... \
  -//test/src/worker:image_transcoder_test \
  -//test/src/worker:worker_test \
  -//test/e2e:pipeline_test

echo ""
echo "=== Running libaom-dependent tests in Docker (reduced parallelism) ==="
# Limit Bazel concurrency to prevent a second heavy action alongside cmake.
"$SCRIPT_DIR/docker-test.sh" test --local_test_jobs=2 --jobs=2 \
  //test/src/worker:image_transcoder_test \
  //test/src/worker:worker_test \
  //test/e2e:pipeline_test

echo ""
echo "=== Running ASan + UBSan + LeakSanitizer in Docker ==="
# Exclude tests that transitively depend on libaom (via libavif): the libaom
# cmake build under sanitizer instrumentation OOMs in Docker containers. These
# tests are validated in the normal (non-sanitizer) test runs above.
# ed25519 shift UB and libpng alignment UB are suppressed via per-target
# -fno-sanitize flags in third_party/*.BUILD files. TL2cgen generated predictor
# files use -fno-sanitize=all in lib/image/generated/BUILD to prevent OOM.
# gif_reader_test (159 GIF decodes) OOMs under ASan memory overhead in Docker;
# validated in normal test runs above.
"$SCRIPT_DIR/docker-test.sh" test --local_test_jobs=4 --config=asan-leaks \
  -- //... \
  -//test/src/worker:image_transcoder_test \
  -//test/src/worker:worker_test \
  -//test/e2e:pipeline_test \
  -//test/lib/image:gif_reader_test

echo ""
echo "=== Running TSan in Docker (with suppressions) ==="
# Most image/pipeline tests already have no_tsan BUILD tags, filtered by
# .bazelrc test:tsan --test_tag_filters=-no_tsan.
# Only worker_test needs manual exclusion (multi-threaded + libaom OOM).
# --jobs=2 prevents concurrent heavy actions alongside cmake under TSan.
"$SCRIPT_DIR/docker-test.sh" test --local_test_jobs=1 --jobs=2 --config=tsan-suppressed \
  -- //... \
  -//test/src/worker:worker_test

echo ""
echo "All sanitizer tests passed."
