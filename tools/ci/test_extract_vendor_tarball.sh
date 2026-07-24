#!/usr/bin/env bash
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/extract-vendor-tarball.sh.
#
# Reproduces the defect failure modes locally, with no network and no real
# cache-host, by shimming `ssh` and `rsync` so the "cache-host shared dir" is just a
# local directory. Each case asserts the hardened behaviour:
#
#   1. happy/local        local tarball present + valid -> extracts, no fetch.
#   2. truncated/local    local tarball is a truncated .zst -> MUST NOT reach
#                         tar; self-heals from the (valid) cache-host copy.
#   3. empty-source       local missing AND cache-host source empty/missing ->
#                         fails LOUD (exit 1) with "rerun the full workflow"
#                         guidance; no partial extraction.
#   4. fallback/aux       no local candidate (aux runner) + valid cache-host copy
#                         -> fetches, verifies, extracts.
#   5. sha-mismatch       local .zst is a *valid* zstd frame but its sidecar
#                         sha256 disagrees -> rejected, self-heals from cache-host.
#   6. vanish-at-extract  local tarball PASSES verify but `tar` fails on it
#                         (the observed race: "Local tarball present and
#                         verified", then extraction dies) -> self-heals from
#                         cache-host and extracts correctly.
#
# Run:
#   tools/ci/test_extract_vendor_tarball.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTRACT="${SCRIPT_DIR}/extract-vendor-tarball.sh"
[ -x "$EXTRACT" ] || chmod +x "$EXTRACT"

command -v zstd  >/dev/null || { echo "SKIP: zstd not installed"; exit 0; }

TEST_TMP="$(mktemp -d)"
cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT

FAKE_BIN="$TEST_TMP/bin"
SHARED="$TEST_TMP/cache-host-shared"   # stand-in for /Users/builder/ci/shared/vendor
mkdir -p "$FAKE_BIN" "$SHARED"

SHA="deadbee"
NAME="mod_pagespeed-${SHA}.tar.zst"

# ---------------------------------------------------------------------------
# Build a real, valid vendor tarball (a tiny workspace with GIT_COMMIT).
# ---------------------------------------------------------------------------
SRC="$TEST_TMP/src"
mkdir -p "$SRC"
echo "abcdef0123456789" > "$SRC/GIT_COMMIT"
echo "hello workspace" > "$SRC/marker.txt"
GOOD="$TEST_TMP/${NAME}"
tar --zstd -cf "$GOOD" -C "$SRC" .

# ---------------------------------------------------------------------------
# Shims. The script calls `rsync -a ... user@host:/path/NAME local` and
# `ssh ... user@host "touch ..."`. We translate the "host:/path" form into a
# copy out of $SHARED, and make ssh a no-op success (for the TTL `touch`).
# `--ignore-missing-args` sidecar fetches must succeed-or-skip silently.
# ---------------------------------------------------------------------------
cat > "$FAKE_BIN/rsync" <<SH
#!/usr/bin/env bash
# Last two non-option args are <remote_spec> <local_dest>.
args=()
for a in "\$@"; do
  case "\$a" in
    -*) ;;            # drop flags (-a, -e, --ignore-missing-args)
    "ssh "*) ;;       # drop the -e ssh command value
    *) args+=("\$a") ;;
  esac
done
remote="\${args[\${#args[@]}-2]}"
dest="\${args[\${#args[@]}-1]}"
# remote looks like user@host:/abs/path/<basename>
base="\$(basename "\${remote#*:}")"
src="$SHARED/\$base"
if [ ! -e "\$src" ]; then
  # mimic rsync's behaviour: with --ignore-missing-args, a missing source is
  # a silent no-op success; otherwise it's an error.
  case " \$* " in
    *" --ignore-missing-args "*) exit 0 ;;
    *) echo "rsync: link_stat \"\$src\" failed: No such file or directory" >&2; exit 23 ;;
  esac
fi
cp "\$src" "\$dest"
SH
chmod +x "$FAKE_BIN/rsync"

