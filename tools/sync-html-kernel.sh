#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# sync-html-kernel.sh - Vendor the canonical HTML kernel from mod_pagespeed 1.15.
#
# This repository does NOT own its HTML lexer/parser kernel. Since #1130
# (cloning the JS-kernel machinery) the 1.15 repository
# (mod_pagespeed) is the single canonical source: fixes land there
# first and flow here through this script. The vendored copy under lib/html/
# is CHECKED IN so the 2.0 build stays hermetic, and
# the required differential job runs this script with --check against the
# canonical repository at the pin (tools/ci/check-kernel-bytes.sh) and fails
# if the checked-in files would change — a hand-edit of a vendored file, or a
# pin bump without a re-sync, can never drift silently.
# Hand edits to vendored files are NEVER sanctioned; fix the canonical source
# and re-sync.
#
# The pin lives in lib/html/HTML_KERNEL_PIN (first token of the first
# non-comment line = the canonical 1.15 commit SHA). Bump the pin, re-run
# this script, and commit the result together.
#
# Pin-downgrade ratchet (D1 #1104, cloned): the same file records a
# forward-only floor on a second line, `floor <sha>`. The ancestry gate below
# alone would accept ANY ancestor of canonical master, so re-pinning to an
# older-but-complete commit yields a self-consistent tree that passes both
# the byte check and the differential gates. The floor closes that window: this
# script fails (exit 6) unless the pin equals the floor or has it as an
# ancestor. Convention: a routine pin bump moves the floor to the new pin in
# the same commit.
# tools/ci/check-floor-ratchet.sh additionally enforces that the floor
# itself never moves backwards relative to origin/main — as a REQUIRED check
# in the html-kernel-differential job of pipeline-pr.yml.
#
# Usage:
#   tools/sync-html-kernel.sh --source <path-to-1.15-git-checkout-or-mirror>
#   tools/sync-html-kernel.sh --source <path> --check   # byte check (no writes)
#
# --source may be any local git dir (worktree, clone, bare mirror) in which
# the pinned commit is reachable; only `git show` is used, never the working
# tree. --check regenerates into a temp dir and byte-compares against the
# committed tree: exit 0 = in sync, exit 1 = drift (offending files listed).
# Every sync also (re)writes lib/html/SYNCED_FILES.txt — the explicit ledger
# of the synced set that tools/check-html-kernel-membership.sh enforces
# tree-locally (also invoked by --check).
#
# Pin ancestry: the pinned commit must be an ancestor of the source's master
# (origin/master if present, else master) — a merged, permanent commit. There
# is no override: a pin on an unmerged branch is refused (exit 4). Wait for
# the merge and pin the merged commit.
#
# Transform (deterministic):
#   * anchored ^#include rewrites per the fixed mapping table below
#     (pagespeed/kernel/html -> lib/html, http/content_type.h -> lib/html,
#     google_url + message_handler family -> lib/html/compat/, other
#     kernel/base headers -> lib/html/compat/ or lib/base/);
#   * NO source-code rewrites beyond the include lines. The canonical kernel
#     no longer uses the StringPiece::CopyToString member call (retired
#     canonical-side, mpp #671), so the vendored files compile against the
#     2.0 compat StringPiece — a TRUE alias of std::string_view, which cannot
#     supply that member. A residue check below fails the sync loudly if the
#     canonical source ever reintroduces the call.
#   * a provenance banner is prepended; the original license header of each
#     canonical file is retained byte-for-byte below it (the vendored kernel
#     files carry their Apache-2.0 header — see lib/html/LICENSE.apache-2.0).
#
# Scope note: html_parse_probe.cc and html_parse_recorder.h ride the pin
# (D1 probe precedent: the D2 differential goldens gate both repos' probes).
# They were briefly held out of the synced set because canonical
# html_parse_recorder.h used the StringPiece member AppendToString — absent
# from the 2.0 compat's TRUE std::string_view alias; mpp #675 reconciled
# that canonical-side (append(data(), size())), so they joined the set.
# The residue check below still greps AppendToString so a reintroduction
# anywhere in the synced set fails loudly. The parse corpus itself
# (tools/html-parse-corpus/) is vendored separately and byte-aligned; it is
# NOT part of this sync. optimizer-owned lib/html files
# (subresource_collector_filter, sitemap_parser, markdown_extractor_filter,
# robots_ai_directives, llms_txt_formatter, html_metadata_extractor,
# safe_entity_decode, csp_inline_policy, html_fuzz) stay 2.0-owned and are
# never touched by this script; tools/check-html-kernel-membership.sh
# enumerates them explicitly.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PIN_FILE="${REPO_ROOT}/lib/html/HTML_KERNEL_PIN"
DEST_ROOT="${REPO_ROOT}"

