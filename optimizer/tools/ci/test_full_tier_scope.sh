#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/full_tier_scope.sh: a fake `gh` on PATH (the runs and
# jobs API of push_range_base.sh --require-job) and a temporary git
# repository with a fixture pipeline-full.yml; each case commits a change on
# top of a base commit. One static case pins the trigger expressions in the
# real workflows.
#
# Env: optional DEFAULT_BRANCH for the static case (default: origin/HEAD, else main).
#
# Run: tools/ci/test_full_tier_scope.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/full_tier_scope.sh"
ROOT="${HERE}/../.."
# The default branch the static case pins: DEFAULT_BRANCH, else the clone's
# origin/HEAD, else main.
BRANCH="${DEFAULT_BRANCH:-$(git -C "${ROOT}" symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null | sed 's|^origin/||' || true)}"
BRANCH="${BRANCH:-main}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

BIN="${WORK}/bin"
mkdir -p "${BIN}"
# Fake gh: logs every call to FAKE_LOG. A jobs call (.../runs/<id>/jobs...)
# prints FAKE_JOBS_<id> (empty when unset). Any other call fails with HTTP 500
# when FAKE_GH_FAIL=1 and otherwise prints FAKE_GH_BODY.
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
case "$*" in
  */actions/runs/*/jobs*)
    id="$*"
    id="${id#*/actions/runs/}"
    id="${id%%/*}"
    var="FAKE_JOBS_${id}"
    printf '%s' "${!var-}"
    exit 0
    ;;
esac
if [ "${FAKE_GH_FAIL:-0}" = "1" ]; then
  echo "HTTP 500" >&2
  exit 1
fi
printf '%s' "${FAKE_GH_BODY-}"
GH
chmod +x "${BIN}/gh"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

GREEN='{"total_count":2,"jobs":[{"name":"Lint","conclusion":"success"},{"name":"Full tier scope","conclusion":"success"}]}'

REPO="${WORK}/repo"
git init -q "${REPO}"
git -C "${REPO}" config user.email t@example.invalid
git -C "${REPO}" config user.name t
mkdir -p "${REPO}/.github/workflows" "${REPO}/.github/actions/ref-guard" "${REPO}/tools/ci" "${REPO}/src"
cat > "${REPO}/.github/workflows/pipeline-full.yml" <<'YML'
name: pipeline-full
jobs:
  build:
    steps:
      - run: source "${GITHUB_WORKSPACE}/tools/ci/slot-budget.sh"
      - run: bash tools/ci/a.sh
      - run: |
          & (Join-Path $env:GITHUB_WORKSPACE 'tools\ci\Prune-WinDiskCache.ps1') -CacheDir x
YML
printf 'bash tools/ci/b.sh\n' > "${REPO}/tools/ci/a.sh"
for f in tools/ci/b.sh tools/ci/slot-budget.sh tools/ci/Prune-WinDiskCache.ps1 \
         tools/ci/release_version.sh src/x.cc .github/workflows/ci.yml \
         .github/workflows/nightly.yml .github/workflows/pipeline-nightly.yml \
         .github/actions/ref-guard/action.yml; do
  echo base > "${REPO}/${f}"
done
git -C "${REPO}" add -A
git -C "${REPO}" commit -q -m base
BASE="$(git -C "${REPO}" rev-parse HEAD)"

# commit_on <parent> <path>...: a commit on top of <parent> that edits each
# path; prints its SHA.
commit_on() {
  local parent="$1" f
  shift
  git -C "${REPO}" checkout -q --detach "${parent}"
  for f in "$@"; do
    mkdir -p "${REPO}/$(dirname "${f}")"
    echo "change $RANDOM" >> "${REPO}/${f}"
  done
  git -C "${REPO}" add -A
  git -C "${REPO}" commit -q -m "change $*"
  git -C "${REPO}" rev-parse HEAD
}

