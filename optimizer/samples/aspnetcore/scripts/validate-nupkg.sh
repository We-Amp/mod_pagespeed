#!/usr/bin/env bash
# validate-nupkg.sh — Structural validation of .nupkg files.
# SPDX-License-Identifier: Apache-2.0
# Usage: validate-nupkg.sh <directory-containing-nupkg-files>
set -euo pipefail

NUPKG_DIR="${1:?Usage: validate-nupkg.sh <nupkg-directory>}"
NUPKG_DIR="$(cd "$NUPKG_DIR" && pwd)"

PASS=0
FAIL=0
WARN=0

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }
warn() { echo "  [WARN] $1"; WARN=$((WARN + 1)); }

# Temporary extraction directory
EXTRACT_DIR="$(mktemp -d)"
trap 'rm -rf "$EXTRACT_DIR"' EXIT

echo "=== NuGet Package Structural Validation ==="
echo "Directory: $NUPKG_DIR"
echo ""

# ---------- Helper: validate a single package ----------
validate_package() {
    local pkg_pattern="$1"
    local pkg_label="$2"
    shift 2
    # Remaining args are pairs: "path_in_zip" "description"

    local nupkg
    nupkg="$(find "$NUPKG_DIR" -maxdepth 1 -name "$pkg_pattern" -print -quit 2>/dev/null)"

    if [[ -z "$nupkg" ]]; then
        warn "$pkg_label — package not found (skipping)"
        echo ""
        return
    fi

    echo "--- $pkg_label ---"
    echo "  File: $(basename "$nupkg")"

    # Check valid zip
    if python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).testzip() is None or exit(1)" "$nupkg" 2>/dev/null; then
        pass "Valid ZIP archive"
    else
        fail "Invalid ZIP archive"
        echo ""
        return
    fi

    # List contents
    echo "  Contents:"
    python3 -c "
import zipfile, sys
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    print(f'    {i.file_size:>10}  {i.filename}')
" "$nupkg"

    # Extract for binary inspection
    local extract_sub="$EXTRACT_DIR/$(basename "$nupkg" .nupkg)"
    mkdir -p "$extract_sub"
    python3 -c "
import zipfile, sys
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
" "$nupkg" "$extract_sub"

    # Check expected files
    while [[ $# -ge 2 ]]; do
        local expected_path="$1"
        local description="$2"
        shift 2

        local full_path="$extract_sub/$expected_path"
        if [[ -f "$full_path" ]]; then
            local size
            size=$(wc -c < "$full_path" | tr -d ' ')
            local file_type
            file_type=$(file -b "$full_path" 2>/dev/null || echo "unknown")
            pass "$description: $expected_path ($size bytes)"
            echo "         Format: $file_type"
        else
            fail "$description: $expected_path — NOT FOUND"
        fi
    done

    echo ""
}

# NB: console/index.html is intentionally NOT validated — the workbench SPA
# isn't packaged in v2.0.1 (see NativeAssets.*.csproj `Exclude="...console/**"`).

# ---------- NativeAssets.Linux ----------
validate_package \
    "WeAmp.PageSpeed.NativeAssets.Linux.*.nupkg" \
    "WeAmp.PageSpeed.NativeAssets.Linux" \
    "runtimes/linux-x64/native/libpagespeed.so" "Native library (Linux)" \
    "runtimes/linux-x64/native/factory_worker" "Worker binary (Linux)"

# ---------- NativeAssets.macOS ----------
validate_package \
    "WeAmp.PageSpeed.NativeAssets.macOS.*.nupkg" \
    "WeAmp.PageSpeed.NativeAssets.macOS" \
    "runtimes/osx-arm64/native/libpagespeed.dylib" "Native library (macOS)" \
    "runtimes/osx-arm64/native/factory_worker" "Worker binary (macOS)"

# ---------- NativeAssets.Windows ----------
validate_package \
    "WeAmp.PageSpeed.NativeAssets.Windows.*.nupkg" \
    "WeAmp.PageSpeed.NativeAssets.Windows" \
    "runtimes/win-x64/native/pagespeed.dll" "Native library (Windows)" \
    "runtimes/win-x64/native/factory_worker.exe" "Worker binary (Windows)"

# ---------- WeAmp.PageSpeed (managed) ----------
validate_package \
    "WeAmp.PageSpeed.2.*.nupkg" \
    "WeAmp.PageSpeed (managed)" \
    "lib/net10.0/WeAmp.PageSpeed.dll" "Managed assembly"

# ---------- WeAmp.PageSpeed.AspNetCore (managed) ----------
validate_package \
    "WeAmp.PageSpeed.AspNetCore.*.nupkg" \
    "WeAmp.PageSpeed.AspNetCore (managed)" \
    "lib/net10.0/WeAmp.PageSpeed.AspNetCore.dll" "Managed assembly"

# ---------- Summary ----------
echo "==========================================="
echo "  PASS: $PASS  |  FAIL: $FAIL  |  WARN: $WARN"
echo "==========================================="

if [[ $FAIL -gt 0 ]]; then
    echo "RESULT: FAILED"
    exit 1
else
    echo "RESULT: OK"
    exit 0
fi
