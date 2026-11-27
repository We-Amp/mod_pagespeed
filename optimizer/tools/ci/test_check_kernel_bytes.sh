#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Offline test for tools/ci/check-kernel-bytes.sh. The sync tool is replaced by
# a stub that exits with a chosen code, so the test pins the decision table
# (pass, tool failure with its reason, unreachable canonical on a pull request
# with and without a pin bump, on a push, with a failing origin fetch, an
# absent or malformed pin, a failed clone with retries, and a compare that dies
# with exit 128). It also pins the sync tools' pin-ancestry gate (no override)
# against a tiny local repository.
#
# Optional end-to-end part, NOT part of the gate (it needs a local clone of the
# canonical repository, which needs the network once):
#   git clone --bare https://github.com/We-Amp/mod_pagespeed.git /tmp/canon.git
#   KERNEL_BYTES_E2E_SOURCE=/tmp/canon.git tools/ci/test_check_kernel_bytes.sh
# It runs the real sync tools against that clone: the unmodified tree must pass,
# a comment-only edit of a vendored file and a planted file must fail, each with
# the tool's own reason.
#
# Run: tools/ci/test_check_kernel_bytes.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
CHECK="${HERE}/check-kernel-bytes.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
FAILS=0

SHA_A="$(printf 'a%.0s' $(seq 40))"
SHA_B="$(printf 'b%.0s' $(seq 40))"
GIT=(git -c user.name=t -c user.email=t@example.invalid)

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; [ -z "${2:-}" ] || sed 's/^/    /' "$2"; FAILS=$((FAILS + 1)); }

# A "main" repo whose pin is SHA_A, cloned as the workspace (origin/main there).
git init -q -b main "${WORK}/origin"
mkdir -p "${WORK}/origin/lib/k"
printf '# comment\n%s  note\nfloor %s\n' "${SHA_A}" "${SHA_A}" > "${WORK}/origin/lib/k/PIN"
git -C "${WORK}/origin" add -A
"${GIT[@]}" -C "${WORK}/origin" commit -q -m init
git clone -q "${WORK}/origin" "${WORK}/ws"

# A second main whose pin file is absent.
git init -q -b main "${WORK}/origin2"
echo x > "${WORK}/origin2/other"
git -C "${WORK}/origin2" add -A
"${GIT[@]}" -C "${WORK}/origin2" commit -q -m init
git clone -q "${WORK}/origin2" "${WORK}/ws2"
mkdir -p "${WORK}/ws2/lib/k"

# A workspace whose origin cannot be fetched.
git clone -q "${WORK}/origin" "${WORK}/ws3"
git -C "${WORK}/ws3" remote set-url origin "file://${WORK}/gone"

# Stub sync tool: exits with $STUB_RC after printing $STUB_OUT.
cat > "${WORK}/stub.sh" <<'STUB'
#!/bin/bash
echo "stub sync called: $*"
[ -z "${STUB_OUT:-}" ] || printf '%s\n' "${STUB_OUT}"
exit "${STUB_RC:-0}"
STUB

mkdir -p "${WORK}/canon"   # a "reachable canonical": any existing directory

# run <name> <want-rc> <stub-rc> <event> <pin line or ->  <canonical: yes|no> [workspace]
run() {
  local name="$1" want="$2" stub="$3" event="$4" pin="$5" canon="$6" ws="${7:-${WORK}/ws}" rc=0 args
  if [ "${pin}" = "-" ]; then rm -f "${ws}/lib/k/PIN"
  else printf '# comment\n%s  note\nfloor %s\n' "${pin}" "${SHA_A}" > "${ws}/lib/k/PIN"; fi
  args=(--sync-tool "${WORK}/stub.sh" --pin-file "${ws}/lib/k/PIN" --event "${event}" --base-repo "${ws}")
  if [ "${canon}" = yes ]; then args+=(--canonical "${WORK}/canon")
  else args+=(--canonical-url "file://${WORK}/does-not-exist"); fi
  ( cd "${ws}" && STUB_RC="${stub}" KERNEL_BYTES_CLONE_ATTEMPTS=1 KERNEL_BYTES_RETRY_SLEEP=0 \
      bash "${CHECK}" "${args[@]}" ) > "${WORK}/out" 2>&1 || rc=$?
  if [ "${rc}" = "${want}" ]; then pass "${name}"; else fail "${name} (want rc=${want}, got ${rc})" "${WORK}/out"; fi
}

run "bytes match"                                  0 0   pull_request "${SHA_A}" yes
run "tool failure (exit 1) fails"                  1 1   pull_request "${SHA_A}" yes
run "tool failure fails on a push too"             1 1   push         "${SHA_A}" yes
run "pin not found in canonical (3) fails"         1 3   pull_request "${SHA_A}" yes
run "ancestry refusal (4) fails"                   1 4   pull_request "${SHA_A}" yes
run "floor violation (6) fails"                    1 6   pull_request "${SHA_A}" yes
run "unreachable, PR, no pin bump: warns"          0 0   pull_request "${SHA_A}" no
run "unreachable, PR, pin bump: fails closed"      1 0   pull_request "${SHA_B}" no
run "unreachable, push: fails closed"              1 0   push         "${SHA_A}" no
run "unreachable, merge queue: fails closed"       1 0   merge_group  "${SHA_A}" no
run "compare dies with 128, PR, no bump: warns"    0 128 pull_request "${SHA_A}" yes
run "compare dies with 128, pin bump: fails"       1 128 pull_request "${SHA_B}" yes
run "compare dies with 128, push: fails"           1 128 push         "${SHA_A}" yes

