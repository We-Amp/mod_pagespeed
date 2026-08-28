#!/usr/bin/env bash
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.
#
# ensure-workspace.sh - Make the box-local CI workspace current, self-healing
# when this job was scheduled on a different runner than its upstream.
#
# Why this exists:
#   The Linux CI cascade (linux-build -> linux-unit-tests -> {linux-asan,
#   linux-tsan}) reuses the box-local ${WORKSPACE_DIR} that linux-build
#   extracted, instead of re-extracting in every job. But the
#   heavy x64 runner label set matches MORE THAN ONE box, so a downstream job
#   can be scheduled on another one, where the workspace was never populated.
#   The symptom is the next step failing with
#     bash: ${WORKSPACE_DIR}/tools/ensure-ci-image.sh: No such file or directory
#   (observed on a master CI run: build/unit/tsan on one box, asan on
#   another). linux-build already self-heals via
#   extract-vendor-tarball.sh; this script extends that to the reuse jobs.
#
# Behaviour:
#   - Happy path (same box, GIT_COMMIT stamp == $GITHUB_SHA): no-op, the warm
#     tree and box-local bazel disk cache are reused.
#   - Cross-box / stale (stamp missing or for a different SHA): clear any stale
#     (possibly root-owned) tree, then fetch + extract the vendor tarball from
#     the authoritative CI hub shared dir via extract-vendor-tarball.sh
#     (integrity-checked, retried --). The downstream sanitizer jobs
#     rebuild from source under their own config, so a source-only workspace is
#     sufficient; the only cost on the rare split is a colder bazel cache.
#
#   The single-box ARM64 lane has the same shape with a different trigger: its
#   downstream jobs reuse the workspace linux-arm64-build extracted, but the
#   lane serializes jobs FIFO ACROSS runs, so another run's build can land
#   between this run's build and its downstream jobs, leaving the workspace at
#   the wrong SHA. Observed: a unit-test job silently testing a sibling PR's
#   tree -- failing on that PR's bug while masking it on the PR's own run.
#
# Usage:
#   ensure-workspace.sh <short_sha> [<local_tarball>]
#
#   <local_tarball>: optional box-local tarball path handed to
#   extract-vendor-tarball.sh --local. Used by the single-box ARM64 lane,
#   where the authoritative shared vendor dir is a local filesystem path, so
#   the self-heal never needs the network. Cross-box x86 lanes omit it (the
#   local vendor path there belongs to the other machine).
#
# Requires (set by the workflow env): WORKSPACE_DIR, GITHUB_SHA, GITHUB_WORKSPACE.
set -euo pipefail

SHORT_SHA="${1:-}"
[ -n "$SHORT_SHA" ] || { echo "::error::ensure-workspace.sh: <short_sha> argument required" >&2; exit 2; }
LOCAL_TARBALL="${2:-}"
: "${WORKSPACE_DIR:?ensure-workspace.sh: WORKSPACE_DIR must be set}"
: "${GITHUB_SHA:?ensure-workspace.sh: GITHUB_SHA must be set}"
: "${GITHUB_WORKSPACE:?ensure-workspace.sh: GITHUB_WORKSPACE must be set}"

STAMPED="$(tr -d '[:space:]' < "${WORKSPACE_DIR}/GIT_COMMIT" 2>/dev/null || true)"
if [ "$STAMPED" = "$GITHUB_SHA" ]; then
  echo "ensure-workspace: workspace current on $(hostname) (${GITHUB_SHA}) -- reusing warm tree"
  exit 0
fi

echo "ensure-workspace: workspace missing/stale on $(hostname) (have '${STAMPED}', want '${GITHUB_SHA}') -- self-healing from the CI hub"

# Ensure the workspace dir exists owned by THIS runner uid before we touch it.
# On a box where the `vendor` job has never run, ${WORKSPACE_DIR} may not exist
# at all; a docker bind-mount would then create it ROOT-owned and the
# unprivileged extract below would fail "Permission denied". Creating it as the
# runner first (and chowning the dir itself inside the cleanup container)
# guarantees the extract can write into it.
mkdir -p "${WORKSPACE_DIR}" 2>/dev/null || true

# Clear any stale tree first. A previous run may have left root-owned files
# (Docker writes as root), which the unprivileged runner can't rm directly --
# mirror linux-build's Prepare workspace and nuke them via a throwaway
# container, also normalising ownership of the dir itself back to this runner.
docker run --rm -v "${WORKSPACE_DIR}":/workspace alpine sh -c \
  "rm -rf /workspace/* /workspace/.[!.]* 2>/dev/null || true; chown $(id -u):$(id -g) /workspace" \
  2>/dev/null || true
rm -rf "${WORKSPACE_DIR:?}"/* "${WORKSPACE_DIR:?}"/.[!.]* 2>/dev/null || true

# Fetch + extract from the authoritative CI hub shared dir.
#   --stream : extract via `zstd -d | tar` rather than `tar --zstd -xf`, so this
#              works regardless of whether the runner's tar was built with zstd
#              support (the self-heal only ever runs on the OTHER box, where we
#              have no positive proof of `tar --zstd`).
#   --local  : only when the caller passed a box-local tarball path (ARM64
#              lane). Without it -- the cross-box x86 case -- the local vendor
#              path belongs to the other machine, so go straight to the
#              shared copy.
EXTRACT_ARGS=(--sha "${SHORT_SHA}" --dest "${WORKSPACE_DIR}" --stream)
if [ -n "$LOCAL_TARBALL" ]; then
  EXTRACT_ARGS+=(--local "$LOCAL_TARBALL")
fi
bash "${GITHUB_WORKSPACE}/_ci-tools/tools/ci/extract-vendor-tarball.sh" "${EXTRACT_ARGS[@]}"

STAMPED2="$(tr -d '[:space:]' < "${WORKSPACE_DIR}/GIT_COMMIT")"
[ "$STAMPED2" = "$GITHUB_SHA" ] || {
  echo "::error::ensure-workspace: self-heal extract produced wrong stamp ('${STAMPED2}', want '${GITHUB_SHA}')" >&2
  exit 1
}
echo "ensure-workspace: workspace self-healed and verified (${GITHUB_SHA})"
