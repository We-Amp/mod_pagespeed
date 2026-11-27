#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Single entry point for C/C++ formatting. Byte-matches the CI clang-format lint.
#
# CI's "clang-format check" runs clang-format 20 over
# `lib src test/lib test/src test/test_util`. The pinned version is the
# mirrors-clang-format rev in .pre-commit-config.yaml. This script resolves a
# clang-format 20.x binary the same way and formats/checks the identical scope,
# so a local run matches CI exactly — no remote formatter box required.
# (Replaces the removed tools/check-format.sh, whose narrower scope predated
# the test/ dirs.)
#
# Version resolution order:
#   1. clang-format-20 / clang-format on PATH, if its major is 20.
#   2. A pinned pip wheel (clang-format==<PIN>) in a shared cache venv,
#      bootstrapped on first use. The PyPI wheel is the same artifact
#      mirrors-clang-format installs and is byte-identical to the CI binary.
#   3. Otherwise: error with install instructions.
#
# Usage:
#   tools/format.sh              # format in place (whole CI scope)
#   tools/format.sh --check      # verify only (clang-format --dry-run --Werror), like CI
#   tools/format.sh --changed    # restrict to files changed vs upstream (fast)
#   tools/format.sh --check --changed   # push-gate check (used by the pre-push hook)

set -euo pipefail

# Pinned clang-format version — MUST equal the mirrors-clang-format rev in
# .pre-commit-config.yaml. Verified byte-identical to CI's clang-format-20.
PIN="20.1.8"
WANT_MAJOR="${PIN%%.*}"
DEFAULT_BRANCH="main"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# The version source of truth is the mirrors-clang-format rev in
# .pre-commit-config.yaml; PIN above is a convenience copy. Refuse to run on a
# mismatch so a future version bump cannot silently diverge.
SRC_REV="$(awk '/mirrors-clang-format/{f=1;next} f&&/rev:/{gsub(/.*rev:[ \t]*v?/,"");gsub(/[ \t#].*/,"");print;exit}' .pre-commit-config.yaml 2>/dev/null || true)"
if [ "${SRC_REV}" != "$PIN" ]; then
  echo "ERROR: PIN=$PIN in tools/format.sh does not match the mirrors-clang-format" >&2
  echo "       rev v${SRC_REV:-<unreadable>} in .pre-commit-config.yaml (the source of truth)." >&2
  echo "       Update PIN in tools/format.sh to match." >&2
  exit 1
fi

MODE="fix"
SCOPE="all"
for arg in "$@"; do
  case "$arg" in
    --check)   MODE="check" ;;
    --changed) SCOPE="changed" ;;
    -h|--help) grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

cf_major() { "$1" --version 2>/dev/null | grep -oE '[0-9]+' | head -1; }

resolve_cf() {
  local c
  for c in "clang-format-${WANT_MAJOR}" clang-format; do
    if command -v "$c" >/dev/null 2>&1 && [ "$(cf_major "$c")" = "$WANT_MAJOR" ]; then
      command -v "$c"; return 0
    fi
  done
  local cache="${XDG_CACHE_HOME:-$HOME/.cache}/weamp-clang-format/$PIN"
  local cf="$cache/venv/bin/clang-format"
  if [ ! -x "$cf" ]; then
    if ! command -v python3 >/dev/null 2>&1; then
      cat >&2 <<EOF
ERROR: no clang-format ${WANT_MAJOR}.x on PATH and python3 is unavailable to
       bootstrap the pinned wheel. Install ONE of:
  - clang-format ${WANT_MAJOR}: apt-get install clang-format-${WANT_MAJOR} (Linux)
  - python3, then re-run this script (it pip-installs clang-format==${PIN})
EOF
      return 1
    fi
    echo "format.sh: bootstrapping pinned clang-format ${PIN} into ${cache} ..." >&2
    python3 -m venv "$cache/venv" >&2
    "$cache/venv/bin/pip" install --quiet --disable-pip-version-check "clang-format==${PIN}" >&2
  fi
  if [ "$(cf_major "$cf")" != "$WANT_MAJOR" ]; then
    echo "ERROR: pinned wheel at $cf is not major ${WANT_MAJOR}." >&2; return 1
  fi
  printf '%s\n' "$cf"
}