# run <sha> <runs body> [env assignments...]
run() {
  local sha="$1" body="$2"
  shift 2
  : > "${WORK}/gh.log"
  set +e
  (cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" \
    GITHUB_REPOSITORY=We-Amp/example GITHUB_SHA="${sha}" GH_TOKEN=unused \
    DEFAULT_BRANCH=master FAKE_GH_BODY="${body}" \
    "$@" bash "${SCRIPT}" > "${WORK}/out" 2> "${WORK}/err")
  RC=$?
  set -e
}

# one_run <id> <sha> <conclusion> <created>  ->  a runs API body item
one_run() {
  printf '{"id":%s,"name":"CI","head_sha":"%s","conclusion":"%s","created_at":"%s"}' "$1" "$2" "$3" "$4"
}
base_body() { printf '{"workflow_runs":[%s]}' "$(one_run 1 "${BASE}" success 2026-10-25T01:00:00Z)"; }

# check <name> <true|false> [stderr substring...]
check() {
  local name="$1" want="$2" s ok=1
  shift 2
  if [ "${RC}" -ne 0 ]; then
    fail "${name}: exit ${RC}"
    return
  fi
  if [ "$(cat "${WORK}/out")" != "full_tier_change=${want}" ] || [ "$(wc -l < "${WORK}/out")" -ne 1 ]; then
    fail "${name}: stdout '$(cat "${WORK}/out")', want exactly 'full_tier_change=${want}'"
    return
  fi
  if [ "$(grep -c '^full tier scope: ' "${WORK}/err")" -ne 1 ]; then
    fail "${name}: want one 'full tier scope:' line on stderr: $(cat "${WORK}/err")"
    return
  fi
  for s in "$@"; do
    if ! grep -qF -- "${s}" "${WORK}/err"; then
      fail "${name}: stderr lacks '${s}': $(cat "${WORK}/err")"
      ok=0
    fi
  done
  [ "${ok}" = 0 ] || pass "${name}"
}

# case_path <name> <path> <want>: one commit on BASE editing <path>.
case_path() {
  local sha
  sha="$(commit_on "${BASE}" "$2")"
  run "${sha}" "$(base_body)" FAKE_JOBS_1="${GREEN}"
  check "$1" "$3" "${BASE}..${sha}"
}

case_path product-only-range-is-false src/x.cc false
case_path pipeline-full-change-is-true .github/workflows/pipeline-full.yml true
case_path nightly-change-is-true .github/workflows/nightly.yml true
case_path pipeline-nightly-change-is-true .github/workflows/pipeline-nightly.yml true
case_path ci-change-is-true .github/workflows/ci.yml true
case_path action-change-is-true .github/actions/ref-guard/action.yml true
case_path new-action-is-true .github/actions/new/action.yml true
case_path referenced-tool-change-is-true tools/ci/slot-budget.sh true
case_path referenced-windows-path-tool-change-is-true tools/ci/Prune-WinDiskCache.ps1 true
case_path one-level-transitive-reference-is-true tools/ci/b.sh true
case_path unreferenced-tool-change-is-false tools/ci/release_version.sh false

SHA="$(commit_on "${BASE}" src/x.cc)"
run "${SHA}" '{"workflow_runs":[]}'
check "no-base-is-true" true "no usable base"

run "${SHA}" "" FAKE_GH_FAIL=1
check "api-failure-is-true" true "no usable base"

run "${SHA}" "$(base_body)" FAKE_JOBS_1='{"jobs":[{"name":"Full tier scope","conclusion":"skipped"}]}'
check "base-without-green-scope-is-true" true "no usable base"

run "" "$(base_body)" FAKE_JOBS_1="${GREEN}"
check "missing-sha-is-true" true

NOT_THERE=0123456789abcdef0123456789abcdef01234567
run "${NOT_THERE}" "$(base_body)" FAKE_JOBS_1="${GREEN}"
check "unknown-sha-is-true" true

# The definition is read at GITHUB_SHA: a commit that adds a reference to a
# tool and changes that tool proves itself.
git -C "${REPO}" checkout -q --detach "${BASE}"
printf '      - run: bash tools/ci/release_version.sh\n' >> "${REPO}/.github/workflows/pipeline-full.yml"
echo change >> "${REPO}/tools/ci/release_version.sh"
git -C "${REPO}" commit -q -am "reference and change a tool"
SHA="$(git -C "${REPO}" rev-parse HEAD)"
run "${SHA}" "$(base_body)" FAKE_JOBS_1="${GREEN}"
check "reference-read-at-the-pushed-commit" true

# burst-range-covers-replaced-pushes: P1's run evaluated the scope, P2's run
# was cancelled (replaced) and touched pipeline-full.yml, P3 is the push under
# test and changes product code only. The range is P1..P3.
P1="$(commit_on "${BASE}" src/x.cc)"
P2="$(commit_on "${P1}" .github/workflows/pipeline-full.yml)"
P3="$(commit_on "${P2}" src/x.cc)"
BODY="$(printf '{"workflow_runs":[%s,%s,%s]}' \
  "$(one_run 12 "${P2}" cancelled 2026-10-25T03:00:00Z)" \
  "$(one_run 11 "${P1}" success 2026-10-25T02:00:00Z)" \
  "$(one_run 1 "${BASE}" success 2026-10-25T01:00:00Z)")"
run "${P3}" "${BODY}" FAKE_JOBS_11="${GREEN}" FAKE_JOBS_1="${GREEN}" FAKE_JOBS_12="${GREEN}"
check "burst-range-covers-replaced-pushes" true "${P1}..${P3}"
if grep -qF '/runs/12/jobs' "${WORK}/gh.log"; then
  fail "burst-range-covers-replaced-pushes: the cancelled run's jobs were read"
fi
# The same burst with P1's scope job skipped reaches back to BASE.
run "${P3}" "${BODY}" FAKE_JOBS_11='{"jobs":[]}' FAKE_JOBS_1="${GREEN}"
check "burst-range-reaches-the-last-green-scope" true "${BASE}..${P3}"

# nightly-call-guard-pinned (static, on the real tree): the nightly's call of
# the full tier and the push trigger of the full tier in ci.yml.
# job_if <file> <job id>: the if: of that job, lines joined.
job_if() {
  awk -v job="$2" '
    $0 ~ "^  " job ":[[:space:]]*$" { injob = 1; next }
    { match($0, /^ */); ind = RLENGTH; blank = (length($0) == ind) }
    injob && !blank && ind <= 2 { injob = 0; inif = 0 }
    inif && !blank && ind <= 4 { inif = 0 }
    injob && /^    if:/ { inif = 1; sub(/^    if:/, ""); print; next }
    inif { print }
  ' "$1" | tr '\n' ' '
}
static_ok=1
nightly_if="$(job_if "${ROOT}/.github/workflows/nightly.yml" full)"
for term in "github.event_name == 'schedule'" "inputs.full_tier" "github.ref == 'refs/heads/${BRANCH}'"; do
  case "${nightly_if}" in
    *"${term}"*) ;;
    *) fail "nightly-call-guard-pinned: nightly.yml job full: if: lacks ${term}: [${nightly_if}]"; static_ok=0 ;;
  esac
done
ci_if="$(job_if "${ROOT}/.github/workflows/ci.yml" full)"
for term in "!cancelled()" "vars.FULL_TIER_TRIGGER != 'nightly'" "needs.full-scope.result != 'success'" \
            "needs.full-scope.outputs.full_tier_change != 'false'" "github.ref == 'refs/heads/${BRANCH}'"; do
  case "${ci_if}" in
    *"${term}"*) ;;
    *) fail "nightly-call-guard-pinned: ci.yml job full: if: lacks ${term}: [${ci_if}]"; static_ok=0 ;;
  esac
done
[ "${static_ok}" = 0 ] || pass "nightly-call-guard-pinned"

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All full tier scope cases passed"
