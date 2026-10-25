# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Print the path of a repository tool that a step of the master pipeline is
# about to run, taking it from the default branch when the checkout does not
# have it. The PowerShell counterpart of tools/ci/with-default-branch-tool.sh;
# see "Tools a step runs from the checkout" in docs/ci/pipelines.md.
#
# Run from the repository root (the Actions workspace). Without -PreferDefault
# the checkout's copy wins when it exists. The default branch's copy is
# written under $env:RUNNER_TEMP\default-branch-tools\ together with the
# files named in -With (siblings the tool loads, such as its self-test).
# Fails closed (exit 1) when the file is in neither place.
#
# Usage: tools\ci\Get-DefaultBranchTool.ps1 -Path tools/ci/X.ps1 [-With tools/ci/X_test.ps1] [-PreferDefault]
# Env:   DEFAULT_BRANCH_REMOTE (default origin), DEFAULT_BRANCH (default master)
param(
  [Parameter(Mandatory = $true)][string]$Path,
  [string[]]$With = @(),
  [switch]$PreferDefault
)
$ErrorActionPreference = 'Continue'

$remote = if ($env:DEFAULT_BRANCH_REMOTE) { $env:DEFAULT_BRANCH_REMOTE } else { 'origin' }
$branch = if ($env:DEFAULT_BRANCH) { $env:DEFAULT_BRANCH } else { 'master' }

if ((-not $PreferDefault) -and (Test-Path -LiteralPath $Path -PathType Leaf)) {
  Write-Output $Path
  exit 0
}

& git fetch --quiet --depth=1 $remote $branch 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
  Write-Host "::error::$Path is not in this checkout and $remote/$branch could not be fetched"
  exit 1
}
& git cat-file -e "FETCH_HEAD:$Path" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
  Write-Host "::error::$Path is in neither this checkout nor $branch"
  exit 1
}

$base = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$out = Join-Path $base 'default-branch-tools'
New-Item -ItemType Directory -Force -Path $out | Out-Null
# git archive -o and tar keep the bytes exact (a PowerShell pipe would re-encode them).
$tar = Join-Path $out ("fetch-" + [guid]::NewGuid().ToString('N') + ".tar")
$paths = @($Path) + @($With)
& git archive --format=tar -o $tar FETCH_HEAD -- @paths
if ($LASTEXITCODE -ne 0) {
  Write-Host "::error::git archive of $($paths -join ', ') from $branch failed"
  exit 1
}
& tar -xf $tar -C $out
$code = $LASTEXITCODE
Remove-Item -LiteralPath $tar -Force -ErrorAction SilentlyContinue
if ($code -ne 0) {
  Write-Host "::error::could not unpack $Path from $branch"
  exit 1
}
Write-Host "::notice::$Path taken from $branch (this branch predates it or has a different version)"
Write-Output (Join-Path $out ($Path -replace '/', '\'))
exit 0
