#!/bin/bash
# build_sanitizer_module.sh SAN — build a mod_pagespeed.so under an extra
# sanitizer from a scratch repo (which has the fix applied), into a
# DISTINCT output_base so it doesn't clobber the proven ASan build. Copies the
# result to /tmp/mod_<...>.so for cell_run.sh. Run ONE at a time (don't overlap
# heavy sanitizer builds).
#   SAN=ubsan  combined ASan+UBSan (reuses the clang-asan toolchain path; adds
#              -fsanitize=undefined minus the vptr/function checks asan disables)
#   SAN=tsan   ThreadSanitizer (--config=clang-tsan)
set -e
SAN="${1:?usage: build_sanitizer_module.sh ubsan|tsan}"
REPO="$HOME/sanitizer-scratch/repo"
BZL="$HOME/.local/bin/bazelisk"
DC="$HOME/sanitizer-scratch/dcache"
CYC="$HOME/sanitizer-cyclone"
cd "$REPO"

case "$SAN" in
  ubsan)
    OB="$HOME/sanitizer-scratch/obase-ubsan"; DST=/tmp/mod_asan_ubsan.so
    # RECOVERABLE UBSan (no -fno-sanitize-recover): with UBSAN_OPTIONS=halt_on_error=0
    # it logs each "runtime error:" and CONTINUES, so a stress run catalogs every UB
    # site the corpus triggers instead of aborting on the first (often-benign) one.
    # ASan half still aborts on real memory errors (ASAN_OPTIONS=abort_on_error=1).
    EXTRA=(--config=clang-asan
           --copt=-fsanitize=undefined --linkopt=-fsanitize=undefined
           --copt=-fno-sanitize=vptr,function --linkopt=-fno-sanitize=vptr,function) ;;
  tsan)
    OB="$HOME/sanitizer-scratch/obase-tsan"; DST=/tmp/mod_tsan.so
    EXTRA=(--config=clang-tsan) ;;
  *) echo "unknown SAN '$SAN'"; exit 2 ;;
esac

echo "=== build $SAN module  obase=$OB  dst=$DST  ($(date -u +%H:%M:%S)Z) ==="
"$BZL" --output_base="$OB" build "${EXTRA[@]}" \
  --override_repository=cyclone="$CYC" \
  --disk_cache="$DC" \
  --jobs=48 \
  //:libmod_pagespeed.so 2>&1 | tail -30
RC=${PIPESTATUS[0]}
echo "BUILD_RC=$RC"
if [ "$RC" -eq 0 ] && [ -f bazel-bin/libmod_pagespeed.so ]; then
  cp bazel-bin/libmod_pagespeed.so "$DST"
  chmod 644 "$DST"
  echo "COPIED $DST  size=$(stat -c%s "$DST")  buildid=$(file "$DST" | grep -oP 'BuildID\[sha1\]=\K[0-9a-f]+' || echo '?')"
  echo "BUILD_OK $SAN"
else
  echo "BUILD_FAIL $SAN"
fi
