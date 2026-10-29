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
# Fake gh: FAKE_PR_<n> holds "<fork>\t<login>\t<head repo full name>"; FAKE_GH_FAIL=1 fails every call.
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
if [ "${FAKE_GH_FAIL:-0}" = "1" ]; then
  echo "HTTP 500" >&2
  exit 1
fi
n="${2##*/}"
var="FAKE_PR_${n}"
printf '%s\n' "${!var-false$(printf '\t')oschaaf$(printf '\t')We-Amp/example}"
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
    GITHUB_REPOSITORY=We-Amp/example BASE="${BASE_SHA}" HEAD_SHA="${TIP_SHA}" \
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
run_case api-500 1 "refusing" FAKE_GH_FAIL=1
n="$(calls_for 202)"
if [ "${n}" = "5" ]; then pass "api-500 made exactly 5 calls"; else fail "api-500 made ${n} calls, want 5"; fi
if [ "$(tr '\n' ' ' < "${WORK}/sleep.log")" = "2 4 8 16 " ]; then pass "api-500 backoff 2 4 8 16"; else fail "api-500 backoff: $(tr '\n' ' ' < "${WORK}/sleep.log")"; fi

make_repo "a (#201)"
TIP_SHA="${BASE_SHA}"
run_case empty-range 1 "empty merge group"

make_repo "x (#210)"
HEAD_REF_VAL="refs/heads/gh-readonly-queue/main/pr-210-abc"
run_case head-ref-only-pr 0 "PR #210"
n="$(calls_for 210)"
if [ "${n}" = "1" ]; then pass "head-ref-only-pr looked up #210 once"; else fail "#210 looked up ${n} times, want 1"; fi

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All queue fork guard cases passed"
