<#
.SYNOPSIS
    Run the IIS system suite repeatedly under different Application Verifier
    provider sets, and fail if any arm/iteration regresses.

.DESCRIPTION
    Arms are run STRICTLY SERIALLY, and never as a GitHub matrix. AppVerifier
    and gflags settings for w3wp.exe are machine-global (Image File Execution
    Options), and one IIS instance serves them all, so two arms on one runner
    would silently overwrite each other's provider set.

    Why more than one arm, and why nightly rather than per-PR:

      Cuzz (concurrency fuzzing) is what perturbs PageSpeed's async work off its
      inline-completion path. It is what surfaced the WinHTTP async-read race.
      It is nondeterministic: it changes detection PROBABILITY, not correctness,
      and on a healthy tree it has never produced a false failure. So it costs
      nothing on a PR and it cannot be relied on there either.

      Full page heap is the opposite. It catches the memory-corruption class
      deterministically (AppVerifier stop 0x13, a first-chance AV at the moment
      of the bad access) -- that is how the use-after-free defects in this
      module were found. But it also SLOWS everything down, which dampens the
      very races Cuzz is trying to expose.

      Measured against the async-read race, before it was fixed:

          Cuzz + page heap        1 of 3 iterations reproduced
          Cuzz, no page heap      3 of 3 iterations reproduced
          page heap, no Cuzz      0 of 3 iterations reproduced

      So the two settings pull in opposite directions and neither dominates.
      The PR job runs Cuzz + page heap: deterministic on corruption, plus a free
      lottery ticket on races. This nightly buys the statistical power the PR
      job cannot afford -- above all the no-page-heap arm, which is the most
      race-sensitive configuration and is not represented in PR CI at all.

    The Leak provider is never enabled anywhere. It stops on any allocation
    still owned by a DLL at FreeLibrary, and this module keeps immortal
    singletons on purpose, so it fail-fasts the worker on every app-pool
    recycle with no defect behind it.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepoDir,

    # "<arm>:<iterations>". Accepts an array, or one comma-separated string --
    # `powershell -File` flattens array arguments into a single comma-joined
    # string, and that is how CI invokes this.
    [string[]]$Arms = @('cuzz-nopageheap:5', 'cuzz-pageheap:3'),

    [string]$EvidenceDir = 'C:\dumps\appverif-matrix',

    # AppVerif + page heap run PageSpeed's async work 5-50x slower; scales
    # fetch_until's poll budget and the pytest per-test timeout.
    [int]$TimeoutMultiplier = 4,

    # Print the plan and exit, without touching machine-global verifier state.
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$BaseProviders = 'Heaps', 'Handles', 'Locks', 'Memory', 'SRWLock', 'TLS', 'Exceptions', 'Threadpool'

$ArmSpecs = @{
    # The most race-sensitive configuration. Not represented in PR CI.
    'cuzz-nopageheap' = @{ Cuzz = $true;  PageHeap = $false }
    # Mirrors the PR job: corruption class + a probabilistic shot at races.
    'cuzz-pageheap'   = @{ Cuzz = $true;  PageHeap = $true  }
    # Deterministic corruption-only arm; useful to bisect a page-heap stop.
    'pageheap-nocuzz' = @{ Cuzz = $false; PageHeap = $true  }
}

$gflags = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe'
$detector = Join-Path $RepoDir 'tools\ci\Assert-NoWorkerFailFast.ps1'
if (-not (Test-Path $detector)) { throw "detector not found: $detector" }

# Parse the plan up front so a typo fails before any machine state is touched.
$armList = @(($Arms -join ',') -split ',' | Where-Object { $_.Trim() })
$plan = foreach ($a in $armList) {
    $name, $iters = $a.Trim() -split ':', 2
    if (-not $ArmSpecs.ContainsKey($name)) {
        throw "unknown arm '$name'; known: $($ArmSpecs.Keys -join ', ')"
    }
    if (-not $iters) { $iters = '1' }
    [pscustomobject]@{ Name = $name; Iterations = [int]$iters; Spec = $ArmSpecs[$name] }
}

Write-Host '=== AppVerifier matrix plan ==='
foreach ($p in $plan) {
    Write-Host ("  {0,-18} x{1}  cuzz={2} pageheap={3}" -f $p.Name, $p.Iterations, $p.Spec.Cuzz, $p.Spec.PageHeap)
}
Write-Host "  providers: $($BaseProviders -join ' ') (Leak deliberately absent)"
Write-Host "  total iterations: $(($plan | Measure-Object -Property Iterations -Sum).Sum)"
if ($DryRun) { Write-Host 'dry run; machine state untouched.'; exit 0 }