SRC_REPO=""
CHECK_MODE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --source) SRC_REPO="${2:-}"; shift 2 ;;
    --check)  CHECK_MODE=1; shift ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$SRC_REPO" ]]; then
  echo "Usage: $0 --source <path-to-mod_pagespeed-checkout> [--check]" >&2
  exit 2
fi

if [[ ! -f "$PIN_FILE" ]]; then
  echo "ERROR: pin file not found: ${PIN_FILE}" >&2
  exit 2
fi

# First token of the first non-comment, non-empty line is the pin SHA.
# (awk reads the file directly: no head-in-pipeline SIGPIPE under pipefail.)
PIN="$(awk '!/^[[:space:]]*(#|$)/ {print $1; exit}' "$PIN_FILE")"
if ! [[ "$PIN" =~ ^[0-9a-f]{40}$ ]]; then
  echo "ERROR: ${PIN_FILE} does not start with a full 40-hex commit SHA (got '${PIN}')." >&2
  exit 2
fi

# Pin-downgrade ratchet, part 1 — parse the recorded floor (#1104). The
# `floor <sha>` line is keyword-keyed so the first-non-comment-line = pin
# contract stays intact for every other consumer of this file.
FLOOR_COUNT="$(awk '$1 == "floor" {n++} END {print n + 0}' "$PIN_FILE")"
if [[ "$FLOOR_COUNT" -ne 1 ]]; then
  echo "ERROR: ${PIN_FILE} must contain exactly one 'floor <sha>' line (found ${FLOOR_COUNT})." >&2
  echo "       A duplicate floor line is inert to the keyword parsers but ambiguous" >&2
  echo "       to a reviewer; a missing one disables the ratchet. Both are refused." >&2
  exit 6
fi
FLOOR="$(awk '$1 == "floor" {print $2; exit}' "$PIN_FILE")"
if ! [[ "$FLOOR" =~ ^[0-9a-f]{40}$ ]]; then
  echo "ERROR: ${PIN_FILE} has no valid 'floor <40-hex-sha>' line (got '${FLOOR:-<missing>}')." >&2
  echo "       The pin-downgrade ratchet requires one; on a routine pin bump the" >&2
  echo "       floor moves to the new pin in the same commit. See lib/html/CLAUDE.md." >&2
  exit 6
fi

if ! git -C "$SRC_REPO" cat-file -e "${PIN}^{commit}" 2>/dev/null; then
  echo "ERROR: pinned 1.15 commit ${PIN} not reachable in '${SRC_REPO}'." >&2
  echo "       Fetch the mod_pagespeed ref that carries it, or fix" >&2
  echo "       lib/html/HTML_KERNEL_PIN." >&2
  exit 3
fi

# Ancestry gate: the pin must be a permanent (merged) master commit.
MASTER_REF=""
for ref in origin/master master; do
  if git -C "$SRC_REPO" rev-parse --verify --quiet "$ref" >/dev/null; then
    MASTER_REF="$ref"
    break
  fi
done
if [[ -z "$MASTER_REF" ]]; then
  echo "ERROR: neither origin/master nor master resolvable in '${SRC_REPO}';" >&2
  echo "       cannot verify pin ancestry." >&2
  exit 3
