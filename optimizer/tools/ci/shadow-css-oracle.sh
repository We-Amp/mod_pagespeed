#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# shadow-css-oracle.sh — full-shadow comparison for the glue-normalization
# cutover (#1238): build the PRE-cutover token-oracle comparator from one
# git revision and the POST-cutover comparator from a source tree, replay
# every input unit in the given dirs through BOTH binaries, and require
# 100% per-input verdict agreement.
#
# The cutover replaces the N hand-grown edge rules in
# lib/css/css_token_stream.h with Normalize(x) = Phase2(Phase1(x)) plus a
# glue-free extractor. The spike (css-triage-1238 glue-normalization
# report) made "one nightly in shadow before cutover" the adoption gate:
# the old and new comparators must return the SAME verdict (clean vs
# oracle-trip) on every real evidence unit — fuzz corpus, crash
# artifacts, minimized repros, embedded seeds — before the new machinery
# may replace the old. This script is that gate, runnable both locally
# and from the CSS-fuzz lane's shadow job.
#
# Usage:
#   tools/ci/shadow-css-oracle.sh SRC_DIR OLD_REV INPUT_DIR [INPUT_DIR...]
#
#   SRC_DIR   post-cutover source tree (the PR checkout)
#   OLD_REV   git rev resolvable in SRC_DIR holding the baseline
#             comparator (the scheduled job pins the last RATIFIED
#             comparator rev; currently 335c9f2d, the first commit of
#             this repository's history with the comparator. Bump it
#             DELIBERATELY in the same PR as any intentional comparator
#             evolution, or the lane hard-fails on every intentional
#             divergence). May also be a directory holding an
#             already-extracted old source tree (useful where SRC_DIR
#             is not a working git repo, e.g. an rsynced scratch copy).
#   INPUT_DIR dirs walked recursively for input units (fuzz corpus, crash
#             artifacts, dumped seeds); every regular file is one input
#
# Verdicts are compared as booleans (rc==0 "clean" vs rc!=0 "trip") — the
# exact failure code differs by oracle (strict vs token vs sanitizer) and
# is not part of the contract. Exit: 0 = 100% agreement, 1 = at least one
# disagreement, 2 = usage error or zero inputs compared.
#
# Build wiring mirrors the fuzz workflow: direct clang++ compile of the
# standalone harness (NOT bazel — see the css-fuzz.yml header). CXX is
# overridable; default clang++-20 as on the CI runners.

set -uo pipefail

if [ "$#" -lt 3 ]; then
  echo "usage: $0 SRC_DIR OLD_REV INPUT_DIR [INPUT_DIR...]" >&2
  exit 2
fi

SRC_DIR="$1"
OLD_REV="$2"
shift 2

CXX="${CXX:-clang++-20}"
CXXFLAGS="-std=c++17 -O1 -fsanitize=address,undefined"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/css-shadow.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# --- Extract + build the OLD comparator ----------------------------------
# The old harness is self-contained given these four files at their
# repo-relative paths (its only project includes are lib/css/css_minify.h
# and lib/css/css_token_stream.h). OLD_REV may also be a directory holding
# an already-extracted old source tree (useful where SRC_DIR is not a
# working git repo, e.g. an rsynced scratch copy).
OLD_SRC="$WORK/old-src"
if [ -d "$OLD_REV" ]; then
  OLD_SRC="$OLD_REV"
else
  mkdir -p "$OLD_SRC/lib/css"
  for f in css_minify.h css_minify.cc css_token_stream.h css_minify_fuzz.cc \
           css_phases.h; do
    if ! git -C "$SRC_DIR" show "$OLD_REV:lib/css/$f" > "$OLD_SRC/lib/css/$f" 2>/dev/null; then
      rm -f "$OLD_SRC/lib/css/$f"
      # css_phases.h exists only post-cutover: tolerate its absence so a
      # pre-cutover OLD_REV builds, and a mis-pinned POST-cutover rev
      # degrades to a vacuous (self-vs-self) comparison instead of a
      # build error (run 30913256122's mechanical failure).
      if [ "$f" != "css_phases.h" ]; then
        echo "shadow-css-oracle: cannot extract $OLD_REV:lib/css/$f" >&2
        exit 2
      fi
    fi
  done
fi

mkdir -p "$WORK/bin"
echo "shadow-css-oracle: building OLD comparator from $OLD_REV"
"$CXX" $CXXFLAGS -I"$OLD_SRC" \
  "$OLD_SRC/lib/css/css_minify_fuzz.cc" "$OLD_SRC/lib/css/css_minify.cc" \
  -o "$WORK/bin/replay-old" || exit 2

echo "shadow-css-oracle: building NEW comparator from $SRC_DIR"
"$CXX" $CXXFLAGS -I"$SRC_DIR" \
  "$SRC_DIR/lib/css/css_minify_fuzz.cc" "$SRC_DIR/lib/css/css_minify.cc" \
  -o "$WORK/bin/replay-new" || exit 2

# --- Replay every input through both, compare verdicts --------------------
n=0
agree_clean=0
agree_trip=0
disagree=0

while IFS= read -r -d '' f; do
  n=$((n + 1))
  timeout 60 "$WORK/bin/replay-old" "$f" > /dev/null 2>&1
  rc_old=$?
  timeout 60 "$WORK/bin/replay-new" "$f" > /dev/null 2>&1
  rc_new=$?
  if [ "$rc_old" -eq 0 ] && [ "$rc_new" -eq 0 ]; then
    agree_clean=$((agree_clean + 1))
  elif [ "$rc_old" -ne 0 ] && [ "$rc_new" -ne 0 ]; then
    agree_trip=$((agree_trip + 1))
  else
    disagree=$((disagree + 1))
    echo "DISAGREE $f: old rc=$rc_old new rc=$rc_new"
  fi
done < <(find "$@" -type f -print0 | sort -z)

echo "shadow-css-oracle: compared $n inputs vs $OLD_REV:" \
  "$agree_clean both-clean, $agree_trip both-trip, $disagree disagreements"

if [ "$n" -eq 0 ]; then
  echo "shadow-css-oracle: zero inputs — no evidence, refusing to pass" >&2
  exit 2
fi
[ "$disagree" -eq 0 ]
