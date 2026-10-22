#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test: every Bazel test invocation in the post-merge (full), nightly and
# release workflows must carry --nocache_test_results, so a pass there is an
# execution and never a cache hit from an earlier (PR) build.
#
# Finds invocations by `bazel test`, a continuation line starting with `test `
# followed by a config/flag/variable, and PowerShell `test @common`; joins
# backslash and backtick continuations; prints FAIL: <file>:<line> per miss.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

files=(pipeline-full.yml pipeline-nightly.yml pipeline-release-module.yml)
fail=0
total=0
for f in "${files[@]}"; do
  path="$ROOT/.github/workflows/$f"
  if [ ! -f "$path" ]; then echo "FAIL: $f missing"; fail=1; continue; fi
  out="$(awk -v F=".github/workflows/$f" '
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
        if (line ~ /bazel[[:space:]]+test[[:space:]]/ ||
            line ~ /^[[:space:]]*test[[:space:]]+(\$\{|--|@)/ ||
            line ~ /[[:space:]]test[[:space:]]+@common([[:space:]]|$)/) {
          start = NR; cmd = line
        } else next
      } else {
        cmd = cmd " " line
      }
      if (line ~ /[\\`][[:space:]]*$/) next
      flush()
    }
    END { flush(); print "COUNT " n+0 " " miss+0 }
  ' "$path")"
  printf '%s\n' "$out" | grep '^FAIL' && fail=1
  c="$(printf '%s\n' "$out" | awk '/^COUNT/{print $2}')"
  echo "$f: $c test invocation(s) found"
  if [ "${c:-0}" -eq 0 ]; then echo "FAIL: $f has no test invocation found (scan broken?)"; fail=1; fi
  total=$((total + ${c:-0}))
done
[ "$fail" -eq 0 ] && echo "ok   all $total invocations carry --nocache_test_results"
exit "$fail"
