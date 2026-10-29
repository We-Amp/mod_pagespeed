#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/queue_fork_guard.sh: a fake `gh` and `sleep` on PATH and a
# temporary git repository with squash-style commits.
#
# Run: tools/ci/test_queue_fork_guard.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUARD="${HERE}/queue_fork_guard.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

BIN="${WORK}/bin"
mkdir -p "${BIN}"
# Fake gh: FAKE_PR_<n> holds "<fork>\t<login>\t<head repo full name>" and
# FAKE_STATE_<n> the state (default open), FAKE_MERGED_<n> (default false) and
# FAKE_MSHA_<n> (merge_commit_sha, default empty); FAKE_GH_FAIL=1 fails every call.
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
if [ "${FAKE_GH_FAIL:-0}" = "1" ]; then
  echo "HTTP 500" >&2
  exit 1
fi
n="${2##*/}"
var="FAKE_PR_${n}"
svar="FAKE_STATE_${n}"
mvar="FAKE_MERGED_${n}"; hvar="FAKE_MSHA_${n}"
printf '%s\t%s\t%s\t%s\n' "${!var-false$(printf '\t')oschaaf$(printf '\t')We-Amp/example}" "${!svar-open}" "${!mvar-false}" "${!hvar-}"
GH
cat > "${BIN}/sleep" <<'SL'
#!/usr/bin/env bash
echo "$1" >> "${SLEEP_LOG}"
SL
chmod +x "${BIN}/gh" "${BIN}/sleep"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

TAB="$(printf '\t')"
REPO="${WORK}/repo"

# make_repo <subject>...  -> a base commit plus one commit per subject.
# A subject may carry a file to touch: "subject|path/in/repo". Commits are real
# (not --allow-empty) when a path is given, so diff-tree has something to list.
make_repo() {
  rm -rf "${REPO}"
  git init -q "${REPO}"
  git -C "${REPO}" config user.email t@example.invalid
  git -C "${REPO}" config user.name t
  git -C "${REPO}" commit -q --allow-empty -m "base (#1)"
  BASE_SHA="$(git -C "${REPO}" rev-parse HEAD)"
  git -C "${REPO}" update-ref refs/remotes/origin/main "${BASE_SHA}"
  local s
  for s in "$@"; do
    case "${s}" in
      *'|'*)
        mkdir -p "${REPO}/$(dirname "${s#*|}")"
        echo "${s}" >> "${REPO}/${s#*|}"
        git -C "${REPO}" add -A
        git -C "${REPO}" commit -q -m "${s%%|*}"
        ;;
      *) git -C "${REPO}" commit -q --allow-empty -m "${s}" ;;
    esac
  done
  TIP_SHA="$(git -C "${REPO}" rev-parse HEAD)"
}

# run_case <name> <want-exit> <want-substring|-> [env assignments...]
# Uses BASE_SHA / TIP_SHA / HEAD_REF_VAL from the caller.
run_case() {
  local name="$1" want_rc="$2" want_out="$3" rc out
  shift 3
  : > "${WORK}/gh.log"
  : > "${WORK}/sleep.log"
  set +e
  out="$(cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" SLEEP_LOG="${WORK}/sleep.log" \
    GITHUB_REPOSITORY=We-Amp/example DEFAULT_BRANCH=main BASE="${BASE_SHA}" HEAD_SHA="${TIP_SHA}" \
    HEAD_REF="${HEAD_REF_VAL:-refs/heads/gh-readonly-queue/main/pr-999-abc}" "$@" bash < "${GUARD}" 2>&1)"
  rc=$?
  set -e
  if [ "${rc}" -ne "${want_rc}" ]; then
    fail "${name}: exit ${rc}, want ${want_rc}: ${out}"
  elif [ "${want_out}" != "-" ] && ! printf '%s' "${out}" | grep -qF "${want_out}"; then
    fail "${name}: output lacks '${want_out}': ${out}"
  else
    pass "${name}"
  fi
}

calls_for() { grep -c "pulls/$1 " "${WORK}/gh.log" || true; }

HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-203-abc"

make_repo "a (#201)" "b (#202)" "c (#203)"
run_case same-repo-group-of-3 0 "PR #203"

run_case fork-in-middle 3 "PR #202: untrusted" "FAKE_PR_202=true${TAB}someone${TAB}someone/example"
run_case dependabot 3 "PR #203: untrusted (dependabot[bot])" "FAKE_PR_203=false${TAB}dependabot[bot]${TAB}We-Amp/example"
run_case null-head-repo 1 "refusing: PR #202" "FAKE_PR_202=null${TAB}someone${TAB}"
run_case foreign-head-repo 3 "PR #202: untrusted" "FAKE_PR_202=false${TAB}someone${TAB}other/Example"
run_case head-repo-case-insensitive 0 "PR #202" "FAKE_PR_202=false${TAB}someone${TAB}we-amp/EXAMPLE"
FORK="true${TAB}someone${TAB}someone/example"
run_case group-of-only-forks 3 "untrusted" "FAKE_PR_201=${FORK}" "FAKE_PR_202=${FORK}" "FAKE_PR_203=${FORK}"
run_case mixed-same-repo-and-fork 3 "PR #201: same-repo" "FAKE_PR_203=${FORK}"
run_case fork-plus-null-head 1 "refusing: PR #201 has no head repository" "FAKE_PR_202=${FORK}" "FAKE_PR_201=null${TAB}someone${TAB}"
run_case fork-plus-lookup-500 1 "refusing" "FAKE_PR_201=${FORK}" "FAKE_GH_FAIL=1"
run_case bad-head-ref 1 "refusing: unparseable merge group head ref" HEAD_REF=

