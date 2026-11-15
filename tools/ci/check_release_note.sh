#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI release-note gate (BLOCKING): user-facing changes must carry a release note.
#
# A change that alters what an operator or a visitor observes, and ships without
# a line in CHANGELOG.md, is invisible. Nobody upgrading learns that the thing
# they were working around is fixed, and nobody debugging learns that behaviour
# moved. The failure is silent by construction: nothing goes red, the release
# simply under-reports itself, and the omission is only ever noticed later by
# someone who needed the note.
#
# So this gate asks one question of every pull request: you touched product
# code — where is the note? It is deliberately easy to satisfy and deliberately
# impossible to ignore. A change with no user-visible effect declares that in
# one line and moves on; what it cannot do is say nothing at all.
#
# Run locally:  bash tools/ci/check_release_note.sh --base origin/main --head HEAD
# Self-test:    bash tools/ci/check_release_note.sh --self-test
#
# In CI the inputs come from the GitHub API instead of git, so the check works
# on the shallow checkout the rest of the job already uses:
#   bash tools/ci/check_release_note.sh --files-from FILE --haystack-from FILE \
#        [--changelog-added-from FILE] [--fragments-from FILE] [--head-ref BRANCH]
# where --changelog-added-from lists the lines the change adds to CHANGELOG.md
# and to changelog.d/ fragments (so a security entry hidden in a *.fixed.md is
# seen too), and --fragments-from lists the changelog.d/ files the change ADDS
# OR MODIFIES (a deleted fragment is not a note). Without --fragments-from no
# fragment counts.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------

# The file that carries operator-facing notes.
RELEASE_NOTE_FILE="CHANGELOG.md"

# Product code: changing it can change what someone observes.
USER_FACING_RE='^(src|lib)/'

# Carve-outs inside the product tree that cannot reach a user on their own.
# Tests and build plumbing change no shipped behaviour by themselves; a change
# that alters both a test and the code it covers still trips the gate on the
# code half, which is the intent.
EXEMPT_RE='(^|/)(test|tests|testdata)/|(^|/)[^/]*_test\.(cc|cpp|h|hpp|ts|js)$|(^|/)BUILD(\.bazel)?$|\.md$'

# The waiver. A reason is mandatory: the point is to record the judgement, not
# to provide a mute button. Accepts `-`, `:`, an en dash or an em dash as the
# separator, because people type all four.
WAIVER_RE='^[[:space:]]*Release-Note:[[:space:]]*none[[:space:]]*[-:–—]+[[:space:]]*'
WAIVER_MIN_REASON=12

# Security entries. A CHANGELOG.md line that opens a security entry is published
# text about a fix, so it lands only together with the release that ships the
# fix. A pull request that adds one is refused unless it is a release landing
# (a landing/ head branch AND `Release-Note: landing -- <why>`) or carries
# `Release-Note: security-public -- <why>` (same separators and minimum reason
# as the waiver). Renaming the Unreleased heading at the release cut adds no
# such line, so the cut is unaffected.
SECURITY_LINE_RE='^[[:space:]]*(([-*][[:space:]]+)?(\*\*)?security\b[^:]{0,40}:|#+[[:space:]]+(\*\*)?security\b)'
LANDING_RE='^[[:space:]]*Release-Note:[[:space:]]*landing[[:space:]]*[-:–—]+[[:space:]]*'
LANDING_BRANCH_RE='^landing/'
SECPUB_RE='^[[:space:]]*Release-Note:[[:space:]]*security-public[[:space:]]*[-:–—]+[[:space:]]*'

