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
  # One retry: a network blip must not turn a gate red.
  Start-Sleep -Seconds 5
  & git fetch --quiet --depth=1 $remote $branch 2>$null | Out-Null
}
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
# cmd.exe's redirect writes the blob byte-exactly (a PowerShell pipe would
# re-encode it, and a tar from the PATH may not be the one we expect).
foreach ($p in (@($Path) + @($With))) {
  $dest = Join-Path $out ($p -replace '/', '\')
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
  & $env:ComSpec /c "git show FETCH_HEAD:$p > `"$dest`""
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $dest) -or (Get-Item -LiteralPath $dest).Length -eq 0) {
    Write-Host "::error::could not write $p from $branch"
    exit 1
  }
}
Write-Host "::notice::$Path taken from $branch (this branch predates it or has a different version)"
Write-Output (Join-Path $out ($Path -replace '/', '\'))
exit 0
