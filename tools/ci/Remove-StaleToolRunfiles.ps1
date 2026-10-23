# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Remove build-tool runfiles trees that hold directory junctions from a
    Bazel output base.

.DESCRIPTION
    Bazel 8 on Windows creates a directory junction for a runfiles entry whose
    target is a cache hit that is not on disk yet, and it never rebuilds such a
    tree while its manifest is unchanged. rules_foreign_cc copies each build
    tool together with its runfiles tree (pkg-config, cmake, ...) into the
    target's ext_build_deps directory; a junction to a file arrives there as a
    directory and the copy fails ("cannot overwrite directory").

    The Windows config builds no runfiles links (--nobuild_runfile_links), so
    new output bases no longer get such trees, but an output base that
    persists between jobs keeps the ones made before. This removes them: only
    runfiles trees of exec-configuration outputs (build tools), and only those
    that contain a junction. Tools then resolve their runfiles through the
    manifest, as in a fresh output base. Test runfiles trees are left alone.

    Safety: the walks never descend into a link (Windows PowerShell 5.1's
    Get-ChildItem -Recurse and Remove-Item -Recurse do), a listing error
    aborts before anything is deleted, and a tree is removed with
    `rmdir /s /q`, which removes links as links and never enters them. A tree
    that is still there afterwards fails the script.

    Only the given output base is touched: other runners on the same host
    share the output root and may be building in theirs.

.PARAMETER OutputBase
    The job's output base (`bazel info output_base`). When
    WIN_BAZEL_OUTPUT_ROOT is set, the output base must lie under it.
#>
param(
    [Parameter(Mandatory = $true)][string]$OutputBase
)

$ErrorActionPreference = 'Stop'
$reparse = [IO.FileAttributes]::ReparsePoint

function Test-IsLink([string]$Path) {
    return (([IO.File]::GetAttributes($Path) -band $reparse) -ne 0)
}

# Every *.runfiles directory under $Dir, without entering links. Throws on a
# listing error.
function Find-RunfilesTrees([string]$Dir) {
    foreach ($d in [IO.Directory]::GetDirectories($Dir)) {
        if (Test-IsLink $d) { continue }
        if ($d.EndsWith('.runfiles')) { $d } else { Find-RunfilesTrees $d }
    }
}

# True when $Dir holds a directory link (junction or directory symlink) at
# any depth, without entering links. Throws on a listing error.
function Test-HoldsJunction([string]$Dir) {
    foreach ($d in [IO.Directory]::GetDirectories($Dir)) {
        if (Test-IsLink $d) { return $true }
        if (Test-HoldsJunction $d) { return $true }
    }
    return $false
}

$base = [IO.Path]::GetFullPath($OutputBase)
if ($env:WIN_BAZEL_OUTPUT_ROOT) {
    $root = [IO.Path]::GetFullPath($env:WIN_BAZEL_OUTPUT_ROOT).TrimEnd('\') + '\'
    if (-not $base.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "output base '$base' is not under WIN_BAZEL_OUTPUT_ROOT '$root'"
    }
}

$removed = 0
$out = Join-Path $base 'execroot\_main\bazel-out'
if (Test-Path -LiteralPath $out) {
    $execBins = @([IO.Directory]::GetDirectories($out) |
        Where-Object { (Split-Path -Leaf $_) -like '*-exec-*' } |
        ForEach-Object { Join-Path $_ 'bin' } |
        Where-Object { Test-Path -LiteralPath $_ })
    # Decide first, for every tree, before deleting anything.
    $stale = @(foreach ($bin in $execBins) {
        foreach ($tree in @(Find-RunfilesTrees $bin)) {
            if (Test-HoldsJunction $tree) { $tree }
        }
    })
    foreach ($tree in $stale) {
        & $env:ComSpec /c "rmdir /s /q `"$tree`""
        if (Test-Path -LiteralPath $tree) {
            Write-Host "::error::could not remove the stale tool runfiles tree $tree"
            throw "could not remove the stale tool runfiles tree $tree"
        }
        Write-Host "removed a stale tool runfiles tree: $tree"
        $removed++
    }
}
Write-Host "stale tool runfiles trees removed: $removed"
$global:LASTEXITCODE = 0
