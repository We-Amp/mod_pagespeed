#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Name checks for the release attestation subjects (docs/ci/release.md).
#   check_attest_subjects.sh name <name>
#       <name> is one plain file name: a single line, starting with a letter
#       or digit, then letters, digits, . _ + - only, with no "..".
#   check_attest_subjects.sh sbom <sbom.txt> <all.txt>
#       <sbom.txt> holds exactly one line, a valid name ending in
#       ".spdx.json", that is also a line of <all.txt> (the attested subjects).
# Exit 0 when the input is accepted, 1 with an ::error:: line when not.
set -uo pipefail

attest_name_ok() {
  local n="$1"
  # [[ =~ ]] sees the whole string, so a name with an embedded newline cannot
  # pass on the strength of one clean line, as it could with grep -E.
  [[ "$n" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ ]] || return 1
  case "$n" in *..*) return 1 ;; esac
  return 0
}

attest_sbom_ok() {
  local file="$1" all="$2" lines name
  [ -f "$file" ] && [ -f "$all" ] || return 1
  lines="$(wc -l < "$file" | tr -d ' ')"
  [ "$lines" = 1 ] || return 1
  name="$(cat "$file")"
  attest_name_ok "$name" || return 1
  case "$name" in *.spdx.json) ;; *) return 1 ;; esac
  grep -Fxq -- "$name" "$all"
}

main() {
  case "${1:-}" in
    name) attest_name_ok "${2-}" || { echo "::error::unexpected subject name '${2-}'"; return 1; } ;;
    sbom) attest_sbom_ok "${2-}" "${3-}" || { echo "::error::sbom.txt is not one valid SPDX subject listed in all.txt"; return 1; } ;;
    *) echo "usage: $0 name <name> | sbom <sbom.txt> <all.txt>" >&2; return 2 ;;
  esac
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then main "$@"; fi
