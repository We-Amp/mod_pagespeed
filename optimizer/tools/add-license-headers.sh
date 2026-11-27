#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Add or replace license headers on all source files in the mod_pagespeed project.
# Idempotent: safe to run multiple times.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Counters
COUNT_A=0
COUNT_B=0
COUNT_C=0
COUNT_D=0
COUNT_E=0
COUNT_SKIP=0

# --- Header templates ---

HEADER_APACHE_C="// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V."

HEADER_PORTED_C="// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation."

HEADER_APACHE_HASH="# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V."

# --- Helper: strip old license header from C/C++/Rust/TS/Svelte files ---
#
# Two-pass approach:
#   Pass 1: Read all lines. Walk from the top (after optional shebang),
#           tracking whether we are inside a "license zone" — contiguous
#           block of comment lines and blanks that contain at least one
#           license-indicator keyword.  Record the last line number that
#           is part of the license zone.
#   Pass 2: Output shebang (if any), then lines after the license zone,
#           skipping leading blank lines between the old header and content.
#
# License-indicator keywords (case-insensitive):
#   license, copyright, apache, spdx, warranty, permission, condition,
#   distribute, contributor, "AS IS", derived, modified, originally

strip_old_c_header() {
  local file="$1"
  local tmp
  tmp=$(mktemp)

  awk '
  BEGIN {
    n = 0
    shebang = ""
    last_license_line = 0
    start = 1          # first line to scan (after shebang)
  }

  # Slurp all lines
  { lines[++n] = $0 }

  END {
    # Detect shebang
    if (n > 0 && lines[1] ~ /^#!/) {
      shebang = lines[1]
      start = 2
    }

    # Walk forward through comment/blank lines looking for license zone
    in_block = 0
    i = start
    while (i <= n) {
      line = lines[i]

      # Inside a /* */ block comment
      if (in_block) {
        if (is_license_keyword(line)) last_license_line = i
        if (line ~ /\*\//) in_block = 0
        i++
        continue
      }

      # Start of a /* */ block comment
      if (line ~ /^[[:space:]]*\/\*/) {
        if (is_license_keyword(line)) last_license_line = i
        if (line !~ /\*\//) in_block = 1
        i++
        continue
      }

      # Single-line // comment
      if (line ~ /^\/\//) {
        if (is_license_keyword(line)) last_license_line = i
        i++
        continue
      }

      # C-style block-comment continuation: " * text"
      if (line ~ /^[[:space:]]*\*/) {
        if (is_license_keyword(line)) last_license_line = i
        i++
        continue
      }

      # Blank line — might be between license lines or after them
      if (line ~ /^[[:space:]]*$/) {
        i++
        continue
      }

      # Non-comment, non-blank: stop scanning
      break
    }

    # If no license keywords found, output everything unchanged
    if (last_license_line == 0) {
      for (j = 1; j <= n; j++) print lines[j]
    } else {
      # Output shebang if present
      if (shebang != "") print shebang

      # Skip from start to last_license_line, then skip trailing blanks
      j = last_license_line + 1

      # Skip trailing blank lines, stray block-comment closers, and empty // separators
      while (j <= n) {
        line = lines[j]
        # Skip blank lines between header and content
        if (line ~ /^[[:space:]]*$/) { j++; continue }
        # Skip stray block-comment close ( */ ) left after stripping
        if (line ~ /^[[:space:]]*\*\/[[:space:]]*$/) { j++; continue }
        # Skip lone // separator lines (empty comment, no text)
        if (line ~ /^\/\/[[:space:]]*$/) { j++; continue }
        break
      }

      # Output remaining lines
      for (k = j; k <= n; k++) print lines[k]
    }
  }

  function is_license_keyword(s) {
    low = tolower(s)
    if (low ~ /licens/) return 1
    if (low ~ /copyright/) return 1
    if (low ~ /apache/) return 1
    if (low ~ /spdx/) return 1
    if (low ~ /warranty/) return 1
    if (low ~ /permission/) return 1
    if (low ~ /condition/) return 1
    if (low ~ /distribute/) return 1
    if (low ~ /contributor/) return 1
    if (low ~ /as is/) return 1
    if (low ~ /derived/) return 1
    if (low ~ /modified/) return 1
    if (low ~ /originally/) return 1
    if (low ~ /we-amp/) return 1
    return 0
  }
  ' "$file" > "$tmp"

  cat "$tmp" > "$file"
  rm -f "$tmp"
}

# --- Helper: strip old header from hash-comment files (sh, py, bzl, BUILD) ---

strip_old_hash_header() {
  local file="$1"
  local tmp
  tmp=$(mktemp)

  awk '
  BEGIN { in_header = 1; found_content = 0 }

  # Always pass through shebang
  NR == 1 && /^#!/ { print; next }

  !in_header { print; next }

  # License lines to strip
  /^# SPDX-License-Identifier/ { next }
  /^# Copyright \(c\)/ { next }
  /^#$/ && !found_content { next }

  # Blank lines before content
  /^$/ && !found_content { next }

  # Anything else is content
  { in_header = 0; found_content = 1; print }
  ' "$file" > "$tmp"

  cat "$tmp" > "$file"
  rm -f "$tmp"
}

# --- Helper: prepend header to file (after shebang if present) ---

prepend_header() {
  local file="$1"
  local header="$2"
  local tmp
  tmp=$(mktemp)

  if head -1 "$file" | grep -q '^#!'; then
    # Has shebang: keep it, insert header after
    head -1 "$file" > "$tmp"
    echo "" >> "$tmp"
    echo "$header" >> "$tmp"
    echo "" >> "$tmp"
    tail -n +2 "$file" >> "$tmp"
  else
    echo "$header" >> "$tmp"
    echo "" >> "$tmp"
    cat "$file" >> "$tmp"
  fi

  cat "$tmp" > "$file"
  rm -f "$tmp"
}

# --- Helper: check if file has ASF header ---

has_asf_header() {
  head -20 "$1" | grep -q "Licensed to the Apache Software Foundation" 2>/dev/null
}

# --- Helper: check if already has correct SPDX header ---

already_has_spdx() {
  local file="$1"
  local expected_marker="$2"
  # Find the SPDX line (within first 5 lines, accounting for shebang)
  if head -5 "$file" | grep -q "^// SPDX-License-Identifier: Apache-2.0" 2>/dev/null ||
     head -5 "$file" | grep -q "^# SPDX-License-Identifier: Apache-2.0" 2>/dev/null; then
    # Check for the distinguishing marker within header
    if head -10 "$file" | grep -qF "$expected_marker" 2>/dev/null; then
      return 0
    fi
  fi
  return 1
}

# --- Process a C-style file (C/C++/Rust/TS) ---

process_c_file() {
  local file="$1"
  local header="$2"
  local category="$3"

  # Check idempotency
  local marker
  if [[ "$category" == "B" ]]; then
    marker="This file is derived from mod_pagespeed"
  else
    marker="Copyright (c) 2024-2026 We-Amp B.V."
  fi

  if already_has_spdx "$file" "$marker"; then
    return 1  # already correct
  fi

  strip_old_c_header "$file"
  prepend_header "$file" "$header"
  return 0
}

# --- Process a hash-comment file ---

process_hash_file() {
  local file="$1"

  if already_has_spdx "$file" "Copyright (c) 2024-2026 We-Amp B.V."; then
    return 1
  fi

  strip_old_hash_header "$file"
  prepend_header "$file" "$HEADER_APACHE_HASH"
  return 0
}

# --- Helper: is this the script itself? ---

is_self() {
  [[ "$(realpath "$1")" == "$(realpath "$PROJECT_ROOT/tools/add-license-headers.sh")" ]]
}

# ============================================================================
# Category A: src/**/*.{h,cc,cpp} and test/**/*.{h,cc,cpp} -- all Apache-2.0
# ============================================================================

echo "=== Category A: New code (Apache-2.0) ==="

while IFS= read -r -d '' file; do
  if process_c_file "$file" "$HEADER_APACHE_C" "A"; then
    echo "  [A] $file"
    ((COUNT_A++)) || true
  fi
done < <(find "$PROJECT_ROOT/src" "$PROJECT_ROOT/test" -type f \( -name '*.h' -o -name '*.cc' -o -name '*.cpp' \) -print0)

# Category A: lib/ files WITHOUT ASF headers (new code)
while IFS= read -r -d '' file; do
  # Skip generated files
  if [[ "$file" == *"/lib/image/generated/"* ]]; then
    ((COUNT_SKIP++)) || true
    continue
  fi
  if ! has_asf_header "$file"; then
    if process_c_file "$file" "$HEADER_APACHE_C" "A"; then
      echo "  [A] $file"
      ((COUNT_A++)) || true
    fi
  fi
done < <(find "$PROJECT_ROOT/lib" -type f \( -name '*.h' -o -name '*.cc' -o -name '*.cpp' \) -print0)

# Category A: C++ files in tools/ (keygen.cc, quality-curves/, seed-test-cache.cc, etc.)
while IFS= read -r -d '' file; do
  if [[ "$file" == *"/node_modules/"* ]] || [[ "$file" == *"/.venv/"* ]]; then
    continue
  fi
  if process_c_file "$file" "$HEADER_APACHE_C" "A"; then
    echo "  [A] $file"
    ((COUNT_A++)) || true
  fi
done < <(find "$PROJECT_ROOT/tools" -type f \( -name '*.h' -o -name '*.cc' -o -name '*.cpp' \) -print0)

# Category A: Rust FFI file
if [[ -f "$PROJECT_ROOT/lib/image/vtracer_ffi/src/lib.rs" ]]; then
  file="$PROJECT_ROOT/lib/image/vtracer_ffi/src/lib.rs"
  if process_c_file "$file" "$HEADER_APACHE_C" "A"; then
    echo "  [A] $file"
    ((COUNT_A++)) || true
  fi
fi

# ============================================================================
# Category B: lib/ files WITH ASF headers (ported from mod_pagespeed)
# ============================================================================

echo "=== Category B: Ported code (dual license) ==="

while IFS= read -r -d '' file; do
  # Skip generated files
  if [[ "$file" == *"/lib/image/generated/"* ]]; then
    continue
  fi
  if has_asf_header "$file"; then
    if process_c_file "$file" "$HEADER_PORTED_C" "B"; then
      echo "  [B] $file"
      ((COUNT_B++)) || true
    fi
  fi
done < <(find "$PROJECT_ROOT/lib" -type f \( -name '*.h' -o -name '*.cc' -o -name '*.cpp' \) -print0)

# Category B: gperf file
if [[ -f "$PROJECT_ROOT/lib/html/html_name.gperf" ]]; then
  file="$PROJECT_ROOT/lib/html/html_name.gperf"
  if has_asf_header "$file"; then
    marker="This file is derived from mod_pagespeed"
    if ! already_has_spdx "$file" "$marker"; then
      tmp=$(mktemp)
      # The gperf file has two license blocks (before and after %{).
      # Strip all old license blocks and prepend the new header.
      awk '
      BEGIN { n = 0 }
      { lines[++n] = $0 }
      END {
        # Walk and find all license block-comment regions
        # Mark lines to skip
        for (i = 1; i <= n; i++) skip[i] = 0

        in_block = 0
        for (i = 1; i <= n; i++) {
          if (in_block) {
            skip[i] = 1
            if (lines[i] ~ /\*\//) in_block = 0
            continue
          }
          # Block comment with license keywords
          if (lines[i] ~ /^\/\*/ && is_lic(lines[i])) {
            skip[i] = 1
            if (lines[i] !~ /\*\//) in_block = 1
            continue
          }
          # // style license lines
          if (lines[i] ~ /^\/\// && is_lic(lines[i])) {
            skip[i] = 1
            continue
          }
          # SPDX/Copyright lines
          if (lines[i] ~ /^\/\/ SPDX-License-Identifier/) { skip[i] = 1; continue }
          if (lines[i] ~ /^\/\/ Copyright \(c\)/) { skip[i] = 1; continue }
        }

        # Remove blank lines adjacent to skipped regions
        for (i = 1; i <= n; i++) {
          if (!skip[i] && lines[i] ~ /^[[:space:]]*$/) {
            # Check if previous non-blank was skipped or next non-blank is skipped
            all_skip_before = 1
            for (j = 1; j < i; j++) {
              if (!skip[j] && lines[j] !~ /^[[:space:]]*$/) all_skip_before = 0
            }
            if (all_skip_before) skip[i] = 1
          }
        }

        # Output header before first non-skipped line
        header_emitted = 0
        for (i = 1; i <= n; i++) {
          if (skip[i]) continue
          if (!header_emitted) {
            printf "// SPDX-License-Identifier: Apache-2.0\n"
            printf "// Copyright (c) 2024-2026 We-Amp B.V.\n"
            printf "//\n"
            printf "// This file is derived from mod_pagespeed and has been substantially modified.\n"
            printf "// Originally licensed under Apache License, Version 2.0.\n"
            printf "// Copyright (c) 2010-2017 Google Inc.\n"
            printf "// Copyright (c) 2018 The Apache Software Foundation.\n"
            printf "\n"
            header_emitted = 1
          }
          print lines[i]
        }
      }

      function is_lic(s) {
        low = tolower(s)
        if (low ~ /licens/) return 1
        if (low ~ /copyright/) return 1
        if (low ~ /apache/) return 1
        if (low ~ /spdx/) return 1
        if (low ~ /we-amp/) return 1
        if (low ~ /ported/) return 1
        return 0
      }
      ' "$file" > "$tmp"
      cat "$tmp" > "$file"
      rm -f "$tmp"
      echo "  [B] $file"
      ((COUNT_B++)) || true
    fi
  fi
fi

# ============================================================================
# Category C: Cyclone Cache
# ============================================================================

echo "=== Category C: Cyclone Cache ==="

if [[ -d "$PROJECT_ROOT/reference/cyclone/src" ]]; then
  while IFS= read -r -d '' file; do
    if process_c_file "$file" "$HEADER_APACHE_C" "C"; then
      echo "  [C] $file"
      ((COUNT_C++)) || true
    fi
  done < <(find "$PROJECT_ROOT/reference/cyclone/src" -type f \( -name '*.hpp' -o -name '*.cpp' \) -print0)
else
  echo "  (reference/cyclone/src not found, skipping)"
fi

# ============================================================================
# Category D: Workbench TypeScript/Svelte
# ============================================================================

echo "=== Category D: Workbench TypeScript/Svelte ==="

if [[ -d "$PROJECT_ROOT/tools/workbench/packages" ]]; then
  while IFS= read -r -d '' file; do
    if [[ "$file" == *"/node_modules/"* ]]; then
      continue
    fi
    if process_c_file "$file" "$HEADER_APACHE_C" "D"; then
      echo "  [D] $file"
      ((COUNT_D++)) || true
    fi
  done < <(find "$PROJECT_ROOT/tools/workbench/packages" -path "*/src/*" -type f \( -name '*.ts' -o -name '*.svelte' \) -print0)
fi

# Workbench TypeScript/Svelte/JS files outside src/ (tests, configs, e2e)
if [[ -d "$PROJECT_ROOT/tools/workbench/packages" ]]; then
  while IFS= read -r -d '' file; do
    if [[ "$file" == *"/node_modules/"* ]]; then
      continue
    fi
    if process_c_file "$file" "$HEADER_APACHE_C" "D"; then
      echo "  [D] $file"
      ((COUNT_D++)) || true
    fi
  done < <(find "$PROJECT_ROOT/tools/workbench/packages" -type f \( -name '*.ts' -o -name '*.svelte' -o -name '*.js' \) -not -path "*/src/*" -not -path "*/node_modules/*" -not -name '*.config.*' -print0)
  # Config files (vite.config.ts, svelte.config.js, playwright.config.ts)
  while IFS= read -r -d '' file; do
    if [[ "$file" == *"/node_modules/"* ]]; then
      continue
    fi
    if process_c_file "$file" "$HEADER_APACHE_C" "D"; then
      echo "  [D] $file"
      ((COUNT_D++)) || true
    fi
  done < <(find "$PROJECT_ROOT/tools/workbench/packages" -maxdepth 3 -type f \( -name '*.config.ts' -o -name '*.config.js' \) -not -path "*/node_modules/*" -print0)
fi

# Website TypeScript/Svelte/JS files (all of website/, not just src/)
if [[ -d "$PROJECT_ROOT/website" ]]; then
  while IFS= read -r -d '' file; do
    if [[ "$file" == *"/node_modules/"* ]] || [[ "$file" == *"/.svelte-kit/"* ]]; then
      continue
    fi
    if process_c_file "$file" "$HEADER_APACHE_C" "D"; then
      echo "  [D] $file"
      ((COUNT_D++)) || true
    fi
  done < <(find "$PROJECT_ROOT/website" -type f \( -name '*.ts' -o -name '*.svelte' -o -name '*.js' \) -not -path "*/node_modules/*" -not -path "*/.svelte-kit/*" -print0)
fi

# Standalone TS/JS files in tools/ (not already covered)
while IFS= read -r -d '' file; do
  if [[ "$file" == *"/node_modules/"* ]] || [[ "$file" == *"/.venv/"* ]]; then
    continue
  fi
  # Skip files already handled by the workbench section
  if [[ "$file" == *"/tools/workbench/"* ]]; then
    continue
  fi
  if process_c_file "$file" "$HEADER_APACHE_C" "D"; then
    echo "  [D] $file"
    ((COUNT_D++)) || true
  fi
done < <(find "$PROJECT_ROOT/tools" -type f \( -name '*.ts' -o -name '*.js' \) -not -path "*/node_modules/*" -print0)

# ============================================================================
# Category E: Build files, scripts, config
# ============================================================================

echo "=== Category E: Build files and scripts ==="

# BUILD files across the whole project (src/, lib/, test/, bazel/, root)
while IFS= read -r -d '' file; do
  # Skip third_party and reference/mod_pagespeed BUILD files
  if [[ "$file" == *"/third_party/"* ]] || [[ "$file" == *"/reference/mod_pagespeed/"* ]]; then
    ((COUNT_SKIP++)) || true
    continue
  fi
  if [[ "$file" == *"/node_modules/"* ]] || [[ "$file" == *"/.venv/"* ]] || [[ "$file" == *"/bazel-"* ]]; then
    ((COUNT_SKIP++)) || true
    continue
  fi
  if process_hash_file "$file"; then
    echo "  [E] $file"
    ((COUNT_E++)) || true
  fi
done < <(find "$PROJECT_ROOT" -maxdepth 1 -name 'BUILD' -print0; find "$PROJECT_ROOT/src" "$PROJECT_ROOT/lib" "$PROJECT_ROOT/test" "$PROJECT_ROOT/bazel" "$PROJECT_ROOT/tools" -type f -name 'BUILD' -not -path "*/node_modules/*" -print0 2>/dev/null)

# .bzl files in bazel/
while IFS= read -r -d '' file; do
  if process_hash_file "$file"; then
    echo "  [E] $file"
    ((COUNT_E++)) || true
  fi
done < <(find "$PROJECT_ROOT/bazel" -type f -name '*.bzl' -print0)

# Shell scripts across the project (tools/, deploy/, docker/, samples/, t/)
while IFS= read -r -d '' file; do
  if [[ "$file" == *"/node_modules/"* ]] || [[ "$file" == *"/.venv/"* ]] || [[ "$file" == *"/bazel-"* ]]; then
    ((COUNT_SKIP++)) || true
    continue
  fi
  if [[ "$file" == *"/reference/mod_pagespeed/"* ]]; then
    ((COUNT_SKIP++)) || true
    continue
  fi
  if is_self "$file"; then
    continue
  fi
  if process_hash_file "$file"; then
    echo "  [E] $file"
    ((COUNT_E++)) || true
  fi
done < <(find "$PROJECT_ROOT/tools" "$PROJECT_ROOT/deploy" "$PROJECT_ROOT/docker" "$PROJECT_ROOT/samples" "$PROJECT_ROOT/t" -type f -name '*.sh' -print0 2>/dev/null)

# Python files in tools/ (not in node_modules/.venv)
while IFS= read -r -d '' file; do
  if [[ "$file" == *"/node_modules/"* ]] || [[ "$file" == *"/.venv/"* ]]; then
    ((COUNT_SKIP++)) || true
    continue
  fi
  if process_hash_file "$file"; then
    echo "  [E] $file"
    ((COUNT_E++)) || true
  fi
done < <(find "$PROJECT_ROOT/tools" -type f -name '*.py' -print0)

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=== Summary ==="
echo "  Category A (new Apache-2.0):        $COUNT_A files modified"
echo "  Category B (ported):         $COUNT_B files modified"
echo "  Category C (cyclone):        $COUNT_C files modified"
echo "  Category D (workbench):      $COUNT_D files modified"
echo "  Category E (build/scripts):  $COUNT_E files modified"
echo "  Skipped (excluded):          $COUNT_SKIP files"
TOTAL=$((COUNT_A + COUNT_B + COUNT_C + COUNT_D + COUNT_E))
echo "  Total modified:              $TOTAL files"
echo ""
echo "Done. Run again to verify idempotency (should show 0 modifications)."