# In URL mode the clone is complete: exit 128 from the compare is a real failure, never a skip.
git init -q -b main "${WORK}/urlcanon"; echo 1 > "${WORK}/urlcanon/f"; git -C "${WORK}/urlcanon" add -A
"${GIT[@]}" -C "${WORK}/urlcanon" commit -q -m one
printf '# c\n%s  n\nfloor %s\n' "${SHA_A}" "${SHA_A}" > "${WORK}/ws/lib/k/PIN"
rc=0; ( cd "${WORK}/ws" && STUB_RC=128 bash "${CHECK}" --sync-tool "${WORK}/stub.sh" --pin-file "${WORK}/ws/lib/k/PIN" \
    --event pull_request --base-repo "${WORK}/ws" --canonical-url "file://${WORK}/urlcanon" ) > "${WORK}/out" 2>&1 || rc=$?
if [ "${rc}" = 1 ] && ! grep -q '^::warning::' "${WORK}/out" && grep -q 'failed with exit 128' "${WORK}/out"; then
  pass "URL mode: exit 128 on an unchanged-pin PR fails, no warning"
else fail "URL mode exit 128 (rc=${rc})" "${WORK}/out"; fi
run "origin fetch fails, canonical down: fails"    1 0   pull_request "${SHA_A}" no "${WORK}/ws3"
run "pin absent from main, canonical down: fails"  1 0   pull_request "${SHA_A}" no "${WORK}/ws2"
run "malformed pin, canonical down: fails"         1 0   pull_request "not-a-sha" no
run "pin file missing is a usage error"            2 0   pull_request "-" yes

# The warning is an annotation, so a reviewer sees that nothing was compared.
printf '# comment\n%s  note\nfloor %s\n' "${SHA_A}" "${SHA_A}" > "${WORK}/ws/lib/k/PIN"
( cd "${WORK}/ws" && KERNEL_BYTES_CLONE_ATTEMPTS=1 bash "${CHECK}" --sync-tool "${WORK}/stub.sh" \
    --pin-file "${WORK}/ws/lib/k/PIN" --event pull_request --base-repo "${WORK}/ws" \
    --canonical-url "file://${WORK}/does-not-exist" ) > "${WORK}/out" 2>&1
if grep -q '^::warning::kernel byte check' "${WORK}/out"; then pass "the skip is announced as a warning"
else fail "no warning annotation" "${WORK}/out"; fi

# A failed clone is retried a bounded number of times and its stderr is kept.
( cd "${WORK}/ws" && KERNEL_BYTES_CLONE_ATTEMPTS=2 KERNEL_BYTES_RETRY_SLEEP=0 bash "${CHECK}" --sync-tool "${WORK}/stub.sh" \
    --pin-file "${WORK}/ws/lib/k/PIN" --event pull_request --base-repo "${WORK}/ws" \
    --canonical-url "file://${WORK}/does-not-exist" ) > "${WORK}/out" 2>&1
if grep -q 'clone attempt 2/2 failed' "${WORK}/out" && grep -q '^clone: .*\(fatal\|does not\)' "${WORK}/out" \
   && ! grep -q 'clone attempt 3' "${WORK}/out"; then pass "clone retried twice, stderr kept in the log"
else fail "clone retry/log" "${WORK}/out"; fi

# A clone that succeeds after a retry leaves its log behind.
cat > "${WORK}/flaky-git" <<'FLK'
#!/bin/bash
# fails the first clone, then delegates to the real git (this wrapper is first in PATH)
if [ "$1" = clone ] && [ ! -e "${FLAKY_MARK}" ]; then : > "${FLAKY_MARK}"; echo 'fatal: simulated network error' >&2; exit 128; fi
exec "${REAL_GIT}" "$@"
FLK
mkdir -p "${WORK}/flakybin"; cp "${WORK}/flaky-git" "${WORK}/flakybin/git"; chmod +x "${WORK}/flakybin/git"
rc=0; ( cd "${WORK}/ws" && PATH="${WORK}/flakybin:${PATH}" REAL_GIT="$(command -v git)" FLAKY_MARK="${WORK}/flaky.mark" \
    KERNEL_BYTES_RETRY_SLEEP=0 \
    bash "${CHECK}" --sync-tool "${WORK}/stub.sh" --pin-file "${WORK}/ws/lib/k/PIN" --event pull_request \
    --base-repo "${WORK}/ws" --canonical-url "file://${WORK}/urlcanon" ) > "${WORK}/out" 2>&1 || rc=$?
if [ "${rc}" = 0 ] && grep -q 'clone attempt 1/3 failed' "${WORK}/out" && grep -q 'simulated network error' "${WORK}/out"; then
  pass "a retry that later succeeds leaves a trace in the log"
