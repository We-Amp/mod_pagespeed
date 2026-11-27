#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Full tier scope of a push to the default branch (docs/ci/pipelines.md,
# "The full tier"). Decides whether the pushed range changes the full tier's
# own definition, in which case the push runs the full tier even while
# FULL_TIER_TRIGGER is `nightly`: a merged full-tier change is exercised at
# once instead of first in the scheduled nightly.
#
# The definition: .github/workflows/pipeline-full.yml, nightly.yml,
# pipeline-nightly.yml and ci.yml (it holds the trigger expression), every
# file under .github/actions/, every tools/ci/ file pipeline-full.yml names
# (as tools/ci/<file> or tools\ci\<file>), and one level of the same
# references inside those files. All of it is read at GITHUB_SHA, so a commit
# that adds a reference proves itself.
#
# The range runs from the base tools/ci/push_range_base.sh --require-job
# 'Full tier scope' computes (the last push run in which this decision
# succeeded) to GITHUB_SHA, not HEAD^..HEAD: GitHub replaces pending push
# runs in a merge burst, and a full-tier change in a replaced run must still
# be seen.
#
# The workflow reads this file and push_range_base.sh from the default branch
# and runs them from the runner's temp directory, never from the tree under
# test.
#
# Env: GITHUB_SHA, GITHUB_REPOSITORY, GH_TOKEN (actions: read),
# DEFAULT_BRANCH. Run from a checkout of GITHUB_SHA with full history.
#
# Stdout: exactly one line, `full_tier_change=true` or `full_tier_change=false`.
# Stderr: one `full tier scope:` line naming the base, the range and the
# reason (push_range_base.sh adds its own lines). Always exits 0; anything
# unexpected prints true.

set -uo pipefail

decide() { # <true|false> <reason>
  echo "full tier scope: $2" >&2
  echo "full_tier_change=$1"
  exit 0
}

sha="${GITHUB_SHA:-}"
[ -n "${sha}" ] || decide true "GITHUB_SHA missing, full_tier_change=true"
git rev-parse -q --verify "${sha}^{commit}" >/dev/null 2>&1 ||
  decide true "commit ${sha} not in the checkout, full_tier_change=true"

base="$(bash "$(dirname "$0")/push_range_base.sh" --require-job 'Full tier scope')" || base=""
[ -n "${base}" ] || decide true "no usable base, range unknown, full_tier_change=true"
range="${base}..${sha}"

changed="$(git diff --name-only "${base}" "${sha}" 2>/dev/null)" ||
  decide true "base ${base}, range ${range}: the diff failed, full_tier_change=true"
[ -n "${changed}" ] ||
  decide false "base ${base}, range ${range}: no file changed, full_tier_change=false"

full="$(git show "${sha}:.github/workflows/pipeline-full.yml" 2>/dev/null)" ||
  decide true "base ${base}, range ${range}: pipeline-full.yml unreadable at ${sha}, full_tier_change=true"

# tool_refs: every tools/ci path named on stdin, with / separators and
# trailing dots or slashes (sentence ends, directories) removed.
tool_refs() {
  # shellcheck disable=SC1003  # a literal backslash in a bracket expression
  grep -oE 'tools[/\\]ci[/\\][A-Za-z0-9._-]+([/\\][A-Za-z0-9._-]+)*' | tr '\\' '/' |
    sed -e 's#[./]*$##' | sort -u
}

refs="$(tool_refs <<< "${full}")"
nested=""
while IFS= read -r ref; do
  [ -n "${ref}" ] || continue
  [ "$(git cat-file -t "${sha}:${ref}" 2>/dev/null)" = blob ] || continue
  nested="${nested}$(git show "${sha}:${ref}" 2>/dev/null | tool_refs)"$'\n'
done <<< "${refs}"
refs="$(printf '%s\n%s\n' "${refs}" "${nested}" | grep -v '^$' | sort -u)"

hit=""
while IFS= read -r f; do
  [ -n "${f}" ] || continue
  case "${f}" in
    .github/workflows/pipeline-full.yml|.github/workflows/nightly.yml|\
    .github/workflows/pipeline-nightly.yml|.github/workflows/ci.yml|.github/actions/*)
      hit="${f}"
      break
      ;;
  esac
  while IFS= read -r ref; do
    [ -n "${ref}" ] || continue
    case "${f}" in
      "${ref}"|"${ref}"/*) hit="${f}"; break ;;
    esac
  done <<< "${refs}"
  [ -z "${hit}" ] || break
done <<< "${changed}"

if [ -n "${hit}" ]; then
  decide true "base ${base}, range ${range}: ${hit} is part of the full tier, full_tier_change=true"
fi
decide false "base ${base}, range ${range}: no full-tier file changed, full_tier_change=false"
