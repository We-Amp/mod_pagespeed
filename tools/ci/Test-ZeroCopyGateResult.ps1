# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Decide whether the zero-copy gate's pytest run really ran the gate test.

.DESCRIPTION
    The gate test (test/system/iis/test_iis_zerocopy_gate.py) skips itself when
    the rig did not rewrite an image, or when the aliased serve never engaged.
    pytest exits 0 for a skip, so the exit code alone cannot tell "the gate ran
    and held" from "the gate did not run". This script reads the captured pytest
    output and passes only when pytest exited 0 AND the summary reports exactly
    one passed test and no skipped, failed or errored tests. Anything else,
    including a missing summary, is a failure; skip reasons are printed.

    Exit code 0 = the gate ran and passed; 1 = it did not.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$LogPath,
    [Parameter(Mandatory = $true)][int]$PytestRc
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $LogPath)) {
    Write-Host "FAIL: pytest output not found: $LogPath"
    exit 1
}
$lines = @(Get-Content -LiteralPath $LogPath)

# The last pytest summary line: "=== 1 passed, 2 deselected in 41.2s ===" (the
# counts may also be followed by a "(0:00:41)" duration).
$summary = $null
foreach ($l in $lines) {
    if ($l -match '^=+ .*\d+ (passed|skipped|failed|error|errors|deselected|xfailed|xpassed|warnings?).* in [0-9.]+s') { $summary = $l }
}

$count = @{ passed = 0; skipped = 0; failed = 0; error = 0 }
if ($summary) {
    foreach ($m in [regex]::Matches($summary, '(\d+) (passed|skipped|failed|errors?)')) {
        $k = $m.Groups[2].Value -replace '^errors$', 'error'
        $count[$k] = [int]$m.Groups[1].Value
    }
}

Write-Host '=== zero-copy gate test outcome ==='
Write-Host "  pytest rc : $PytestRc"
if ($summary) {
    Write-Host "  summary   : $($summary.Trim('= '))"
} else {
    Write-Host '  summary   : (none found in the pytest output)'
}

foreach ($l in $lines) {
    if ($l -match '^SKIPPED \[') { Write-Host "  skip      : $l" }
}

$reasons = @()
if ($PytestRc -ne 0) { $reasons += "pytest exited $PytestRc" }
if (-not $summary) { $reasons += 'no pytest summary line, so the outcome is unknown' }
elseif ($count.skipped -gt 0) { $reasons += "$($count.skipped) test(s) were skipped; a skipped gate has not run (see the skip reason above)" }
elseif ($count.passed -ne 1) { $reasons += "expected exactly 1 passed test, got $($count.passed)" }
if ($count.failed -gt 0 -or $count.error -gt 0) { $reasons += "$($count.failed) failed, $($count.error) errored" }

if ($reasons.Count -gt 0) {
    foreach ($r in $reasons) { Write-Host "FAIL: $r" }
    exit 1
}
Write-Host 'OK: the gate test ran and passed (1 passed, 0 skipped).'
exit 0
