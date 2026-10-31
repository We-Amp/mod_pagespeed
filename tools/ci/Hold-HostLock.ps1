# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
  Host-wide lock for jobs that bind fixed local ports (the IIS system tests).

.DESCRIPTION
  Two runners on one host that run the IIS system tests at the same time both
  bind the test site to ports 8080 and 8443, and the second one fails. A
  Windows mutex belongs to the thread that took it, and every workflow step is
  its own process, so a step cannot hold it for the steps after it. This script
  therefore runs in two modes:

    -Action acquire   starts a hidden holder process, waits until the holder
                      owns the mutex (logging how long it waited), and returns.
    -Action release   tells the holder to let go and waits for it to exit.

  The holder owns the mutex until the release signal arrives or its own
  lifetime limit passes. If it dies for any reason (including the runner
  ending the job and stopping the job's processes) Windows releases the mutex.

.PARAMETER WaitMinutes
  How long acquire waits for the lock before failing.
#>
param(
  [Parameter(Mandatory = $true)][ValidateSet('acquire', 'release', 'hold')][string]$Action,
  [string]$Name = 'Global\WeAmp-IIS-SystemTests',
  [string]$StateDir = $(if ($env:RUNNER_TEMP) { Join-Path $env:RUNNER_TEMP 'host-lock' } else { Join-Path $env:TEMP 'host-lock' }),
  [int]$WaitMinutes = 75,
  [int]$MaxHoldMinutes = 120
)

$ErrorActionPreference = 'Stop'
$ready = Join-Path $StateDir 'ready'
$stop = Join-Path $StateDir 'release'
$pidFile = Join-Path $StateDir 'holder.pid'

if ($Action -eq 'hold') {
  # The holder process itself.
  $mutex = New-Object System.Threading.Mutex($false, $Name)
  $got = $false
  try {
    try { $got = $mutex.WaitOne([TimeSpan]::FromMinutes($WaitMinutes)) }
    catch [System.Threading.AbandonedMutexException] { $got = $true }
    if (-not $got) { Set-Content -LiteralPath (Join-Path $StateDir 'timeout') -Value 'timeout'; exit 0 }
    Set-Content -LiteralPath $ready -Value 'ready'
    $deadline = (Get-Date).AddMinutes($MaxHoldMinutes)
    while (-not (Test-Path -LiteralPath $stop) -and (Get-Date) -lt $deadline) {
      Start-Sleep -Seconds 2
    }
  }
  finally {
    if ($got) { try { $mutex.ReleaseMutex() } catch { } }
    $mutex.Dispose()
  }
  exit 0
}

if ($Action -eq 'acquire') {
  if (Test-Path -LiteralPath $StateDir) { Remove-Item -LiteralPath $StateDir -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
  $started = Get-Date
  $self = $MyInvocation.MyCommand.Path
  $argList = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', "`"$self`"",
    '-Action', 'hold', '-Name', $Name, '-StateDir', "`"$StateDir`"",
    '-WaitMinutes', $WaitMinutes, '-MaxHoldMinutes', $MaxHoldMinutes)
  # Redirected output keeps the holder from holding this step's output pipe
  # open, which would stall the step until its timeout.
  $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $StateDir 'holder.out.log') `
    -RedirectStandardError (Join-Path $StateDir 'holder.err.log')
  Set-Content -LiteralPath $pidFile -Value $p.Id
  $limit = $started.AddMinutes($WaitMinutes + 1)
  while (-not (Test-Path -LiteralPath $ready)) {
    if ((Test-Path -LiteralPath (Join-Path $StateDir 'timeout')) -or $p.HasExited -or (Get-Date) -gt $limit) {
      Write-Host "::error::could not take the host lock '$Name' within $WaitMinutes minutes"
      exit 1
    }
    Start-Sleep -Seconds 2
  }
  $waited = [int]((Get-Date) - $started).TotalSeconds
  Write-Host "host lock taken after waiting $waited seconds"
  exit 0
}

if ($Action -eq 'release') {
  if (-not (Test-Path -LiteralPath $pidFile)) { Write-Host "no host lock held by this job"; exit 0 }
  Set-Content -LiteralPath $stop -Value 'release'
  $holderPid = [int](Get-Content -LiteralPath $pidFile)
  $proc = Get-Process -Id $holderPid -ErrorAction SilentlyContinue
  if ($proc) { [void]$proc.WaitForExit(30000) }
  Write-Host "host lock released"
  exit 0
}
