#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/nightly_red_report.sh: a fake `gh` on PATH answering from
# a per-case scenario (environment variables), and a temporary git repository
# with a linear history c1..c4 for the commit count. No network, no token.
#
# Run: tools/ci/test_nightly_red_report.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# The script under test runs from a copy next to its extractor, as in a
# checkout.
mkdir -p "${WORK}/ci"
cp "${HERE}/nightly_red_report.sh" "${HERE}/failing_targets.sh" "${WORK}/ci/" 2> /dev/null || true
SCRIPT="${WORK}/ci/nightly_red_report.sh"

BIN="${WORK}/bin"
mkdir -p "${BIN}"
# Fake gh. Every call is logged to FAKE_LOG; a --body-file's text is appended
# to FAKE_BODIES under a `==== <subcommand> <verb>` header. Answers:
#   api [--paginate] .../actions/runs/<id>/jobs...  FAKE_JOBS_<id> (HTTP 500
#                                                    when FAKE_JOBS_FAIL_<id>=1)
#   api .../actions/workflows/nightly.yml/runs...   FAKE_RUNS_NIGHTLY
#   api .../actions/workflows/ci.yml/runs...&event=workflow_dispatch...
#                                                   FAKE_RUNS_CI_DISPATCH
#   api .../actions/workflows/ci.yml/runs...&event=push...
#                                                   FAKE_RUNS_CI_PUSH
#   api .../actions/runs/<id> --jq .created_at      FAKE_RUN_CREATED (fails
#                                                    when FAKE_RUN_FAIL=1)
#   api --allow-escape-sequences .../actions/jobs/<id>/logs
#                                                   FAKE_RAWLOG_<id> (HTTP 404
#                                                    when unset); without the
#                                                    flag, real gh's refusal
#                                                    (exit 0, FAKE_REFUSAL_RC)
#   run view <run> --job <id> ...                   FAKE_LOG_<id> (fails when
#                                                    FAKE_VIEW_FAIL=1; rejects
#                                                    --allow-escape-sequences,
#                                                    as real gh does)
#   issue list ...                                  FAKE_ISSUES (fails when
#                                                    FAKE_ISSUES_FAIL=1)
#   issue create|edit|comment|close, label create   nothing, exit 0
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
prev=""
for a in "$@"; do
  if [ "${prev}" = "--body-file" ]; then
    { echo "==== $1 $2"; cat "${a}"; } >> "${FAKE_BODIES}"
  fi
  prev="${a}"
done
answer() { local var="$1"; printf '%s' "${!var-}"; }
case "$1 $2" in
  "api --paginate"|"api repos/"*|"api --allow-escape-sequences")
    path="$*"
    case "${path}" in
      */actions/runs/*/jobs*)
        id="${path#*/actions/runs/}"
        id="${id%%/*}"
        fail="FAKE_JOBS_FAIL_${id}"
        if [ "${!fail-0}" = "1" ]; then echo "HTTP 500" >&2; exit 1; fi
        answer "FAKE_JOBS_${id}"
        ;;
      */actions/workflows/nightly.yml/runs*) answer FAKE_RUNS_NIGHTLY ;;
      */actions/workflows/ci.yml/runs*event=workflow_dispatch*) answer FAKE_RUNS_CI_DISPATCH ;;
      */actions/workflows/ci.yml/runs*event=push*) answer FAKE_RUNS_CI_PUSH ;;
      */actions/runs/*" --jq .created_at")
        if [ "${FAKE_RUN_FAIL:-0}" = "1" ]; then echo "HTTP 500" >&2; exit 1; fi
        echo "${FAKE_RUN_CREATED}"
        ;;
      */actions/jobs/*/logs)
        id="${path#*/actions/jobs/}"
        id="${id%%/*}"
        var="FAKE_RAWLOG_${id}"
        if [ -z "${!var+set}" ]; then echo "HTTP 404" >&2; exit 1; fi
        if [ "$2" != "--allow-escape-sequences" ] || [ "${FAKE_FORCE_REFUSAL:-0}" = "1" ]; then
          echo "the response contains terminal escape sequences; pass --allow-escape-sequences to print it anyway"
          exit "${FAKE_REFUSAL_RC:-0}"
        fi
        answer "${var}"
        ;;
      *) echo "unexpected api call" >&2; exit 1 ;;
    esac
    ;;
  "run view")
    for a in "$@"; do
      if [ "${a}" = "--allow-escape-sequences" ]; then
        echo "unknown flag: --allow-escape-sequences" >&2
        exit 1
      fi
    done
    if [ "${FAKE_VIEW_FAIL:-0}" = "1" ]; then echo "HTTP 404" >&2; exit 1; fi
    id=""
    prev=""
    for a in "$@"; do
      [ "${prev}" = "--job" ] && id="${a}"
      prev="${a}"
    done
    answer "FAKE_LOG_${id}"
    ;;
  "issue list")
    if [ "${FAKE_ISSUES_FAIL:-0}" = "1" ]; then echo "HTTP 500" >&2; exit 1; fi
    printf '%s' "${FAKE_ISSUES:-[]}"
    ;;
  "issue create"|"issue edit"|"issue comment"|"issue close"|"label create") ;;
  *) echo "unexpected gh call" >&2; exit 1 ;;
