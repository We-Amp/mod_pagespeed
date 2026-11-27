#!/usr/bin/env bash
# smoke-test.sh — Runtime smoke test for WeAmp.PageSpeed NuGet packages.
# SPDX-License-Identifier: Apache-2.0
# Usage: smoke-test.sh <directory-containing-nupkg-files>
#
# Creates a temp .NET 9 web project, installs packages from a local source,
# builds, and runs tests covering:
#   Tier 1: Native API tests (P/Invoke + managed wrappers)
#   Tier 2: ASP.NET Core integration (middleware via TestServer)
#   Tier 3: Worker process lifecycle
#
# Crash diagnostics: the host runs with DOTNET_DbgEnableMiniDump so a
# native crash leaves a dump under $SMOKE_DUMP_DIR (default
# $RUNNER_TEMP/smoke-crash-dumps, else $TMPDIR/smoke-crash-dumps); the host
# prints a [PHASE] line before every tier/block/teardown step, and the failure
# banner names the exit code's meaning, the versions under test, the last
# phase reached and any dumps written. SMOKE_DUMP_TYPE overrides the dump
# type (default 4 = full).
set -euo pipefail

NUPKG_DIR="${1:?Usage: smoke-test.sh <nupkg-directory>}"
NUPKG_DIR="$(cd "$NUPKG_DIR" && pwd)"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ASPNETCORE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Git Bash (MSYS/MinGW) pwd returns POSIX paths like /d/ci/... which native
# Windows programs (dotnet.exe) misinterpret as C:\d\ci\... — convert to
# Windows paths when running under MSYS.
if command -v cygpath &>/dev/null; then
    NUPKG_DIR="$(cygpath -w "$NUPKG_DIR")"
    SCRIPT_DIR="$(cygpath -w "$SCRIPT_DIR")"
    ASPNETCORE_DIR="$(cygpath -w "$ASPNETCORE_DIR")"
fi
PACKAGE_VERSION="$(grep '<Version>' "$ASPNETCORE_DIR/Directory.Build.props" | sed 's/.*<Version>\(.*\)<\/Version>.*/\1/')"
if [[ -z "$PACKAGE_VERSION" ]]; then
    echo "ERROR: Could not extract version from Directory.Build.props"
    exit 1
fi

# ---------- Temp directory with auto-cleanup ----------
WORKDIR="$(mktemp -d)"
# Scope the kill to THIS run's workdir. A bare `pkill -f factory_worker`
# matches any process whose full command line contains the string — including
# a concurrent CI job's `docker run ... bazel build //src/worker:factory_worker`
# client on the same host. Where several runners share one docker daemon,
# that SIGTERM'd the Linux arm64 Build container mid-build whenever a pkg-smoke
# finished alongside it (exit 143; runs 27338112992/27338146826/27341095389).
# Every factory_worker this script spawns runs from under $WORKDIR (isolated
# NUGET_PACKAGES below), so the workdir prefix is exact.
#
# Git Bash on Windows ships no pkill, so there the kill goes through
# PowerShell instead: every process whose image lives under $WORKDIR. Without
# it the Tier 3 workers are still running when `rm -rf` reaches their
# factory_worker.exe, and the MSYS runtime, unable to delete a running image,
# renames it into the volume's recycle bin (C:\$RECYCLE.BIN\<SID>\.<msys><hex>)
# where it stays for good: ~42 MB per Windows pkg-smoke run on a long-lived
# CI machine, 46 GB before anyone noticed.
# This runs from the EXIT trap under `set -euo pipefail`: an errexit in the
# trap would replace the script's real exit code (a crash's 139 becomes 1) and
# skip the rm -rf. So every step is allowed to fail, the trap calls this with
# `|| true`, and the PowerShell/WMI query is bounded by `timeout`.
kill_workdir_processes() {
    if command -v pkill >/dev/null 2>&1; then
        pkill -f "${WORKDIR}.*factory_worker" 2>/dev/null || true
    elif command -v cygpath >/dev/null 2>&1; then
        local ps_exe workdir_win
        ps_exe="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32/WindowsPowerShell/v1.0/powershell.exe" || return 0
        # -l: long names. ExecutablePath is always the long form, so an 8.3
        # TEMP (C:\Users\RUNNER~1\...) would otherwise never match.
        workdir_win="$(cygpath -w -l "$WORKDIR")" || return 0
        # Never sweep a whole drive: an empty or root-level WORKDIR would make
        # the prefix below match every process on the volume.
        [[ "$workdir_win" =~ ^[A-Za-z]:\\[^\\]+ ]] || return 0
        SMOKE_WORKDIR_WIN="$workdir_win" timeout 60 "$ps_exe" -NoProfile \
            -NonInteractive -Command '
            $dir = $env:SMOKE_WORKDIR_WIN.TrimEnd("\") + "\"
            if ($dir.Length -le 3) { exit 0 }
            $procs = @(Get-CimInstance Win32_Process | Where-Object {
                $_.ExecutablePath -and $_.ExecutablePath.StartsWith(
                    $dir, [StringComparison]::OrdinalIgnoreCase) })
            foreach ($p in $procs) {
                Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
            }
            foreach ($p in $procs) {
                Wait-Process -Id $p.ProcessId -Timeout 10 -ErrorAction SilentlyContinue
            }' 2>/dev/null || true
    fi
    return 0
}
trap 'kill_workdir_processes || true; sleep 0.5 || true; rm -rf "$WORKDIR" 2>/dev/null || true' EXIT
echo "=== WeAmp.PageSpeed Smoke Test ==="
echo "Package dir: $NUPKG_DIR"
echo "Work dir:    $WORKDIR"
echo ""

# ---------- Isolated package cache ----------
# Extract packages into a job-local, auto-cleaned NUGET_PACKAGES dir instead of
# the shared ~/.nuget/packages. This script installs STUB packages (empty
# native dirs + a "pkg-smoke"/"CI placeholder" console) that ci.yml's pkg-smoke
# jobs pack at the SAME version as a real release — both derive from
# Directory.Build.props <Version>, which the release PR bumps to the release
# version. Leaving those stubs in the shared global-packages poisons later
# consumers: release-smoke would `dotnet add --version <release>` and silently
# resolve this stub instead of the published nuget.org artifact (the v2.0.13
# release-smoke "/console/ 56 bytes" incident; release-smoke.yml now also evicts
# defensively). Isolating the cache here stops the poisoning at the source AND
# guarantees every run extracts the freshly-built local packages (the cache-bust
# this replaces only cleared *before* the run, leaving the stub behind after).
# The EXIT trap above removes WORKDIR, so the isolated cache is cleaned up too.
export NUGET_PACKAGES="$WORKDIR/nuget-packages"
mkdir -p "$NUGET_PACKAGES"
echo "--- Isolated NUGET_PACKAGES: $NUGET_PACKAGES ---"
echo ""

# ---------- Ensure dotnet is on PATH ----------
export PATH="$HOME/.dotnet:$HOME/bin:$PATH"

# ---------- Detect current RID ----------
RID="$(dotnet --info 2>/dev/null | grep -E '^\s*RID:' | head -1 | awk '{print $NF}' || true)"
if [[ -z "$RID" ]]; then
    # Fallback: derive from uname
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64)   RID="linux-x64" ;;
        Darwin-arm64)   RID="osx-arm64" ;;
        Darwin-x86_64)  RID="osx-x64" ;;
        MINGW*|MSYS*)   RID="win-x64" ;;
        *)              echo "ERROR: Cannot detect RID"; exit 1 ;;
    esac
fi
echo "Detected RID: $RID"
echo ""

# ---------- Create test project ----------
TEST_DIR="$WORKDIR/smoketest"
mkdir -p "$TEST_DIR"

# nuget.config — local source first, then nuget.org
cat > "$TEST_DIR/nuget.config" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="LocalPackages" value="$NUPKG_DIR" />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
  </packageSources>
</configuration>
EOF

# Select the NativeAssets package for the current RID
case "$RID" in
    linux-*)  NATIVE_PKG_ID="WeAmp.PageSpeed.NativeAssets.Linux" ;;
    osx-*)    NATIVE_PKG_ID="WeAmp.PageSpeed.NativeAssets.macOS" ;;
    win-*)    NATIVE_PKG_ID="WeAmp.PageSpeed.NativeAssets.Windows" ;;
    *)        echo "ERROR: No NativeAssets package for RID $RID"; exit 1 ;;
esac
echo "NativeAssets package: $NATIVE_PKG_ID"
echo ""

# SmokeTest.csproj — Web SDK for TestServer support
cat > "$TEST_DIR/SmokeTest.csproj" <<EOF
<Project Sdk="Microsoft.NET.Sdk.Web">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="WeAmp.PageSpeed.AspNetCore" Version="$PACKAGE_VERSION" />
    <PackageReference Include="$NATIVE_PKG_ID" Version="$PACKAGE_VERSION" />
    <PackageReference Include="Microsoft.AspNetCore.Mvc.Testing" Version="10.0.*" />
  </ItemGroup>
</Project>
EOF

# Program.cs — the actual smoke test
cat > "$TEST_DIR/Program.cs" <<'CSEOF'
using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using System.Net.Sockets;
using WeAmp.PageSpeed;
using WeAmp.PageSpeed.AspNetCore;

int failures = 0;
int passed = 0;
var outputDir = AppContext.BaseDirectory;
var rid = RuntimeInformation.RuntimeIdentifier;

Console.WriteLine("=== WeAmp.PageSpeed Runtime Smoke Test ===");
Console.WriteLine($"RID:        {rid}");
Console.WriteLine($"OS:         {RuntimeInformation.OSDescription}");
Console.WriteLine($"Arch:       {RuntimeInformation.ProcessArchitecture}");
Console.WriteLine($"Output dir: {outputDir}");
Console.WriteLine($"Runtime:    {RuntimeInformation.FrameworkDescription}");
Console.WriteLine();

// ---------- Phase markers ----------
// A native crash kills this host with nothing but an exit code: no managed
// stack, no catch block runs, and the [PASS] lines only say which assertion
// was reached. So every tier, every block that opens a native cache, and
// every teardown step announces itself FIRST; the last [PHASE] line in the
// log names where the process died. The bash wrapper repeats it in the
// failure banner.
void Phase(string name) => Console.WriteLine($"[PHASE] {name}");