cat > "$FAKE_BIN/ssh" <<'SH'
#!/usr/bin/env bash
# No-op success: the only ssh call the script makes is the best-effort
# remote `touch` for TTL extension. Always succeed.
exit 0
SH
chmod +x "$FAKE_BIN/ssh"

# getent may not exist on macOS; provide a stub so CI_HUB_HOST resolution is
# deterministic. (The script also accepts CI_HUB_HOST from the env, which we set.)
cat > "$FAKE_BIN/getent" <<'SH'
#!/usr/bin/env bash
exit 2
SH
chmod +x "$FAKE_BIN/getent"

run_extract() {
  # Force the env so no real host is contacted; tighten retries/backoff so the
  # failure case finishes fast.
  PATH="$FAKE_BIN:$PATH" \
  CI_HUB_HOST="cache-host.test" CI_HUB_USER="tester" \
  CI_HUB_VENDOR_DIR="/remote/vendor" \
  FETCH_RETRIES=2 FETCH_BACKOFF=1 \
    bash "$EXTRACT" "$@"
}

PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# ===========================================================================
echo "[1] happy/local: valid local tarball present -> extracts, no fetch"
# ===========================================================================
DEST="$TEST_TMP/dest1"
LOCAL="$TEST_TMP/local1.tar.zst"; cp "$GOOD" "$LOCAL"
# Leave $SHARED empty so any fallback fetch would FAIL -- proves we used local.
rm -f "$SHARED/$NAME"
if run_extract --sha "$SHA" --dest "$DEST" --local "$LOCAL" >"$TEST_TMP/out1.log" 2>&1; then
  if [ -f "$DEST/GIT_COMMIT" ] && [ "$(cat "$DEST/marker.txt")" = "hello workspace" ]; then
    ok "extracted from local without contacting cache-host"
  else
    bad "exit 0 but workspace not extracted"; cat "$TEST_TMP/out1.log"
  fi
else
  bad "happy path returned non-zero"; cat "$TEST_TMP/out1.log"
fi

# ===========================================================================
echo "[2] truncated/local: partial local .zst must NOT reach tar; self-heals"
# ===========================================================================
DEST="$TEST_TMP/dest2"
LOCAL="$TEST_TMP/local2.tar.zst"
# Truncate the good tarball to a partial frame.
head -c 200 "$GOOD" > "$LOCAL"
# Put a VALID copy on cache-host so self-heal can succeed.
cp "$GOOD" "$SHARED/$NAME"
if run_extract --sha "$SHA" --dest "$DEST" --local "$LOCAL" >"$TEST_TMP/out2.log" 2>&1; then
  if [ -f "$DEST/GIT_COMMIT" ] && grep -q "self-healing from cache-host" "$TEST_TMP/out2.log"; then
    ok "rejected partial local, self-healed from cache-host, extracted"
  else
    bad "exit 0 but did not self-heal as expected"; cat "$TEST_TMP/out2.log"
  fi
else
  bad "self-heal path returned non-zero despite valid cache-host copy"; cat "$TEST_TMP/out2.log"
fi

# ===========================================================================
echo "[3] empty-source: local missing AND cache-host empty -> fail LOUD, no partial"
# ===========================================================================
DEST="$TEST_TMP/dest3"
rm -f "$SHARED/$NAME" "$SHARED/$NAME.sha256"
set +e
run_extract --sha "$SHA" --dest "$DEST" --local "$TEST_TMP/does-not-exist.tar.zst" >"$TEST_TMP/out3.log" 2>&1
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
  if grep -q "rerun the FULL workflow" "$TEST_TMP/out3.log"; then
    if [ ! -f "$DEST/GIT_COMMIT" ]; then
      ok "failed loudly (exit $rc) with rerun guidance, no partial extraction"
    else
      bad "failed but left a partial workspace"; cat "$TEST_TMP/out3.log"
    fi
  else
    bad "failed but without actionable rerun guidance"; cat "$TEST_TMP/out3.log"
  fi
