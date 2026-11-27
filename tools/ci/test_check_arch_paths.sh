#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for check_arch_paths.sh: pass and fail fixtures, then the real
# tree.
#   bash tools/ci/test_check_arch_paths.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/check_arch_paths.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

fail=0
expect() {
  local want="$1" name="$2" file="$3" rc=0
  bash "$SCRIPT" "$file" >"$T/out" 2>&1 || rc=$?
  if { [ "$want" = pass ] && [ "$rc" -ne 0 ]; } || { [ "$want" = fail ] && [ "$rc" -ne 1 ]; }; then
    echo "FAIL: ${name}: want ${want}, exit ${rc}"
    cat "$T/out"
    fail=1
  else
    echo "ok: ${name}"
  fi
}

cat > "$T/good.sh" <<'EOF'
# Comments may name x86_64-redhat-linux and aarch64-linux-gnu.
EL_ARCH="$(rpm --eval '%{_arch}')"
ln -sfn "${P}/lib/gcc/${EL_ARCH}-redhat-linux/15" "/usr/lib/gcc/${EL_ARCH}-linux-gnu/15"
case "$(uname -m)" in
  amd64|x86_64)  t="x86_64-linux-gnu" ;;
  arm64|aarch64) t="aarch64-linux-gnu" ;;
esac
  x86_64) LIB=/usr/lib/gcc/x86_64-redhat-linux ;;
mkdir -p /usr/lib/gcc/x86_64-linux-gnu  # arch-path-ok: this leg is x86_64 only
EOF
expect pass "derived triplets, arch case arms, allow marker" "$T/good.sh"

cat > "$T/bad_mkdir.sh" <<'EOF'
mkdir -p /usr/include/c++ /usr/lib/gcc/x86_64-linux-gnu
EOF
expect fail "literal x86_64 multiarch path" "$T/bad_mkdir.sh"

cat > "$T/bad_rh.sh" <<'EOF'
  LIBA="${P}/lib/gcc/aarch64-redhat-linux/13/libstdc++.a"
EOF
expect fail "literal aarch64 toolset path" "$T/bad_rh.sh"

cat > "$T/bad_after_arm.sh" <<'EOF'
case "$x" in
  x86_64) ;;
esac
ln -sfn a /usr/include/x86_64-linux-gnu/c++/15
EOF
expect fail "a literal path after an arch case closes" "$T/bad_after_arm.sh"

expect fail "missing file fails closed" "$T/nope.sh"

# The real tree must be clean.
if bash "$SCRIPT" >"$T/out" 2>&1; then
  echo "ok: repository container scripts"
else
  echo "FAIL: repository container scripts"
  cat "$T/out"
  fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "test_check_arch_paths: all passed"
