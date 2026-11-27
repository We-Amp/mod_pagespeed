#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

set -euo pipefail

REQUIRED_HEADER="SPDX-License-Identifier: Apache-2.0"

# Paths to exclude (checked as prefix or pattern match)
should_skip() {
  local file="$1"

  # third_party/ holds no vendored sources (Bazel fetches them); its BUILD and
  # *.BUILD files are repo-authored build glue and ARE checked.
  case "$file" in
    third_party/BUILD|third_party/*.BUILD) ;;
    third_party/*|reference/mod_pagespeed/*|node_modules/*|.venv/*|bazel-*|lib/image/generated/*)
      return 0 ;;
  esac

  # The vendored JS-minifier corpus inputs (lib/js/corpus/bundle-fixtures/src,
  # lib/js/corpus/findings/repros) carry the SPDX header in the canonical 1.15
  # tree and arrive here byte-identical through the sync, so they are checked
  # like every other source file; the shared goldens manifest pins the
  # headered bytes. lib/js/SYNCED_FILES.txt membership is enforced separately
  # by tools/check-js-kernel-membership.sh.

  # Static HTML/CSS whose bytes are pinned or that is not ours to re-header:
  # the three HTML-parse corpus seeds whose test IS their exact byte content
  # (empty input, whitespace-only input, a document with no markup — every
  # other seed carries the header, mirrored from the canonical 1.15 tree),
  # the byte-frozen async-css-probe capture, and the archived 1.0
  # documentation under website/public/1.0/ (upstream Apache-2.0 text, not
  # the SPDX form). Ordinary test data and the website's own demo pages are
  # first-party and ARE checked. Mirrors .rat-excludes.
  case "$file" in
    *.html|*.css)
      case "$file" in
        website/public/1.0/*|tools/async-css-probe/fixtures/*|\
        tools/html-parse-corpus/seeds/empty.html|\
        tools/html-parse-corpus/seeds/whitespace-only.html|\
        tools/html-parse-corpus/seeds/text-only.html)
          return 0 ;;
      esac ;;
  esac

  # Config files
  case "$file" in
    *.bazelrc|*.gitignore|*.yaml|*.yml|*.json|*.toml|*.cfg|*.ini)
      return 0 ;;
  esac

  # Lock files
  case "$file" in
    MODULE.bazel.lock|pnpm-lock.yaml|*.lock)
      return 0 ;;
  esac

  # Test data / binary files
  case "$file" in
    *.rgba|*.gif|*.png|*.jpg|*.jpeg|*.webp|*.original|*.minified|*.avif)
      return 0 ;;
  esac

  # Documentation
  case "$file" in
    *.md) return 0 ;;
  esac

  # Env files
  case "$file" in
    .env|.env.*|*/.env|*/.env.*)
      return 0 ;;
  esac

  # Special files
  local basename
  basename="$(basename "$file")"
  case "$basename" in
    NOTICE|LICENSE|THIRD-PARTY-NOTICES|RELEASING.md|CONTRIBUTING.md|CLAUDE.md)
      return 0 ;;
  esac

  return 1
}

# Check if file is a source file that MUST carry the SPDX header
is_source_file() {
  local file="$1"
  local basename
  basename="$(basename "$file")"

  case "$basename" in
    BUILD|BUILD.bazel|MODULE.bazel|Dockerfile|Dockerfile.*) return 0 ;;
  esac

  case "$file" in
    *.c|*.cc|*.cpp|*.h|*.hpp|*.py|*.ts|*.js|*.mjs|*.cs|*.rs|*.svelte|*.sh|*.ps1|*.bzl|*.BUILD|*.t|*.hurl|*.gperf)
      return 0 ;;
  esac

  # Markup, stylesheets, MSBuild project files, SELinux policy sources and the
  # linker export lists: comment syntax differs, the SPDX line is the same.
  case "$file" in
    *.css|*.html|*.csproj|*.props|*.targets|*.def|*.lds|*.exp|*.fc|*.te)
      return 0 ;;
  esac

  return 1
}

