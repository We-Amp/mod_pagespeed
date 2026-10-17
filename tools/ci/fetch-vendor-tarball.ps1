# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# fetch-vendor-tarball.ps1 - stage, verify and extract the vendor tarball on a
# Windows runner without ever reading a file another job may be writing,
# deleting or overwriting.
#
# The tarball lives in a directory that Linux jobs of the same machine
# publish into and prune (reached here through a drive mount), or on the CI
# hub. A reader must therefore never extract straight from that directory.
# This script instead:
#   1. copies the tarball (and its .sha256 sidecar) into a private staging
#      directory under a name unique to this job, from the first source that
#      has it: the shared directories in order, then the hub;
#   2. verifies the private copy against the sidecar digest (and non-empty,
#      and `zstd -t`); a copy that is absent, short or fails the digest is
#      discarded and the whole pass is retried after a short, doubling
#      backoff, because the producer publishes the sidecar just after the
#      tarball and a copy can race a prune;
#   3. decompresses and extracts only the verified private copy, with a
#      decompression intermediate that is also private to this job;
#   4. removes everything it staged, success or not.
# A tarball with no sidecar is accepted only on the last pass (an older run
# predates the sidecar), after `zstd -t`.
#
# Exit codes: 0 extracted; 4 no source has the tarball (the caller decides:
# Bazel can fetch dependencies itself); 1 a tarball exists but never
# verified, or extraction failed (loud).
#
# Usage: fetch-vendor-tarball.ps1 -Name <file> -Dest <dir> -StageDir <dir>
#          [-SearchDirs <dir>[,<dir>...]] [-HubSsh <user@host> -HubDir <dir>]
#          [-Retries 4] [-BackoffSeconds 3] [-Tag <unique>] [-TarExe <path>]
param(
  [Parameter(Mandatory = $true)][string]$Name,
  [Parameter(Mandatory = $true)][string]$Dest,
  [Parameter(Mandatory = $true)][string]$StageDir,
  [string[]]$SearchDirs = @(),
  [string]$HubSsh = '',
  [string]$HubDir = '',
  [int]$Retries = 4,
  [int]$BackoffSeconds = 3,
  [string]$Tag = '',
  [string]$TarExe = ''
)
$ErrorActionPreference = 'Stop'

if (-not $Tag) {
  $Tag = "$($env:GITHUB_RUN_ID)-$($env:GITHUB_RUN_ATTEMPT)-$PID"
}
if (-not $TarExe) { $TarExe = Join-Path $env:SystemRoot 'System32\tar.exe' }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
$staged = Join-Path $StageDir "$Name.$Tag.stage"
$sidecarStaged = "$staged.sha256"
$tarFile = Join-Path $StageDir "$Name.$Tag.tar"

function Remove-Quiet {
  param([string[]]$Paths)
  foreach ($p in $Paths) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue }
}

# Copy-From-Source: put a private copy of the tarball and sidecar at $staged.
# Returns $true when a tarball was copied.
function Copy-FromSource {
  param([string]$Dir)
  $src = Join-Path $Dir $Name
  if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { return $false }
  try {
    Copy-Item -LiteralPath $src -Destination $staged -Force -ErrorAction Stop
    # Copy-Item keeps the source's LastWriteTime; a private copy must look new
    # so a sibling's prune does not take it.
    (Get-Item -LiteralPath $staged).LastWriteTime = Get-Date
    if (Test-Path -LiteralPath "$src.sha256" -PathType Leaf) {
      Copy-Item -LiteralPath "$src.sha256" -Destination $sidecarStaged -Force -ErrorAction Stop
      (Get-Item -LiteralPath $sidecarStaged).LastWriteTime = Get-Date
    }
    return $true
  } catch {
    Write-Host "::warning::fetch-vendor-tarball: copy from a shared directory failed: $($_.Exception.Message)"
    return $false
  }
}

function Copy-FromHub {
  # Windows PowerShell 5.1 turns a native command's stderr into a terminating
  # error under 'Stop' once it is redirected; the exit code is what counts.
  $ErrorActionPreference = 'Continue'
  if (-not $HubSsh -or -not $HubDir) { return $false }
  $opts = @('-o', 'StrictHostKeyChecking=accept-new', '-o', 'BatchMode=yes', '-o', 'LogLevel=ERROR')
  & scp @opts "${HubSsh}:${HubDir}/$Name" $staged 2>$null
  if ($LASTEXITCODE -ne 0) { Remove-Quiet @($staged); return $false }
  & scp @opts "${HubSsh}:${HubDir}/$Name.sha256" $sidecarStaged 2>$null
  if ($LASTEXITCODE -ne 0) { Remove-Quiet @($sidecarStaged) }
  return $true
}

