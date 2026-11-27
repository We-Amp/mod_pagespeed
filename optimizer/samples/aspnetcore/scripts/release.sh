#!/usr/bin/env bash
# release.sh — End-to-end NuGet release pipeline.
# SPDX-License-Identifier: Apache-2.0
#
# Runs the full pipeline: vendor → build → pack → validate → smoke test.
# Designed to run on a WSL2 build host which can build linux-x64 (Docker)
# and win-x64 (MSVC via bazelisk.exe). osx-arm64 is included when run on Mac.
#
# Usage: release.sh [options]
#   --skip-vendor     Skip bazel vendor step (use existing vendor/ dir)
#   --skip-build      Skip native binary builds (use existing binaries)
#   --skip-validate   Skip structural package validation
#   --skip-smoke      Skip runtime smoke tests
#   --rids=R1,R2      Comma-separated RIDs (default: auto-detect)
#   --dry-run         Show what would be done without executing
#   --clean           Remove vendor/, nupkg/, and native binary dirs before starting
#
# Environment:
#   VERSION                  Package version (default: from Directory.Build.props)
#   REMOTE_BUILD_HOST        SSH host for remote builds (no default)
#   WIN_DOTNET               Windows .NET path (default: C:\dotnet10\dotnet.exe)
set -euo pipefail

win_userprofile() {
    # cmd.exe refuses a UNC working directory and prints a "UNC paths are not
    # supported" banner on STDOUT before the value, so run it from a drive-backed
    # cwd and keep only the last line. Strip CR, not LF: `tr -d "\r\n"` would
    # glue the banner onto the value.
    (cd /mnt/c && /mnt/c/Windows/System32/cmd.exe /c "echo %USERPROFILE%" 2>/dev/null | tr -d "\r" | tail -n1)
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ASPNETCORE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$ASPNETCORE_DIR/../.." && pwd)"

# ---------- Defaults ----------
SKIP_VENDOR=false
SKIP_BUILD=false
SKIP_VALIDATE=false
SKIP_SMOKE=false
RIDS=""
DRY_RUN=false
CLEAN=false
WIN_DOTNET="${WIN_DOTNET:-C:\\dotnet10\\dotnet.exe}"

# Version from Directory.Build.props
VERSION="${VERSION:-$(grep '<Version>' "$ASPNETCORE_DIR/Directory.Build.props" | sed 's/.*<Version>\(.*\)<\/Version>.*/\1/')}"
NUPKG_DIR="$ASPNETCORE_DIR/nupkg"

# ---------- Parse arguments ----------
for arg in "$@"; do
    case "$arg" in
        --skip-vendor)   SKIP_VENDOR=true ;;
        --skip-build)    SKIP_BUILD=true ;;
        --skip-validate) SKIP_VALIDATE=true ;;
        --skip-smoke)    SKIP_SMOKE=true ;;
        --rids=*)        RIDS="${arg#--rids=}" ;;
        --dry-run)       DRY_RUN=true ;;
        --clean)         CLEAN=true ;;
        -h|--help)
            sed -n '2,/^set -/{ /^#/s/^# \?//p }' "$0"
            exit 0
            ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# ---------- Detect platform & RIDs ----------
HOST_OS="$(uname -s)"
IS_WSL=false
if [[ "$HOST_OS" == "Linux" ]] && grep -qi microsoft /proc/version 2>/dev/null; then
    IS_WSL=true
fi

if [[ -z "$RIDS" ]]; then
    RIDS=""
    case "$HOST_OS" in
        Darwin)
            [[ "$(uname -m)" == "arm64" ]] && RIDS="osx-arm64"
            ;;
        Linux)
            if command -v docker &>/dev/null; then
                RIDS="linux-x64"
            fi
            if [[ "$IS_WSL" == true ]]; then
                RIDS="${RIDS:+$RIDS,}win-x64"
            fi
            ;;
    esac
fi

# ---------- Timing ----------
PIPELINE_START=$(date +%s)
step_timer() { echo "  ($(( $(date +%s) - $1 ))s)"; }

# ---------- Summary ----------
echo "╔══════════════════════════════════════════════════════╗"
echo "║       WeAmp.PageSpeed NuGet Release Pipeline        ║"
echo "╠══════════════════════════════════════════════════════╣"
echo "║  Version:    $VERSION"
echo "║  RIDs:       $RIDS"
echo "║  Repo:       $REPO_ROOT"
echo "║  Output:     $NUPKG_DIR"
echo "║  Host:       $HOST_OS $(uname -m)$( [[ "$IS_WSL" == true ]] && echo ' (WSL)' )"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

