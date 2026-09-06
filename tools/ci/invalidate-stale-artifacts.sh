#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Ported from pagespeed-optimizer tools/ci/ (corp the design record hub) — keep the copies in sync.
# Invalidate a local CI artifacts staging dir if its metadata.json reports a
# different commit SHA than the one expected by the caller. Protects against
# concurrent-PR contamination on dedicated shared runners: when two PRs run
# back-to-back, the later Build overwrites the earlier Build's
# .ci-artifacts/, and a Pkg Smoke rerun of the earlier PR would otherwise
# read the other PR's binary.
#
# Usage: invalidate-stale-artifacts.sh <staging-dir> <expected-sha>
#
# Exits 0 always. If metadata.json is present and its "commit" field does
# not match <expected-sha>, the entire staging dir is wiped so the caller's
# existence check triggers a fresh fetch from SHA-scoped shared storage.
set -euo pipefail

STAGING="${1:-}"
EXPECTED="${2:-}"
if [ -z "$STAGING" ] || [ -z "$EXPECTED" ]; then
  echo "usage: $0 <staging-dir> <expected-sha>" >&2
  exit 2
fi

META="$STAGING/metadata.json"
[ -f "$META" ] || exit 0

# Extract the commit SHA from metadata.json. grep+sed (vs a single
# sed with greedy ".*") ensures the FIRST "commit":"..." occurrence
# wins, and the [[:space:]]* between key and value tolerates
# pretty-printed JSON. Kept bash-only to avoid a jq dependency.
# `|| true` swallows grep's exit-1 on no-match so `set -o pipefail`
# doesn't abort — an absent key should fall through as empty $LOCAL_SHA
# and be treated as mismatch below.
LOCAL_SHA=$({ grep -oE '"commit"[[:space:]]*:[[:space:]]*"[^"]*"' "$META" || true; } | head -1 | sed 's/.*"\([^"]*\)"$/\1/')
if [ "$LOCAL_SHA" != "$EXPECTED" ]; then
  echo "::warning::Local $STAGING is for ${LOCAL_SHA:-<unknown>}, expected $EXPECTED -- invalidating"
  rm -rf "$STAGING"
fi
