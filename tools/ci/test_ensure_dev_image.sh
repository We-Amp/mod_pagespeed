#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/ensure-dev-image.sh. Hermetic: `docker` is a shim that
# records its calls and keeps "images" as marker files, so no daemon is
# needed. Cases:
#
#   1. path-independent  the same docker/ bytes at two absolute paths get the
#                        same tag (workflows once hashed absolute paths).
#   2. content-sensitive a changed byte, or a renamed file, changes the tag.
#   3. empty/missing     an empty or missing tree fails loud (exit 1, ::error::).
#   4. build-once        a missing image is built once; the next call is a
#                        cache hit; --tag-latest tags :latest; stdout is the
#                        image name alone.
#   5. dead-holder       (flock only) a lock holder that is SIGKILLed does not
#                        strand the waiter.
#   6. bounded-wait      (flock only) a live holder past --lock-timeout: the
#                        waiter warns and builds anyway.
#   7. derived           --derived builds the dev image, then REPO:<arch>-<hash>
#                        from the named Dockerfile with BASE=<dev image>, each
#                        once; prints the derived name (also with --name-only);
#                        a missing Dockerfile or a bad REPO fails.
#   8. derived-lock      (flock only) the derived build takes its own lock
#                        file: a live holder of it makes the waiter wait.
#   9. old-lock-sweep    a build call removes EMPTY legacy
#                        docker-build-*.lock dirs past the age guard, and
#                        leaves fresh ones, non-empty ones, other names, and
#                        everything on a --name-only call.
#  10. lock-dir-create   (flock only) a missing CI_DEV_IMAGE_LOCK_DIR is
#                        created (mode 1777) and the lock file in it is used.
#  11. build-race-won    docker build fails but the image exists afterwards
#                        (a concurrent build won): success, image printed.
#  12. build-fail        docker build fails and no image exists: failure.
#
# Run: tools/ci/test_ensure_dev_image.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENSURE="${SCRIPT_DIR}/ensure-dev-image.sh"

T="$(mktemp -d)"
HOLDER=""
cleanup() {
  [ -z "$HOLDER" ] || kill -9 "$HOLDER" 2>/dev/null || true
  rm -rf "$T"
}
trap cleanup EXIT

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

mk_tree() {
  mkdir -p "$1/sub"
  printf 'FROM scratch\n' > "$1/Dockerfile"
  printf 'echo hi\n' > "$1/build-release.sh"
  printf 'x\n' > "$1/sub/file.txt"
}

# docker shim: `image inspect X` succeeds iff $T/images/<X> exists; `build -t
# X` creates it (after SHIM_BUILD_DELAY seconds), or fails with
# SHIM_BUILD_FAIL=present (creating X first, as a concurrent builder would)
# or SHIM_BUILD_FAIL=absent; `tag A B` creates B. Every call is appended to
# $T/docker.log.
mkdir -p "$T/bin" "$T/images" "$T/lock"
cat > "$T/bin/docker" <<'SHIM'
#!/usr/bin/env bash
echo "$*" >> "$SHIM_ROOT/docker.log"
key() { printf '%s' "$1" | tr '/:' '__'; }
case "$1 $2" in
  "image inspect") [ -e "$SHIM_ROOT/images/$(key "$3")" ] ;;
  "build -t"*) sleep "${SHIM_BUILD_DELAY:-0}"; echo "building $3"
               case "${SHIM_BUILD_FAIL:-}" in
                 # Another builder created the tag first.
                 present) touch "$SHIM_ROOT/images/$(key "$3")"
                          echo "ERROR: image \"$3\": already exists" >&2
                          exit 1 ;;
                 absent) echo "ERROR: build failed" >&2; exit 1 ;;
               esac
               touch "$SHIM_ROOT/images/$(key "$3")" ;;
  "tag "*) touch "$SHIM_ROOT/images/$(key "$3")" ;;
  *) exit 0 ;;
esac
SHIM
chmod +x "$T/bin/docker"
export PATH="$T/bin:$PATH" SHIM_ROOT="$T" CI_DEV_IMAGE_LOCK_DIR="$T/lock" \
  CI_OLD_LOCK_DIR="$T/no-old-locks"

# 1. path-independent
mk_tree "$T/a/docker"
mk_tree "$T/elsewhere/deeper/docker"
A=$(bash "$ENSURE" --name-only x64 "$T/a/docker")
B=$(bash "$ENSURE" --name-only x64 "$T/elsewhere/deeper/docker")
R=$(cd "$T/a" && bash "$ENSURE" --name-only x64 docker)
if [ "$A" = "$B" ] && [ "$A" = "$R" ]; then
  pass "same bytes, different paths -> same tag ($A)"