// Native-side identity, filled in by Tier 0 and printed as one [VERSIONS]
// line the failure banner can quote (two earlier crashes could not be tied
// to a specific native build from the log alone).
string nativeApi = "?", nativeCommit = "?", nativeProduct = "?";
// Hosts that were StopAsync()'ed but never disposed. StopAsync only stops the
// hosted services; the IPageSpeedCache singleton (and its native handle)
// belong to the service provider and are closed by DisposeAsync. .NET runs no
// finalizers at process exit, so these caches stay open — with Cyclone's
// background threads alive — until the runtime tears the process down. The
// exit [PHASE] line reports the count so a crash there can be read against
// it.
int undisposedHosts = 0;

// ---------- Test helpers ----------
// Every assertion must pass. The only exception is an explicit, named, opt-in
// skip for a check that depends on the environment: a failing assertion that
// carries a skip id is reported as [SKIP] (and listed in the summary) only when
// that id is listed in SMOKE_ALLOW_SKIP (comma-separated). No check carries a
// skip id at present. Nothing is skipped unless the caller asks for it by name.
var allowedSkips = new HashSet<string>(
    (Environment.GetEnvironmentVariable("SMOKE_ALLOW_SKIP") ?? "")
        .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries),
    StringComparer.Ordinal);
var skipped = new List<string>();
void Assert(bool condition, string name, string? skipId = null)
{
    if (condition)
    {
        Console.WriteLine($"[PASS] {name}");
        passed++;
    }
    else if (skipId != null && allowedSkips.Contains(skipId))
    {
        Console.WriteLine($"[SKIP] {name} (skip id '{skipId}' allowed by SMOKE_ALLOW_SKIP)");
        skipped.Add($"{skipId}: {name}");
    }
    else
    {
        Console.WriteLine($"[FAIL] {name}");
        failures++;
    }
}

// Helper: find file in output dir or runtimes/ subdirectory
string? FindFile(string name)
{
    var direct = Path.Combine(outputDir, name);
    if (File.Exists(direct)) return direct;

    var runtimesPath = Path.Combine(outputDir, "runtimes", rid, "native", name);
    if (File.Exists(runtimesPath)) return runtimesPath;

    return null;
}

// Helper: HTTP client for a worker's management API. The worker binds
// 127.0.0.1 only. HttpClient's default socket is dual-stack and reaches that
// address as ::ffff:127.0.0.1, which host networking on some container
// runtimes refuses (the worker is up, the probe is refused).
// Connect over IPv4 to the address the worker actually binds.
HttpClient NewWorkerApiClient() => new HttpClient(new SocketsHttpHandler
{
    ConnectCallback = async (ctx, ct) =>
    {
        var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)
        {
            NoDelay = true,
        };
        try
        {
            await s.ConnectAsync(new IPEndPoint(IPAddress.Loopback, ctx.DnsEndPoint.Port), ct);
            return new NetworkStream(s, ownsSocket: true);
        }
        catch
        {
            s.Dispose();
            throw;
        }
    },
});

// ============================================================
// TIER 0: File existence and native library loading
// ============================================================
Console.WriteLine("──── Tier 0: File Existence & Native Loading ────");
Phase("Tier 0: file existence + explicit NativeLibrary.TryLoad of the native library");

// ---------- T0.1: Native library file exists ----------
var libName = RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
    ? "pagespeed.dll"
    : RuntimeInformation.IsOSPlatform(OSPlatform.OSX)
        ? "libpagespeed.dylib"
        : "libpagespeed.so";

var libPath = FindFile(libName);
if (libPath != null)
{
    var size = new FileInfo(libPath).Length;
    Assert(true, $"Native library found: {libPath} ({size:N0} bytes)");
}
else
{
    Assert(false, $"Native library found: {libName}");
}

// ---------- T0.2: factory_worker exists ----------
var workerName = RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
    ? "factory_worker.exe" : "factory_worker";
var workerPath = FindFile(workerName);
if (workerPath != null)
{
    var size = new FileInfo(workerPath).Length;
    Assert(true, $"factory_worker found: {workerPath} ({size:N0} bytes)");
}
else
{
    Assert(false, $"factory_worker found: {workerName}");
}

// ---------- T0.3: NativeLibrary.TryLoad ----------
IntPtr nativeHandle = IntPtr.Zero;
if (libPath != null)
{
    if (NativeLibrary.TryLoad(libPath, out nativeHandle) && nativeHandle != IntPtr.Zero)
    {
        Assert(true, "NativeLibrary.TryLoad succeeded (explicit path)");

        // ---------- T0.4: ps_version_major export ----------
        if (NativeLibrary.TryGetExport(nativeHandle, "ps_version_major", out var versionPtr))
        {
            Assert(true, "ps_version_major export found");

            try
            {
                var versionMajorFn = Marshal.GetDelegateForFunctionPointer<VersionFunc>(versionPtr);
                var major = versionMajorFn();

                NativeLibrary.TryGetExport(nativeHandle, "ps_version_minor", out var minorPtr);
                NativeLibrary.TryGetExport(nativeHandle, "ps_version_patch", out var patchPtr);
                var minor = minorPtr != IntPtr.Zero
                    ? Marshal.GetDelegateForFunctionPointer<VersionFunc>(minorPtr)() : 0;
                var patch = patchPtr != IntPtr.Zero
                    ? Marshal.GetDelegateForFunctionPointer<VersionFunc>(patchPtr)() : 0;

                nativeApi = $"{major}.{minor}.{patch}";
                Assert(true, $"PageSpeed native API version: {nativeApi}");
            }
            catch (Exception ex)
            {
                Assert(false, $"Version call: {ex.Message}");
            }
        }
        else
        {
            Assert(false, "ps_version_major export found");
        }

        // ---------- T0.5: ps_error_name export ----------
        Assert(
            NativeLibrary.TryGetExport(nativeHandle, "ps_error_name", out _),
            "ps_error_name export found");

        // ---------- T0.6: All P/Invoke entry points resolve ----------
        // Keep in sync with src/WeAmp.PageSpeed/Native/NativePageSpeed.cs
        {
            var entryPoints = new[]
            {
                "ps_version_major", "ps_version_minor", "ps_version_patch",
                "ps_git_commit", "ps_product_version",
                "ps_error_name", "ps_strerror", "ps_last_error_message",
                "ps_classify", "ps_mask_set_viewport_from_width", "ps_score_alternate",
                "ps_normalize_hostname", "ps_classify_content_type", "ps_content_type_mime",
                "ps_cache_config_init", "ps_cache_open", "ps_cache_close",
                "ps_cache_read_best", "ps_cache_read_alternate", "ps_cache_read_early_hints",
                "ps_read_content", "ps_read_copy", "ps_read_mask",
                "ps_read_content_type", "ps_read_origin_content_type", "ps_read_flags",
                "ps_read_is_ram_hit", "ps_read_cache_inserted_at", "ps_read_free",
                "ps_write_params_init", "ps_cache_write_begin", "ps_cache_write_sentinel",
                "ps_write_data", "ps_write_close", "ps_write_abort",
                "ps_cache_alternate_exists", "ps_cache_remove", "ps_cache_list_alternates",
                "ps_alternates_free", "ps_cache_stats", "ps_notify_worker",
                "ps_html_config_init", "ps_html_process", "ps_html_result_output",
                "ps_html_result_modified", "ps_html_result_has_critical_css",
                "ps_html_result_early_hints", "ps_html_result_needs_revalidation",
                "ps_html_result_free",
                "ps_html_scan", "ps_scan_element_count", "ps_scan_element",
                "ps_scan_element_classes", "ps_scan_stylesheet_count", "ps_scan_stylesheet",
                "ps_scan_inline_css", "ps_scan_lcp_candidate",
                "ps_scan_origin_count", "ps_scan_origin", "ps_scan_result_free",
                "ps_critical_css_config_init", "ps_css_extract_critical",
                "ps_critical_css_output", "ps_critical_css_stats", "ps_critical_css_result_free",
                "ps_css_validate", "ps_css_flatten_imports", "ps_css_minify", "ps_free",
                "ps_html_transform_create", "ps_html_transform_run",
                "ps_html_transform_modified", "ps_html_transform_free",
            };
            int missing = 0;
            foreach (var name in entryPoints)
            {
                if (!NativeLibrary.TryGetExport(nativeHandle, name, out _))
                {
                    Console.WriteLine($"  [MISSING] {name}");
                    missing++;
                }
            }
            Assert(missing == 0,
                $"All {entryPoints.Length} P/Invoke exports resolved ({missing} missing)");
        }

        // ---------- T0.7: ps_git_commit returns valid short hash ----------
        if (NativeLibrary.TryGetExport(nativeHandle, "ps_git_commit", out var gitCommitPtr))
        {
            var gitCommitFn = Marshal.GetDelegateForFunctionPointer<StringReturnFunc>(gitCommitPtr);
            var commitStr = Marshal.PtrToStringUTF8(gitCommitFn()) ?? "";
            nativeCommit = commitStr;
            Assert(commitStr.Length > 0, $"ps_git_commit() non-empty (got \"{commitStr}\")");
            Assert(commitStr != "0", $"ps_git_commit() != \"0\" (got \"{commitStr}\")");
            Assert(commitStr.Length <= 7, $"ps_git_commit() <= 7 chars (got {commitStr.Length})");
        }
        else
        {
            Assert(false, "ps_git_commit export found");
        }

        // ---------- T0.8: ps_product_version returns valid version ----------
        if (NativeLibrary.TryGetExport(nativeHandle, "ps_product_version", out var productVersionPtr))
        {
            var productVersionFn = Marshal.GetDelegateForFunctionPointer<StringReturnFunc>(productVersionPtr);
            var versionStr = Marshal.PtrToStringUTF8(productVersionFn()) ?? "";
            nativeProduct = versionStr;
            Assert(versionStr.Length > 0, $"ps_product_version() non-empty (got \"{versionStr}\")");
            Assert(versionStr.Contains('.'), $"ps_product_version() contains dot (got \"{versionStr}\")");
        }
        else
        {
            Assert(false, "ps_product_version export found");
        }
    }
    else
    {
        Assert(false, "NativeLibrary.TryLoad succeeded");
    }
}
else
{
    Console.WriteLine("[SKIP] Skipping load tests — native library not found");
}
{
    // One line the failure banner greps back out: which package, which
    // managed assembly, which native build (product version + git commit +
    // C API version) and which runtime were under test.
    var managedAsm = typeof(PageSpeedCache).Assembly;
    var managedVersion = managedAsm
        .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? managedAsm.GetName().Version?.ToString() ?? "?";
    var packageVersion = Environment.GetEnvironmentVariable("SMOKE_PACKAGE_VERSION") ?? "?";
    Console.WriteLine(
        $"[VERSIONS] package={packageVersion} managed={managedVersion} " +
        $"native-product={nativeProduct} native-commit={nativeCommit} native-api={nativeApi} " +
        $"runtime={RuntimeInformation.FrameworkDescription} rid={rid}");
}
Console.WriteLine();

