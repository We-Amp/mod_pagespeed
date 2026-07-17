#!/usr/bin/env bash

# Single entry point for C/C++ formatting. Byte-matches the CI clang-format lint.
#
# CI (the "clang-format check" workflow step, run in the lint Docker
# image) runs clang-format 20 over `pagespeed net`, excluding `pagespeed/iis/*`
# and vendored/generated files that carry a "DO NOT EDIT BY HAND" banner. The
# pinned version is the mirrors-clang-format rev in .pre-commit-config.yaml (and
# the lint image installs clang-format-20 from apt.llvm.org). This script
# resolves a clang-format 20.x binary the same way and formats/checks the
# identical scope, so a local run matches CI exactly — no remote formatter box
# required. It is the fix/verify counterpart of tools/fix-format.sh.
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
DEFAULT_BRANCH="master"

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

# --- repo CI scope (mirror of the CI workflow's "clang-format check") ---
# Skip vendored/generated files carrying the banner (e.g. license crypto
# vendored from ModPageSpeed 2.0, the design record — guarded by crypto-drift-check).
banner_free() { ! grep -q 'DO NOT EDIT BY HAND' "$1"; }
in_scope() {
  case "$1" in
    pagespeed/iis/*) return 1 ;;
    net/instaweb/rewriter/generated/*) return 1 ;;
    pagespeed/*|net/*)
      case "$1" in *.cc|*.cpp|*.h|*.hpp) return 0 ;; esac ;;
  esac
  return 1
}
all_scope_files() {
  local f
  find pagespeed net -type f \
    \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    -not -path 'pagespeed/iis/*' \
    -not -path 'net/instaweb/rewriter/generated/*' 2>/dev/null | sort | while IFS= read -r f; do
    if banner_free "$f"; then printf '%s\n' "$f"; fi
  done
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
    if [ -n "$f" ] && [ -f "$f" ] && in_scope "$f" && banner_free "$f"; then printf '%s\n' "$f"; fi
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
