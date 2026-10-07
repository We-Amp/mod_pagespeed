#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# collect-systest-diagnostics.sh - Failure diagnostics for a system-test lane.
#
# Usage:
#   collect-systest-diagnostics.sh apache|nginx <test-exit-status> <out-dir>
#   collect-systest-diagnostics.sh --self-test
#
# Runs inside the lane's container right after the test run, before the
# container (docker run --rm) and its server log are gone.
#
# - Always prints the crash/fatal lines of the server's error log into the
#   job log, so the answer is on the job page even without the artifact.
# - Only when <test-exit-status> is non-zero: copies an allow-list of files
#   into a private staging directory in the container's own temp space and
#   packs it as ONE tarball,
#     <out-dir>/systest-diagnostics-<SYSTEST_DIAG_TAG>.tar.gz
#   The workflow sets SYSTEST_DIAG_TAG per job, run and attempt and uploads
#   only the file carrying its own tag, so a tarball left behind by another
#   job can never be uploaded as this job's evidence. The tarball is the only
#   thing uploaded, so no collected file name can be refused by the upload
#   step (the nginx rig, for example, holds files with ':' in their names).
#
# Allow-list -- nothing else is collected. In particular the rig's certs/
# directory (the per-run test TLS private key) is never read:
#   apache: the Apache error log; the lane fixture servers' logs
#           (flush_origin.log, pathological_server.log) and the lane_fixtures
#           list from the lane state directory
#   nginx:  the rig's logs/error.log
#   both:   a ps snapshot of the server's processes, the oom_kill line of
#           /proc/vmstat, and /proc/pressure/{cpu,io,memory}
#
# Never changes the lane's result: every step is best effort, nothing is
# signalled or stopped, and the script always exits 0.
#
# Overrides (used by --self-test): DIAG_APACHE_ERROR_LOG, DIAG_APACHE_STATE,
# DIAG_NGINX_RIG, DIAG_PROC, DIAG_TMP.

set -u

# Crash/fatal lines worth showing on the job page.
#  apache: httpd's child-death reports (AH00050-AH00052, "exit signal") and
#          the module's fatal lines (base/logging.h [FATAL] and CHECK).
#  nginx:  a worker that died on a signal or cannot be respawned, [emerg],
#          and the module's fatal lines. Not every [alert]: nginx logs
#          "open socket ... left in connection" and "aborting" alerts on
#          every ordinary shutdown.
APACHE_CRASH_RE='AH0005[0-2]|exit signal|\[FATAL\]|Check failed'
NGINX_CRASH_RE='exited on signal|exited with fatal code|\[emerg\]|\[FATAL\]|Check failed'

# copy_in <source> <stage> <dest-name>: plain read first, then sudo -n (the
# Apache log is root-owned). Leaves no file behind when neither works.
copy_in() {
  local src="$1" dest="$2/$3"
  if cat "$src" > "$dest" 2>/dev/null; then
    return 0
  fi
  # The redirect is meant to run as the invoking user: only the read needs
  # root, and the staged copy must stay readable without it.
  # shellcheck disable=SC2024
  if sudo -n cat "$src" > "$dest" 2>/dev/null; then
    return 0
  fi
  rm -f "$dest" 2>/dev/null
  return 1
}