# ---------------------------------------------------------------------------
# Core decision, kept pure so the self-test can drive every branch
# ---------------------------------------------------------------------------
#
# $1 — newline-separated changed-file list
# $2 — waiver haystack (commit messages and/or PR body)
# $3 — lines the change adds to CHANGELOG.md (optional)
# $4 — the pull request's head branch (optional)
# $5 — changelog.d/ files the change adds or modifies (optional)
#
# 0 = pass, 1 = block.
evaluate_release_note() {
  local changed="$1"
  local haystack="$2"
  local sec_added="${3:-}" head_ref="${4:-}" frags_in="${5:-}" frags
  local triggering note_touched waiver_line reason sec_lines sec_frag sec_ok="" why

  sec_lines="$(printf '%s\n' "$sec_added" | grep -iE "$SECURITY_LINE_RE" || true)"
  # Only fragments the change adds or modifies count: deleting one (as the
  # release cut does) is neither a note nor a security entry.
  frags="$(printf '%s\n' "$frags_in" | grep -E '^changelog\.d/[^/]+\.md$' | grep -vFx 'changelog.d/README.md' || true)"
  # A *.security.md fragment is a security entry by its name.
  sec_frag="$(printf '%s\n' "$frags" | grep -E '^changelog\.d/[^/]+\.security\.md$' || true)"
  [ -n "$sec_frag" ] && sec_lines="$(printf '%s\n%s\n' "$sec_lines" "$sec_frag" | grep -v '^$')"
  if [ -n "$sec_lines" ]; then
    if printf '%s\n' "$head_ref" | grep -qE "$LANDING_BRANCH_RE"; then
      why="$(printf '%s\n' "$haystack" | grep -iE "$LANDING_RE" | head -n 1 \
        | sed -E "s/$LANDING_RE//I; s/[[:space:]]+/ /g; s/^[[:space:]]+//; s/[[:space:]]+\$//" || true)"
      [ "${#why}" -ge "$WAIVER_MIN_REASON" ] && sec_ok=yes
    fi
    why="$(printf '%s\n' "$haystack" | grep -iE "$SECPUB_RE" | head -n 1 \
      | sed -E "s/$SECPUB_RE//I; s/[[:space:]]+/ /g; s/^[[:space:]]+//; s/[[:space:]]+\$//" || true)"
    [ "${#why}" -ge "$WAIVER_MIN_REASON" ] && sec_ok=yes
    if [ -z "$sec_ok" ]; then
      echo "" >&2
      echo "FAILED: this change adds a security entry to CHANGELOG.md outside a release landing:" >&2
      printf '%s\n' "$sec_lines" | head -n 5 | cut -c1-100 | sed 's/^/  /' >&2
      echo "" >&2
      echo "Security changelog entries land only with the release that ships the fix:" >&2
      echo "from a landing/<version> branch with 'Release-Note: landing -- <why>', or with" >&2
      echo "'Release-Note: security-public -- <why>' (reason of at least $WAIVER_MIN_REASON characters)." >&2
      return 1
    fi
  fi

  triggering="$(printf '%s\n' "$changed" \
    | grep -E "$USER_FACING_RE" \
    | grep -vE "$EXEMPT_RE" \
    || true)"

  if [ -z "$triggering" ]; then
    echo "OK: no user-facing product code in this change — no release note required."
    return 0
  fi

  # A changelog.d/ fragment (README.md excluded) counts as the note, like an
  # edit of CHANGELOG.md. The fragment name and content are checked by
  # `python3 -I tools/changelog/assemble.py --lint`, a separate CI step.
  note_touched="$(printf '%s\n' "$changed" | grep -Fx "$RELEASE_NOTE_FILE" || true)"
  [ -z "$note_touched" ] && note_touched="$frags"
  if [ -n "$note_touched" ]; then
    echo "OK: user-facing change carries a $RELEASE_NOTE_FILE entry."
    return 0
  fi

  # A waiver reason wraps. The gate's own suggested form is a sentence, and a
  # sentence in a commit message runs onto the next line -- so read the waiver
  # line plus its continuation, stopping at a blank line or at the next trailer
  # (Co-Authored-By: and friends). Reading only the first physical line both
  # under-reports the judgement this gate exists to record and can fail the
  # minimum-length check on a reason that is in fact perfectly good.
  waiver_start="$(printf '%s\n' "$haystack" | grep -niE "$WAIVER_RE" | head -n 1 | cut -d: -f1 || true)"
  if [ -n "$waiver_start" ]; then
    waiver_line="$(printf '%s\n' "$haystack" | sed -n "${waiver_start}p")"
    reason="$(printf '%s\n' "$haystack" | awk -v s="$waiver_start" '
        NR < s  { next }
        NR == s { buf = $0; next }
        /^[[:space:]]*$/ { exit }
        /^[[:space:]]*[A-Za-z][A-Za-z-]*:[[:space:]]/ { exit }
        { buf = buf " " $0 }
        END { print buf }' | sed -E "s/$WAIVER_RE//I")"
    # Collapse the joined whitespace, then trim.
    reason="$(printf '%s' "$reason" | sed -E 's/[[:space:]]+/ /g; s/^[[:space:]]+//; s/[[:space:]]+$//')"
    if [ "${#reason}" -lt "$WAIVER_MIN_REASON" ]; then
      echo "" >&2
      echo "FAILED: the release-note waiver has no usable reason." >&2
      echo "" >&2
      echo "  found:  $waiver_line" >&2
      echo "" >&2
      echo "A waiver records a judgement, so it has to say something. Write why this" >&2
      echo "change is invisible to users — at least $WAIVER_MIN_REASON characters of actual reason." >&2
      return 1
    fi
    echo "OK: release note waived — $reason"
    return 0
  fi

  echo "" >&2
  echo "FAILED: this change touches user-facing product code but adds no release note." >&2
  echo "" >&2
  echo "Files that triggered the gate:" >&2
  printf '%s\n' "$triggering" | sed 's/^/  /' >&2
  echo "" >&2
  echo "Do one of these:" >&2
  echo "" >&2
  echo "  1. Add an entry to $RELEASE_NOTE_FILE, under the unreleased heading at the" >&2
  echo "     top. Write it for someone deciding whether to upgrade: what they observed" >&2
  echo "     before, what they observe now. See RELEASING.md." >&2
  echo "" >&2
  echo "  2. If nothing a user can observe has changed, say so explicitly:" >&2
  echo "" >&2
  echo "       Release-Note: none — <why this is invisible to users>" >&2
  echo "" >&2
  echo "     Put that line in a commit message (pushing re-runs this check) or in the" >&2
  echo "     pull request description (then re-run this job — editing a description" >&2
  echo "     does not itself re-trigger CI)." >&2
  echo "" >&2
  return 1
}

