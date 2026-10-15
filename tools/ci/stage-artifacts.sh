#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Stage pre-built CI artifacts for Dockerfile.prebuilt consumption.
#
# Usage: tools/ci/stage-artifacts.sh <source-dir>
#
# Copies factory_worker, ngx_pagespeed_module.so, the brotli module, and
# libc++ runtime libs from <source-dir>/.ci-artifacts/ to .ci-artifacts/ in
# the current directory (which should be the repo checkout root).
set -euo pipefail

SRC="${1:?Usage: stage-artifacts.sh <source-dir>}"

# Fail fast with a clear error if Docker isn't healthy -- we'll need it
# below to build pagespeed2-base-runtime, and the cascade error from a
# raw `docker build` is much harder to diagnose.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"${SCRIPT_DIR}/docker-preflight.sh"

# Invalidate a STALE runner-local bundle before the existence check below.
# Persistent CI runners keep ${SRC}/.ci-artifacts/ across jobs, so a
# prior (different-SHA) Build's factory_worker can satisfy the existence check
# and be used silently -- an HTTP-Compliance 404 was once traced to a stale
# ngx_pagespeed_module.so from a pre-merge build sitting on the runner. The
# Linux Build job's "Upload CI artifacts" step records the producing commit in
# metadata.json; if it disagrees with this run's GITHUB_SHA, wipe the staging
# dir so the SHA-keyed the CI hub fetch below pulls the correct bundle. No-op when
# metadata.json is absent (old/foreign bundle) -- same behaviour as before.
# Mirrors the same stale-bundle guard the packaging/native-asset consumers
# already apply (ci.yml "Download CI artifacts for packaging"). Skip when
# the bundle SHA is unset (local/dev invocation) so we never wipe based on an
# empty expected SHA.
#
# BUNDLE_SHA is the commit the bundle was published under. It is GITHUB_SHA
# unless CI_ARTIFACTS_SHA names another commit: a pull_request Build publishes
# under the PR's MERGE commit, while a workflow_dispatch on the same branch runs
# with GITHUB_SHA = the branch HEAD, for which no bundle exists. The
# dispatching workflow resolves the merge commit and passes it here.
BUNDLE_SHA="${CI_ARTIFACTS_SHA:-${GITHUB_SHA:-}}"
if [[ "${BUNDLE_SHA}" =~ ^[0-9a-f]{40}$ ]]; then
  "${SCRIPT_DIR}/invalidate-stale-artifacts.sh" "${SRC}/.ci-artifacts" "${BUNDLE_SHA}"
fi

# Validate what is left as a COMPLETE bundle, not just "factory_worker exists":
# a same-SHA bundle from another producer (the Release set has no
# ngx_pagespeed_module.so), a half-copied bundle, or one truncated by an
# interrupted `rsync --inplace` used to pass and fail later at `install`. The
# checks are file presence, the bundle's own checksums.sha256 when it ships one,
# and metadata.json's platform when present. A bundle WITHOUT metadata.json
# (arm64 periodic local builds, or a Build still writing it) gets no commit or
# platform check.
#
# A rejected bundle is NEVER deleted here: ${SRC}/.ci-artifacts is a shared
# runner directory, and an incomplete bundle may be a concurrent Build's output
# in progress (the light-tier consumers share WORKSPACE_DIR with the heavy
# producer). Instead the replacement is fetched into a per-job directory and
# installed from there.
BUNDLE_DIR="${SRC}/.ci-artifacts"
if "${SCRIPT_DIR}/validate-local-bundle.sh" "${BUNDLE_DIR}" "${CI_ARTIFACTS_PLATFORM:-linux-x64}" 2>"${TMPDIR:-/tmp}/validate-bundle.$$.err"; then
  LOCAL_BUNDLE_OK=1
else
  LOCAL_BUNDLE_OK=0
  if [ -d "${BUNDLE_DIR}" ]; then
    echo "::warning::Local bundle rejected ($(cat "${TMPDIR:-/tmp}/validate-bundle.$$.err")) -- fetching a fresh one into a per-job directory (the shared ${BUNDLE_DIR} is left alone)"
  fi
  BUNDLE_DIR="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/ci-artifacts-${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-0}-${GITHUB_JOB:-job}-$$"
  rm -rf "${BUNDLE_DIR}"
  # Later steps (e.g. provenance notes) can find the staged bundle here.
  if [ -n "${GITHUB_ENV:-}" ]; then echo "STAGED_BUNDLE_DIR=${BUNDLE_DIR}" >> "${GITHUB_ENV}"; fi
fi
rm -f "${TMPDIR:-/tmp}/validate-bundle.$$.err"

