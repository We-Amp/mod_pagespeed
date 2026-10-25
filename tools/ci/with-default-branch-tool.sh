#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Print the path of a repository tool that a step of the master pipeline is
# about to run, taking it from the default branch when the checkout cannot
# supply it.
#
# Pull-request runs use pipeline-pr.yml at master, but check out the pull
# request's own commit. A branch that predates a tool (or a change to its
# command line) would otherwise fail with "No such file or directory" or an
# unknown-argument error. See "Tools a step runs from the checkout" in
# docs/ci/pipelines.md.
#
# Usage: tools/ci/with-default-branch-tool.sh [--prefer-default] [--with <path>]... <repo-relative path>
#   Run from the repository root (the Actions workspace).
#   Without the flag: the checkout's copy when it exists, else the default
#   branch's. With it: always the default branch's (for a tool whose command
#   line changed, where an old copy exists but is the wrong version).
#   The default branch's copy is written, together with the other files in
#   the same directory (siblings the tool may load), under
#   ${RUNNER_TEMP}/default-branch-tools/<dir>/. Fails closed when the file is
#   in neither place.
#   tools/ci/with-default-branch-tool.sh --self-test
#
# Env: DEFAULT_BRANCH_REMOTE (default: origin), DEFAULT_BRANCH (default: master).

set -euo pipefail

REMOTE="${DEFAULT_BRANCH_REMOTE:-origin}"
BRANCH="${DEFAULT_BRANCH:-master}"

resolve() { # <prefer-default 0|1> <path> [extra path]...
  local prefer="$1" path="$2" dir out name extra
  shift 2
  if [ "$prefer" = 0 ] && [ -f "$path" ]; then
    printf '%s\n' "$path"
    return 0
  fi
  # One retry: a network blip must not turn a gate red.
  git fetch --quiet --depth=1 "$REMOTE" "$BRANCH" >&2 \
    || { sleep "${DEFAULT_BRANCH_RETRY_SLEEP:-5}"; git fetch --quiet --depth=1 "$REMOTE" "$BRANCH" >&2; } \
    || {
      echo "::error::$path is not in this checkout and ${REMOTE}/${BRANCH} could not be fetched" >&2
      return 1
    }
  if ! git cat-file -e "FETCH_HEAD:${path}" 2>/dev/null; then
    echo "::error::$path is in neither this checkout nor ${BRANCH}" >&2
    return 1
  fi
  dir="$(dirname "$path")"
  out="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/default-branch-tools"
  mkdir -p "${out}/${dir}"
  git ls-tree --name-only FETCH_HEAD "${dir}/" | while IFS= read -r name; do
    # Blobs only (ls-tree lists sub-directories too).
    if [ "$(git cat-file -t "FETCH_HEAD:${name}")" = blob ]; then
      git show "FETCH_HEAD:${name}" > "${out}/${name}"
      if [ "$(git ls-tree FETCH_HEAD "$name" | cut -c1-6)" = 100755 ]; then chmod +x "${out}/${name}"; fi
    fi
  done
  for extra in "$@"; do
    mkdir -p "${out}/$(dirname "$extra")"
    git show "FETCH_HEAD:${extra}" > "${out}/${extra}" || return 1
  done
  echo "::notice::$path taken from ${BRANCH} (this branch predates it or has a different version)" >&2
  printf '%s\n' "${out}/${path}"
}