fi
if ! git -C "$SRC_REPO" merge-base --is-ancestor "$PIN" "$MASTER_REF"; then
  echo "ERROR: pin ${PIN:0:12} is not an ancestor of ${MASTER_REF} in '${SRC_REPO}'." >&2
  echo "       Merged pins only: wait for the 1.15 merge and pin the merged commit." >&2
  exit 4
fi

# Pin-downgrade ratchet, part 2 — enforce the floor (#1104). Runs IN
# ADDITION to the ancestry gate above. Exit 6 is this check's distinct code.
if ! git -C "$SRC_REPO" cat-file -e "${FLOOR}^{commit}" 2>/dev/null; then
  echo "ERROR: floor commit ${FLOOR} not reachable in '${SRC_REPO}' —" >&2
  echo "       cannot evaluate the pin-downgrade ratchet. Fetch the" >&2
  echo "       mod_pagespeed ref that carries it, or fix the floor" >&2
  echo "       line in lib/html/HTML_KERNEL_PIN." >&2
  exit 3
fi
if [[ "$FLOOR" != "$PIN" ]] && \
   ! git -C "$SRC_REPO" merge-base --is-ancestor "$FLOOR" "$PIN"; then
  echo "ERROR: pin-downgrade ratchet: pin ${PIN:0:12} is neither equal to nor a" >&2
  echo "       descendant of the recorded floor ${FLOOR:0:12} (lib/html/HTML_KERNEL_PIN)." >&2
  echo "       Re-pinning below the floor is refused — the floor only moves" >&2
  echo "       forward. If this downgrade is intentional it must be done in the" >&2
  echo "       open: lower the floor line in the same commit and justify it in" >&2
  echo "       review (CI's RATCHET check will flag the rewrite). See" >&2
  echo "       lib/html/CLAUDE.md." >&2
  exit 6
fi

short="${PIN:0:12}"

# ---------------------------------------------------------------------------
# The vendored set (canonical path -> lib/html path) and the transform
# ---------------------------------------------------------------------------

# Kernel + probe + recorder: get the include-mapping transform and the
# banner. Format: "<canonical-repo-path>:<dest-path-under-repo-root>".
# This is the #1130 13-file scope (26 files with headers, html_name as
# .h+.gperf, content_type from kernel/http) plus the D2 differential probe
# and recorder (D1 probe precedent; see the scope note in the header).
TRANSFORMED=(
  "pagespeed/kernel/html/doctype.h:lib/html/doctype.h"
  "pagespeed/kernel/html/doctype.cc:lib/html/doctype.cc"
  "pagespeed/kernel/html/empty_html_filter.h:lib/html/empty_html_filter.h"
  "pagespeed/kernel/html/empty_html_filter.cc:lib/html/empty_html_filter.cc"
  "pagespeed/kernel/html/html_element.h:lib/html/html_element.h"
  "pagespeed/kernel/html/html_element.cc:lib/html/html_element.cc"
  "pagespeed/kernel/html/html_event.h:lib/html/html_event.h"
  "pagespeed/kernel/html/html_event.cc:lib/html/html_event.cc"
  "pagespeed/kernel/html/html_filter.h:lib/html/html_filter.h"
  "pagespeed/kernel/html/html_filter.cc:lib/html/html_filter.cc"
  "pagespeed/kernel/html/html_keywords.h:lib/html/html_keywords.h"
  "pagespeed/kernel/html/html_keywords.cc:lib/html/html_keywords.cc"
  "pagespeed/kernel/html/html_lexer.h:lib/html/html_lexer.h"
  "pagespeed/kernel/html/html_lexer.cc:lib/html/html_lexer.cc"
  "pagespeed/kernel/html/html_name.h:lib/html/html_name.h"
  "pagespeed/kernel/html/html_name.gperf:lib/html/html_name.gperf"
  "pagespeed/kernel/html/html_node.h:lib/html/html_node.h"
  "pagespeed/kernel/html/html_node.cc:lib/html/html_node.cc"
  "pagespeed/kernel/html/html_parse.h:lib/html/html_parse.h"
  "pagespeed/kernel/html/html_parse.cc:lib/html/html_parse.cc"
  "pagespeed/kernel/html/html_writer_filter.h:lib/html/html_writer_filter.h"
  "pagespeed/kernel/html/html_writer_filter.cc:lib/html/html_writer_filter.cc"
  "pagespeed/kernel/html/html_parse_probe.cc:lib/html/html_parse_probe.cc"
  "pagespeed/kernel/html/html_parse_recorder.h:lib/html/html_parse_recorder.h"
  "pagespeed/kernel/http/content_type.h:lib/html/content_type.h"
  "pagespeed/kernel/http/content_type.cc:lib/html/content_type.cc"
)

