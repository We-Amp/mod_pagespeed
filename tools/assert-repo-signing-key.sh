#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# assert-repo-signing-key.sh — guard that the apt/rpm installer
# fragments embed a real repository signing key, not a TODO or an empty
# placeholder block. The .deb/.rpm repos set gpgcheck=1, so an empty key block
# silently breaks signature verification for end users.
#
# Asserts, for install/common/apt.include and install/common/rpm.include:
#   1. No "TODO ... signing key" left behind.
#   2. No EMPTY armored block (a BEGIN line immediately followed by END).
#   3. At least one substantial PGP PUBLIC KEY BLOCK (>= 10 body lines).
#   4. If gpg is available: the embedded key parses and carries the expected
#      key id F50D6054F10712A0.
#
# Needs only POSIX shell + awk (gpg optional). Run from the repo root.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
EXPECT_KEYID="F50D6054F10712A0"
RC=0

for f in install/common/apt.include install/common/rpm.include; do
  [ -f "$f" ] || { echo "FAIL: $f missing" >&2; RC=1; continue; }

  if grep -qiE 'TODO.*signing key' "$f"; then
    echo "FAIL: $f still has a 'TODO ... signing key' placeholder." >&2
    RC=1
  fi

  # An empty block = BEGIN line directly followed by END line.
  if awk '
      /-----BEGIN PGP PUBLIC KEY BLOCK-----/ { prev=NR; next }
      /-----END PGP PUBLIC KEY BLOCK-----/   { if (prev==NR-1) { found=1 } }
      END { exit(found?0:1) }' "$f"; then
    echo "FAIL: $f contains an EMPTY PGP key block (BEGIN immediately followed by END)." >&2
    RC=1
  fi

  # Count body lines inside the largest key block.
  BODY=$(awk '
      /-----BEGIN PGP PUBLIC KEY BLOCK-----/ { inb=1; n=0; next }
      /-----END PGP PUBLIC KEY BLOCK-----/   { if (n>max) max=n; inb=0; next }
      inb { if (length($0)>0) n++ }
      END { print max+0 }' "$f")
  if [ "${BODY:-0}" -lt 10 ]; then
    echo "FAIL: $f has no substantial key block (largest body = ${BODY} lines)." >&2
    RC=1
  else
    echo "OK: $f embeds a key block (${BODY} body lines)."
  fi

  # Optional cryptographic check.
  if command -v gpg >/dev/null 2>&1; then
    KEYID=$(awk '/-----BEGIN PGP PUBLIC KEY BLOCK-----/{p=1} p{print} /-----END PGP PUBLIC KEY BLOCK-----/{p=0}' "$f" \
      | gpg --show-keys --with-colons 2>/dev/null | awk -F: '/^pub:/{print $5; exit}')
    if [ "${KEYID}" = "${EXPECT_KEYID}" ]; then
      echo "OK: $f key parses to ${EXPECT_KEYID}."
    else
      echo "FAIL: $f key id '${KEYID:-<unparseable>}' != expected ${EXPECT_KEYID}." >&2
      RC=1
    fi
  fi
done

if [ "${RC}" -ne 0 ]; then
  echo "=== repo-signing-key assertion FAILED ===" >&2
  exit 1
fi
echo "=== repo-signing-key assertion PASSED ==="