# Test-Staged: $true when the private copy is whole. $AllowNoSidecar admits a
# sound tarball that has no digest.
function Test-Staged {
  param([bool]$AllowNoSidecar)
  $ErrorActionPreference = 'Continue'
  if (-not (Test-Path -LiteralPath $staged -PathType Leaf)) { return $false }
  if ((Get-Item -LiteralPath $staged).Length -le 0) {
    Write-Host '::warning::fetch-vendor-tarball: staged tarball is empty'
    return $false
  }
  & zstd -t -q $staged 2>$null
  if ($LASTEXITCODE -ne 0) {
    Write-Host '::warning::fetch-vendor-tarball: staged tarball is not a complete zstd frame (short or mid-publish)'
    return $false
  }
  $want = ''
  if (Test-Path -LiteralPath $sidecarStaged -PathType Leaf) {
    $line = Get-Content -LiteralPath $sidecarStaged -TotalCount 1 -ErrorAction SilentlyContinue
    if ($line) { $want = ([string]$line).Trim().Split(' ')[0].ToLowerInvariant() }
  }
  if (-not $want) {
    if ($AllowNoSidecar) {
      Write-Host '::warning::fetch-vendor-tarball: no sidecar digest; accepting on zstd -t alone'
      return $true
    }
    Write-Host '::warning::fetch-vendor-tarball: no sidecar digest yet (the producer may be mid-publish)'
    return $false
  }
  $got = (Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($got -ne $want) {
    Write-Host "::warning::fetch-vendor-tarball: sha256 mismatch (want $($want.Substring(0, 12)).. got $($got.Substring(0, 12)).."
    return $false
  }
  return $true
}

$verified = $false
$seen = $false
$backoff = $BackoffSeconds
try {
  for ($attempt = 1; $attempt -le $Retries -and -not $verified; $attempt++) {
    $last = ($attempt -eq $Retries)
    $sources = @()
    foreach ($d in $SearchDirs) { if ($d) { $sources += ,@('dir', $d) } }
    if ($HubSsh -and $HubDir) { $sources += ,@('hub', '') }
    foreach ($s in $sources) {
      Remove-Quiet @($staged, $sidecarStaged)
      if ($s[0] -eq 'dir') { $got = Copy-FromSource $s[1] } else { $got = Copy-FromHub }
      if (-not $got) { continue }
      $seen = $true
      if (Test-Staged $last) { $verified = $true; break }
      Write-Host "fetch-vendor-tarball: $Name from a $($s[0]) source did not verify (pass $attempt/$Retries)"
    }
    if (-not $verified -and -not $last) {
      if ($seen) { Start-Sleep -Seconds $backoff; $backoff = $backoff * 2 }
      else { Start-Sleep -Seconds ([Math]::Min($backoff, 1)) }
    }
  }

  if (-not $verified) {
    if (-not $seen) {
      Write-Host "fetch-vendor-tarball: $Name is not reachable from this runner"
      exit 4
    }
    Write-Host "::error::[vendor-tarball-unavailable] $Name was found but never verified after $Retries passes (short, or its digest did not match). Re-run the full workflow, not only the failed jobs, to regenerate it."
    exit 1
  }

  Write-Host "extracting $Name (verified private copy)"
  # Decompress with all cores, then extract with the Windows tar (the MSYS2
  # GNU tar reads a drive letter as a remote host).
  & zstd -d -f -q -T0 $staged -o $tarFile
  if ($LASTEXITCODE -ne 0) { Write-Host "::error::zstd decompression failed (exit $LASTEXITCODE)"; exit 1 }
  # zstd -d keeps the source's LastWriteTime too; stamp the intermediate.
  (Get-Item -LiteralPath $tarFile).LastWriteTime = Get-Date
  New-Item -ItemType Directory -Force -Path $Dest | Out-Null
  & $TarExe xf $tarFile -C $Dest
  if ($LASTEXITCODE -ne 0) { Write-Host "::error::tar extraction failed (exit $LASTEXITCODE)"; exit 1 }
  exit 0
} finally {
  Remove-Quiet @($staged, $sidecarStaged, $tarFile)
}