# If artifacts don't exist locally, try fetching from the CI hub via rsync.
# Retry with backoff to absorb transient network/SSH blips, but
# distinguish "upstream never produced artifacts" (rsync exit 23/24,
# directory missing) from "transient failure" -- the former is a hard
# failure that won't be fixed by retrying.
if [ "${LOCAL_BUNDLE_OK}" -ne 1 ]; then
  echo "::warning::Local CI artifacts not found in ${BUNDLE_DIR}/ -- fetching from the CI hub"
  mkdir -p "${BUNDLE_DIR}"

  # Defensive: the bundle SHA is normally a 40-char hex SHA (GITHUB_SHA from
  # GHA, or CI_ARTIFACTS_SHA from the dispatching workflow), but validate before
  # splicing it into a remote rsync path so an unexpected value can't traverse
  # outside the artifacts tree.
  if ! [[ "${BUNDLE_SHA}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "::error::bundle SHA is missing or not a 40-char hex SHA (CI_ARTIFACTS_SHA='${CI_ARTIFACTS_SHA:-<unset>}', GITHUB_SHA='${GITHUB_SHA:-<unset>}'); refusing to construct rsync path." >&2
    exit 1
  fi
  if [ -n "${CI_ARTIFACTS_SHA:-}" ] && [ "${CI_ARTIFACTS_SHA}" != "${GITHUB_SHA:-}" ]; then
    echo "Fetching the bundle published under ${CI_ARTIFACTS_SHA} (this run's GITHUB_SHA is ${GITHUB_SHA:-<unset>})."
  fi
  # Hub bundles exist for x64 platforms only. With no explicit platform, an
  # arm64 host would fetch linux-x64 binaries and then pass revalidation, so
  # refuse instead (set CI_ARTIFACTS_PLATFORM to a published arm64 variant).
  case "$(uname -m)" in
    aarch64|arm64)
      if [ -z "${CI_ARTIFACTS_PLATFORM:-}" ]; then
        echo "::error::local bundle at ${SRC}/.ci-artifacts is unusable and CI_ARTIFACTS_PLATFORM is unset on an arm64 host; refusing to fetch linux-x64 binaries." >&2
        exit 1
      fi
      ;;
  esac
  # CI_ARTIFACTS_PLATFORM selects the shared-dir variant to fetch. Default
  # linux-x64 is the PR tier's push bundle (--config=ci, with nginx-version +
  # brotli); the nightly tier's x64 test jobs set linux-x64-periodic to fetch
  # the --config=opt bundle their Build (x64) publishes, so the two producers
  # never read each other's binaries for the same SHA.
  # CI_HUB_SSH / CI_HUB_ARTIFACTS_ROOT come from the runner environment (see
  # docs/ci/runner-environment.md); nothing environment-specific is hardcoded
  # here.
  : "${CI_HUB_SSH:?CI_HUB_SSH must be set (runner environment; see docs/ci/runner-environment.md)}"
  : "${CI_HUB_ARTIFACTS_ROOT:?CI_HUB_ARTIFACTS_ROOT must be set (runner environment; see docs/ci/runner-environment.md)}"
  RSYNC_REMOTE="${CI_HUB_SSH}:${CI_HUB_ARTIFACTS_ROOT}/${BUNDLE_SHA}/${CI_ARTIFACTS_PLATFORM:-linux-x64}/"
  RSYNC_SSH='ssh -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=10'

  attempt=1
  max_attempts=3
  delay=5
  while :; do
    rc=0
    rsync -az -e "${RSYNC_SSH}" "${RSYNC_REMOTE}" "${BUNDLE_DIR}/" || rc=$?
    if [ "$rc" -eq 0 ]; then
      break
    fi
    # Exit-code classification policy:
    #   23/24 (partial transfer, usually missing source path) → TERMINAL.
    #     The upstream Linux Build job didn't publish artifacts; no amount
    #     of retry will conjure them.
    #   anything else non-zero → TRANSIENT by default (SSH/network blips,
    #     transient peer errors). This is conservative -- codes like 2/5/10/12
    #     are arguably terminal in theory, but in practice we've only seen
    #     them under transient network/SSH conditions, so retry is safe and
    #     retrying-then-failing is cheap. Do NOT widen the terminal set
    #     without evidence of a class of failure that retry masks.
    if [ "$rc" -eq 23 ] || [ "$rc" -eq 24 ]; then
      echo "::error::No artifacts at the hub bundle path for ${BUNDLE_SHA}/${CI_ARTIFACTS_PLATFORM:-linux-x64} (rsync exit ${rc})." >&2
      echo "::error::The upstream Linux Build job for SHA ${BUNDLE_SHA} did not publish artifacts -- check that job's log first; this rsync failure is a cascade. (A pull_request Build publishes under the PR merge commit, not the branch head; a workflow_dispatch on a PR branch must name that commit via CI_ARTIFACTS_SHA / the bundle_sha input.)" >&2
      exit 1
    fi
    if [ "$attempt" -ge "$max_attempts" ]; then
      echo "::error::rsync from the CI hub failed ${max_attempts} times (last exit ${rc}). Giving up." >&2
      exit 1
    fi
    echo "::warning::rsync attempt ${attempt}/${max_attempts} failed (exit ${rc}); retrying in ${delay}s..." >&2
    sleep "$delay"
    attempt=$((attempt + 1))
    delay=$((delay * 2))
  done

  if ! "${SCRIPT_DIR}/validate-local-bundle.sh" "${BUNDLE_DIR}" "${CI_ARTIFACTS_PLATFORM:-linux-x64}"; then
    echo "::error::rsync reported success but the fetched bundle under ${BUNDLE_DIR}/ is incomplete (see above); the producer published a partial set." >&2
    exit 1
  fi
fi

# Ensure pagespeed2-base-runtime exists AND matches the current Dockerfile.
# Persistent CI runners keep the image across jobs, so a plain
# "rebuild if missing" check (the previous behaviour) silently reuses a
# stale image when Dockerfile.base-runtime changes -- invisibly breaking
# downstream consumers (e.g. nginx version drift between builder and
# runtime → "module … version X instead of Y", HTTP Compliance red).
# Stamp the image with the SHA-256 of the Dockerfile and rebuild on drift.
EXPECTED_HASH=$(sha256sum docker/Dockerfile.base-runtime | awk '{print $1}')
ACTUAL_HASH=$(docker image inspect pagespeed2-base-runtime \
  --format '{{ index .Config.Labels "weamp.base-runtime.dockerfile_sha256" }}' 2>/dev/null || true)
if [ "$ACTUAL_HASH" != "$EXPECTED_HASH" ]; then
  echo "Rebuilding pagespeed2-base-runtime (Dockerfile sha mismatch: have='${ACTUAL_HASH}' want='${EXPECTED_HASH}')"
  # APT_REFRESH busts the apt dist-upgrade layer so a warm cache can't re-freeze
  # OS-package CVEs (the image CVE gate); pass today's UTC date fresh each run.
  docker build -t pagespeed2-base-runtime \
    --build-arg APT_REFRESH="$(date -u +%F)" \
    --label "weamp.base-runtime.dockerfile_sha256=${EXPECTED_HASH}" \
    -f docker/Dockerfile.base-runtime .
fi

mkdir -p .ci-artifacts
install -m 755 "${BUNDLE_DIR}/factory_worker" .ci-artifacts/
install -m 644 "${BUNDLE_DIR}/ngx_pagespeed_module.so" .ci-artifacts/
# libc++/libc++abi/libunwind statically linked into factory_worker since v2.0.2.

# nginx-version: the exact nginx version the prebuilt module was built against,
# recorded by the Linux Build job. Dockerfile.prebuilt pins its runtime nginx +
# brotli to this so the module loads (base-runtime tracks nginx.org's current
# release and can drift a point release ahead of a cached module).
# Back-compat: artifacts predating this recording lack the file -- fall back to
# base-runtime's own nginx version so the pin becomes a harmless no-op rather
# than failing the COPY (such old artifacts behave exactly as before; only
# fresh artifacts get the real drift fix).
if [ -f "${BUNDLE_DIR}/nginx-version" ]; then
  install -m 644 "${BUNDLE_DIR}/nginx-version" .ci-artifacts/
else
  echo "::warning::nginx-version absent from ${BUNDLE_DIR}/ -- deriving from base-runtime (no-op pin; old artifact)."
  docker run --rm pagespeed2-base-runtime sh -c "nginx -v 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1" \
    > .ci-artifacts/nginx-version
fi
[ -s .ci-artifacts/nginx-version ] || { echo "::error::Could not determine nginx-version for prebuilt image." >&2; exit 1; }
echo "Prebuilt image will pin nginx to: $(cat .ci-artifacts/nginx-version)"

# Brotli module: Dockerfile.prebuilt COPYs a pre-built
# ngx_http_brotli_filter_module.so instead of recompiling it from source on
# every (cold-cache) image build. The x64 Linux Build job builds it on a
# warm-cache build runner and ships it in the artifact bundle, so the CI test
# runners never recompile brotli.
# Producers that don't ship it (arm64 ci-periodic; older artifact bundles) fall
# back to building it here -- keyed by nginx-version, so it's a Docker
# layer-cache no-op on a warm host. Build needs .ci-artifacts/nginx-version,
# established just above.
if [ -f "${BUNDLE_DIR}/ngx_http_brotli_filter_module.so" ]; then
  install -m 644 "${BUNDLE_DIR}/ngx_http_brotli_filter_module.so" .ci-artifacts/
  echo "Staged pre-built brotli module from ${SRC}."
else
  echo "::warning::brotli module absent from ${BUNDLE_DIR}/ -- building it (no prebuilt artifact: arm64 periodic or old bundle)."
  "${SCRIPT_DIR}/build-brotli-module.sh" "$(pwd)"
fi

echo "Staged CI artifacts from ${SRC}:"
ls -lh .ci-artifacts/
