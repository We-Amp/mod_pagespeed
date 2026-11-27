#!/usr/bin/env bash
# build-native.sh — Builds native binaries for a specific RID and copies them
# SPDX-License-Identifier: Apache-2.0
# into the appropriate NativeAssets project.
#
# Usage: ./build-native.sh <rid>
#   rid: linux-x64 | osx-arm64 | win-x64
#
# For linux-x64: runs Bazel inside the pagespeed2-dev Docker container.
# For osx-arm64: runs Bazel directly on the host (requires macOS + Apple Silicon).
# For win-x64:   not yet implemented.
#
# If a vendor/ directory exists in the repo root, --vendor_dir=vendor is passed
# to Bazel for reproducible offline builds (from a vendored source tarball).
set -euo pipefail

win_userprofile() {
    # cmd.exe refuses a UNC working directory and prints a "UNC paths are not
    # supported" banner on STDOUT before the value, so run it from a drive-backed
    # cwd and keep only the last line. Strip CR, not LF: `tr -d "\r\n"` would
    # glue the banner onto the value.
    (cd /mnt/c && /mnt/c/Windows/System32/cmd.exe /c "echo %USERPROFILE%" 2>/dev/null | tr -d "\r" | tail -n1)
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SAMPLES_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- Argument parsing ---

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <rid>"
  echo "  rid: linux-x64 | osx-arm64 | win-x64"
  exit 1
fi

RID="$1"

# --- RID-to-project mapping ---

case "$RID" in
  linux-x64)
    NATIVE_ASSETS_PROJECT="WeAmp.PageSpeed.NativeAssets.Linux"
    ;;
  osx-arm64)
    NATIVE_ASSETS_PROJECT="WeAmp.PageSpeed.NativeAssets.macOS"
    ;;
  win-x64)
    NATIVE_ASSETS_PROJECT="WeAmp.PageSpeed.NativeAssets.Windows"
    ;;
  *)
    echo "ERROR: Unknown RID '$RID'. Must be linux-x64, osx-arm64, or win-x64."
    exit 1
    ;;
esac

OUTPUT_DIR="$SAMPLES_ROOT/src/$NATIVE_ASSETS_PROJECT/runtimes/$RID/native"

# --- Bazel targets ---

TARGETS=(
  "//lib/pagespeed:libpagespeed.so"
  "//src/worker:factory_worker"
)

# --- Worker attribution copts ---
# Stamp the bundled factory_worker so its license-server heartbeat / activation
# calls attribute correctly in compliance reports. Without these, the NuGet
# worker reports server=nginx (version.h default) and distribution=source.
#
# Quoting: bash strips one layer of quotes, bazel strips another; the inner
# double-quotes need to survive into the C preprocessor as part of the macro
# expansion so the value is a C string literal, not a bare identifier.
WORKER_COPTS=(
  "--copt=-DPAGESPEED_SERVER=\"aspnetcore\""
  "--copt=-DPAGESPEED_DISTRIBUTION=\"nuget\""
)

# --- Vendor mode ---

VENDOR_ARGS=()
if [[ -d "$REPO_ROOT/vendor" ]]; then
  echo "=== Vendor directory detected — building offline ==="
  VENDOR_ARGS=("--vendor_dir=vendor")
fi

# --- Build functions ---