collect() {
  local server="$1" rc="$2" out="$3"
  local tag stage name crash_re error_log proc_name p
  # Read at call time so --self-test can point them at a fake tree.
  local APACHE_ERROR_LOG="${DIAG_APACHE_ERROR_LOG:-/var/log/apache2/error.log}"
  local APACHE_STATE="${DIAG_APACHE_STATE:-/tmp/apache_pagespeed_test}"
  local NGINX_RIG="${DIAG_NGINX_RIG:-/tmp/nginx_pagespeed_test}"
  local PROC="${DIAG_PROC:-/proc}"
  local TMP_ROOT="${DIAG_TMP:-${TMPDIR:-/tmp}}"

  case "$server" in
    apache)
      error_log="$APACHE_ERROR_LOG"; crash_re="$APACHE_CRASH_RE"
      proc_name=apache2 ;;
    nginx)
      error_log="$NGINX_RIG/logs/error.log"; crash_re="$NGINX_CRASH_RE"
      proc_name=nginx ;;
    *)
      echo "collect-systest-diagnostics: unknown server '$server'" >&2
      return 0 ;;
  esac

  stage="$(mktemp -d "$TMP_ROOT/systest-diag.XXXXXX" 2>/dev/null)" || {
    echo "collect-systest-diagnostics: cannot create a staging directory" >&2
    return 0
  }

  echo "=== crash/fatal lines in the $server error log (if any) ==="
  if copy_in "$error_log" "$stage" "$server-error.log"; then
    grep -E "$crash_re" "$stage/$server-error.log" || echo "(none)"
  else
    echo "(error log not readable: $error_log)"
  fi

  if [ "$rc" = "0" ]; then
    rm -rf "$stage"
    return 0
  fi

  # Allow-list, server-specific part (the error log is already staged).
  if [ "$server" = "apache" ]; then
    for name in flush_origin.log pathological_server.log lane_fixtures; do
      copy_in "$APACHE_STATE/$name" "$stage" "fixture-$name" || true
    done
  fi

  # Allow-list, common part.
  ps -o pid,ppid,etimes,stat,rss,nlwp,args -C "$proc_name" \
    > "$stage/$server-ps.txt" 2>&1 || true
  grep oom_kill "$PROC/vmstat" > "$stage/oom_kill.txt" 2>&1 || true
  for p in cpu io memory; do
    copy_in "$PROC/pressure/$p" "$stage" "pressure-$p.txt" || true
  done

  tag="$(printf '%s' "${SYSTEST_DIAG_TAG:-local}" | tr -c 'A-Za-z0-9._-' '_')"
  name="systest-diagnostics-$tag.tar.gz"
  if mkdir -p "$out" 2>/dev/null \
      && tar -czf "$out/.$name.partial" -C "$stage" . 2>/dev/null \
      && mv -f "$out/.$name.partial" "$out/$name" 2>/dev/null; then
    chmod a+r "$out/$name" 2>/dev/null || true
    echo "Diagnostics: $out/$name"
  else
    rm -f "$out/.$name.partial" 2>/dev/null
    echo "collect-systest-diagnostics: could not write $out/$name" >&2
  fi
  rm -rf "$stage"
  return 0
}

