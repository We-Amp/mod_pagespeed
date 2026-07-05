#!/bin/bash
# Adjudicate ASan / crash evidence after a run. Exit non-zero on any
# hit so CI goes red. Apache under ASan prints its report to stderr -> error.log
# (abort_on_error may also drop a core). error.log is the primary signal; cores
# are best-effort. HARD tokens only — strings a sanitizer report emits that can't
# occur in echoed CSS/HTML/JS source (mirrors cell_run.sh's scan).
set -uo pipefail

RIG_DIR="${RIG_DIR:-${TMPDIR:-/tmp}/apache-asan-rig}"
ERRLOG="${ERRLOG:-/var/log/apache2/error.log}"
HARNESS_RC="${1:-0}"
hits=0

echo "==== harness rc ===="
echo "$HARNESS_RC"
if [[ "$HARNESS_RC" != "0" ]]; then echo "harness reported non-zero -> FAIL"; hits=$((hits+1)); fi

TOKENS='AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|heap-use-after-free|heap-buffer-overflow|stack-use-after|use-after-poison|: runtime error:|data race|Segmentation fault'

echo "==== sanitizer/crash tokens in $ERRLOG ===="
if sudo grep -aE "$TOKENS" "$ERRLOG" 2>/dev/null | tail -30; then
  echo "sanitizer/crash token(s) in error.log -> FAIL"; hits=$((hits+1))
else echo "NONE"; fi

echo "==== core dumps ($RIG_DIR/cores) ===="
shopt -s nullglob
cores=("$RIG_DIR"/cores/*)
if (( ${#cores[@]} )); then
  ls -la "$RIG_DIR"/cores/; echo "core dump(s) present -> FAIL"; hits=$((hits+1))
else echo "NONE"; fi

echo "==== SWEEP DONE: $hits failure signal(s) ===="
[[ $hits -eq 0 ]] || exit 1
