#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# cell_run.sh — run ONE sanitizer x filter-set stress cell of the
# shutdown-race matrix against the single Apache rig on :18080. Encapsulates all
# the fragile rig manipulation (module swap, sanitizer LD_PRELOAD, per-cell
# restart script, health gate) behind hardcoded absolute paths so an orchestrator
# only has to call it. Cells MUST run sequentially — they share one port/module.
#
# Usage (run as a user with passwordless sudo):
#   cell_run.sh SAN CONFIG URLFILE DURATION RESTART RELOAD FLUSH CONC CACHEBUST LABEL
#     SAN      asan | ubsan | tsan   (selects module .so + runtime + *SAN_OPTIONS)
#     CONFIG   absolute path to the vhost .conf to activate
#     URLFILE  absolute path to the harness --url-file
# Exit: 0 cell ran (see RESULT line for PASS/FAIL), 3 skipped (module missing).
set -u

SAN="$1"; CONFIG="$2"; URLFILE="$3"
DURATION="${4:-420}"; RESTART="${5:-150}"; RELOAD="${6:-75}"; FLUSH="${7:-40}"
CONC="${8:-24}"; CACHEBUST="${9:-0.25}"; LABEL="${10:-cell}"

PORT=18080
MODDIR=/usr/lib/apache2/modules
HARNESS=/tmp/stress/stress_shutdown.py
ERRLOG=/var/log/apache2/error.log
RTDIR=/usr/lib/llvm-18/lib/clang/18/lib/linux
OUTDIR=/tmp/matrix_results
mkdir -p "$OUTDIR"
CELLLOG="$OUTDIR/${LABEL}.harness.log"

# --- module + sanitizer runtime selection ------------------------------------
case "$SAN" in
  asan)  MODSO=/tmp/mod_asan.so;  RT="$RTDIR/libclang_rt.asan-x86_64.so"
         OPTS="ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:handle_abort=1:handle_segv=1:exitcode=66" ;;
  ubsan) # combined ASan+UBSan module — preload the ASan runtime (it carries the
         # UBSan handlers when built combined); UB is reported as "runtime error:"
         MODSO=/tmp/mod_asan_ubsan.so; RT="$RTDIR/libclang_rt.asan-x86_64.so"
         OPTS="ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:handle_abort=1:handle_segv=1:exitcode=66 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:report_error_type=1" ;;
  tsan)  MODSO=/tmp/mod_tsan.so;  RT="$RTDIR/libclang_rt.tsan-x86_64.so"
         # die_after_fork=0: Apache forks worker children that then spawn the
         # rewrite thread pool; without this TSan kills every child ("new threads
         # after multi-threaded fork is not supported"). NOTE: post-fork TSan
         # tracking is best-effort + apache/APR are uninstrumented, so in-server
         # TSan is a low-confidence probe, not authoritative (see README).
         OPTS="TSAN_OPTIONS=die_after_fork=0:report_atomic_races=0:detect_deadlocks=0:history_size=4:suppressions=/tmp/stress/tsan_suppressions.txt:exitcode=66" ;;
  *) echo "CELL $LABEL: unknown sanitizer '$SAN'"; exit 2 ;;
esac

echo "=================================================================="
echo "CELL $LABEL  san=$SAN config=$(basename "$CONFIG") dur=${DURATION}s"
echo "  chaos: restart=${RESTART}s reload=${RELOAD}s flush=${FLUSH}s conc=$CONC bust=$CACHEBUST"
if [ ! -f "$MODSO" ]; then echo "CELL $LABEL: SKIP — module $MODSO not built"; exit 3; fi
if [ ! -f "$RT" ];    then echo "CELL $LABEL: SKIP — runtime $RT missing";    exit 3; fi
if [ ! -f "$URLFILE" ]; then echo "CELL $LABEL: SKIP — urlfile $URLFILE missing"; exit 3; fi

# --- cache dir from the active config ----------------------------------------
CACHEDIR=$(grep -oP 'ModPagespeedFileCachePath\s+"\K[^"]+' "$CONFIG" | head -1)
CACHEDIR="${CACHEDIR:-/tmp/pgcache_cell}"

# --- write per-cell restart/reload scripts (bake in THIS cell's sanitizer env)-
# Restart kills the process, so it must re-export the right LD_PRELOAD/OPTS.
cat > /tmp/cell_restart.sh <<EOF
#!/bin/bash
set -u
sudo pkill -9 -x apache2 2>/dev/null || true
for i in \$(seq 1 50); do ss -lnt 2>/dev/null | grep -q ":$PORT " || break; sleep 0.2; done
sudo bash -c 'set -a; source /etc/apache2/envvars; export LD_PRELOAD=$RT; export $OPTS; exec apache2 -k start'
for i in \$(seq 1 50); do ss -lnt 2>/dev/null | grep -q ":$PORT " && break; sleep 0.2; done
EOF
cat > /tmp/cell_reload.sh <<'EOF'
#!/bin/bash
sudo apache2 -k graceful
EOF
chmod +x /tmp/cell_restart.sh /tmp/cell_reload.sh

