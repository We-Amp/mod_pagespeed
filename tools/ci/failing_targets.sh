#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Failing Bazel targets named in a CI job log (docs/ci/pipelines.md, "The
# nightly-red issue").
#
#   failing_targets.sh --mode test|build [file]
#
# Reads the file, or stdin when none is named: the output of
# `gh run view --log` or `--log-failed` (lines prefixed `<job>\t<step>\t<ISO
# timestamp> `), or a raw job log (lines prefixed `<ISO timestamp> `). ANSI
# escape sequences, as ESC bytes or in the caret notation `gh run view` writes
# when its output is not a terminal (`^[[31m`), the prefix and a trailing CR
# are removed first. Then:
#
#   //<pkg>:<name>  FAILED in ... | FAILED          -> test-failed
#   //<pkg>:<name>  TIMEOUT in ...                  -> test-timeout
#   //<pkg>:<name>  FAILED TO BUILD | NO STATUS     -> build-failed
#   ... Target //<label> failed to build ...        -> build-failed
#
# FLAKY and PASSED lines name nothing. Stdout: sorted unique `<label>\t<kind>`
# lines; `--mode build` prints the build-failed ones only. Exit 0 (empty
# output means nothing was extracted); exit 2 on a usage error.

set -uo pipefail

usage() {
  echo "usage: failing_targets.sh --mode test|build [file]" >&2
  exit 2
}

mode=""
file=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --mode)
      [ "$#" -ge 2 ] || usage
      mode="$2"
      shift 2
      ;;
    -*) usage ;;
    *)
      [ -z "${file}" ] || usage
      file="$1"
      shift
      ;;
  esac
done
case "${mode}" in
  test|build) ;;
  *) usage ;;
esac
if [ -n "${file}" ] && [ ! -r "${file}" ]; then
  echo "failing targets: cannot read ${file}" >&2
  exit 2
fi

LC_ALL=C awk -v mode="${mode}" '
  {
    line = $0
    gsub(/\033\[[0-9;]*[A-Za-z]/, "", line)
    gsub(/\^\[\[[0-9;]*[A-Za-z]/, "", line)
    sub(/\r$/, "", line)
    sub(/^[^\t]*\t[^\t]*\t/, "", line)
    sub(/^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9:.]+Z /, "", line)
    sub(/[ \t]+$/, "", line)

    if (match(line, /Target \/\/[^ \t]+ failed to build/)) {
      print substr(line, RSTART + 7, RLENGTH - 23) "\tbuild-failed"
      next
    }
    if (!match(line, /^\/\/[^ \t]+:[^ \t]+[ \t]+/)) next
    rest = substr(line, RLENGTH + 1)
    label = substr(line, 1, RLENGTH)
    sub(/[ \t]+$/, "", label)
    if (rest ~ /^(FAILED TO BUILD|NO STATUS)/) {
      print label "\tbuild-failed"
    } else if (mode == "test" && rest ~ /^FAILED( in|$)/) {
      print label "\ttest-failed"
    } else if (mode == "test" && rest ~ /^TIMEOUT in/) {
      print label "\ttest-timeout"
    }
  }
' ${file:+"${file}"} | LC_ALL=C sort -u
exit 0