# Anchored, include-line-only rewrites. This table plus the compat headers
# under lib/html/compat/ and the existing lib/base/ headers must jointly
# account for every non-std include across the vendored set (asserted below
# by the residue check).
#
# Two mapping classes:
#   * lib/html/compat/*.h — 2.0-owned adaptation shims for canonical headers
#     lib/base has no equivalent of (string/string_util additions, logging,
#     sparse_hash_map, string_hash, stl_util, timer, message_handler family,
#     google_url policy seam, html_page_limit).
#   * lib/base/*.h — the 2.0-native equivalents of the canonical base
#     headers (same net_instaweb namespace, API-compatible: basictypes,
#     arena, atom, inline_slist, printf_format, symbol_table, writer).
#     Mapping directly avoids duplicating them under lib/html/compat/.
transform_includes() {
  sed -e 's|^#include "pagespeed/kernel/html/html_page_limit.h"|#include "lib/html/compat/html_page_limit.h"|' \
      -e 's|^#include "pagespeed/kernel/html/|#include "lib/html/|' \
      -e 's|^#include "pagespeed/kernel/http/content_type.h"|#include "lib/html/content_type.h"|' \
      -e 's|^#include "pagespeed/kernel/http/google_url.h"|#include "lib/html/compat/google_url.h"|' \
      -e 's|^#include "pagespeed/kernel/base/message_handler.h"|#include "lib/html/compat/message_handler.h"|' \
      -e 's|^#include "pagespeed/kernel/base/null_message_handler.h"|#include "lib/html/compat/message_handler.h"|' \
      -e 's|^#include "pagespeed/kernel/base/print_message_handler.h"|#include "lib/html/compat/message_handler.h"|' \
      -e 's|^#include "pagespeed/kernel/base/string.h"|#include "lib/html/compat/string.h"|' \
      -e 's|^#include "pagespeed/kernel/base/string_util.h"|#include "lib/html/compat/string_util.h"|' \
      -e 's|^#include "pagespeed/kernel/base/sparse_hash_map.h"|#include "lib/html/compat/sparse_hash_map.h"|' \
      -e 's|^#include "pagespeed/kernel/base/string_hash.h"|#include "lib/html/compat/string_hash.h"|' \
      -e 's|^#include "pagespeed/kernel/base/stl_util.h"|#include "lib/html/compat/stl_util.h"|' \
      -e 's|^#include "pagespeed/kernel/base/timer.h"|#include "lib/html/compat/timer.h"|' \
      -e 's|^#include "pagespeed/kernel/base/basictypes.h"|#include "lib/base/basictypes.h"|' \
      -e 's|^#include "pagespeed/kernel/base/arena.h"|#include "lib/base/arena.h"|' \
      -e 's|^#include "pagespeed/kernel/base/atom.h"|#include "lib/base/atom.h"|' \
      -e 's|^#include "pagespeed/kernel/base/inline_slist.h"|#include "lib/base/inline_slist.h"|' \
      -e 's|^#include "pagespeed/kernel/base/printf_format.h"|#include "lib/base/printf_format.h"|' \
      -e 's|^#include "pagespeed/kernel/base/symbol_table.h"|#include "lib/base/symbol_table.h"|' \
      -e 's|^#include "pagespeed/kernel/base/writer.h"|#include "lib/base/writer.h"|' \
      -e 's|^#include "base/logging.h"|#include "lib/html/compat/logging.h"|'
}