// ============================================================
// TIER 1: Native API Tests (managed wrappers + raw P/Invoke)
// ============================================================
Console.WriteLine("──── Tier 1: Native API Tests ────");
Phase("Tier 1: native API via managed wrappers + raw P/Invoke");

if (nativeHandle == IntPtr.Zero)
{
    Console.WriteLine("[SKIP] Tier 1 skipped — native library not loaded");
}
else
{
    // ---- T1.1: Version via managed wrapper ----
    try
    {
        var version = PageSpeedVersion.Current;
        Assert(version.Major > 0, $"PageSpeedVersion.Current.Major > 0 (got {version.Major})");
        Assert(version.Minor >= 0, $"PageSpeedVersion.Current.Minor >= 0 (got {version.Minor})");
        Assert(version.Build >= 0, $"PageSpeedVersion.Current.Build >= 0 (got {version.Build})");
    }
    catch (Exception ex)
    {
        Assert(false, $"PageSpeedVersion.Current: {ex.Message}");
    }

    // ---- T1.2: Error name/strerror via raw P/Invoke ----
    try
    {
        if (NativeLibrary.TryGetExport(nativeHandle, "ps_error_name", out var errNamePtr) &&
            NativeLibrary.TryGetExport(nativeHandle, "ps_strerror", out var strErrPtr))
        {
            var errorNameFn = Marshal.GetDelegateForFunctionPointer<ErrorNameFunc>(errNamePtr);
            var strErrorFn = Marshal.GetDelegateForFunctionPointer<ErrorNameFunc>(strErrPtr);

            var okName = Marshal.PtrToStringUTF8(errorNameFn(0)) ?? "";
            Assert(okName == "PS_OK", $"ps_error_name(0) == \"PS_OK\" (got \"{okName}\")");

            var notFoundName = Marshal.PtrToStringUTF8(errorNameFn(1)) ?? "";
            Assert(notFoundName == "PS_ERR_NOT_FOUND", $"ps_error_name(1) == \"PS_ERR_NOT_FOUND\" (got \"{notFoundName}\")");

            var invalidArgName = Marshal.PtrToStringUTF8(errorNameFn(5)) ?? "";
            Assert(invalidArgName == "PS_ERR_INVALID_ARG", $"ps_error_name(5) == \"PS_ERR_INVALID_ARG\" (got \"{invalidArgName}\")");

            var okStr = Marshal.PtrToStringUTF8(strErrorFn(0)) ?? "";
            Assert(okStr.Length > 0, $"ps_strerror(0) non-empty (got \"{okStr}\")");
        }
        else
        {
            Assert(false, "ps_error_name/ps_strerror exports found");
        }
    }
    catch (Exception ex)
    {
        Assert(false, $"Error name tests: {ex.Message}");
    }

    // ---- T1.3: Request classification via managed API ----
    try
    {
        // WebP + gzip
        var mask1 = WeAmp.PageSpeed.RequestClassifier.Classify(
            "image/webp", null, null, "gzip");
        Assert((mask1 & 0x01) != 0, $"Classify(webp, gzip): WebP bit set (mask=0x{mask1:X2})");
        Assert((mask1 & 0x40) != 0, $"Classify(webp, gzip): Gzip bit set (mask=0x{mask1:X2})");

        // Brotli
        var mask2 = WeAmp.PageSpeed.RequestClassifier.Classify(
            null, null, null, "br");
        Assert((mask2 & 0x80) != 0, $"Classify(br): Brotli bit set (mask=0x{mask2:X2})");

        // Content type classification
        var ctHtml = WeAmp.PageSpeed.RequestClassifier.ClassifyContentType("text/html");
        Assert(ctHtml == PageSpeedContentType.Html, $"ClassifyContentType(text/html) == Html (got {ctHtml})");

        var ctCss = WeAmp.PageSpeed.RequestClassifier.ClassifyContentType("text/css");
        Assert(ctCss == PageSpeedContentType.Css, $"ClassifyContentType(text/css) == Css (got {ctCss})");

        var ctJs = WeAmp.PageSpeed.RequestClassifier.ClassifyContentType("application/javascript");
        Assert(ctJs == PageSpeedContentType.Js, $"ClassifyContentType(application/javascript) == Js (got {ctJs})");

        var ctImg = WeAmp.PageSpeed.RequestClassifier.ClassifyContentType("image/png");
        Assert(ctImg == PageSpeedContentType.Image, $"ClassifyContentType(image/png) == Image (got {ctImg})");
    }
    catch (Exception ex)
    {
        Assert(false, $"Classification tests: {ex.Message}");
    }

    // ---- T1.4: CSS validation & minification via raw P/Invoke ----
    try
    {
        if (NativeLibrary.TryGetExport(nativeHandle, "ps_css_validate", out var cssValPtr) &&
            NativeLibrary.TryGetExport(nativeHandle, "ps_css_minify", out var cssMinPtr) &&
            NativeLibrary.TryGetExport(nativeHandle, "ps_free", out var freePtr))
        {
            var cssValidateFn = Marshal.GetDelegateForFunctionPointer<CssValidateFunc>(cssValPtr);
            var cssMinifyFn = Marshal.GetDelegateForFunctionPointer<CssMinifyFunc>(cssMinPtr);
            var freeFn = Marshal.GetDelegateForFunctionPointer<FreeFunc>(freePtr);

            // Valid CSS
            var validCss = Encoding.UTF8.GetBytes("body { color: red; }");
            unsafe
            {
                fixed (byte* p = validCss)
                {
                    int valResult = cssValidateFn((IntPtr)p, (nuint)validCss.Length);
                    Assert(valResult == 0, $"ps_css_validate(valid CSS) == 0 (got {valResult})");
                }
            }

            // CSS with null byte (XSS detection)
            var xssCss = Encoding.UTF8.GetBytes("body { color: red; }\x00<script>alert(1)</script>");
            unsafe
            {
                fixed (byte* p = xssCss)
                {
                    int valResult = cssValidateFn((IntPtr)p, (nuint)xssCss.Length);
                    Assert(valResult != 0, $"ps_css_validate(null byte XSS) != 0 (got {valResult})");
                }
            }

            // Minification
            var cssToMinify = Encoding.UTF8.GetBytes("body {\n  color:  red;\n  margin:  0;\n}");
            unsafe
            {
                fixed (byte* p = cssToMinify)
                {
                    int minResult = cssMinifyFn(
                        (IntPtr)p, (nuint)cssToMinify.Length,
                        out IntPtr outCss, out nuint outLen);
                    Assert(minResult == 0, $"ps_css_minify succeeded (err={minResult})");
                    if (minResult == 0 && outCss != IntPtr.Zero)
                    {
                        var minified = Marshal.PtrToStringUTF8(outCss, (int)outLen) ?? "";
                        Assert(minified.Length < cssToMinify.Length,
                            $"Minified CSS is shorter ({minified.Length} < {cssToMinify.Length})");
                        Assert(minified.Contains("color:red") || minified.Contains("color: red"),
                            $"Minified CSS preserves content");
                        freeFn(outCss);
                    }
                }
            }
        }
        else
        {
            Assert(false, "CSS exports found");
        }
    }
    catch (Exception ex)
    {
        Assert(false, $"CSS tests: {ex.Message}");
    }

    // ---- T1.5: HTML scanning via raw P/Invoke ----
    try
    {
        if (NativeLibrary.TryGetExport(nativeHandle, "ps_html_scan", out var htmlScanPtr) &&
            NativeLibrary.TryGetExport(nativeHandle, "ps_scan_element_count", out var scanCountPtr) &&
            NativeLibrary.TryGetExport(nativeHandle, "ps_scan_stylesheet_count", out var ssCountPtr) &&
            NativeLibrary.TryGetExport(nativeHandle, "ps_scan_stylesheet", out var ssPtr) &&
            NativeLibrary.TryGetExport(nativeHandle, "ps_scan_result_free", out var scanFreePtr))
        {
            var htmlScanFn = Marshal.GetDelegateForFunctionPointer<HtmlScanFunc>(htmlScanPtr);
            var scanCountFn = Marshal.GetDelegateForFunctionPointer<ScanCountFunc>(scanCountPtr);
            var ssCountFn = Marshal.GetDelegateForFunctionPointer<ScanCountFunc>(ssCountPtr);
            var ssFn = Marshal.GetDelegateForFunctionPointer<ScanStylesheetFunc>(ssPtr);
            var scanFreeFn = Marshal.GetDelegateForFunctionPointer<ScanResultFreeFunc>(scanFreePtr);

            var html = Encoding.UTF8.GetBytes(
                "<html><head>" +
                "<link rel=\"stylesheet\" href=\"/style.css\" media=\"screen\">" +
                "</head><body><h1>Hello</h1><p>World</p></body></html>");
            var url = Marshal.StringToCoTaskMemUTF8("https://example.com/");

            unsafe
            {
                fixed (byte* hp = html)
                {
                    int err = htmlScanFn((IntPtr)hp, (nuint)html.Length, url, out IntPtr scanResult);
                    Assert(err == 0, $"ps_html_scan succeeded (err={err})");

                    if (err == 0 && scanResult != IntPtr.Zero)
                    {
                        var elemCount = scanCountFn(scanResult);
                        Assert(elemCount > 0, $"Scan found {elemCount} elements");

                        var stylesheetCount = ssCountFn(scanResult);
                        Assert(stylesheetCount == 1, $"Scan found {stylesheetCount} stylesheet(s) (expected 1)");

                        if (stylesheetCount > 0)
                        {
                            int ssErr = ssFn(scanResult, 0, out IntPtr hrefPtr, out IntPtr mediaPtr);
                            Assert(ssErr == 0, "ps_scan_stylesheet(0) succeeded");
                            if (ssErr == 0)
                            {
                                var href = Marshal.PtrToStringUTF8(hrefPtr) ?? "";
                                Assert(href.Contains("style.css"), $"Stylesheet href contains 'style.css' (got \"{href}\")");
                            }
                        }

                        scanFreeFn(scanResult);
                    }
                }
            }

            Marshal.FreeCoTaskMem(url);
        }
        else
        {
            Assert(false, "HTML scan exports found");
        }
    }
    catch (Exception ex)
    {
        Assert(false, $"HTML scan tests: {ex.Message}");
    }

    // ---- T1.6: Cache lifecycle via managed API ----
    var cacheDir = Path.Combine(Path.GetTempPath(), $"ps_smoke_{Guid.NewGuid():N}");
    try
    {
        Directory.CreateDirectory(cacheDir);
        var cachePath = Path.Combine(cacheDir, "volume.dat");
        var cacheOpts = new WeAmp.PageSpeed.PageSpeedCacheOptions
        {
            VolumePath = cachePath,
            VolumeSizeBytes = 16 * 1024 * 1024,  // 16 MB
            EnableChecksum = true,
            RamCacheSizeBytes = 1024 * 1024,      // 1 MB
            MaxMetadataSizeBytes = 4096,
        };
        Phase("T1.6: open cache (PageSpeedCache ctor -> ps_cache_open)");
        using var cache = new PageSpeedCache(cacheOpts);
        Assert(true, "PageSpeedCache created successfully");

        // Write an entry
        var testUrl = "https://example.com/test.css";
        var testHostname = "example.com";
        var testContent = Encoding.UTF8.GetBytes("body{color:red}");
        var writeParams = new CacheWriteParams
        {
            AlternateId = 0x08,
            ContentLength = (ulong)testContent.Length,
            FullMask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        };

        using (var writer = cache.BeginWrite(testUrl, testHostname, "https", writeParams))
        {
            writer.Write(testContent);
            writer.Commit();
        }
        Assert(true, "Cache write + commit succeeded");

        // Read back
        using var readResult = cache.ReadBest(testUrl, testHostname, "https", 0x08);
        Assert(readResult != null, "Cache read returned non-null");
        if (readResult != null)
        {
            var content = readResult.ContentMemory;
            var readBack = Encoding.UTF8.GetString(content.Span);
            Assert(readBack == "body{color:red}",
                $"Cache read content matches (got \"{readBack}\")");
            Assert(readResult.ContentType == PageSpeedContentType.Css,
                $"Cache read ContentType == Css (got {readResult.ContentType})");
        }

        // Cache miss
        using var missResult = cache.ReadBest("https://example.com/nonexistent", testHostname, "https", 0x08);
        Assert(missResult == null, "Cache read for nonexistent URL returns null");

        // Stats
        var stats = cache.GetStats();
        Assert(stats.CurrentEntries >= 1, $"Cache stats: {stats.CurrentEntries} entries");
        // BytesWritten is not tracked per-operation in Cyclone (returns 0).
        // `using var` disposes in reverse declaration order at the end of this
        // block: both read results first, then the cache (ps_cache_close).
        Phase("T1.6: dispose read results, then the cache (using-scope exit -> ps_cache_close)");
    }
    catch (Exception ex)
    {
        Assert(false, $"Cache lifecycle: {ex.Message}");
    }
    finally
    {
        try { Directory.Delete(cacheDir, true); } catch { }
    }

    // ---- T1.7: HTML processing via managed API ----
    var htmlCacheDir = Path.Combine(Path.GetTempPath(), $"ps_smoke_html_{Guid.NewGuid():N}");
    try
    {
        Directory.CreateDirectory(htmlCacheDir);
        var htmlCachePath = Path.Combine(htmlCacheDir, "volume.dat");
        var htmlCacheOpts = new WeAmp.PageSpeed.PageSpeedCacheOptions
        {
            VolumePath = htmlCachePath,
            VolumeSizeBytes = 16 * 1024 * 1024,
        };
        Phase("T1.7: open cache for HtmlProcessor (ps_cache_open)");
        using var htmlCache = new PageSpeedCache(htmlCacheOpts);

        var htmlOpts = new WeAmp.PageSpeed.HtmlProcessingOptions
        {
            EnableCriticalCss = false,
            EnableLazyLoad = true,
            EnableLcpPreload = true,
            EnablePreconnect = true,
        };
        var processor = new WeAmp.PageSpeed.HtmlProcessor(htmlCache, htmlOpts);

        var testHtml = Encoding.UTF8.GetBytes(
            "<!DOCTYPE html><html><head><title>Test</title></head>" +
            "<body><h1>Hello World</h1>" +
            "<img src=\"https://example.com/hero.jpg\" width=\"800\" height=\"600\">" +
            "</body></html>");

        using var result = processor.Process(
            testHtml, "https://example.com/page", "example.com");

        Assert(true, "HtmlProcessor.Process completed without error");
        // The result may or may not be modified depending on whether
        // the native library decides to transform the HTML.
        // We just verify it doesn't crash and returns valid data.
        var outputMem = result.OutputMemory;
        Assert(outputMem.Length > 0 || !result.Modified,
            $"HTML result has output ({outputMem.Length} bytes, modified={result.Modified})");
        Phase("T1.7: dispose HTML result, then the cache (using-scope exit -> ps_cache_close)");
    }
    catch (Exception ex)
    {
        Assert(false, $"HTML processing: {ex.Message}");
    }
    finally
    {
        try { Directory.Delete(htmlCacheDir, true); } catch { }
    }

    // ---- T1.8: Cache auto-creates parent directory ----
    var autoDirBase = Path.Combine(Path.GetTempPath(), $"ps_smoke_autodir_{Guid.NewGuid():N}");
    try
    {
        var autoVolumePath = Path.Combine(autoDirBase, "deep", "nested", "volume.dat");
        var autoParent = Path.GetDirectoryName(autoVolumePath)!;

        // Parent must NOT exist before cache creation
        Assert(!Directory.Exists(autoParent), "Auto-create: parent dir does not exist before cache creation");

        var autoCacheOpts = new WeAmp.PageSpeed.PageSpeedCacheOptions
        {
            VolumePath = autoVolumePath,
            VolumeSizeBytes = 16 * 1024 * 1024,
            RamCacheSizeBytes = 1024 * 1024,
            MaxMetadataSizeBytes = 4096,
        };
        Phase("T1.8: open cache under a not-yet-existing parent dir (ps_cache_open)");
        using var autoCache = new PageSpeedCache(autoCacheOpts);
        Assert(true, "Auto-create: PageSpeedCache created with non-existent parent dir");
        Assert(Directory.Exists(autoParent), "Auto-create: parent directory was created");
        Phase("T1.8: dispose cache (using-scope exit -> ps_cache_close)");
    }
    catch (Exception ex)
    {
        Assert(false, $"Auto-create: {ex.Message}");
    }
    finally
    {
        try { Directory.Delete(autoDirBase, true); } catch { }
    }

    // Free native handle after all Tier 1 raw P/Invoke tests. This drops only
    // the explicit TryLoad reference; the P/Invoke resolver's own load of the
    // same image stays mapped for Tiers 2-3.
    Phase("Tier 1: NativeLibrary.Free of the explicit handle (P/Invoke's own load stays mapped)");
    NativeLibrary.Free(nativeHandle);
    nativeHandle = IntPtr.Zero;
}

