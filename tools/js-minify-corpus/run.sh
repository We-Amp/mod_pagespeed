#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# End-to-end corpus stress run for the tokenizer-based JS minifier.
#
#   run.sh [--no-fetch] [--limit N] [--gate]
#
# Steps: build the probe, fetch the pinned real-world corpus + build the
# bundle fixtures (network, skipped with --no-fetch), regenerate the
# synthetic corpus (deterministic), run the oracle harness, write reports
# under .corpus/reports/<timestamp>/.
# Nothing under .corpus/ is committed.
# --gate makes the oracle run exit non-zero on any correctness failure
# (parse/semantic/idempotence/crash); coverage stats never gate.
#
# Extra bazel flags for the probe build (e.g. "--config=ci --disk_cache=/cache"
# in CI) can be passed via the BAZEL_BUILD_OPTIONS environment variable,
# matching the convention used by tools/gen_compilation_database.py.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

DO_FETCH=1
LIMIT=0
GATE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --no-fetch) DO_FETCH=0 ;;
    --limit) shift; LIMIT="$1" ;;
    --gate) GATE=1 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
  shift
done

CORPUS="$HERE/.corpus"
REAL="$CORPUS/real"
SYNTH="$CORPUS/synthetic"
STAMP="$(date +%Y%m%d-%H%M%S)"
REPORT="$CORPUS/reports/$STAMP"

echo "== build probe"
# Word-splitting of BAZEL_BUILD_OPTIONS is intended (flag list).
# shellcheck disable=SC2086
bazel build ${BAZEL_BUILD_OPTIONS:-} //tools/js-minify-corpus:js_minify_probe
PROBE="$ROOT/bazel-bin/tools/js-minify-corpus/js_minify_probe"

if [ "$DO_FETCH" -eq 1 ]; then
  echo "== fetch real-world corpus"
  python3 "$HERE/fetch_corpus.py" fetch
  echo "== build bundle fixtures (pinned webpack/rollup/esbuild)"
  # Network-dependent like the fetch above; a failure here (e.g. registry
  # unreachable) skips the bundle slice instead of killing the whole run —
  # any previously built .corpus/bundles/ is still included below.
  python3 "$HERE/build_bundles.py" build \
    || echo "warning: bundle fixture build failed; continuing without it" >&2
fi

echo "== generate synthetic corpus"
python3 "$HERE/generate_synthetic.py" --dest "$SYNTH"

echo "== oracle run"
CORPORA=()
[ -d "$REAL" ] && CORPORA+=("$REAL")
[ -d "$CORPUS/bundles" ] && CORPORA+=("$CORPUS/bundles")
CORPORA+=("$SYNTH")
ARGS=(--probe "$PROBE" --report "$REPORT" --corpus "${CORPORA[@]}")
if [ "$LIMIT" -gt 0 ]; then
  ARGS+=(--limit "$LIMIT")
fi
if [ "$GATE" -eq 1 ]; then
  ARGS+=(--gate)
fi
python3 "$HERE/run_oracles.py" "${ARGS[@]}"

ln -sfn "$STAMP" "$CORPUS/reports/latest"
echo "== done: $REPORT/SUMMARY.md"