SPDX_LINE='// SPDX-License-Identifier: Apache-2.0'

banner() {
  # $1 = canonical repo path, $2 = 1 if the canonical file already opens with
  # the SPDX line (do not duplicate it).
  if [[ "$2" != "1" ]]; then
    printf '%s\n' "$SPDX_LINE"
  fi
  cat <<EOF
// GENERATED — DO NOT EDIT BY HAND. Synced by tools/sync-html-kernel.sh.
// Canonical source: mod_pagespeed 1.15, ${1} (#1130).
// Synced from the commit pinned in lib/html/HTML_KERNEL_PIN.
// Drift guard: CI re-runs tools/sync-html-kernel.sh and byte-compares.
// The canonical file's original license header is retained in full below;
// Apache-2.0 headed files: see lib/html/LICENSE.apache-2.0 for the license text.

EOF
}

# ---------------------------------------------------------------------------
# Sync into a staging dir, verify, then install (or byte-compare in --check)
# ---------------------------------------------------------------------------

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/html-kernel-sync.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

emit() {  # $1 canonical path, $2 dest rel path
  local src_path="$1" dest_rel="$2"
  local out="${STAGE}/${dest_rel}"
  mkdir -p "$(dirname "$out")"
  local first_line has_spdx=0
  # sed -n 1p consumes the whole stream: no SIGPIPE under pipefail.
  first_line="$(git -C "$SRC_REPO" show "${PIN}:${src_path}" | sed -n '1p')"
  [[ "$first_line" == "$SPDX_LINE" ]] && has_spdx=1
  {
    banner "$src_path" "$has_spdx"
    git -C "$SRC_REPO" show "${PIN}:${src_path}" | transform_includes
  } > "$out"
}

