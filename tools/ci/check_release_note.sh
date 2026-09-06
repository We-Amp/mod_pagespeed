#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI release-note gate (BLOCKING): user-facing changes must carry a release note.
#
# A change that alters what an operator observes, and ships without a line in
# RELEASE_NOTES.md or CHANGELOG.md, is invisible. Nobody upgrading learns that
# the thing they were working around is fixed, and nobody debugging learns that
# behaviour moved. The failure is silent by construction: nothing goes red, the
# release simply under-reports itself, and the omission is only ever noticed
# later by someone who needed the note.
#
# So this gate asks one question of every pull request: you touched product
# code -- where is the note? It is deliberately easy to satisfy and deliberately
# impossible to ignore. A change with no user-visible effect declares that in
# one line and moves on; what it cannot do is say nothing at all.
#
# This repo keeps two note files and EITHER satisfies the gate:
#   RELEASE_NOTES.md  operator-facing, newest release first; the in-development
#                     section is the top entry marked "**Status:** In development"
#   CHANGELOG.md      Keep a Changelog; add under "## [Unreleased]"
#
# Run locally:  bash tools/ci/check_release_note.sh --base origin/master --head HEAD
# Self-test:    bash tools/ci/check_release_note.sh --self-test
#
# In CI the inputs come from the GitHub API instead of git, so the check works
# on the shallow checkout the rest of the job already uses:
#   bash tools/ci/check_release_note.sh --files-from FILE --haystack-from FILE

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------

# The files that carry operator-facing notes. Touching ANY ONE of them satisfies
# the gate -- RELEASE_NOTES.md is the operator narrative, CHANGELOG.md the
# Keep-a-Changelog ledger, and which one a change belongs in is the author's
# call, not this script's.
RELEASE_NOTE_FILES="RELEASE_NOTES.md
CHANGELOG.md"

# Product code: changing it can change what someone observes. pagespeed/ and
# net/ are the two source trees (see "Two Source Trees" in CLAUDE.md).
#
# From install/, only the shipped *configuration* qualifies — pagespeed.conf and
# its siblings, which an operator reads and copies, and whose comments have
# already been caught contradicting the code. Everything else under install/ is
# packaging and build plumbing.
#
# That distinction is measured, not assumed. Taking all of install/ blocked 5 of
# the last 14 pull requests, and 4 of those 5 were the same thing:
# install/*/build_*_in_container.sh, a container build script that ships to
# nobody. A gate that is wrong a third of the time teaches people to reach for
# the waiver without reading it, which costs more than the coverage is worth.
USER_FACING_RE='^(pagespeed|net)/|^install/.*\.(conf|template)$'

# Carve-outs inside the product tree that cannot reach a user on their own.
# Tests and build plumbing change no shipped behaviour by themselves; a change
# that alters both a test and the code it covers still trips the gate on the
# code half, which is the intent.
#
#   1. a test/ , tests/ , testdata/ , or *_test(s)/ directory anywhere in the
#      path -- this repo has test/ at the root and install/mod_pagespeed_test/
#   2. a *_test.<ext> file next to its source (.sh and .py matter here: install/
#      is full of *_test.sh system tests)
#   3. Bazel BUILD files
#   4. Markdown
#
# NOTE: pagespeed/iis/ is deliberately NOT exempt. It is shipped product code.
# tools/fix-format.sh excludes it for unrelated formatting reasons; that
# exclusion must not be copied here.
EXEMPT_RE='(^|/)(test|tests|testdata)/|(^|/)[^/]*_tests?/|(^|/)[^/]*_test\.(cc|cpp|h|hpp|sh|py|bat|bzl|js|mjs|ts|html|proto)$|(^|/)BUILD(\.bazel)?$|\.md$'

# The waiver. A reason is mandatory: the point is to record the judgement, not
# to provide a mute button. Accepts `-`, `:`, an en dash or an em dash as the
# separator, because people type all four.
WAIVER_RE='^[[:space:]]*Release-Note:[[:space:]]*none[[:space:]]*[-:–—][[:space:]]*'
WAIVER_MIN_REASON=12

