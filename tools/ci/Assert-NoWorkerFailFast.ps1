<#
.SYNOPSIS
    Fail loud when an IIS worker process died during a verified test run.

.DESCRIPTION
    Application Verifier does not fail a test run when it stops a process: it
    fail-fasts w3wp.exe, and the harness, talking to a freshly spawned worker,
    never notices. A job whose tests all pass is therefore NOT evidence that the
    worker survived -- the AppVerif job printed "No crash dumps (clean run)" on
    runs where w3wp was dying once per iteration.

    This reads the three places Windows records such a death, and fails if any
    of them saw one:

      1. System log, WAS 5009 "terminated unexpectedly" -- carries the exit
         code. 5011/5078 are recorded as context but never fail on their own.
      2. Application log, "Application Error" -- names the faulting module
         (vrfcore.dll for a verifier stop).
      3. WER Report.wer -- Sig[8].Value is the AppVerifier stop code, which
         says WHICH provider stopped, and is the only signal that does.

    Deliberately does NOT depend on a crash .dmp or on the LocalDumps registry
    key. Arming LocalDumps makes WER write the dump but SKIP the ReportArchive
    entry, so a dump-based probe and an archive-based probe each go blind in the
    other's presence. The event log is the one signal neither suppresses.

    Nothing in a healthy run produces a 5009: the harness tears the pool down
    with appcmd/Restart-WebAppPool, and only ever Stop-Processes iisexpress.
    A hand-run `Stop-Process w3wp` does produce one (exit 0xffffffff), which is
    why this is a CI gate and not something to run on a box someone is poking at.
#>
[CmdletBinding()]
param(
    # Only consider events at or after this instant. Pass the moment
    # verification was armed, not job start: the app-pool recycle that precedes
    # arming is graceful and must not be mistaken for a stop.
    [Parameter(Mandatory = $true)][DateTime]$Since,

    [string]$ReportPath = 'C:\dumps\failfast-report.txt',
    [string]$ProcessName = 'w3wp.exe',

    # Report findings but exit 0, for staging the gate on a noisy runner.
    [switch]$WarnOnly
)

$ErrorActionPreference = 'Stop'

# AppVerifier stop codes observed in this repo, keyed by Sig[8].Value (hex, no 0x).
$KnownStops = @{
    '13'  = 'Heaps: FIRST_CHANCE_ACCESS_VIOLATION (use-after-free / heap corruption)'
    '253' = 'Locks/SRWLock: recursive acquire'
    '900' = 'Leak: allocation still owned by a DLL at FreeLibrary'
}

# Console stays skimmable; the uploaded report keeps everything.
$MaxConsoleLines = 12

$report = [System.Collections.Generic.List[string]]::new()
function Emit([string]$s, [switch]$FileOnly) {
    $report.Add($s)
    if (-not $FileOnly) { Write-Host $s }
}
function EmitMany([string[]]$all) {
    $i = 0
    foreach ($s in $all) {
        if ($i -lt $MaxConsoleLines) { Emit $s } else { Emit $s -FileOnly }
        $i++
    }
    if ($all.Count -gt $MaxConsoleLines) {
        Write-Host "  ... and $($all.Count - $MaxConsoleLines) more (see $ReportPath)"
    }
}

Emit "=== worker fail-fast check for $ProcessName since $($Since.ToString('o')) ==="

$failFast = 0      # 5009 with STATUS_FAIL_FAST_EXCEPTION
$otherExit = 0     # 5009 with any other exit code
$appErrModules = @{}
$stopCodes = @{}

# -- 1. WAS events -----------------------------------------------------------
# Get-WinEvent throws a terminating "no events found" even under
# -ErrorAction SilentlyContinue, so every query is wrapped.
try {
    $was = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 5009, 5011, 5078; StartTime = $Since } -ErrorAction Stop)
} catch { $was = @() }

if ($was.Count -gt 0) {
    Emit "--- WAS events ($($was.Count)) ---"
    EmitMany @($was | ForEach-Object { "[$($_.TimeCreated)] WAS $($_.Id): $($_.Message.Trim())" })
    foreach ($e in $was | Where-Object { $_.Id -eq 5009 }) {
        if ($e.Message -match "exit code was '0x([0-9a-fA-F]+)'") {
            if ($Matches[1] -eq 'c0000421') { $failFast++ } else { $otherExit++ }
        } else { $otherExit++ }
    }
} else { Emit '--- WAS events: none ---' }