# --- Anti-vacuous assertion, evaluated BEFORE anything is emitted: a sync
# that would write nothing (or almost nothing) must fail, not silently pass.
# It runs first on purpose — a degenerate/empty file set must trip THIS
# check rather than crash an emit loop (an empty array expansion under
# `set -u` is not a reliable failure signal). 26 files at the current pin
# (the 13-file scope with headers + probe + recorder);
# canonical-side additions only grow this.
MIN_EXPECTED=26
planned=${#TRANSFORMED[@]}
if [[ "$planned" -lt "$MIN_EXPECTED" ]]; then
  echo "ERROR: sync would write only ${planned} files (expected >= ${MIN_EXPECTED})" >&2
  echo "       — vacuous sync; refusing to touch lib/html/." >&2
  exit 5
fi

count=0
for entry in "${TRANSFORMED[@]}"; do
  emit "${entry%%:*}" "${entry#*:}"
  count=$((count + 1))
done

# Emitted-count reconciliation: guards against an emit that silently skips.
staged_files=$(find "$STAGE" -type f | wc -l | tr -d ' ')
if [[ "$count" -ne "$planned" ]] || [[ "$staged_files" -ne "$planned" ]]; then
  echo "ERROR: planned ${planned} files, emitted ${count}, staged ${staged_files} — mismatch." >&2
  exit 5
fi

# --- Synced-set manifest: lib/html/SYNCED_FILES.txt lists every file this
# sync produced (including itself). It is the explicit-membership ledger that
# tools/check-html-kernel-membership.sh verifies tree-locally — the
# shape-keyed lint/license carve-outs are only trusted for files recorded
# here.
manifest_body="$(cd "$STAGE" && find . -type f | sed 's|^\./||')"
{ printf '%s\n' "$manifest_body"; echo "lib/html/SYNCED_FILES.txt"; } \
  | LC_ALL=C sort > "${STAGE}/lib/html/SYNCED_FILES.txt"
count=$((count + 1))

# --- StringPiece member-call residue: the canonical kernel retired
# CopyToString (mpp #671), and the compat StringPiece (a true
# std::string_view alias) cannot supply member calls — a canonical
# reintroduction must fail HERE, loudly, not later at compile time in a
# consumer build. AppendToString is checked for the same reason (the D1
# retirement; canonical html_parse_recorder.h used it until mpp #675
# reconciled it, which is what let the probe/recorder join the synced
# set).
if grep -rnE '\.(CopyToString|AppendToString)\(|->(CopyToString|AppendToString)\(' "${STAGE}/lib/html" >/dev/null 2>&1; then
  echo "ERROR: StringPiece member-call residue after transform:" >&2
  grep -rnE '\.(CopyToString|AppendToString)\(|->(CopyToString|AppendToString)\(' "${STAGE}/lib/html" >&2
  exit 5
fi

# --- Residue assertion: no canonical include path may survive the transform.
if grep -rn '"pagespeed/kernel\|"base/logging' "${STAGE}/lib/html" >/dev/null 2>&1; then
  echo "ERROR: canonical include residue after transform:" >&2
  grep -rn '"pagespeed/kernel\|"base/logging' "${STAGE}/lib/html" >&2
  exit 5
fi

# --- Vendored-name invariant: the style-lint carve-outs (ci.yml,
# .pre-commit-config.yaml) match vendored files by an EXPLICIT name list —
# lib/html has no uniform vendored-name prefix, and optimizer-owned files
# (html_metadata_extractor, subresource_collector_filter, ...) share the
# html_* shape, so a glob would silently excuse owned files from the gates.
# Every synced file must be one of the exact names below; anything else
# fails the sync rather than riding the carve-outs.
is_vendored_name() {
  case "$1" in
    lib/html/content_type.h|lib/html/content_type.cc|\
lib/html/doctype.h|lib/html/doctype.cc|\
lib/html/empty_html_filter.h|lib/html/empty_html_filter.cc|\
lib/html/html_element.h|lib/html/html_element.cc|\
lib/html/html_event.h|lib/html/html_event.cc|\
lib/html/html_filter.h|lib/html/html_filter.cc|\
lib/html/html_keywords.h|lib/html/html_keywords.cc|\
lib/html/html_lexer.h|lib/html/html_lexer.cc|\
lib/html/html_name.h|lib/html/html_name.gperf|\
lib/html/html_node.h|lib/html/html_node.cc|\
lib/html/html_parse.h|lib/html/html_parse.cc|\
lib/html/html_parse_probe.cc|lib/html/html_parse_recorder.h|\
lib/html/html_writer_filter.h|lib/html/html_writer_filter.cc|\
lib/html/SYNCED_FILES.txt)
      return 0 ;;
  esac
  return 1
}
while IFS= read -r f; do
  rel="${f#"${STAGE}/"}"
  if ! is_vendored_name "$rel"; then
    echo "ERROR: synced file '${rel}' violates the vendored-name invariant" >&2
    echo "       (the explicit vendored-name list in tools/sync-html-kernel.sh);" >&2
    echo "       the lint carve-out would not cover it." >&2
    exit 5
  fi
done < <(find "$STAGE" -type f)

if [[ "$CHECK_MODE" == "1" ]]; then
  drift=0
  while IFS= read -r f; do
    rel="${f#"${STAGE}/"}"
    if ! cmp -s "$f" "${DEST_ROOT}/${rel}"; then
      echo "DRIFT: ${rel}"
      drift=1
    fi
  done < <(find "$STAGE" -type f | sort)
  if [[ "$drift" -ne 0 ]]; then
    echo "ERROR: vendored HTML kernel does not match the pinned 1.15 source (${short})." >&2
    echo "       Re-run tools/sync-html-kernel.sh --source <1.15-checkout> and commit," >&2
    echo "       or revert the hand-edit. See lib/html/CLAUDE.md." >&2
    exit 1
  fi
  # Byte-compare above proves synced-set -> tree; the membership gate proves
  # the CONVERSE: nothing exists under lib/html/ that is neither synced nor
  # 2.0-owned (a hand-planted lib/html/html_evil.cc would otherwise ride the
  # name-keyed carve-outs unseen).
  "${SCRIPT_DIR}/check-html-kernel-membership.sh"
  echo "OK — vendored HTML kernel matches the pinned 1.15 source (${short}, ${count} files)."
  exit 0