esac
GH
chmod +x "${BIN}/gh"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

REPO="${WORK}/repo"
git init -q "${REPO}"
git -C "${REPO}" config user.email t@example.invalid
git -C "${REPO}" config user.name t
for c in c1 c2 c3 c4; do git -C "${REPO}" commit -q --allow-empty -m "${c}"; done
C1="$(git -C "${REPO}" rev-parse HEAD~3)"
C2="$(git -C "${REPO}" rev-parse HEAD~2)"
C3="$(git -C "${REPO}" rev-parse HEAD~1)"
C4="$(git -C "${REPO}" rev-parse HEAD)"

# jobs <id|name|conclusion|step=conclusion;step=conclusion>...  ->  a jobs page
jobs() {
  local out="" item id name concl steps st s
  for item in "$@"; do
    IFS='|' read -r id name concl steps <<< "${item}"
    st=""
    if [ -n "${steps}" ]; then
      while IFS= read -r s; do
        [ -n "${s}" ] || continue
        st="${st}${st:+,}{\"name\":\"${s%=*}\",\"conclusion\":\"${s##*=}\"}"
      done <<< "$(printf '%s\n' "${steps}" | tr ';' '\n')"
    fi
    out="${out}${out:+,}{\"id\":${id},\"name\":\"${name}\",\"status\":\"completed\",\"conclusion\":\"${concl}\",\"created_at\":\"2026-10-26T04:00:00Z\",\"html_url\":\"https://github.com/We-Amp/example/actions/runs/0/job/${id}\",\"steps\":[${st}]}"
  done
  printf '{"total_count":%d,"jobs":[%s]}' "$#" "${out}"
}

# runs <id|event|sha|created>...  ->  a workflow runs page
runs() {
  local out="" item id event sha created
  for item in "$@"; do
    IFS='|' read -r id event sha created <<< "${item}"
    out="${out}${out:+,}{\"id\":${id},\"event\":\"${event}\",\"head_sha\":\"${sha}\",\"status\":\"completed\",\"created_at\":\"${created}\",\"html_url\":\"https://github.com/We-Amp/example/actions/runs/${id}\"}"
  done
  printf '{"total_count":%d,"workflow_runs":[%s]}' "$#" "${out}"
}

NO_RUNS="$(runs)"
ISSUE7='[{"number":7,"body":"The scheduled full tier is red.\n\n### full / Linux TSan\n<!-- nightly-red-job: full / Linux TSan -->\n- SHA: x"}]'

