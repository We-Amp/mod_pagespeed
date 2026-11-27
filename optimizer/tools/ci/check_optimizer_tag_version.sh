#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Release guard of the optimizer lane (pipeline-release-optimizer.yml): the tag
# `optimizer-v<X>` must name the version VERSION.txt carries at the tagged
# commit -- the number the daemon's --version and /v1/health report, generated
# from VERSION.txt. The NuGet lane holds `v<X>` to VERSION.txt the same way
# (pipeline-release-nuget.yml, "Verify VERSION.txt matches the release tag").
#
# Enforced from 2.5.0 on. Before it the host packages were numbered on the
# module's 1.x line while VERSION.txt carried the product line (2.N ==
# 1.(N+15): optimizer-v1.19.0 was built from VERSION.txt 2.4.0). Nothing more
# ships on that numbering, but the tags published on it (optimizer-v1.16.0 to
# optimizer-v1.19.0) must still verify, so below 2.5.0 the guard passes with a
# notice.
#
# --dry-run means the release is not final: the tag does not exist yet and the
# lane builds the dispatched ref instead (the lane passes the flag only then).
# In that state a mismatch is a warning (a `::warning::` annotation and a line
# in $GITHUB_STEP_SUMMARY) and a `-pre` tag is not checked. Once the tag
# exists the flag is not passed and a mismatch fails, a dry run included.
#
# A pushed `-pre` tag runs the lane as a real run, like any other tag (the
# lane releases every prerelease as a GitHub pre-release and special-cases
# none), so without --dry-run a `-pre` tag is checked like a release
# candidate: its X.Y.Z core must equal VERSION.txt. VERSION.txt (leading and
# trailing whitespace stripped) may equal the full version or its X.Y.Z core.
#
#   check_optimizer_tag_version.sh TAG VERSION_FILE [--dry-run]
#       exit 0: match, a skip (notice) or a --dry-run warning; 1: mismatch;
#       2: bad input (a bad tag, a missing or empty VERSION file).
#   check_optimizer_tag_version.sh --self-test
set -euo pipefail

ENFORCED_SINCE="2.5.0"
SEMVER_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-([0-9A-Za-z]+(\.[0-9A-Za-z]+)*))?$'

# core_lt A B: X.Y.Z A < X.Y.Z B, numerically per field.
core_lt() {
  local IFS=. i
  # shellcheck disable=SC2206  # splitting on IFS=. is the point
  local -a a=($1) b=($2)
  for i in 0 1 2; do
    (( a[i] < b[i] )) && return 0
    (( a[i] > b[i] )) && return 1
  done
  return 1
}

# trim TEXT: TEXT without leading and trailing whitespace (inner kept).
trim() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "$s"
}

# verdict TAG VERSION_TEXT [--dry-run]: prints "pass|skip|warn|fail<TAB>message";
# returns 2 on bad input.
verdict() {
  local tag="$1" have="$2" dry="${3:-}" ver core pre msg
  case "$tag" in
    optimizer-v[0-9]*) ;;
    *) echo "tag '$tag' does not match optimizer-v<X.Y.Z>[-<prerelease>]" >&2; return 2 ;;
  esac
  ver="${tag#optimizer-v}"
  if ! printf '%s' "$ver" | grep -Eq "$SEMVER_RE"; then
    echo "'$ver' is not X.Y.Z[-prerelease] (no build metadata)" >&2; return 2
  fi
  core="${ver%%-*}"
  pre=""; case "$ver" in *-*) pre="${ver#*-}" ;; esac
  have="$(trim "$have")"
  if [ "$dry" = --dry-run ] && [[ "$pre" =~ ^pre([.0-9]|$) ]]; then
    printf 'skip\t%s is a dry-run tag (-%s) and the release is not final; the VERSION.txt guard ignores it\n' "$tag" "$pre"
  elif core_lt "$core" "$ENFORCED_SINCE"; then
    printf 'skip\t%s is below %s: the legacy 1.x host-package numbering paired it with VERSION.txt %s; the equality is enforced from %s on\n' \
      "$tag" "$ENFORCED_SINCE" "${have:-?}" "$ENFORCED_SINCE"
  elif [ -z "$have" ]; then
    echo "VERSION.txt is empty" >&2; return 2
  elif [ "$have" = "$ver" ] || [ "$have" = "$core" ]; then
    printf 'pass\tVERSION.txt=%s matches release tag %s\n' "$have" "$tag"
  else
    msg="VERSION.txt ($have) != release tag ($tag -> $core). Bump VERSION.txt on main and tag the bump commit."
    if [ "$dry" = --dry-run ]; then
      printf 'warn\trelease not final (dry run): %s\n' "$msg"
    else
      printf 'fail\t%s\n' "$msg"
    fi
  fi
}