make_repo "a (#201)" "b (#202)|src/b.txt" "c (#203)|.github/workflows/ci.yml"
run_case fork-touches-workflow 1 "fork PR #203 changes .github/" "FAKE_PR_203=${FORK}"
run_case two-forks-one-touches-workflow 1 "refusing" "FAKE_PR_202=${FORK}" "FAKE_PR_203=${FORK}"
run_case same-repo-touches-workflow 0 "PR #203"
run_case dependabot-touches-workflow 3 "PR #203: untrusted (dependabot[bot])" "FAKE_PR_203=false${TAB}dependabot[bot]${TAB}We-Amp/example"
run_case fork-clean-files 3 "PR #202: untrusted" "FAKE_PR_202=${FORK}"

make_repo "a (#201)" "b (#202)|tools/ci/x.sh" "c (#203)"
run_case fork-touches-tools-ci 1 "fork PR #202 changes .github/ or tools/ci/" "FAKE_PR_202=${FORK}"

make_repo "a (#201)" "b (#202)|tools/cis/x.sh" "c (#203)|docs/.github/y.md"
run_case fork-lookalike-paths-ok 3 "PR #203: untrusted" "FAKE_PR_203=${FORK}" "FAKE_PR_202=${FORK}"

make_repo "a (#201)" "fix things"
run_case no-pr-number 1 "no PR number"

make_repo "a (#201)" "b (#202)"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-202-abc"
run_case api-500 1 "refusing" FAKE_GH_FAIL=1
n="$(calls_for 202)"
if [ "${n}" = "5" ]; then pass "api-500 made exactly 5 calls"; else fail "api-500 made ${n} calls, want 5"; fi
if [ "$(tr '\n' ' ' < "${WORK}/sleep.log")" = "2 4 8 16 " ]; then pass "api-500 backoff 2 4 8 16"; else fail "api-500 backoff: $(tr '\n' ' ' < "${WORK}/sleep.log")"; fi

HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-203-abc"
make_repo "a (#201)"
TIP_SHA="${BASE_SHA}"
run_case empty-range 1 "empty merge group"

make_repo "x (#210)"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-210-abc"
run_case head-ref-only-pr 0 "PR #210"
n="$(calls_for 210)"
if [ "${n}" = "1" ]; then pass "head-ref-only-pr looked up #210 once"; else fail "#210 looked up ${n} times, want 1"; fi

# Hardening cases.
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-202-abc"
make_repo "a (#200)" "b (#201) (#202)"
run_case title-injected-number 3 "PR #202: untrusted" "FAKE_PR_202=${FORK}"

make_repo "a (#200)" "b (#201) (#202)|.github/workflows/ci.yml"
run_case title-injected-number-touching-workflow 1 "fork PR #202 changes" "FAKE_PR_202=${FORK}"

# More than 64 KiB of paths, .github/ sorting first: a pipe into grep -q would
# be killed by SIGPIPE under pipefail and read as "no match".
make_repo "a (#200)" "b (#202)"
mkdir -p "${REPO}/.github" "${REPO}/zzzz"
echo x > "${REPO}/.github/a"
i=0
while [ "${i}" -lt 3000 ]; do
  echo x > "${REPO}/zzzz/a-long-file-name-to-fill-the-listing-quickly-${i}.txt"
  i=$((i + 1))
done
git -C "${REPO}" add -A
git -C "${REPO}" commit -q -m "big (#203)"
TIP_SHA="$(git -C "${REPO}" rev-parse HEAD)"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-203-abc"
run_case huge-file-list-fork-touches-github 1 "fork PR #203 changes" "FAKE_PR_203=${FORK}"

HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-202-abc"
make_repo "a (#201)" "b (#202)|.GitHub/x.yml"
run_case fork-case-variant-path 1 "fork PR #202 changes" "FAKE_PR_202=${FORK}"

make_repo "a (#201)" "b (#202)"
git -C "${REPO}" checkout -q "${BASE_SHA}"
git -C "${REPO}" checkout -q -b side
git -C "${REPO}" commit -q --allow-empty -m "side (#203)"
git -C "${REPO}" checkout -q -
git -C "${REPO}" merge -q --no-ff side -m "Merge (#204)"
TIP_SHA="$(git -C "${REPO}" rev-parse HEAD)"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-204-abc"
run_case merge-commit-refused 1 "merge commit"