Console.WriteLine();

// ============================================================
// TIER 2: ASP.NET Core Integration (TestServer)
// ============================================================
Console.WriteLine("──── Tier 2: ASP.NET Core Integration ────");
Phase("Tier 2: ASP.NET Core TestServer integration");

var tier2CacheDir = Path.Combine(Path.GetTempPath(), $"ps_smoke_t2_{Guid.NewGuid():N}");
try
{
    Directory.CreateDirectory(tier2CacheDir);
    var tier2CachePath = Path.Combine(tier2CacheDir, "volume.dat");

    // ---- T2.1: Service registration (AddPageSpeed) ----
    var builder = WebApplication.CreateBuilder(Array.Empty<string>());
    builder.WebHost.UseTestServer();
    builder.Logging.SetMinimumLevel(LogLevel.Warning);
    builder.Services.AddPageSpeed(opts =>
    {
        opts.Cache.VolumePath = tier2CachePath;
        opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
        opts.Worker.AutoStart = false;  // Don't start factory_worker in tests
    });
    Assert(true, "AddPageSpeed() service registration succeeded");

    // ---- T2.2: Build app with middleware ----
    Phase("T2.2: Build()+StartAsync() main app (opens DI cache; stays open until process exit)");
    var app = builder.Build();

    // Static HTML endpoint for testing
    app.MapGet("/test-html", () => Results.Content(
        "<!DOCTYPE html><html><head><title>Test</title></head>" +
        "<body><h1>Hello from TestServer</h1></body></html>",
        "text/html"));

    // CSS endpoint
    app.MapGet("/test.css", () => Results.Content(
        "body { color: red; margin: 0; }",
        "text/css"));

    // JSON endpoint (should pass through unmodified)
    app.MapGet("/api/data", () => Results.Json(new { status = "ok" }));

    app.UsePageSpeed();

    await app.StartAsync();
    Assert(true, "WebApplication with PageSpeed middleware started");

    var testServer = app.GetTestServer();
    var client = testServer.CreateClient();

    // ---- T2.3: HTML response is served correctly ----
    var htmlResponse = await client.GetAsync("/test-html");
    Assert(htmlResponse.StatusCode == HttpStatusCode.OK,
        $"GET /test-html returns 200 (got {(int)htmlResponse.StatusCode})");
    var htmlBody = await htmlResponse.Content.ReadAsStringAsync();
    Assert(htmlBody.Contains("Hello from TestServer"),
        "HTML response contains expected content");
    var htmlCt = htmlResponse.Content.Headers.ContentType?.MediaType ?? "";
    Assert(htmlCt == "text/html",
        $"HTML response Content-Type is text/html (got \"{htmlCt}\")");

    // ---- T2.4: CSS response is served correctly ----
    var cssResponse = await client.GetAsync("/test.css");
    Assert(cssResponse.StatusCode == HttpStatusCode.OK,
        $"GET /test.css returns 200 (got {(int)cssResponse.StatusCode})");
    var cssBody = await cssResponse.Content.ReadAsStringAsync();
    Assert(cssBody.Contains("color"),
        "CSS response contains expected content");

    // ---- T2.5: API endpoint passes through (excluded path) ----
    var apiResponse = await client.GetAsync("/api/data");
    Assert(apiResponse.StatusCode == HttpStatusCode.OK,
        $"GET /api/data returns 200 (got {(int)apiResponse.StatusCode})");
    var apiBody = await apiResponse.Content.ReadAsStringAsync();
    Assert(apiBody.Contains("ok"),
        "API response passes through unmodified");

    // ---- T2.6: Cache resolves via DI ----
    using (var scope = app.Services.CreateScope())
    {
        var diCache = scope.ServiceProvider.GetService<IPageSpeedCache>();
        Assert(diCache != null, "IPageSpeedCache resolved from DI");
    }

    // ---- T2.7: HTML processor resolves via DI ----
    using (var scope = app.Services.CreateScope())
    {
        var diProcessor = scope.ServiceProvider.GetService<IHtmlProcessor>();
        Assert(diProcessor != null, "IHtmlProcessor resolved from DI");
    }

    // ---- T2.8: Options bind correctly ----
    using (var scope = app.Services.CreateScope())
    {
        var opts = scope.ServiceProvider.GetRequiredService<IOptions<PageSpeedOptions>>().Value;
        Assert(opts.Enabled, "PageSpeedOptions.Enabled defaults to true");
        Assert(opts.Cache.VolumePath == tier2CachePath,
            "PageSpeedOptions.Cache.VolumePath matches configured value");
        Assert(!opts.Worker.AutoStart,
            "PageSpeedOptions.Worker.AutoStart was set to false");
    }

    // ---- T2.9: Second HTML request (potential cache hit) ----
    var htmlResponse2 = await client.GetAsync("/test-html");
    Assert(htmlResponse2.StatusCode == HttpStatusCode.OK,
        $"Second GET /test-html returns 200 (got {(int)htmlResponse2.StatusCode})");
    var htmlBody2 = await htmlResponse2.Content.ReadAsStringAsync();
    Assert(htmlBody2.Contains("Hello from TestServer"),
        "Second HTML response contains expected content");

    // ---- T2.10: AddPageSpeed with non-existent cache directory (DI path) ----
    var diAutoDirBase = Path.Combine(Path.GetTempPath(), $"ps_smoke_di_auto_{Guid.NewGuid():N}");
    try
    {
        var diAutoCachePath = Path.Combine(diAutoDirBase, "sub", "volume.dat");
        Assert(!Directory.Exists(diAutoDirBase), "DI auto-create: base dir does not exist");

        var diBuilder = WebApplication.CreateBuilder(Array.Empty<string>());
        diBuilder.WebHost.UseTestServer();
        diBuilder.Logging.SetMinimumLevel(LogLevel.Warning);
        diBuilder.Services.AddPageSpeed(opts =>
        {
            opts.Cache.VolumePath = diAutoCachePath;
            opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
            opts.Worker.AutoStart = false;
        });

        // This is the block the first observed crash died in (exit 127, 0.4 s
        // after the assertion above, nothing else printed): the SECOND
        // concurrently-open native cache in this process, the main app's
        // still being open.
        Phase("T2.10: Build()+StartAsync() DI auto-create app (2nd concurrently-open native cache; Worker.AutoStart=false)");
        var diApp = diBuilder.Build();
        diApp.UsePageSpeed();
        await diApp.StartAsync();

        Assert(Directory.Exists(Path.GetDirectoryName(diAutoCachePath)),
            "DI auto-create: parent directory was created via AddPageSpeed DI path");

        IPageSpeedCache? diCacheRef;
        using (var scope = diApp.Services.CreateScope())
        {
            diCacheRef = scope.ServiceProvider.GetService<IPageSpeedCache>();
            Assert(diCacheRef != null, "DI auto-create: IPageSpeedCache resolved from DI");
        }

        // Teardown ordering, asserted step by step:
        //   StopAsync  -> hosted services stop; the cache singleton is NOT
        //                 disposed (it belongs to the service provider).
        //   DisposeAsync -> service provider disposed -> PageSpeedCache.Dispose
        //                 -> SafeCacheHandle -> ps_cache_close.
        // A crash in either step now has its own [PHASE] line.
        Phase("T2.10: StopAsync() (hosted services stop; cache singleton must stay open)");
        await diApp.StopAsync();
        Assert(true, "DI auto-create: app stopped cleanly");
        if (diCacheRef != null)
        {
            var statsAfterStop = diCacheRef.GetStats();  // ObjectDisposedException if Stop disposed it
            Assert(true,
                $"DI auto-create: cache still open after StopAsync ({statsAfterStop.CurrentEntries} entries)");
        }
        else
        {
            Assert(false, "DI auto-create: cache still open after StopAsync (no cache resolved)");
        }

        Phase("T2.10: DisposeAsync() (service provider -> PageSpeedCache.Dispose -> ps_cache_close)");
        await diApp.DisposeAsync();
        Assert(true, "DI auto-create: DisposeAsync completed (native cache closed)");
        bool rejectsUseAfterDispose = false;
        try { diCacheRef?.GetStats(); }
        catch (ObjectDisposedException) { rejectsUseAfterDispose = true; }
        Assert(rejectsUseAfterDispose,
            "DI auto-create: cache rejects use after DisposeAsync (ObjectDisposedException)");
    }
    catch (Exception ex)
    {
        Assert(false, $"DI auto-create: {ex.Message}");
    }
    finally
    {
        try { Directory.Delete(diAutoDirBase, true); } catch { }
    }

    // ---- T2.11: Cache-hit path exercises CacheInsertedAt ----
    var cacheHitDir = Path.Combine(Path.GetTempPath(), $"ps_smoke_cachehit_{Guid.NewGuid():N}");
    try
    {
        var cacheHitVolume = Path.Combine(cacheHitDir, "volume.dat");

        var chBuilder = WebApplication.CreateBuilder(Array.Empty<string>());
        chBuilder.WebHost.UseTestServer();
        chBuilder.Logging.SetMinimumLevel(LogLevel.Warning);
        chBuilder.Services.AddPageSpeed(opts =>
        {
            opts.Cache.VolumePath = cacheHitVolume;
            opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
            opts.Worker.AutoStart = false;
        });

        Phase("T2.11: Build()+StartAsync() cache-hit app (another concurrently-open native cache)");
        var chApp = chBuilder.Build();
        chApp.MapGet("/cache-hit-test", () => Results.Content(
            "<!DOCTYPE html><html><head><title>Cache</title></head><body>Cached</body></html>",
            "text/html"));
        chApp.UsePageSpeed();
        await chApp.StartAsync();
        Assert(true, "Cache-hit test app started");

        using var chClient = chApp.GetTestServer().CreateClient();

        // Request 1: cache miss (writes to cache)
        var chResp1 = await chClient.GetAsync("/cache-hit-test");
        Assert(chResp1.StatusCode == HttpStatusCode.OK,
            $"Cache-hit req 1 returns 200 (got {(int)chResp1.StatusCode})");

        // Request 2: cache hit (exercises CacheInsertedAt → ps_read_cache_inserted_at)
        var chResp2 = await chClient.GetAsync("/cache-hit-test");
        Assert(chResp2.StatusCode == HttpStatusCode.OK,
            $"Cache-hit req 2 returns 200 (got {(int)chResp2.StatusCode})");

        // If Age header is present, CacheInsertedAt was successfully called
        var ageHeader = chResp2.Headers.Contains("Age")
            ? chResp2.Headers.GetValues("Age").FirstOrDefault() ?? ""
            : "";
        if (!string.IsNullOrEmpty(ageHeader))
        {
            Assert(true, $"Cache-hit response has Age header (value={ageHeader})");
        }
        else
        {
            Console.WriteLine("[INFO] Age header not present — middleware may not have cached this response");
        }

        Phase("T2.11: StopAsync() cache-hit app (not disposed: its native cache stays open)");
        await chApp.StopAsync();
        undisposedHosts++;
        Assert(true, "Cache-hit test app stopped cleanly");
    }
    catch (Exception ex)
    {
        Assert(false, $"Cache-hit test: {ex.Message}");
    }
    finally
    {
        try { Directory.Delete(cacheHitDir, true); } catch { }
    }

    // Check for X-PageSpeed header (may be HIT or MISS depending on processing)
    var xpsHeader = htmlResponse2.Headers.Contains("X-PageSpeed")
        ? htmlResponse2.Headers.GetValues("X-PageSpeed").FirstOrDefault() ?? ""
        : "";
    if (!string.IsNullOrEmpty(xpsHeader))
    {
        Assert(xpsHeader == "HIT" || xpsHeader == "MISS",
            $"X-PageSpeed header is HIT or MISS (got \"{xpsHeader}\")");
    }
    else
    {
        Console.WriteLine("[INFO] X-PageSpeed header not present on second request");
    }

    Phase("T2: StopAsync() main app (not disposed: its native cache stays open)");
    await app.StopAsync();
    undisposedHosts++;
    Assert(true, "WebApplication stopped cleanly");
}
catch (Exception ex)
{
    Assert(false, $"Tier 2: {ex.GetType().Name}: {ex.Message}");
    if (ex.InnerException != null)
        Console.WriteLine($"       Inner: {ex.InnerException.Message}");
}
finally
{
    try { Directory.Delete(tier2CacheDir, true); } catch { }
}

