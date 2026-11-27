#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Ref guard for the release lanes (see docs/ci/release.md). Defense in depth
# next to the `release` environment's tag policy: a release pipeline may only
# run on refs/heads/main or on a release tag whose commit is reachable from
# main, and a manually dispatched release may only name a tag that already
# exists and is reachable from main.
#
# Usage (CI):   GITHUB_REF=... GITHUB_EVENT_NAME=... [INPUT_TAG=...] \
#                 [ALLOWED_TAG_GLOB='v*'] [ALLOWED_EVENTS='push workflow_dispatch'] \
#                 bash tools/ci/ref_guard.sh check
#               ALLOWED_TAG_GLOB empty = main only (no tag may run the job).
#               ALLOWED_EVENTS unset or empty = any event.
# Library:      source this file, call ref_guard_ref_allowed / _dispatch_tag_ok.

# ref_guard_ref_allowed REF [TAG_GLOB]  -> 0 allowed, 1 refused (reason on stderr)
ref_guard_ref_allowed() {
  local ref="${1:-}" glob="${2-v*}"
  case "$ref" in
    refs/heads/main) return 0 ;;
    refs/tags/*)
      local name="${ref#refs/tags/}"
      # shellcheck disable=SC2254
      if [ -n "$glob" ]; then
        case "$name" in
          $glob) return 0 ;;
        esac
      fi
      echo "ref-guard: tag '$name' does not match '${glob:-<none>}'" >&2
      return 1
      ;;
  esac
  echo "ref-guard: '$ref' may not run release jobs (only refs/heads/main${glob:+ and refs/tags/$glob})" >&2
  return 1
}

# ref_guard_dispatch_tag_ok TAG [TAG_GLOB] [MAIN_REF]
# Run inside a clone that has the tags and MAIN_REF (default origin/main).
ref_guard_dispatch_tag_ok() {
  local tag="${1:-}" glob="${2-v*}" main_ref="${3:-origin/main}"
  if ! printf '%s' "$tag" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._+-]*$'; then
    echo "ref-guard: dispatch tag '$tag' is empty or has unexpected characters" >&2
    return 1
  fi
  if [ -z "$glob" ]; then
    echo "ref-guard: this workflow takes no dispatch tag" >&2
    return 1
  fi
  # shellcheck disable=SC2254
  case "$tag" in
    $glob) ;;
    *) echo "ref-guard: dispatch tag '$tag' does not match '$glob'" >&2; return 1 ;;
  esac
  local sha
  if ! sha="$(git rev-parse --verify --quiet "refs/tags/${tag}^{commit}")"; then
    echo "ref-guard: dispatch tag '$tag' does not exist" >&2
    return 1
  fi
  if ! git rev-parse --verify --quiet "${main_ref}^{commit}" >/dev/null; then
    echo "ref-guard: cannot resolve ${main_ref}; refusing" >&2
    return 1
  fi
  if ! git merge-base --is-ancestor "$sha" "$main_ref"; then
    echo "ref-guard: dispatch tag '$tag' (${sha}) is not reachable from main" >&2
    return 1
  fi
  return 0
}

# ref_guard_sha_on_main SHA [MAIN_REF]: SHA is reachable from MAIN_REF.
ref_guard_sha_on_main() {
  local sha="${1:-}" main_ref="${2:-origin/main}"
  if ! git rev-parse --verify --quiet "${main_ref}^{commit}" >/dev/null; then
    echo "ref-guard: cannot resolve ${main_ref}; refusing" >&2
    return 1
  fi
  if [ -z "$sha" ] || ! git merge-base --is-ancestor "$sha" "$main_ref" 2>/dev/null; then
    echo "ref-guard: commit '${sha}' being run is not reachable from main" >&2
    return 1
  fi
}

# ref_guard_event_allowed EVENT [ALLOWED_EVENTS]: EVENT is one of the
# space-separated ALLOWED_EVENTS (empty list = any event).
ref_guard_event_allowed() {
  local ev="${1:-}" allowed="${2:-}" a
  [ -n "$allowed" ] || return 0
  for a in $allowed; do
    [ "$ev" = "$a" ] && return 0
  done
  echo "ref-guard: event '${ev}' may not run this workflow (allowed: ${allowed})" >&2
  return 1
}

# ref_guard_check: env-driven entry point used by the composite action.
ref_guard_check() {
  local glob="${ALLOWED_TAG_GLOB-v*}" main_ref="${MAIN_REF:-origin/main}"
  ref_guard_event_allowed "${GITHUB_EVENT_NAME:-}" "${ALLOWED_EVENTS:-}" || return 1
  ref_guard_ref_allowed "${GITHUB_REF:-}" "$glob" || return 1
  case "${GITHUB_REF:-}" in
    refs/tags/*)
      # A tag name proves nothing: anyone with contents:write can push v9.9.9
      # onto an arbitrary commit. The commit being run must be on main.
      ref_guard_sha_on_main "${GITHUB_SHA:-}" "$main_ref" || return 1
      if [ "${GITHUB_EVENT_NAME:-}" = "workflow_dispatch" ] && [ "${INPUT_TAG-}" != "${GITHUB_REF#refs/tags/}" ]; then
        echo "ref-guard: dispatch on tag '${GITHUB_REF#refs/tags/}' with input tag '${INPUT_TAG-}' (must be equal)" >&2
        return 1
      fi
      ;;
  esac
  if [ "${GITHUB_EVENT_NAME:-}" = "workflow_dispatch" ] && [ -n "${INPUT_TAG+x}" ]; then
    ref_guard_dispatch_tag_ok "$INPUT_TAG" "$glob" "$main_ref" || return 1
  fi
  echo "ref-guard: ok (${GITHUB_REF:-}, ${GITHUB_EVENT_NAME:-})"
}

# ref_guard_checkout_ref: the ref a release job must check out. A dispatch
# builds the dispatched tag (INPUT_TAG), never the HEAD of the ref the dispatch
# ran on; any other event builds the ref that triggered it. Call after
# ref_guard_check has validated the inputs.
ref_guard_checkout_ref() {
  if [ "${GITHUB_EVENT_NAME:-}" = "workflow_dispatch" ] && [ -n "${INPUT_TAG:-}" ]; then
    printf 'refs/tags/%s\n' "$INPUT_TAG"
  else
    printf '%s\n' "${GITHUB_REF:-}"
  fi
}

# ref_guard_outputs: `ref=` and `sha=` lines for $GITHUB_OUTPUT (sha = the
# commit that ref resolves to, peeled through annotated tags).
ref_guard_outputs() {
  local ref sha
  ref="$(ref_guard_checkout_ref)"
  case "$ref" in
    refs/tags/*)
      sha="$(git rev-parse --verify --quiet "${ref}^{commit}")" || { echo "ref-guard: cannot resolve $ref" >&2; return 1; }
      # A tag push must still point where the event said it did (a moved tag).
      case "${GITHUB_REF:-}" in
        refs/tags/*)
          local ev; ev="$(git rev-parse --verify --quiet "${GITHUB_SHA:-}^{commit}")" || ev=""
          if [ "$ev" != "$sha" ]; then
            echo "ref-guard: $ref now resolves to $sha but the run is at ${GITHUB_SHA:-<unset>} (moved tag?)" >&2
            return 1
          fi ;;
      esac ;;
    *) sha="${GITHUB_SHA:-}" ;;
  esac
  printf 'ref=%s\nsha=%s\n' "$ref" "$sha"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -euo pipefail
  case "${1:-}" in
    check) ref_guard_check ;;
    outputs) ref_guard_outputs ;;
    *) echo "usage: $0 check" >&2; exit 2 ;;
  esac
fi
