#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# sweep-grep-gate.sh -- count the remaining hits of a C++ sweep pattern.
#
# Complements the single-check clang-tidy sweep (.github/workflows/tidy-sweep.yml):
# clang-tidy only sees Linux-compiled TUs, so this grep covers everything,
# including pagespeed/iis, pagespeed/windows and _WIN32 branches.
#
# Usage: tools/ci/sweep-grep-gate.sh [--allowlist FILE] PATTERN [THRESHOLD]
#   PATTERN    Perl regex, e.g. '\bNULL\b' or '^\s*typedef\b'
#   THRESHOLD  maximum allowed hits (default 0); exits 1 when exceeded
#   --allowlist FILE  extra grep -E patterns (one per line) matched against
#                     "path:line:text"; matching hits are ignored
#
# Hits are counted after stripping // and /* */ comments (single-line) and
# string/char literals, so mentions in comments or strings do not count.
# Excluded: third_party/, external/, bazel-*/, net/instaweb/rewriter/generated/
# and any file carrying the "DO NOT EDIT BY HAND" banner.
set -euo pipefail

allowlist=""
if [ "${1:-}" = "--allowlist" ]; then
  allowlist="${2:?--allowlist needs a file}"
  shift 2
fi
pattern="${1:?usage: $0 [--allowlist FILE] PATTERN [THRESHOLD]}"
threshold="${2:-0}"
case "$threshold" in ''|*[!0-9]*) echo "THRESHOLD must be a non-negative integer" >&2; exit 2;; esac

root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$root"

roots=()
for d in pagespeed net test; do [ -d "$d" ] && roots+=("$d"); done
[ "${#roots[@]}" -gt 0 ] || { echo "no source roots found under $root" >&2; exit 2; }

hits="$(mktemp)"
trap 'rm -f "$hits"' EXIT

find "${roots[@]}" -type f \
  \( -name '*.cc' -o -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) \
  -not -path '*/third_party/*' -not -path '*/external/*' -not -path '*/bazel-*' \
  -not -path 'net/instaweb/rewriter/generated/*' -print0 \
  | xargs -0 -r grep -L 'DO NOT EDIT BY HAND' 2>/dev/null \
  | while IFS= read -r f; do
      # Strip string/char literals and comments, then match. The block
      # comment state is tracked across lines.
      perl -ne '
        BEGIN { $pat = shift @ARGV; $in = 0; }
        chomp; $orig = $_; $l = $_;
        if ($in) { if ($l =~ s{^.*?\*/}{}) { $in = 0 } else { next } }
        $l =~ s{"(?:\\.|[^"\\])*"}{""}g;
        $l =~ s{\x27(?:\\.|[^\x27\\])*\x27}{\x27\x27}g;
        $l =~ s{/\*.*?\*/}{}g;
        if ($l =~ s{/\*.*$}{}) { $in = 1 }
        $l =~ s{//.*$}{};
        print "$ARGV:$.:$orig\n" if $l =~ /$pat/;
        close ARGV if eof;
      ' "$pattern" "$f"
    done > "$hits" || true

if [ -n "$allowlist" ] && [ -s "$allowlist" ]; then
  grep -v -E -f "$allowlist" "$hits" > "$hits.f" || true
  mv "$hits.f" "$hits"
fi

total="$(wc -l < "$hits" | tr -d ' ')"
echo "sweep-grep-gate: pattern=${pattern} threshold=${threshold}"
echo "hits per directory (top two path components):"
if [ "$total" -gt 0 ]; then
  cut -d: -f1 "$hits" | awk -F/ '{ print ($2 != "" ? $1 "/" $2 : $1) }' \
    | sort | uniq -c | sort -rn
fi
echo "total: ${total}"
if [ "$total" -gt "$threshold" ]; then
  echo "sweep-grep-gate: ${total} hit(s) exceed threshold ${threshold}; first 20:" >&2
  head -20 "$hits" >&2
  exit 1
fi