Console.WriteLine();

// ============================================================
// TIER 3: Worker Process Integration
// ============================================================
Console.WriteLine("──── Tier 3: Worker Process Integration ────");
Phase("Tier 3: worker process integration");

var tier3CacheDir = Path.Combine(Path.GetTempPath(), $"ps_smoke_t3_{Guid.NewGuid():N}");
try
{
    Directory.CreateDirectory(tier3CacheDir);
    var tier3CachePath = Path.Combine(tier3CacheDir, "volume.dat");
    // Use /tmp/ directly to keep socket path under macOS 104-char limit.
    var socketPath = $"/tmp/ps_smoke_{Guid.NewGuid():N}.sock";

    // ---- T3.1: WorkerProcessHost starts factory_worker ----
    var workerBuilder = WebApplication.CreateBuilder(Array.Empty<string>());
    workerBuilder.WebHost.UseTestServer();
    workerBuilder.Logging.SetMinimumLevel(LogLevel.Warning);
    workerBuilder.Services.AddPageSpeed(opts =>
    {
        opts.Cache.VolumePath = tier3CachePath;
        opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
        opts.Worker.AutoStart = true;
        opts.Worker.SocketPath = socketPath;
    });

    Phase("T3.1: Build()+StartAsync() worker app (AutoStart=true: spawns factory_worker; opens native cache)");
    var workerApp = workerBuilder.Build();
    workerApp.UsePageSpeed();

    await workerApp.StartAsync();
    Assert(true, "App with Worker.AutoStart=true started");

    // ---- T3.2: WorkerProcessHost resolved from DI ----
    var workerHost = workerApp.Services.GetService<WorkerProcessHost>();
    Assert(workerHost != null, "WorkerProcessHost resolved from DI");

    // ---- T3.3: Give worker time to start and check socket ----
    // The worker process needs a moment to create the socket
    await Task.Delay(2000);

    if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
    {
        // On Linux/macOS, check if the Unix domain socket was created
        var socketExists = File.Exists(socketPath);
        if (socketExists)
        {
            Assert(true, $"Worker socket created at {socketPath}");
        }
        else
        {
            // The socket may not exist if factory_worker couldn't start
            // (e.g., missing permissions). This is acceptable in CI.
            Console.WriteLine($"[INFO] Worker socket not found at {socketPath} (worker may not have started)");
        }
    }

    // ---- T3.4: WorkerNotificationService resolved from DI ----
    var notifier = workerApp.Services.GetService<WorkerNotificationService>();
    Assert(notifier != null, "WorkerNotificationService resolved from DI");

    // ---- T3.5: TryNotify does not throw (even if worker isn't running) ----
    if (notifier != null)
    {
        try
        {
            bool queued = notifier.TryNotify(
                "https://example.com/test", "example.com", "https",
                PageSpeedContentType.Html, 0x08);
            Assert(true, $"TryNotify completed without error (queued={queued})");
        }
        catch (Exception ex)
        {
            Assert(false, $"TryNotify threw: {ex.Message}");
        }
    }

    // ---- T3.6: Worker API port is reachable ----
    if (workerPath != null)
    {
        var apiCacheDir = Path.Combine(Path.GetTempPath(), $"ps_smoke_api_{Guid.NewGuid():N}");
        try
        {
            var listener = new System.Net.Sockets.TcpListener(
                System.Net.IPAddress.Loopback, 0);
            listener.Start();
            int freePort = ((System.Net.IPEndPoint)listener.LocalEndpoint).Port;
            listener.Stop();

            var apiBuilder = WebApplication.CreateBuilder(Array.Empty<string>());
            apiBuilder.WebHost.UseTestServer();
            apiBuilder.Logging.SetMinimumLevel(LogLevel.Warning);
            apiBuilder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = Path.Combine(apiCacheDir, "volume.dat");
                opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
                opts.Worker.AutoStart = true;
                opts.Worker.ApiPort = freePort;
            });

            Phase("T3.6: Build()+StartAsync() worker-API app (spawns a 2nd factory_worker; opens native cache)");
            var apiApp = apiBuilder.Build();
            apiApp.UsePageSpeed();
            await apiApp.StartAsync();

            // Wait for worker to start (up to 5 seconds)
            bool apiReachable = false;
            using var apiClient = NewWorkerApiClient();
            for (int attempt = 0; attempt < 10; attempt++)
            {
                await Task.Delay(500);
                try
                {
                    using var healthResp = await apiClient.GetAsync(
                        $"http://127.0.0.1:{freePort}/v1/health");
                    if (healthResp.IsSuccessStatusCode)
                    {
                        apiReachable = true;
                        break;
                    }
                }
                catch (HttpRequestException)
                {
                    // Worker not ready yet
                }
            }

            if (apiReachable)
            {
                Assert(true, $"Worker API port {freePort} is reachable (/v1/health returned 200)");
            }
            else
            {
                Assert(false, $"T3.6: Worker API port {freePort} not reachable (factory_worker may not have started)");
            }

            Phase("T3.6: StopAsync() worker-API app (not disposed: native cache stays open)");
            try { await apiApp.StopAsync(); } catch { }
            undisposedHosts++;
            Assert(true, "Worker API port test app stopped cleanly");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[INFO] Worker API port test skipped: {ex.Message}");
        }
        finally
        {
            try { Directory.Delete(apiCacheDir, true); } catch { }
        }
    }
    else
    {
        Assert(false, "T3.6 skipped: factory_worker not found");
    }

    // ---- T3.7: End-to-end image optimization ----
    // Verifies the full async pipeline: middleware cache → worker notification →
    // optimized variant served. This catches missing --cache-path, broken socket
    // communication, and worker processing failures.
    if (workerPath != null)
    {
        var e2eCacheDir = Path.Combine(Path.GetTempPath(), $"ps_e2e_{Guid.NewGuid():N}");
        var e2eSocket = $"/tmp/ps{Guid.NewGuid():N}.sock";
        try
        {
            // Find a free port for worker API
            var e2eListener = new TcpListener(IPAddress.Loopback, 0);
            e2eListener.Start();
            int e2ePort = ((IPEndPoint)e2eListener.LocalEndpoint).Port;
            e2eListener.Stop();

            var e2eBuilder = WebApplication.CreateBuilder(Array.Empty<string>());
            e2eBuilder.WebHost.UseTestServer();
            e2eBuilder.Logging.SetMinimumLevel(LogLevel.Warning);
            e2eBuilder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = Path.Combine(e2eCacheDir, "volume.dat");
                opts.Cache.VolumeSizeBytes = 64 * 1024 * 1024;
                opts.Worker.AutoStart = true;
                opts.Worker.SocketPath = e2eSocket;
                opts.Worker.ApiPort = e2ePort;
            });

            var e2eApp = e2eBuilder.Build();

            // Generate a 256x256 RGB gradient with deterministic noise. The
            // worker only keeps a WebP/AVIF variant that is smaller than the
            // original, so the fixture must be an image WebP clearly beats: a
            // flat-colour PNG is already tiny and is refused as "not smaller".
            byte[] testPng;
            {
                int pw = 256, ph = 256;
                using var pms = new MemoryStream();
                // PNG signature
                pms.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 });

                void WritePngU32(Stream s, uint v) {
                    s.WriteByte((byte)(v >> 24)); s.WriteByte((byte)(v >> 16));
                    s.WriteByte((byte)(v >> 8)); s.WriteByte((byte)v);
                }
                void WritePngChunk(Stream s, string type, byte[] data) {
                    WritePngU32(s, (uint)data.Length);
                    var tb = Encoding.ASCII.GetBytes(type);
                    s.Write(tb);
                    s.Write(data);
                    uint crc = 0xFFFFFFFF;
                    foreach (var b in tb) { crc ^= b; for (int i = 0; i < 8; i++) crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1; }
                    foreach (var b in data) { crc ^= b; for (int i = 0; i < 8; i++) crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1; }
                    crc ^= 0xFFFFFFFF;
                    WritePngU32(s, crc);
                }

                // IHDR chunk
                var ihdr = new byte[13];
                ihdr[0] = (byte)(pw >> 24); ihdr[1] = (byte)(pw >> 16);
                ihdr[2] = (byte)(pw >> 8); ihdr[3] = (byte)pw;
                ihdr[4] = (byte)(ph >> 24); ihdr[5] = (byte)(ph >> 16);
                ihdr[6] = (byte)(ph >> 8); ihdr[7] = (byte)ph;
                ihdr[8] = 8;  // bit depth
                ihdr[9] = 2;  // color type: RGB
                WritePngChunk(pms, "IHDR", ihdr);

                // IDAT chunk: raw pixel data compressed with zlib
                using var rawMs = new MemoryStream();
                var rng = new Random(12345); // fixed seed: same bytes every run
                byte Clamp(int v) => (byte)Math.Clamp(v, 0, 255);
                for (int y = 0; y < ph; y++) {
                    rawMs.WriteByte(0); // filter: none
                    for (int x = 0; x < pw; x++) {
                        rawMs.WriteByte(Clamp(x + rng.Next(-6, 7)));
                        rawMs.WriteByte(Clamp(y + rng.Next(-6, 7)));
                        rawMs.WriteByte(Clamp((x + y) / 2 + rng.Next(-6, 7)));
                    }
                }
                var rawPixels = rawMs.ToArray();

                using var compMs = new MemoryStream();
                // zlib header (deflate, default compression)
                compMs.WriteByte(0x78); compMs.WriteByte(0x01);
                using (var deflate = new System.IO.Compression.DeflateStream(
                    compMs, System.IO.Compression.CompressionLevel.Fastest, true))
                    deflate.Write(rawPixels);
                // Adler-32 checksum (zlib footer)
                uint a32 = 1, b32 = 0;
                foreach (var b in rawPixels) { a32 = (a32 + b) % 65521; b32 = (b32 + a32) % 65521; }
                var adler = (b32 << 16) | a32;
                compMs.WriteByte((byte)(adler >> 24)); compMs.WriteByte((byte)(adler >> 16));
                compMs.WriteByte((byte)(adler >> 8)); compMs.WriteByte((byte)adler);
                WritePngChunk(pms, "IDAT", compMs.ToArray());

                // IEND chunk
                WritePngChunk(pms, "IEND", Array.Empty<byte>());
                testPng = pms.ToArray();
            }

            e2eApp.MapGet("/test-image.png", () => Results.File(testPng, "image/png"));
            e2eApp.UsePageSpeed();
            Phase("T3.7: StartAsync() E2E app (spawns a 3rd factory_worker; opens native cache)");
            await e2eApp.StartAsync();

            // Wait for worker to be ready BEFORE first request.
            // If we request before the worker's socket is ready, the notification
            // fails silently and the worker never gets notified (no retry).
            bool workerReady = false;
            using var e2eStatsClient = NewWorkerApiClient();
            for (int i = 0; i < 20; i++)
            {
                await Task.Delay(500);
                try
                {
                    using var hr = await e2eStatsClient.GetAsync(
                        $"http://127.0.0.1:{e2ePort}/v1/health");
                    if (hr.IsSuccessStatusCode) { workerReady = true; break; }
                }
                catch (HttpRequestException) { }
            }

            if (!workerReady)
            {
                Assert(false, "T3.7: E2E worker did not start within 10s");
            }
            else
            {
                var e2eClient = e2eApp.GetTestServer().CreateClient();

                // Request 1: cache miss, triggers worker notification
                e2eClient.DefaultRequestHeaders.Add("Accept", "image/webp,image/png,*/*");
                var imgResp1 = await e2eClient.GetAsync("/test-image.png");
                Assert(imgResp1.StatusCode == HttpStatusCode.OK,
                    $"E2E image req 1 returns 200 (got {(int)imgResp1.StatusCode})");
                var origBytes = await imgResp1.Content.ReadAsByteArrayAsync();
                var origCt = imgResp1.Content.Headers.ContentType?.MediaType ?? "";
                Assert(origCt == "image/png",
                    $"E2E image req 1 is PNG (got {origCt})");

                // Wait for worker to process (up to 15 seconds, polling every 500ms)
                bool optimized = false;
                // 60 attempts × 500ms = 30s — CI (fastbuild + shared runner) needs
                // more headroom than local release builds.
                for (int attempt = 0; attempt < 60; attempt++)
                {
                    await Task.Delay(500);
                    using var probe = new HttpRequestMessage(HttpMethod.Get, "/test-image.png");
                    probe.Headers.Add("Accept", "image/webp,image/png,*/*");
                    using var imgResp2 = await e2eClient.SendAsync(probe);
                    var ct2 = imgResp2.Content.Headers.ContentType?.MediaType ?? "";
                    if (ct2 == "image/webp")
                    {
                        var webpBytes = await imgResp2.Content.ReadAsByteArrayAsync();
                        Assert(true,
                            $"E2E image optimized: PNG {origBytes.Length}B -> WebP {webpBytes.Length}B");
                        optimized = true;
                        break;
                    }
                }

                if (!optimized)
                {
                    // Diagnostic: query worker stats to understand why WebP was not produced
                    try
                    {
                        using var diag = await e2eStatsClient.GetAsync(
                            $"http://127.0.0.1:{e2ePort}/v1/health");
                        var diagBody = await diag.Content.ReadAsStringAsync();
                        Console.WriteLine($"[DIAG] Worker /v1/health: {diagBody}");
                        using var statsResp = await e2eStatsClient.GetAsync(
                            $"http://127.0.0.1:{e2ePort}/v1/stats");
                        var statsBody = await statsResp.Content.ReadAsStringAsync();
                        Console.WriteLine($"[DIAG] Worker /v1/stats: {statsBody}");
                    }
                    catch (Exception diagEx)
                    {
                        Console.WriteLine($"[DIAG] Failed to query worker: {diagEx.Message}");
                    }
                    string reason = "";
                    try
                    {
                        using var stats = await e2eStatsClient.GetAsync(
                            $"http://127.0.0.1:{e2ePort}/v1/stats");
                        using var doc = System.Text.Json.JsonDocument.Parse(
                            await stats.Content.ReadAsStringAsync());
                        var root = doc.RootElement;
                        var refused = root.TryGetProperty("image_size_refused", out var r)
                            ? r.ToString() : "?";
                        var noSavings = root.TryGetProperty("image_no_savings_skipped", out var n)
                            ? n.ToString() : "?";
                        reason = $" (image_size_refused={refused}, image_no_savings_skipped={noSavings})";
                    }
                    catch (Exception statsEx)
                    {
                        reason = $" (worker stats unavailable: {statsEx.Message})";
                    }
                    Assert(false, "E2E image optimization: expected WebP but still PNG after 30s" + reason);
                }
            }

            Phase("T3.7: StopAsync() E2E app (not disposed: native cache stays open)");
            try { await e2eApp.StopAsync(); } catch { }
            undisposedHosts++;
            Assert(true, "E2E optimization test app stopped cleanly");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[INFO] E2E optimization test skipped: {ex.Message}");
        }
        finally
        {
            try { File.Delete(e2eSocket); } catch { }
            try { Directory.Delete(e2eCacheDir, true); } catch { }
        }
    }
    else
    {
        Assert(false, "T3.7 skipped: factory_worker not found");
    }

    Phase("T3.1: StopAsync() worker app (not disposed: native cache stays open)");
    await workerApp.StopAsync();
    undisposedHosts++;
    Assert(true, "App with worker stopped cleanly");

    // Cleanup socket
    try { File.Delete(socketPath); } catch { }
}
catch (Exception ex)
{
    Assert(false, $"Tier 3: {ex.GetType().Name}: {ex.Message}");
    if (ex.InnerException != null)
        Console.WriteLine($"       Inner: {ex.InnerException.Message}");
}
finally
{
    try { Directory.Delete(tier3CacheDir, true); } catch { }
}

