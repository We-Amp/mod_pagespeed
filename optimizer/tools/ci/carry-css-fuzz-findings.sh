#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# carry-css-fuzz-findings.sh - carry needs-triage CSS fuzz crash units from
# one scheduled run to the next, so the tracking issue (label
# css-fuzz-finding) closes only when the reported units replay clean, not on
# the first night the fuzzer happens not to rediscover them.
#
#   carry-css-fuzz-findings.sh in  OPEN_DIR CRASH_DIR FINDINGS_MD CACHE_KEY
#   carry-css-fuzz-findings.sh out OPEN_DIR CRASH_DIR FINDINGS_MD
#   carry-css-fuzz-findings.sh --self-test
#
# OPEN_DIR is the directory the workflow restores from and saves to its
# open-findings cache. CACHE_KEY is the key the restore matched (empty on a
# miss).
#
# `in` (before the classifier): copies every carried unit into CRASH_DIR, so
# tools/ci/classify-css-fuzz-artifacts.sh replays it with tonight's units (a
# unit that still fails is listed in FINDINGS_MD again; one that replays
# clean is counted stale and dropped). It also guards the carry itself. A
# cache hit that restored at least one unit is trusted. In every other case
# (the restore missed, it restored a set with no units, or an earlier run
# left the `unverified` marker) it looks up the open tracking issue: a set
# with no units is also what a failed or colliding cache save leaves behind
# when an older entry is restored. When a tracking issue is open, or the
# lookup fails, the earlier units cannot be re-verified, so it sets the
# marker and appends an entry to FINDINGS_MD, which keeps the issue open.
# Once a lookup finds no tracking issue open, the marker is dropped.
#
# Fail closed by design: the marker sticks for as long as the lookup keeps
# failing, or the issue stays open, so a broken lookup can hold the issue
# open but can never close it.
#
# `out` (after the classifier): rewrites OPEN_DIR to hold exactly the units
# FINDINGS_MD lists (the classifier names each as "### `<unit>`"), plus the
# marker when it is set, plus a .keep file so the cache save never sees an
# empty directory.
#
# The open-issue lookup is `gh issue list` on GITHUB_REPOSITORY; the
# self-test replaces it through CSS_FUZZ_OPEN_ISSUES_CMD.

set -uo pipefail

LABEL="css-fuzz-finding"
MARKER="unverified"

usage() {
  echo "Usage: $0 in OPEN_DIR CRASH_DIR FINDINGS_MD CACHE_KEY" >&2
  echo "       $0 out OPEN_DIR CRASH_DIR FINDINGS_MD" >&2
  echo "       $0 --self-test" >&2
  exit 2
}

# open_issue_count — prints the number of open tracking issues, or fails.
open_issue_count() {
  if [[ -n "${CSS_FUZZ_OPEN_ISSUES_CMD:-}" ]]; then
    bash -c "$CSS_FUZZ_OPEN_ISSUES_CMD"
    return
  fi
  gh issue list --repo "${GITHUB_REPOSITORY:?}" --label "$LABEL" \
    --state open --json number --jq 'length'
}

is_unit() {  # $1=basename
  case "$1" in
    crash-*|oom-*|timeout-*) return 0 ;;
    *) return 1 ;;
  esac
}

