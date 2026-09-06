#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# run_matrix.sh — drive the full shutdown-race stress MATRIX on
# the single Apache rig: sanitizer x filter-set x corpus cells, run STRICTLY
# SEQUENTIALLY (they share one port/module/caches). Long + gentle chaos. Cells
# whose sanitizer module wasn't built are auto-skipped by cell_run.sh (exit 3).
# Writes per-cell logs + an aggregate RESULTS.tsv + a DONE sentinel.
#
# Run detached:  nohup bash /tmp/stress/run_matrix.sh > /tmp/matrix.log 2>&1 &
set -u
CELL=/tmp/stress/cell_run.sh
CFG=/tmp/stress/configs
INET=/corpus-internet/urls.txt
MPS=/tmp/corpus_replay_paths.txt
MPSCFG=/etc/apache2/sites-available/docroot-replay.conf
OUT=/tmp/matrix_results
mkdir -p "$OUT"
RES="$OUT/RESULTS.tsv"
rm -f "$OUT"/*.harness.log "$OUT"/*.summary "$RES" /tmp/matrix.done
printf "label\tsan\tconfig\tduration\texit\tresult\tcrashes\tub_findings\terrlog_san_hits\trequests\n" > "$RES"

# gentle chaos (shared): restart 150s, reload 75s, flush 40s, conc 24, bust 0.25
G_RESTART=150; G_RELOAD=75; G_FLUSH=40; G_CONC=24; G_BUST=0.25

# cell table: LABEL SAN CONFIG URLFILE DURATION
# breadth first (fast signal), depth last.
CELLS=(
  "asan-core            asan  $CFG/internet-core.conf  $INET  420"
  "asan-all             asan  $CFG/internet-all.conf   $INET  420"
  "ubsan-all            ubsan $CFG/internet-all.conf   $INET  420"
  "ubsan-core           ubsan $CFG/internet-core.conf  $INET  420"
  "tsan-core            tsan  $CFG/internet-core.conf  $INET  300"
  "asan-mps-continuity  asan  $MPSCFG                   $MPS   300"
  "asan-all-depth       asan  $CFG/internet-all.conf   $INET  1800"
)

echo "########## MATRIX START $(date -u) ##########"
echo "corpus(internet) urls: $(wc -l < "$INET" 2>/dev/null || echo MISSING)"
echo "modules: asan=$(ls -la /tmp/mod_asan.so 2>/dev/null|awk '{print $5}') ubsan=$(ls -la /tmp/mod_asan_ubsan.so 2>/dev/null|awk '{print $5}') tsan=$(ls -la /tmp/mod_tsan.so 2>/dev/null|awk '{print $5}')"

for row in "${CELLS[@]}"; do
  read -r LABEL SAN CONFIG URLFILE DUR <<<"$row"
  echo; echo "########## CELL $LABEL ($(date -u +%H:%M:%S)Z) ##########"
  bash "$CELL" "$SAN" "$CONFIG" "$URLFILE" "$DUR" "$G_RESTART" "$G_RELOAD" "$G_FLUSH" "$G_CONC" "$G_BUST" "$LABEL"
  RC=$?
  if [ "$RC" -eq 3 ]; then
    echo "CELL $LABEL SKIPPED (module missing)"
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$LABEL" "$SAN" "$(basename "$CONFIG")" "$DUR" "$RC" "SKIP" "NA" "NA" "NA" "0" >> "$RES"
    continue
  fi
  # consume the cell's machine-readable summary: LABEL RESULT HRC DELTA SAN_HITS REQS CRASHES UB
  SUM="$OUT/${LABEL}.summary"
  if [ -f "$SUM" ]; then
    IFS=$'\t' read -r _l RESULT _hrc _delta HITS REQS CRASHN UBN < "$SUM"
  else
    RESULT=NA; HITS=NA; REQS=0; CRASHN=NA; UBN=NA
  fi
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$LABEL" "$SAN" "$(basename "$CONFIG")" "$DUR" "$RC" "${RESULT:-NA}" "${CRASHN:-NA}" "${UBN:-NA}" "${HITS:-NA}" "${REQS:-0}" >> "$RES"
done

echo; echo "########## MATRIX COMPLETE $(date -u) ##########"
echo "=== RESULTS.tsv ==="; cat "$RES"
touch /tmp/matrix.done