# --- repo CI scope (mirror of ci.yml "clang-format check") ---
# Vendored kernels keep canonical formatting (style-lint carve-out ONLY; see
# lib/js/CLAUDE.md and lib/html/CLAUDE.md): lib/js/js_*, lib/js/source_map.h,
# lib/js/corpus/**, and the explicit lib/html vendored-name list (no uniform
# prefix — html_parse_probe.cc/html_parse_recorder.h and the optimizer-native
# filters are 2.0-owned and stay formatted). Mirrors the find -path excludes
# in ci.yml; membership is enforced by tools/check-*-kernel-membership.sh.
all_scope_files() {
  find lib src test/lib test/src test/test_util -type f \
    \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    ! -path "lib/js/js_*" ! -path "lib/js/source_map.h" \
    ! -path "lib/js/corpus/*" \
    ! -path "lib/html/content_type.h" ! -path "lib/html/content_type.cc" \
    ! -path "lib/html/doctype.h" ! -path "lib/html/doctype.cc" \
    ! -path "lib/html/empty_html_filter.h" ! -path "lib/html/empty_html_filter.cc" \
    ! -path "lib/html/html_element.h" ! -path "lib/html/html_element.cc" \
    ! -path "lib/html/html_event.h" ! -path "lib/html/html_event.cc" \
    ! -path "lib/html/html_filter.h" ! -path "lib/html/html_filter.cc" \
    ! -path "lib/html/html_keywords.h" ! -path "lib/html/html_keywords.cc" \
    ! -path "lib/html/html_lexer.h" ! -path "lib/html/html_lexer.cc" \
    ! -path "lib/html/html_name.h" \
    ! -path "lib/html/html_node.h" ! -path "lib/html/html_node.cc" \
    ! -path "lib/html/html_parse.h" ! -path "lib/html/html_parse.cc" \
    ! -path "lib/html/html_parse_probe.cc" ! -path "lib/html/html_parse_recorder.h" \
    ! -path "lib/html/html_writer_filter.h" ! -path "lib/html/html_writer_filter.cc" \
    2>/dev/null | sort
}
in_scope() {
  case "$1" in
    lib/js/js_*|lib/js/source_map.h|lib/js/corpus/*) return 1 ;;
    lib/html/content_type.h|lib/html/content_type.cc|\
lib/html/doctype.h|lib/html/doctype.cc|\
lib/html/empty_html_filter.h|lib/html/empty_html_filter.cc|\
lib/html/html_element.h|lib/html/html_element.cc|\
lib/html/html_event.h|lib/html/html_event.cc|\
lib/html/html_filter.h|lib/html/html_filter.cc|\
lib/html/html_keywords.h|lib/html/html_keywords.cc|\
lib/html/html_lexer.h|lib/html/html_lexer.cc|\
lib/html/html_name.h|\
lib/html/html_node.h|lib/html/html_node.cc|\
lib/html/html_parse.h|lib/html/html_parse.cc|\
lib/html/html_parse_probe.cc|lib/html/html_parse_recorder.h|\
lib/html/html_writer_filter.h|lib/html/html_writer_filter.cc)
      return 1 ;;
  esac
  case "$1" in
    lib/*|src/*|test/lib/*|test/src/*|test/test_util/*)
      case "$1" in *.cc|*.cpp|*.h|*.hpp) return 0 ;; esac ;;
  esac
  return 1
}

# Base for --changed: prefer the exact range being pushed. pre-commit exports
# PRE_COMMIT_FROM_REF/PRE_COMMIT_TO_REF during its pre-push stage, and a
# .githooks-style pre-push hook can export the same pair from its stdin ref
# lines. Otherwise fall back to merge-base with the upstream. If no base can be
# resolved, FAIL CLOSED: silently checking nothing would green-light pushing
# misformatted commits exactly when the repo state is unusual (no upstream,
# shallow clone, mid-rebase).
resolve_base() {
  local zeros="0000000000000000000000000000000000000000"
  local from="${PRE_COMMIT_FROM_REF:-}" upstream base
  if [ -n "$from" ] && [ "$from" != "$zeros" ] \
     && git rev-parse -q --verify "${from}^{commit}" >/dev/null 2>&1; then
    printf '%s\n' "$from"; return 0
  fi
  upstream="$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || echo "origin/${DEFAULT_BRANCH}")"
  base="$(git merge-base HEAD "$upstream" 2>/dev/null || true)"
  if [ -z "$base" ]; then
    cat >&2 <<EOF
ERROR: --changed could not resolve a diff base (no usable PRE_COMMIT_FROM_REF,
       and no merge-base between HEAD and ${upstream}). Refusing to silently
       check nothing. Fix one of:
  - git fetch origin ${DEFAULT_BRANCH}    (make origin/${DEFAULT_BRANCH} available)
  - git branch --set-upstream-to=origin/${DEFAULT_BRANCH}
  - or run tools/format.sh --check (full scope) instead.
EOF
    return 1
  fi
  printf '%s\n' "$base"
}
resolve_head() {
  if [ -n "${PRE_COMMIT_TO_REF:-}" ] \
     && git rev-parse -q --verify "${PRE_COMMIT_TO_REF}^{commit}" >/dev/null 2>&1; then
    printf '%s\n' "${PRE_COMMIT_TO_REF}"
  else
    printf 'HEAD\n'
  fi
}
changed_scope_files() {
  local base head f
  base="$(resolve_base)" || return 1
  head="$(resolve_head)"
  {
    git diff --name-only --diff-filter=ACMR "$base" "$head"
    git diff --name-only --diff-filter=ACMR HEAD
    git diff --name-only --cached --diff-filter=ACMR
  } 2>/dev/null | sort -u | while IFS= read -r f; do
    if [ -n "$f" ] && [ -f "$f" ] && in_scope "$f"; then printf '%s\n' "$f"; fi
  done
}

CF="$(resolve_cf)" || exit 1
echo "format.sh: using $CF ($("$CF" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)); scope=$SCOPE mode=$MODE" >&2

TMPL="$(mktemp)"
trap 'rm -f "$TMPL"' EXIT
if [ "$SCOPE" = "changed" ]; then changed_scope_files; else all_scope_files; fi > "$TMPL"
N="$(grep -c . "$TMPL" 2>/dev/null || true)"; N="${N:-0}"
if [ "$N" -eq 0 ]; then echo "format.sh: no in-scope files."; exit 0; fi

if [ "$MODE" = "check" ]; then
  tr '\n' '\0' < "$TMPL" | xargs -0 "$CF" --dry-run --Werror
  echo "format.sh: OK — $N file(s) already match clang-format ${PIN}."
else
  tr '\n' '\0' < "$TMPL" | xargs -0 "$CF" -i
  echo "format.sh: formatted $N file(s) with clang-format ${PIN}."
fi
