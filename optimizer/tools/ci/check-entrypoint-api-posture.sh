#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Management-API posture gate for container entrypoints.
#
# The daemon refuses to start when the management API is bound to a
# non-loopback address without a token, and it treats a token shorter than 16
# characters as unset. docker/entrypoint-worker.sh is the contract; every other
# entrypoint in the tree is a copy of it, and copies drift. When they drift the
# failure is a container that exits during config parse, in a periodic job, on
# a runner nobody is watching -- which is exactly how a 14-character
# `e2e-test-token` reached a release candidate.
#
# Two checks, both static:
#
#   1. Any entrypoint that configures a NON-LOOPBACK management-API bind must
#      also carry the remote opt-in AND a token default of at least 16
#      characters -- the same pairing the daemon insists on at startup.
#   2. Any PAGESPEED_API_TOKEN fallback literal ANYWHERE in the tree is at
#      least 16 characters. Entrypoints and their test clients keep their
#      defaults in lockstep by hand, so a short literal in a conftest is the
#      same defect one file over.
#
# Usage:  bash tools/ci/check-entrypoint-api-posture.sh [--self-test]
# Exit:   0 all clear, 1 a violation was found (message names file + remedy).

set -uo pipefail

MIN_TOKEN_LEN=16

# --- helpers ----------------------------------------------------------------

# Does this file configure a management API port at all?  Nothing else in here
# applies to a worker that never opens one (tools/stress, for instance).
enables_api() {
  grep -qE '(^|[^-])--api-port|PAGESPEED_API_PORT=' "$1"
}

# A bind value that is not loopback.  Matches both spellings a copy can use:
# the flag (`--api-bind 0.0.0.0`) and the env var (`PAGESPEED_API_BIND=0.0.0.0`).
# 127.0.0.0/8, ::1 and localhost are loopback; anything else (0.0.0.0, ::, a
# concrete LAN address) is remote as far as the daemon is concerned.
nonloopback_bind() {
  grep -oE -- '--api-bind[= ]+[^ \\"'"'"']+|PAGESPEED_API_BIND=[^ \\"'"'"']+' "$1" \
    | sed -E 's/.*[= ]//' \
    | grep -qvE '^(127\.[0-9.]+|::1|\[::1\]|localhost)$'
}

has_allow_remote() {
  grep -qE -- '--api-allow-remote|PAGESPEED_API_ALLOW_REMOTE=true' "$1"
}

# The token default this file would hand the daemon, i.e. the literal in
# `PAGESPEED_API_TOKEN="${PAGESPEED_API_TOKEN:-<literal>}"`.  Empty when the
# file only passes an externally supplied value through.
token_default() {
  grep -oE 'PAGESPEED_API_TOKEN:-[^}"'"'"']*' "$1" | sed -E 's/^PAGESPEED_API_TOKEN:-//' | head -1
}

# --- check 1: entrypoint bind/token posture ---------------------------------

check_entrypoints() {
  local root="$1" rc=0 f
  while IFS= read -r f; do
    enables_api "$f" || continue
    nonloopback_bind "$f" || continue

    if ! has_allow_remote "$f"; then
      echo "ERROR: $f binds the management API off loopback without the remote opt-in." >&2
      echo "       The daemon refuses to start in that state. Add --api-allow-remote" >&2
      echo "       (or PAGESPEED_API_ALLOW_REMOTE=true), or bind 127.0.0.1." >&2
      rc=1
    fi

    local tok
    tok="$(token_default "$f")"
    if [ -z "$tok" ]; then
      echo "ERROR: $f binds the management API off loopback with no token default." >&2
      echo "       A remotely reachable management API is never allowed to be" >&2
      echo "       unauthenticated. Set PAGESPEED_API_TOKEN, or bind 127.0.0.1." >&2
      rc=1
    elif [ "${#tok}" -lt "$MIN_TOKEN_LEN" ]; then
      echo "ERROR: $f defaults PAGESPEED_API_TOKEN to a ${#tok}-character value" >&2
      echo "       ('$tok'); the daemon requires at least $MIN_TOKEN_LEN and treats" >&2
      echo "       anything shorter as unset -- which then fails the non-loopback" >&2
      echo "       bind and kills the container at startup. Lengthen the literal" >&2
      echo "       (and any test client that mirrors it)." >&2
      rc=1
    fi
  done < <(find "$root" -name 'entrypoint*.sh' -type f -not -path '*/node_modules/*' | sort)
  return $rc
}

