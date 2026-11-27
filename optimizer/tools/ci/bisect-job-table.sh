# shellcheck shell=bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Per-repository data for tools/ci/bisect-job.sh (docs/ci/pipelines.md,
# "Bisecting a red full-tier job"). Sourced, never run. Data only: the
# bisect driver is the same file in every repository, and what differs
# lives here. tools/ci/test_bisect_job.sh checks every value below against
# .github/workflows/pipeline-full.yml, so a workflow edit that is not
# mirrored here fails that test.
#
# Per job (JOB is the job key upper-cased, '-' as '_'):
#   _NAME              the job's `name:` in pipeline-full.yml
#   _KIND              test | build (what a bisect step runs)
#   _CONFIG            the .bazelrc config the job passes (--config=...)
#   _BAZEL_FLAGS       the job's other Bazel flags without a cache host
#   _OUTPUT_ROOT       the container-local --output_user_root
#   _DOCKER_FLAGS      container flags a bazelrc cannot hold
#   _ENV               container environment (KEY=VALUE words)
#   _PASSWD            1: mount a passwd file naming the host user `dev`
#   _SERVICES          service containers the job starts (image tags are read
#                      from the bisected commit's pipeline-full.yml)
#   _SERVICE_TEST_ENV  --test_env values beside <SERVICE>_HOST

# shellcheck disable=SC2034 # read by the sourcing script

BISECT_REPO_NAME=pagespeed-optimizer
BISECT_GH_REPO=We-Amp/pagespeed-optimizer
BISECT_JOBS="linux-asan linux-tsan linux-build"

# The workflow's vendor cache key: hashFiles() over these paths.
BISECT_VENDOR_KEY_PATHS="MODULE.bazel .bazelversion bazel third_party"

# Populate vendor/ in the current directory (the bisected commit's tree).
bisect_vendor_cmd() {
  bazel vendor --vendor_dir=vendor --repository_cache="$HOME/.cache/optimizer-bazel-repo" //...
}

BISECT_LINUX_ASAN_NAME="Linux ASan + UBSan"
BISECT_LINUX_ASAN_KIND="test"
BISECT_LINUX_ASAN_CONFIG=ci-asan
BISECT_LINUX_ASAN_BAZEL_FLAGS="--nostamp"
BISECT_LINUX_ASAN_OUTPUT_ROOT=/tmp/bazel-out-asan
BISECT_LINUX_ASAN_DOCKER_FLAGS="--shm-size=512m"
BISECT_LINUX_ASAN_ENV="HOME=/workspace"
BISECT_LINUX_ASAN_PASSWD=0
BISECT_LINUX_ASAN_SERVICES=""
BISECT_LINUX_ASAN_SERVICE_TEST_ENV=""

BISECT_LINUX_TSAN_NAME="Linux TSan"
BISECT_LINUX_TSAN_KIND="test"
BISECT_LINUX_TSAN_CONFIG=ci-tsan
BISECT_LINUX_TSAN_BAZEL_FLAGS="--nostamp"
BISECT_LINUX_TSAN_OUTPUT_ROOT=/tmp/bazel-out-tsan
BISECT_LINUX_TSAN_DOCKER_FLAGS="--shm-size=512m --security-opt seccomp=unconfined"
BISECT_LINUX_TSAN_ENV="HOME=/workspace"
BISECT_LINUX_TSAN_PASSWD=0
BISECT_LINUX_TSAN_SERVICES=""
BISECT_LINUX_TSAN_SERVICE_TEST_ENV=""

BISECT_LINUX_BUILD_NAME="Linux Build"
BISECT_LINUX_BUILD_KIND="build"
BISECT_LINUX_BUILD_CONFIG=ci-build
BISECT_LINUX_BUILD_BAZEL_FLAGS="--config=docker --nostamp"
BISECT_LINUX_BUILD_OUTPUT_ROOT=/tmp/bazel-out
BISECT_LINUX_BUILD_DOCKER_FLAGS=""
BISECT_LINUX_BUILD_ENV="HOME=/workspace"
BISECT_LINUX_BUILD_PASSWD=0
BISECT_LINUX_BUILD_SERVICES=""
BISECT_LINUX_BUILD_SERVICE_TEST_ENV=""
