# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Prune-WinDiskCache.ps1 - cap a Windows runner's Bazel disk cache by size.
#
# Bazel garbage-collects a disk cache (--experimental_disk_cache_gc_max_size)
# only from its server, only after the server has been idle for a while
# (--experimental_disk_cache_gc_idle_delay), and a `bazel shutdown` or a
# server exit cancels a collection in progress. A Windows job's Bazel server
# is shut down at the start of the next job on the runner, so the cap passed
# on the command line never takes effect and a cache kept under
# CI_WIN_CACHE_DIR grows without bound (docs/ci/runner-environment.md). Bazel
# 8 has no way to run the collection synchronously, so the jobs call this
# right after their stale-server shutdown, when nothing holds the directory.
# It does for the Windows disk cache what prune-bazel-disk-cache.sh does for
# the Linux one.
#
# It touches ONLY <CacheDir>\ac and <CacheDir>\cas (what Bazel's disk cache
# writes), never anything else under <CacheDir>, and never follows a reparse
# point. If ac + cas exceed -MaxSize (N[K|M|G|T], Bazel's size syntax;
# default: the CI_WIN_CACHE_MAX_SIZE runner variable), the files with the
# oldest last-write time go first until the rest fits; an action-cache entry
# goes before a content blob of the same time. Bazel refreshes the time of an
# entry it hits, so this is least-recently-used order. No -MaxSize (or an
# unparsable one) means no cap: nothing is removed. A missing directory is
# not an error. A file that cannot be removed is a warning. The script
# always exits 0, and logs the size before and after.
#
# Usage: Prune-WinDiskCache.ps1 -CacheDir <dir> [-MaxSize 12G]
param(
  [string]$CacheDir,
  [string]$MaxSize = $env:CI_WIN_CACHE_MAX_SIZE
)
$ErrorActionPreference = 'Stop'
$tag = 'disk cache prune'

try {
  if (-not $MaxSize) {
    Write-Host "${tag}: no size cap set (CI_WIN_CACHE_MAX_SIZE), nothing to do"
    exit 0
  }
  if ($MaxSize -notmatch '^(\d{1,13})([KMGT]?)B?$') {
    Write-Host "::warning::${tag}: cannot read the size cap '$MaxSize' (expected N[K|M|G|T]), not pruning"
    exit 0
  }
  $unit = switch ($Matches[2]) { 'K' { 1KB } 'M' { 1MB } 'G' { 1GB } 'T' { 1TB } default { 1 } }
  [long]$max = [long]$Matches[1] * $unit

  if (-not $CacheDir -or -not (Test-Path -LiteralPath $CacheDir -PathType Container)) {
    Write-Host "${tag}: '$CacheDir' does not exist, nothing to do"
    exit 0
  }

  $files = New-Object System.Collections.Generic.List[object]
  foreach ($sub in 'ac', 'cas') {
    $root = Join-Path $CacheDir $sub
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
    if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
    $stack = New-Object System.Collections.Generic.Stack[string]
    $stack.Push($root)
    while ($stack.Count -gt 0) {
      $dir = $stack.Pop()
      foreach ($e in (Get-ChildItem -LiteralPath $dir -Force)) {
        if ($e.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
        if ($e.PSIsContainer) { $stack.Push($e.FullName) }
        else { $files.Add([pscustomobject]@{ Path = $e.FullName; Size = [long]$e.Length; Time = $e.LastWriteTimeUtc; Rank = $(if ($sub -eq 'ac') { 0 } else { 1 }) }) }
      }
    }
  }

  [long]$total = 0
  foreach ($f in $files) { $total += $f.Size }
  [long]$before = $total
  $removed = 0
  [long]$freed = 0
  if ($total -gt $max) {
    foreach ($f in ($files | Sort-Object Time, Rank, Path)) {
      if ($total -le $max) { break }
      try {
        Remove-Item -LiteralPath $f.Path -Force
        $total -= $f.Size; $freed += $f.Size; $removed += 1
      } catch {
        Write-Host "::warning::${tag}: could not remove $($f.Path): $($_.Exception.Message)"
      }
    }
  }
  Write-Host ("${tag}: {0}: {1} files, {2} bytes before; removed {3} files, freed {4} bytes; {5} bytes kept (cap {6})" -f $CacheDir, $files.Count, $before, $removed, $freed, $total, $MaxSize)
} catch {
  Write-Host "::warning::${tag}: failed: $($_.Exception.Message); the cache is left as it is"
}
exit 0
