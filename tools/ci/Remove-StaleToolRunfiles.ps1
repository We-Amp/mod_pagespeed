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
    persists between jobs keeps the ones made before. This removes them: only runfiles
    trees of exec-configuration outputs (build tools), and only those that
    contain a junction. Tools then resolve their runfiles through the
    manifest, as in a fresh output base. Test runfiles trees are left alone.

    Only the given output base is touched: other runners on the same host
    share the output root and may be building in theirs.

.PARAMETER OutputBase
    The job's output base (`bazel info output_base`).
#>
param(
    [Parameter(Mandatory = $true)][string]$OutputBase
)

$ErrorActionPreference = 'Continue'
$removed = 0
$out = Join-Path $OutputBase 'execroot\_main\bazel-out'
if (Test-Path -LiteralPath $out) {
    $execBins = Get-ChildItem -LiteralPath $out -Directory -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like '*-exec-*' } |
        ForEach-Object { Join-Path $_.FullName 'bin' } |
        Where-Object { Test-Path -LiteralPath $_ }
    foreach ($bin in $execBins) {
        # -Attributes !ReparsePoint keeps the walk out of the junctions.
        $trees = Get-ChildItem -LiteralPath $bin -Recurse -Directory -Force -Attributes !ReparsePoint -Filter '*.runfiles' -ErrorAction SilentlyContinue
        foreach ($tree in $trees) {
            $links = @(Get-ChildItem -LiteralPath $tree.FullName -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue)
            $junctions = @($links | Where-Object { $_.PSIsContainer })
            if ($junctions.Count -gt 0) {
                try {
                    # Unlink every link first (deleting a junction or a file
                    # symlink removes the link, never its target), then the
                    # rest of the tree.
                    foreach ($l in $links) {
                        if ($l.PSIsContainer) { [IO.Directory]::Delete($l.FullName, $false) }
                        else { [IO.File]::Delete($l.FullName) }
                    }
                    Remove-Item -LiteralPath $tree.FullName -Recurse -Force -ErrorAction Stop
                } catch {
                    Write-Host "::warning::$($_.Exception.Message)"
                }
                if (Test-Path -LiteralPath $tree.FullName) {
                    Write-Host "::warning::could not remove the stale runfiles tree $($tree.FullName)"
                } else {
                    Write-Host "removed a stale tool runfiles tree: $($tree.FullName)"
                    $removed++
                }
            }
        }
    }
}
Write-Host "stale tool runfiles trees removed: $removed"