run() { # [env assignments...]
  : > "${WORK}/gh.log"
  : > "${WORK}/bodies"
  set +e
  (cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" \
    FAKE_BODIES="${WORK}/bodies" GITHUB_REPOSITORY=We-Amp/example \
    GITHUB_RUN_ID=50 GITHUB_RUN_ATTEMPT=1 GITHUB_SHA="${C4}" \
    DEFAULT_BRANCH=main GH_TOKEN=fake FAKE_RUNS_NIGHTLY="${NO_RUNS}" \
    FAKE_RUNS_CI_DISPATCH="${NO_RUNS}" FAKE_RUNS_CI_PUSH="${NO_RUNS}" \
    FAKE_RUN_CREATED=2026-10-26T04:00:00Z "$@" bash "${SCRIPT}" > "${WORK}/out" 2> "${WORK}/err")
  RC=$?
  set -e
}

calls() { grep -cE -- "$1" "${WORK}/gh.log" || true; }
body_has() { grep -qF -- "$1" "${WORK}/bodies"; }

# expect <case> <condition description> <test command...>
expect() {
  local name="$1" what="$2"
  shift 2
  if "$@"; then
    return 0
  fi
  fail "${name}: ${what}; exit ${RC}; gh log: $(cat "${WORK}/gh.log"); bodies: $(cat "${WORK}/bodies"); stderr: $(cat "${WORK}/err")"
  CASE_OK=0
}
start() { CASE_OK=1; }
done_case() { [ "${CASE_OK}" = 0 ] || pass "$1"; }
eq() { [ "$1" = "$2" ]; }
le() { [ "$1" -le "$2" ]; }
no() { ! "$@"; }

RED_JOBS="$(jobs "501|nightly / Fuzz CSS|success|" "502|full / Linux TSan|failure|" \
  "503|full / Linux ASan + UBSan|success|")"
TSAN_LOG='Linux TSan	Run the tests	2026-10-26T04:12:30.0000000Z //t:race_test    FAILED in 3.1s'
H40="$(runs "40|schedule|${C1}|2026-10-25T04:00:00Z")"
J40_GREEN="$(jobs "401|full / Linux TSan|success|")"

# --- red-creates-issue
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" FAKE_RUNS_NIGHTLY="${H40}" \
  FAKE_JOBS_40="${J40_GREEN}"
expect red-creates-issue "exit 0" eq "${RC}" 0
expect red-creates-issue "one issue create" eq "$(calls '^gh issue create ')" 1
expect red-creates-issue "create carries title and label" \
  grep -qF -- "gh issue create --title Nightly full tier red --label nightly-red --body-file" "${WORK}/gh.log"
expect red-creates-issue "label created first" \
  eq "$(grep -nE '^gh (label|issue) create' "${WORK}/gh.log" | cut -d: -f1 | tr '\n' ' ')" \
  "$(grep -n '^gh label create nightly-red --color B60205' "${WORK}/gh.log" | cut -d: -f1) $(grep -n '^gh issue create' "${WORK}/gh.log" | cut -d: -f1) "
for s in "full / Linux TSan" "${C4}" "//t:race_test" "actions/runs/40" "3 commits" \
  "<!-- nightly-red-job: full / Linux TSan -->" "schedule"; do
  expect red-creates-issue "body has '${s}'" body_has "${s}"
done
expect red-creates-issue "green job not listed" no body_has "full / Linux ASan + UBSan"
expect red-creates-issue "no comment/edit" eq "$(calls '^gh issue (comment|edit|close)')" 0
expect red-creates-issue "this run's jobs paginated" \
  grep -qF -- "gh api --paginate repos/We-Amp/example/actions/runs/50/jobs?filter=latest&per_page=100" "${WORK}/gh.log"
for q in "nightly.yml/runs?branch=main&status=completed&per_page=20" \
  "ci.yml/runs?branch=main&status=completed&event=workflow_dispatch&per_page=20" \
  "ci.yml/runs?branch=main&status=completed&event=push&per_page=20"; do
  expect red-creates-issue "history list ${q}" grep -qxF -- "gh api repos/We-Amp/example/actions/workflows/${q}" "${WORK}/gh.log"
done
expect red-creates-issue "three history lists" eq "$(calls 'actions/workflows/')" 3
expect red-creates-issue "run object read for the cutoff" \
  grep -qxF -- "gh api repos/We-Amp/example/actions/runs/50 --jq .created_at" "${WORK}/gh.log"
expect red-creates-issue "uniform skipped line" body_has "Skipped (neither failed nor green): none"
expect red-creates-issue "primary log path is not marked" no body_has "from the whole job log"
expect red-creates-issue "issue list shape" \
  grep -qF -- "gh issue list --label nightly-red --state open --json number,body --limit 5" "${WORK}/gh.log"
done_case red-creates-issue

# --- red-updates-open-issue
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" FAKE_RUNS_NIGHTLY="${H40}" \
  FAKE_JOBS_40="${J40_GREEN}" FAKE_ISSUES="${ISSUE7}"
expect red-updates-open-issue "exit 0" eq "${RC}" 0
expect red-updates-open-issue "comment 7" eq "$(calls '^gh issue comment 7 --body-file')" 1
expect red-updates-open-issue "edit 7" eq "$(calls '^gh issue edit 7 --body-file')" 1
expect red-updates-open-issue "no create" eq "$(calls '^gh (issue|label) create')" 0
expect red-updates-open-issue "edited body has the target" body_has "//t:race_test"
done_case red-updates-open-issue

# --- skipped-never-green
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" \
  FAKE_RUNS_NIGHTLY="$(runs "45|schedule|${C2}|2026-10-25T05:00:00Z" "40|schedule|${C1}|2026-10-24T04:00:00Z")" \
  FAKE_JOBS_45="$(jobs "451|full / Linux TSan|skipped|")" FAKE_JOBS_40="${J40_GREEN}"
expect skipped-never-green "exit 0" eq "${RC}" 0
expect skipped-never-green "last green is run 40" body_has "[run 40](https://github.com/We-Amp/example/actions/runs/40)"
expect skipped-never-green "run 45 is not green" no body_has "actions/runs/45"
expect skipped-never-green "run 45 was looked up" eq "$(calls 'actions/runs/45/jobs')" 1
done_case skipped-never-green

# --- skipped-job-listed-separately
start
run FAKE_JOBS_50="$(jobs "502|full / Linux TSan|failure|" "504|full / Windows Build|skipped|")" \
  FAKE_LOG_502="${TSAN_LOG}" FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect skipped-job-listed-separately "exit 0" eq "${RC}" 0
# shellcheck disable=SC2016 # literal backticks
expect skipped-job-listed-separately "skipped line" body_has 'Skipped (neither failed nor green): `full / Windows Build`'
expect skipped-job-listed-separately "no marker for the skipped job" no body_has "nightly-red-job: full / Windows Build"
done_case skipped-job-listed-separately

# --- runtime-step-check-makes-green-job-red
CHECK='full / Windows ASan=Run the unit tests under ASan'
start
run RUNTIME_STEP_CHECKS="${CHECK}" \
  FAKE_JOBS_50="$(jobs "505|full / Windows ASan|success|Build the IIS module with ASan=success;Run the unit tests under ASan=skipped")" \
  FAKE_RUNS_NIGHTLY="$(runs "45|schedule|${C2}|2026-10-25T05:00:00Z" "40|schedule|${C1}|2026-10-24T04:00:00Z")" \
  FAKE_JOBS_45="$(jobs "455|full / Windows ASan|success|Run the unit tests under ASan=skipped")" \
  FAKE_JOBS_40="$(jobs "405|full / Windows ASan|success|Run the unit tests under ASan=success")"
expect runtime-step-check "exit 0" eq "${RC}" 0
expect runtime-step-check "listed as failed" body_has "<!-- nightly-red-job: full / Windows ASan -->"
expect runtime-step-check "detail names the step" body_has 'step "Run the unit tests under ASan" concluded skipped'
expect runtime-step-check "same shape in history is not green" no body_has "actions/runs/45"
expect runtime-step-check "last green is run 40" body_has "[run 40]"
done_case runtime-step-check-makes-green-job-red

# The same job without the check is green: nothing to report.
start
run FAKE_JOBS_50="$(jobs "505|full / Windows ASan|success|Run the unit tests under ASan=skipped")"
expect runtime-step-check-unset "exit 0" eq "${RC}" 0
expect runtime-step-check-unset "no writes" eq "$(calls '^gh (issue (create|edit|comment|close)|label create)')" 0
done_case runtime-step-check-unset-job-is-green

# --- missed-night-finds-dispatch-green
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" \
  FAKE_RUNS_NIGHTLY="$(runs "46|workflow_dispatch|${C3}|2026-10-25T09:00:00Z" "30|schedule|${C1}|2026-10-20T04:00:00Z")" \
  FAKE_RUNS_CI_PUSH="$(runs "48|push|${C3}|2026-10-25T12:00:00Z" "47|push|${C3}|2026-10-25T11:00:00Z")" \
  FAKE_RUNS_CI_DISPATCH="$(runs "41|workflow_dispatch|${C2}|2026-10-25T08:00:00Z")" \
  FAKE_JOBS_48="$(jobs "481|pr / Linux Build|success|")" FAKE_JOBS_47="$(jobs "471|pr / Linux Build|success|")" \
  FAKE_JOBS_46="$(jobs "461|nightly / Fuzz CSS|success|")" \
  FAKE_JOBS_41="$(jobs "411|full / Linux TSan|success|")" FAKE_JOBS_30="$(jobs "301|full / Linux TSan|success|")"
expect missed-night "exit 0" eq "${RC}" 0
expect missed-night "last green is the dispatch" body_has "[run 41](https://github.com/We-Amp/example/actions/runs/41) (workflow_dispatch, \`${C2}\`), 2 commits since"
expect missed-night "older schedule run not used" no body_has "actions/runs/30"
expect missed-night "newest first: 48, 47, 46, 41" \
  eq "$(grep -oE 'actions/runs/[0-9]+/jobs' "${WORK}/gh.log" | tr '\n' ' ')" \
  "actions/runs/50/jobs actions/runs/48/jobs actions/runs/47/jobs actions/runs/46/jobs actions/runs/41/jobs "
done_case missed-night-finds-dispatch-green

# --- this-run-and-later-runs-excluded
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" \
  FAKE_RUNS_NIGHTLY="$(runs "52|workflow_dispatch|${C4}|2026-10-26T06:00:00Z" "50|schedule|${C4}|2026-10-26T03:59:00Z" "40|schedule|${C1}|2026-10-25T04:00:00Z")" \
  FAKE_JOBS_52="$(jobs "521|full / Linux TSan|success|")" FAKE_JOBS_40="${J40_GREEN}"
expect later-runs-excluded "exit 0" eq "${RC}" 0
expect later-runs-excluded "last green is run 40" body_has "[run 40]"
expect later-runs-excluded "run 52 never looked up" eq "$(calls 'actions/runs/52/jobs')" 0
expect later-runs-excluded "run 50 looked up once (itself)" eq "$(calls 'actions/runs/50/jobs')" 1
done_case this-run-and-later-runs-excluded

# --- lookup-cap
start
run REPORT_MAX_LOOKUPS=2 FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" \
  FAKE_RUNS_NIGHTLY="$(runs "46|schedule|${C3}|2026-10-25T04:00:00Z" "45|schedule|${C3}|2026-10-24T04:00:00Z" "44|schedule|${C2}|2026-10-23T04:00:00Z" "43|schedule|${C1}|2026-10-22T04:00:00Z")" \
  FAKE_JOBS_46="$(jobs "461|full / Linux TSan|failure|")" FAKE_JOBS_45="$(jobs "451|full / Linux TSan|failure|")" \
  FAKE_JOBS_44="$(jobs "441|full / Linux TSan|failure|")" FAKE_JOBS_43="$(jobs "431|full / Linux TSan|failure|")"
expect lookup-cap "exit 0" eq "${RC}" 0
expect lookup-cap "at most 2 history lookups" le "$(grep -E 'actions/runs/[0-9]+/jobs' "${WORK}/gh.log" | grep -vc 'actions/runs/50/jobs' || true)" 2
expect lookup-cap "body says the cap" body_has "no green within 2 completed full-tier runs"
done_case lookup-cap

# --- green-closes-issue
GREEN_JOBS="$(jobs "502|full / Linux TSan|success|" "503|full / Linux ASan + UBSan|success|" "501|nightly / Fuzz CSS|failure|")"
start
run FAKE_JOBS_50="${GREEN_JOBS}" FAKE_ISSUES="${ISSUE7}"
expect green-closes-issue "exit 0" eq "${RC}" 0
expect green-closes-issue "comment 7" eq "$(calls '^gh issue comment 7 --body-file')" 1
expect green-closes-issue "close 7" eq "$(calls '^gh issue close 7')" 1
expect green-closes-issue "comment before close" \
  eq "$(grep -oE '^gh issue (comment|close)' "${WORK}/gh.log" | tr '\n' ' ')" "gh issue comment gh issue close "
expect green-closes-issue "no history read" eq "$(calls 'actions/workflows/')" 0
expect green-closes-issue "no log fetch" eq "$(calls '^gh run view')" 0
done_case green-closes-issue

# --- rerun-attempt-green-comments-but-does-not-close
start
run GITHUB_RUN_ATTEMPT=2 FAKE_JOBS_50="${GREEN_JOBS}" FAKE_ISSUES="${ISSUE7}"
expect rerun-green "exit 0" eq "${RC}" 0
expect rerun-green "comment 7" eq "$(calls '^gh issue comment 7 --body-file')" 1
expect rerun-green "comment names the attempt" body_has "re-run attempt 2"
expect rerun-green "never closes" eq "$(calls '^gh issue close')" 0
done_case rerun-attempt-green-comments-but-does-not-close

# --- green-with-listed-job-skipped-stays-open
start
run FAKE_JOBS_50="$(jobs "502|full / Linux TSan|skipped|" "503|full / Linux ASan + UBSan|success|")" FAKE_ISSUES="${ISSUE7}"
expect listed-skipped "exit 0" eq "${RC}" 0
expect listed-skipped "no close, no comment" eq "$(calls '^gh issue (close|comment)')" 0
done_case green-with-listed-job-skipped-stays-open

# --- green-issue-without-markers-is-left-alone
start
run FAKE_JOBS_50="${GREEN_JOBS}" FAKE_ISSUES='[{"number":9,"body":"filed by hand"}]'
expect no-markers "exit 0" eq "${RC}" 0
expect no-markers "no writes" eq "$(calls '^gh issue (close|comment|edit)')" 0
done_case green-issue-without-markers-is-left-alone

# --- dry-run-writes-nothing
start
run DRY_RUN=true FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" FAKE_RUNS_NIGHTLY="${H40}" \
  FAKE_JOBS_40="${J40_GREEN}"
expect dry-run "exit 0" eq "${RC}" 0
expect dry-run "no writes" eq "$(calls '^gh (issue (create|edit|comment|close)|label create)')" 0
expect dry-run "says what it would do" grep -qF "dry run: would create" "${WORK}/out"
expect dry-run "prints the body" grep -qF "//t:race_test" "${WORK}/out"
done_case dry-run-writes-nothing

start
run DRY_RUN=true FAKE_JOBS_50="${GREEN_JOBS}" FAKE_ISSUES="${ISSUE7}"
expect dry-run-close "exit 0" eq "${RC}" 0
expect dry-run-close "no writes" eq "$(calls '^gh issue (comment|close)')" 0
expect dry-run-close "says it would close" grep -qF "dry run: would close issue #7" "${WORK}/out"
done_case dry-run-close-writes-nothing

# --- no-full-jobs-noop
start
run FAKE_JOBS_50="$(jobs "501|nightly / Fuzz CSS|failure|")" FAKE_ISSUES="${ISSUE7}"
expect no-full-jobs "exit 0" eq "${RC}" 0
expect no-full-jobs "no issue calls" eq "$(calls '^gh issue')" 0
done_case no-full-jobs-noop

# --- api failures
start
run FAKE_JOBS_FAIL_50=1
expect api-failure "exit 1" eq "${RC}" 1
expect api-failure "error annotation" grep -qF "::error::" "${WORK}/err"
done_case api-failure-exits-1

start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_FAIL_40=1
expect history-api-failure "exit 1" eq "${RC}" 1
expect history-api-failure "no writes" eq "$(calls '^gh (issue (create|edit|comment|close)|label create)')" 0
done_case history-jobs-api-failure-exits-1

start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" FAKE_RUNS_NIGHTLY='{"message":"Server Error"}'
expect runs-unusable "exit 1" eq "${RC}" 1
done_case runs-answer-unusable-exits-1

start
run FAKE_JOBS_50="${GREEN_JOBS}" FAKE_ISSUES_FAIL=1
expect issue-list-failure "exit 1" eq "${RC}" 1
done_case issue-list-failure-exits-1

start
run FAKE_JOBS_50=""
expect empty-jobs-answer "exit 1" eq "${RC}" 1
done_case empty-jobs-answer-exits-1

# --- no-targets-extracted
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="" FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect no-targets "exit 0" eq "${RC}" 0
expect no-targets "none extracted" body_has "Failing targets: none extracted"
done_case no-targets-extracted

# --- timeout-only
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502='//t:slow_test    TIMEOUT in 300.0s' FAKE_RUNS_NIGHTLY="${H40}" \
  FAKE_JOBS_40="${J40_GREEN}"
expect timeout-only "exit 0" eq "${RC}" 0
expect timeout-only "marked timeout-only" body_has "Failing targets (timeout-only):"
# shellcheck disable=SC2016 # literal backticks
expect timeout-only "target listed" body_has '`//t:slow_test` (test-timeout)'
done_case timeout-only

start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="$(printf '//t:slow_test    TIMEOUT in 300.0s\n//t:race_test    FAILED in 3.1s')" \
  FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect mixed-kinds "exit 0" eq "${RC}" 0
expect mixed-kinds "not timeout-only" no body_has "timeout-only"
done_case mixed-kinds-are-not-timeout-only

# --- no-green-in-history
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}"
expect no-history "exit 0" eq "${RC}" 0
expect no-history "no green within 0" body_has "no green within 0 completed full-tier runs"
done_case no-history-says-no-green