self_test() {
  local root fails=0 out listing printed
  root="$(mktemp -d "${TMPDIR:-/tmp}/collect-diag-selftest.XXXXXX")" || return 1

  check() {  # check <description> <command...>
    local what="$1"; shift
    if "$@"; then echo "ok   - $what"; else echo "FAIL - $what"; fails=$((fails + 1)); fi
  }
  # shellcheck disable=SC2329  # invoked through check()
  lacks() {  # lacks <text> <grep -e pattern...>: true when no pattern matches
    local text="$1"; shift
    ! grep -q "$@" <<<"$text"
  }
  # shellcheck disable=SC2329  # invoked through check()
  file_is() {  # file_is <path> <content>
    [ "$(cat "$1")" = "$2" ]
  }

  # Fake server state.
  mkdir -p "$root/apache-state/certs" "$root/nginx/logs" "$root/nginx/certs" \
           "$root/proc/pressure" "$root/tmp"
  printf '%s\n' \
    '[Sun Oct 05 08:00:00.1 2026] [mpm_prefork:notice] [pid 1] AH00163: resuming normal operations' \
    '[Sun Oct 05 08:01:00.1 2026] [core:notice] [pid 1] AH00052: child pid 7 exit signal Segmentation fault (11)' \
    > "$root/apache-error.log"
  echo "fixture log" > "$root/apache-state/flush_origin.log"
  echo "cache_flush,stats_log" > "$root/apache-state/lane_fixtures"
  echo "PRIVATE KEY" > "$root/apache-state/certs/server.key"
  printf '%s\n' \
    '2026/10/05 12:39:29 [alert] 57#57: *263 open socket #19 left in connection 9' \
    '2026/10/05 12:39:29 [alert] 57#57: aborting' \
    '2026/10/05 12:39:30 [alert] 1#1: worker process 12 exited on signal 11' \
    > "$root/nginx/logs/error.log"
  echo "stats" > "$root/nginx/logs/stats_log_dummy_hostname:-1"
  echo "PRIVATE KEY" > "$root/nginx/certs/server.key"
  echo "oom_kill 0" > "$root/proc/vmstat"
  for p in cpu io memory; do echo "some avg10=0.00" > "$root/proc/pressure/$p"; done

  # A previous job's leftovers in the shared output directory.
  out="$root/workspace/_systest-logs"
  mkdir -p "$out/old-copy/nginx_pagespeed_test"
  echo stale > "$out/systest-diagnostics-previous-job.tar.gz"

  export DIAG_APACHE_ERROR_LOG="$root/apache-error.log"
  export DIAG_APACHE_STATE="$root/apache-state"
  export DIAG_NGINX_RIG="$root/nginx"
  export DIAG_PROC="$root/proc"
  export DIAG_TMP="$root/tmp"

  # 1. Passing run: crash lines printed, nothing collected.
  printed="$(SYSTEST_DIAG_TAG=apache-1-1 collect apache 0 "$out")"
  check "pass: crash line printed" grep -q "AH00052" <<<"$printed"
  check "pass: no tarball written" test ! -e "$out/systest-diagnostics-apache-1-1.tar.gz"

  # 2. Failing Apache run: one tarball, allow-list only.
  SYSTEST_DIAG_TAG=apache-1-1 collect apache 3 "$out" > /dev/null
  check "apache fail: tarball written under its own tag" \
    test -s "$out/systest-diagnostics-apache-1-1.tar.gz"
  listing="$(tar -tzf "$out/systest-diagnostics-apache-1-1.tar.gz" | sed 's|^\./||' | grep -v '^$' | sort)"
  check "apache fail: error log collected" grep -qx "apache-error.log" <<<"$listing"
  check "apache fail: fixture log collected" grep -qx "fixture-flush_origin.log" <<<"$listing"
  check "apache fail: no key material" lacks "$listing" -e '\.key$' -e 'certs'
  check "apache fail: previous job's tarball untouched, not merged" \
    file_is "$out/systest-diagnostics-previous-job.tar.gz" stale

  # 3. Failing nginx run: colon-named rig file and key not collected; shutdown
  #    alerts not reported, the crash alert is.
  printed="$(SYSTEST_DIAG_TAG='nginx:1/2' collect nginx 1 "$out")"
  check "nginx fail: crash alert printed" grep -q "exited on signal 11" <<<"$printed"
  check "nginx fail: shutdown alerts not printed" \
    lacks "$printed" -e 'open socket' -e 'aborting'
  check "nginx fail: unsafe tag characters replaced" \
    test -s "$out/systest-diagnostics-nginx_1_2.tar.gz"
  listing="$(tar -tzf "$out/systest-diagnostics-nginx_1_2.tar.gz" | sed 's|^\./||' | grep -v '^$' | sort)"
  check "nginx fail: error log collected" grep -qx "nginx-error.log" <<<"$listing"
  check "nginx fail: no ':' in any collected name" lacks "$listing" -e ':'
  check "nginx fail: no key material" lacks "$listing" -e '\.key$' -e 'certs'

  # 4. Unreadable error log: reported, still exit 0.
  DIAG_APACHE_ERROR_LOG="$root/missing.log" collect apache 1 "$out" > "$root/p4" 2>&1
  check "missing log: returns 0" test $? -eq 0
  check "missing log: reported" grep -q "error log not readable" "$root/p4"

  rm -rf "$root"
  if [ "$fails" -ne 0 ]; then
    echo "self-test: $fails failure(s)"
    return 1
  fi
  echo "self-test: all passed"
  return 0
}

case "${1:-}" in
  --self-test)
    self_test
    exit $? ;;
  apache|nginx)
    if [ "$#" -ne 3 ]; then
      echo "usage: $0 apache|nginx <test-exit-status> <out-dir>" >&2
      exit 0
    fi
    collect "$1" "$2" "$3"
    exit 0 ;;
  *)
    echo "usage: $0 apache|nginx <test-exit-status> <out-dir> | --self-test" >&2
    exit 0 ;;
esac
