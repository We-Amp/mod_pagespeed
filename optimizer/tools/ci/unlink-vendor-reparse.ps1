# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# unlink-vendor-reparse.ps1 - remove the reparse points (junctions and
# symlinks) a previous build left in a Windows runner workspace.
#
# A Windows build leaves vendor\bazel-external as a junction to a Bazel output
# base, which itself holds many nested junctions that point back into
# vendor\. The checkout step's clean walks that loop on every run. This script
# runs before the checkout and removes only the links, so the walk has nothing
# to follow.
#
# It looks at the direct children of <Workspace>\vendor and, when
# <Workspace>\vendor\bazel-external is a real directory, at its direct
# children. Each reparse point goes with [IO.Directory]::Delete($path, $false),
# which removes the link and never its target. It never recurses into a
# reparse point. A workspace without vendor\ is not an error.
#
# Usage: unlink-vendor-reparse.ps1 -Workspace <dir>
# Prints: unlinked <n> reparse point(s)
param(
  [Parameter(Mandatory = $true)][string]$Workspace
)
$ErrorActionPreference = 'Stop'

$reparse = [IO.FileAttributes]::ReparsePoint
$isDir = [IO.FileAttributes]::Directory
$count = 0
$failed = 0

# Remove one reparse point. A directory link or junction goes with
# Directory.Delete(path, $false); a file symlink with File.Delete. Both remove
# the link and never its target. A failure is a warning, never an error: this
# is a cleanup helper and must not fail the leg.
function Remove-Link {
  param([System.IO.FileSystemInfo]$Entry)
  try {
    if (($Entry.Attributes -band $isDir) -ne 0) {
      [IO.Directory]::Delete($Entry.FullName, $false)
    } else {
      [IO.File]::Delete($Entry.FullName)
    }
    $script:count += 1
  } catch {
    $script:failed += 1
    Write-Host ("::warning::could not unlink " + $Entry.FullName + ": " + $_.Exception.Message)
  }
}

function Remove-Links {
  param([string]$Dir)
  $di = New-Object System.IO.DirectoryInfo($Dir)
  foreach ($e in $di.GetFileSystemInfos()) {
    if (($e.Attributes -band $reparse) -ne 0) { Remove-Link $e }
  }
}

function Write-Summary {
  Write-Host ("unlinked " + $count + " reparse point(s), " + $failed + " failed")
}

$vendor = Join-Path $Workspace 'vendor'

# Everything below is one guarded body: a listing error is a warning, never a
# failed leg.
try {
  # GetAttributes works for a dangling link too (Test-Path does not).
  $attr = $null
  try {
    $attr = [IO.File]::GetAttributes($vendor)
  } catch [System.IO.FileNotFoundException] {
  } catch [System.IO.DirectoryNotFoundException] {
  }

  if ($null -ne $attr) {
    if (($attr -band $reparse) -ne 0) {
      # vendor itself a link: unlink it and stop; never list inside it.
      try {
        [IO.Directory]::Delete($vendor, $false)
        $count += 1
      } catch {
        $failed += 1
        Write-Host ("::warning::could not unlink " + $vendor + ": " + $_.Exception.Message)
      }
    } else {
      Remove-Links $vendor

      # A real (non-link) bazel-external directory: unlink the links inside it.
      $bx = Join-Path $vendor 'bazel-external'
      if (Test-Path -LiteralPath $bx) {
        $item = Get-Item -LiteralPath $bx -Force
        if (($item.Attributes -band $reparse) -eq 0) { Remove-Links $bx }
      }
    }
  }
} catch {
  Write-Host ("::warning::unlink step failed: " + $_.Exception.Message)
}

Write-Summary
exit 0