# --- primary-log-fetch-has-no-escape-flag (gh run view has no such flag)
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect primary-log-fetch "exact call" grep -qxF -- "gh run view 50 --job 502 --log-failed" "${WORK}/gh.log"
expect primary-log-fetch "one fetch per failed job" eq "$(calls '^gh run view')" 1
expect primary-log-fetch "no fallback" eq "$(calls 'actions/jobs/')" 0
expect primary-log-fetch "target extracted" body_has "//t:race_test"
done_case primary-log-fetch-has-no-escape-flag

# --- fallback-uses-api-escape-flag
start
run FAKE_VIEW_FAIL=1 FAKE_JOBS_50="${RED_JOBS}" \
  FAKE_RAWLOG_502='2026-10-26T04:12:30.0000000Z //t:race_test    FAILED in 3.1s' \
  FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect fallback-flag "exit 0" eq "${RC}" 0
expect fallback-flag "exact call" \
  grep -qxF -- "gh api --allow-escape-sequences repos/We-Amp/example/actions/jobs/502/logs" "${WORK}/gh.log"
expect fallback-flag "target extracted" body_has "//t:race_test"
done_case fallback-uses-api-escape-flag

# --- the escape-sequence refusal is no log (exit 0 and exit 1 forms)
for rc in 0 1; do
  start
  run FAKE_VIEW_FAIL=1 FAKE_FORCE_REFUSAL=1 FAKE_REFUSAL_RC="${rc}" FAKE_JOBS_50="${RED_JOBS}" \
    FAKE_RAWLOG_502='//t:race_test    FAILED in 3.1s' FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
  expect refusal "exit 0" eq "${RC}" 0
  expect refusal "log not available" body_has "none extracted (log not available)"
  expect refusal "not marked as read" no body_has "from the whole job log"
  done_case "escape-refusal-is-no-log (gh exit ${rc})"
