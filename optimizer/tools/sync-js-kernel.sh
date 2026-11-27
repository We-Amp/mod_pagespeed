#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# sync-js-kernel.sh - Vendor the canonical JS kernel from mod_pagespeed 1.15.
#
# This repository does NOT own its JavaScript tokenizer/minifier kernel.
# The 1.15 repository (mod_pagespeed) is the single
# canonical source: fixes land there first and flow here through this script.
# The vendored copy under lib/js/ is CHECKED IN so the 2.0 build stays
# hermetic, and the required differential job runs this script with --check
# against the canonical repository at the pin (tools/ci/check-kernel-bytes.sh)
# and fails if the checked-in files would change — a hand-edit of a vendored
# file, or a pin bump without a re-sync, can never drift silently. Hand edits
# to vendored files are NEVER sanctioned; fix the canonical source and re-sync.
#
# The pin lives in lib/js/JS_KERNEL_PIN (first token of the first
# non-comment line = the canonical 1.15 commit SHA). Bump the pin, re-run
# this script, and commit the result together.
#
# Pin-downgrade ratchet (#1104): the same file records a forward-only floor
# on a second line, `floor <sha>`. The ancestry gate below alone would
# accept ANY ancestor of canonical master, so re-pinning to an
# older-but-complete commit yields a self-consistent tree that passes both
# the byte check and the differential gates. The floor closes that window: this
# script fails (exit 6) unless the pin equals the floor or has it as an
# ancestor. Convention: a routine pin bump moves the floor to the new pin
# in the same commit.
# tools/ci/check-floor-ratchet.sh additionally enforces that the floor
# itself never moves backwards relative to origin/main — as a REQUIRED
# check in the js-kernel-differential job of pipeline-pr.yml.
#
# Usage:
#   tools/sync-js-kernel.sh --source <path-to-1.15-git-checkout-or-mirror>
#   tools/sync-js-kernel.sh --source <path> --check   # byte check (no writes)
#
# --source may be any local git dir (worktree, clone, bare mirror) in which
# the pinned commit is reachable; only `git show` is used, never the working
# tree. --check regenerates into a temp dir and byte-compares against the
# committed tree: exit 0 = in sync, exit 1 = drift (offending files listed).
# Every sync also (re)writes lib/js/SYNCED_FILES.txt — the explicit ledger
# of the synced set that tools/check-js-kernel-membership.sh enforces
# tree-locally (also invoked by --check).
#
# Pin ancestry: the pinned commit must be an ancestor of the source's master
# (origin/master if present, else master) — a merged, permanent commit. There
# is no override: a pin on an unmerged branch is refused (exit 4). Wait for
# the merge and pin the merged commit.
#
# Transform (deterministic):
#   * anchored ^#include rewrites per the fixed mapping table below
#     (pagespeed/kernel/js -> lib/js, base/util headers -> lib/js/compat/);
#   * NO source-code rewrites beyond the include lines. The canonical
#     kernel no longer uses the StringPiece::AppendToString member call
#     (retired canonical-side), so the vendored files compile against the
#     2.0 compat StringPiece — a TRUE alias of std::string_view, which
#     cannot supply that member. A residue check below fails the sync
#     loudly if the canonical source ever reintroduces the call.
#   * a provenance banner is prepended; the original license header of each
#     canonical file is retained byte-for-byte below it (the vendored kernel
#     files carry their Apache-2.0 header — see lib/js/LICENSE.apache-2.0).
# Corpus files (gen_goldens.py, generate_synthetic.py, goldens manifest,
# checked-in corpus inputs) are copied VERBATIM: the goldens manifest pins
# the generators' sha256 and every input's sha256, so any transformation
# would break the shared expectation that gates both repos.
#
# NOTE for source_map.h: vendored HEADER-ONLY by design. source_map.cc
# (Encode and friends) drags a jsoncpp dependency and has zero 2.0 callers;
# net_instaweb::source_map::Encode stays declared-but-undefined here, and a
# link error on first use is the documentation of that decision.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PIN_FILE="${REPO_ROOT}/lib/js/JS_KERNEL_PIN"
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
  echo "       floor moves to the new pin in the same commit. See lib/js/CLAUDE.md." >&2
  exit 6
fi

