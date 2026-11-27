#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test: every Bazel test invocation in the post-merge (full), nightly and
# NuGet release workflows must carry --nocache_test_results, so a pass there is an
# execution and never a cache hit from an earlier (PR) build.
#
# Finds invocations by `bazel test`, a `test` word followed by a flag, a
# variable or `@args` (continuation lines, bazelisk @startup test); joins
# backslash and backtick continuations; prints FAIL: <file>:<line> per miss.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

files=(pipeline-full.yml pipeline-nightly.yml pipeline-release-nuget.yml)
fail=0
total=0
scan() { # scan <path> <label>: prints FAIL lines and a COUNT line
  awk -v F="$2" '
    function flush() {
      if (cmd != "") {
        n++
        if (cmd !~ /--nocache_test_results/) { print "FAIL: " F ":" start; miss++ }
        cmd = ""
      }
    }
    {
      line = $0
      if (cmd == "") {
        low = tolower(line)
        if (low ~ /bazel[a-z_]*"?\}?([[:space:]]+("[^"]*"|[-@][^[:space:]]*))*[[:space:]]+test([[:space:]]|$)/ ||
            low ~ /^[[:space:]]*test[[:space:]]+(\$[A-Za-z_{]|"\$\{|@|--|\/\/)/) {
          start = NR; cmd = line
        } else next
      } else {
        cmd = cmd " " line
      }
      if (line ~ /[\\`][[:space:]]*$/) next
      flush()
    }
    END { flush(); print "COUNT " n+0 " " miss+0 }
  ' "$1"
}

if [ "${1:-}" = "--selftest" ]; then
  # Negative fixture: invocations lacking the flag must be reported, other
  # bazel commands and flagged invocations must not.
  fx="$(mktemp)"; trap 'rm -f "$fx"' EXIT
  cat > "$fx" <<'FIX'
run: |
  "$BAZEL_BIN" --output_user_root=/x test //a:b
  & $bazel test //a:c
  bazelisk test //a:d
  bazel --output_user_root=/y \
    test "${FLAGS[@]}" //a:e
  bazel build //a:f
  bazel info output_base
  bazel --output_user_root=/z test --nocache_test_results //a:ok
FIX
  out="$(scan "$fx" fixture)"
  printf '%s\n' "$out"
  want="$(printf 'FAIL: fixture:2\nFAIL: fixture:3\nFAIL: fixture:4\nFAIL: fixture:6\nCOUNT 5 4')"
  if [ "$out" = "$want" ]; then echo "ok   negative fixture reported as expected"; exit 0; fi
  echo "FAIL: negative fixture not reported as expected"; exit 1
fi

for f in "${files[@]}"; do
  path="$ROOT/.github/workflows/$f"
  if [ ! -f "$path" ]; then echo "FAIL: $f missing"; fail=1; continue; fi
  out="$(scan "$path" ".github/workflows/$f")"
  printf '%s\n' "$out" | grep '^FAIL' && fail=1
  c="$(printf '%s\n' "$out" | awk '/^COUNT/{print $2}')"
  echo "$f: $c test invocation(s) found"
  if [ "${c:-0}" -eq 0 ]; then echo "FAIL: $f has no test invocation found (scan broken?)"; fail=1; fi
  total=$((total + ${c:-0}))
done
[ "$fail" -eq 0 ] && echo "ok   all $total invocations carry --nocache_test_results"
exit "$fail"
