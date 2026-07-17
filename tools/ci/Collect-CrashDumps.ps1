<#
.SYNOPSIS
    Collect, decode, and stage w3wp crash dumps produced by WER LocalDumps
    during a verified run -- and say so LOUDLY when the worker died and no dump
    exists.

.DESCRIPTION
    Replaces the old "Collect crash dumps" step, whose failure mode was to print
    "No crash dumps (clean run)" on runs where w3wp had in fact fail-fasted
    under Application Verifier. That line was actively misleading: it looked
    only at C:\dumps\*.dmp, and nothing was ever configured to put a dump there
    for a verifier stop (see Set-WerLocalDumps.ps1 for why procdump did not).

    Three things this does that the old step did not:

      1. Looks in the LocalDumps directory, and attributes dumps to THIS run by
         timestamp (-Since), so a stale dump from a previous run cannot ship
         mislabeled as this run's evidence.

      2. Cross-checks against the fail-fast verdict. If the worker died and no
         dump was captured, that is an INSTRUMENTATION defect and is reported as
         a GitHub error annotation -- never as a clean run. It does not change
         the exit code: Assert-NoWorkerFailFast.ps1 owns the gate, and this
         script must not double-fail or mask it.

      3. Waits for WER. WER writes the dump asynchronously and a 1-2 GB full
         dump takes tens of seconds; collecting immediately after the harness
         exits would race it and conclude "no dump" while the file was still
         being written. Polls for size stability, bounded by -SettleSeconds.

    DECODE. Arming LocalDumps costs us the WER ReportArchive entry, and with it
    Sig[8] -- the only signal that said WHICH provider stopped (see
    Assert-NoWorkerFailFast.ps1). That classification is recovered here, and
    then some: `!avrf` on the dump yields the provider, the stop code, the owner
    DLL and the allocation stack -- strictly more than Sig[8] carried. Decoding
    in-job is what makes the artifact answer the question without anyone
    downloading a multi-GB dump first. Best-effort: a missing cdb or a slow
    symbol server degrades the artifact, it does not fail the job.

.NOTES
    Exit code is always 0 by design; the fail-fast gate is the adjudicator.
#>
[CmdletBinding()]
param(
    # LocalDumps directory (PAGESPEED_DUMP_DIR, published by Set-WerLocalDumps.ps1).
    [Parameter(Mandatory = $true)][string]$DumpDir,

    # Small directory that the upload-artifact step ships. Compressed dumps and
    # decode output land here; the multi-GB originals never do.
    [Parameter(Mandatory = $true)][string]$StageDir,

    # Only dumps at/after this instant belong to this run. Pass APPVERIF_T0.
    [DateTime]$Since = [DateTime]::MinValue,

    # Verdict source from Assert-NoWorkerFailFast.ps1: either its
    # failfast-report.txt (the CI workflow, one run) or a DIRECTORY of per-iteration
    # reports (the nightly matrix writes one <arm>-<n>.txt per iteration). Any
    # report carrying a FAIL: verdict means the worker died at least once.
    [string]$FailFastReport = '',

    # Stage at most this many dumps, EARLIEST first: the first stop is the
    # root cause; later ones are usually cascade from the same corruption.
    [int]$MaxDumps = 2,

    # Upper bound on waiting for WER to finish writing.
    [int]$SettleSeconds = 120,

    # Symbols for the decode (the .pdb the job downloaded alongside the DLL).
    [string]$SymbolDir = '',

    # Give up on cdb after this long; symbol-server fetches can crawl.
    [int]$DecodeTimeoutSeconds = 600
)

$ErrorActionPreference = 'Continue'

New-Item -ItemType Directory -Path $StageDir -Force | Out-Null

