#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Decides whether this website run sits inside a release window, so the
# live-state guards (the 2.1 version-drift guard, the unit tests that read the
# package index or the Helm chart) warn instead of failing. Prints
# `release_window=true|false` and a `reason=...` line; with GITHUB_ENV set it
# also appends RELEASE_WINDOW=1|0 and RELEASE_SYNC=1|0 (the window is the
# sync pull request itself) there for the later steps of the job.
#
# A release window is a pull request whose head branch is
# `auto/sync-release-manifests` (the pull request that is the fix for a
# lagging manifest) or `release/*` (the release lane's own pull requests, for
# example the Helm chart bump), and whose head lives in THIS repository: a
# fork can name its branch anything, so a fork never gets the tolerance. In
# the merge queue the ref is `gh-readonly-queue/<base>/pr-<N>-<sha>`; the
# pull request is looked up through the API. Pushes to main, every other pull
# request and any lookup that fails are NOT a release window: the guards stay
# failures.
#
# Inputs (environment): EVENT_NAME, HEAD_REF, HEAD_REPO (pull_request),
# REF_NAME (merge_group), GITHUB_REPOSITORY, GH_TOKEN (merge_group only).

set -euo pipefail

event="${EVENT_NAME:-}"
repo="${GITHUB_REPOSITORY:-}"
head_ref="${HEAD_REF:-}"
head_repo="${HEAD_REPO:-}"
window=false
sync=false
reason="not a release branch"

if [ "$event" = "merge_group" ]; then
  pr=$(printf '%s' "${REF_NAME:-}" | sed -nE 's#^gh-readonly-queue/.*/pr-([0-9]+)-[0-9a-f]+$#\1#p')
  head_ref=""
  head_repo=""
  if [ -n "$pr" ] && [ -n "$repo" ]; then
    # gh failure leaves both empty: not a window (fail closed).
    info=$(gh api "repos/${repo}/pulls/${pr}" --jq '.head.ref + " " + (.head.repo.full_name // "")' 2>/dev/null || true)
    head_ref="${info%% *}"
    head_repo="${info#* }"
    [ "$head_repo" = "$info" ] && head_repo=""
  fi
  if [ -z "$head_ref" ]; then
    reason="merge queue entry: pull request head not resolved (no release window; the token needs pull-requests: read)"
    echo "::warning::release window: could not resolve the pull request of merge queue entry '${REF_NAME:-}', so this run is NOT treated as a release window and the live-state guards stay strict. The job token needs pull-requests: read."
  fi
fi

if [ "$event" = "pull_request" ] || [ "$event" = "merge_group" ]; then
  case "$head_ref" in
    auto/sync-release-manifests | release/*)
      if [ -n "$repo" ] && [ "$head_repo" = "$repo" ]; then
        window=true
        [ "$head_ref" = "auto/sync-release-manifests" ] && sync=true
        reason="head branch ${head_ref}"
      else
        reason="head branch ${head_ref} is not from ${repo:-this repository}"
      fi
      ;;
  esac
else
  reason="event ${event:-unknown} is not a pull request"
fi

echo "release_window=${window}"
echo "sync_pr=${sync}"
echo "reason=${reason}"
if [ -n "${GITHUB_ENV:-}" ]; then
  if [ "$window" = true ]; then echo "RELEASE_WINDOW=1" >>"$GITHUB_ENV"; else echo "RELEASE_WINDOW=0" >>"$GITHUB_ENV"; fi
  if [ "$sync" = true ]; then echo "RELEASE_SYNC=1" >>"$GITHUB_ENV"; else echo "RELEASE_SYNC=0" >>"$GITHUB_ENV"; fi
fi