carry_in() {
  local open_dir="$1" crash_dir="$2" findings="$3" cache_key="$4"
  mkdir -p "$open_dir" "$crash_dir"
  [[ -e "$findings" ]] || : > "$findings"
  local f name carried=0
  for f in "$open_dir"/*; do
    [[ -f "$f" ]] || continue
    name="$(basename "$f")"
    is_unit "$name" || continue
    # A unit the fuzzer found again tonight has the same name (libFuzzer
    # names units by content hash) and the same bytes: keep tonight's.
    [[ -e "$crash_dir/$name" ]] || cp "$f" "$crash_dir/$name"
    carried=$((carried + 1))
  done
  echo "css-fuzz carry: ${carried} open finding unit(s) carried from earlier runs"

  # Trust only a hit that restored units: an empty restored set may be an
  # older entry left by a failed or colliding save.
  if [[ -n "$cache_key" && "$carried" -gt 0 && ! -e "$open_dir/$MARKER" ]]; then
    return 0
  fi
  local count
  if ! count="$(open_issue_count 2>/dev/null)" || ! [[ "$count" =~ ^[0-9]+$ ]]; then
    count="unknown"
  fi
  if [[ "$count" == "0" ]]; then
    rm -f "$open_dir/$MARKER"
    return 0
  fi
  touch "$open_dir/$MARKER"
  {
    echo "### earlier findings could not be re-verified"
    echo
    echo "The open-findings cache was missing or held no units on a run while the tracking issue was open (open issues: ${count}), so the units it reported were not replayed. Auto-close stays off until this issue is closed by hand, after its units have been checked against main."
    echo
  } >> "$findings"
  echo "css-fuzz carry: earlier units not re-verifiable; auto-close suppressed"
}

carry_out() {
  local open_dir="$1" crash_dir="$2" findings="$3"
  local next="${open_dir}.next"
  rm -rf "$next"
  mkdir -p "$next"
  touch "$next/.keep"
  [[ -e "$open_dir/$MARKER" ]] && touch "$next/$MARKER"
  local f name kept=0
  for f in "$crash_dir"/*; do
    [[ -f "$f" ]] || continue
    name="$(basename "$f")"
    is_unit "$name" || continue
    if [[ -f "$findings" ]] && grep -qF "### \`${name}\`" "$findings"; then
      cp "$f" "$next/$name"
      kept=$((kept + 1))
    fi
  done
  rm -rf "$open_dir"
  mv "$next" "$open_dir"
  echo "css-fuzz carry: ${kept} open finding unit(s) saved for the next run"
}

self_test() {
  local tmp fails=0
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN

  check() {  # $1=description, rest=command
    local desc="$1"
    shift
    if "$@"; then
      echo "ok   - $desc"
    else
      echo "FAIL - $desc"
      fails=$((fails + 1))
    fi
  }

  local O="$tmp/open" C="$tmp/crash" F="$tmp/findings.md"

  # 1. Cache hit, no marker: units are carried, no lookup, no entry.
  mkdir -p "$O" "$C"
  printf 'a' > "$O/crash-aaa"
  touch "$O/.keep"
  : > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='exit 1' carry_in "$O" "$C" "$F" "key-1" >/dev/null
  check "hit: unit copied into the crash dir" test -f "$C/crash-aaa"
  check "hit: .keep is not copied" test ! -e "$C/.keep"
  check "hit: no findings entry" test ! -s "$F"

  # 2. A unit the classifier still lists is kept; a clean one is dropped.
  printf 'b' > "$C/crash-bbb"
  # shellcheck disable=SC2016  # literal backticks: the classifier's format
  printf '### `crash-aaa` — token-stream oracle\n' > "$F"
  carry_out "$O" "$C" "$F" >/dev/null
  check "out: listed unit kept" test -f "$O/crash-aaa"
  check "out: unlisted unit dropped" test ! -e "$O/crash-bbb"
  check "out: .keep written" test -f "$O/.keep"
  check "out: no marker" test ! -e "$O/$MARKER"

  # 3. Everything replays clean: the set empties (only .keep remains).
  : > "$F"
  carry_out "$O" "$C" "$F" >/dev/null
  check "out: empty findings empty the set" \
    test "$(find "$O" -type f ! -name .keep | wc -l | tr -d ' ')" = "0"

  # 4. Cache miss with an open issue: marker set, entry written.
  rm -rf "$O" "$C"
  : > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='echo 1' carry_in "$O" "$C" "$F" "" >/dev/null
  check "miss+open: marker set" test -e "$O/$MARKER"
  check "miss+open: findings entry" grep -q "could not be re-verified" "$F"
  carry_out "$O" "$C" "$F" >/dev/null
  check "miss+open: marker carried to the next run" test -e "$O/$MARKER"

  # 5. Marker carried, issue still open: entry again, even on a cache hit.
  : > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='echo 1' carry_in "$O" "$C" "$F" "key-2" >/dev/null
  check "marker+open: findings entry again" grep -q "could not be re-verified" "$F"

  # 6. Marker carried, issue closed by hand: marker dropped, no entry.
  : > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='echo 0' carry_in "$O" "$C" "$F" "key-3" >/dev/null
  check "marker+closed: marker dropped" test ! -e "$O/$MARKER"
  check "marker+closed: no entry" test ! -s "$F"

  # 7. Cache miss, lookup fails: fail closed.
  rm -rf "$O"
  : > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='exit 3' carry_in "$O" "$C" "$F" "" >/dev/null
  check "miss+lookup failed: entry (fail closed)" grep -q "could not be re-verified" "$F"

  # 8. Cache miss, no open issue: nothing to verify.
  rm -rf "$O"
  : > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='echo 0' carry_in "$O" "$C" "$F" "" >/dev/null
  check "miss+none open: no entry" test ! -s "$F"
  check "miss+none open: no marker" test ! -e "$O/$MARKER"

  # 9. Cache hit that restored no units (an older entry after a failed or
  #    colliding save) while an issue is open: fail closed.
  rm -rf "$O" "$C"
  mkdir -p "$O"
  touch "$O/.keep"
  : > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='echo 1' carry_in "$O" "$C" "$F" "key-4" >/dev/null
  check "hit+no units+open: findings entry" grep -q "could not be re-verified" "$F"
  check "hit+no units+open: marker set" test -e "$O/$MARKER"

  # 10. Same, but no issue open: a clean night, nothing to verify.
  rm -rf "$O" "$C"
  mkdir -p "$O"
  touch "$O/.keep"
  : > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='echo 0' carry_in "$O" "$C" "$F" "key-5" >/dev/null
  check "hit+no units+none open: no entry" test ! -s "$F"
  check "hit+no units+none open: no marker" test ! -e "$O/$MARKER"

  # 11. Entries the caller wrote before `in` survive (append mode).
  printf 'replay floor red\n' > "$F"
  CSS_FUZZ_OPEN_ISSUES_CMD='echo 1' carry_in "$O" "$C" "$F" "" >/dev/null
  check "append: earlier entry kept" grep -q "replay floor red" "$F"

  if [[ "$fails" -ne 0 ]]; then
    echo "carry-css-fuzz-findings self-test: ${fails} failure(s)"
    return 1
  fi
  echo "carry-css-fuzz-findings self-test: all checks passed"
}

case "${1:-}" in
  in) [[ $# -eq 5 ]] || usage; carry_in "$2" "$3" "$4" "$5" ;;
  out) [[ $# -eq 4 ]] || usage; carry_out "$2" "$3" "$4" ;;
  --self-test) self_test ;;
  *) usage ;;
esac