# ---------------------------------------------------------------------------
# Core decision, kept pure so the self-test can drive every branch
# ---------------------------------------------------------------------------
#
# $1 -- newline-separated changed-file list
# $2 -- waiver haystack (commit messages and/or PR body)
#
# 0 = pass, 1 = block.
evaluate_release_note() {
  local changed="$1"
  local haystack="$2"
  local triggering note_touched waiver_line reason

  triggering="$(printf '%s\n' "$changed" \
    | grep -E "$USER_FACING_RE" \
    | grep -vE "$EXEMPT_RE" \
    || true)"

  if [ -z "$triggering" ]; then
    echo "OK: no user-facing product code in this change -- no release note required."
    return 0
  fi

  # Exact whole-line match: docs/CHANGELOG.md is not the release note.
  note_touched="$(printf '%s\n' "$changed" \
    | grep -Fx "$RELEASE_NOTE_FILES" || true)"
  if [ -n "$note_touched" ]; then
    echo "OK: user-facing change carries an entry in: $(printf '%s' "$note_touched" | tr '\n' ' ')"
    return 0
  fi

  waiver_line="$(printf '%s\n' "$haystack" | grep -iE "$WAIVER_RE" | head -n 1 || true)"
  if [ -n "$waiver_line" ]; then
    reason="$(printf '%s\n' "$waiver_line" | sed -E "s/$WAIVER_RE//I")"
    # Trim.
    reason="$(printf '%s' "$reason" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
    if [ "${#reason}" -lt "$WAIVER_MIN_REASON" ]; then
      echo "" >&2
      echo "FAILED: the release-note waiver has no usable reason." >&2
      echo "" >&2
      echo "  found:  $waiver_line" >&2
      echo "" >&2
      echo "A waiver records a judgement, so it has to say something. Write why this" >&2
      echo "change is invisible to users -- at least $WAIVER_MIN_REASON characters of actual reason." >&2
      return 1
    fi
    echo "OK: release note waived -- $reason"
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
  echo "  1. Add an entry to RELEASE_NOTES.md (the in-development section at the top," >&2
  echo "     marked '**Status:** In development') or to CHANGELOG.md (under" >&2
  echo "     '## [Unreleased]'). Either satisfies this gate. Write it for someone" >&2
  echo "     deciding whether to upgrade: what they observed before, what they" >&2
  echo "     observe now." >&2
  echo "" >&2
  echo "  2. If nothing a user can observe has changed, say so explicitly:" >&2
  echo "" >&2
  echo "       Release-Note: none -- <why this is invisible to users>" >&2
  echo "" >&2
  echo "     Put that line in a commit message (pushing re-runs this check) or in the" >&2
  echo "     pull request description (then re-run this job -- editing a description" >&2
  echo "     does not itself re-trigger CI)." >&2
  echo "" >&2
  return 1
}