run() {
  local tag="${1:-}" file="${2:-}" dry="${3:-}" out v msg text=""
  # The file is only needed when the check applies; verdict refuses an empty
  # (or, here, missing) one then.
  if [ -f "$file" ]; then text="$(cat "$file")"; fi
  if ! out="$(verdict "$tag" "$text" "$dry" 2>&1)"; then
    [ -f "$file" ] || out="no such VERSION file: $file ($out)"
    echo "::error::$out" >&2; return 2
  fi
  v="${out%%$'\t'*}"; msg="${out#*$'\t'}"
  case "$v" in
    pass) echo "$msg" ;;
    skip) echo "::notice::$msg" ;;
    warn)
      echo "::warning::$msg"
      if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        printf '### Optimizer tag version guard\n\n:warning: %s\n\n' "$msg" >> "$GITHUB_STEP_SUMMARY"
      fi ;;
    fail) echo "::error::$msg" >&2; return 1 ;;
  esac
}

self_test() {
  local fail=0 tag have mode want got line flag
  while read -r tag have mode want; do
    [ -n "$tag" ] || continue
    [ "$have" = '-' ] && have=""
    flag=""; [ "$mode" = dry ] && flag=--dry-run
    got="$(verdict "$tag" "$have" "$flag" 2>/dev/null)" || got="refused"
    got="${got%%$'\t'*}"
    if [ "$got" = "$want" ]; then line=ok; else line="FAIL (want $want)"; fail=1; fi
    printf '  %-26s %-11s %-5s %-8s %s\n' "$tag" "${have:-''}" "$mode" "$got" "$line"
  done <<'EOF'
optimizer-v2.5.0           2.5.0       real pass
optimizer-v2.5.1           2.5.0       real fail
optimizer-v2.5.0           2.4.0       real fail
optimizer-v2.5.0           2.4.0       dry  warn
optimizer-v2.5.0-rc.1      2.5.0       real pass
optimizer-v2.5.0-rc.1      2.5.0-rc.1  real pass
optimizer-v2.5.0-rc.1      2.4.0       real fail
optimizer-v2.10.0          2.10.0      real pass
optimizer-v2.10.0          2.1.0       real fail
optimizer-v3.0.0           2.9.0       real fail
optimizer-v1.19.0          2.4.0       real skip
optimizer-v1.19.1          2.4.0       real skip
optimizer-v1.100.0         2.4.0       real skip
optimizer-v2.4.0           2.3.0       real skip
optimizer-v1.19.1-pre.1    2.4.0       real skip
optimizer-v2.5.0-pre.2     2.4.0       dry  skip
optimizer-v2.5.0-pre.2     2.4.0       real fail
optimizer-v2.5.0-pre.2     2.5.0       real pass
optimizer-v2.5.0-preview.1 2.4.0       dry  warn
optimizer-v2.5.0           -           real refused
optimizer-v2.5             2.5.0       real refused
optimizer-v2.5.0+r1        2.5.0       real refused
optimizer-v02.5.0          2.5.0       real refused
v2.5.0                     2.5.0       real refused
module-v2.5.0              2.5.0       real refused
EOF
  # run(): whitespace, exit codes, missing file, summary, end to end
  local t rc; t="$(mktemp -d)"
  printf '  2.5.0 \n\n' > "$t/V"
  run optimizer-v2.5.0 "$t/V" >/dev/null 2>&1 || { echo "FAIL: run pass (padded VERSION.txt)"; fail=1; }
  # Inner whitespace is kept and so refused: `2 .5.0` reads 2.5.0 under a
  # collapsing tr -d, so the case tells trim from collapse.
  printf '2 .5.0\n' > "$t/Vin"
  if run optimizer-v2.5.0 "$t/Vin" >/dev/null 2>&1; then echo "FAIL: inner whitespace accepted"; fail=1; fi
  run optimizer-v1.19.0 "$t/V" >/dev/null 2>&1 || { echo "FAIL: run skip"; fail=1; }
  if run optimizer-v2.5.1 "$t/V" >/dev/null 2>&1; then echo "FAIL: run mismatch accepted"; fail=1; fi
  GITHUB_STEP_SUMMARY="$t/summary" run optimizer-v2.5.1 "$t/V" --dry-run 2>/dev/null | grep -q '^::warning::release not final' \
    || { echo "FAIL: dry-run mismatch should warn"; fail=1; }
  grep -q 'VERSION.txt (2.5.0) != release tag (optimizer-v2.5.1' "$t/summary" 2>/dev/null \
    || { echo "FAIL: dry-run warning missing from the step summary"; fail=1; }
  rc=0; run optimizer-v2.5 "$t/V" >/dev/null 2>&1 || rc=$?
  [ "$rc" = 2 ] || { echo "FAIL: bad tag exit $rc, want 2"; fail=1; }
  rc=0; run optimizer-v2.5.0 "$t/missing" >/dev/null 2>&1 || rc=$?
  [ "$rc" = 2 ] || { echo "FAIL: missing file exit $rc, want 2"; fail=1; }
  : > "$t/empty"
  rc=0; run optimizer-v2.5.0 "$t/empty" >/dev/null 2>&1 || rc=$?
  [ "$rc" = 2 ] || { echo "FAIL: empty file exit $rc, want 2"; fail=1; }
  rm -rf "$t"
  if [ "$fail" = 0 ]; then echo "check_optimizer_tag_version self-test: ok"; fi
  return "$fail"
}

case "${1:-}" in
  --self-test) self_test ;;
  *) [ $# -ge 2 ] || { echo "usage: $0 TAG VERSION_FILE [--dry-run] | --self-test" >&2; exit 2; }
     run "$@" ;;
esac
