#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Merge-queue classifier: refuses a merge group that holds a fork or a
# Dependabot pull request, so no such code reaches a fleet runner. Fails closed.
#
# The workflow reads this file from the default branch and pipes it to bash
# (`git show "origin/${DEFAULT_BRANCH}:tools/ci/queue_fork_guard.sh" | bash`),
# never from the queued tree.
#
# Env: BASE (merge group base sha), HEAD_SHA (merge group head sha), HEAD_REF
# (refs/heads/gh-readonly-queue/<base>/pr-<N>-<sha>), GITHUB_REPOSITORY,
# GH_TOKEN. Run from a checkout that holds BASE..HEAD_SHA.
#
# The PR-number rule depends on the queue's SQUASH merge method: one squash
# commit per pull request, its subject ending in "(#N)". A different merge
# method (merge commits, rebase) must change this classifier first; until
# then a group without that shape is refused.
#
# Exit 0: every pull request in the group is same-repo and not authored by
# dependabot[bot]. Exit 1: refuse.

set -euo pipefail

refuse() { echo "refusing: $*" >&2; exit 1; }

# PR numbers: the last "(#N)" of each commit subject in BASE..HEAD_SHA, plus
# N from HEAD_REF.
ref_n="$(printf '%s\n' "${HEAD_REF:-}" | sed -nE 's|.*/pr-([0-9]+)-.*|\1|p')"
[ -n "${ref_n}" ] || refuse "unparseable merge group head ref"

shas="$(git rev-list "${BASE}..${HEAD_SHA}")"
[ -n "${shas}" ] || refuse "empty merge group (${BASE}..${HEAD_SHA} holds no commits)"

prs=""
add_pr() {
  case " ${prs} " in *" $1 "*) ;; *) prs="${prs} $1" ;; esac
}

for sha in ${shas}; do
  subject="$(git log -1 --format=%s "${sha}")"
  n="$(printf '%s\n' "${subject}" | grep -oE '\(#[0-9]+\)' | tail -n 1 | tr -dc '0-9' || true)"
  [ -n "${n}" ] || refuse "commit ${sha} has no PR number in its subject"
  add_pr "${n}"
done

add_pr "${ref_n}"

for n in ${prs}; do
  out=""
  tries=0
  delay=2
  while :; do
    tries=$((tries + 1))
    if out="$(gh api "repos/${GITHUB_REPOSITORY}/pulls/${n}" \
      --jq '[(.head.repo.fork | tostring), (.user.login // ""), (.head.repo.full_name // "")] | @tsv')"; then
      break
    fi
    [ "${tries}" -lt 5 ] || refuse "could not look up PR #${n} after 5 tries"
    sleep "${delay}"
    delay=$((delay * 2))
  done

  fork="${out%%$'\t'*}"
  rest="${out#*$'\t'}"
  login="${rest%%$'\t'*}"
  full=""
  case "${rest}" in *$'\t'*) full="${rest#*$'\t'}" ;; esac
  [ "${fork}" != "true" ] || refuse "PR #${n} is from a fork"
  [ "${fork}" != "null" ] || refuse "PR #${n} has no head repository (deleted fork)"
  [ "${fork}" = "false" ] || refuse "PR #${n} has an unreadable fork flag"
  [ "${login}" != "dependabot[bot]" ] || refuse "PR #${n} is authored by dependabot[bot]"
  want_lc="$(printf '%s' "${GITHUB_REPOSITORY}" | tr '[:upper:]' '[:lower:]')"
  full_lc="$(printf '%s' "${full}" | tr '[:upper:]' '[:lower:]')"
  [ "${full_lc}" = "${want_lc}" ] || refuse "PR #${n} head repository ${full} is not ${GITHUB_REPOSITORY}"
  echo "PR #${n}: same-repo, author ${login}"
done
