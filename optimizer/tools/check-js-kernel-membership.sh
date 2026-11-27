#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# check-js-kernel-membership.sh - explicit-membership gate for lib/js/.
#
# The style-lint and license carve-outs for the vendored JS kernel key on
# filename shape (lib/js/js_*, lib/js/source_map.h, lib/js/corpus/**). Shape
# alone is spoofable: a hand-planted lib/js/js_backdoor.cc would dodge
# clang-tidy/clang-format by name while never having come through the sync
# tool. This script closes that hole by making membership EXPLICIT and
# locally verifiable — no canonical-repo access needed:
#
#   * lib/js/SYNCED_FILES.txt (written by tools/sync-js-kernel.sh as part of
#     the synced output) lists every file the sync produced;
#   * a small closed set of 2.0-owned files is declared below;
#   * every file that exists under lib/js/ must be in exactly one of those
#     two sets, and every listed synced file must exist. Anything else is RED.
#
# Callers: tools/sync-js-kernel.sh --check, and the js-kernel-differential
# job in CI (runs on every main-targeted PR, so a
# planted file cannot merge quietly once that job is a required check).
# Works on a plain tree (CI tarball extract has no .git): enumeration is
# filesystem-based, which also catches untracked planted files locally.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "$REPO_ROOT"

MANIFEST="lib/js/SYNCED_FILES.txt"

# 2.0-owned files under lib/js/ (everything else must be in the manifest).
# compat/ ownership is FLAT and .h-ONLY: it holds the five 2.0-authored
# shim headers and nothing else. A .cc or a nested file there is rejected
# here — compat headers are format- and license-gated and diff-visible, but
# full-mode clang-tidy excludes all of lib/js, so we do not rely on lint to
# police compat contents.
is_owned() {
  case "$1" in
    lib/js/compat/*/*) return 1 ;;
    lib/js/compat/*.h) return 0 ;;
    lib/js/BUILD|lib/js/corpus/BUILD|lib/js/CLAUDE.md|lib/js/JS_KERNEL_PIN)
      return 0 ;;
    lib/js/LICENSE.apache-2.0|lib/js/SYNCED_FILES.txt)
      return 0 ;;
    # 2.0-owned manual mirror of the canonical 1.15 fuzz harness (mpp
    # #688), never synced — the lib/html/html_fuzz.cc precedent; see
    # lib/js/CLAUDE.md.
    lib/js/js_minify_fuzz.cc)
      return 0 ;;
  esac
  return 1
}

if [[ ! -f "$MANIFEST" ]]; then
  echo "ERROR: ${MANIFEST} missing — the sync tool writes it; a lib/js tree" >&2
  echo "       without it cannot prove vendored-set membership." >&2
  exit 1
fi

# Anti-vacuous: the manifest must plausibly cover the vendored set.
listed=$(grep -c '^lib/js/' "$MANIFEST" || true)
if [[ "$listed" -lt 30 ]]; then
  echo "ERROR: ${MANIFEST} lists only ${listed} lib/js/ files (expected >= 30)" >&2
  echo "       — truncated or hand-edited manifest." >&2
  exit 1
fi

fail=0

# Symlinks have no business in a generated vendored tree: a symlink named
# into the vendored shape would dodge `find -type f` enumeration and the
# style gates while pointing at content in an unlinted tree. Reject them
# outright, fail-closed.
while IFS= read -r l; do
  echo "RED: symlink under lib/js/: ${l} — symlinks are rejected here" >&2
  fail=1
done < <(find lib/js -type l | LC_ALL=C sort)

# Direction 1: every listed synced file must exist.
while IFS= read -r f; do
  [[ -z "$f" ]] && continue
  case "$f" in
    lib/js/*) ;;
    *) echo "RED: ${MANIFEST} entry outside lib/js/: '${f}'" >&2; fail=1; continue ;;
  esac
  if [[ ! -f "$f" ]]; then
    echo "RED: synced file listed in ${MANIFEST} is missing: ${f}" >&2
    fail=1
  fi
done < "$MANIFEST"

# Direction 2: every file that exists under lib/js/ is either synced or owned.
while IFS= read -r f; do
  if is_owned "$f"; then
    continue
  fi
  if ! grep -qxF "$f" "$MANIFEST"; then
    echo "RED: unexpected file under lib/js/: ${f}" >&2
    echo "     Not produced by tools/sync-js-kernel.sh (not in ${MANIFEST})" >&2
    echo "     and not a declared 2.0-owned file. Planted/stray files are" >&2
    echo "     rejected fail-closed; see lib/js/CLAUDE.md." >&2
    fail=1
  fi
done < <(find lib/js -type f | LC_ALL=C sort)

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
echo "OK — lib/js/ membership clean: $(find lib/js -type f | wc -l | tr -d ' ') files, ${listed} synced + owned set."
