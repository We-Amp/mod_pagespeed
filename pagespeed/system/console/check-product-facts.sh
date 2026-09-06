#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Drift-guard: verify the checked-in src/lib/data/product-facts.mjs copy is in
# sync with the canonical product-facts single source (see sync-product-facts.sh
# for what it is and why it is copied verbatim).
#
# The canonical file lives OUTSIDE this repository, so the guard is only as
# strong as the source it can reach:
#   - source found    -> byte-compare, fail on drift (run sync-product-facts.sh
#                        and rebuild the console bundle)
#   - source not found (CI job that checks out only this repo, no sibling
#                        checkout) -> skip with a notice; the checked-in copy
#                        still feeds the admin_console.html srchash guard, so a
#                        facts edit without a bundle rebuild stays loud.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

FACTS_REL="website/src/data/product-facts.mjs"
COPY="src/lib/data/product-facts.mjs"

# Same resolution order as sync-product-facts.sh (keep in sync).
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
  if [ -d "$dir" ] && git -C "$dir" rev-parse --git-dir >/dev/null 2>&1; then
    if git -C "$dir" cat-file -e "origin/main:$FACTS_REL" 2>/dev/null; then
      git -C "$dir" show "origin/main:$FACTS_REL"
      return 0
    fi
  fi
  return 1
}

if [ ! -f "$COPY" ]; then
  echo "::error::$COPY is missing. Run 'pagespeed/system/console/sync-product-facts.sh' and commit it."
  exit 1
fi

if ! content="$(read_source)"; then
  echo "NOTICE: canonical product-facts.mjs not reachable from this checkout;"
  echo "skipping upstream drift check ($COPY is present)."
  exit 0
fi

if [ "$content" != "$(cat "$COPY")" ]; then
  echo "::error::$COPY and the canonical $FACTS_REL differ."
  echo "If the canonical file is newer, run"
  echo "'pagespeed/system/console/sync-product-facts.sh', rebuild the console"
  echo "(build.sh), and commit both the copy and the rebuilt bundle. If the"
  echo "checked-in copy is AHEAD (e.g. it was synced from a not-yet-merged"
  echo "upstream facts branch), do NOT sync — that would downgrade the copy;"
  echo "the upstream change must merge first."
  exit 1
fi

echo "OK: $COPY matches the canonical product facts."
