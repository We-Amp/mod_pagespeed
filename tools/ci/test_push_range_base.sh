#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/push_range_base.sh: a fake `gh` on PATH and a temporary
# git repository with a linear history c1..c4 and one side commit.
#
# Run: tools/ci/test_push_range_base.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/push_range_base.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

BIN="${WORK}/bin"
mkdir -p "${BIN}"
# Fake gh: logs every call to FAKE_LOG. FAKE_GH_FAIL=1 fails with HTTP 500;
# otherwise it prints FAKE_GH_BODY (which may be empty) and exits 0.
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
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

REPO="${WORK}/repo"
git init -q "${REPO}"
git -C "${REPO}" config user.email t@example.invalid
git -C "${REPO}" config user.name t
git -C "${REPO}" commit -q --allow-empty -m c1
C1="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" commit -q --allow-empty -m c2
C2="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" commit -q --allow-empty -m c3
C3="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" commit -q --allow-empty -m c4
C4="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" checkout -q -b side "${C1}"
git -C "${REPO}" commit -q --allow-empty -m side
SIDE="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" checkout -q -
NOT_THERE=0123456789abcdef0123456789abcdef01234567

run() { # <fake body> [env assignments...]
  local fb="$1"
  shift
  : > "${WORK}/gh.log"
  set +e
  (cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" \
    GITHUB_REPOSITORY=We-Amp/example GITHUB_SHA="${C4}" FAKE_GH_BODY="${fb}" \
    "$@" bash "${SCRIPT}" > "${WORK}/out" 2> "${WORK}/err")
  RC=$?
  set -e
}

# runs <name:sha:conclusion:created>...  ->  the API body
runs() {
  local out="" item name sha concl created
  for item in "$@"; do
    IFS=: read -r name sha concl created <<< "${item}"
    out="${out}${out:+,}{\"name\":\"${name}\",\"head_sha\":\"${sha}\",\"conclusion\":\"${concl}\",\"created_at\":\"${created}\"}"
  done
  printf '{"total_count":%d,"workflow_runs":[%s]}' "$#" "${out}"
}

# check <name> <want stdout, may be empty> [stderr substring...]
check() {
  local name="$1" want="$2" s ok=1
  shift 2
  if [ "${RC}" -ne 0 ]; then
    fail "${name}: exit ${RC}"
    return
  fi
  if [ "$(cat "${WORK}/out")" != "${want}" ]; then
    fail "${name}: stdout '$(cat "${WORK}/out")', want '${want}'"
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

run "$(runs "CI:${C3}:success:2026-10-25T03:00:00Z" "CI:${C2}:success:2026-10-25T02:00:00Z")"
check "newest-completed-run-is-the-base" "${C3}"
if grep -qF 'repos/We-Amp/example/actions/runs?event=push&branch=master&status=completed&per_page=30' "${WORK}/gh.log"; then
  pass "queried the completed push runs of master"
else
  fail "gh log: $(cat "${WORK}/gh.log")"
fi

# Order in the answer does not matter: created_at decides.
run "$(runs "CI:${C2}:success:2026-10-25T02:00:00Z" "CI:${C3}:failure:2026-10-25T03:00:00Z")"
check "failed-run-still-evaluated-its-range" "${C3}"

run "$(runs "CI:${C3}:cancelled:2026-10-25T03:00:00Z" "CI:${C2}:cancelled:2026-10-25T02:00:00Z" "CI:${C1}:success:2026-10-25T01:00:00Z")"
check "cancelled-runs-are-skipped" "${C1}" "base ${C1}"

run "$(runs "CI:${SIDE}:success:2026-10-25T03:00:00Z" "CI:${C2}:success:2026-10-25T02:00:00Z")"
check "non-ancestor-head-is-skipped" "${C2}" "skipping ${SIDE}: not an ancestor"

run "$(runs "CI:${NOT_THERE}:success:2026-10-25T03:00:00Z" "CI:${C2}:success:2026-10-25T02:00:00Z")"
check "unknown-head-is-skipped" "${C2}"

run "$(runs "CI:${C4}:success:2026-10-25T04:00:00Z" "CI:${C3}:success:2026-10-25T03:00:00Z")"
check "current-sha-is-excluded" "${C3}"

run "$(runs "CI:${C4}:success:2026-10-25T04:00:00Z")"
check "only-current-sha-gives-nothing" "" "no completed"

run "$(runs "Other:${C3}:success:2026-10-25T03:00:00Z" "CI:${C2}:success:2026-10-25T02:00:00Z")"
check "other-workflows-are-ignored" "${C2}"

run "$(runs "CI:${SIDE}:success:2026-10-25T03:00:00Z" "CI:${NOT_THERE}:success:2026-10-25T02:00:00Z")"
check "no-ancestor-gives-nothing" "" "no candidate is an ancestor"

run "" FAKE_GH_FAIL=1
check "api-failure-gives-nothing" "" "API call failed"

run ""
check "empty-body-gives-nothing" "" "no completed"

run "<html>"
check "unparseable-body-gives-nothing" "" "unusable"

run '{"message":"Not Found"}'
check "error-object-gives-nothing" "" "no completed"

run "$(runs "CI:${C3}:success:2026-10-25T03:00:00Z")" DEFAULT_BRANCH=release
if grep -qF 'branch=release&' "${WORK}/gh.log"; then pass "DEFAULT_BRANCH is honoured"; else fail "DEFAULT_BRANCH: $(cat "${WORK}/gh.log")"; fi

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All push range base cases passed"