if ! git -C "$SRC_REPO" cat-file -e "${PIN}^{commit}" 2>/dev/null; then
  echo "ERROR: pinned 1.15 commit ${PIN} not reachable in '${SRC_REPO}'." >&2
  echo "       Fetch the mod_pagespeed ref that carries it, or fix" >&2
  echo "       lib/js/JS_KERNEL_PIN." >&2
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
  echo "       line in lib/js/JS_KERNEL_PIN." >&2
  exit 3
fi
if [[ "$FLOOR" != "$PIN" ]] && \
   ! git -C "$SRC_REPO" merge-base --is-ancestor "$FLOOR" "$PIN"; then
  echo "ERROR: pin-downgrade ratchet: pin ${PIN:0:12} is neither equal to nor a" >&2
  echo "       descendant of the recorded floor ${FLOOR:0:12} (lib/js/JS_KERNEL_PIN)." >&2
  echo "       Re-pinning below the floor is refused — the floor only moves" >&2
  echo "       forward. If this downgrade is intentional it must be done in the" >&2
  echo "       open: lower the floor line in the same commit and justify it in" >&2
  echo "       review (CI's RATCHET check will flag the rewrite). See" >&2
  echo "       lib/js/CLAUDE.md." >&2
  exit 6
fi

short="${PIN:0:12}"

# ---------------------------------------------------------------------------
# The vendored set (canonical path -> lib/js path) and the transform
# ---------------------------------------------------------------------------

# Kernel + probe: get the include-mapping transform and the banner.
# Format: "<canonical-repo-path>:<dest-path-under-repo-root>"
TRANSFORMED=(
  "pagespeed/kernel/js/js_keywords.h:lib/js/js_keywords.h"
  "pagespeed/kernel/js/js_keywords.cc:lib/js/js_keywords.cc"
  "pagespeed/kernel/js/js_tokenizer.h:lib/js/js_tokenizer.h"
  "pagespeed/kernel/js/js_tokenizer.cc:lib/js/js_tokenizer.cc"
  "pagespeed/kernel/js/js_minify.h:lib/js/js_minify.h"
  "pagespeed/kernel/js/js_minify.cc:lib/js/js_minify.cc"
  "pagespeed/kernel/base/source_map.h:lib/js/source_map.h"
  "tools/js-minify-corpus/js_minify_probe.cc:lib/js/corpus/js_minify_probe.cc"
)

# Corpus machinery: byte-for-byte (hash-load-bearing: the goldens manifest
# records the generators' and inputs' sha256).
VERBATIM=(
  "tools/js-minify-corpus/gen_goldens.py:lib/js/corpus/gen_goldens.py"
  "tools/js-minify-corpus/generate_synthetic.py:lib/js/corpus/generate_synthetic.py"
  "tools/js-minify-corpus/goldens/manifest.json:lib/js/corpus/goldens/manifest.json"
)
# Checked-in corpus inputs, enumerated at the pin so additions in 1.15 flow
# through on a re-pin without editing this script.
for dir in "tools/js-minify-corpus/bundle-fixtures/src" \
           "tools/js-minify-corpus/findings/repros"; do
  while IFS= read -r path; do
    case "$path" in
      *.js|*.mjs)
        VERBATIM+=("${path}:lib/js/corpus/${path#tools/js-minify-corpus/}") ;;
    esac
  done < <(git -C "$SRC_REPO" ls-tree -r --name-only "$PIN" -- "$dir")
done

# Anchored, include-line-only rewrites. This table plus the compat headers
# under lib/js/compat/ must jointly account for every non-std include across
# the vendored set (asserted below by the residue check).
transform_includes() {
  sed -e 's|^#include "pagespeed/kernel/js/|#include "lib/js/|' \
      -e 's|^#include "pagespeed/kernel/base/source_map.h"|#include "lib/js/source_map.h"|' \
      -e 's|^#include "pagespeed/kernel/base/string.h"|#include "lib/js/compat/string.h"|' \
      -e 's|^#include "pagespeed/kernel/base/string_util.h"|#include "lib/js/compat/string_util.h"|' \
      -e 's|^#include "pagespeed/kernel/base/basictypes.h"|#include "lib/js/compat/basictypes.h"|' \
      -e 's|^#include "pagespeed/kernel/util/re2.h"|#include "lib/js/compat/re2.h"|' \
      -e 's|^#include "base/logging.h"|#include "lib/js/compat/logging.h"|'
}