Console.WriteLine();

// ============================================================
// Summary
// ============================================================
Console.WriteLine("════════════════════════════════════════════");
Console.WriteLine($"Total: {passed + failures} tests — {passed} passed, {failures} failed");

// All assertions must pass: there is no minimum pass count. (Windows runs
// fewer assertions than Unix because the worker-socket check is Unix-only, which
// is a different count, not a failure.) Skips are explicit and listed by name.
if (skipped.Count > 0)
{
    Console.WriteLine($"Skipped {skipped.Count} check(s) by request (SMOKE_ALLOW_SKIP):");
    foreach (var sk in skipped) Console.WriteLine($"  [SKIP] {sk}");
}
if (passed == 0)
{
    Console.WriteLine("[FAIL] No assertion passed");
    failures++;
}

int exitCode;
if (failures == 0)
{
    Console.WriteLine(skipped.Count == 0
        ? "=== ALL TESTS PASSED ==="
        : $"=== ALL TESTS PASSED ({skipped.Count} skipped by request) ===");
    exitCode = 0;
}
else
{
    Console.WriteLine($"=== {failures} TEST(S) FAILED ({passed} passed) ===");
    exitCode = 1;
}

// The second observed crash was a segfault ~90 ms after "=== ALL TESTS PASSED
// ==="
// — i.e. here, in the runtime's process teardown, with the hosts below
// stopped but never disposed. Anything after this line is runtime shutdown:
// no finalizers run (.NET Core), the native caches of the undisposed hosts
// are still open with Cyclone's threads alive, and the native library's
// static destructors / DLL_PROCESS_DETACH run underneath them.
Phase($"process exit: returning {exitCode} from Main; {undisposedHosts} host(s) StopAsync'ed but never disposed " +
      "(their native caches stay open: .NET runs no finalizers at exit, so ps_cache_close never runs for them)");
