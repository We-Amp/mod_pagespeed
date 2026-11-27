#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# check-html-kernel-membership.sh - explicit-membership gate for lib/html/.
#
# The style-lint and license carve-outs for the vendored HTML kernel key on
# an EXPLICIT name list (lib/html has no uniform vendored-name prefix, and
# optimizer-owned files like html_metadata_extractor share the html_* shape).
# Names alone are spoofable: a hand-planted lib/html/html_node_evil.cc would
# dodge nothing, but a hand-edited SYNCED_FILES.txt or a stray file would
# blur the vendored/owned boundary. This script makes membership EXPLICIT
# and locally verifiable — no canonical-repo access needed:
#
#   * lib/html/SYNCED_FILES.txt (written by tools/sync-html-kernel.sh as
#     part of the synced output) lists every file the sync produced;
#   * a closed set of 2.0-owned files is declared below (compat shims,
#     build/docs/pin/license, and the optimizer-native lib/html files);
#   * every file that exists under lib/html/ must be in exactly one of those
#     two sets, and every listed synced file must exist. Anything else is
#     RED.
#
# Callers: tools/sync-html-kernel.sh --check, and the html-kernel-
# differential job in CI (runs on every main-targeted
# PR, so a planted file cannot merge quietly once that job is a required
# check). Works on a plain tree (CI tarball extract has no .git):
# enumeration is filesystem-based, which also catches untracked planted
# files locally.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "$REPO_ROOT"

MANIFEST="lib/html/SYNCED_FILES.txt"

# 2.0-owned files under lib/html/ (everything else must be in the manifest).
# compat/ ownership is FLAT and .h-ONLY: it holds the 2.0-authored shim
# headers and nothing else. A .cc or a nested file there is rejected here —
# compat headers are format- and license-gated and diff-visible, but
# full-mode clang-tidy excludes the vendored set, so we do not rely on lint
# to police compat contents.
is_owned() {
  case "$1" in
    lib/html/compat/*/*) return 1 ;;
    lib/html/compat/*.h) return 0 ;;
    # Build/docs/pin/ledger/license machinery.
    lib/html/BUILD|lib/html/CLAUDE.md|lib/html/HTML_KERNEL_PIN)
      return 0 ;;
    lib/html/LICENSE.apache-2.0|lib/html/SYNCED_FILES.txt)
      return 0 ;;
    # optimizer-native lib/html files (never synced; sync scope keeps these
    # forked on the 2.0 side by design).
    lib/html/csp_inline_policy.h|lib/html/csp_inline_policy.cc|\
lib/html/html_fuzz.cc|\
lib/html/html_metadata_extractor.h|lib/html/html_metadata_extractor.cc|\
lib/html/llms_txt_formatter.h|lib/html/llms_txt_formatter.cc|\
lib/html/markdown_extractor_filter.h|lib/html/markdown_extractor_filter.cc|\
lib/html/robots_ai_directives.h|lib/html/robots_ai_directives.cc|\
lib/html/safe_entity_decode.h|lib/html/safe_entity_decode.cc|\
lib/html/html5_named_entities.inc|\
lib/html/sitemap_parser.h|lib/html/sitemap_parser.cc|\
lib/html/subresource_collector_filter.h|lib/html/subresource_collector_filter.cc)
      return 0 ;;
  esac
  return 1
}

if [[ ! -f "$MANIFEST" ]]; then
  echo "ERROR: ${MANIFEST} missing — the sync tool writes it; a lib/html tree" >&2
  echo "       without it cannot prove vendored-set membership." >&2
  exit 1
fi

# Anti-vacuous: the manifest must plausibly cover the vendored set. At the
# current pin it lists exactly 27 entries (26 kernel/probe files + the
# manifest itself); a truncated or hand-edited manifest with fewer is RED.
listed=$(grep -c '^lib/html/' "$MANIFEST" || true)
if [[ "$listed" -lt 27 ]]; then
  echo "ERROR: ${MANIFEST} lists only ${listed} lib/html/ files (expected >= 27)" >&2
  echo "       — truncated or hand-edited manifest." >&2
  exit 1
fi

fail=0

# Symlinks have no business in a generated vendored tree: a symlink named
# into the vendored set would dodge `find -type f` enumeration and the style
# gates while pointing at content in an unlinted tree. Reject them outright,
# fail-closed.
while IFS= read -r l; do
  echo "RED: symlink under lib/html/: ${l} — symlinks are rejected here" >&2
  fail=1
done < <(find lib/html -type l | LC_ALL=C sort)

# Direction 1: every listed synced file must exist.
while IFS= read -r f; do
  [[ -z "$f" ]] && continue
  case "$f" in
    lib/html/*) ;;
    *) echo "RED: ${MANIFEST} entry outside lib/html/: '${f}'" >&2; fail=1; continue ;;
  esac
  if [[ ! -f "$f" ]]; then
    echo "RED: synced file listed in ${MANIFEST} is missing: ${f}" >&2
    fail=1
  fi
done < "$MANIFEST"

# Direction 2: every file that exists under lib/html/ is either synced or
# owned.
while IFS= read -r f; do
  if is_owned "$f"; then
    continue
  fi
  if ! grep -qxF "$f" "$MANIFEST"; then
    echo "RED: unexpected file under lib/html/: ${f}" >&2
    echo "     Not produced by tools/sync-html-kernel.sh (not in ${MANIFEST})" >&2
    echo "     and not a declared 2.0-owned file. Planted/stray files are" >&2
    echo "     rejected fail-closed; see lib/html/CLAUDE.md." >&2
    fail=1
  fi
done < <(find lib/html -type f | LC_ALL=C sort)

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
echo "OK — lib/html/ membership clean: $(find lib/html -type f | wc -l | tr -d ' ') files, ${listed} synced + owned set."