make_repo "a (#201)" "b (#201)"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-201-abc"
run_case two-commits-one-pr 1 "more than one commit"

make_repo "a (#201)" "b (#202)"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-201-abc"
run_case head-ref-subject-mismatch 1 "head ref names PR #201"

DEP="false${TAB}dependabot[bot]${TAB}We-Amp/example"
DEP_FOREIGN="false${TAB}dependabot[bot]${TAB}other/example"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-202-abc"
make_repo "a (#201)" "b (#202)|.github/workflows/ci.yml"
run_case dependabot-same-repo-touching-workflow 3 "PR #202: untrusted (dependabot[bot])" "FAKE_PR_202=${DEP}"
run_case dependabot-foreign-head-touching-workflow 1 "fork PR #202 changes" "FAKE_PR_202=${DEP_FOREIGN}"

HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-202-abc"
make_repo "a (#201)" "b (#202)"
run_case closed-pr-refused 1 "PR #201 is not open" "FAKE_STATE_201=closed"
run_case merged-pr-refused 1 "is not open" "FAKE_STATE_202=merged"
run_case missing-state-refused 1 "is not open" "FAKE_STATE_202="

# A pull request that merged earlier in the same queue run is accepted only
# when its merge_commit_sha is the commit attributed to it in the group.
make_repo "a (#201)" "b (#202)"
SHA_201="$(git -C "${REPO}" rev-parse HEAD~1)"
run_case merged-pr-matching-commit-ok 0 "PR #201" "FAKE_STATE_201=closed" "FAKE_MERGED_201=true" "FAKE_MSHA_201=${SHA_201}"
run_case merged-pr-other-commit-refused 1 "PR #201 is not open" "FAKE_STATE_201=closed" "FAKE_MERGED_201=true" "FAKE_MSHA_201=1111111111111111111111111111111111111111"
run_case closed-unmerged-pr-matching-sha-refused 1 "PR #201 is not open" "FAKE_STATE_201=closed" "FAKE_MERGED_201=false" "FAKE_MSHA_201=${SHA_201}"
run_case merged-fork-pr-still-hosted 3 "PR #201: untrusted" "FAKE_PR_201=${FORK}" "FAKE_STATE_201=closed" "FAKE_MERGED_201=true" "FAKE_MSHA_201=${SHA_201}"

# Stacked queue: merge_group.base_sha is the previous entry's queue commit, so
# BASE is not the range base. The group is origin/main..HEAD.
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-552-abc"
make_repo "fork (#551)" "same (#552)"
STACKED_BASE="$(git -C "${REPO}" rev-parse HEAD~1)"
run_case stacked-base-fork-ahead-untrusted 3 "PR #551: untrusted" "FAKE_PR_551=${FORK}" "BASE=${STACKED_BASE}"
make_repo "fork (#551)|.github/workflows/ci.yml" "same (#552)"
STACKED_BASE="$(git -C "${REPO}" rev-parse HEAD~1)"
run_case stacked-base-fork-ahead-touching-workflow 1 "fork PR #551 changes" "FAKE_PR_551=${FORK}" "BASE=${STACKED_BASE}"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-553-abc"
make_repo "a (#551)" "b (#552)" "c (#553)"
STACKED_BASE="$(git -C "${REPO}" rev-parse HEAD~1)"
run_case stacked-chain-of-three-trusted 0 "PR #553" "BASE=${STACKED_BASE}"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-554-abc"
make_repo "a (#551)" "b (#552)" "c (#553)" "d (#554)"
run_case four-entries-ok 0 "PR #554"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-555-abc"
make_repo "a (#551)" "b (#552)" "c (#553)" "d (#554)" "e (#555)"
run_case five-entries-ok 0 "PR #555"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-559-abc"
make_repo "a (#550)" "a (#551)" "b (#552)" "c (#553)" "d (#554)" "e (#555)" "f (#556)" "g (#557)" "h (#558)" "i (#559)"
run_case ten-entries-ok 0 "PR #559"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-560-abc"
make_repo "a (#550)" "a (#551)" "b (#552)" "c (#553)" "d (#554)" "e (#555)" "f (#556)" "g (#557)" "h (#558)" "i (#559)" "j (#560)"
run_case eleven-entries-refused 1 "more than 10"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-553-abc"
make_repo "a (#551)" "b (#552)" "c (#553)"
git -C "${REPO}" commit -q --allow-empty -m "unrelated main"
git -C "${REPO}" update-ref refs/remotes/origin/main "$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" checkout -q "${BASE_SHA}"
git -C "${REPO}" commit -q --allow-empty -m "other (#553)"
TIP_SHA="$(git -C "${REPO}" rev-parse HEAD)"
run_case main-not-an-ancestor-refused 1 "not an ancestor"
make_repo "a (#551)"
git -C "${REPO}" update-ref -d refs/remotes/origin/main
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-551-abc"
run_case main-ref-missing-refused 1 "not available"

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All queue fork guard cases passed"