# -- Did the worker die? ------------------------------------------------------
$failFast = $false
if ($FailFastReport -and (Test-Path $FailFastReport)) {
    $reports = if (Test-Path $FailFastReport -PathType Container) {
        @(Get-ChildItem "$FailFastReport\*.txt" -ErrorAction SilentlyContinue)
    } else {
        @(Get-Item $FailFastReport)
    }
    $failed = @($reports | Where-Object {
            @(Get-Content $_.FullName -ErrorAction SilentlyContinue | Where-Object { $_ -match '^FAIL:' }).Count -gt 0
        })
    $failFast = $failed.Count -gt 0
    # Kept as plain statements rather than a nested-quote subexpression: this
    # runs on Windows PowerShell 5.1 and must not hinge on nested-quote parsing.
    if ($failFast) {
        $which = ($failed | ForEach-Object { $_.BaseName }) -join ', '
        Write-Host "fail-fast verdict from $($reports.Count) report(s): FAIL -- worker died in: $which"
    } else {
        Write-Host "fail-fast verdict from $($reports.Count) report(s): PASS"
    }
} else {
    Write-Host "note: no fail-fast report at '$FailFastReport'; cannot cross-check dump presence against it"
}

# -- Wait for WER, then enumerate --------------------------------------------
function Get-RunDumps {
    if (-not (Test-Path $DumpDir)) { return @() }
    return @(Get-ChildItem "$DumpDir\*.dmp" -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $Since } | Sort-Object LastWriteTime)
}

# Only wait when we have reason to expect a dump; a clean run must not pay it.
if ($failFast) {
    $deadline = (Get-Date).AddSeconds($SettleSeconds)
    $lastTotal = -1
    while ((Get-Date) -lt $deadline) {
        $d = Get-RunDumps
        $total = ($d | Measure-Object -Property Length -Sum).Sum
        if (-not $total) { $total = 0 }
        # Stable and non-empty => WER is done writing.
        if ($d.Count -gt 0 -and $total -eq $lastTotal) { break }
        if ($d.Count -eq 0) { Write-Host 'waiting for WER to write a dump...' }
        else { Write-Host "dump(s) still growing ($([math]::Round($total / 1MB)) MB); waiting..." }
        $lastTotal = $total
        Start-Sleep -Seconds 5
    }
}

$dumps = Get-RunDumps

# -- Report -------------------------------------------------------------------
Write-Host ''
Write-Host "=== crash dumps in $DumpDir (this run: at/after $($Since.ToString('o'))) ==="
if ($dumps.Count -gt 0) {
    foreach ($d in $dumps) {
        Write-Host ("  {0}  {1,6} MB  {2}" -f $d.Name, [math]::Round($d.Length / 1MB), $d.LastWriteTime.ToString('o'))
    }
} else {
    Write-Host '  (none)'
}
$older = @(Get-ChildItem "$DumpDir\*.dmp" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $Since })
if ($older.Count -gt 0) { Write-Host "  note: $($older.Count) older dump(s) present, not from this run (ignored)" }

if ($failFast -and $dumps.Count -eq 0) {
    # The exact hole this hardening exists to close. Loud, and never "clean run".
    Write-Host "::error title=Worker fail-fast with NO dump captured::w3wp.exe died under Application Verifier but WER LocalDumps produced no dump in $DumpDir. The stop is real (see the fail-fast report); the capture path is broken. Check: (1) the LocalDumps key survived the run -- another job's cleanup may have removed it mid-run; (2) the dump drive had free space; (3) WER service is running; (4) the stop killed the process too fast for WER (raise DumpCount/disk headroom)."
} elseif ($failFast) {
    Write-Host "::notice title=Crash dump captured::$($dumps.Count) dump(s) captured for the verifier stop; decode output is in the evidence artifact."
} elseif ($dumps.Count -gt 0) {
    Write-Host "::warning title=Dump without a fail-fast verdict::$($dumps.Count) dump(s) were written although the fail-fast check passed. Worth a look: a worker died in a way the event-log probes did not classify."
} else {
    # Now this line is honest: it is cross-checked, not assumed.
    Write-Host 'No crash dumps, and the fail-fast check passed: clean run.'
}

if ($dumps.Count -eq 0) { exit 0 }

