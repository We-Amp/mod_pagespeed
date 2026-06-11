#!/bin/bash
# sync-crypto.sh - Vendor the shared Ed25519 license crypto from ModPageSpeed 2.0.
#
# mod_pagespeed 1.1 does NOT own its license crypto. The token wire format, the
# Ed25519 verifier, and the production public keys are a SINGLE source of truth
# shared with ModPageSpeed 2.0 so that one customer license verifies
# byte-identically on both products.
#
# Historically 1.1 consumed 2.0 as a Bazel git_repository (@modpagespeed2) and
# adapted src/crypto/ at build time via a genrule. To remove that build-time
# dependency on the 2.0 repository, the six crypto files are now
# VENDORED (checked in) under pagespeed/kernel/license_v2/. This script is the
# canonical regeneration path; the crypto-drift CI check runs it
# and fails if the checked-in files would change, so the vendored copy can never
# silently drift from the canonical 2.0 source.
#
# Usage:
#   tools/sync-crypto.sh <path-to-pagespeed-optimizer-git-checkout>
#
# Regenerates the six vendored files from the 2.0 checkout at PINNED_MPS2_COMMIT.
# Run this after bumping PINNED_MPS2_COMMIT (e.g. a key rotation, a verifier
# security fix, or a new entitlement) and commit the result.

set -euo pipefail

# Canonical ModPageSpeed 2.0 commit the vendored crypto is synced from.
# Bump this (and re-run the script) whenever 2.0's shared crypto changes.
#
# Points at the 2.0 main commit carrying the additive scope/domain payload
# fields. the design record entitlements[] claim this previously
# gated on has merged to 2.0 main.
PINNED_MPS2_COMMIT="9621667d9af6cd397deff98aa3e2ea1e1677c812"

SRC_REPO="${1:-}"
if [[ -z "$SRC_REPO" ]]; then
  echo "Usage: $0 <path-to-pagespeed-optimizer-git-checkout>" >&2
  exit 2
fi

if ! git -C "$SRC_REPO" cat-file -e "${PINNED_MPS2_COMMIT}^{commit}" 2>/dev/null; then
  echo "ERROR: commit ${PINNED_MPS2_COMMIT} not reachable in '${SRC_REPO}'." >&2
  echo "       Fetch the pagespeed-optimizer branch that carries it, or update" >&2
  echo "       PINNED_MPS2_COMMIT in this script." >&2
  exit 3
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST="${SCRIPT_DIR}/../pagespeed/kernel/license_v2"

# The six files exported by @modpagespeed2//src/crypto:BUILD (exports_files).
FILES=(
  license_token.h
  license_token.cc
  license_verifier.h
  license_verifier.cc
  license_signer.h
  license_signer.cc
)

short="${PINNED_MPS2_COMMIT:0:12}"

for f in "${FILES[@]}"; do
  {
    cat <<EOF
// GENERATED — DO NOT EDIT BY HAND.
// Vendored from ModPageSpeed 2.0 src/crypto/${f} by tools/sync-crypto.sh.
// Canonical source: github.com/We-Amp/pagespeed-optimizer src/crypto/.
// Synced from commit ${PINNED_MPS2_COMMIT}.
// To update: bump PINNED_MPS2_COMMIT in tools/sync-crypto.sh and re-run it.
// Drift guard: the crypto-drift CI check. See the design record.

EOF
    # Rewrite only the cross-repo include paths — identical to the historical
    # genrule transform, so the compiled bytes are unchanged.
    git -C "$SRC_REPO" show "${PINNED_MPS2_COMMIT}:src/crypto/${f}" \
      | sed -e 's|#include "src/crypto/|#include "pagespeed/kernel/license_v2/|g' \
            -e 's|#include "lib/base/string_util.h"|#include "pagespeed/kernel/base/string_util.h"|g'
  } > "${DEST}/${f}"
  echo "  synced ${f}"
done

echo "Done. Vendored ${#FILES[@]} crypto files into pagespeed/kernel/license_v2/ from ${short}."
