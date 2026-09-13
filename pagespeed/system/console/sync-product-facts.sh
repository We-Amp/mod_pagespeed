#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Sync the product-facts single source of truth into the console tree.
#
# The console chrome (title, logo line, footer/legal links) derives from the
# canonical product-facts.mjs so a product rename or URL change is a one-file
# edit upstream. The canonical file is shared/product-facts.mjs at the root of
# the pagespeed-optimizer repo — the identity-only, pricing-free module the website
# itself re-exports from; this script copies it VERBATIM to
# src/lib/data/product-facts.mjs, which is checked in (the console builds
# hermetically, without the pagespeed-optimizer repo). A verbatim copy keeps the
# drift guard a plain byte compare -- see check-product-facts.sh.
#
# Source resolution, first hit wins:
#   1. $PRODUCT_FACTS_SOURCE            (explicit path to the canonical file)
#   2. $MODPAGESPEED2_DIR/shared/product-facts.mjs
#   3. a "pagespeed-optimizer" checkout next to this repository checkout
#   4. the origin/main blob of that checkout (when its working tree predates
#      the file)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

FACTS_REL="shared/product-facts.mjs"
COPY="src/lib/data/product-facts.mjs"

# Print the canonical file's content on stdout; exit 1 when no source found.
read_source() {
  if [ -n "${PRODUCT_FACTS_SOURCE:-}" ]; then
    if [ -f "$PRODUCT_FACTS_SOURCE" ]; then
      cat "$PRODUCT_FACTS_SOURCE"
      return 0
    fi
    echo "error: PRODUCT_FACTS_SOURCE is set but not readable: $PRODUCT_FACTS_SOURCE" >&2
    return 1
  fi
  local dir="${MODPAGESPEED2_DIR:-$SCRIPT_DIR/../../../../pagespeed-optimizer}"
  if [ -f "$dir/$FACTS_REL" ]; then
    cat "$dir/$FACTS_REL"
    return 0
  fi
  # A checkout whose working tree predates the facts file still carries it as
  # a blob on origin/main; the guard should work there too.
  if [ -d "$dir" ] && git -C "$dir" rev-parse --git-dir >/dev/null 2>&1; then
    if git -C "$dir" cat-file -e "origin/main:$FACTS_REL" 2>/dev/null; then
      git -C "$dir" show "origin/main:$FACTS_REL"
      return 0
    fi
  fi
  return 1
}

if ! content="$(read_source)"; then
  echo "error: canonical product-facts.mjs not found." >&2
  echo "Set PRODUCT_FACTS_SOURCE to the canonical file, or MODPAGESPEED2_DIR" >&2
  echo "to a pagespeed-optimizer checkout." >&2
  exit 1
fi

if [ -f "$COPY" ] && [ "$content" = "$(cat "$COPY")" ]; then
  echo "OK: $COPY already in sync."
  exit 0
fi

printf '%s\n' "$content" > "$COPY"
echo "Synced $FACTS_REL -> $COPY"
echo "Rebuild the console (build.sh) so the bundle picks up the new facts."