if [[ "$DRY_RUN" == true ]]; then
    echo "[DRY RUN] Would execute:"
    [[ "$SKIP_VENDOR" == false ]] && echo "  1. bazel vendor → vendor/"
    [[ "$SKIP_BUILD" == false ]]  && echo "  2. build-native.sh for: $RIDS"
    echo "  3. dotnet pack → $NUPKG_DIR"
    [[ "$SKIP_VALIDATE" == false ]] && echo "  4. validate-nupkg.sh"
    [[ "$SKIP_SMOKE" == false ]]    && echo "  5. smoke-test.sh for: $RIDS"
    exit 0
fi

# ---------- Clean ----------
if [[ "$CLEAN" == true ]]; then
    echo "--- Cleaning previous artifacts ---"
    rm -rf "$REPO_ROOT/vendor" "$NUPKG_DIR"
    for d in "$ASPNETCORE_DIR"/src/WeAmp.PageSpeed.NativeAssets.*/runtimes/*/native/; do
        find "$d" -type f ! -name '.gitkeep' -delete 2>/dev/null || true
    done
    echo "  Done."
    echo ""
fi

# Ensure dotnet and bazel are on PATH
export PATH="$HOME/.dotnet:$HOME/bin:$PATH"

FAILURES=0

# ============================================================
# Stage 1: Vendor dependencies
# ============================================================
if [[ "$SKIP_VENDOR" == false ]]; then
    echo "━━━ Stage 1: Vendor dependencies ━━━"
    STAGE_START=$(date +%s)

    cd "$REPO_ROOT"
    bazel vendor \
        --vendor_dir=vendor \
        //lib/pagespeed:libpagespeed.so \
        //src/worker:factory_worker

    echo "  vendor/ size: $(du -sh vendor/ | cut -f1)"
    step_timer "$STAGE_START"
    echo ""
else
    echo "━━━ Stage 1: SKIPPED (--skip-vendor) ━━━"
    if [[ ! -d "$REPO_ROOT/vendor" ]]; then
        echo "  WARNING: vendor/ directory not found — builds may download deps"
    fi
    echo ""
fi

# ============================================================
# Stage 2: Build native binaries
# ============================================================
if [[ "$SKIP_BUILD" == false ]]; then
    echo "━━━ Stage 2: Build native binaries ━━━"

    # Capture the worker SHA once for the whole pipeline so every RID's
    # BUILD_INFO.json reports the same commit (otherwise a long pipeline
    # could drift if HEAD moves between RIDs). build-native.sh reads
    # WORKER_GIT_SHA / WORKER_GIT_SHA_SHORT from the environment.
    WORKER_GIT_SHA="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    WORKER_GIT_SHA_SHORT="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    export WORKER_GIT_SHA WORKER_GIT_SHA_SHORT
    echo "  Stamping worker builds with SHA: $WORKER_GIT_SHA_SHORT ($WORKER_GIT_SHA)"

    IFS=',' read -ra RID_ARRAY <<< "$RIDS"
    for rid in "${RID_ARRAY[@]}"; do
        rid="$(echo "$rid" | tr -d ' ')"
        STAGE_START=$(date +%s)
        echo ""
        echo "--- $rid ---"

        "$SCRIPT_DIR/build-native.sh" "$rid"

        step_timer "$STAGE_START"
    done
    echo ""
else
    echo "━━━ Stage 2: SKIPPED (--skip-build) ━━━"
    echo ""
fi

# ============================================================
# Stage 3: Pack NuGet packages
# ============================================================
echo "━━━ Stage 3: Pack NuGet packages ━━━"
STAGE_START=$(date +%s)

rm -rf "$NUPKG_DIR"
mkdir -p "$NUPKG_DIR"

# Pack each NativeAssets project that has binaries
IFS=',' read -ra RID_ARRAY <<< "$RIDS"
for rid in "${RID_ARRAY[@]}"; do
    rid="$(echo "$rid" | tr -d ' ')"
    case "$rid" in
        linux-x64)  proj="WeAmp.PageSpeed.NativeAssets.Linux" ;;
        osx-arm64)  proj="WeAmp.PageSpeed.NativeAssets.macOS" ;;
        win-x64)    proj="WeAmp.PageSpeed.NativeAssets.Windows" ;;
        *)          echo "  WARN: Unknown RID $rid"; continue ;;
    esac
    native_dir="$ASPNETCORE_DIR/src/$proj/runtimes/$rid/native"
    file_count=$(find "$native_dir" -type f ! -name '.gitkeep' 2>/dev/null | wc -l)
    if [[ "$file_count" -gt 0 ]]; then
        echo "  Packing $proj ($file_count files)..."
        dotnet pack "$ASPNETCORE_DIR/src/$proj/$proj.csproj" \
            -c Release -o "$NUPKG_DIR" --verbosity quiet 2>&1 | grep -v "warning CS" || true
    else
        echo "  SKIP $proj — no native binaries in $native_dir"
    fi
done

# Pack managed packages
echo "  Packing WeAmp.PageSpeed..."
dotnet pack "$ASPNETCORE_DIR/src/WeAmp.PageSpeed/WeAmp.PageSpeed.csproj" \
    -c Release -o "$NUPKG_DIR" --verbosity quiet 2>&1 | grep -v "warning CS" || true

echo "  Packing WeAmp.PageSpeed.AspNetCore..."
dotnet pack "$ASPNETCORE_DIR/src/WeAmp.PageSpeed.AspNetCore/WeAmp.PageSpeed.AspNetCore.csproj" \
    -c Release -o "$NUPKG_DIR" --verbosity quiet 2>&1 | grep -v "warning CS" || true

echo ""
echo "  Packages:"
for f in "$NUPKG_DIR"/*.nupkg; do
    size=$(wc -c < "$f" | tr -d ' ')
    if [[ "$size" -gt 1048576 ]]; then
        echo "    $(basename "$f")  ($(( size / 1048576 )) MB)"
    else
        echo "    $(basename "$f")  ($(( size / 1024 )) KB)"
    fi
done
step_timer "$STAGE_START"
echo ""

# ============================================================
# Stage 4: Validate package structure
# ============================================================
if [[ "$SKIP_VALIDATE" == false ]]; then
    echo "━━━ Stage 4: Validate package structure ━━━"
    STAGE_START=$(date +%s)

    if "$SCRIPT_DIR/validate-nupkg.sh" "$NUPKG_DIR"; then
        echo "  Validation: PASSED"
    else
        echo "  Validation: FAILED"
        FAILURES=$((FAILURES + 1))
    fi
    step_timer "$STAGE_START"
    echo ""
else
    echo "━━━ Stage 4: SKIPPED (--skip-validate) ━━━"
    echo ""
fi

# ============================================================
# Stage 5: Smoke tests
# ============================================================
if [[ "$SKIP_SMOKE" == false ]]; then
    echo "━━━ Stage 5: Runtime smoke tests ━━━"

    IFS=',' read -ra RID_ARRAY <<< "$RIDS"
    for rid in "${RID_ARRAY[@]}"; do
        rid="$(echo "$rid" | tr -d ' ')"
        STAGE_START=$(date +%s)
        echo ""
        echo "--- Smoke test: $rid ---"

        case "$rid" in
            linux-x64|osx-arm64)
                # Run locally — dotnet is on this host
                if "$SCRIPT_DIR/smoke-test.sh" "$NUPKG_DIR"; then
                    echo "  $rid smoke test: PASSED"
                else
                    echo "  $rid smoke test: FAILED"
                    FAILURES=$((FAILURES + 1))
                fi
                ;;

            win-x64)
                # Run via Windows dotnet from WSL.
                # smoke-test.sh can't run directly because it's a bash script
                # that calls `dotnet` — and Windows dotnet needs Windows paths.
                # Instead, we run a minimal inline test.
                echo "  Running win-x64 smoke test via Windows .NET..."

                WIN_NUPKG_DIR=$(wslpath -w "$NUPKG_DIR")
                # Windows user profile is discovered at runtime; no user name
                # or home path is stored in this tree.
                WIN_USERPROFILE=$(win_userprofile)
                if [ -z "$WIN_USERPROFILE" ]; then
                    echo "  ERROR: could not read %USERPROFILE% from Windows" >&2
                    exit 1
                fi
                WIN_TEMP="${WIN_USERPROFILE}\\AppData\\Local\\Temp\\ps-smoke-$$"
                WIN_TEMP_DIR="$(wslpath -u "${WIN_USERPROFILE}")/AppData/Local/Temp"
                WIN_DOTNET_PATH="${WIN_DOTNET//\\/\\\\}"

                # Create test project on Windows filesystem
                /mnt/c/Windows/System32/cmd.exe /c "mkdir $WIN_TEMP" 2>/dev/null || true

                # Write nuget.config
                cat > "${WIN_TEMP_DIR}/ps-smoke-$$/nuget.config" <<NCEOF
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="Local" value="$WIN_NUPKG_DIR" />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
  </packageSources>
</configuration>
NCEOF

                # Write csproj
                cat > "${WIN_TEMP_DIR}/ps-smoke-$$/SmokeTest.csproj" <<CEOF
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="WeAmp.PageSpeed.AspNetCore" Version="$VERSION" />
    <PackageReference Include="WeAmp.PageSpeed.NativeAssets.Windows" Version="$VERSION" />
  </ItemGroup>
</Project>
CEOF

                # Write Program.cs (Tier 1 P/Invoke tests)
                cat > "${WIN_TEMP_DIR}/ps-smoke-$$/Program.cs" <<'CSEOF'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

int failures = 0;
int passed = 0;
var outputDir = AppContext.BaseDirectory;

Console.WriteLine("=== win-x64 Tier 1 Smoke Test ===");
Console.WriteLine($"RID: {RuntimeInformation.RuntimeIdentifier}");
Console.WriteLine($"Dir: {outputDir}");
Console.WriteLine();

void Pass(string msg) { Console.WriteLine($"[PASS] {msg}"); passed++; }
void Fail(string msg) { Console.WriteLine($"[FAIL] {msg}"); failures++; }
void Assert(bool ok, string msg) { if (ok) Pass(msg); else Fail(msg); }

string? FindFile(string name) {
    var d = Path.Combine(outputDir, name);
    if (File.Exists(d)) return d;
    var r = Path.Combine(outputDir, "runtimes", "win-x64", "native", name);
    if (File.Exists(r)) return r;
    return null;
}

// Helper: get a delegate for a native export
T GetFn<T>(IntPtr lib, string name) where T : Delegate {
    if (!NativeLibrary.TryGetExport(lib, name, out var ptr) || ptr == IntPtr.Zero)
        throw new Exception($"Export not found: {name}");
    return Marshal.GetDelegateForFunctionPointer<T>(ptr);
}

// Helper: marshal a native const char* to string
string PtrToStr(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "";

// Helper: allocate UTF-8 string for native (must free with Marshal.FreeCoTaskMem)
IntPtr StrToPtr(string s) => Marshal.StringToCoTaskMemUTF8(s);

// ---- File existence checks ----
var lib = FindFile("pagespeed.dll");
Assert(lib != null, $"pagespeed.dll exists");
var w = FindFile("factory_worker.exe");
Assert(w != null, $"factory_worker.exe exists");

if (lib == null) {
    Console.WriteLine("Cannot continue without pagespeed.dll");
    return failures;
}

if (!NativeLibrary.TryLoad(lib, out var h) || h == IntPtr.Zero) {
    Fail("NativeLibrary.TryLoad");
    return failures;
}
Pass("NativeLibrary.TryLoad");

try {
    // ======== T1.1: Version & Error Strings ========
    Console.WriteLine("\n--- T1.1: Version & Error Strings ---");
    var vMajor = GetFn<FnInt>(h, "ps_version_major")();
    var vMinor = GetFn<FnInt>(h, "ps_version_minor")();
    var vPatch = GetFn<FnInt>(h, "ps_version_patch")();
    Assert(vMajor > 0, $"Version {vMajor}.{vMinor}.{vPatch}");

    var errorName = GetFn<FnIntRetPtr>(h, "ps_error_name");
    Assert(PtrToStr(errorName(0)) == "PS_OK", "ps_error_name(0) == PS_OK");
    Assert(PtrToStr(errorName(1)) == "PS_ERR_NOT_FOUND", "ps_error_name(1) == PS_ERR_NOT_FOUND");

    // ======== T1.2: Request Classification ========
    Console.WriteLine("\n--- T1.2: Request Classification ---");
    var classify = GetFn<FnClassify>(h, "ps_classify");
    var classifyCt = GetFn<FnClassifyCt>(h, "ps_classify_content_type");

    // WebP + Gzip
    var maskWg = classify("image/webp", null, null, "gzip");
    Assert(maskWg != 0, $"WebP+Gzip mask=0x{maskWg:X8}");

    // Null → default (no crash, returns some mask)
    var maskNull = classify(null, null, null, null);
    Pass($"null classify mask=0x{maskNull:X8}");

    // Content type classification
    Assert(classifyCt("text/html") == 0, "ClassifyContentType text/html == Html(0)");
    Assert(classifyCt("text/css") == 1, "ClassifyContentType text/css == Css(1)");
    Assert(classifyCt("application/javascript") == 2, "ClassifyContentType js == Js(2)");

    // ======== T1.3: CSS Validation & Minification ========
    Console.WriteLine("\n--- T1.3: CSS Validation & Minification ---");
    var cssValidate = GetFn<FnBufRetInt>(h, "ps_css_validate");
    var cssMinify = GetFn<FnCssMinify>(h, "ps_css_minify");
    var psFree = GetFn<FnFreePtr>(h, "ps_free");

    var goodCss = Encoding.UTF8.GetBytes("body { color: red; }");
    Assert(cssValidate(goodCss, (nuint)goodCss.Length) == 0, "CSS validate good → Ok");

    var cssIn = Encoding.UTF8.GetBytes("body {\n  color:  red;\n}");
    int minErr = cssMinify(cssIn, (nuint)cssIn.Length, out var minPtr, out var minLen);
    Assert(minErr == 0, "CSS minify → Ok");
    if (minErr == 0 && minPtr != IntPtr.Zero) {
        var minified = PtrToStr(minPtr);
        Assert(minified.Contains("color:red"), $"Minified contains color:red: '{minified}'");
        psFree(minPtr);
    }

    // ======== T1.4: HTML Scanning ========
    Console.WriteLine("\n--- T1.4: HTML Scanning ---");
    var htmlScan = GetFn<FnHtmlScan>(h, "ps_html_scan");
    var scanElCount = GetFn<FnPtrRetNuint>(h, "ps_scan_element_count");
    var scanSsCount = GetFn<FnPtrRetNuint>(h, "ps_scan_stylesheet_count");
    var scanSs = GetFn<FnScanStylesheet>(h, "ps_scan_stylesheet");
    var scanOriginCount = GetFn<FnPtrRetNuint>(h, "ps_scan_origin_count");
    var scanFree = GetFn<FnFreePtr>(h, "ps_scan_result_free");

    var html = Encoding.UTF8.GetBytes(
        "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>" +
        "<body><img src=\"/img.png\"></body></html>");
    var urlPtr = StrToPtr("https://example.com/");
    int scanErr = htmlScan(html, (nuint)html.Length, urlPtr, out var scanResult);
    Marshal.FreeCoTaskMem(urlPtr);
    Assert(scanErr == 0, "HTML scan → Ok");
    if (scanErr == 0 && scanResult != IntPtr.Zero) {
        var elCount = scanElCount(scanResult);
        Assert(elCount > 0, $"Element count = {elCount}");
        var ssCount = scanSsCount(scanResult);
        Assert(ssCount >= 1, $"Stylesheet count = {ssCount}");
        if (ssCount >= 1) {
            scanSs(scanResult, (nuint)0, out var hrefPtr, out var mediaPtr);
            var href = PtrToStr(hrefPtr);
            Assert(href.Contains("style.css"), $"Stylesheet href = {href}");
        }
        var origCount = scanOriginCount(scanResult);
        Pass($"Origin count = {origCount}");
        scanFree(scanResult);
    }

    // ======== T1.5: Cache Lifecycle ========
    Console.WriteLine("\n--- T1.5: Cache Lifecycle ---");
    var cacheConfigInit = GetFn<FnRefConfig>(h, "ps_cache_config_init");
    var cacheOpen = GetFn<FnCacheOpen>(h, "ps_cache_open");
    var cacheClose = GetFn<FnFreePtr>(h, "ps_cache_close");
    var writeParamsInit = GetFn<FnRefWriteParams>(h, "ps_write_params_init");
    var cacheWriteBegin = GetFn<FnCacheWriteBegin>(h, "ps_cache_write_begin");
    var writeData = GetFn<FnWriteData>(h, "ps_write_data");
    var writeClose = GetFn<FnPtrRetInt>(h, "ps_write_close");
    var cacheReadBest = GetFn<FnCacheReadBest>(h, "ps_cache_read_best");
    var readContent = GetFn<FnReadContent>(h, "ps_read_content");
    var readMask = GetFn<FnPtrRetUint>(h, "ps_read_mask");
    var readContentType = GetFn<FnPtrRetInt>(h, "ps_read_content_type");
    var readFree = GetFn<FnFreePtr>(h, "ps_read_free");
    var cacheStats = GetFn<FnCacheStats>(h, "ps_cache_stats");
    var cacheRemove = GetFn<FnCacheRemove>(h, "ps_cache_remove");

    var tmpDir = Path.Combine(Path.GetTempPath(), $"ps-cache-test-{Environment.ProcessId}");
    Directory.CreateDirectory(tmpDir);
    try {
        // Init config
        var cfg = new NativeCacheConfig();
        cacheConfigInit(ref cfg);
        var volumePath = Path.Combine(tmpDir, "volume.dat");
        var pathPtr = StrToPtr(volumePath);
        cfg.VolumePath = pathPtr;
        cfg.VolumeSize = 16 * 1024 * 1024; // 16 MB

        int openErr = cacheOpen(ref cfg, out var cachePtr);
        Marshal.FreeCoTaskMem(pathPtr);
        Assert(openErr == 0 && cachePtr != IntPtr.Zero, "Cache open → Ok");

        if (openErr == 0 && cachePtr != IntPtr.Zero) {
            // Write
            var wp = new NativeWriteParams();
            writeParamsInit(ref wp);
            var body = Encoding.UTF8.GetBytes("hello cache");
            wp.ContentLength = (ulong)body.Length;
            wp.FullMask = 0x01;
            wp.ContentType = 0; // Html

            var wUrl = StrToPtr("https://example.com/test");
            var wHost = StrToPtr("example.com");
            int wbErr = cacheWriteBegin(cachePtr, wUrl, wHost, ref wp, out var wh);
            Assert(wbErr == 0, "Cache write begin → Ok");
            if (wbErr == 0 && wh != IntPtr.Zero) {
                int wdErr = writeData(wh, body, (nuint)body.Length);
                Assert(wdErr == 0, "Cache write data → Ok");
                int wcErr = writeClose(wh);
                Assert(wcErr == 0, "Cache write close → Ok");
            }

            // Read back
            int rbErr = cacheReadBest(cachePtr, wUrl, wHost, 0x01, out var readPtr);
            Assert(rbErr == 0 && readPtr != IntPtr.Zero, "Cache read best → Ok");
            if (rbErr == 0 && readPtr != IntPtr.Zero) {
                int rcErr = readContent(readPtr, out var dataPtr, out var dataLen);
                Assert(rcErr == 0, "Read content → Ok");
                if (rcErr == 0 && dataPtr != IntPtr.Zero) {
                    unsafe {
                        var content = Encoding.UTF8.GetString(
                            new ReadOnlySpan<byte>((void*)dataPtr, (int)dataLen));
                        Assert(content == "hello cache", $"Content = '{content}'");
                    }
                }
                var rMask = readMask(readPtr);
                Assert(rMask == 0x01, $"Read mask = 0x{rMask:X}");
                var rCt = readContentType(readPtr);
                Assert(rCt == 0, $"Read content type = {rCt} (Html)");
                readFree(readPtr);
            }

            // Stats
            var stats = new NativeCacheStats();
            stats.StructSize = (nuint)Marshal.SizeOf<NativeCacheStats>();
            int stErr = cacheStats(cachePtr, ref stats);
            Assert(stErr == 0, "Cache stats → Ok");
            if (stErr == 0)
                Assert(stats.CurrentEntries >= 1, $"Cache entries = {stats.CurrentEntries}");

            // Remove
            int rmErr = cacheRemove(cachePtr, wUrl, wHost);
            Assert(rmErr == 0, "Cache remove → Ok");

            Marshal.FreeCoTaskMem(wUrl);
            Marshal.FreeCoTaskMem(wHost);
            cacheClose(cachePtr);
            Pass("Cache closed");
        }
    } finally {
        try { Directory.Delete(tmpDir, true); } catch { }
    }

    // ======== T1.6: HTML Processing ========
    Console.WriteLine("\n--- T1.6: HTML Processing ---");
    var htmlConfigInit = GetFn<FnRefHtmlConfig>(h, "ps_html_config_init");
    var htmlProcess = GetFn<FnHtmlProcess>(h, "ps_html_process");
    var htmlResultModified = GetFn<FnPtrRetInt>(h, "ps_html_result_modified");
    var htmlResultOutput = GetFn<FnHtmlResultOutput>(h, "ps_html_result_output");
    var htmlResultFree = GetFn<FnFreePtr>(h, "ps_html_result_free");

    var htmlCfg = new NativeHtmlConfig();
    htmlConfigInit(ref htmlCfg);
    htmlCfg.EnableLazyLoad = 1;

    var htmlDoc = Encoding.UTF8.GetBytes(
        "<!DOCTYPE html><html><head><title>Test</title></head>" +
        "<body><h1>Hello World</h1>" +
        "<img src=\"https://example.com/hero.jpg\" width=\"800\" height=\"600\">" +
        "</body></html>");
    var hUrl = StrToPtr("https://example.com/page");
    var hHost = StrToPtr("example.com");
    int hpErr = htmlProcess(htmlDoc, (nuint)htmlDoc.Length, hUrl, hHost,
        ref htmlCfg, IntPtr.Zero, out var hResult);
    Marshal.FreeCoTaskMem(hUrl);
    Marshal.FreeCoTaskMem(hHost);
    Assert(hpErr == 0, "HTML process → Ok");
    if (hpErr == 0 && hResult != IntPtr.Zero) {
        var modified = htmlResultModified(hResult);
        Assert(modified != 0, "HTML was modified");
        var outPtr = htmlResultOutput(hResult, out var outLen);
        Assert(outPtr != IntPtr.Zero && outLen > 0, $"HTML output non-empty ({outLen} bytes)");
        if (outPtr != IntPtr.Zero && outLen > 0) {
            unsafe {
                var output = Encoding.UTF8.GetString(
                    new ReadOnlySpan<byte>((void*)outPtr, (int)outLen));
                bool hasLazy = output.Contains("loading=");
                if (hasLazy)
                    Pass("Output has loading= attribute");
                else
                    Console.WriteLine($"[INFO] loading= not found (output may use different optimizations)");
            }
        }
        htmlResultFree(hResult);
    }

} finally {
    NativeLibrary.Free(h);
}

Console.WriteLine($"\n=== {passed} passed, {failures} failed ===");

// Guard: require minimum number of passing tests to prevent silent false positives
const int MIN_EXPECTED_PASS = 15;
if (passed < MIN_EXPECTED_PASS)
{
    Console.WriteLine($"[FAIL] Only {passed} tests passed (minimum {MIN_EXPECTED_PASS} required)");
    Console.WriteLine("       Tests may have been silently skipped — check native library availability.");
    failures++;
}
return failures;

// ======== Delegate types (Cdecl) ========
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int FnInt();
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr FnIntRetPtr(int v);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate uint FnClassify(
    [MarshalAs(UnmanagedType.LPUTF8Str)] string? accept,
    [MarshalAs(UnmanagedType.LPUTF8Str)] string? ua,
    [MarshalAs(UnmanagedType.LPUTF8Str)] string? saveData,
    [MarshalAs(UnmanagedType.LPUTF8Str)] string? ae);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnClassifyCt([MarshalAs(UnmanagedType.LPUTF8Str)] string ct);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnBufRetInt(byte[] buf, nuint len);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnCssMinify(byte[] buf, nuint len, out IntPtr outCss, out nuint outLen);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FnFreePtr(IntPtr p);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnHtmlScan(byte[] html, nuint len, IntPtr url, out IntPtr result);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate nuint FnPtrRetNuint(IntPtr p);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnScanStylesheet(IntPtr result, nuint idx, out IntPtr href, out IntPtr media);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate void FnRefConfig(ref NativeCacheConfig cfg);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnCacheOpen(ref NativeCacheConfig cfg, out IntPtr cache);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate void FnRefWriteParams(ref NativeWriteParams p);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnCacheWriteBegin(IntPtr cache, IntPtr url, IntPtr host,
    ref NativeWriteParams p, out IntPtr wh);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnWriteData(IntPtr wh, byte[] data, nuint len);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int FnPtrRetInt(IntPtr p);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate uint FnPtrRetUint(IntPtr p);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnCacheReadBest(IntPtr cache, IntPtr url, IntPtr host,
    uint mask, out IntPtr result);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnReadContent(IntPtr result, out IntPtr data, out nuint len);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnCacheStats(IntPtr cache, ref NativeCacheStats stats);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnCacheRemove(IntPtr cache, IntPtr url, IntPtr host);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate void FnRefHtmlConfig(ref NativeHtmlConfig cfg);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int FnHtmlProcess(byte[] html, nuint len, IntPtr url, IntPtr host,
    ref NativeHtmlConfig cfg, IntPtr cache, out IntPtr result);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate IntPtr FnHtmlResultOutput(IntPtr result, out nuint len);

// ======== Native structs (matching NativeStructs.cs layouts) ========
[StructLayout(LayoutKind.Explicit, Size = 48)]
struct NativeCacheConfig {
    [FieldOffset(0)]  public nuint StructSize;
    [FieldOffset(8)]  public IntPtr VolumePath;
    [FieldOffset(16)] public ulong VolumeSize;
    [FieldOffset(24)] public int EnableChecksum;
    [FieldOffset(32)] public nuint RamCacheSize;
    [FieldOffset(40)] public nuint MaxMetadataSize;
}

[StructLayout(LayoutKind.Explicit, Size = 48)]
struct NativeWriteParams {
    [FieldOffset(0)]  public nuint StructSize;
    [FieldOffset(8)]  public byte AlternateId;
    [FieldOffset(16)] public ulong ContentLength;
    [FieldOffset(24)] public uint FullMask;
    [FieldOffset(28)] public int ContentType;
    [FieldOffset(32)] public byte Flags;
    [FieldOffset(40)] public IntPtr OriginCt;
}

[StructLayout(LayoutKind.Sequential)]
struct NativeCacheStats {
    public nuint StructSize;
    public ulong RamCacheHits, RamCacheMisses, DiskCacheHits, DiskCacheMisses;
    public ulong BytesRead, BytesWritten, Evictions;
    public ulong CurrentEntries, CurrentSizeBytes, VolumeCapacityBytes, RamCacheBytes;
    public ulong TotalHits, TotalMisses;
}

[StructLayout(LayoutKind.Explicit, Size = 120)]
struct NativeHtmlConfig {
    [FieldOffset(0)]   public nuint StructSize;
    [FieldOffset(8)]   public int EnableCriticalCss;
    [FieldOffset(12)]  public int EnableLazyLoad;
    [FieldOffset(16)]  public int EnableImageDimensions;
    [FieldOffset(20)]  public int EnableLcpPreload;
    [FieldOffset(24)]  public int EnablePreconnect;
    [FieldOffset(28)]  public int EnableSpeculationRules;
    [FieldOffset(32)]  public int CriticalCssMaxElements;
    [FieldOffset(36)]  public int CriticalCssMaxDepth;
    [FieldOffset(40)]  public int CssImportMaxDepth;
    [FieldOffset(44)]  public int Viewport;
    [FieldOffset(48)]  public nuint MaxHtmlSize;
    [FieldOffset(56)]  public nuint MaxCssSize;
    [FieldOffset(64)]  public IntPtr AlwaysIncludeSelectors;
    [FieldOffset(72)]  public IntPtr IncludeTagPatterns;
    [FieldOffset(80)]  public IntPtr IncludeClassPatterns;
    [FieldOffset(88)]  public IntPtr IncludeIdPatterns;
    [FieldOffset(96)]  public IntPtr ExcludeClassPatterns;
    [FieldOffset(104)] public IntPtr ExcludeIdPatterns;
    [FieldOffset(112)] public IntPtr ExcludeTagPatterns;
}
CSEOF

                # Run via cmd.exe
                if /mnt/c/Windows/System32/cmd.exe /c "cd /d $WIN_TEMP && $WIN_DOTNET build -r win-x64 -c Release --verbosity quiet && $WIN_DOTNET run -r win-x64 --no-build -c Release" 2>&1; then
                    echo "  win-x64 smoke test: PASSED"
                else
                    echo "  win-x64 smoke test: FAILED"
                    FAILURES=$((FAILURES + 1))
                fi

                # Cleanup
                /mnt/c/Windows/System32/cmd.exe /c "rmdir /s /q $WIN_TEMP" 2>/dev/null || true
                ;;

            *)
                echo "  SKIP: no smoke test runner for $rid"
                ;;
        esac

        step_timer "$STAGE_START"
    done
    echo ""
else
    echo "━━━ Stage 5: SKIPPED (--skip-smoke) ━━━"
    echo ""
fi

# ============================================================
# Summary
# ============================================================
ELAPSED=$(( $(date +%s) - PIPELINE_START ))
echo "╔══════════════════════════════════════════════════════╗"
if [[ "$FAILURES" -eq 0 ]]; then
    echo "║  PIPELINE PASSED  ($ELAPSED seconds)                       "
else
    echo "║  PIPELINE FAILED  ($FAILURES failure(s), $ELAPSED seconds)            "
fi
echo "╠══════════════════════════════════════════════════════╣"
echo "║  Packages in: $NUPKG_DIR"
for f in "$NUPKG_DIR"/*.nupkg; do
    echo "║    $(basename "$f")"
done
echo "║"
echo "║  To publish:"
echo "║    dotnet nuget push 'nupkg/*.nupkg' \\"
echo "║      --api-key <KEY> --source https://api.nuget.org/v3/index.json"
echo "╚══════════════════════════════════════════════════════╝"

exit "$FAILURES"