else
  bad "MISSING artifact did NOT fail (the silent-success bug this guard exists to kill)"; cat "$TEST_TMP/out3.log"
fi

# ===========================================================================
echo "[4] fallback/aux: no local candidate + valid cache-host copy -> fetch+extract"
# ===========================================================================
DEST="$TEST_TMP/dest4"
cp "$GOOD" "$SHARED/$NAME"
if run_extract --sha "$SHA" --dest "$DEST" >"$TEST_TMP/out4.log" 2>&1; then
  if [ -f "$DEST/GIT_COMMIT" ] && grep -q "No local candidate provided" "$TEST_TMP/out4.log"; then
    ok "aux runner fetched from cache-host and extracted"
  else
    bad "exit 0 but workspace not extracted on aux path"; cat "$TEST_TMP/out4.log"
  fi
else
  bad "aux fallback returned non-zero despite valid cache-host copy"; cat "$TEST_TMP/out4.log"
fi

# ===========================================================================
echo "[5] sha-mismatch: valid zstd frame but wrong sidecar -> reject, self-heal"
# ===========================================================================
DEST="$TEST_TMP/dest5"
LOCAL="$TEST_TMP/local5.tar.zst"; cp "$GOOD" "$LOCAL"
# Sidecar claims a different digest -> integrity must reject even though
# `zstd -t` passes (closes the frame-boundary-truncation gap).
echo "0000000000000000000000000000000000000000000000000000000000000000  $NAME" > "$LOCAL.sha256"
# Valid cache-host copy WITHOUT a sidecar (older vendor run) so self-heal succeeds
# on zstd -t alone.
cp "$GOOD" "$SHARED/$NAME"; rm -f "$SHARED/$NAME.sha256"
if run_extract --sha "$SHA" --dest "$DEST" --local "$LOCAL" >"$TEST_TMP/out5.log" 2>&1; then
  if [ -f "$DEST/GIT_COMMIT" ] && grep -q "sha256 mismatch" "$TEST_TMP/out5.log"; then
    ok "rejected sha-mismatched local, self-healed from cache-host"
  else
    bad "exit 0 but did not detect sha mismatch"; cat "$TEST_TMP/out5.log"
  fi
else
  bad "sha-mismatch path returned non-zero despite valid cache-host copy"; cat "$TEST_TMP/out5.log"
fi

# ===========================================================================
echo "[6] vanish-at-extract: local verifies OK but tar fails -> heal + extract"
# ===========================================================================
DEST="$TEST_TMP/dest6"
LOCAL="$TEST_TMP/local6.tar.zst"
# A valid zstd frame whose payload is NOT a tar archive: passes `zstd -t`
# (no sidecar, so zstd -t is the floor) but `tar --zstd -xf` fails on it.
# This is the same contract as the observed race (verify OK, extraction dies
# seconds later) injected deterministically -- the harness
# cannot delete the file between the script's verify and extract steps.
printf 'valid zstd frame, but definitely not a tar archive\n' | zstd -q > "$LOCAL"
# Valid copy on cache-host so extract-phase self-heal can succeed.
cp "$GOOD" "$SHARED/$NAME"; rm -f "$SHARED/$NAME.sha256"
if run_extract --sha "$SHA" --dest "$DEST" --local "$LOCAL" >"$TEST_TMP/out6.log" 2>&1; then
  if [ -f "$DEST/GIT_COMMIT" ] && [ "$(cat "$DEST/marker.txt")" = "hello workspace" ] \
     && grep -q "verified but extraction failed" "$TEST_TMP/out6.log"; then
    ok "local verified but failed extraction; self-healed from cache-host, extracted"
  else
    bad "exit 0 but did not self-heal/extract as expected"; cat "$TEST_TMP/out6.log"
  fi
else
  bad "extract-phase self-heal returned non-zero despite valid cache-host copy"; cat "$TEST_TMP/out6.log"
fi

echo ""
echo "==== ${PASS} passed, ${FAIL} failed ===="
[ "$FAIL" -eq 0 ]