# ---------------------------------------------------------------------------
# Self-test — every branch above, including the ones that must block
# ---------------------------------------------------------------------------

self_test() {
  local failures=0 n=0

  check() {
    local name="$1" want="$2" changed="$3" haystack="$4"
    local got=0
    n=$((n + 1))
    evaluate_release_note "$changed" "$haystack" >/dev/null 2>&1 || got=$?
    if [ "$got" -ne "$want" ]; then
      echo "  FAIL  $name (wanted exit $want, got $got)" >&2
      failures=$((failures + 1))
    else
      echo "  ok    $name"
    fi
  }

  echo "Self-test: release-note gate"

  check "product code + note                 -> pass" 0 \
    "$(printf 'src/worker/rewrite.cc\nCHANGELOG.md\n')" ""

  check "product code, no note, no waiver    -> BLOCK" 1 \
    "$(printf 'src/worker/rewrite.cc\n')" ""

  check "product code + waiver in commit     -> pass" 0 \
    "$(printf 'src/worker/rewrite.cc\n')" \
    "$(printf 'refactor: rename a local\n\nRelease-Note: none - pure rename, no behaviour change\n')"

  check "product code + waiver in PR body    -> pass" 0 \
    "$(printf 'lib/js/js_minify.cc\n')" \
    "Release-Note: none — internal assertion only, unreachable in release builds"

  check "waiver with em dash                 -> pass" 0 \
    "$(printf 'src/a.cc\n')" "Release-Note: none — dead code deleted, never compiled in"

  check "waiver with colon                   -> pass" 0 \
    "$(printf 'src/a.cc\n')" "Release-Note: none: dead code deleted, never compiled in"

  check "waiver with no reason               -> BLOCK" 1 \
    "$(printf 'src/a.cc\n')" "Release-Note: none -"

  check "waiver with a token reason          -> BLOCK" 1 \
    "$(printf 'src/a.cc\n')" "Release-Note: none - nfc"

  check "tests only                          -> pass" 0 \
    "$(printf 'test/lib/js/js_minify_test.cc\n')" ""

  check "test file inside src tree           -> pass" 0 \
    "$(printf 'src/worker/rewrite_test.cc\n')" ""

  check "BUILD file only                     -> pass" 0 \
    "$(printf 'src/worker/BUILD\n')" ""

  check "docs and CI only                    -> pass" 0 \
    "$(printf 'docs/configuration.md\n.github/dependabot.yml\n')" ""

  check "samples only                        -> pass" 0 \
    "$(printf 'samples/aspnetcore/samples/DemoSite/Program.cs\n')" ""

  check "code + test, note absent            -> BLOCK" 1 \
    "$(printf 'src/worker/rewrite.cc\nsrc/worker/rewrite_test.cc\n')" ""

  check "note alone, no product code         -> pass" 0 \
    "$(printf 'CHANGELOG.md\n')" ""

  # changelog.d/ fragments (see changelog.d/README.md). Fifth argument: the
  # fragments the change adds or modifies.
  fchk() {
    local name="$1" want="$2" changed="$3" frags="$4" haystack="${5:-}" secadd="${6:-}" ref="${7:-fix/topic}"
    local got=0
    n=$((n + 1))
    evaluate_release_note "$changed" "$haystack" "$secadd" "$ref" "$frags" >/dev/null 2>&1 || got=$?
    if [ "$got" -ne "$want" ]; then
      echo "  FAIL  $name (wanted exit $want, got $got)" >&2
      failures=$((failures + 1))
    else
      echo "  ok    $name"
    fi
  }
  local SRC=$'src/worker/rewrite.cc\n' F1='changelog.d/fix-rewrite.fixed.md'
  fchk "product code + added fragment        -> pass" 0 "${SRC}${F1}" "$F1"
  fchk "product code + deleted fragment only -> BLOCK" 1 "${SRC}${F1}" ""
  fchk "product code + README only           -> BLOCK" 1 "${SRC}changelog.d/README.md" "changelog.d/README.md"
  fchk "product code + nested fragment path  -> BLOCK" 1 "${SRC}changelog.d/sub/x.fixed.md" "changelog.d/sub/x.fixed.md"
  fchk "security fragment, normal branch     -> BLOCK" 1 "${SRC}changelog.d/s.security.md" "changelog.d/s.security.md"
  fchk "security fragment + public marker    -> pass" 0 "${SRC}changelog.d/s.security.md" "changelog.d/s.security.md" \
    "Release-Note: security-public -- advisory already published upstream"
  fchk "security fragment removed (the cut)  -> pass" 0 "changelog.d/s.security.md" ""
  fchk "security line inside a fixed fragment-> BLOCK" 1 "${SRC}${F1}" "$F1" "" \
    "Security: a client could read a response meant for another client."
  fchk "same, landing branch + marker        -> pass" 0 "${SRC}${F1}" "$F1" \
    "Release-Note: landing -- the 2.9.0 release landing" \
    "Security: a client could read a response meant for another client." landing/2.9.0

  # The separator is a RUN, not one character. The gate's own help text
  # suggests `Release-Note: none -- <why>`, so the double hyphen has to work;
  # before it did, the second `-` survived into the reason and counted toward
  # the minimum length.
  check "waiver with a double hyphen         -> pass" 0 \
    "$(printf 'src/a.cc\n')" "Release-Note: none -- dead code deleted, never compiled in"

  check "double hyphen does not pad a token  -> BLOCK" 1 \
    "$(printf 'src/a.cc\n')" "Release-Note: none -- nfc"

  # A reason that wraps is read whole, and stops before the next trailer.
  check "waiver wrapping onto the next line  -> pass" 0 \
    "$(printf 'src/a.cc\n')" \
    "$(printf 'fix: something\n\nRelease-Note: none --\n  the whole reason lives on the following line\n\nCo-Authored-By: Someone <x@example.invalid>\n')"

  check "waiver must not match a mention     -> BLOCK" 1 \
    "$(printf 'src/a.cc\n')" "I wondered whether to write Release-Note: none but decided against it"

  # --- security entries land only with a release landing ---

  schk() {
    local name="$1" want="$2" secadd="$3" haystack="$4" ref="$5"
    local got=0
    n=$((n + 1))
    evaluate_release_note "CHANGELOG.md" "$haystack" "$secadd" "$ref" >/dev/null 2>&1 || got=$?
    if [ "$got" -ne "$want" ]; then
      echo "  FAIL  $name (wanted exit $want, got $got)" >&2
      failures=$((failures + 1))
    else
      echo "  ok    $name"
    fi
  }
  local SL="Security: a client could read a response meant for another client. Update recommended."
  local LM="Release-Note: landing -- the 2.9.0 release landing"
  local PM="Release-Note: security-public -- advisory already published upstream"

  schk "security line, normal branch         -> BLOCK" 1 "$SL" "" "fix/topic"
  schk "security line, no branch known       -> BLOCK" 1 "$SL" "" ""
  schk "security line, landing mode          -> pass" 0 "$SL" "$LM" "landing/2.9.0"
  schk "landing marker, other branch         -> BLOCK" 1 "$SL" "$LM" "fix/topic"
  schk "landing branch, no marker            -> BLOCK" 1 "$SL" "" "landing/2.9.0"
  schk "security line, public marker         -> pass" 0 "$SL" "$PM" "fix/topic"
  schk "public marker, colon separator       -> pass" 0 "$SL" "Release-Note: security-public: advisory already published" "x/y"
  schk "public marker, no reason             -> BLOCK" 1 "$SL" "Release-Note: security-public --" "fix/topic"
  schk "public marker, token reason          -> BLOCK" 1 "$SL" "Release-Note: security-public -- ok" "fix/topic"
  schk "public marker only as a mention      -> BLOCK" 1 "$SL" "we may write Release-Note: security-public -- advisory already out" "fix/topic"
  schk "security heading, normal branch      -> BLOCK" 1 "### Security" "" "fix/topic"
  schk "bulleted security entry              -> BLOCK" 1 "- **Security:** hardened a parser" "" "fix/topic"
  schk "other changelog line                 -> pass" 0 "Changed: the worker lists more options in --help." "" "fix/topic"
  schk "a word starting with security        -> pass" 0 "Securitykit is a name, not a heading." "" "fix/topic"
  schk "Security, known issue: form          -> BLOCK" 1 "Security, known issue: a client could read a response." "" "fix/topic"
  schk "Security fix: form                   -> BLOCK" 1 "Security fix: a client could read a response." "" "fix/topic"
  schk "### Security fixes heading           -> BLOCK" 1 "### Security fixes" "" "fix/topic"
  schk "bold bullet Security fix:            -> BLOCK" 1 "- **Security fix:** hardened a parser" "" "fix/topic"
  schk "lowercase security:                  -> BLOCK" 1 "security: a client could read a response." "" "fix/topic"
  schk "wrapped prose without a colon        -> pass" 0 "Security matters to every release of the worker." "" "fix/topic"
  schk "prose mentioning security mid-line   -> pass" 0 "Changed: the worker reports a security: header." "" "fix/topic"
  schk "release cut (heading rename only)    -> pass" 0 "## 2.9.0 - 2026-11-20" "" "release/2.9.0"

  # End-to-end through the entry point, because the CI path and the fail-closed
  # branches live there rather than in the pure function above. A gate that
  # cannot see its input must go red, not quietly green.
  local self="${BASH_SOURCE[0]}" tmp
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN

  e2e() {
    local name="$1" want="$2"; shift 2
    local got=0
    n=$((n + 1))
    bash "$self" "$@" >/dev/null 2>&1 || got=$?
    if [ "$got" -ne "$want" ]; then
      echo "  FAIL  $name (wanted exit $want, got $got)" >&2
      failures=$((failures + 1))
    else
      echo "  ok    $name"
    fi
  }

  printf 'src/worker/rewrite.cc\n' > "$tmp/files-block.txt"
  printf 'src/worker/rewrite.cc\nCHANGELOG.md\n' > "$tmp/files-pass.txt"
  printf 'Release-Note: none - build plumbing only, nothing user visible\n' > "$tmp/waiver.txt"

  e2e "--files-from missing               -> BLOCK" 1 --files-from "$tmp/nope.txt"
  e2e "--haystack-from missing            -> BLOCK" 1 \
    --files-from "$tmp/files-block.txt" --haystack-from "$tmp/nope.txt"
  e2e "--files-from, no note              -> BLOCK" 1 --files-from "$tmp/files-block.txt"
  e2e "--files-from, with note            -> pass " 0 --files-from "$tmp/files-pass.txt"
  e2e "--files-from + waiver file         -> pass " 0 \
    --files-from "$tmp/files-block.txt" --haystack-from "$tmp/waiver.txt"
  printf 'src/worker/rewrite.cc\nCHANGELOG.md\n' > "$tmp/files-sec.txt"
  printf 'Security: a client could read a response meant for another client.\n' > "$tmp/secadd.txt"
  : > "$tmp/secadd-none.txt"
  printf 'Release-Note: landing -- the 2.9.0 release landing\n' > "$tmp/landing.txt"
  e2e "--changelog-added-from missing     -> BLOCK" 1 \
    --files-from "$tmp/files-sec.txt" --changelog-added-from "$tmp/nope.txt"
  e2e "--changelog-added-from, security   -> BLOCK" 1 \
    --files-from "$tmp/files-sec.txt" --changelog-added-from "$tmp/secadd.txt" --head-ref fix/topic
  e2e "--changelog-added-from, none       -> pass " 0 \
    --files-from "$tmp/files-sec.txt" --changelog-added-from "$tmp/secadd-none.txt" --head-ref fix/topic
  e2e "security + landing/ branch + marker-> pass " 0 \
    --files-from "$tmp/files-sec.txt" --changelog-added-from "$tmp/secadd.txt" \
    --haystack-from "$tmp/landing.txt" --head-ref landing/2.9.0
  printf 'src/worker/rewrite.cc\nchangelog.d/x-y-z.fixed.md\n' > "$tmp/files-frag.txt"
  printf 'changelog.d/x-y-z.fixed.md\n' > "$tmp/frags.txt"
  e2e "fragment listed in --fragments-from-> pass " 0 \
    --files-from "$tmp/files-frag.txt" --fragments-from "$tmp/frags.txt"
  e2e "fragment without --fragments-from  -> BLOCK" 1 --files-from "$tmp/files-frag.txt"
  e2e "--fragments-from missing           -> BLOCK" 1 \
    --files-from "$tmp/files-frag.txt" --fragments-from "$tmp/nope.txt"
  e2e "no arguments at all                -> BLOCK" 1
  e2e "unresolvable base                  -> BLOCK" 1 --base 0000000000000000000000000000000000000000

  echo ""
  if [ "$failures" -ne 0 ]; then
    echo "FAILED: $failures of $n self-test cases did not behave as specified." >&2
    return 1
  fi
  echo "OK: all $n self-test cases behave as specified."
  return 0
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

BASE=""
HEAD="HEAD"
FILES_FROM=""
HAYSTACK_FROM=""
CHANGELOG_ADDED_FROM=""
FRAGMENTS_FROM=""
HEAD_REF="${GITHUB_HEAD_REF:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) self_test; exit $? ;;
    --base) BASE="${2:-}"; shift 2 ;;
    --head) HEAD="${2:-}"; shift 2 ;;
    --files-from) FILES_FROM="${2:-}"; shift 2 ;;
    --haystack-from) HAYSTACK_FROM="${2:-}"; shift 2 ;;
    --changelog-added-from) CHANGELOG_ADDED_FROM="${2:-}"; shift 2 ;;
    --fragments-from) FRAGMENTS_FROM="${2:-}"; shift 2 ;;
    --head-ref) HEAD_REF="${2:-}"; shift 2 ;;
    -h|--help)
      sed -n '6,28p' "${BASH_SOURCE[0]}"
      exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