# --- install module + config, clean cache ------------------------------------
sudo cp "$MODSO" "$MODDIR/mod_pagespeed.so"; sudo chmod 644 "$MODDIR/mod_pagespeed.so"
# single active site on :18080 — disable any others, enable this one
for s in /etc/apache2/sites-enabled/*.conf; do [ -e "$s" ] && sudo a2dissite "$(basename "$s")" >/dev/null 2>&1; done
sudo cp "$CONFIG" /etc/apache2/sites-available/zz-cell.conf
sudo a2ensite zz-cell.conf >/dev/null 2>&1
sudo rm -rf "$CACHEDIR"; sudo mkdir -p "$CACHEDIR"; sudo chmod 777 "$CACHEDIR"

# --- start apache under this sanitizer ---------------------------------------
# Truncate the errlog ONCE at cell start so each cell's log (and the end-of-cell
# scan) is bounded to this cell; chaos restarts do NOT truncate (evidence kept).
sudo pkill -9 -x apache2 2>/dev/null || true; sleep 1
sudo truncate -s 0 "$ERRLOG" 2>/dev/null || true
bash /tmp/cell_restart.sh
sleep 2

# pick a real page from the url-file for the health probe
HEALTHPATH=$(grep -m1 '^/pages/' "$URLFILE" || head -1 "$URLFILE")
HCODE=$(curl -s -o /dev/null -w '%{http_code}' "http://localhost:$PORT$HEALTHPATH")
echo "CELL $LABEL: health $HEALTHPATH -> $HCODE  (loaded RT: $(grep -c "$(basename "$RT")" /proc/$(pgrep -x apache2 | head -1)/maps 2>/dev/null || echo '?'))"
if [ "$HCODE" != "200" ]; then echo "CELL $LABEL: WARN apache not healthy pre-run ($HCODE)"; fi

# --- mark errlog, run the harness --------------------------------------------
ERR_START=$(sudo wc -l < "$ERRLOG" 2>/dev/null || echo 0)
echo "CELL $LABEL: starting harness ($(date -u +%H:%M:%S)Z) ..."
sudo python3 -u "$HARNESS" run \
  --server apache \
  --base-url "http://localhost:$PORT" \
  --url-file "$URLFILE" \
  --cache-dir "$CACHEDIR" \
  --reload-cmd /tmp/cell_reload.sh \
  --restart-cmd /tmp/cell_restart.sh \
  --error-log "$ERRLOG" \
  --duration "$DURATION" --concurrency "$CONC" \
  --cache-bust-frac "$CACHEBUST" \
  --flush-interval "$FLUSH" --reload-interval "$RELOAD" --restart-interval "$RESTART" \
  2>&1 | tee "$CELLLOG"
HRC=${PIPESTATUS[0]}

# --- independent errlog sanitizer scan (cell-scoped: log was truncated at start)
# Cross-check, separate from the harness detector. HARD tokens only — strings a
# sanitizer report emits that cannot occur in echoed CSS/HTML/JS source.
ERR_END=$(sudo wc -l < "$ERRLOG" 2>/dev/null || echo 0)
DELTA=$(( ERR_END - ERR_START )); [ "$DELTA" -lt 0 ] && DELTA=0
SAN_HITS=$(sudo grep -icE 'AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|heap-use-after-free|heap-buffer-overflow|stack-use-after|: runtime error:|data race|Segmentation fault' "$ERRLOG" 2>/dev/null || true)
# persist the DEDUPED finding lines (the log is truncated next cell) — UBSan UB
# sites, ASan/TSan reports — as "<count> <line>", most frequent first.
sudo grep -hE 'AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|: runtime error:|data race|heap-use-after-free|heap-buffer-overflow|stack-use-after|SUMMARY: .*Sanitizer|Segmentation fault' "$ERRLOG" 2>/dev/null \
  | sed -E 's/byte [0-9]+/byte N/g; s/0x[0-9a-f]+/0xADDR/g; s/pid [0-9]+/pid N/g' \
  | sort | uniq -c | sort -rn > "$OUTDIR/${LABEL}.findings" 2>/dev/null || true
RESULT=$(grep -E '^RESULT:' "$CELLLOG" | tail -1 | sed 's/^RESULT:[[:space:]]*//')
REQS=$(grep -oiE 'requests=[0-9]+' "$CELLLOG" | tail -1 | grep -oE '[0-9]+' || echo 0)
CRASHN=$(grep -oE 'crash/asan hits=[0-9]+' "$CELLLOG" | tail -1 | grep -oE '[0-9]+' || echo 0)
UBN=$(grep -oE 'ub findings[^=]*=[0-9]+' "$CELLLOG" | tail -1 | grep -oE '[0-9]+' || echo 0)
# machine-readable summary: LABEL RESULT HRC DELTA SAN_HITS REQS CRASHES UB
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$LABEL" "${RESULT:-NA}" "$HRC" "$DELTA" "$SAN_HITS" "${REQS:-0}" "${CRASHN:-0}" "${UBN:-0}" > "$OUTDIR/${LABEL}.summary"
echo "CELL $LABEL: RESULT=${RESULT:-NA}  harness_rc=$HRC  crashes=$CRASHN  ub=$UBN  errlog_san_hits=$SAN_HITS  reqs=$REQS"
echo "CELL_DONE $LABEL"