# ---------------------------------------------------------------------------
# Self-test -- every branch above, including the ones that must block
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

  # --- the two note files, either of which satisfies ---

  check "product code + RELEASE_NOTES.md     -> pass" 0 \
    "$(printf 'net/instaweb/rewriter/css_filter.cc\nRELEASE_NOTES.md\n')" ""

  check "product code + CHANGELOG.md         -> pass" 0 \
    "$(printf 'net/instaweb/rewriter/css_filter.cc\nCHANGELOG.md\n')" ""

  check "product code + both note files      -> pass" 0 \
    "$(printf 'pagespeed/kernel/http/response_headers.cc\nRELEASE_NOTES.md\nCHANGELOG.md\n')" ""

  check "RELEASE_NOTES.md alone              -> pass" 0 \
    "$(printf 'RELEASE_NOTES.md\n')" ""

  check "CHANGELOG.md alone                  -> pass" 0 \
    "$(printf 'CHANGELOG.md\n')" ""

  # A nested file of the same basename is not the release note.
  check "docs/CHANGELOG.md is not the note   -> BLOCK" 1 \
    "$(printf 'pagespeed/kernel/http/response_headers.cc\ndocs/CHANGELOG.md\n')" ""

  check "some other .md is not the note      -> BLOCK" 1 \
    "$(printf 'pagespeed/kernel/http/response_headers.cc\ndocs/GLOSSARY.md\n')" ""

  # --- the trigger surface ---

  check "no note, no waiver                  -> BLOCK" 1 \
    "$(printf 'net/instaweb/rewriter/css_filter.cc\n')" ""

  check "pagespeed/ triggers                 -> BLOCK" 1 \
    "$(printf 'pagespeed/system/system_rewrite_driver_factory.cc\n')" ""

  check "install/ sample conf triggers       -> BLOCK" 1 \
    "$(printf 'install/debug.conf.template\n')" ""

  check "install/ shipped pagespeed.conf     -> BLOCK" 1 \
    "$(printf 'install/common/pagespeed.conf\n')" ""

  # The measured false-positive class. install/ earns its place in the trigger
  # because of the shipped configuration only; the container build scripts and
  # the packaging plumbing beside it reach no user, and taking all of install/
  # blocked 4 of the last 14 pull requests on these files alone.
  check "install/ container build script     -> pass " 0 \
    "$(printf 'install/nginx/build_module_in_container.sh\n')" ""

  check "install/ rpm container build        -> pass " 0 \
    "$(printf 'install/rpm/build_rpm_in_container.sh\n')" ""

  check "install/ debian packaging plumbing  -> pass " 0 \
    "$(printf 'install/debian/postinst\n')" ""

  check "install/ conf + build script        -> BLOCK" 1 \
    "$(printf 'install/nginx/build_module_in_container.sh\ninstall/common/pagespeed.conf\n')" ""

  # pagespeed/iis is shipped product code. tools/fix-format.sh excludes it for
  # formatting reasons only -- that exclusion must never leak into this gate.
  check "pagespeed/iis/ is NOT exempt        -> BLOCK" 1 \
    "$(printf 'pagespeed/iis/iis_rewrite_driver_factory.cpp\n')" ""

  check "apache module triggers              -> BLOCK" 1 \
    "$(printf 'pagespeed/apache/mod_instaweb.cc\n')" ""

  check "generated shipped JS triggers       -> BLOCK" 1 \
    "$(printf 'net/instaweb/genfiles/rewriter/js_defer_opt.js\n')" ""

  # --- the waiver ---

  check "waiver in commit message            -> pass" 0 \
    "$(printf 'net/instaweb/rewriter/css_filter.cc\n')" \
    "$(printf 'refactor: rename a local\n\nRelease-Note: none - pure rename, no behaviour change\n')"

  check "waiver in PR body                   -> pass" 0 \
    "$(printf 'pagespeed/kernel/js/js_minify.cc\n')" \
    "Release-Note: none — internal assertion only, unreachable in release builds"

  check "waiver with em dash                 -> pass" 0 \
    "$(printf 'pagespeed/a.cc\n')" "Release-Note: none — dead code deleted, never compiled in"

  check "waiver with en dash                 -> pass" 0 \
    "$(printf 'pagespeed/a.cc\n')" "Release-Note: none – dead code deleted, never compiled in"

  check "waiver with colon                   -> pass" 0 \
    "$(printf 'pagespeed/a.cc\n')" "Release-Note: none: dead code deleted, never compiled in"

  check "waiver with no reason               -> BLOCK" 1 \
    "$(printf 'pagespeed/a.cc\n')" "Release-Note: none -"

  check "waiver with a token reason          -> BLOCK" 1 \
    "$(printf 'pagespeed/a.cc\n')" "Release-Note: none - nfc"

  check "waiver must not match a mention     -> BLOCK" 1 \
    "$(printf 'pagespeed/a.cc\n')" \
    "I wondered whether to write Release-Note: none but decided against it"

  # --- the exemptions ---

  check "tests only (root test/ tree)        -> pass" 0 \
    "$(printf 'test/pagespeed/kernel/js/js_minify_test.cc\n')" ""

  check "test file inside pagespeed tree     -> pass" 0 \
    "$(printf 'pagespeed/kernel/js/js_minify_test.cc\n')" ""

  check "install system test .sh             -> pass" 0 \
    "$(printf 'install/apache_experiment_test.sh\n')" ""

  check "install/mod_pagespeed_test/ fixture -> pass" 0 \
    "$(printf 'install/mod_pagespeed_test/rewrite_images.html\n')" ""

  check "BUILD file only                     -> pass" 0 \
    "$(printf 'pagespeed/kernel/js/BUILD\n')" ""

  check "BUILD.bazel only                    -> pass" 0 \
    "$(printf 'net/instaweb/rewriter/BUILD.bazel\n')" ""

  check "in-tree .md only                    -> pass" 0 \
    "$(printf 'pagespeed/envoy/README.md\n')" ""

  check "docs and CI only                    -> pass" 0 \
    "$(printf 'docs/GLOSSARY.md