# Files that are only scanned for header residue (no header is required — the
# website's .astro pages, components and layouts deliberately carry none — but
# a pre-relicense header must not survive in them either).
is_residue_scanned_file() {
  local file="$1"
  case "$file" in
    *.astro) return 0 ;;
  esac
  return 1
}

check_file() {
  local file="$1"

  if should_skip "$file"; then
    return 0
  fi

  if ! is_source_file "$file"; then
    return 0
  fi

  # Check first 10 lines for the required header
  if head -n 10 "$file" | grep -qF "$REQUIRED_HEADER"; then
    return 0
  fi

  return 1
}

# Header-window residue that must never come back in a first-party file: the
# pre-relicense SPDX identifier, its license text, and "All rights reserved"
# copyright lines. The last is legally harmless next to an Apache-2.0 grant
# (the license applies regardless of the phrase), but it sends a mixed signal
# to a reader of the header, so the sweep removes it as hygiene. Checked on
# the first 10 lines only — the same window the SPDX line must sit in — so
# prose deeper in a file (changelog history, test fixtures, copy) is not in
# scope.
RESIDUE_PATTERN='BUSL-1\.1|Business Source License|All rights reserved'

check_residue() {
  local file="$1"

  if should_skip "$file"; then
    return 0
  fi

  if ! is_source_file "$file" && ! is_residue_scanned_file "$file"; then
    return 0
  fi

  if head -n 10 "$file" | grep -qiE "$RESIDUE_PATTERN"; then
    return 1
  fi

  return 0
}

# Collect files to check
files=()

if [[ "${1:-}" == "--all" ]]; then
  # Find all source files in the repo
  while IFS= read -r -d '' file; do
    files+=("$file")
  done < <(git ls-files -z -- \
    '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' '*.py' '*.ts' '*.js' '*.mjs' '*.svelte' '*.sh' '*.bzl' \
    '*.cs' '*.rs' '*.ps1' '*.astro' '*.BUILD' '*.t' '*.hurl' '*.gperf' \
    '*.css' '*.html' '*.csproj' '*.props' '*.targets' '*.def' '*.lds' '*.exp' '*.fc' '*.te' \
    'BUILD' '*/BUILD' 'BUILD.bazel' '*/BUILD.bazel' 'MODULE.bazel' '*/MODULE.bazel' \
    'Dockerfile' '*/Dockerfile' 'Dockerfile.*' '*/Dockerfile.*')
else
  # Files passed as arguments (pre-commit mode)
  files=("$@")
fi

if [[ ${#files[@]} -eq 0 ]]; then
  exit 0
fi

missing=()
residue=()
for file in "${files[@]}"; do
  if [[ ! -f "$file" ]]; then
    continue
  fi
  if ! check_file "$file"; then
    missing+=("$file")
  fi
  if ! check_residue "$file"; then
    residue+=("$file")
  fi
done

status=0

if [[ ${#missing[@]} -gt 0 ]]; then
  echo "ERROR: The following files are missing the required license header:"
  echo "       ($REQUIRED_HEADER)"
  echo ""
  for file in "${missing[@]}"; do
    echo "  $file"
  done
  echo ""
  echo "Add the following to the top of each file (within the first 10 lines):"
  echo "  // SPDX-License-Identifier: Apache-2.0    (for C/C++/C#/JS/TS)"
  echo "  # SPDX-License-Identifier: Apache-2.0     (for Python/Shell/PowerShell/Bazel/Dockerfile/Perl/Hurl/SELinux)"
  echo "  /* SPDX-License-Identifier: Apache-2.0 */ (for CSS/linker version scripts)"
  echo "  <!-- SPDX-License-Identifier: Apache-2.0 -->  (for Svelte/HTML/MSBuild XML)"
  status=1
fi

if [[ ${#residue[@]} -gt 0 ]]; then
  echo "ERROR: The following files carry pre-relicense header residue in their"
  echo "       first 10 lines (matches: $RESIDUE_PATTERN):"
  echo ""
  for file in "${residue[@]}"; do
    echo "  $file"
  done
  echo ""
  echo "The tree is licensed under the Apache License 2.0 (see LICENSE). Replace"
  echo "the header with the copyright line(s) plus '$REQUIRED_HEADER'."
  status=1
fi

exit $status