else
  fail "tags differ by path: $A / $B / $R"
fi
case "$A" in
  pagespeed1.1-dev:x64-[0-9a-f]*) pass "tag shape ($A)" ;;
  *) fail "unexpected tag shape: $A" ;;
esac

# 2. content-sensitive
mk_tree "$T/c/docker"; printf 'FROM scratch \n' > "$T/c/docker/Dockerfile"
mk_tree "$T/d/docker"; mv "$T/d/docker/sub/file.txt" "$T/d/docker/sub/other.txt"
C=$(bash "$ENSURE" --name-only x64 "$T/c/docker")
D=$(bash "$ENSURE" --name-only x64 "$T/d/docker")
if [ "$C" != "$A" ] && [ "$D" != "$A" ] && [ "$C" != "$D" ]; then
  pass "content and relative path both feed the tag"
else
  fail "tag did not change: $A / $C / $D"
fi

# 3. empty/missing
mkdir -p "$T/empty/docker"
for d in "$T/empty/docker" "$T/missing/docker"; do
  if OUT=$(bash "$ENSURE" x64 "$d" 2>&1); then
    fail "no failure for $d"
  elif printf '%s' "$OUT" | grep -q '::error::'; then
    pass "loud failure for ${d#"$T"/}"
  else
    fail "failure without ::error:: for $d: $OUT"
  fi
done

# 4. build-once
: > "$T/docker.log"
OUT1=$(bash "$ENSURE" --tag-latest x64 "$T/a/docker" 2>/dev/null)
OUT2=$(bash "$ENSURE" x64 "$T/a/docker" 2>/dev/null)
BUILDS=$(grep -c '^build ' "$T/docker.log" || true)
if [ "$OUT1" = "$A" ] && [ "$OUT2" = "$A" ] && [ "$BUILDS" -eq 1 ]; then
  pass "built once, then cache hit; stdout is the name alone"
else
  fail "builds=$BUILDS out1='$OUT1' out2='$OUT2'"
fi
if grep -q "^tag $A pagespeed1.1-dev:latest$" "$T/docker.log"; then
  pass "--tag-latest tags :latest"
else
  fail "--tag-latest did not tag"
fi

if ! command -v flock >/dev/null 2>&1; then
  echo "SKIP: flock not installed (cases 5-6)"
else
  mk_tree "$T/e/docker"; printf 'e\n' > "$T/e/docker/e.txt"
  E=$(bash "$ENSURE" --name-only x64 "$T/e/docker")
  LOCK="$T/lock/pagespeed1.1-dev-build-x64-${E##*-}.lock"
  # The holder keeps the locked descriptor itself (exec), so a SIGKILL of
  # $HOLDER is the death of the only process holding the lock.
  hold_lock() {
    ( exec 9>>"$1"; flock 9; exec sleep 600 ) &
    HOLDER=$!
  }

  # 5. dead-holder: a holder that dies by SIGKILL releases the lock.
  hold_lock "$LOCK"
  sleep 1
  ( sleep 3; kill -9 "$HOLDER" 2>/dev/null ) &
  START=$SECONDS
  bash "$ENSURE" --lock-timeout 60 x64 "$T/e/docker" >/dev/null 2>&1
  WAITED=$((SECONDS - START))
  wait "$HOLDER" 2>/dev/null || true
  HOLDER=""
  # >= 2: the holder lives ~3 s into the wait, so a waiter that ignored the
  # lock would finish at once and must not pass.
  if [ "$WAITED" -ge 2 ] && [ "$WAITED" -lt 30 ] \
     && [ -e "$T/images/$(printf '%s' "$E" | tr '/:' '__')" ]; then
    pass "SIGKILLed holder released the lock (waited ${WAITED}s)"
  else
    fail "waiter stranded or no build (waited ${WAITED}s)"
  fi

  # 6. bounded-wait: a live holder beyond the timeout -> warn, build anyway.
  mk_tree "$T/f/docker"; printf 'f\n' > "$T/f/docker/f.txt"
  F=$(bash "$ENSURE" --name-only x64 "$T/f/docker")
  hold_lock "$T/lock/pagespeed1.1-dev-build-x64-${F##*-}.lock"
  sleep 1
  ERR=$(bash "$ENSURE" --lock-timeout 3 x64 "$T/f/docker" 2>&1 >/dev/null)
  kill -9 "$HOLDER" 2>/dev/null || true
  wait "$HOLDER" 2>/dev/null || true
  HOLDER=""
  if printf '%s' "$ERR" | grep -q '::warning::waited 3s' \
     && [ -e "$T/images/$(printf '%s' "$F" | tr '/:' '__')" ]; then
    pass "bounded wait: warned and built without the lock"
  else
    fail "bounded wait misbehaved: $ERR"
  fi