SPDX_LINE='// SPDX-License-Identifier: Apache-2.0'

banner() {
  # $1 = canonical repo path, $2 = 1 if the canonical file already opens with
  # the SPDX line (do not duplicate it).
  if [[ "$2" != "1" ]]; then
    printf '%s\n' "$SPDX_LINE"
  fi
  cat <<EOF
// GENERATED — DO NOT EDIT BY HAND. Synced by tools/sync-js-kernel.sh.
// Canonical source: mod_pagespeed 1.15, ${1}.
// Synced from the commit pinned in lib/js/JS_KERNEL_PIN.
// Drift guard: CI re-runs tools/sync-js-kernel.sh and byte-compares.
// The canonical file's original license header is retained in full below;
// Apache-2.0 headed files: see lib/js/LICENSE.apache-2.0 for the license text.

EOF
}

# ---------------------------------------------------------------------------
# Sync into a staging dir, verify, then install (or byte-compare in --check)
# ---------------------------------------------------------------------------

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/js-kernel-sync.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

emit() {  # $1 canonical path, $2 dest rel path, $3 transform? (1/0)
  local src_path="$1" dest_rel="$2" do_transform="$3"
  local out="${STAGE}/${dest_rel}"
  mkdir -p "$(dirname "$out")"
  if [[ "$do_transform" == "1" ]]; then
    local first_line has_spdx=0
    # sed -n 1p consumes the whole stream: no SIGPIPE under pipefail.
    first_line="$(git -C "$SRC_REPO" show "${PIN}:${src_path}" | sed -n '1p')"
    [[ "$first_line" == "$SPDX_LINE" ]] && has_spdx=1
    {
      banner "$src_path" "$has_spdx"
      git -C "$SRC_REPO" show "${PIN}:${src_path}" | transform_includes
    } > "$out"
  else
    git -C "$SRC_REPO" show "${PIN}:${src_path}" > "$out"
  fi
}