else fail "retry trace (rc=${rc})" "${WORK}/out"; fi

# The error repeats the tool's own reason: a planted file is not called a byte difference.
( cd "${WORK}/ws" && STUB_RC=1 STUB_OUT="RED: unexpected file under lib/k: lib/k/evil.cc" bash "${CHECK}" \
    --sync-tool "${WORK}/stub.sh" --pin-file "${WORK}/ws/lib/k/PIN" --event pull_request \
    --base-repo "${WORK}/ws" --canonical "${WORK}/canon" ) > "${WORK}/out" 2>&1
if grep -q '^::error::.*RED: unexpected file under lib/k: lib/k/evil.cc' "${WORK}/out" && ! grep -qi 'differ' "${WORK}/out"; then
  pass "the error carries the tool's own reason"
else fail "tool reason not repeated" "${WORK}/out"; fi

# --- Pin-ancestry gate of the real sync tools, offline: no override. ---
for k in html js; do
  R="${WORK}/sync-${k}"
  mkdir -p "${R}/tools" "${R}/lib/${k}"
  cp "${ROOT}/tools/sync-${k}-kernel.sh" "${ROOT}/tools/check-${k}-kernel-membership.sh" "${R}/tools/"
  C="${WORK}/canon-${k}"
  git init -q -b master "${C}"
  echo 1 > "${C}/f"; git -C "${C}" add -A; "${GIT[@]}" -C "${C}" commit -q -m one
  git -C "${C}" checkout -q -b side
  echo 2 > "${C}/f"; "${GIT[@]}" -C "${C}" commit -q -am two
  SIDE="$(git -C "${C}" rev-parse HEAD)"
  git -C "${C}" checkout -q master
  MAIN="$(git -C "${C}" rev-parse HEAD)"
  pinfile="$(echo "${k}" | tr a-z A-Z)_KERNEL_PIN"
  printf '%s  unmerged\nfloor %s\n' "${SIDE}" "${MAIN}" > "${R}/lib/${k}/${pinfile}"
  rc=0; SYNC_ALLOW_UNMERGED_PIN=1 bash "${R}/tools/sync-${k}-kernel.sh" --source "${C}" --check > "${WORK}/out" 2>&1 || rc=$?
  if [ "${rc}" = 4 ] && grep -q 'Merged pins only' "${WORK}/out" && ! grep -q 'SYNC_ALLOW_UNMERGED_PIN' "${WORK}/out"; then
    pass "${k}: an unmerged pin is refused (exit 4), the old override variable is ignored"
  else fail "${k}: unmerged pin gate (rc=${rc})" "${WORK}/out"; fi
done

# --- Optional end-to-end part (real sync tools, real canonical clone). ---
if [ -n "${KERNEL_BYTES_E2E_SOURCE:-}" ]; then
  for k in html js; do
    pinfile="$(echo "${k}" | tr a-z A-Z)_KERNEL_PIN"
    E="${WORK}/e2e-${k}"
    mkdir -p "${E}/tools/ci" "${E}/lib"
    cp "${ROOT}/tools/sync-${k}-kernel.sh" "${ROOT}/tools/check-${k}-kernel-membership.sh" "${E}/tools/"
    cp -R "${ROOT}/lib/${k}" "${E}/lib/${k}"
    git init -q -b main "${E}" >/dev/null 2>&1
    e2e() { # name want-rc grep-pattern
      local rc=0
      ( cd "${E}" && bash "${CHECK}" --sync-tool tools/sync-${k}-kernel.sh --pin-file "lib/${k}/${pinfile}" \
          --event pull_request --base-repo "${E}" --canonical "${KERNEL_BYTES_E2E_SOURCE}" ) > "${WORK}/out" 2>&1 || rc=$?
      if [ "${rc}" = "$2" ] && { [ -z "${3:-}" ] || grep -q "$3" "${WORK}/out"; }; then pass "e2e ${k}: $1"
      else fail "e2e ${k}: $1 (want rc=$2, got ${rc})" "${WORK}/out"; fi
    }
    e2e "unmodified tree passes" 0
    f="$(grep -E '\.cc$' "${E}/lib/${k}/SYNCED_FILES.txt" | head -n1)"
    f="${E}/${f}"
    cp "${f}" "${WORK}/saved"; echo "// local tweak" >> "${f}"
    e2e "comment-only edit of a vendored file fails with DRIFT" 1 'DRIFT:'
    cp "${WORK}/saved" "${f}"
    echo "int x;" > "${E}/lib/${k}/planted_$k.cc"
    e2e "planted file fails with the membership reason" 1 'RED: unexpected file'
    rm -f "${E}/lib/${k}/planted_$k.cc"
    e2e "restored tree passes again" 0
  done
else
  echo "SKIP: end-to-end part (set KERNEL_BYTES_E2E_SOURCE to a local clone of the canonical repository)"
fi

if [ "${FAILS}" -eq 0 ]; then echo "check-kernel-bytes tests passed"; else echo "${FAILS} failed"; exit 1; fi
