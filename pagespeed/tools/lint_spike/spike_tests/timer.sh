set -u
O=~/code/lint-t0-out
P=$O/timer_probe
SRC=~/code/lint-t0
W=$O/timer; rm -rf $W; mkdir -p $W/in
# 20 corpus files: the example pages first, then test pages, sorted.
( cd $SRC/install && find mod_pagespeed_example mod_pagespeed_test -maxdepth 2 -name "*.html" -type f | sort | head -20 ) > $W/list.txt
i=0
while read f; do i=$((i+1)); cp "$SRC/install/$f" "$W/in/$(printf %02d $i).html"; done < $W/list.txt
FLAGS="--rewrite_level=CoreFilters --rewriters=collapse_whitespace,inline_import_to_link,outline_css,rewrite_css"
for mode in real mock mockS hung-real hung-mock hung-mockS; do
  for run in 1 2; do
    mkdir -p $W/out-$mode-$run
    hangs=0; fails=0; t0=$(date +%s.%N)
    for f in $(ls $W/in); do
      s=$(date +%s.%N)
      timeout 15 unshare -rn $P $mode $FLAGS $W/in $W/out-$mode-$run $f >/dev/null 2>&1; rc=$?
      e=$(date +%s.%N)
      if [ $rc = 124 ]; then hangs=$((hangs+1)); echo "  HANG $mode run$run $f"; elif [ $rc != 0 ]; then fails=$((fails+1)); echo "  rc=$rc $mode $f"; fi
      echo "$mode $run $f $rc $(echo "$e - $s" | bc)" >> $W/times.txt
    done
    t1=$(date +%s.%N)
    echo "$mode run$run: hangs=$hangs fails=$fails total=$(echo "$t1 - $t0" | bc)s"
  done
  if diff -rq $W/out-$mode-1 $W/out-$mode-2 >/dev/null; then echo "$mode: run1 == run2 (byte-identical)"; else echo "$mode: run1 != run2"; diff -rq $W/out-$mode-1 $W/out-$mode-2 | head -3; fi
done
echo "real vs mockS outputs:"; diff -rq $W/out-real-1 $W/out-mockS-1 | head -5; echo "(end)"
