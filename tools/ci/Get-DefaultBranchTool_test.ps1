# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test for Get-DefaultBranchTool.ps1, using temporary git
# repositories as the checkout and the default branch's remote.
# Run: powershell -NoProfile -File tools\ci\Get-DefaultBranchTool_test.ps1
$ErrorActionPreference = 'Continue'
$helper = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'Get-DefaultBranchTool.ps1'
$fails = 0
function Check { param([string]$Name, [bool]$Ok) if ($Ok) { Write-Host "PASS: $Name" } else { Write-Host "FAIL: $Name"; $script:fails += 1 } }

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("dbt-test-" + [guid]::NewGuid().ToString('N'))
$origin = Join-Path $tmp 'origin'
$work = Join-Path $tmp 'work'
$oldLoc = Get-Location
try {
  New-Item -ItemType Directory -Force -Path (Join-Path $origin 'tools\x'), (Join-Path $work 'tools\x'), (Join-Path $tmp 'rt') | Out-Null
  Set-Content -Path (Join-Path $origin 'tools\x\tool.ps1') -Value 'new-tool' -Encoding ascii
  Set-Content -Path (Join-Path $origin 'tools\x\tool_test.ps1') -Value 'sibling' -Encoding ascii
  & git -C $origin init -q -b master
  & git -C $origin config user.email t@example.invalid
  & git -C $origin config user.name t
  & git -C $origin add -A
  & git -C $origin commit -q -m init
  & git -C $work init -q -b topic
  Set-Content -Path (Join-Path $work 'tools\x\old.ps1') -Value 'old-tool' -Encoding ascii
  $env:RUNNER_TEMP = Join-Path $tmp 'rt'
  Set-Location $work

  $env:DEFAULT_BRANCH_REMOTE = 'file:///nonexistent'
  $got = & powershell -NoProfile -File $helper -Path 'tools/x/old.ps1' 2>$null
  Check 'present in checkout' ($LASTEXITCODE -eq 0 -and "$got" -eq 'tools/x/old.ps1')

  $env:DEFAULT_BRANCH_REMOTE = $origin
  $got = & powershell -NoProfile -File $helper -Path 'tools/x/tool.ps1' -With 'tools/x/tool_test.ps1' 2>$null
  $gotPath = ($got | Select-Object -Last 1)
  Check 'absent: fetched, same content' (($LASTEXITCODE -eq 0) -and (Test-Path -LiteralPath "$gotPath") -and ((Get-Content "$gotPath") -match 'new-tool'))
  Check 'absent: sibling extracted alongside' (Test-Path -LiteralPath (Join-Path (Split-Path -Parent "$gotPath") 'tool_test.ps1'))

  & powershell -NoProfile -File $helper -Path 'tools/x/none.ps1' *> $null
  Check 'missing everywhere fails closed' ($LASTEXITCODE -ne 0)
} finally {
  Set-Location $oldLoc
  Remove-Item Env:DEFAULT_BRANCH_REMOTE -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($fails -gt 0) { Write-Host "$fails check(s) failed"; exit 1 }
Write-Host "all checks passed"
exit 0
