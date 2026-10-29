#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Merge-queue classifier: decides whether a merge group may run on the build
# fleet or must run on GitHub-hosted runners. Fails closed.
#
# The workflow reads this file from the default branch and pipes it to bash
# (`git show "origin/${DEFAULT_BRANCH}:tools/ci/queue_fork_guard.sh" | bash`),
# never from the queued tree.
#
# Env: DEFAULT_BRANCH, HEAD_SHA (merge group head sha), HEAD_REF
# (refs/heads/gh-readonly-queue/<base>/pr-<N>-<sha>), GITHUB_REPOSITORY,
# GH_TOKEN. Run from a checkout whose origin/<DEFAULT_BRANCH> was fetched
# just before, with full history. The group is every commit between
# origin/<DEFAULT_BRANCH> and HEAD_SHA: in a stacked queue the group's own
# base_sha is the previous entry's queue commit, which would hide the entries
# ahead of it, so base_sha is not used.
#
# The PR-number rule depends on the queue's SQUASH merge method: one squash
# commit per pull request, its subject ending in "(#N)". The shape is
# enforced: no merge commits, each commit maps to a distinct pull request, and
# the head commit's number equals the one in HEAD_REF. A different merge
# method (merge commits, rebase) must change this classifier first; until
# then a group without that shape is refused.
#
# Exit 0: trusted. Every pull request in the group is same-repo and not
# authored by dependabot[bot]; the group may use the fleet.
# Exit 3: untrusted. The group holds a fork pull request, a pull request whose
# head repository is not this one, or a dependabot[bot] pull request; the
# group must run on GitHub-hosted runners. A fork pull request (not
# Dependabot) that touches .github/ or tools/ci/ is refused instead (exit 1).
# Any other exit status: refuse. Callers must treat only 0 as trusted and
# only 3 as hosted. This script never writes GITHUB_OUTPUT itself.

set -euo pipefail

refuse() { echo "refusing: $*" >&2; exit 1; }

# PR numbers: the last "(#N)" of each commit subject in origin/<DEFAULT_BRANCH>..HEAD_SHA.
ref_n="$(printf '%s\n' "${HEAD_REF:-}" | sed -nE 's|.*/pr-([0-9]+)-.*|\1|p')"
[ -n "${ref_n}" ] || refuse "unparseable merge group head ref"

[ -n "${DEFAULT_BRANCH:-}" ] || refuse "DEFAULT_BRANCH is not set"
main_ref="origin/${DEFAULT_BRANCH}"
git rev-parse -q --verify "${main_ref}^{commit}" >/dev/null 2>&1 || refuse "${main_ref} is not available"
git merge-base --is-ancestor "${main_ref}" "${HEAD_SHA}" 2>/dev/null ||
  refuse "${main_ref} is not an ancestor of the merge group head"

shas="$(git rev-list "${main_ref}..${HEAD_SHA}")"
[ -n "${shas}" ] || refuse "empty merge group (${main_ref}..${HEAD_SHA} holds no commits)"
n_range="$(printf '%s\n' ${shas} | wc -l | tr -dc '0-9')"
[ "${n_range}" -le 3 ] || refuse "merge group holds ${n_range} commits; the queue builds at most 3 entries"

merges="$(git rev-list --min-parents=2 "${main_ref}..${HEAD_SHA}")"
[ -z "${merges}" ] || refuse "merge group holds a merge commit; the queue must squash"

prs=""
pr_shas=""
n_commits=0
for sha in ${shas}; do
  subject="$(git log -1 --format=%s "${sha}")"
  n="$(printf '%s\n' "${subject}" | grep -oE '\(#[0-9]+\)' | tail -n 1 | tr -dc '0-9' || true)"
  [ -n "${n}" ] || refuse "commit ${sha} has no PR number in its subject"
  case " ${prs} " in *" ${n} "*) refuse "PR #${n} maps to more than one commit" ;; esac
  prs="${prs} ${n}"
  pr_shas="${pr_shas}${n} ${sha}"$'\n'
  n_commits=$((n_commits + 1))
done

n_prs="$(printf '%s\n' ${prs} | wc -l | tr -dc '0-9')"
[ "${n_prs}" = "${n_commits}" ] || refuse "${n_commits} commits map to ${n_prs} pull requests"

