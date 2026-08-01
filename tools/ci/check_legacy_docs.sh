#!/bin/sh

# CI legacy-documentation guard (BLOCKING): html/ is an archive, not a doc site.
#
# `html/` holds the mod_pagespeed 1.0 documentation as it shipped, and it is
# published unchanged as the /1.0/ archive on modpagespeed.com. Current
# documentation lives in the pagespeed-optimizer repository — 2.0 under /docs/, 1.15
# under /1.1/docs/.
#
# The failure this prevents is specific and has already happened. Someone
# changes product behaviour, greps the tree for the prose describing the old
# behaviour, finds it in `html/doc/system.html`, and updates it there. The edit
# looks right in review: the sentence really did describe the behaviour, and it
# really was out of date. But nothing published to a user changed, the actual
# documentation still says the old thing, and the archive has quietly stopped
# being a record of what 1.0 did.
#
# `html/CLAUDE.md` has said "legacy — do not invest effort" since 2023 and did
# not prevent it, because a grep lands inside a file, not at the top of a
# directory. So the notice now sits in every file, and this gate stops the
# commit.
#
# Run locally:  sh tools/ci/check_legacy_docs.sh --base origin/master --head HEAD
# Self-test:    sh tools/ci/check_legacy_docs.sh --self-test

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# The archive. Anything under here is frozen.
ARCHIVE_RE='^html/'

# Deliberate exceptions live here, not in the caller's head: the pointer files
# are how someone finds out where the real documentation is, so keeping them
# accurate is the opposite of the drift this guard exists to stop.
ARCHIVE_EXEMPT_RE='^html/CLAUDE\.md$|^html/README(\.md)?$'

# The waiver, with a mandatory reason — same shape as the release-note gate, so
# there is one convention to remember rather than two.
WAIVER_RE='^[[:space:]]*Legacy-Docs:[[:space:]]*'
WAIVER_MIN_REASON=12

evaluate_legacy_docs() {
  changed="$1"
  haystack="$2"

  triggering="$(printf '%s\n' "$changed" \
    | grep -E "$ARCHIVE_RE" \
    | grep -vE "$ARCHIVE_EXEMPT_RE" \
    || true)"

  if [ -z "$triggering" ]; then
    echo "OK: the html/ archive is untouched."
    return 0
  fi

  waiver_line="$(printf '%s\n' "$haystack" | grep -iE "$WAIVER_RE" | head -n 1 || true)"
  if [ -n "$waiver_line" ]; then
    reason="$(printf '%s\n' "$waiver_line" \
      | sed -E "s/$WAIVER_RE//I" \
      | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
    if [ "${#reason}" -lt "$WAIVER_MIN_REASON" ]; then
      echo "" >&2
      echo "FAILED: the html/ archive waiver has no usable reason." >&2
      echo "  found:  $waiver_line" >&2
      echo "At least $WAIVER_MIN_REASON characters saying why the archive must change." >&2
      return 1
    fi
    echo "OK: html/ archive change accepted — $reason"
    return 0
  fi

  echo "" >&2
  echo "FAILED: this change edits the html/ archive." >&2
  echo "" >&2
  printf '%s\n' "$triggering" | sed 's/^/  /' >&2
  echo "" >&2
  echo "html/ is the mod_pagespeed 1.0 documentation as it shipped, published" >&2
  echo "unchanged as the /1.0/ archive. It describes what that release did, so" >&2
  echo "it does not get updated when behaviour changes — an archive that tracks" >&2
  echo "the current product is a record of nothing." >&2
  echo "" >&2
  echo "Current documentation is in the pagespeed-optimizer repository:" >&2
  echo "" >&2
  echo "  2.0    website/src/content/docs/         -> modpagespeed.com/docs/" >&2
  echo "  1.15   website/src/content/docs-1.1/     -> modpagespeed.com/1.1/docs/" >&2
  echo "" >&2
  echo "If you came here because product behaviour changed, that is where the" >&2
  echo "change belongs." >&2
  echo "" >&2
  echo "To edit the archive itself — fixing a broken link, or retiring it —" >&2
  echo "say so in a commit message:" >&2
  echo "" >&2
  echo "  Legacy-Docs: <why the archive itself has to change>" >&2
  echo "" >&2
  return 1
}

