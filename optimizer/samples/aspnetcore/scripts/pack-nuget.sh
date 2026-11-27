#!/usr/bin/env bash
# pack-nuget.sh — Build native binaries, pack NuGet packages, validate, and smoke test.
# SPDX-License-Identifier: Apache-2.0
# Usage: pack-nuget.sh [options]
#   --skip-build         Skip native binary builds
#   --skip-validate      Skip structural validation
#   --skip-smoke-test    Skip runtime smoke test
#   --rids=RID1,RID2     Comma-separated RIDs to build (default: auto-detect)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ASPNETCORE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$ASPNETCORE_DIR/../.." && pwd)"
NUPKG_DIR="$ASPNETCORE_DIR/nupkg"

# ---------- Defaults ----------
SKIP_BUILD=false
SKIP_VALIDATE=false
SKIP_SMOKE_TEST=false
RIDS=""
# SSH host used for remote native builds. No default: set it in your
# environment (or a shell profile) — no machine name is stored in this tree.
REMOTE_BUILD_HOST="${REMOTE_BUILD_HOST:-}"
REMOTE_BUILD_REPO="${REMOTE_BUILD_REPO:-~/code/pagespeed-optimizer}"

# ---------- Parse arguments ----------
for arg in "$@"; do
    case "$arg" in
        --skip-build)       SKIP_BUILD=true ;;
        --skip-validate)    SKIP_VALIDATE=true ;;
        --skip-smoke-test)  SKIP_SMOKE_TEST=true ;;
        --rids=*)           RIDS="${arg#--rids=}" ;;
        -h|--help)
            echo "Usage: pack-nuget.sh [--skip-build] [--skip-validate] [--skip-smoke-test] [--rids=RID1,RID2]"
            echo ""
            echo "Options:"
            echo "  --skip-build       Skip native binary builds (use existing binaries)"
            echo "  --skip-validate    Skip structural .nupkg validation"
            echo "  --skip-smoke-test  Skip runtime smoke test"
            echo "  --rids=RID1,RID2   Build only for specified RIDs (default: auto-detect)"
            echo ""
            echo "Supported RIDs: linux-x64, osx-arm64, win-x64"
            echo ""
            echo "Environment:"
            echo "  REMOTE_BUILD_HOST  SSH host for remote linux-x64/win-x64 builds (no default)"
            echo "  REMOTE_BUILD_REPO  Repo path on that host (default: ~/code/pagespeed-optimizer)"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            exit 1
            ;;
    esac
done

# ---------- Detect buildable RIDs ----------
detect_rids() {
    local host_os host_arch
    host_os="$(uname -s)"
    host_arch="$(uname -m)"

    local rids=""

    case "$host_os" in
        Darwin)
            case "$host_arch" in
                arm64) rids="osx-arm64" ;;
                x86_64) rids="osx-x64" ;;
            esac
            # linux-x64 via Docker (build-native.sh handles this)
            if command -v docker &>/dev/null && docker image inspect pagespeed2-dev &>/dev/null 2>&1; then
                rids="$rids,linux-x64"
                echo "  (Docker + pagespeed2-dev available — linux-x64 included)" >&2
            elif [ -n "$REMOTE_BUILD_HOST" ] && ssh -o ConnectTimeout=3 -o BatchMode=yes "$REMOTE_BUILD_HOST" true 2>/dev/null; then
                rids="$rids,linux-x64"
                echo "  (remote build host reachable — linux-x64 via remote build)" >&2
            else
                echo "  (no Docker and no REMOTE_BUILD_HOST — linux-x64 skipped)" >&2
            fi
            ;;
        Linux)
            case "$host_arch" in
                x86_64) rids="linux-x64" ;;
                aarch64) rids="linux-arm64" ;;
            esac
            ;;
        MINGW*|MSYS*|CYGWIN*)
            rids="win-x64"
            ;;
    esac

    echo "$rids"
}

if [[ -z "$RIDS" ]]; then
    echo "--- Detecting buildable RIDs ---"
    RIDS="$(detect_rids)"
    echo "  RIDs: $RIDS"
fi

echo ""
echo "=== WeAmp.PageSpeed NuGet Pack Pipeline ==="
echo "  Repo root:     $REPO_ROOT"
echo "  ASP.NET Core:  $ASPNETCORE_DIR"
echo "  Output:        $NUPKG_DIR"
echo "  RIDs:          $RIDS"
echo "  Skip build:    $SKIP_BUILD"
echo "  Skip validate: $SKIP_VALIDATE"
echo "  Skip smoke:    $SKIP_SMOKE_TEST"
echo ""

