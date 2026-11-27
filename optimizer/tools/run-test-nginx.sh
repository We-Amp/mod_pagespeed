#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODULE_SO="${MODULE_SO:-${REPO_ROOT}/bazel-bin/src/nginx/ngx_pagespeed_module.so}"

# --- Pre-flight checks ---

if [ ! -f "$MODULE_SO" ]; then
  echo "Error: Module not built. Run:"
  echo "  NGINX_PATH=/path/to/nginx-src bazel build //src/nginx:ngx_pagespeed_module.so"
  exit 1
fi

# ABI check: verify the module loads without crashing. nginx -t cannot read
# its config from /dev/stdin when stdin is a socket, so use a temp file.
ABI_TMPDIR="$(mktemp -d)"
trap 'rm -rf "$ABI_TMPDIR"' EXIT
cat >"$ABI_TMPDIR/abi-test.conf" <<ABIEOF
load_module $MODULE_SO;
events { worker_connections 64; }
http { server { listen 19840; } }
ABIEOF
if ! nginx -t -c "$ABI_TMPDIR/abi-test.conf" 2>/dev/null; then
  echo "Error: Module ABI mismatch. Rebuild with matching nginx headers."
  echo "  nginx binary: $(nginx -v 2>&1)"
  exit 1
fi

# Kill stale nginx from interrupted test runs
pkill -u "$(whoami)" -f "nginx.*master.*servroot" 2>/dev/null || true
sleep 0.3

# Clean up cache files and sockets from previous runs
rm -f /tmp/test-ps-*.vol /tmp/test-ps-*.vol.gen /tmp/test-ps-*.sock

# --- Seed cache files for HIT-path tests ---

SEED_TOOL="${SEED_TOOL:-${SEED_TEST_CACHE:-${REPO_ROOT}/bazel-bin/tools/seed_test_cache}}"
export SEED_TEST_CACHE="$SEED_TOOL"
if [ -x "$SEED_TOOL" ]; then
  for seed_script in "$REPO_ROOT"/t/seed-*.sh; do
    [ -f "$seed_script" ] && bash "$seed_script"
  done
fi

# --- Run tests ---

# t/103-webbotauth-valid.t (RFC 9421 signatures) and t/104-rslcap.t (RSL-CAP
# capability tokens) mint fresh material with the sign tool. A missing tool
# must be LOUD: a silent self-skip would turn the whole valid-signature /
# enforcement e2e surface into a green no-op.
SIGN_TOOL="${SIGN_TOOL:-${REPO_ROOT}/bazel-bin/src/crypto/webbotauth/webbotauth_sign_tool}"
if [ -x "$SIGN_TOOL" ]; then
  export WEBBOTAUTH_SIGN_TOOL="$SIGN_TOOL"
elif [ "${WEBBOTAUTH_SKIP_SIGN_TESTS:-}" = "1" ]; then
  echo "WARNING: webbotauth_sign_tool not built — t/103-webbotauth-valid.t and" >&2
  echo "         t/104-rslcap.t will SKIP (WEBBOTAUTH_SKIP_SIGN_TESTS=1 set)." >&2
else
  echo "Error: webbotauth_sign_tool not built; t/103-webbotauth-valid.t and" >&2
  echo "       t/104-rslcap.t need it." >&2
  echo "  Build it:  bazel build //src/crypto/webbotauth:webbotauth_sign_tool" >&2
  echo "  Or set WEBBOTAUTH_SKIP_SIGN_TESTS=1 to run without it." >&2
  exit 1
fi

export TEST_NGINX_LOAD_MODULES="$MODULE_SO"
export TEST_NGINX_LOG_LEVEL="${TEST_NGINX_LOG_LEVEL:-warn}"
export TEST_NGINX_SERVER_PORT="${TEST_NGINX_SERVER_PORT:-19840}"

cd "$REPO_ROOT"
# PROVE_OPTS is prove options only; test files are positional arguments.
# PROVE_ARGS (older name) holds the whole prove invocation.
rc=0
if [ -z "${PROVE_OPTS:-}" ] && [ -n "${PROVE_ARGS:-}" ]; then
  # shellcheck disable=SC2086
  prove $PROVE_ARGS || rc=$?
elif [ $# -gt 0 ]; then
  prove ${PROVE_OPTS:--v} "$@" || rc=$?
else
  prove ${PROVE_OPTS:--v -r} t/ || rc=$?
fi

# On failure, dump error log for diagnostics
if [ $rc -ne 0 ] && [ -f t/servroot/logs/error.log ]; then
    echo ""
    echo "=== nginx error.log (last 50 lines) ==="
    tail -50 t/servroot/logs/error.log
fi

exit $rc