self_test() {
  failures=0
  n=0

  check() {
    name="$1"; want="$2"; changed="$3"; haystack="$4"
    got=0
    n=$((n + 1))
    evaluate_legacy_docs "$changed" "$haystack" >/dev/null 2>&1 || got=$?
    if [ "$got" -ne "$want" ]; then
      echo "  FAIL  $name (wanted exit $want, got $got)" >&2
      failures=$((failures + 1))
    else
      echo "  ok    $name"
    fi
  }

  echo "Self-test: legacy-documentation guard"

  check "the exact edit this prevents      -> BLOCK" 1 \
    "$(printf 'html/doc/system.html\n')" ""
  check "archive image                     -> BLOCK" 1 \
    "$(printf 'html/doc/images/console.png\n')" ""
  check "archive root file                 -> BLOCK" 1 \
    "$(printf 'html/index.html\n')" ""
  check "product code only                 -> pass " 0 \
    "$(printf 'pagespeed/system/system_rewrite_driver_factory.cc\n')" ""
  check "nothing changed                   -> pass " 0 "" ""
  check "html/CLAUDE.md is exempt          -> pass " 0 \
    "$(printf 'html/CLAUDE.md\n')" ""
  check "exempt file + a real archive edit -> BLOCK" 1 \
    "$(printf 'html/CLAUDE.md\nhtml/doc/filters.html\n')" ""
  check "waiver in a commit message        -> pass " 0 \
    "$(printf 'html/doc/system.html\n')" \
    "$(printf 'chore: fix a dead link\n\nLegacy-Docs: link rot, target moved; content unchanged\n')" ""
  check "waiver with no reason             -> BLOCK" 1 \
    "$(printf 'html/doc/system.html\n')" "Legacy-Docs:"
  check "waiver with a token reason        -> BLOCK" 1 \
    "$(printf 'html/doc/system.html\n')" "Legacy-Docs: wip"
  check "a mention is not a waiver         -> BLOCK" 1 \
    "$(printf 'html/doc/system.html\n')" \
    "I considered writing Legacy-Docs: but did not"
  # A path that merely contains "html" must not be swept up: the trigger is the
  # html/ directory at the repo root, not the string.
  check "unrelated .html elsewhere         -> pass " 0 \
    "$(printf 'install/mod_pagespeed_example/rewrite_css.html\n')" ""
  check "a path ending in html/            -> pass " 0 \
    "$(printf 'pagespeed/kernel/html/html_parse.cc\n')" ""

  self="$0"
  tmp="$(mktemp -d)"

  e2e() {
    name="$1"; want="$2"; shift 2
    got=0
    n=$((n + 1))
    sh "$self" "$@" >/dev/null 2>&1 || got=$?
    if [ "$got" -ne "$want" ]; then
      echo "  FAIL  $name (wanted exit $want, got $got)" >&2
      failures=$((failures + 1))
    else
      echo "  ok    $name"
    fi
  }

  printf 'html/doc/system.html\n' > "$tmp/files-block.txt"
  printf 'Legacy-Docs: retiring the archive, tracked in an issue\n' > "$tmp/waiver.txt"

  e2e "--files-from missing              -> BLOCK" 1 --files-from "$tmp/nope.txt"
  e2e "--haystack-from missing           -> BLOCK" 1 \
    --files-from "$tmp/files-block.txt" --haystack-from "$tmp/nope.txt"
  e2e "--files-from, archive edit        -> BLOCK" 1 --files-from "$tmp/files-block.txt"
  e2e "--files-from + waiver file        -> pass " 0 \
    --files-from "$tmp/files-block.txt" --haystack-from "$tmp/waiver.txt"
  e2e "no arguments at all               -> BLOCK" 1
  e2e "unresolvable base                 -> BLOCK" 1 \
    --base 0000000000000000000000000000000000000000

  rm -rf "$tmp"

  echo ""
  if [ "$failures" -ne 0 ]; then
    echo "FAILED: $failures of $n self-test cases did not behave as specified." >&2
    return 1
  fi
  echo "OK: all $n self-test cases behave as specified."
  return 0
}

BASE=""
HEAD="HEAD"
FILES_FROM=""
HAYSTACK_FROM=""

while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) self_test; exit $? ;;
    --base) BASE="${2:-}"; shift 2 ;;
    --head) HEAD="${2:-}"; shift 2 ;;
    --files-from) FILES_FROM="${2:-}"; shift 2 ;;
    --haystack-from) HAYSTACK_FROM="${2:-}"; shift 2 ;;
    -h|--help) sed -n '3,24p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

cd "$REPO_ROOT"

if [ -n "$FILES_FROM" ]; then
  if [ ! -f "$FILES_FROM" ]; then
    echo "FAILED: --files-from '$FILES_FROM' does not exist." >&2
    echo "This check cannot run, so it is failing rather than passing silently." >&2
    exit 1
  fi
  HAYSTACK=""
  if [ -n "$HAYSTACK_FROM" ]; then
    if [ ! -f "$HAYSTACK_FROM" ]; then
      echo "FAILED: --haystack-from '$HAYSTACK_FROM' does not exist." >&2
      exit 1
    fi
    HAYSTACK="$(cat "$HAYSTACK_FROM")"
  fi
  evaluate_legacy_docs "$(cat "$FILES_FROM")" "$HAYSTACK"
  exit $?
fi

if [ -z "$BASE" ]; then
  echo "FAILED: --base is required (or pass --self-test / --files-from)." >&2
  exit 1
fi

# Fail closed: a guard that cannot see the diff must not report success.
if ! MERGE_BASE="$(git merge-base "$BASE" "$HEAD" 2>/dev/null)" || [ -z "$MERGE_BASE" ]; then
  echo "FAILED: cannot resolve a merge base between '$BASE' and '$HEAD'." >&2
  echo "This check cannot run, so it is failing rather than passing silently." >&2
  exit 1
fi

CHANGED="$(git diff --name-only "$MERGE_BASE" "$HEAD")"
MESSAGES="$(git log --format='%B' "$MERGE_BASE..$HEAD")"
evaluate_legacy_docs "$CHANGED" "$(printf '%s\n%s\n' "$MESSAGES" "${PR_BODY:-}")"