New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

function Disable-Verifier {
    # Native tools exit non-zero when nothing is registered, and 2>&1 turns their
    # stderr into terminating ErrorRecords under ErrorActionPreference=Stop.
    try { appverif -disable * -for w3wp.exe 2>&1 | Out-Null } catch { }
    try { & $gflags /p /disable w3wp.exe 2>&1 | Out-Null } catch { }
    $global:LASTEXITCODE = 0
}

$results = [System.Collections.Generic.List[object]]::new()

try {
    foreach ($arm in $plan) {
        for ($i = 1; $i -le $arm.Iterations; $i++) {
            $tag = "$($arm.Name)-$i"
            Write-Host ''
            Write-Host "########## $tag ##########"

            Disable-Verifier

            # Purge the file cache so a previous iteration's cache-extended
            # resources cannot produce Last-Modified mismatches.
            if (Test-Path 'C:\pagespeed_cache') { Remove-Item 'C:\pagespeed_cache' -Recurse -Force -ErrorAction SilentlyContinue }

            appverif -enable @BaseProviders -for w3wp.exe
            if ($arm.Spec.Cuzz)     { appverif -enable Cuzz -for w3wp.exe -with Cuzz.FuzzingLevel=4 }
            if ($arm.Spec.PageHeap) { & $gflags /p /enable w3wp.exe /full /dlls pagespeed_iis.dll }

            # Recycle AFTER arming: IFEO settings are read at process start, so
            # the worker that serves this iteration must be spawned now. The old
            # worker dies unverified, which is why this cannot trip the detector.
            Import-Module WebAdministration -ErrorAction SilentlyContinue
            if (Get-Command Restart-WebAppPool -ErrorAction SilentlyContinue) {
                Restart-WebAppPool -Name 'DefaultAppPool' -ErrorAction SilentlyContinue
            }

            $since = Get-Date
            $sw = [Diagnostics.Stopwatch]::StartNew()

            $env:PAGESPEED_TEST_TIMEOUT_MULTIPLIER = "$TimeoutMultiplier"
            # Route run_iis_tests.ps1's on-failure snapshots (statistics +
            # message_history, taken before its teardown recycles the worker)
            # into this iteration's corner of the evidence artifact.
            $env:PAGESPEED_EVIDENCE_DIR = Join-Path $EvidenceDir $tag
            & { $ErrorActionPreference = 'Continue'
                Push-Location $RepoDir
                & powershell -NoProfile -ExecutionPolicy Bypass -File "$RepoDir\test\system\run_iis_tests.ps1" -SkipBuild -UseFullIIS
                Pop-Location }
            $testRc = $LASTEXITCODE
            $sw.Stop()

            & powershell -NoProfile -ExecutionPolicy Bypass -File $detector `
                -Since $since -ReportPath (Join-Path $EvidenceDir "$tag.txt")
            $failFastRc = $LASTEXITCODE

            Disable-Verifier

            $ok = ($testRc -eq 0) -and ($failFastRc -eq 0)
            $results.Add([pscustomobject]@{
                    Arm = $arm.Name; Iteration = $i; Seconds = [int]$sw.Elapsed.TotalSeconds
                    TestRc = $testRc; FailFast = ($failFastRc -ne 0); Ok = $ok
                })
            Write-Host "$tag : tests rc=$testRc, fail-fast=$($failFastRc -ne 0), $([int]$sw.Elapsed.TotalSeconds)s"
        }
    }
} finally {
    # MUST run even on failure: leaving IFEO verifier
    # settings armed would slow, and eventually fail-fast, every later job.
    Disable-Verifier
    Write-Host 'AppVerif + page heap disabled.'
}

Write-Host ''
Write-Host '=== matrix summary ==='
$results | Format-Table -AutoSize | Out-String | Write-Host

$bad = @($results | Where-Object { -not $_.Ok })
if ($bad.Count -gt 0) {
    Write-Host "FAIL: $($bad.Count) of $($results.Count) iteration(s) regressed:"
    foreach ($b in $bad) {
        $why = @()
        if ($b.TestRc -ne 0) { $why += "tests rc=$($b.TestRc)" }
        if ($b.FailFast)     { $why += 'worker fail-fast' }
        Write-Host "  $($b.Arm)-$($b.Iteration): $($why -join ', ')"
    }
    Write-Host "evidence: $EvidenceDir"
    exit 1
}
Write-Host "PASS: all $($results.Count) iteration(s) clean."
exit 0