# -- Decode (best-effort) -----------------------------------------------------
$cdb = @(
    'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe',
    'C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1

$target = $dumps[0]   # earliest = first stop = root cause
if ($cdb) {
    $symPath = "srv*C:\symcache*https://msdl.microsoft.com/download/symbols"
    if ($SymbolDir -and (Test-Path $SymbolDir)) { $symPath = "$symPath;$SymbolDir" }
    $decodeOut = Join-Path $StageDir "$($target.BaseName).avrf.txt"
    Write-Host ''
    Write-Host "=== decoding $($target.Name) with cdb (!avrf) ==="
    # NOT $args -- that is an automatic variable in PowerShell.
    $cdbArgs = @('-z', $target.FullName, '-y', $symPath, '-c', '.symopt+0x40; !avrf; !analyze -v; q')
    $p = Start-Process -FilePath $cdb -ArgumentList $cdbArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput $decodeOut -RedirectStandardError "$decodeOut.err"
    if (-not $p.WaitForExit($DecodeTimeoutSeconds * 1000)) {
        Write-Warning "cdb exceeded ${DecodeTimeoutSeconds}s (symbol server slow?); killing it and keeping partial output"
        try { $p.Kill() } catch { }
    }
    if (Test-Path $decodeOut) {
        # The provider/stop lines are the ones that replace the lost WER Sig[8].
        $head = @(Get-Content $decodeOut -ErrorAction SilentlyContinue |
                Where-Object { $_ -match 'VERIFIER STOP|Application Verifier|^Stop code|owner DLL|FAULTING|EXCEPTION_CODE|MODULE_NAME|IMAGE_NAME|BUGCHECK' } |
                Select-Object -First 20)
        if ($head.Count -gt 0) {
            Write-Host '--- decode highlights (full output in the artifact) ---'
            $head | ForEach-Object { Write-Host "  $_" }
        } else {
            Write-Host "  (no verifier/analyze lines matched; see $(Split-Path $decodeOut -Leaf) in the artifact)"
        }
    }
} else {
    Write-Host '::warning title=cdb not found::Debugging Tools for Windows (cdb.exe) not on this runner; shipping the dump undecoded. Decode manually: cdb -z <dmp> -y "srv*C:\symcache*https://msdl.microsoft.com/download/symbols;<pdbdir>" -c "!avrf"'
}

# -- Stage (compressed) -------------------------------------------------------
# zstd is already a hard dependency of these workflows (vendor tarballs), and a
# page-heap dump is mostly fill patterns, so it compresses hard and fast. -T0 -3
# keeps this in seconds rather than the minutes upload-artifact's deflate would
# spend on a 2 GB file.
$staged = 0
foreach ($d in $dumps | Select-Object -First $MaxDumps) {
    $dest = Join-Path $StageDir "$($d.Name).zst"
    & zstd -3 -T0 -q --force -o $dest $d.FullName 2>$null
    if (($LASTEXITCODE -eq 0) -and (Test-Path $dest)) {
        $ratio = [math]::Round($d.Length / (Get-Item $dest).Length, 1)
        Write-Host ("staged {0} -> {1} MB (.zst, {2}x)" -f $d.Name, [math]::Round((Get-Item $dest).Length / 1MB), $ratio)
        $staged++
    } else {
        Write-Warning "zstd failed for $($d.Name); staging uncompressed (artifact will be large)"
        Copy-Item $d.FullName (Join-Path $StageDir $d.Name) -Force -ErrorAction SilentlyContinue
        $staged++
    }
}
$global:LASTEXITCODE = 0

if ($dumps.Count -gt $MaxDumps) {
    Write-Host "note: $($dumps.Count - $MaxDumps) further dump(s) not staged (-MaxDumps $MaxDumps); the earliest stop is the diagnostic one"
}
Write-Host "staged $staged dump(s) into $StageDir"

# Manifest, so the artifact records what existed even when not everything shipped.
$manifest = Join-Path $StageDir 'dumps-manifest.txt'
$lines = @("dump dir: $DumpDir", "run anchor (APPVERIF_T0): $($Since.ToString('o'))", "fail-fast: $failFast", '')
$lines += $dumps | ForEach-Object { "{0}  {1} MB  {2}" -f $_.Name, [math]::Round($_.Length / 1MB), $_.LastWriteTime.ToString('o') }
Set-Content -Path $manifest -Value $lines -Encoding UTF8

exit 0