# --- Anti-vacuous assertion, evaluated BEFORE anything is emitted: a sync
# that would write nothing (or almost nothing) must fail, not silently pass.
# It runs first on purpose — a degenerate/empty file set must trip THIS
# check rather than crash an emit loop (an empty array expansion under
# `set -u` is not a reliable failure signal). 37 files at the initial pin;
# canonical-side additions only grow this.
MIN_EXPECTED=30
planned=$(( ${#TRANSFORMED[@]} + ${#VERBATIM[@]} ))
if [[ "$planned" -lt "$MIN_EXPECTED" ]]; then
  echo "ERROR: sync would write only ${planned} files (expected >= ${MIN_EXPECTED})" >&2
  echo "       — vacuous sync; refusing to touch lib/js/." >&2
  exit 5
fi

count=0
for entry in "${TRANSFORMED[@]}"; do
  emit "${entry%%:*}" "${entry#*:}" 1
  count=$((count + 1))
done
for entry in "${VERBATIM[@]}"; do
  emit "${entry%%:*}" "${entry#*:}" 0
  count=$((count + 1))
done

# Emitted-count reconciliation: guards against an emit that silently skips.
staged_files=$(find "$STAGE" -type f | wc -l | tr -d ' ')
if [[ "$count" -ne "$planned" ]] || [[ "$staged_files" -ne "$planned" ]]; then
  echo "ERROR: planned ${planned} files, emitted ${count}, staged ${staged_files} — mismatch." >&2
  exit 5
fi

# --- Synced-set manifest: lib/js/SYNCED_FILES.txt lists every file this sync
# produced (including itself). It is the explicit-membership ledger that
# tools/check-js-kernel-membership.sh verifies tree-locally — the shape-keyed
# lint/license carve-outs are only trusted for files recorded here.
manifest_body="$(cd "$STAGE" && find . -type f | sed 's|^\./||')"
{ printf '%s\n' "$manifest_body"; echo "lib/js/SYNCED_FILES.txt"; } \
  | LC_ALL=C sort > "${STAGE}/lib/js/SYNCED_FILES.txt"
count=$((count + 1))

# --- AppendToString residue: the canonical kernel retired this member call,
# and the compat StringPiece (a true std::string_view alias) cannot supply
# it — a canonical reintroduction must fail HERE, loudly, not later at
# compile time in a consumer build.
if grep -rn '\.AppendToString(' "${STAGE}/lib/js" >/dev/null 2>&1; then
  echo "ERROR: AppendToString member-call residue after transform:" >&2
  grep -rn '\.AppendToString(' "${STAGE}/lib/js" >&2
  exit 5
fi

# --- Residue assertion: no canonical include path may survive the transform.
if grep -rn '"pagespeed/kernel\|"base/logging' "${STAGE}/lib/js" >/dev/null 2>&1; then
  echo "ERROR: canonical include residue after transform:" >&2
  grep -rn '"pagespeed/kernel\|"base/logging' "${STAGE}/lib/js" >&2
  exit 5
fi

# --- Vendored-name invariant: the style-lint carve-out (ci.yml,
# .pre-commit-config.yaml) matches vendored files by name (lib/js/js_*,
# lib/js/source_map.h, lib/js/corpus/**). A synced file outside that shape
# would silently re-enter the lint gates in 1.15 idiom — fail instead.
while IFS= read -r f; do
  rel="${f#"${STAGE}/"}"
  case "$rel" in
    lib/js/js_*|lib/js/source_map.h|lib/js/corpus/*|lib/js/SYNCED_FILES.txt) ;;
    *)
      echo "ERROR: synced file '${rel}' violates the vendored-name invariant" >&2
      echo "       (lib/js/js_*, lib/js/source_map.h, lib/js/corpus/**);" >&2
      echo "       the lint carve-out would not cover it." >&2
      exit 5 ;;
  esac
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
    echo "ERROR: vendored JS kernel does not match the pinned 1.15 source (${short})." >&2
    echo "       Re-run tools/sync-js-kernel.sh --source <1.15-checkout> and commit," >&2
    echo "       or revert the hand-edit. See lib/js/CLAUDE.md." >&2
    exit 1
  fi
  # Byte-compare above proves synced-set -> tree; the membership gate proves
  # the CONVERSE: nothing exists under lib/js/ that is neither synced nor
  # 2.0-owned (a hand-planted lib/js/js_backdoor.cc would otherwise ride the
  # shape-keyed carve-outs unseen).
  "${SCRIPT_DIR}/check-js-kernel-membership.sh"
  echo "OK — vendored JS kernel matches the pinned 1.15 source (${short}, ${count} files)."
  exit 0
fi

# Install: replace the synced set wholesale so canonical-side deletions
# propagate. Only the synced shapes are removed; lib/js/compat/, BUILD,
# CLAUDE.md, JS_KERNEL_PIN, LICENSE.apache-2.0 are 2.0-owned and untouched.
# The 2.0-owned js_minify_fuzz.cc mirror matches the js_*.cc shape but is
# NOT synced (membership-gate declared, the lib/html/html_fuzz.cc
# precedent) — exclude it explicitly from the wholesale removal.
rm -f "${DEST_ROOT}"/lib/js/js_*.h "${DEST_ROOT}/lib/js/source_map.h" \
      "${DEST_ROOT}/lib/js/SYNCED_FILES.txt"
find "${DEST_ROOT}/lib/js" -maxdepth 1 -name 'js_*.cc' \
     ! -name 'js_minify_fuzz.cc' -delete
rm -rf "${DEST_ROOT}/lib/js/corpus/js_minify_probe.cc" \
       "${DEST_ROOT}/lib/js/corpus/gen_goldens.py" \
       "${DEST_ROOT}/lib/js/corpus/generate_synthetic.py" \
       "${DEST_ROOT}/lib/js/corpus/goldens" \
       "${DEST_ROOT}/lib/js/corpus/bundle-fixtures" \
       "${DEST_ROOT}/lib/js/corpus/findings"
( cd "$STAGE" && find . -type f | while IFS= read -r f; do
    rel="${f#./}"
    mkdir -p "${DEST_ROOT}/$(dirname "$rel")"
    cp "$f" "${DEST_ROOT}/${rel}"
  done )

echo "Done. Vendored ${count} JS-kernel files into lib/js/ from 1.15 @ ${short}."