# --- check 2: every token fallback literal in the tree ----------------------

check_token_literals() {
  local root="$1" rc=0 line file tok
  while IFS= read -r line; do
    file="${line%%:*}"
    tok="$(printf '%s' "${line#*:}" | grep -oE 'PAGESPEED_API_TOKEN(:-|", ")[^}"'"'"']*' \
           | sed -E 's/^PAGESPEED_API_TOKEN(:-|", ")//' | head -1)"
    [ -n "$tok" ] || continue
    if [ "${#tok}" -lt "$MIN_TOKEN_LEN" ]; then
      echo "ERROR: $file falls back to a ${#tok}-character PAGESPEED_API_TOKEN ('$tok')." >&2
      echo "       The daemon treats anything under $MIN_TOKEN_LEN characters as unset." >&2
      rc=1
    fi
  done < <(grep -rnE 'PAGESPEED_API_TOKEN(:-|", ")[^}"'"'"']' "$root" \
             --include='*.sh' --include='*.py' --include='*.yml' --include='*.yaml' \
             --include='*.ts' --include='*.conf' \
             --exclude-dir=node_modules --exclude-dir=.git \
             --exclude="$(basename "${BASH_SOURCE[0]}")" 2>/dev/null)
  return $rc
}

# --- self-test --------------------------------------------------------------
# A gate whose own logic has rotted reports success on every PR, which is the
# failure mode it exists to prevent. Build both a violating and a conforming
# fixture and assert the verdicts.

self_test() {
  local tmp rc=0
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN

  mkdir -p "$tmp/bad" "$tmp/good"

  cat > "$tmp/bad/entrypoint-short-token.sh" <<'EOF'
export PAGESPEED_API_PORT=9881
export PAGESPEED_API_BIND=0.0.0.0
export PAGESPEED_API_ALLOW_REMOTE=true
export PAGESPEED_API_TOKEN="${PAGESPEED_API_TOKEN:-too-short}"
EOF
  cat > "$tmp/bad/entrypoint-no-optin.sh" <<'EOF'
exec factory_worker --api-port 9882 --api-bind 0.0.0.0
export PAGESPEED_API_TOKEN="${PAGESPEED_API_TOKEN:-a-long-enough-token}"
EOF
  cat > "$tmp/good/entrypoint-loopback.sh" <<'EOF'
export PAGESPEED_API_PORT=9881
export PAGESPEED_API_BIND=127.0.0.1
EOF
  cat > "$tmp/good/entrypoint-remote-ok.sh" <<'EOF'
export PAGESPEED_API_PORT=9881
export PAGESPEED_API_BIND=0.0.0.0
export PAGESPEED_API_ALLOW_REMOTE=true
export PAGESPEED_API_TOKEN="${PAGESPEED_API_TOKEN:-a-sufficiently-long-token}"
EOF
  cat > "$tmp/good/entrypoint-no-api.sh" <<'EOF'
exec factory_worker --socket /shared/pagespeed.sock --api-bind-nothing
EOF

  if check_entrypoints "$tmp/bad" >/dev/null 2>&1; then
    echo "SELF-TEST FAIL: violating fixtures were accepted" >&2; rc=1
  fi
  if ! check_entrypoints "$tmp/good" 2>&1; then
    echo "SELF-TEST FAIL: conforming fixtures were rejected" >&2; rc=1
  fi
  if check_token_literals "$tmp/bad" >/dev/null 2>&1; then
    echo "SELF-TEST FAIL: short token literal was accepted" >&2; rc=1
  fi
  if ! check_token_literals "$tmp/good" 2>&1; then
    echo "SELF-TEST FAIL: long token literal was rejected" >&2; rc=1
  fi

  [ $rc -eq 0 ] && echo "check-entrypoint-api-posture.sh: self-test OK"
  return $rc
}

# --- main -------------------------------------------------------------------

if [ "${1:-}" = "--self-test" ]; then
  self_test
  exit $?
fi

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
status=0
check_entrypoints "$ROOT" || status=1
check_token_literals "$ROOT" || status=1
if [ $status -eq 0 ]; then
  echo "check-entrypoint-api-posture.sh: all entrypoints conform to the management-API posture"
fi
exit $status