done

# --- log-fetch-falls-back-to-the-job-log-api
start
run FAKE_VIEW_FAIL=1 FAKE_JOBS_50="${RED_JOBS}" \
  FAKE_RAWLOG_502='2026-10-26T04:12:30.0000000Z //t:race_test    FAILED in 3.1s' \
  FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect log-fallback "exit 0" eq "${RC}" 0
expect log-fallback "job log API read" grep -qxF -- "gh api --allow-escape-sequences repos/We-Amp/example/actions/jobs/502/logs" "${WORK}/gh.log"
expect log-fallback "target extracted" body_has "//t:race_test"
expect log-fallback "marked as from the whole job log" body_has "- Failing targets (from the whole job log):"
done_case log-fetch-falls-back-to-the-job-log-api

start
run FAKE_VIEW_FAIL=1 FAKE_JOBS_50="${RED_JOBS}" FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect log-missing "exit 0 (the issue still lands)" eq "${RC}" 0
expect log-missing "says so" body_has "none extracted (log not available)"
done_case missing-log-still-reports

# --- paginated-jobs-are-merged
start
run FAKE_JOBS_50="$(jobs "501|nightly / Fuzz CSS|success|")$(jobs "502|full / Linux TSan|failure|")" \
  FAKE_LOG_502="${TSAN_LOG}" FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect paginated "exit 0" eq "${RC}" 0
