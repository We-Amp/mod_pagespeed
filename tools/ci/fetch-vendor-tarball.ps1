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
# The extraction never writes over an existing file: the destination is
# emptied first (every top-level entry except .git; links are removed as
# links and never entered, the read-only attribute is cleared before a
# delete). Extracting over the files the checkout had just written is
# what made the Windows ASan job die on "Refusing to overwrite archive:
# Input/output error" (issue #231). That error is libarchive's same-file
# check (archive_write_disk_windows.c): before writing a member it stats
# the existing destination file and refuses when that file's (volume
# serial, file index) identity equals the archive's own. Windows file
# indexes are hashed or truncated there, so unrelated files can compare
# equal; the job log shows the workspace had just been cleaned, so
# nothing was locked or read-only. An emptied target has no existing
# file to stat, so the check cannot fire. The tarball carries the
# complete tree of the same commit, so clearing loses nothing. Whatever
# resists the clear, or the file tar still cannot write, is named in the
# failure message with its existence, read-only attribute and lock
# state.
#
# The clear is the one destructive act here, so its target is tied down
# before anything is downloaded: an absolute path equal to
# $env:GITHUB_WORKSPACE (full paths, no trailing separator, compared
# case-insensitively). A drive-relative path (C:), a relative path and a
# staging directory inside the target are refused outright;
# -AllowTargetOutsideWorkspaceForTest lifts only the workspace equality,
# for the self-test's temporary targets.
#
# Exit codes: 0 extracted; 4 no source has the tarball, or zstd is not on
# PATH so the tarball channel is unusable (the caller decides: Bazel can
# fetch dependencies itself); 1 a tarball exists but never verified, or
# the destination could not be cleared, or extraction failed (loud).
#
# Usage: fetch-vendor-tarball.ps1 -Name <file> -Dest <dir> -StageDir <dir>
#          [-SearchDirs <dir>[,<dir>...]] [-HubSsh <user@host> -HubDir <dir>]
#          [-Retries 4] [-BackoffSeconds 3] [-Tag <unique>] [-TarExe <path>]
#          [-AllowTargetOutsideWorkspaceForTest]  # self-test only
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
  [string]$TarExe = '',
  [switch]$AllowTargetOutsideWorkspaceForTest
)
$ErrorActionPreference = 'Stop'

if (-not $Tag) {
  $Tag = "$($env:GITHUB_RUN_ID)-$($env:GITHUB_RUN_ATTEMPT)-$PID"
}
if (-not $TarExe) { $TarExe = Join-Path $env:SystemRoot 'System32\tar.exe' }
# Without zstd there is nothing this script can do with a tarball (the
# integrity check and the decompression both need it): say so with the
# exit code that lets the caller fall back to Bazel fetching dependencies.
if (-not (Get-Command zstd -ErrorAction SilentlyContinue)) {
  Write-Host "fetch-vendor-tarball: zstd is not on PATH; this runner cannot use a vendor tarball"
  exit 4
}