cd "$REPO_ROOT"

# CI path: the changed-file list and the waiver text are supplied by the caller,
# so no git history is needed and a depth-1 checkout is fine.
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
  SECADD=""
  if [ -n "$CHANGELOG_ADDED_FROM" ]; then
    if [ ! -f "$CHANGELOG_ADDED_FROM" ]; then
      echo "FAILED: --changelog-added-from '$CHANGELOG_ADDED_FROM' does not exist." >&2
      exit 1
    fi
    SECADD="$(cat "$CHANGELOG_ADDED_FROM")"
  fi
  FRAGS=""
  if [ -n "$FRAGMENTS_FROM" ]; then
    if [ ! -f "$FRAGMENTS_FROM" ]; then
      echo "FAILED: --fragments-from '$FRAGMENTS_FROM' does not exist." >&2
      exit 1
    fi
    FRAGS="$(cat "$FRAGMENTS_FROM")"
  fi
  evaluate_release_note "$(cat "$FILES_FROM")" "$HAYSTACK" "$SECADD" "$HEAD_REF" "$FRAGS"
  exit $?
fi

if [ -z "$BASE" ]; then
  echo "FAILED: --base is required (or pass --self-test / --files-from)." >&2
  exit 1
fi

# Fail closed. A gate that cannot see the diff must not report success — that is
# indistinguishable from a gate that passed, which is the whole failure mode
# this file exists to prevent. A shallow clone is the common cause: the CI job
# needs fetch-depth: 0.
if ! MERGE_BASE="$(git merge-base "$BASE" "$HEAD" 2>/dev/null)" || [ -z "$MERGE_BASE" ]; then
  echo "FAILED: cannot resolve a merge base between '$BASE' and '$HEAD'." >&2
  echo "This check cannot run, so it is failing rather than passing silently." >&2
  echo "In CI, ensure actions/checkout uses fetch-depth: 0." >&2
  exit 1
fi

CHANGED="$(git diff --name-only "$MERGE_BASE" "$HEAD")"
COMMIT_MESSAGES="$(git log --format='%B' "$MERGE_BASE..$HEAD")"
HAYSTACK="$(printf '%s\n%s\n' "$COMMIT_MESSAGES" "${PR_BODY:-}")"

# Added or modified fragments only (a deletion is not a note).
FRAGS="$(git diff --name-only --diff-filter=AM "$MERGE_BASE" "$HEAD" -- changelog.d || true)"
SECADD="$(git diff -U0 "$MERGE_BASE" "$HEAD" -- CHANGELOG.md ':(glob)changelog.d/*.md' ':!changelog.d/README.md' | grep -E '^\+' | grep -vE '^\+\+\+' | sed 's/^+//' || true)"

evaluate_release_note "$CHANGED" "$HAYSTACK" "$SECADD" "$HEAD_REF" "$FRAGS"
