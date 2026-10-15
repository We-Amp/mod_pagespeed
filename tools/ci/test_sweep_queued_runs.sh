#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/sweep-queued-runs.sh with a fake gh that serves canned
# JSON and records the cancellations it is asked for. No network, no token.
#
# Run: tools/ci/test_sweep_queued_runs.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SWEEP="${HERE}/sweep-queued-runs.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
mkdir -p "${TMP}/bin" "${TMP}/fix"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

# "Now" is 2026-10-14T12:00:00Z.
NOW=1791979200

cat > "${TMP}/fix/runs-queued.json" <<'JSON'
{"workflow_runs": [
  {"id": 101, "name": "CI",      "created_at": "2026-10-14T11:00:00Z"},
  {"id": 102, "name": "CI",      "created_at": "2026-10-14T11:45:00Z"},
  {"id": 103, "name": "Nightly", "created_at": "2026-10-14T10:00:00Z"},
  {"id": 104, "name": "Release", "created_at": "2026-10-14T09:00:00Z"},
  {"id": 105, "name": "CI",      "created_at": "2026-10-14T06:00:00Z", "run_started_at": "2026-10-14T11:50:00Z"},
  {"id": 106, "name": "CI",      "created_at": "2026-10-14T11:00:00Z", "run_started_at": null},
  {"id": 107, "name": "CI",      "created_at": "2026-10-14T11:00:00Z", "run_started_at": "2026-10-14T11:00:00Z"}
]}
JSON
cat > "${TMP}/fix/runs-waiting.json" <<'JSON'
{"workflow_runs": [
  {"id": 201, "name": "CI", "created_at": "2026-10-14T11:00:00Z"}
]}
JSON
echo '{"jobs": [{"id": 1, "status": "queued", "runner_id": null}]}' > "${TMP}/fix/jobs-101.json"
echo '{"jobs": [{"id": 2, "status": "in_progress", "runner_id": 5}, {"id": 3, "status": "queued", "runner_id": null}]}' > "${TMP}/fix/jobs-103.json"
echo '{"jobs": [{"id": 4, "status": "queued", "runner_id": null}]}' > "${TMP}/fix/jobs-104.json"
echo '{"jobs": []}' > "${TMP}/fix/jobs-201.json"
echo '{"jobs": [{"id": 5, "status": "queued", "runner_id": null}]}' > "${TMP}/fix/jobs-106.json"
echo '{"jobs": [{"id": 6, "status": "queued", "runner_id": null}]}' > "${TMP}/fix/jobs-107.json"

cat > "${TMP}/bin/gh" <<'GH'
#!/usr/bin/env bash
# Fake gh: serves ${FAKE_FIX} and logs cancellations to ${FAKE_LOG}.
set -euo pipefail
case "$1" in
  api)
    case "$2" in
      */actions/runs\?status=queued*)  cat "${FAKE_FIX}/runs-queued.json" ;;
      */actions/runs\?status=waiting*) cat "${FAKE_FIX}/runs-waiting.json" ;;
      */actions/runs/*/jobs*)
        id="${2#*/actions/runs/}"; id="${id%%/*}"
        cat "${FAKE_FIX}/jobs-${id}.json"
        ;;
      *) echo "fake gh: unexpected api call: $2" >&2; exit 1 ;;
    esac
    ;;
  run)
    if [ "$2" = "cancel" ]; then
      echo "$3" >> "${FAKE_LOG}"
    else
      echo "fake gh: unexpected run call: $*" >&2; exit 1
    fi
    ;;
  *) echo "fake gh: unexpected call: $*" >&2; exit 1 ;;
esac
GH
chmod +x "${TMP}/bin/gh"

run_sweep() { # <args...>; sets OUT and CANCELLED
  : > "${TMP}/log"
  OUT="$(PATH="${TMP}/bin:${PATH}" FAKE_FIX="${TMP}/fix" FAKE_LOG="${TMP}/log" \
         GITHUB_REPOSITORY=example/repo SWEEP_NOW_EPOCH="${NOW}" \
         bash "${SWEEP}" "$@" 2>&1)" || { fail "sweep exited non-zero: ${OUT}"; return 1; }
  CANCELLED="$(tr '\n' ' ' < "${TMP}/log" | sed 's/ $//')"
}

expect() { # <description> <got> <want>
  if [ "$2" = "$3" ]; then pass "$1"; else fail "$1: got '$2', want '$3'"; fi
}
expect_out() { # <description> <substring>
  if printf '%s' "${OUT}" | grep -qF "$2"; then pass "$1"; else fail "$1: no '$2' in: ${OUT}"; fi
}

# Default run: the stale queued runs and the stale waiting run go; the young
# run, the run with a runner and the skip-listed run stay. Run 105 is a
# re-run: created hours ago, but its latest attempt started 10 minutes ago, so
# it must be kept. Run 106 has a null run_started_at and falls back to
# created_at (60 minutes). Run 107 carries both fields (60 minutes).
run_sweep
expect "default run cancels 101 106 107 201 only" "${CANCELLED}" "101 106 107 201"
expect_out "a re-run is not stale (age from run_started_at)" "examined 6 stale run(s)"
if printf '%s' "${OUT}" | grep -q 'run 105'; then fail "re-run 105 was examined"; else pass "re-run 105 is left alone"; fi
expect_out "log names the cancelled run" "cancelled run 101 (CI, queued 60 min, no runner assigned)"
expect_out "log explains the run with a runner" "kept run 103 (Nightly, queued 120 min): a job has a runner"
expect_out "log explains the skip list" "kept run 104 (Release, queued 180 min): workflow is on the skip list"

# Dry run: nothing is cancelled, the log says what would be.
run_sweep --dry-run
expect "dry run cancels nothing" "${CANCELLED}" ""
expect_out "dry run logs the intent" "would cancel run 101"
expect_out "dry run summary" "would cancel 4"

# A longer threshold leaves the 60 minute runs alone.
run_sweep --max-age 90
expect "max-age 90 cancels nothing" "${CANCELLED}" ""

# An empty skip list puts the release run in play.
SWEEP_SKIP_WORKFLOWS="" run_sweep
expect "empty skip list cancels 104 101 106 107 201 (oldest first)" "${CANCELLED}" "104 101 106 107 201"

# The repository variable sets the default threshold; --max-age wins over it.
SWEEP_MAX_AGE_MINUTES=90 run_sweep
expect "SWEEP_MAX_AGE_MINUTES=90 cancels nothing" "${CANCELLED}" ""
SWEEP_MAX_AGE_MINUTES=90 run_sweep --max-age 30
expect "--max-age overrides SWEEP_MAX_AGE_MINUTES" "${CANCELLED}" "101 106 107 201"
SWEEP_MAX_AGE_MINUTES="" run_sweep
expect "an empty SWEEP_MAX_AGE_MINUTES means 30" "${CANCELLED}" "101 106 107 201"

# Bad arguments.
if bash "${SWEEP}" --max-age abc >/dev/null 2>&1; then
  fail "a non-numeric --max-age was accepted"
else
  pass "a non-numeric --max-age is rejected"
fi

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All sweep cases passed"