fi

# 7. derived
mk_tree "$T/g/docker"; printf 'ARG BASE\nFROM base\n' > "$T/g/docker/Dockerfile.lint"
G=$(bash "$ENSURE" --name-only x64 "$T/g/docker")
GL=$(bash "$ENSURE" --name-only --derived pagespeed1.1-lint Dockerfile.lint x64 "$T/g/docker")
if [ "$GL" = "pagespeed1.1-lint:x64-${G##*-}" ]; then
  pass "--name-only --derived prints the derived name ($GL)"
else
  fail "derived name '$GL' does not match dev tag '$G'"
fi
: > "$T/docker.log"
OUT1=$(bash "$ENSURE" --tag-latest --derived pagespeed1.1-lint Dockerfile.lint x64 "$T/g/docker" 2>/dev/null)
OUT2=$(bash "$ENSURE" --derived pagespeed1.1-lint Dockerfile.lint x64 "$T/g/docker" 2>/dev/null)
DEV_BUILDS=$(grep -c "^build -t $G " "$T/docker.log" || true)
LINT_BUILDS=$(grep -c "^build -t $GL --build-arg BASE=$G -f $T/g/docker/Dockerfile.lint $T/g/docker$" "$T/docker.log" || true)
if [ "$OUT1" = "$GL" ] && [ "$OUT2" = "$GL" ] && [ "$DEV_BUILDS" -eq 1 ] \
   && [ "$LINT_BUILDS" -eq 1 ] && grep -q "^tag $G pagespeed1.1-dev:latest$" "$T/docker.log"; then
  pass "derived: dev then derived image built once each, BASE passed, :latest on the dev image"
else
  fail "derived: out1='$OUT1' out2='$OUT2' dev_builds=$DEV_BUILDS lint_builds=$LINT_BUILDS; log: $(cat "$T/docker.log")"
fi
if OUT=$(bash "$ENSURE" --derived pagespeed1.1-lint Dockerfile.nope x64 "$T/g/docker" 2>&1); then
  fail "no failure for a missing derived Dockerfile"
elif printf '%s' "$OUT" | grep -q '::error::'; then
  pass "missing derived Dockerfile fails loud"
else
  fail "missing derived Dockerfile failed without ::error::: $OUT"
fi
RC=0
bash "$ENSURE" --derived pagespeed1.1-dev Dockerfile.lint x64 "$T/g/docker" >/dev/null 2>&1 || RC=$?
RC2=0
bash "$ENSURE" --derived lint Dockerfile.lint >/dev/null 2>&1 || RC2=$?
if [ "$RC" -eq 2 ] && [ "$RC2" -eq 2 ]; then
  pass "bad --derived usage exits 2"
else
  fail "bad --derived usage: rc=$RC rc2=$RC2"
fi

if command -v flock >/dev/null 2>&1; then
  # 8. derived-lock: the dev image is present, the derived lock is held.
  mk_tree "$T/h/docker"; printf 'ARG BASE\nFROM base\nRUN h\n' > "$T/h/docker/Dockerfile.lint"
  bash "$ENSURE" x64 "$T/h/docker" >/dev/null 2>&1
  HL=$(bash "$ENSURE" --name-only --derived pagespeed1.1-lint Dockerfile.lint x64 "$T/h/docker")
  hold_lock "$T/lock/pagespeed1.1-lint-build-x64-${HL##*-}.lock"
  sleep 1
  ( sleep 3; kill -9 "$HOLDER" 2>/dev/null ) &
  START=$SECONDS
  ERR=$(bash "$ENSURE" --lock-timeout 60 --derived pagespeed1.1-lint Dockerfile.lint x64 "$T/h/docker" 2>&1 >/dev/null)
  WAITED=$((SECONDS - START))
  wait "$HOLDER" 2>/dev/null || true
  HOLDER=""
  if [ "$WAITED" -ge 2 ] && [ "$WAITED" -lt 30 ] \
     && printf '%s' "$ERR" | grep -q "waiting for a concurrent build of $HL" \
     && [ -e "$T/images/$(printf '%s' "$HL" | tr '/:' '__')" ]; then
    pass "derived build waits on its own lock (waited ${WAITED}s)"
  else
    fail "derived lock not honoured (waited ${WAITED}s): $ERR"
  fi