self_test() {
  local fails=0 tmp origin work got self
  self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/dbt-test.XXXXXX")"
  origin="${tmp}/origin"; work="${tmp}/work"
  pass() { echo "ok    $1"; }
  fail() { echo "FAIL  $1"; fails=$((fails + 1)); }

  git init -q -b master "$origin"
  git -C "$origin" config user.email t@example.invalid
  git -C "$origin" config user.name t
  mkdir -p "$origin/tools/x"
  echo 'echo new-tool' > "$origin/tools/x/tool.sh"
  echo 'sibling' > "$origin/tools/x/sibling.txt"
  git -C "$origin" add -A && git -C "$origin" commit -q -m init
  git init -q -b topic "$work"
  mkdir -p "$work/tools/x"
  echo 'echo old-tool' > "$work/tools/x/old.sh"
  echo 'echo stale-tool' > "$work/tools/x/tool-stale.sh"
  export RUNNER_TEMP="${tmp}/rt"; mkdir -p "$RUNNER_TEMP"
  export DEFAULT_BRANCH_REMOTE="file://${origin}"

  # 1: present in the checkout -> the checkout's copy, no fetch needed.
  got="$(cd "$work" && DEFAULT_BRANCH_REMOTE=file:///nonexistent bash "$self" tools/x/old.sh 2>/dev/null)" || true
  if [ "$got" = tools/x/old.sh ]; then pass "present in checkout"; else fail "present in checkout (got '$got')"; fi

  # 2: absent -> fetched from the default branch, siblings alongside.
  got="$(cd "$work" && bash "$self" tools/x/tool.sh 2>/dev/null)" || got=""
  if [ "$got" = "${RUNNER_TEMP}/default-branch-tools/tools/x/tool.sh" ] \
     && grep -q new-tool "$got" && [ -f "$(dirname "$got")/sibling.txt" ]; then
    pass "absent -> fetched with siblings"
  else
    fail "absent -> fetched with siblings (got '$got')"
  fi

  # 3: --prefer-default takes the default branch's copy even when one exists.
  echo 'echo old-version' > "$work/tools/x/tool.sh"
  got="$(cd "$work" && bash "$self" --prefer-default tools/x/tool.sh 2>/dev/null)" || got=""
  if [ -n "$got" ] && [ "$got" != tools/x/tool.sh ] && grep -q new-tool "$got"; then
    pass "--prefer-default overrides an old copy"
  else
    fail "--prefer-default overrides an old copy (got '$got')"
  fi

  # 3b: --prefer-default with the file absent from the checkout still fetches.
  rm -f "$work/tools/x/tool.sh"
  got="$(cd "$work" && bash "$self" --prefer-default tools/x/tool.sh 2>/dev/null)" || got=""
  if [ -n "$got" ] && grep -q new-tool "$got"; then pass "--prefer-default, absent in checkout"; else fail "--prefer-default, absent in checkout (got '$got')"; fi

  # 4: --with also extracts a named repo file next to the tool tree.
  got="$(cd "$work" && bash "$self" --prefer-default --with tools/x/sibling.txt tools/x/tool.sh 2>/dev/null)" || got=""
  if [ -n "$got" ] && [ -f "${RUNNER_TEMP}/default-branch-tools/tools/x/sibling.txt" ]; then
    pass "--with extracts the named file"
  else
    fail "--with extracts the named file"
  fi

  # 5: in neither place -> fails closed.
  if (cd "$work" && bash "$self" tools/x/none.sh >/dev/null 2>&1); then
    fail "missing everywhere must fail"
  else
    pass "missing everywhere fails closed"
  fi

  # 6: an unreachable remote fails closed after the retry.
  if (cd "$work" && DEFAULT_BRANCH_RETRY_SLEEP=0 DEFAULT_BRANCH_REMOTE="file://${tmp}/nonexistent" bash "$self" tools/x/tool.sh >/dev/null 2>&1); then
    fail "unreachable remote must fail"
  else
    pass "unreachable remote fails closed"
  fi

  rm -rf "$tmp"
  if [ "$fails" -ne 0 ]; then echo "self-test: $fails failure(s)"; return 1; fi
  echo "with-default-branch-tool self-test: ok"
}

prefer=0; extras=()
while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) self_test; exit $? ;;
    --prefer-default) prefer=1; shift ;;
    --with) [ $# -ge 2 ] || { echo "usage: $0 [--prefer-default] [--with <path>]... <path>" >&2; exit 2; }
            extras+=("$2"); shift 2 ;;
    -*) echo "usage: $0 [--prefer-default] [--with <path>]... <path>" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ $# -eq 1 ] || { echo "usage: $0 [--prefer-default] [--with <path>]... <path>" >&2; exit 2; }
resolve "$prefer" "$1" "${extras[@]+"${extras[@]}"}"