# ---------- Step 1: Build native binaries ----------
if [[ "$SKIP_BUILD" == "false" ]]; then
    echo "=== Step 1: Building native binaries ==="

    IFS=',' read -ra RID_ARRAY <<< "$RIDS"
    for rid in "${RID_ARRAY[@]}"; do
        rid="$(echo "$rid" | tr -d ' ')"
        echo ""
        echo "--- Building for $rid ---"

        case "$rid" in
            osx-arm64|linux-x64)
                # build-native.sh handles both:
                #   osx-arm64: native Bazel build on macOS
                #   linux-x64: Docker-based build (or remote if Docker unavailable)
                if [[ "$rid" == "linux-x64" ]] && [[ "$(uname -s)" != "Linux" ]] && \
                   ! (command -v docker &>/dev/null && docker image inspect pagespeed2-dev &>/dev/null 2>&1); then
                    # No Docker — fall back to remote SSH build
                    if [ -z "$REMOTE_BUILD_HOST" ]; then
                        echo "ERROR: REMOTE_BUILD_HOST not set and no Docker for $rid" >&2
                        exit 1
                    fi
                    echo "  No Docker — building remotely on $REMOTE_BUILD_HOST..."

                    ssh "$REMOTE_BUILD_HOST" bash -c "'
                        cd $REMOTE_BUILD_REPO &&
                        git pull --ff-only 2>/dev/null || true &&
                        cd samples/aspnetcore &&
                        ./scripts/build-native.sh linux-x64
                    '"

                    # SCP binaries back
                    local_native_dir="$ASPNETCORE_DIR/src/WeAmp.PageSpeed.NativeAssets.Linux/runtimes/linux-x64/native"
                    mkdir -p "$local_native_dir"
                    REMOTE_NATIVE="$REMOTE_BUILD_REPO/samples/aspnetcore/src/WeAmp.PageSpeed.NativeAssets.Linux/runtimes/linux-x64/native"
                    scp "$REMOTE_BUILD_HOST:$REMOTE_NATIVE/libpagespeed.so" "$local_native_dir/"
                    scp "$REMOTE_BUILD_HOST:$REMOTE_NATIVE/factory_worker" "$local_native_dir/"
                    # libc++/libc++abi/libunwind statically linked since v2.0.2.
                    chmod +x "$local_native_dir/factory_worker"
                    echo "  Copied from $REMOTE_BUILD_HOST to $local_native_dir"
                else
                    "$SCRIPT_DIR/build-native.sh" "$rid"
                fi
                ;;

            win-x64)
                if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
                    "$SCRIPT_DIR/build-native.sh" "$rid"
                else
                    if [ -z "$REMOTE_BUILD_HOST" ]; then
                        echo "ERROR: REMOTE_BUILD_HOST not set and no Docker for $rid" >&2
                        exit 1
                    fi
                    echo "  Remote Windows builds via SSH to $REMOTE_BUILD_HOST..."

                    ssh "$REMOTE_BUILD_HOST" bash -c "'
                        cd $REMOTE_BUILD_REPO &&
                        git pull --ff-only 2>/dev/null || true &&
                        cd samples/aspnetcore &&
                        ./scripts/build-native.sh win-x64
                    '"

                    local_native_dir="$ASPNETCORE_DIR/src/WeAmp.PageSpeed.NativeAssets.Windows/runtimes/win-x64/native"
                    mkdir -p "$local_native_dir"
                    REMOTE_NATIVE="$REMOTE_BUILD_REPO/samples/aspnetcore/src/WeAmp.PageSpeed.NativeAssets.Windows/runtimes/win-x64/native"
                    scp "$REMOTE_BUILD_HOST:$REMOTE_NATIVE/pagespeed.dll" "$local_native_dir/"
                    scp "$REMOTE_BUILD_HOST:$REMOTE_NATIVE/factory_worker.exe" "$local_native_dir/"
                    echo "  Copied from $REMOTE_BUILD_HOST to $local_native_dir"
                fi
                ;;

            *)
                echo "  WARNING: Unknown RID '$rid' — skipping build"
                ;;
        esac
    done
    echo ""
else
    echo "=== Step 1: SKIPPED (--skip-build) ==="
    echo ""
fi

# ---------- Step 1.5: Console SPA ----------
# v2.0.1 ships without the workbench SPA. NativeAssets.*.csproj has
# Exclude="runtimes/<rid>/native/console/**", so even if a stub exists
# locally it will not appear in the produced .nupkg. The runtime trial
# endpoint POST /v1/license/trial is exposed independently of any SPA.
# Re-enable this build step once the SPA's publish-RID extraction layout
# is fixed (currently the runtimes/<rid>/native/console/ subdir flattens
# to the publish-output root and collides with consumer assets).
echo "--- Skipping console SPA build (excluded from packaging in v2.0.1) ---"
echo ""

# ---------- Step 2: dotnet pack ----------
echo "=== Step 2: Packing NuGet packages ==="
rm -rf "$NUPKG_DIR"
mkdir -p "$NUPKG_DIR"

dotnet pack "$ASPNETCORE_DIR/WeAmp.PageSpeed.sln" \
    -c Release \
    -o "$NUPKG_DIR" \
    --verbosity quiet

echo "  Packages created:"
for f in "$NUPKG_DIR"/*.nupkg; do
    echo "    $(basename "$f")  ($(wc -c < "$f" | tr -d ' ') bytes)"
done
echo ""

# ---------- Step 3: Structural validation ----------
if [[ "$SKIP_VALIDATE" == "false" ]]; then
    echo "=== Step 3: Structural validation ==="
    "$SCRIPT_DIR/validate-nupkg.sh" "$NUPKG_DIR"
    echo ""
else
    echo "=== Step 3: SKIPPED (--skip-validate) ==="
    echo ""
fi

# ---------- Step 4: Smoke test ----------
if [[ "$SKIP_SMOKE_TEST" == "false" ]]; then
    echo "=== Step 4: Runtime smoke test ==="
    "$SCRIPT_DIR/smoke-test.sh" "$NUPKG_DIR"
    echo ""
else
    echo "=== Step 4: SKIPPED (--skip-smoke-test) ==="
    echo ""
fi

# ---------- Summary ----------
echo "==========================================="
echo "  NuGet packages ready in: $NUPKG_DIR"
echo ""
for f in "$NUPKG_DIR"/*.nupkg; do
    echo "    $(basename "$f")"
done
echo ""
echo "  To publish to nuget.org:"
echo "    dotnet nuget push '$NUPKG_DIR/*.nupkg' --api-key <key> --source https://api.nuget.org/v3/index.json"
echo "==========================================="