# -- 2. Application Error ----------------------------------------------------
try {
    $appErr = @(Get-WinEvent -FilterHashtable @{
            LogName = 'Application'; ProviderName = 'Application Error'; StartTime = $Since
        } -ErrorAction Stop | Where-Object { $_.Message -match [regex]::Escape($ProcessName) })
} catch { $appErr = @() }

if ($appErr.Count -gt 0) {
    Emit "--- Application Error ($($appErr.Count)) ---"
    foreach ($e in $appErr) {
        $mod = if ($e.Message -match 'Faulting module name:\s*([^,]+)') { $Matches[1].Trim() } else { 'unknown' }
        $exc = if ($e.Message -match 'Exception code:\s*(\S+)') { $Matches[1] } else { 'unknown' }
        if (-not $appErrModules.ContainsKey($mod)) { $appErrModules[$mod] = 0 }
        $appErrModules[$mod]++
        Emit "[$($e.TimeCreated)] $ProcessName crashed: module=$mod exception=$exc"
        Emit $e.Message -FileOnly
    }
} else { Emit '--- Application Error: none ---' }

# -- 3. WER reports ----------------------------------------------------------
# Archive and queue both: a report lands in one or the other depending on
# whether WER finished processing it.
$stem = $ProcessName -replace '\.exe$', ''
$reports = @(foreach ($root in 'C:\ProgramData\Microsoft\Windows\WER\ReportArchive',
        'C:\ProgramData\Microsoft\Windows\WER\ReportQueue') {
        if (Test-Path $root) {
            Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match [regex]::Escape($stem) -and $_.LastWriteTime -ge $Since }
        }
    })

if ($reports.Count -gt 0) {
    Emit "--- WER reports ($($reports.Count)) ---"
    foreach ($r in $reports | Sort-Object LastWriteTime) {
        $wer = Join-Path $r.FullName 'Report.wer'
        if (-not (Test-Path $wer)) { continue }
        $sig = @(Get-Content $wer -ErrorAction SilentlyContinue | Where-Object { $_ -match '^(Sig\[\d+\]\.|EventType=)' })
        Emit "[$($r.LastWriteTime)] $($r.Name)" -FileOnly
        foreach ($s in $sig) { Emit "    $($s.Trim())" -FileOnly }

        $code = @($sig | Where-Object { $_ -match '^Sig\[8\]\.Value=(.+)$' } |
                ForEach-Object { $Matches[1].Trim() })[0]
        $key = if ($code) { $code } else { '(no Sig[8])' }
        if (-not $stopCodes.ContainsKey($key)) { $stopCodes[$key] = 0 }
        $stopCodes[$key]++
    }
    foreach ($k in $stopCodes.Keys | Sort-Object) {
        $desc = if ($KnownStops.ContainsKey($k)) { $KnownStops[$k] } else { 'unrecognized stop code' }
        Emit "  stop 0x${k} x$($stopCodes[$k]): $desc"
    }
} else { Emit '--- WER reports: none ---' }

# -- verdict -----------------------------------------------------------------
$total = $failFast + $otherExit + $appErr.Count + $reports.Count
Emit ''
if ($total -eq 0) {
    Emit "PASS: no $ProcessName fail-fast recorded by WAS, Application Error, or WER."
} else {
    Emit "FAIL: $ProcessName died during this run."
    if ($failFast)  { Emit "  WAS 5009 x${failFast}: STATUS_FAIL_FAST_EXCEPTION (0xc0000421) -- a verifier stop" }
    if ($otherExit) { Emit "  WAS 5009 x${otherExit}: terminated unexpectedly, other exit code" }
    foreach ($m in $appErrModules.Keys) { Emit "  Application Error x$($appErrModules[$m]): faulting module $m" }
    foreach ($k in $stopCodes.Keys | Sort-Object) {
        $desc = if ($KnownStops.ContainsKey($k)) { $KnownStops[$k] } else { 'unrecognized' }
        Emit "  WER stop 0x${k} x$($stopCodes[$k]): $desc"
    }
    Emit ''
    Emit 'A verified worker that dies is a real defect even when every test passed:'
    Emit 'the harness reconnects to a fresh worker and reports green. Decode a dump with'
    Emit '  cdb -z <dmp> -y "srv*C:\symcache*https://msdl.microsoft.com/download/symbols;<pdbdir>" -c "!avrf"'
    Emit '!avrf prints Arg3 = owner DLL, Arg2 = allocation stack (dps <addr>).'
}

$dir = Split-Path $ReportPath -Parent
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
Set-Content -Path $ReportPath -Value $report -Encoding UTF8
Write-Host "full report: $ReportPath"

if ($total -gt 0 -and -not $WarnOnly) { exit 1 }
exit 0