.github/dependabot.yml
')" ""

  check "tools/ and bazel/ only              -> pass" 0 \
    "$(printf 'tools/format.sh\nbazel/pagespeed_test.bzl\n')" ""

  check "code + its test, note absent        -> BLOCK" 1 \
    "$(printf 'pagespeed/kernel/js/js_minify.cc\ntest/pagespeed/kernel/js/js_minify_test.cc\n')" ""

  check "nothing changed at all              -> pass" 0 "" ""

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

  printf 'net/instaweb/rewriter/css_filter.cc\n' > "$tmp/files-block.txt"
  printf 'net/instaweb/rewriter/css_filter.cc\nRELEASE_NOTES.md\n' > "$tmp/files-relnotes.txt"
  printf 'net/instaweb/rewriter/css_filter.cc\nCHANGELOG.md\n' > "$tmp/files-changelog.txt"
  printf 'Release-Note: none - build plumbing only, nothing user visible\n' > "$tmp/waiver.txt"

  e2e "--files-from missing               -> BLOCK" 1 --files-from "$tmp/nope.txt"
  e2e "--haystack-from missing            -> BLOCK" 1 \
    --files-from "$tmp/files-block.txt" --haystack-from "$tmp/nope.txt"
  e2e "--files-from, no note              -> BLOCK" 1 --files-from "$tmp/files-block.txt"
  e2e "--files-from, RELEASE_NOTES.md     -> pass " 0 --files-from "$tmp/files-relnotes.txt"
  e2e "--files-from, CHANGELOG.md         -> pass " 0 --files-from "$tmp/files-changelog.txt"
  e2e "--files-from + waiver file         -> pass " 0 \
    --files-from "$tmp/files-block.txt" --haystack-from "$tmp/waiver.txt"
  e2e "no arguments at all                -> BLOCK" 1
  e2e "unresolvable base                  -> BLOCK" 1 \
    --base 0000000000000000000000000000000000000000

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

while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) self_test; exit $? ;;
    --base) BASE="${2:-}"; shift 2 ;;
    --head) HEAD="${2:-}"; shift 2 ;;
    --files-from) FILES_FROM="${2:-}"; shift 2 ;;
    --haystack-from) HAYSTACK_FROM="${2:-}"; shift 2 ;;
    -h|--help)
      sed -n '6,31p' "${BASH_SOURCE[0]}"
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
      echo "This check cannot run, so it is failing rather than passing silently." >&2
      exit 1
    fi
    HAYSTACK="$(cat "$HAYSTACK_FROM")"
  fi
  evaluate_release_note "$(cat "$FILES_FROM")" "$HAYSTACK"
  exit $?
fi

if [ -z "$BASE" ]; then
  echo "FAILED: --base is required (or pass --self-test / --files-from)." >&2
  exit 1
fi

# Fail closed. A gate that cannot see the diff must not report success -- that is
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

evaluate_release_note "$CHANGED" "$HAYSTACK"