expect paginated "second page's job reported" body_has "<!-- nightly-red-job: full / Linux TSan -->"
done_case paginated-jobs-are-merged

# --- rerun-all-jobs-keeps-original-cutoff: a re-run of all jobs gives every
# job a new created_at; the run object keeps the original one.
start
run GITHUB_RUN_ATTEMPT=2 FAKE_RUN_CREATED=2026-10-26T04:00:00Z \
  FAKE_JOBS_50="$(jobs "502|full / Linux TSan|failure|" | sed 's/2026-10-26T04:00:00Z/2026-10-27T09:00:00Z/g')" \
  FAKE_LOG_502="${TSAN_LOG}" \
  FAKE_RUNS_CI_PUSH="$(runs "60|push|${C4}|2026-10-26T20:00:00Z")" FAKE_JOBS_60="$(jobs "601|full / Linux TSan|success|")" \
  FAKE_RUNS_NIGHTLY="${H40}" FAKE_JOBS_40="${J40_GREEN}"
expect rerun-cutoff "exit 0" eq "${RC}" 0
expect rerun-cutoff "newer run not picked" no body_has "actions/runs/60"
expect rerun-cutoff "newer run never looked up" eq "$(calls 'actions/runs/60/jobs')" 0
expect rerun-cutoff "last green is run 40" body_has "[run 40]"
done_case rerun-all-jobs-keeps-original-cutoff

