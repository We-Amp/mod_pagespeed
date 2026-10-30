# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Fixture test for Test-ZeroCopyGateResult.ps1: a skipped gate must fail.
# Run: powershell -NoProfile -File tools\ci\Test-ZeroCopyGateResult_test.ps1
$ErrorActionPreference = 'Continue'
$verifier = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'Test-ZeroCopyGateResult.ps1'
$fails = 0
function Check { param([string]$Name, [bool]$Ok) if ($Ok) { Write-Host "PASS: $Name" } else { Write-Host "FAIL: $Name"; $script:fails += 1 } }

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("zcg-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
function Run-Case {
    param([string]$Name, [string[]]$Log, [int]$Rc, [int]$Want, [string]$Expect = '')
    $f = Join-Path $tmp ($Name -replace '[^A-Za-z0-9]', '_')
    Set-Content -Path $f -Value $Log -Encoding ascii
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $verifier -LogPath $f -PytestRc $Rc 2>&1 | Out-String
    $ok = ($LASTEXITCODE -eq $Want)
    if ($Expect) { $ok = $ok -and ($out -match [regex]::Escape($Expect)) }
    Check $Name $ok
}
try {
    $skipReason = 'SKIPPED [1] test/system/iis/test_iis_zerocopy_gate.py:333: no rewritten .pagespeed.ic URL appeared for the gate'
    Run-Case 'one passed' @('test_iis_zerocopy_gate.py::TestZeroCopyMergeGate::test_slow_client_region_rewrite_canary PASSED', '===== 1 passed, 480 deselected in 41.20s =====') 0 0
    Run-Case 'one passed with duration suffix' @('===== 1 passed, 480 deselected in 41.20s (0:00:41) =====') 0 0
    Run-Case 'skip is a failure (rc 0)' @('...SKIPPED', '===== short test summary info =====', $skipReason, '===== 1 skipped, 480 deselected in 3.10s =====') 0 1 'were skipped'
    Run-Case 'skip reason is printed' @($skipReason, '===== 1 skipped in 3.10s =====') 0 1 'no rewritten .pagespeed.ic URL'
    Run-Case 'failed' @('===== 1 failed in 9.00s =====') 1 1
    Run-Case 'passed but pytest rc nonzero' @('===== 1 passed in 9.00s =====') 1 1 'pytest exited 1'
    Run-Case 'nothing selected' @('===== 481 deselected in 2.00s =====') 5 1
    Run-Case 'two passed' @('===== 2 passed in 9.00s =====') 0 1 'exactly 1 passed'
    Run-Case 'one passed one skipped' @('===== 1 passed, 1 skipped in 9.00s =====') 0 1 'were skipped'
    Run-Case 'no summary' @('python crashed') 0 1 'no pytest summary'
    $gone = & powershell -NoProfile -ExecutionPolicy Bypass -File $verifier -LogPath (Join-Path $tmp 'absent') -PytestRc 0 2>&1
    Check 'missing log fails closed' ($LASTEXITCODE -ne 0)
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($fails -ne 0) { Write-Host "$fails check(s) failed"; exit 1 }
Write-Host 'all checks passed'
exit 0