fi

# Install: replace the synced set wholesale so canonical-side deletions
# propagate — first remove everything the PREVIOUS sync produced (the old
# tree's own ledger, if any), then the new set, then copy the new set in.
# Only ledger-recorded names are ever removed; lib/html/compat/, BUILD,
# CLAUDE.md, HTML_KERNEL_PIN, LICENSE.apache-2.0 and the optimizer-owned
# filters/parsers (see check-html-kernel-membership.sh) are 2.0-owned and
# untouched.
#
# Ledger-entry validation (fail-closed): a ledger is repo CONTENT — the old
# tree's SYNCED_FILES.txt could be hand-edited to plant a traversal entry
# (e.g. ../outside.txt) that the rm loop below would happily delete. Every
# entry, from EITHER ledger, must therefore pass the traversal guards AND an
# explicit tool-controlled name set BEFORE any rm; an invalid entry aborts
# the sync loudly instead of being skipped or acted on. The removable set is
# the CURRENT vendored names plus FORMERLY_VENDORED (files un-vendored by a
# scope change — their removal from trees synced by an older tool is exactly
# how scope shrinks propagate; drop entries once no supported tree's ledger
# can contain them). This matches D1's safety level (its rm set is
# hardcoded) by keeping every rm target inside an explicit name set in this
# tool — a planted ledger entry for any other name (e.g. lib/html/BUILD, or
# anything outside lib/html/) is refused.
# Currently empty: html_parse_probe.cc/html_parse_recorder.h sat here
# while un-vendored over the AppendToString incompatibility and re-entered
# the vendored set when mpp #675 reconciled it canonical-side.
FORMERLY_VENDORED=()
is_removable_name() {  # $1 = entry
  local rel="$1" f
  if is_vendored_name "$rel"; then
    return 0
  fi
  for f in ${FORMERLY_VENDORED[@]+"${FORMERLY_VENDORED[@]}"}; do
    [[ "$rel" == "$f" ]] && return 0
  done
  return 1
}
validate_ledger_entry() {  # $1 = entry; exits the sync (rc 5) on invalid
  local rel="$1"
  case "$rel" in
    *..*|/*|*\ *)
      echo "ERROR: unsafe path in synced-set ledger: '${rel}' — refusing to" >&2
      echo "       install. Ledger entries must be plain relative paths under" >&2
      echo "       lib/html/; a hand-edited SYNCED_FILES.txt is not a sync" >&2
      echo "       instruction." >&2
      exit 5 ;;
  esac
  if ! is_removable_name "$rel"; then
    echo "ERROR: ledger entry '${rel}' is not in the removable name set" >&2
    echo "       (the vendored-name allowlist plus FORMERLY_VENDORED in" >&2
    echo "       tools/sync-html-kernel.sh) — refusing to remove it. The" >&2
    echo "       install step only ever removes tool-controlled names." >&2
    exit 5
  fi
}
if [[ -f "${DEST_ROOT}/lib/html/SYNCED_FILES.txt" ]]; then
  while IFS= read -r rel; do
    [[ -z "$rel" ]] && continue
    validate_ledger_entry "$rel"
    rm -f "${DEST_ROOT}/${rel}"
  done < "${DEST_ROOT}/lib/html/SYNCED_FILES.txt"
fi
while IFS= read -r rel; do
  [[ -z "$rel" ]] && continue
  validate_ledger_entry "$rel"
  rm -f "${DEST_ROOT}/${rel}"
done < "${STAGE}/lib/html/SYNCED_FILES.txt"
( cd "$STAGE" && find . -type f | while IFS= read -r f; do
    rel="${f#./}"
    mkdir -p "${DEST_ROOT}/$(dirname "$rel")"
    cp "$f" "${DEST_ROOT}/${rel}"
  done )

echo "Done. Vendored ${count} HTML-kernel files into lib/html/ from 1.15 @ ${short}."
