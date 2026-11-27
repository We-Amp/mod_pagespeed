#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Verify CI artifacts match the expected commit.
#
# Exits non-zero with "STALE ARTIFACT: ..." on the first mismatch so the
# calling job fails before running tests against an out-of-date binary.
#
# Usage:
#   verify-artifacts.sh <expected-sha> <spec>...
#
# Each <spec> takes one of these forms:
#   stamp:<file>                Plain-text GIT_COMMIT file; contents must
#                               match <expected-sha> exactly.
#   binary:<path>               Runs "<path> --version", extracts the first
#                               hex commit in the output, and compares it
#                               against <expected-sha> using a common-prefix
#                               match (so short SHAs like 7-char are OK).
#   sharedlib:<path>            Loads a libpagespeed shared library via
#                               ctypes and calls ps_git_commit(); compares
#                               the result against <expected-sha> with the
#                               same prefix match as binary. Requires a
#                               matching-RID runner (cannot dlopen a foreign
#                               ABI binary) and python3 on PATH.
#   container:<name>:<path>     Runs "docker exec <name> cat <path>" and
#                               compares the output against <expected-sha>.
#
# Example:
#   verify-artifacts.sh "$GITHUB_SHA" \
#     "stamp:$WORKSPACE_DIR/GIT_COMMIT" \
#     "binary:$WORKSPACE_DIR/.ci-artifacts/factory_worker" \
#     "sharedlib:$WORKSPACE_DIR/.ci-artifacts/libpagespeed.so" \
#     "container:worker:/workspace/GIT_COMMIT"

set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "Usage: $0 <expected-sha> <spec>..." >&2
  echo "  spec: stamp:<file> | binary:<path> | sharedlib:<path> |" >&2
  echo "        container:<name>:<path>" >&2
  exit 2
fi

EXPECTED="$1"
shift

fail() {
  echo "::error::STALE ARTIFACT: $1" >&2
  exit 1
}

check_stamp() {
  local file="$1"
  [ -f "$file" ] || fail "missing stamp file: $file"
  local actual
  actual=$(tr -d '[:space:]' < "$file")
  [ "$actual" = "$EXPECTED" ] || \
    fail "expected $EXPECTED, got $actual in $file"
  echo "verify-artifacts: stamp $file = $actual"
}

check_binary() {
  local bin="$1"
  [ -x "$bin" ] || fail "binary not executable: $bin"
  local out version
  out=$("$bin" --version 2>&1) || fail "$bin --version failed: $out"
  version=$(printf '%s\n' "$out" | grep -oE '[0-9a-f]{7,40}' | head -n 1 || true)
  [ -n "$version" ] || fail "$bin --version printed no commit sha: $out"
  local minlen=${#version}
  [ ${#EXPECTED} -lt "$minlen" ] && minlen=${#EXPECTED}
  [ "${EXPECTED:0:$minlen}" = "${version:0:$minlen}" ] || \
    fail "$bin reports $version, expected prefix of $EXPECTED"
  echo "verify-artifacts: binary $bin = $version"
}

check_container() {
  local container="$1" path="$2"
  local actual
  actual=$(docker exec "$container" cat "$path" 2>/dev/null | tr -d '[:space:]') || \
    fail "docker exec $container cat $path failed"
  [ -n "$actual" ] || fail "empty stamp in $container:$path"
  [ "$actual" = "$EXPECTED" ] || \
    fail "container $container:$path reports $actual, expected $EXPECTED"
  echo "verify-artifacts: container $container:$path = $actual"
}

check_sharedlib() {
  local lib="$1"
  [ -f "$lib" ] || fail "shared library not found: $lib"
  command -v python3 >/dev/null 2>&1 || fail "python3 required for sharedlib: spec"
  local version
  # dlopen the lib and call ps_git_commit(); it returns the short
  # build-commit string (e.g. "9e53e63") that gen_build_commit stamps.
  # Any load/lookup error surfaces as a non-zero exit from python3.
  version=$(python3 -c '
import ctypes, os, sys
path = os.path.abspath(sys.argv[1])
if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
    os.add_dll_directory(os.path.dirname(path))
lib = ctypes.CDLL(path)
lib.ps_git_commit.restype = ctypes.c_char_p
s = lib.ps_git_commit()
sys.stdout.write(s.decode("ascii") if s else "")
' "$lib" 2>&1) || fail "$lib ps_git_commit() failed: $version"
  printf '%s' "$version" | grep -qE '^[0-9a-f]{7,40}$' || \
    fail "$lib ps_git_commit() returned non-hex value: $version"
  local minlen=${#version}
  [ ${#EXPECTED} -lt "$minlen" ] && minlen=${#EXPECTED}
  [ "${EXPECTED:0:$minlen}" = "${version:0:$minlen}" ] || \
    fail "$lib reports $version, expected prefix of $EXPECTED"
  echo "verify-artifacts: sharedlib $lib = $version"
}

for spec in "$@"; do
  case "$spec" in
    stamp:*)
      check_stamp "${spec#stamp:}"
      ;;
    binary:*)
      check_binary "${spec#binary:}"
      ;;
    sharedlib:*)
      check_sharedlib "${spec#sharedlib:}"
      ;;
    container:*)
      rest="${spec#container:}"
      container="${rest%%:*}"
      path="${rest#*:}"
      if [ "$container" = "$path" ] || [ -z "$container" ] || [ -z "$path" ]; then
        echo "Malformed container spec: $spec (expected container:<name>:<path>)" >&2
        exit 2
      fi
      check_container "$container" "$path"
      ;;
    *)
      echo "Unknown spec: $spec" >&2
      echo "  expected stamp:<file> | binary:<path> | sharedlib:<path> |" >&2
      echo "           container:<name>:<path>" >&2
      exit 2
      ;;
  esac
done
