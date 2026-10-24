#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/pr_tier_scope.sh: a fake `gh` and `sleep` on PATH and a
# temporary git repository with a root commit and one child commit whose
# changed path each case sets.
#
# Run: tools/ci/test_pr_tier_scope.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCOPE="${HERE}/pr_tier_scope.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

BIN="${WORK}/bin"
mkdir -p "${BIN}"
# Fake gh: logs every call to FAKE_LOG. FAKE_GH_FAIL=1 fails every call with
# HTTP 500; otherwise it prints FAKE_GH_BODY (which may be empty) and exits 0.
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
if [ "${FAKE_GH_FAIL:-0}" = "1" ]; then
  echo "HTTP 500" >&2
  exit 1
fi
printf '%s' "${FAKE_GH_BODY-}"
GH
cat > "${BIN}/sleep" <<'SL'
#!/usr/bin/env bash
echo "$1" >> "${SLEEP_LOG}"
SL
chmod +x "${BIN}/gh" "${BIN}/sleep"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

REPO="${WORK}/repo"
SHA_VAL=""

# make_repo <changed path>: a root commit and one child commit that adds the
# path. make_repo with no argument leaves only the root commit.
make_repo() {
  rm -rf "${REPO}"
  git init -q "${REPO}"
  git -C "${REPO}" config user.email t@example.invalid
  git -C "${REPO}" config user.name t
  git -C "${REPO}" commit -q --allow-empty -m "root"
  if [ $# -gt 0 ]; then
    mkdir -p "${REPO}/$(dirname "$1")"
    echo x > "${REPO}/$1"
    git -C "${REPO}" add -A
    git -C "${REPO}" commit -q -m "change $1"
  fi
  SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
}

suite() { # <head_branch> <status> <conclusion as JSON>
  printf '{"head_branch":"%s","status":"%s","conclusion":%s}' "$1" "$2" "$3"
}
body() { # <suite>...
  local IFS=,
  printf '{"total_count":%d,"check_suites":[%s]}' "$#" "$*"
}

PROOF_TRUE="$(body "$(suite gh-readonly-queue/master/pr-12-abc completed '"success"')")"
PROOF_MASTER="$(body "$(suite master completed '"success"')")"
PROOF_FAILED="$(body "$(suite gh-readonly-queue/master/pr-12-abc completed '"failure"')")"
PROOF_RUNNING="$(body "$(suite gh-readonly-queue/master/pr-12-abc in_progress null)")"

OUT="${WORK}/out"
ERR="${WORK}/err"

# run_scope [env assignments...]: runs the script in the repository with the
# push defaults; stdout to OUT, stderr to ERR, the exit code in RC.
run_scope() {
  : > "${WORK}/gh.log"
  : > "${WORK}/sleep.log"
  set +e
  (cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" SLEEP_LOG="${WORK}/sleep.log" \
    GITHUB_EVENT_NAME=push GITHUB_REPOSITORY=We-Amp/example SHA="${SHA_VAL}" GH_TOKEN=x \
    FORK_OR_BOT=false PR_FULL=false CI_SCOPE= FAKE_GH_BODY="${PROOF_TRUE}" \
    "$@" bash "${SCOPE}" > "${OUT}" 2> "${ERR}")
  RC=$?
  set -e
  CASES=$((CASES + 1))
  if [ "$(wc -l < "${OUT}" | tr -d ' ')" = 2 ]; then STDOUT_OK=$((STDOUT_OK + 1)); else STDOUT_BAD="${STDOUT_BAD} ${CUR}"; fi
}

CASES=0
STDOUT_OK=0
STDOUT_BAD=""
CUR=""

# check <name> <want fleet> [want stderr substring...]
check() {
  local name="$1" want="$2" s ok=1
  shift 2
  if [ "${RC}" -ne 0 ]; then
    fail "${name}: exit ${RC}: $(cat "${ERR}")"
    return
  fi
  if [ "$(cat "${OUT}")" != "$(printf 'fleet=%s\nheavy=%s' "${want}" "${want}")" ]; then
    fail "${name}: stdout is '$(tr '\n' ' ' < "${OUT}")', want fleet=${want} then heavy=${want}"
    return
  fi
  for s in "$@"; do
    if ! grep -qF -- "${s}" "${ERR}"; then
      fail "${name}: stderr lacks '${s}': $(cat "${ERR}")"
      ok=0
    fi
  done
  [ "${ok}" = 0 ] || pass "${name}"
}

gh_calls() { grep -c 'check-suites' "${WORK}/gh.log" || true; }

make_repo src/a.cc

CUR=scope-unset-push-runs
run_scope CI_SCOPE=
check "${CUR}" true "shadow: would_skip=true proof=true pipeline_change=false" "fleet scope: event=push scope=full fleet=true heavy=true"
if grep -qF 'repos/We-Amp/example/commits/'"${SHA_VAL}"'/check-suites?app_id=15368&per_page=100' "${WORK}/gh.log"; then
  pass "${CUR}: queried the check suites of the pushed commit"
else
  fail "${CUR}: gh log: $(cat "${WORK}/gh.log")"
fi

CUR=scope-unknown-runs
run_scope CI_SCOPE=light
check "${CUR}" true "reason=scope full" "shadow: would_skip=true"

CUR=scope-uppercase-push
run_scope CI_SCOPE=PUSH
check "${CUR}" false "scope=push fleet=false heavy=false"

CUR=push-proof-skips
run_scope CI_SCOPE=push
check "${CUR}" false "shadow: would_skip=true proof=true pipeline_change=false"

for sc in pr-build pr; do
  CUR="push-proof-skips-scope-${sc}"
  run_scope CI_SCOPE="${sc}"
  check "${CUR}" false "scope=${sc} fleet=false"
done

CUR=push-proof-false-runs
run_scope CI_SCOPE=push FAKE_GH_BODY="${PROOF_MASTER}"
check "${CUR}" true "shadow: would_skip=false proof=false pipeline_change=false"

CUR=push-queue-suite-failed-runs
run_scope CI_SCOPE=push FAKE_GH_BODY="${PROOF_FAILED}"
check "${CUR}" true "proof=false"

CUR=push-queue-suite-in-progress-runs
run_scope CI_SCOPE=push FAKE_GH_BODY="${PROOF_RUNNING}"
check "${CUR}" true "proof=false"

CUR=push-api-error-runs
run_scope CI_SCOPE=push FAKE_GH_FAIL=1
check "${CUR}" true "proof=error"
n="$(gh_calls)"
if [ "${n}" = 3 ]; then pass "${CUR}: exactly 3 check-suites calls"; else fail "${CUR}: ${n} check-suites calls, want 3"; fi
if [ "$(tr '\n' ' ' < "${WORK}/sleep.log")" = "2 4 " ]; then pass "${CUR}: backoff 2 4"; else fail "${CUR}: backoff: $(tr '\n' ' ' < "${WORK}/sleep.log")"; fi

CUR=push-empty-body-runs
run_scope CI_SCOPE=push FAKE_GH_BODY=
check "${CUR}" true "proof=error"

CUR=push-unparseable-body-runs
run_scope CI_SCOPE=push FAKE_GH_BODY='<html>'
check "${CUR}" true "proof=error"

for p in .github/workflows/pipeline-pr.yml .github/workflows/ci.yml .github/actions/x/action.yml tools/ci/a.sh; do
  make_repo "${p}"
  CUR="push-pipeline-change-runs (${p})"
  run_scope CI_SCOPE=push
  check "${CUR}" true "pipeline_change=true" "would_skip=false"
  n="$(gh_calls)"
  if [ "${n}" = 0 ]; then pass "${CUR}: no gh call"; else fail "${CUR}: ${n} gh calls, want 0"; fi
done

# A large diff whose first path is a pipeline path: the match must survive
# grep -q exiting early (no SIGPIPE turning the match into a miss).
make_repo .github/actions/x/action.yml
mkdir -p "${REPO}/src/many"
i=0
while [ "${i}" -lt 3000 ]; do
  : > "${REPO}/src/many/file-with-a-long-name-to-fill-the-pipe-buffer-${i}.cc"
  i=$((i + 1))
done
git -C "${REPO}" add -A
git -C "${REPO}" commit -q --amend -m "large change"
SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
CUR=push-pipeline-change-large-diff-runs
run_scope CI_SCOPE=push
check "${CUR}" true "pipeline_change=true"

# A path that only looks like a pipeline path is not one.
make_repo docs/tools/ci/a.md
CUR=push-lookalike-path-skips
run_scope CI_SCOPE=push
check "${CUR}" false "pipeline_change=false"

make_repo
CUR=push-root-commit-runs
run_scope CI_SCOPE=push
check "${CUR}" true "pipeline_change=true"

make_repo src/a.cc
CUR=pull-request-at-push-scope-runs
run_scope CI_SCOPE=push GITHUB_EVENT_NAME=pull_request
check "${CUR}" true "event=pull_request"
if grep -q 'shadow:' "${ERR}"; then fail "${CUR}: printed a shadow line"; else pass "${CUR}: no shadow line"; fi
n="$(gh_calls)"
if [ "${n}" = 0 ]; then pass "${CUR}: no gh call"; else fail "${CUR}: ${n} gh calls, want 0"; fi

for ev in merge_group workflow_dispatch schedule; do
  CUR="${ev}-at-push-scope-runs"
  run_scope CI_SCOPE=push GITHUB_EVENT_NAME="${ev}"
  check "${CUR}" true "event=${ev}"
done

if [ -z "${STDOUT_BAD}" ]; then
  pass "stdout-is-two-lines (${STDOUT_OK} of ${CASES} runs)"
else
  fail "stdout-is-two-lines: not 2 lines in:${STDOUT_BAD}"
fi

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All PR tier scope cases passed"
