# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Run the IIS zero-copy M1 merge-gate integration test under Application
    Verifier: slow client + region-rewrite canary against the aliased serve.

.DESCRIPTION
    The zero-copy submit loop (DriveZeroCopyServe) carries an in-code MERGE
    GATE (M1) requiring, before merge, an on-IIS integration test of
    "slow client + AppVerifier + a canary that rewrites the mapped region
    after each completion and asserts byte-identity-or-reset."

    This wraps that test (test/system/iis/test_iis_zerocopy_gate.py) in the
    AppVerifier harness so an aliased use-after-overwrite surfaces as a
    first-chance AppVerifier stop (page heap, stop 0x13) rather than as silent
    corruption that a black-box byte-compare might race past. Full page heap on
    pagespeed_iis.dll is the point here; Cuzz is not needed (the slow client
    already forces the async completion off the inline path).

    The rig is configured for the gate by setup_iis_full.ps1 when
    PAGESPEED_ZEROCOPY_GATE=1: CycloneZeroCopy(+Serve) on and a 64 MB Cyclone
    volume so the flood wraps it mid-serve.

    Providers deliberately match Invoke-AppVerifMatrix's base set (Leak stays
    off -- immortal singletons fail-fast it on recycle). Machine-global IFEO
    state is always disabled in the finally block.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepoDir,
    [string]$EvidenceDir = 'C:\dumps\zerocopy-gate',
    # AppVerifier + page heap run the async serve much slower; scale the
    # pytest poll/timeout budget the same way the matrix does.
    [int]$TimeoutMultiplier = 4,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$BaseProviders = 'Heaps', 'Handles', 'Locks', 'Memory', 'SRWLock', 'TLS', 'Exceptions', 'Threadpool'
$gflags = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe'
$detector = Join-Path $RepoDir 'tools\ci\Assert-NoWorkerFailFast.ps1'
if (-not (Test-Path $detector)) { throw "detector not found: $detector" }

Write-Host '=== zero-copy M1 gate plan ==='
Write-Host "  providers: $($BaseProviders -join ' ') + full page heap on pagespeed_iis.dll (Leak absent)"
Write-Host '  rig: PAGESPEED_ZEROCOPY_GATE=1 (CycloneZeroCopy on, 64 MB Cyclone volume)'
Write-Host "  test: iis/test_iis_zerocopy_gate.py (slow client + region-rewrite canary)"
if ($DryRun) { Write-Host 'dry run; machine state untouched.'; exit 0 }

New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

function Disable-Verifier {
    try { appverif -disable * -for w3wp.exe 2>&1 | Out-Null } catch { }
    try { & $gflags /p /disable w3wp.exe 2>&1 | Out-Null } catch { }
    $global:LASTEXITCODE = 0
}

$since = Get-Date
$testRc = 1
$failFastRc = 1
try {
    Disable-Verifier
    if (Test-Path 'C:\pagespeed_cache') { Remove-Item 'C:\pagespeed_cache' -Recurse -Force -ErrorAction SilentlyContinue }

    appverif -enable @BaseProviders -for w3wp.exe
    & $gflags /p /enable w3wp.exe /full /dlls pagespeed_iis.dll

    $env:PAGESPEED_ZEROCOPY_GATE = '1'
    $env:PAGESPEED_TEST_TIMEOUT_MULTIPLIER = "$TimeoutMultiplier"
    $env:PAGESPEED_EVIDENCE_DIR = $EvidenceDir

    # run_iis_tests.ps1 -UseFullIIS spawns the worker AFTER arming IFEO (its
    # setup recycles the app pool), so the worker that serves the gate is the
    # verified one. -TestFilter narrows to the gate module.
    & { $ErrorActionPreference = 'Continue'
        Push-Location $RepoDir
        & powershell -NoProfile -ExecutionPolicy Bypass `
            -File "$RepoDir\test\system\run_iis_tests.ps1" `
            -SkipBuild -UseFullIIS -TestFilter zerocopy_gate -Verbose
        Pop-Location }
    $testRc = $LASTEXITCODE

    & powershell -NoProfile -ExecutionPolicy Bypass -File $detector `
        -Since $since -ReportPath (Join-Path $EvidenceDir 'worker-failfast.txt')
    $failFastRc = $LASTEXITCODE
} finally {
    Disable-Verifier
    Write-Host 'AppVerif + page heap disabled.'
}

Write-Host ''
Write-Host '=== zero-copy M1 gate summary ==='
Write-Host "  pytest rc      : $testRc"
Write-Host "  worker failfast: $($failFastRc -ne 0)"
Write-Host "  evidence       : $EvidenceDir"

if (($testRc -ne 0) -or ($failFastRc -ne 0)) {
    Write-Host 'FAIL: zero-copy M1 gate regressed.'
    exit 1
}
Write-Host 'PASS: zero-copy M1 gate clean.'
exit 0