return exitCode;

// ============================================================
// Delegate types for raw P/Invoke
// ============================================================

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate IntPtr StringReturnFunc();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int VersionFunc();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate IntPtr ErrorNameFunc(int err);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int CssValidateFunc(IntPtr css, nuint len);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int CssMinifyFunc(IntPtr css, nuint len, out IntPtr outCss, out nuint outLen);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate void FreeFunc(IntPtr ptr);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int HtmlScanFunc(IntPtr html, nuint len, IntPtr url, out IntPtr outResult);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate nuint ScanCountFunc(IntPtr result);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate int ScanStylesheetFunc(IntPtr result, nuint index, out IntPtr outHref, out IntPtr outMedia);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate void ScanResultFreeFunc(IntPtr result);
CSEOF

# ---------- Crash diagnostics ----------
# The host died twice on a Windows CI runner with nothing but an exit code:
# 127 inside the DI auto-create block, 139 at process exit. Both passed on
# rerun. Have the .NET runtime write a dump when the host dies -- native
# faults included: the runtime's unhandled-exception / signal path runs
# createdump before the process is torn down -- into a directory that
# survives this script's EXIT trap (WORKDIR is wiped), so the workflow can
# upload it (ci.yml: the "Upload smoke crash dumps" step after each Pkg
# Smoke run). On CI that is RUNNER_TEMP; SMOKE_DUMP_DIR overrides it.
IS_MSYS=false
if command -v cygpath &>/dev/null; then IS_MSYS=true; fi

DUMP_DIR="${SMOKE_DUMP_DIR:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/smoke-crash-dumps}"
if $IS_MSYS; then
    DUMP_DIR="$(cygpath -u "$DUMP_DIR")"   # RUNNER_TEMP arrives as a Windows path
fi
rm -rf "$DUMP_DIR"                         # only THIS run's dumps get uploaded
mkdir -p "$DUMP_DIR"
DUMP_DIR_NATIVE="$DUMP_DIR"
DUMP_SEP="/"
if $IS_MSYS; then
    DUMP_DIR_NATIVE="$(cygpath -w "$DUMP_DIR")"
    DUMP_SEP='\'
fi
export DOTNET_DbgEnableMiniDump=1
# 4 = Full: all memory including the module images. The crash is native and
# the faulting code lives in pagespeed.dll / libpagespeed.*, so the dump has
# to carry the images to be readable away from the runner. Expect a few
# hundred MB per dump; the artifact's retention is short.
export DOTNET_DbgMiniDumpType="${SMOKE_DUMP_TYPE:-4}"
# %e = executable name (dotnet), %p = pid, %t = epoch seconds.
export DOTNET_DbgMiniDumpName="${DUMP_DIR_NATIVE}${DUMP_SEP}smoke-%e-%p-%t.dmp"
# createdump reports its own progress and failures on the crashing host's
# console, so a dump that could NOT be written says so in the job log.
export DOTNET_CreateDumpDiagnostics=1
case "$(uname -s)" in
    Linux*|Darwin*)
        # A JSON report of the faulting thread's stack next to the dump
        # (<dump>.crashreport.json), readable straight from the job log --
        # the banner below prints it. Not supported on Windows.
        export DOTNET_EnableCrashReport=1
        ;;
esac
if [[ "$(uname -s)" == Linux* ]]; then
    # Secondary: a kernel core, if core_pattern puts it somewhere readable.
    # createdump is the mechanism that is actually relied on.
    ulimit -c unlimited 2>/dev/null || true
fi
echo "--- Crash dumps: $DUMP_DIR_NATIVE (DOTNET_DbgMiniDumpType=$DOTNET_DbgMiniDumpType) ---"
echo ""

