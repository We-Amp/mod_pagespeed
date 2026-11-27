#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI doc-drift guard (BLOCKING): clang-format / clang-tidy version literals.
#
# CLAUDE.md and AGENTS.md state the pinned C++ formatter/linter major in several
# places. The source of truth is the mirrors-clang-format rev in
# .pre-commit-config.yaml (which CI mirrors via clang-format-20 /
# run-clang-tidy-20). If a doc literal disagrees with that rev, an agent installs
# the wrong toolchain and
# red-lines the exact CI gate the doc exists to prevent (the recurring
# drift). This check fails the build so drift is caught at review time instead
# of a CI round-trip.
#
# Run locally: bash tools/ci/check_doc_commands.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PRECOMMIT=".pre-commit-config.yaml"
# Every agent-facing doc that may embed a clang-format/clang-tidy major. Keep the
# pre-commit `files:` regex (id: doc-version-literals) in sync with this list.
DOCS=(CLAUDE.md AGENTS.md)
FATAL=0

# ---------------------------------------------------------------------------
# clang-format/clang-tidy version literals must match the pinned rev.
# ---------------------------------------------------------------------------
if [ -f "$PRECOMMIT" ]; then
  # Pinned rev lives on the `rev:` line that follows the mirrors-clang-format
  # repo entry. Grab the line after that repo declaration. Expected form
  # `rev: v20.1.8`.
  PINNED_REV="$(awk '
    /mirrors-clang-format/ { in_block = 1; next }
    in_block && /rev:/ {
      gsub(/.*rev:[ \t]*/, "");
      gsub(/[ \t#].*/, "");
      print;
      exit
    }
  ' "$PRECOMMIT")"

  if [ -z "$PINNED_REV" ]; then
    echo "ERROR: could not read the mirrors-clang-format rev from $PRECOMMIT" >&2
    FATAL=1
  else
    # Major version from a rev like v20.1.8 -> 20.
    PINNED_MAJOR="$(printf '%s' "$PINNED_REV" | sed -E 's/^v?([0-9]+).*/\1/')"
    if ! [[ "$PINNED_MAJOR" =~ ^[0-9]+$ ]]; then
      echo "ERROR: could not parse a major version from pinned rev '$PINNED_REV'" >&2
      FATAL=1
    else
      echo "Pinned clang-format rev: $PINNED_REV (major $PINNED_MAJOR)"

      MISMATCH=0

      for DOC in "${DOCS[@]}"; do
        [ -f "$DOC" ] || continue

        # 2a. Tool-name literals that embed a major:
        #     clang-format-N / clang-tidy-N / run-clang-tidy-N / llvm@N.
        #     Each must use the pinned major.
        while IFS=: read -r lineno literal; do
          [ -z "$literal" ] && continue
          major="$(printf '%s' "$literal" | sed -E 's/.*[-@]([0-9]+)$/\1/')"
          if [ "$major" != "$PINNED_MAJOR" ]; then
            echo "ERROR: $DOC:$lineno '$literal' uses major $major but the pinned clang-format major is $PINNED_MAJOR" >&2
            MISMATCH=1
          fi
        done < <(grep -noE '(run-clang-tidy|clang-format|clang-tidy)-[0-9]+|llvm@[0-9]+' "$DOC")

        # 2b. "N.x" version phrasing, but only on lines that talk about
        #     clang-format/clang-tidy (so unrelated "X.x" text can't false-positive).
        while IFS=: read -r lineno content; do
          for tok in $(printf '%s' "$content" | grep -oE '[0-9]+\.x'); do
            major="${tok%%.x}"
            if [ "$major" != "$PINNED_MAJOR" ]; then
              echo "ERROR: $DOC:$lineno '$tok' (clang-format/clang-tidy context) uses major $major but the pinned major is $PINNED_MAJOR" >&2
              MISMATCH=1
            fi
          done
        done < <(grep -niE 'clang-format|clang-tidy' "$DOC" | grep -E '[0-9]+\.x')

        # 2c. Any explicit full rev literal (vNN.NN.NN) on a line that also names
        #     .pre-commit-config.yaml or clang-format must equal the pinned rev
        #     exactly (catches a stale "currently v20.1.7" annotation).
        while IFS=: read -r lineno content; do
          for tok in $(printf '%s' "$content" | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+'); do
            if [ "$tok" != "$PINNED_REV" ]; then
              echo "ERROR: $DOC:$lineno '$tok' does not match the pinned mirrors-clang-format rev '$PINNED_REV'" >&2
              MISMATCH=1
            fi
          done
        done < <(grep -nE 'pre-commit-config\.yaml|clang-format' "$DOC" | grep -E 'v[0-9]+\.[0-9]+\.[0-9]+')

      done

      if [ "$MISMATCH" -eq 0 ]; then
        echo "OK: CLAUDE.md / AGENTS.md clang-format/clang-tidy version literals match the pinned rev."
      else
        FATAL=1
      fi
    fi
  fi
else
  echo "NOTE: $PRECOMMIT missing — skipping version-literal check."
fi

if [ "$FATAL" -ne 0 ]; then
  echo ""
  echo "FAILED: documentation version literals drifted from the source of truth." >&2
  exit 1
fi

echo "OK: doc-command check passed."
exit 0