start
run FAKE_RUN_FAIL=1 FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}"
expect run-object-failure "exit 1" eq "${RC}" 1
expect run-object-failure "no writes" eq "$(calls '^gh (issue (create|edit|comment|close)|label create)')" 0
done_case run-object-failure-exits-1

# --- push-runs-without-full-tier-do-not-consume-cap
PUSH15=()
for n in $(seq 70 84); do
  PUSH15+=("${n}|push|${C3}|2026-10-25T$((n - 60)):00:00Z")
  export "FAKE_JOBS_${n}=$(jobs "${n}1|pr / Linux Build|success|")"
done
start
run FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" \
  FAKE_RUNS_CI_PUSH="$(runs "${PUSH15[@]}")" \
  FAKE_RUNS_NIGHTLY="$(runs "40|schedule|${C1}|2026-10-25T04:00:00Z")" FAKE_JOBS_40="${J40_GREEN}"
expect push-cap "exit 0" eq "${RC}" 0
expect push-cap "all 15 push runs looked up" eq "$(grep -cE 'actions/runs/(7[0-9]|8[0-4])/jobs' "${WORK}/gh.log" || true)" 15
expect push-cap "the nightly is found" body_has "[run 40](https://github.com/We-Amp/example/actions/runs/40) (schedule"
done_case push-runs-without-full-tier-do-not-consume-cap