head_subject="$(git log -1 --format=%s "${HEAD_SHA}")"
head_n="$(printf '%s\n' "${head_subject}" | grep -oE '\(#[0-9]+\)' | tail -n 1 | tr -dc '0-9' || true)"
[ "${head_n}" = "${ref_n}" ] || refuse "head commit is PR #${head_n:-?} but the head ref names PR #${ref_n}"

untrusted=0
want_lc="$(printf '%s' "${GITHUB_REPOSITORY}" | tr '[:upper:]' '[:lower:]')"

for n in ${prs}; do
  out=""
  tries=0
  delay=2
  while :; do
    tries=$((tries + 1))
    if out="$(gh api "repos/${GITHUB_REPOSITORY}/pulls/${n}" \
      --jq '[(.head.repo.fork | tostring), (.user.login // ""), (.head.repo.full_name // ""), (.state // ""), ((.merged // false) | tostring), (.merge_commit_sha // "")] | @tsv')"; then
      break
    fi
    [ "${tries}" -lt 5 ] || refuse "could not look up PR #${n} after 5 tries"
    sleep "${delay}"
    delay=$((delay * 2))
  done

  fork="${out%%$'\t'*}"
  rest="${out#*$'\t'}"
  login="${rest%%$'\t'*}"
  rest="${rest#*$'\t'}"
  full="${rest%%$'\t'*}"
  rest="${rest#*$'\t'}"
  state="${rest%%$'\t'*}"
  rest="${rest#*$'\t'}"
  merged="${rest%%$'\t'*}"
  merge_sha="${rest#*$'\t'}"
  [ "${fork}" != "null" ] || refuse "PR #${n} has no head repository (deleted fork)"
  [ "${fork}" = "true" ] || [ "${fork}" = "false" ] || refuse "PR #${n} has an unreadable fork flag"
  # A pull request in a merge group is open, or it already merged earlier in
  # the same queue run: then it merged as exactly the commit attributed to it
  # in this group (the queue fast-forwards to the tested commit). Anything
  # else names an old or closed pull request.
  if [ "${state}" != "open" ]; then
    in_group="$(printf '%s' "${pr_shas}" | awk -v n="${n}" -v m="${merge_sha}" '$1 == n && $2 == m { print "y" }')"
    if [ "${merged}" != "true" ] || [ -z "${merge_sha}" ] || [ -z "${in_group}" ]; then
      refuse "PR #${n} is not open (state '${state}')"
    fi
  fi
  full_lc="$(printf '%s' "${full}" | tr '[:upper:]' '[:lower:]')"

  foreign=0
  if [ "${fork}" = "true" ] || [ "${full_lc}" != "${want_lc}" ]; then
    foreign=1
  fi

  # Same-repo Dependabot: untrusted, and exempt from the path check (its
  # github-actions bumps touch .github/workflows).
  if [ "${login}" = "dependabot[bot]" ] && [ "${foreign}" = "0" ]; then
    echo "PR #${n}: untrusted (dependabot[bot])"
    untrusted=1
    continue
  fi

  if [ "${foreign}" = "1" ]; then
    # Fork code may run on hosted runners, but a change to the CI definitions
    # themselves is never queued from a fork: a maintainer re-lands it.
    sha_list="$(printf '%s' "${pr_shas}" | awk -v n="${n}" '$1 == n { print $2 }')"
    [ -n "${sha_list}" ] || refuse "PR #${n} is from a fork but has no commit in the group to inspect"
    for sha in ${sha_list}; do
      files="$(git diff-tree -z --no-commit-id --name-only -r --root "${sha}" | tr '\0' '\n')" ||
        refuse "could not list the files of commit ${sha} (PR #${n})"
      # A here-string, not a pipe: under pipefail a pipe into grep -q can
      # report SIGPIPE (141) on a long list and be read as "no match".
      if grep -qiE '^(\.github|tools/ci)(/|$)' <<<"${files}"; then
        refuse "fork PR #${n} changes .github/ or tools/ci/; a maintainer must re-land it"
      fi
    done
    echo "PR #${n}: untrusted (fork or foreign head repository ${full}), author ${login}"
    untrusted=1
    continue
  fi

  echo "PR #${n}: same-repo, author ${login}"
done

if [ "${untrusted}" = "1" ]; then
  echo "group is untrusted: hosted runners" >&2
  exit 3
fi
exit 0