build_linux_x64() {
  echo "=== Building for linux-x64 (Docker) ==="

  # Verify Docker is available
  if ! command -v docker &>/dev/null; then
    echo "ERROR: Docker is required for linux-x64 builds but not found."
    exit 1
  fi

  # Check that the dev image exists
  if ! docker image inspect pagespeed2-dev &>/dev/null; then
    echo "ERROR: Docker image 'pagespeed2-dev' not found."
    echo "Build it first:  docker build -f docker/Dockerfile -t pagespeed2-dev ."
    exit 1
  fi

  echo "--- Running Bazel build inside pagespeed2-dev container ---"
  mkdir -p "$OUTPUT_DIR"

  # Build in a named container, then docker cp the outputs out.
  # We can't rely on bazel-bin symlinks on the host since they point to
  # container-internal paths. Instead we use `docker cp` from the stopped
  # container before removing it.
  local CONTAINER_NAME="pagespeed-build-$$"
  local ARCH_LIB="/usr/lib/x86_64-linux-gnu"

  docker run --name "$CONTAINER_NAME" \
    -v "$REPO_ROOT:/workspace" \
    -w /workspace \
    pagespeed2-dev \
    bazel build \
      --config=docker \
      --config=opt \
      "${WORKER_COPTS[@]}" \
      ${VENDOR_ARGS[*]:+${VENDOR_ARGS[*]}} \
      ${TARGETS[*]}

  echo "--- Extracting binaries from container ---"
  mkdir -p "$OUTPUT_DIR"
  docker cp "$CONTAINER_NAME:/workspace/bazel-bin/lib/pagespeed/libpagespeed.so" "$OUTPUT_DIR/libpagespeed.so"
  docker cp "$CONTAINER_NAME:/workspace/bazel-bin/src/worker/factory_worker" "$OUTPUT_DIR/factory_worker"

  # libc++/libc++abi/libunwind are statically linked into both binaries
  # since v2.0.2 — see lib/pagespeed/BUILD linkopts. No separate .so files
  # to stage.

  docker rm -f "$CONTAINER_NAME" > /dev/null

  chmod +r "$OUTPUT_DIR"/*.so* 2>/dev/null || true
}

build_osx_arm64() {
  echo "=== Building for osx-arm64 (native) ==="

  # Verify we're on macOS ARM64
  if [[ "$(uname)" != "Darwin" ]]; then
    echo "ERROR: osx-arm64 builds require macOS. Detected: $(uname)"
    exit 1
  fi
  if [[ "$(uname -m)" != "arm64" ]]; then
    echo "ERROR: osx-arm64 builds require Apple Silicon. Detected: $(uname -m)"
    exit 1
  fi

  # Verify Bazel is available
  if ! command -v bazel &>/dev/null; then
    echo "ERROR: Bazel is required but not found on PATH."
    exit 1
  fi

  echo "--- Running Bazel build ---"
  cd "$REPO_ROOT"
  bazel build \
    --config=opt \
    "${WORKER_COPTS[@]}" \
    ${VENDOR_ARGS[@]+"${VENDOR_ARGS[@]}"} \
    "${TARGETS[@]}"

  echo "--- Copying binaries to $OUTPUT_DIR ---"
  mkdir -p "$OUTPUT_DIR"

  # On macOS, Bazel produces .dylib for linkshared cc_binary targets even
  # though the target name says .so. The output filename follows the target
  # name but with a .dylib extension.
  cp -f "$REPO_ROOT/bazel-bin/lib/pagespeed/libpagespeed.so" "$OUTPUT_DIR/libpagespeed.dylib"
  cp -f "$REPO_ROOT/bazel-bin/src/worker/factory_worker"     "$OUTPUT_DIR/factory_worker"
}

build_win_x64() {
  echo "=== Building for win-x64 (native MSVC) ==="

  # This function can run in two contexts:
  # 1. Directly on Windows (Git Bash / MSYS2)
  # 2. Inside WSL, invoking bazelisk.exe on the Windows side
  #
  # In both cases, the repo must be on the Windows filesystem (NTFS),
  # not under a WSL ext4 mount. Bazel on Windows cannot handle UNC paths.

  local WIN_REPO_ROOT="${WIN_REPO_ROOT:-C:\\pagespeed-2}"
  local WSL_REPO_ROOT="/mnt/c/pagespeed-2"

  if [[ "$(uname -s)" == Linux ]] && grep -qi microsoft /proc/version 2>/dev/null; then
    # Running inside WSL
    echo "  Detected WSL — building via Windows bazelisk.exe"

    if [[ ! -d "$WSL_REPO_ROOT" ]]; then
      echo "ERROR: Repo not found at $WSL_REPO_ROOT"
      echo "Clone it:  cd /mnt/c && git clone <repo-url> pagespeed-2"
      exit 1
    fi

    # Find bazelisk.exe -- check known WinGet install path and PATH
    local BAZELISK
    local WINGET_PATH="${WIN_USER_HOME:-/mnt/c/Users/${WIN_USER:-$USER}}/AppData/Local/Microsoft/WinGet/Packages/Bazel.Bazelisk_Microsoft.Winget.Source_8wekyb3d8bbwe/bazelisk.exe"
    if [[ -x "$WINGET_PATH" ]]; then
      BAZELISK="$WINGET_PATH"
    else
      BAZELISK="$(command -v bazelisk.exe 2>/dev/null || true)"
    fi
    if [[ -z "$BAZELISK" ]]; then
      echo "ERROR: bazelisk.exe not found. Install via: winget install Bazel.Bazelisk"
      exit 1
    fi
    echo "  Using: $BAZELISK"

    # BAZEL_SH required for Bazel on Windows; --output_base keeps paths short
    # Discovered at runtime from the Windows environment; no user name or home
    # path is stored in this tree. Override with BAZEL_SH if Git lives elsewhere.
    WIN_USERPROFILE=$(win_userprofile)
    export BAZEL_SH="${BAZEL_SH:-${WIN_USERPROFILE}\\AppData\\Local\\Programs\\Git\\bin\\bash.exe}"

    # Build on Windows side (cd to Windows path first)
    cd "$WSL_REPO_ROOT"
    "$BAZELISK" --output_base="C:\b" build \
      --config=opt \
      "${WORKER_COPTS[@]}" \
      ${VENDOR_ARGS[@]+"${VENDOR_ARGS[@]}"} \
      "${TARGETS[@]}"

    echo "--- Copying binaries to $OUTPUT_DIR ---"
    mkdir -p "$OUTPUT_DIR"
    # On Windows, linkshared cc_binary produces a .dll (Bazel renames from .so target)
    cp -f "$WSL_REPO_ROOT/bazel-bin/lib/pagespeed/libpagespeed.so" "$OUTPUT_DIR/pagespeed.dll" 2>/dev/null \
      || cp -f "$WSL_REPO_ROOT/bazel-bin/lib/pagespeed/pagespeed.dll" "$OUTPUT_DIR/pagespeed.dll"
    cp -f "$WSL_REPO_ROOT/bazel-bin/src/worker/factory_worker.exe" "$OUTPUT_DIR/factory_worker.exe" 2>/dev/null \
      || cp -f "$WSL_REPO_ROOT/bazel-bin/src/worker/factory_worker" "$OUTPUT_DIR/factory_worker.exe"
  else
    # Running directly on Windows (Git Bash / MSYS2)
    echo "  Building natively on Windows"

    if ! command -v bazelisk &>/dev/null && ! command -v bazel &>/dev/null; then
      echo "ERROR: Bazel/Bazelisk not found on PATH."
      exit 1
    fi
    local BAZEL_CMD="${BAZEL_CMD:-bazelisk}"

    cd "$REPO_ROOT"
    "$BAZEL_CMD" build \
      --config=opt \
      "${WORKER_COPTS[@]}" \
      ${VENDOR_ARGS[@]+"${VENDOR_ARGS[@]}"} \
      "${TARGETS[@]}"

    echo "--- Copying binaries to $OUTPUT_DIR ---"
    mkdir -p "$OUTPUT_DIR"
    cp -f "$REPO_ROOT/bazel-bin/lib/pagespeed/libpagespeed.so" "$OUTPUT_DIR/pagespeed.dll" 2>/dev/null \
      || cp -f "$REPO_ROOT/bazel-bin/lib/pagespeed/pagespeed.dll" "$OUTPUT_DIR/pagespeed.dll"
    cp -f "$REPO_ROOT/bazel-bin/src/worker/factory_worker.exe" "$OUTPUT_DIR/factory_worker.exe" 2>/dev/null \
      || cp -f "$REPO_ROOT/bazel-bin/src/worker/factory_worker" "$OUTPUT_DIR/factory_worker.exe"
  fi
}

# --- Build console SPA ---
# v2.0.1 ships without the workbench SPA. NativeAssets.*.csproj has
# Exclude="runtimes/<rid>/native/console/**", so anything dropped here will
# not appear in the produced .nupkg. Re-enable once the SPA's
# publish-RID-extraction layout is fixed (currently flattens to publish-output
# root and collides with consumer assets).
build_console_spa() {
    echo "--- Skipping console SPA build (excluded from packaging in v2.0.1) ---"
}

# --- BUILD_INFO.json stamp ---
# Records which commit + when the bundled factory_worker was built.
# Released NativeAssets packages embed this at runtimes/<rid>/native/BUILD_INFO.json
# so support can correlate compliance-report heartbeats back to a specific
# build. WORKER_GIT_SHA env var overrides for reproducible builds (e.g. when
# release.sh captures the SHA once for the whole pipeline); otherwise we read
# `git rev-parse HEAD` from REPO_ROOT.
write_build_info() {
  local out="$OUTPUT_DIR/BUILD_INFO.json"
  local sha="${WORKER_GIT_SHA:-}"
  local sha_short="${WORKER_GIT_SHA_SHORT:-}"
  if [[ -z "$sha" ]]; then
    sha="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
  fi
  if [[ -z "$sha_short" ]]; then
    sha_short="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  fi
  local ts
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  mkdir -p "$OUTPUT_DIR"
  cat > "$out" <<JSON
{
  "git_sha": "$sha",
  "git_sha_short": "$sha_short",
  "build_timestamp_utc": "$ts",
  "rid": "$RID"
}
JSON
  echo "--- Wrote $out (sha=$sha_short) ---"
}

# --- Main ---

case "$RID" in
  linux-x64)  build_linux_x64 ;;
  osx-arm64)  build_osx_arm64 ;;
  win-x64)    build_win_x64   ;;
esac

# --- Build attribution stamp ---
write_build_info

# --- Console SPA (excluded from v2.0.1 packaging) ---
build_console_spa "$OUTPUT_DIR"

echo ""
echo "=== Build complete for $RID ==="
echo "Output directory: $OUTPUT_DIR"
ls -lh "$OUTPUT_DIR/"