# --- api-call-ceiling-stops-search
start
run REPORT_MAX_API_CALLS=5 FAKE_JOBS_50="${RED_JOBS}" FAKE_LOG_502="${TSAN_LOG}" \
  FAKE_RUNS_CI_PUSH="$(runs "${PUSH15[@]}")" \
  FAKE_RUNS_NIGHTLY="$(runs "40|schedule|${C1}|2026-10-25T04:00:00Z")" FAKE_JOBS_40="${J40_GREEN}"
expect ceiling "exit 0" eq "${RC}" 0
expect ceiling "5 history jobs calls" eq "$(grep -E 'actions/runs/[0-9]+/jobs' "${WORK}/gh.log" | grep -vc 'actions/runs/50/jobs' || true)" 5
expect ceiling "body says so" body_has "no green within 0 completed full-tier runs (API call ceiling reached)"
done_case api-call-ceiling-stops-search
for n in $(seq 70 84); do unset "FAKE_JOBS_${n}"; done

# --- missing-env
start
run DEFAULT_BRANCH=
expect missing-env "exit 1" eq "${RC}" 1
expect missing-env "no gh call" eq "$(calls '^gh ')" 0
done_case missing-env-exits-1

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} check(s) failed"
  exit 1
fi
echo "all checks passed"
