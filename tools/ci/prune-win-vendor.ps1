# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# prune-win-vendor.ps1 - age-guarded prune of a Windows runner's local
# vendor directory.
#
# A Windows runner may keep the vendor tarballs it fetched in a local
# directory (the runner variable CI_WIN_VENDOR_DIR, see
# docs/ci/runner-environment.md). Each run leaves a tarball of roughly
# 700 MB there, plus the temporary file of an interrupted fetch or the
# decompressed .tar of an interrupted extract. This script removes them
# once they are older than a short window.
#
# The rules match the Linux vendor job's prune:
#   * only files named mod_pagespeed-*.tar.zst, *.tar.zst.sha256 and
#     *.tar.zst.*.tmp, plus *.tar.zst.tar (the decompression intermediate),
#     and only once older than -MaxAgeMinutes (default 2 hours);
#   * never this run's tarball or its sidecar (-ShortSha); this run's
#     tarball is re-stamped first, so the window counts from last use;
#   * a sidecar goes only when its tarball is gone too, so a kept tarball
#     never loses its digest.
# A live run's temporary file is younger than the window: the fetch and the
# decompression write it continuously, and no job runs longer than 2 hours.
#
# The directory named by -SharedDir (default: the CI_WIN_VENDOR_DIR runner
# variable) is left alone when it is the directory being pruned: a runner
# that shares that directory with the Linux jobs of the same machine lets
# the Linux vendor job's prune own it. Deletion errors (a file in use) are
# warnings; the prune never fails the job for them.
#
# Usage: prune-win-vendor.ps1 -VendorDir <dir> -ShortSha <sha7>
#          [-MaxAgeMinutes 120] [-SharedDir <dir>]
param(
  [string]$VendorDir,
  [Parameter(Mandatory = $true)][string]$ShortSha,
  [int]$MaxAgeMinutes = 120,
  [string]$SharedDir = $env:CI_WIN_VENDOR_DIR
)
$ErrorActionPreference = 'Stop'

if ($ShortSha -notmatch '^[0-9a-f]{7,40}$') {
  Write-Host "::error::prune-win-vendor: -ShortSha must be a hex commit id, got '$ShortSha'"
  exit 2
}
if (-not $VendorDir -or -not (Test-Path -LiteralPath $VendorDir -PathType Container)) {
  Write-Host "prune-win-vendor: no vendor dir at '$VendorDir'; nothing to prune"
  exit 0
}
$full = (Resolve-Path -LiteralPath $VendorDir).ProviderPath.TrimEnd('\')
if ($SharedDir -and ($full -ieq $SharedDir.TrimEnd('\'))) {
  Write-Host "prune-win-vendor: $full is the shared vendor dir; the Linux vendor job's prune owns it"
  exit 0
}

$prefix = 'mod_pagespeed-'
$curTarball = "$prefix$ShortSha.tar.zst"
$curSidecar = "$curTarball.sha256"
$now = Get-Date
$cutoff = $now.AddMinutes(-$MaxAgeMinutes)

# Re-stamp this run's tarball (and sidecar) so later jobs of this run, and
# other runs of this SHA, keep it for a full window from now.
foreach ($n in @($curTarball, $curSidecar)) {
  $p = Join-Path $full $n
  if (Test-Path -LiteralPath $p -PathType Leaf) {
    try { (Get-Item -LiteralPath $p).LastWriteTime = $now }
    catch { Write-Host "::warning::prune-win-vendor: could not re-stamp ${p}: $($_.Exception.Message)" }
  }
}

$script:freed = 0
$script:removed = 0
function Remove-Aged {
  param([System.IO.FileInfo]$File)
  try {
    $size = $File.Length
    Remove-Item -LiteralPath $File.FullName -Force
    $script:freed += $size
    $script:removed += 1
    Write-Host ("prune-win-vendor: removed {0} ({1:N0} MB, last written {2:u})" -f $File.Name, ($size / 1MB), $File.LastWriteTime.ToUniversalTime())
  } catch {
    Write-Host "::warning::prune-win-vendor: could not remove $($File.FullName): $($_.Exception.Message)"
  }
}

# -like, not Get-ChildItem -Filter: the filesystem filter's 8.3 quirk lets
# '*.zst' match '*.zst.tmp' too.
$files = @(Get-ChildItem -LiteralPath $full -File | Where-Object { $_.Name -like "$prefix*" })
$aged = @($files | Where-Object { $_.LastWriteTime -lt $cutoff })

# Pass 1: tarballs, temp files and decompression intermediates.
foreach ($f in $aged) {
  $n = $f.Name
  if ($n -eq $curTarball -or $n -eq $curSidecar) { continue }
  if ($n -like "$prefix*.tar.zst" -or $n -like "$prefix*.tar.zst.*.tmp" `
      -or ($n -like "$prefix*.tar.zst.tar" -and $n -ne "$curTarball.tar")) {
    Remove-Aged $f
  }
}

# Pass 2: sidecars, only where the tarball is gone (kept tarballs keep theirs).
foreach ($f in $aged) {
  $n = $f.Name
  if ($n -eq $curSidecar -or -not ($n -like "$prefix*.tar.zst.sha256")) { continue }
  $tarball = Join-Path $full ($n.Substring(0, $n.Length - '.sha256'.Length))
  if (Test-Path -LiteralPath $tarball) { continue }
  Remove-Aged $f
}

Write-Host ("prune-win-vendor: {0} file(s) removed, {1:N0} MB freed in {2} (window {3} min, kept {4})" -f $script:removed, ($script:freed / 1MB), $full, $MaxAgeMinutes, $curTarball)
exit 0