else
  echo "SKIP: flock not installed (case 8)"
fi

# 9. old-lock-sweep
OLD="$T/oldtmp"
mkdir -p "$OLD/docker-build-x64-aaaaaaaaaaaa.lock" \
         "$OLD/docker-build-x64-bbbbbbbbbbbb.lock" \
         "$OLD/docker-build-arm64-cccccccccccc.lock/x" \
         "$OLD/other-build-x64-dddddddddddd.lock"
touch -t 202601010000 "$OLD/docker-build-x64-aaaaaaaaaaaa.lock" \
  "$OLD/docker-build-arm64-cccccccccccc.lock" \
  "$OLD/other-build-x64-dddddddddddd.lock"
CI_OLD_LOCK_DIR="$OLD" bash "$ENSURE" --name-only x64 "$T/a/docker" >/dev/null
if [ -d "$OLD/docker-build-x64-aaaaaaaaaaaa.lock" ]; then
  pass "--name-only leaves old lock dirs alone"
else
  fail "--name-only swept an old lock dir"
fi
ERR=$(CI_OLD_LOCK_DIR="$OLD" bash "$ENSURE" x64 "$T/a/docker" 2>&1 >/dev/null)
if [ ! -e "$OLD/docker-build-x64-aaaaaaaaaaaa.lock" ] \
   && [ -d "$OLD/docker-build-x64-bbbbbbbbbbbb.lock" ] \
   && [ -d "$OLD/docker-build-arm64-cccccccccccc.lock/x" ] \
   && [ -d "$OLD/other-build-x64-dddddddddddd.lock" ] \
   && printf '%s' "$ERR" | grep -q 'removed stale legacy lock directory'; then
  pass "old-lock sweep: aged empty dir removed; fresh, non-empty and other dirs kept"
else
  fail "old-lock sweep misbehaved: $(ls "$OLD") / $ERR"
fi

# 10. lock-dir-create
if command -v flock >/dev/null 2>&1; then
  mk_tree "$T/i/docker"; printf 'i\n' > "$T/i/docker/i.txt"
  NEWLOCK="$T/cleared/locks"
  ERR=$(CI_DEV_IMAGE_LOCK_DIR="$NEWLOCK" bash "$ENSURE" x64 "$T/i/docker" 2>&1 >/dev/null)
  I=$(bash "$ENSURE" --name-only x64 "$T/i/docker")
  MODE=$(stat -c %a "$NEWLOCK" 2>/dev/null || stat -f %Lp "$NEWLOCK")
  if [ -d "$NEWLOCK" ] && [ "$MODE" = 1777 ] \
     && [ -f "$NEWLOCK/pagespeed1.1-dev-build-x64-${I##*-}.lock" ] \
     && ! printf '%s' "$ERR" | grep -q 'without the per-host lock' \
     && [ -e "$T/images/$(printf '%s' "$I" | tr '/:' '__')" ]; then
    pass "missing lock dir created (mode $MODE) and its lock used"
  else
    fail "lock dir not created or lock not used (mode '$MODE'): $ERR"
  fi
else
  echo "SKIP: flock not installed (case 10)"
fi

# 11. build-race-won
mk_tree "$T/j/docker"; printf 'j\n' > "$T/j/docker/j.txt"
J=$(bash "$ENSURE" --name-only x64 "$T/j/docker")
RC=0
OUT=$(SHIM_BUILD_FAIL=present bash "$ENSURE" x64 "$T/j/docker" 2>"$T/err11") || RC=$?
if [ "$RC" -eq 0 ] && [ "$OUT" = "$J" ] \
   && grep -q 'a concurrent build won' "$T/err11"; then
  pass "failed build with the image present afterwards succeeds"
else
  fail "race-won build: rc=$RC out='$OUT' err=$(cat "$T/err11")"
fi

# 12. build-fail
mk_tree "$T/k/docker"; printf 'k\n' > "$T/k/docker/k.txt"
RC=0
SHIM_BUILD_FAIL=absent bash "$ENSURE" x64 "$T/k/docker" >/dev/null 2>&1 || RC=$?
if [ "$RC" -ne 0 ]; then
  pass "failed build with no image still fails (rc=$RC)"
else
  fail "failed build with no image returned success"
fi

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS case(s) failed"
  exit 1
fi
echo "All ensure-dev-image cases passed"