# Assert-ClearTarget: the clear empties a directory, so its target is
# tied down before anything is downloaded or created: an absolute path
# equal to $env:GITHUB_WORKSPACE (full paths, no trailing separator,
# compared case-insensitively). A drive-relative path (C:) and a
# relative path both resolve against a current directory the caller did
# not choose, and a staging directory inside the target would have its
# staged files deleted by the clear itself. Refusals are loud and exit
# 1; -AllowTargetOutsideWorkspaceForTest (self-test only) lifts the
# workspace equality, never the other checks.
function Assert-ClearTarget([string]$Target, [string]$Stage) {
  if ($Target -cmatch '^[A-Za-z]:(?!\\|/)') {
    throw "a drive-relative target ($Target) names the current directory on that drive, not a directory"
  }
  if (-not [IO.Path]::IsPathRooted($Target)) {
    throw "a relative target ($Target) resolves against a current directory the caller did not choose"
  }
  $full = [IO.Path]::GetFullPath($Target).TrimEnd('\', '/')
  if ($full -ieq [IO.Path]::GetPathRoot($full).TrimEnd('\', '/')) {
    throw "refusing to clear a filesystem root: $full"
  }
  if (-not $AllowTargetOutsideWorkspaceForTest) {
    $ws = ''
    if ($env:GITHUB_WORKSPACE) {
      $ws = [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE).TrimEnd('\', '/')
    }
    if (-not $ws) {
      throw 'GITHUB_WORKSPACE is not set, so no target can be proven to be the workspace'
    }
    if ($full -ine $ws) {
      throw "refusing to clear ${full}: it is not the workspace $ws"
    }
  }
  $stageFull = [IO.Path]::GetFullPath($Stage).TrimEnd('\', '/')
  if ($stageFull -ieq $full -or $stageFull -like "$full\*" -or $stageFull -like "$full/*") {
    throw "the staging directory $stageFull sits inside the target $full; the clear would delete what it stages"
  }
}
try { Assert-ClearTarget $Dest $StageDir } catch {
  Write-Host "::error::refusing to clear the extraction target: $($_.Exception.Message)"
  exit 1
}

New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
$staged = Join-Path $StageDir "$Name.$Tag.stage"
$sidecarStaged = "$staged.sha256"
$tarFile = Join-Path $StageDir "$Name.$Tag.tar"
$tarErr = Join-Path $StageDir "$Name.$Tag.tar-err.txt"

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

# --- clear the destination before extraction ---------------------------------
# Windows PowerShell 5.1's Remove-Item -Recurse enters directory links, so
# the walks below use .NET listings and never descend into a reparse point
# (the same discipline as tools/ci/Remove-StaleToolRunfiles.ps1).
$reparse = [IO.FileAttributes]::ReparsePoint
$dirFlag = [IO.FileAttributes]::Directory
$roFlag = [IO.FileAttributes]::ReadOnly
$script:stuck = @()

function Test-IsLink([string]$Path) {
  try { return (([IO.File]::GetAttributes($Path) -band $reparse) -ne 0) }
  catch { return $false }
}

function Test-IsDir([string]$Path) {
  return (([IO.File]::GetAttributes($Path) -band $dirFlag) -ne 0)
}

# Get-TopEntries: the entries of $Dir to delete: everything except .git (no
# tarball carries .git, and the checkout's post step and git expect it).
function Get-TopEntries([string]$Dir) {
  return @([IO.Directory]::GetFileSystemEntries($Dir) |
    Where-Object { (Split-Path -Leaf $_) -ine '.git' })
}

# Remove-Link: delete a junction or symlink itself, never its target.
function Remove-Link([string]$Path) {
  if (Test-IsDir $Path) { [IO.Directory]::Delete($Path) }
  else { [IO.File]::Delete($Path) }
}

# Remove-FileCareful: delete one file, clearing the read-only attribute
# first. A failure is recorded in $script:stuck, not raised: the caller
# re-examines what is left and reports it with the reason.
function Remove-FileCareful([string]$Path) {
  try {
    $attr = [IO.File]::GetAttributes($Path)
    if (($attr -band $roFlag) -ne 0) {
      [IO.File]::SetAttributes($Path, ($attr -band (-bnot $roFlag)))
    }
    [IO.File]::Delete($Path)
  } catch { $script:stuck += $Path }
}

# Remove-TreeCareful: delete a directory tree bottom-up (the fallback for
# whatever the fast rmdir pass leaves behind), clearing read-only
# attributes and removing links as links.
function Remove-TreeCareful([string]$Path) {
  foreach ($e in [IO.Directory]::GetFileSystemEntries($Path)) {
    if (Test-IsLink $e) {
      try { Remove-Link $e } catch { $script:stuck += $e }
    } elseif (Test-IsDir $e) {
      Remove-TreeCareful $e
    } else {
      Remove-FileCareful $e
    }
  }
  try { [IO.Directory]::Delete($Path) } catch { $script:stuck += $Path }
}

# Describe-Path: why a path could not be written or deleted: existence, the
# read-only attribute and, for a file, whether it is locked (an exclusive
# open fails). Part of the failure message of issue #231.
function Describe-Path([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) { return 'gone' }
  try { $attr = [IO.File]::GetAttributes($Path) }
  catch { return "present, attributes unknown ($($_.Exception.GetType().Name))" }
  if (($attr -band $dirFlag) -ne 0) { return 'exists (directory)' }
  $desc = 'exists (file), read-only: ' + ((($attr -band $roFlag) -ne 0))
  $locked = 'no'
  try {
    $h = [IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
    $h.Close()
  } catch { $locked = 'yes (' + $_.Exception.GetType().Name + ')' }
  return "$desc, locked: $locked"
}

# Clear-Dest: empty $Dir except .git, so the extraction cannot hit a file an
# earlier job left. Throws when entries survive every attempt; a scanner's
# or a crashed job's handle is usually transient, hence the retries.
function Clear-Dest([string]$Dir) {
  $full = [IO.Path]::GetFullPath($Dir).TrimEnd('\', '/')
  if ($full -ieq [IO.Path]::GetPathRoot($full).TrimEnd('\', '/')) {
    throw "refusing to clear a filesystem root: $full"
  }
  if (-not (Test-Path -LiteralPath $full -PathType Container)) { return }
  if (Test-IsLink $full) { throw "refusing to clear a link: $full" }
  for ($attempt = 1; $attempt -le 3; $attempt++) {
    foreach ($e in (Get-TopEntries $full)) {
      if (Test-IsLink $e) {
        try { Remove-Link $e } catch { $script:stuck += $e }
      } elseif (Test-IsDir $e) {
        if ($env:ComSpec) {
          # rmdir /s /q is fast and never enters a junction.
          & $env:ComSpec /c "rmdir /s /q `"$e`""
        } else {
          Remove-TreeCareful $e
        }
      } else {
        Remove-FileCareful $e
      }
    }
    # Whatever the rmdir pass left behind gets the careful walk.
    $script:stuck = @()
    foreach ($e in (Get-TopEntries $full)) {
      if (Test-IsLink $e) { continue }
      if (Test-IsDir $e) { Remove-TreeCareful $e } else { Remove-FileCareful $e }
    }
    if ((Get-TopEntries $full).Count -eq 0) { return }
    if ($attempt -lt 3) { Start-Sleep -Seconds (2 * $attempt) }
  }
  $survivors = @(@($script:stuck) + @(Get-TopEntries $full) | Select-Object -Unique)
  Write-Host 'paths that resisted every delete attempt:'
  foreach ($e in $survivors) { Write-Host "  $e ($(Describe-Path $e))" }
  throw "$($survivors.Count) path(s) under $full could not be removed (locked by another process?); nothing was extracted"
}

# Show-TarFailure: echo tar's own stderr and, for the first archive member
# it names, say whether that path exists, is read-only or is locked.
function Show-TarFailure([string]$ErrFile, [string]$Dest) {
  $lines = @()
  if (Test-Path -LiteralPath $ErrFile -PathType Leaf) {
    $lines = @(Get-Content -LiteralPath $ErrFile -ErrorAction SilentlyContinue)
  }
  foreach ($l in $lines) { if ($l) { Write-Host "tar: $l" } }
  foreach ($l in $lines) {
    # A member line looks like './tools/ci/x.ps1: Refusing to overwrite...'.
    # cmd.exe wrote those lines plain; a caller that still routes tar
    # through PowerShell's own 2> has them decorated as
    # 'tar.exe : ./tools/...', so the member may start mid-line.
    if ($l -match '(?:^|\s)(\.[/\\][^:]+):\s') {
      $abs = Join-Path $Dest ($Matches[1] -replace '^\.[/\\]', '')
      $state = Describe-Path $abs
      if ($l -match 'Refusing to overwrite archive') {
        # libarchive's same-file check: the existing destination file
        # matched the archive's (volume serial, file index) identity, a
        # false match the truncated Windows file index allows (issue
        # #231). An emptied target has no file to stat, so no member can
        # hit it; one that does means the clear missed this path.
        Write-Host ("::error::tar refused to write $abs ($state): libarchive's" +
          " same-file check matched this existing file to the archive itself" +
          " (a Windows file-index false match); an emptied target avoids the check")
      } else {
        Write-Host "::error::tar could not write $abs ($state)"
      }
      break
    }
  }
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
  # The target is emptied first, so no destination file exists for
  # libarchive's same-file check to trip over (issue #231). .git survives
  # the clear.
  try {
    Clear-Dest $Dest
  } catch {
    Write-Host "::error::could not clear the extraction target ${Dest}: $($_.Exception.Message)"
    exit 1
  }
  # tar's stderr goes to a file: Windows PowerShell 5.1 turns redirected
  # native stderr into a terminating error under 'Stop', and the exit code
  # is what counts (see Copy-FromHub above). On Windows cmd.exe runs tar
  # and does the redirection itself: PowerShell's own 2> decorates every
  # native stderr line as an ErrorRecord and wraps long ones at the
  # host's console width, which hid the member name the diagnostic below
  # reports. Without cmd.exe (the hosts the self-test runs on off
  # Windows) the plain redirect writes tar's lines exactly.
  $ErrorActionPreference = 'Continue'
  if ($env:ComSpec) {
    & $env:ComSpec /c "`"$TarExe`" xf `"$tarFile`" -C `"$Dest`" 2>`"$tarErr`""
  } else {
    & $TarExe xf $tarFile -C $Dest 2> $tarErr
  }
  $tarRc = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($tarRc -ne 0) {
    Show-TarFailure $tarErr $Dest
    Write-Host "::error::tar extraction failed (exit $tarRc); the target was emptied before extraction, so see tar's own error above"
    exit 1
  }
  exit 0
} finally {
  Remove-Quiet @($staged, $sidecarStaged, $tarFile, $tarErr)
}
