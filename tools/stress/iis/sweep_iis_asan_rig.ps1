<#
.SYNOPSIS
  Collect + adjudicate ASan / crash evidence after a rig run.

.DESCRIPTION
  The harness already fails on hits it sees in its tailed logs, but ASan on
  Windows writes its report to a SEPARATE file (log_path=C:\pagespeed_asan<pid>,
  set via __asan_default_options) that the harness does not tail. This sweep is
  the authoritative gate: it EXITS NON-ZERO if any ASan report file, new crash
  dump, or w3wp Application-Error/WER event is found -- so the CI job goes red on
  a real memory bug even when the harness itself returned 0.
#>
param(
  [string]$RigDir      = $(if (Test-Path 'D:\') { 'D:\iis-asan-rig' } else { 'C:\iis-asan-rig' }),
  [string]$CoredumpDir = 'C:\CrashDumps',
  [datetime]$Since     = [datetime]::MinValue   # default: derived from the rig-start marker below
)
$ErrorActionPreference = 'Continue'
$hits = 0

# Derive the evidence window from the rig-start marker written by setup (or the
# harness-start marker as fallback), minus a small skew buffer -- ONE timezone,
# covering the whole rig lifetime. The old default, sweep-start minus 2h, was
# wrong twice over: it was anchored to the wrong moment AND only matched events
# via a UTC-vs-local rendering skew (rig gap 2).
if ($Since -eq [datetime]::MinValue) {
  $marker = @("$RigDir\logs\rig.setup.started", "$RigDir\logs\harness.started") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($marker) {
    $stamp = ((Get-Content $marker -TotalCount 1) -replace '^started\s+', '').Trim()
    try {
      # 30s skew buffer only: marker and event log share the same host clock.
      # Must stay SHORTER than setup's 60s post-iisreset settle, so events from
      # leftover workers killed by the hermetic preamble stay out of the window.
      $Since = ([datetime]::Parse($stamp, [cultureinfo]::InvariantCulture,
        [System.Globalization.DateTimeStyles]::RoundtripKind)).ToLocalTime().AddSeconds(-30)
      Write-Host "Since derived from $marker -> $Since (local)"
    } catch {
      Write-Warning "Could not parse '$stamp' from $marker"
    }
  }
  if ($Since -eq [datetime]::MinValue) {
    $Since = (Get-Date).AddHours(-2)
    Write-Warning "No usable rig-start marker under $RigDir\logs; falling back to Since=$Since"
  }
}

Write-Host "==== harness exit ===="
Get-Content "$RigDir\logs\harness.exit" -ErrorAction SilentlyContinue
$exitLine = Get-Content "$RigDir\logs\harness.exit" -ErrorAction SilentlyContinue | Select-String '^exit='
if ($exitLine -and $exitLine.Line -notmatch '^exit=0$') {
  Write-Host "HARNESS reported non-zero exit"; $hits++
}

Write-Host "`n==== harness log (last 60 lines) ===="
Get-Content "$RigDir\logs\harness.log" -Tail 60 -ErrorAction SilentlyContinue

Write-Host "`n==== ASan report files (C:\pagespeed_asan*) ===="
$asan = Get-ChildItem C:\pagespeed_asan* -File -ErrorAction SilentlyContinue
if ($asan) {
  $asan | Select-Object Name,Length,LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host
  Write-Host "---- first report (first 120 lines) ----"
  Get-Content $asan[0].FullName -TotalCount 120
  Write-Host "ASan report file(s) present -> FAIL"; $hits++
} else { Write-Host "NONE" }

Write-Host "`n==== new crash dumps since $Since ===="
$newDumps = Get-ChildItem $CoredumpDir -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $Since }
if ($newDumps) {
  $newDumps | Select-Object Name,Length,LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host
  Write-Host "Crash dump(s) present -> FAIL"; $hits++
} else { Write-Host "NONE" }

Write-Host "`n==== w3wp Application Error / WER events since $Since ===="
$ev = Get-WinEvent -FilterHashtable @{LogName="Application"; StartTime=$Since} -MaxEvents 300 -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -in @("Application Error","Windows Error Reporting",".NET Runtime") -and $_.Message -match "w3wp" }
if ($ev) {
  $ev | Select-Object TimeCreated,ProviderName,Id,@{n="Msg";e={$_.Message.Substring(0,[Math]::Min(400,$_.Message.Length))}} | Format-List | Out-String | Write-Host
  Write-Host "w3wp crash event(s) present -> FAIL"; $hits++
} else { Write-Host "NONE" }

# WAS teardown events are EVIDENCE the recycles actually happened (not a failure).
Write-Host "`n==== WAS teardown events since $Since (recycle evidence) ===="
$was = Get-WinEvent -FilterHashtable @{LogName="System"; ProviderName="Microsoft-Windows-WAS"; StartTime=$Since} -MaxEvents 500 -ErrorAction SilentlyContinue
if ($was) { $was | Group-Object Id | Select-Object Name,Count | Sort-Object Name | Format-Table -AutoSize | Out-String | Write-Host } else { Write-Host "NONE" }

Write-Host "`n==== cache stats ===="
Write-Host "cache files: $((Get-ChildItem "$RigDir\cache" -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count)"

Write-Host "`n==== SWEEP DONE: $hits failure signal(s) ===="
if ($hits -gt 0) { exit 1 } else { exit 0 }
