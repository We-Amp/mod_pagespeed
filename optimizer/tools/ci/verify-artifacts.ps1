#!/usr/bin/env pwsh
# SPDX-License-Identifier: Apache-2.0
# Verify CI artifacts match the expected commit (PowerShell/Windows).
#
# Exits non-zero with "STALE ARTIFACT: ..." on the first mismatch so the
# calling job fails before running tests against an out-of-date binary.
#
# Usage:
#   verify-artifacts.ps1 -Expected <sha> -Specs <spec>[,<spec>...]
#
# Each <spec> takes one of these forms:
#   stamp:<file>                Plain-text GIT_COMMIT file; contents must
#                               match <Expected> exactly.
#   binary:<path>               Runs "<path> --version", extracts the first
#                               hex commit in the output, and compares it
#                               against <Expected> using a common-prefix
#                               match (so short SHAs like 7-char are OK).
#   sharedlib:<path>            Loads pagespeed.dll via Win32 LoadLibraryEx
#                               and calls ps_git_commit(); compares result
#                               against <Expected> with the same prefix match
#                               as binary. Uses PowerShell-native P/Invoke,
#                               no external runtime required.
#   container:<name>:<path>     Runs "docker exec <name> cat <path>" and
#                               compares the output against <Expected>.
#
# Example:
#   verify-artifacts.ps1 -Expected $env:GITHUB_SHA `
#     -Specs "stamp:$env:WIN_WORKSPACE_DIR\GIT_COMMIT"

param(
    [Parameter(Mandatory = $true)][string]$Expected,
    [Parameter(Mandatory = $true)][string[]]$Specs
)

$ErrorActionPreference = 'Stop'

function Fail-Stale($msg) {
    Write-Host "::error::STALE ARTIFACT: $msg"
    exit 1
}

function Check-Stamp($file) {
    if (-not (Test-Path -LiteralPath $file)) {
        Fail-Stale "missing stamp file: $file"
    }
    $actual = (Get-Content -LiteralPath $file -Raw).Trim()
    if ($actual -ne $Expected) {
        Fail-Stale "expected $Expected, got $actual in $file"
    }
    Write-Host "verify-artifacts: stamp $file = $actual"
}

function Check-Binary($bin) {
    if (-not (Test-Path -LiteralPath $bin)) {
        Fail-Stale "binary not found: $bin"
    }
    $out = & $bin --version 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Fail-Stale "$bin --version failed: $out"
    }
    $match = [regex]::Match($out, '[0-9a-f]{7,40}')
    if (-not $match.Success) {
        Fail-Stale "$bin --version printed no commit sha: $out"
    }
    $version = $match.Value
    $minLen = [Math]::Min($version.Length, $Expected.Length)
    if ($Expected.Substring(0, $minLen) -ne $version.Substring(0, $minLen)) {
        Fail-Stale "$bin reports $version, expected prefix of $Expected"
    }
    Write-Host "verify-artifacts: binary $bin = $version"
}

function Check-Container($container, $path) {
    $raw = docker exec $container cat $path 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $raw) {
        Fail-Stale "docker exec $container cat $path failed"
    }
    $actual = ($raw -join '').Trim()
    if (-not $actual) {
        Fail-Stale "empty stamp in ${container}:$path"
    }
    if ($actual -ne $Expected) {
        Fail-Stale "container ${container}:$path reports $actual, expected $Expected"
    }
    Write-Host "verify-artifacts: container ${container}:$path = $actual"
}

# Compile the Win32 P/Invoke shim once per process. The type name is versioned
# so future edits to this script don't collide with a cached Add-Type from a
# prior run in the same session.
if (-not ('PSVerifyArtifacts.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace PSVerifyArtifacts {
    public static class Native {
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr LoadLibraryExW(string path, IntPtr reserved, uint flags);

        [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)]
        public static extern IntPtr GetProcAddress(IntPtr h, string name);

        [DllImport("kernel32")]
        public static extern bool FreeLibrary(IntPtr h);

        public const uint LOAD_WITH_ALTERED_SEARCH_PATH = 0x00000008;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr PsGitCommitFn();
    }
}
'@
}

function Check-SharedLib($lib) {
    if (-not (Test-Path -LiteralPath $lib)) {
        Fail-Stale "shared library not found: $lib"
    }
    # Absolute path + LOAD_WITH_ALTERED_SEARCH_PATH makes LoadLibraryEx resolve
    # dependent DLLs from the library's own directory (same behaviour the
    # Python variant got via os.add_dll_directory).
    $absLib = [System.IO.Path]::GetFullPath($lib)
    $h = [PSVerifyArtifacts.Native]::LoadLibraryExW(
        $absLib, [IntPtr]::Zero,
        [PSVerifyArtifacts.Native]::LOAD_WITH_ALTERED_SEARCH_PATH)
    if ($h -eq [IntPtr]::Zero) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Fail-Stale "LoadLibraryEx($absLib) failed (Win32 error $err)"
    }
    try {
        $proc = [PSVerifyArtifacts.Native]::GetProcAddress($h, 'ps_git_commit')
        if ($proc -eq [IntPtr]::Zero) {
            Fail-Stale "ps_git_commit symbol not found in $absLib"
        }
        $fn = [System.Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
            $proc, [PSVerifyArtifacts.Native+PsGitCommitFn])
        $strPtr = $fn.Invoke()
        if ($strPtr -eq [IntPtr]::Zero) {
            Fail-Stale "$absLib ps_git_commit() returned null"
        }
        $version = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi($strPtr)
    }
    finally {
        [PSVerifyArtifacts.Native]::FreeLibrary($h) | Out-Null
    }
    if (-not $version) {
        Fail-Stale "$absLib ps_git_commit() returned empty string"
    }
    $version = $version.Trim()
    if ($version -notmatch '^[0-9a-f]{7,40}$') {
        Fail-Stale "$absLib ps_git_commit() returned non-hex value: $version"
    }
    $minLen = [Math]::Min($version.Length, $Expected.Length)
    if ($Expected.Substring(0, $minLen) -ne $version.Substring(0, $minLen)) {
        Fail-Stale "$absLib reports $version, expected prefix of $Expected"
    }
    Write-Host "verify-artifacts: sharedlib $absLib = $version"
}

foreach ($spec in $Specs) {
    if ($spec -match '^stamp:(.+)$') {
        Check-Stamp $Matches[1]
    }
    elseif ($spec -match '^binary:(.+)$') {
        Check-Binary $Matches[1]
    }
    elseif ($spec -match '^sharedlib:(.+)$') {
        Check-SharedLib $Matches[1]
    }
    elseif ($spec -match '^container:([^:]+):(.+)$') {
        Check-Container $Matches[1] $Matches[2]
    }
    else {
        Write-Host "Unknown spec: $spec"
        Write-Host "  expected stamp:<file> | binary:<path> | sharedlib:<path> |"
        Write-Host "           container:<name>:<path>"
        exit 2
    }
}