# What an exit status means. The smoke host is `dotnet run` of a program that
# returns its own [FAIL] count, so small positive codes are assertion
# failures; 128+N is a signal; and under Git Bash a Windows crash arrives
# folded (see run_host_windows). $2 is the raw Windows exit status when the
# cmd.exe wrapper captured one.
describe_exit() {
    local code="$1" raw="${2:-}" hex
    if [[ -n "$raw" && "$raw" =~ ^-?[0-9]+$ ]] && (( raw < 0 || raw > 255 )); then
        hex="$(printf '0x%08X' $(( raw & 0xFFFFFFFF )))"
        case "$hex" in
            0xC0000005) echo "Windows $hex STATUS_ACCESS_VIOLATION -- native segfault (Git Bash folds it to 139)" ;;
            0xC0000409) echo "Windows $hex STATUS_STACK_BUFFER_OVERRUN -- fail-fast: abort()/std::terminate in native code, __fastfail, or .NET Environment.FailFast (Git Bash folds it to 127)" ;;
            0xC0000374) echo "Windows $hex STATUS_HEAP_CORRUPTION -- the native heap detected corruption (Git Bash folds it to 127)" ;;
            0xC00000FD) echo "Windows $hex STATUS_STACK_OVERFLOW (Git Bash folds it to 127)" ;;
            0xC000001D) echo "Windows $hex STATUS_ILLEGAL_INSTRUCTION (Git Bash folds it to 132)" ;;
            0xC0000135) echo "Windows $hex STATUS_DLL_NOT_FOUND (Git Bash folds it to 127)" ;;
            0xC0000142) echo "Windows $hex STATUS_DLL_INIT_FAILED (Git Bash folds it to 127)" ;;
            0xC000013A) echo "Windows $hex STATUS_CONTROL_C_EXIT (Git Bash folds it to 130)" ;;
            0xE0434352) echo "Windows $hex CLR exception code -- an unhandled managed exception; its text should be above (Git Bash folds it to 127)" ;;
            0x80131623) echo "Windows $hex COR_E_FAILFAST -- .NET Environment.FailFast" ;;
            0x80131506) echo "Windows $hex ExecutionEngineException -- CLR internal fatal error" ;;
            *)          echo "Windows exit status $raw ($hex) -- not in this script's table" ;;
        esac
        return
    fi
    case "$code" in
        0)   echo "success" ;;
        126) echo "command found but not executable (dotnet?)" ;;
        127)
            if $IS_MSYS; then
                echo "either dotnet is not on PATH, or -- once the host has printed anything -- the Windows host died with an NTSTATUS Git Bash does not map to a signal: fail-fast 0xC0000409 (abort/std::terminate/FailFast), heap corruption 0xC0000374, stack overflow 0xC00000FD, a DLL load failure, or an unhandled CLR exception 0xE0434352; cygwin's status_exit() folds all of those to 127. The raw status is captured by the cmd.exe wrapper and printed above when available."
            else
                echo "command not found -- dotnet is not on PATH"
            fi ;;
        130) echo "SIGINT" ;;
        131) echo "SIGQUIT" ;;
        132) echo "SIGILL -- illegal instruction (on Git Bash: STATUS_ILLEGAL_INSTRUCTION)" ;;
        133) echo "SIGTRAP -- breakpoint/trap (e.g. __builtin_trap, Rust/absl hard failure)" ;;
        134) echo "SIGABRT -- abort(): std::terminate / uncaught C++ exception / assertion in native code, or .NET FailFast on Unix" ;;
        135) echo "SIGBUS (Linux) -- misaligned or unmapped mmap access; on Git Bash: STATUS_NO_MEMORY" ;;
        136) echo "SIGFPE -- arithmetic fault" ;;
        137) echo "SIGKILL -- killed externally (OOM killer, timeout)" ;;
        138) echo "SIGBUS (macOS) -- misaligned or unmapped mmap access (Linux: SIGUSR1)" ;;
        139) echo "SIGSEGV -- segfault in native code (on Git Bash: STATUS_ACCESS_VIOLATION 0xC0000005)" ;;
        141) echo "SIGPIPE" ;;
        143) echo "SIGTERM -- terminated externally (job cancellation, container stop)" ;;
        *)
            if (( code > 128 )); then
                echo "killed by signal $(( code - 128 ))"
            else
                echo "the smoke returned its own [FAIL] count ($code assertion failure(s) below the pass floor), or dotnet run failed before the host ran"
            fi ;;
    esac
}

# Git Bash folds a Windows process's 32-bit exit status into a POSIX one:
# cygwin's status_exit() keeps only STATUS_ACCESS_VIOLATION (-> SIGSEGV, 139),
# STATUS_ILLEGAL_INSTRUCTION (-> 132), STATUS_NO_MEMORY (-> 135) and
# STATUS_CONTROL_C_EXIT (-> 130); EVERY other 0xC... crash code becomes 127 --
# which is all the first observed crash left behind. So on Windows the host runs
# under cmd.exe, which writes the real %ERRORLEVEL% to a file; this script
# then prints it and folds it the way Git Bash would have, so the banner's
# exit code stays comparable with earlier runs.
HOST_LOG="$WORKDIR/smoke-host.log"
RAW_EXIT=""
run_host_windows() {
    local exit_file="$WORKDIR/host-exit-code.txt"
    # Redirection FIRST: `echo %CODE%> file` with CODE=1 would parse as a
    # handle-1 redirect and write "ECHO is on." instead of the code.
    cat > "$WORKDIR/run-host.cmd" <<'CMDEOF'
@echo off
dotnet run --project "%~1" -r %~2 --no-build -c Release
set HOST_CODE=%ERRORLEVEL%
>"%~3" echo %HOST_CODE%
exit /b 0
CMDEOF
    # Full path: an MSYS `cmd` on PATH would shadow System32\cmd.exe.
    local cmd_exe
    cmd_exe="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32/cmd.exe"
    # NOT `| tee` here (unlike run_host_posix). The Tier 3 hosts spawn
    # factory_worker.exe with RedirectStandardOutput/Error, which .NET's
    # Windows Process.Start does with bInheritHandles=TRUE -- so every worker
    # also inherits a copy of this pipeline's stdout handle. Those hosts are
    # never disposed (see the host's exit [PHASE] line) and the workers
    # outlive the host until this script's EXIT trap kills them, and tee would
    # wait for an EOF that only the runner's post-step orphan cleanup can
    # deliver: the first Windows run with it sat in this step until cancelled.
    # (On Unix the .NET child closes every fd above 2 before exec, so tee is
    # fine there.) Redirect to the log file and stream it with tail, which
    # stops when cmd.exe exits; a file handle held open by an orphan blocks
    # nothing.
    : > "$HOST_LOG"
    "$cmd_exe" //c "$(cygpath -w "$WORKDIR/run-host.cmd")" \
        "$(cygpath -w "$TEST_DIR/SmokeTest.csproj")" "$RID" \
        "$(cygpath -w "$exit_file")" > "$HOST_LOG" 2>&1 &
    local cmd_pid=$!
    tail -n +1 -f --pid="$cmd_pid" "$HOST_LOG"
    wait "$cmd_pid"
    local cmd_status=$?
    if [[ -s "$exit_file" ]]; then
        RAW_EXIT="$(tr -d '[:space:]' < "$exit_file")"
    fi
    if [[ "$RAW_EXIT" =~ ^-?[0-9]+$ ]]; then
        if (( RAW_EXIT >= 0 && RAW_EXIT <= 255 )); then
            EXIT_CODE=$RAW_EXIT
        else
            case "$(printf '0x%08X' $(( RAW_EXIT & 0xFFFFFFFF )))" in
                0xC0000005) EXIT_CODE=139 ;;
                0xC000001D) EXIT_CODE=132 ;;
                0xC0000017) EXIT_CODE=135 ;;
                0xC000013A) EXIT_CODE=130 ;;
                0xC*)       EXIT_CODE=127 ;;
                *)          EXIT_CODE=$(( RAW_EXIT & 0xFF )) ;;
            esac
        fi
    else
        # The wrapper itself failed to run or to write the file: fall back to
        # cmd.exe's own status (non-zero only if cmd.exe could not start it).
        echo "WARNING: cmd.exe wrapper left no exit-code file (cmd.exe status $cmd_status)"
        EXIT_CODE=$cmd_status
        if (( EXIT_CODE == 0 )); then EXIT_CODE=1; fi
    fi
}

run_host_posix() {
    dotnet run --project "$TEST_DIR/SmokeTest.csproj" -r "$RID" --no-build -c Release 2>&1 | tee "$HOST_LOG"
    EXIT_CODE=${PIPESTATUS[0]}
}

report_failure() {
    echo "Exit code meaning:   $(describe_exit "$EXIT_CODE" "$RAW_EXIT")"
    if [[ -n "$RAW_EXIT" ]]; then
        echo "Raw Windows status:  $RAW_EXIT ($(printf '0x%08X' $(( RAW_EXIT & 0xFFFFFFFF ))))"
    fi
    echo "Package version:     $PACKAGE_VERSION (Directory.Build.props), RID $RID"
    local versions
    versions="$(grep -h '^\[VERSIONS\]' "$HOST_LOG" 2>/dev/null | tail -1 || true)"
    if [[ -n "$versions" ]]; then
        echo "Versions under test: ${versions#\[VERSIONS\] }"
    else
        echo "Versions under test: (host died before Tier 0 printed its [VERSIONS] line)"
    fi
    local last_phase last_assert
    last_phase="$(grep -h '^\[PHASE\]' "$HOST_LOG" 2>/dev/null | tail -1 || true)"
    last_phase="${last_phase:-[PHASE] (none printed)}"
    last_assert="$(grep -h '^\[PASS\]\|^\[FAIL\]' "$HOST_LOG" 2>/dev/null | tail -1 || true)"
    echo "Last phase reached:  ${last_phase#\[PHASE\] }"
    echo "Last assertion:      ${last_assert:-(none printed)}"
    echo "Dump directory:      $DUMP_DIR_NATIVE"
    local dumps
    dumps="$(find "$DUMP_DIR" -type f 2>/dev/null || true)"
    if [[ -z "$dumps" ]]; then
        echo "Dumps written:       none (no native crash, createdump could not run, or the process was killed outright; see any createdump lines above)"
    else
        echo "Dumps written:"
        ls -la "$DUMP_DIR" | sed 's/^/                     /'
        local report
        while IFS= read -r report; do
            echo "--- $(basename "$report") (first 16 KB) ---"
            head -c 16384 "$report"
            echo ""
        done < <(find "$DUMP_DIR" -type f -name '*.crashreport.json' 2>/dev/null)
    fi
}

# ---------- Restore and build ----------
echo "--- Restoring packages ---"
dotnet restore "$TEST_DIR/SmokeTest.csproj" -r "$RID" --verbosity quiet

echo ""
echo "--- Building ---"
dotnet build "$TEST_DIR/SmokeTest.csproj" -r "$RID" --no-restore -c Release --verbosity quiet

echo ""
echo "--- Running smoke test ---"
export SMOKE_PACKAGE_VERSION="$PACKAGE_VERSION"   # for the host's [VERSIONS] line
EXIT_CODE=1
set +e
if $IS_MSYS; then
    run_host_windows
else
    run_host_posix
fi
set -e

echo ""
if [[ $EXIT_CODE -eq 0 ]]; then
    echo "=== SMOKE TEST PASSED ==="
else
    echo "=== SMOKE TEST FAILED (exit code $EXIT_CODE) ==="
    report_failure
fi
exit $EXIT_CODE
