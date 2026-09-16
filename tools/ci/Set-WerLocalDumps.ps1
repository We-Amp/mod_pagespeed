# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Arm (or remove) a WER LocalDumps override for a process, on whichever fixed
    drive of this runner actually has room for the dumps.

.DESCRIPTION
    Application Verifier does not dump the process it stops: it fail-fasts it
    (STATUS_FAIL_FAST_EXCEPTION 0xc0000421) after raising the stop as a
    BREAKPOINT (0x80000003). Two consequences shape this script:

      * procdump is the wrong tool here. `procdump -e 1 -w w3wp.exe` misses the
        stop twice over: `-e 1` catches first-chance EXCEPTIONS but not
        breakpoints (that needs `-b`), and `-w` attaches to the FIRST w3wp
        instance only, so under app-pool recycle churn nearly every crashing
        worker is a later, unmonitored instance. Measured on this lane:
        8 w3wp faults, zero dumps captured.

      * WER LocalDumps is consulted by WER per-crash, out of the registry, so
        EVERY w3wp instance is covered with no attach and no race. This is the
        mechanism setup_iis_asan_rig.ps1 has used for the same reason, and
        the remedy the AppVerif lane needs: arm WER LocalDumps for w3wp.exe
        in the win-appverif job's arm step (registry key + capped DumpFolder).

    DRIVE SELECTION. A full w3wp dump under page heap runs 1-2 GB, which is not
    a safe assumption on any particular drive: the Windows CI runners do not
    agree on which one has room, and a CI drive has been filled to zero here
    before, taking Windows CI down with it. So the dump directory is DERIVED
    rather than hardcoded: the fixed (DriveType=3) local drive with the most
    free space, provided it clears -MinFreeGB. This also survives re-imaging and
    new runners. Set PAGESPEED_DUMP_ROOT in a runner's environment to pin it.

    LIFECYCLE. -Disable removes the key again, for the reason
    cleanup_iis_asan_rig.ps1 gives: left armed it would silently full-dump every
    future w3wp crash on a shared runner. There is a second reason specific to
    this repo -- per Assert-NoWorkerFailFast.ps1, WER writes a LocalDumps dump
    INSTEAD of a ReportArchive entry, so leaving the key armed would blind the
    archive-based probe of every other job on the box.

    COEXISTENCE. The original arming assumed "dedicated CI runners; nothing
    else configures
    w3wp LocalDumps on them". Once both the AppVerif lane and the ASan rig arm
    this key, that assumption is false, so both must tolerate the other:
      * Arming is idempotent (Set-ItemProperty over an existing key).
      * -Disable never fails when the key is already gone.
      * Neither deletes the other's dumps: -Purge is timestamp-scoped to the
        current run, and dumps predating it are left alone.
    The two workflows are still scheduled disjointly, so in practice only one
    arms at a time; this just makes an overlap non-destructive instead of
    silently cross-contaminating evidence.

.EXAMPLE
    Set-WerLocalDumps.ps1 -Enable -GitHubEnv
    Set-WerLocalDumps.ps1 -Disable -Purge -Since $t0
#>
[CmdletBinding()]
param(
    # Arm the override. Prints the resolved directory on stdout.
    [switch]$Enable,

    # Remove the override.
    [switch]$Disable,

    [string]$ProcessName = 'w3wp.exe',

    # Explicit dump directory. Empty = derive (see DESCRIPTION).
    [string]$DumpDir = '',

    # Max dumps WER keeps in the folder; beyond this it replaces the oldest.
    # 3 x ~2 GB caps the folder near ~6 GB even if cleanup never runs.
    [int]$DumpCount = 3,

    # A drive must have at least this much free to be eligible.
    [int]$MinFreeGB = 10,

    # -Disable only: also delete dumps written at/after -Since (this run's).
    # Dumps OLDER than -Since are left alone: they are someone else's evidence,
    # and DumpCount already bounds the folder.
    [switch]$Purge,
    [DateTime]$Since = [DateTime]::MinValue,

    # Publish PAGESPEED_DUMP_DIR to $env:GITHUB_ENV for later steps.
    [switch]$GitHubEnv
)

$ErrorActionPreference = 'Stop'

if (-not ($Enable -or $Disable)) { throw 'pass -Enable or -Disable' }
if ($Enable -and $Disable) { throw '-Enable and -Disable are mutually exclusive' }

$werKey = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$ProcessName"

function Resolve-DumpDir {
    param([int]$MinFreeGB)

    if ($env:PAGESPEED_DUMP_ROOT) {
        Write-Host "dump root pinned by PAGESPEED_DUMP_ROOT=$env:PAGESPEED_DUMP_ROOT"
        return (Join-Path $env:PAGESPEED_DUMP_ROOT 'localdumps')
    }

    $disks = @(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' -ErrorAction SilentlyContinue |
            Sort-Object -Property FreeSpace -Descending)
    if ($disks.Count -gt 0) {
        Write-Host 'fixed drives by free space:'
        foreach ($d in $disks) {
            Write-Host ('  {0} {1,8:N1} GB free of {2,8:N1} GB' -f `
                    $d.DeviceID, ($d.FreeSpace / 1GB), ($d.Size / 1GB))
        }
    }
    $best = @($disks | Where-Object { $_.FreeSpace -ge ($MinFreeGB * 1GB) })[0]
    if ($best) {
        Write-Host "selected $($best.DeviceID) ($([math]::Round($best.FreeSpace / 1GB, 1)) GB free)"
        return (Join-Path "$($best.DeviceID)\dumps" 'localdumps')
    }

    Write-Warning ("no fixed drive has ${MinFreeGB} GB free; falling back to C:\dumps\localdumps. " +
        'A full w3wp dump under page heap runs 1-2 GB, so capture may be truncated or fail.')
    return 'C:\dumps\localdumps'
}

if ($Enable) {
    $dir = if ($DumpDir) { $DumpDir } else { Resolve-DumpDir -MinFreeGB $MinFreeGB }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null

    New-Item -Path $werKey -Force | Out-Null
    Set-ItemProperty -Path $werKey -Name DumpFolder -Value $dir -Type ExpandString
    Set-ItemProperty -Path $werKey -Name DumpType   -Value 2 -Type DWord   # 2 = full: heap needed for !avrf
    Set-ItemProperty -Path $werKey -Name DumpCount  -Value $DumpCount -Type DWord

    # Pre-existing dumps are NOT deleted here -- they may be evidence someone is
    # still working on. Callers attribute this run's dumps by timestamp instead
    # (the same anchoring idea as APPVERIF_T0), and -Disable -Purge -Since
    # removes only what this run produced.
    $stale = @(Get-ChildItem "$dir\*.dmp" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) {
        $mb = [math]::Round((($stale | Measure-Object -Property Length -Sum).Sum / 1MB))
        Write-Host "note: $($stale.Count) pre-existing dump(s) in $dir (${mb} MB); left in place, not attributed to this run"
    }

    Write-Host "WER LocalDumps armed for ${ProcessName} -> $dir (DumpType=2 full, DumpCount=$DumpCount)"
    if ($GitHubEnv -and $env:GITHUB_ENV) {
        # MUST use `>>`, matching every other GITHUB_ENV write in these
        # workflows. Windows PowerShell 5.1's `>>` is Out-File -Append with the
        # DEFAULT encoding, i.e. UTF-16LE; `Out-File -Encoding utf8` writes
        # UTF-8 *with BOM*. Mixing the two in one GITHUB_ENV file makes the
        # runner fail with "Unable to process file command 'env' successfully /
        # Invalid format" -- it locks onto the first write's encoding and reads
        # the rest as garbage. Whoever writes first wins, so everyone must agree.
        "PAGESPEED_DUMP_DIR=$dir" >> $env:GITHUB_ENV
    }
    Write-Output $dir
    exit 0
}

# -- Disable -----------------------------------------------------------------
$dir = $DumpDir
if (-not $dir -and (Test-Path $werKey)) {
    $dir = (Get-ItemProperty -Path $werKey -Name DumpFolder -ErrorAction SilentlyContinue).DumpFolder
}

if (Test-Path $werKey) {
    Remove-Item $werKey -Force -Recurse -ErrorAction SilentlyContinue
    $state = if (Test-Path $werKey) { 'FAILED' } else { 'ok' }
    Write-Host "WER LocalDumps override for ${ProcessName} removed: $state"
} else {
    Write-Host "WER LocalDumps override for ${ProcessName} not present; nothing to remove"
}

if ($Purge -and $dir -and (Test-Path $dir)) {
    $mine = @(Get-ChildItem "$dir\*.dmp" -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $Since })
    if ($mine.Count -gt 0) {
        $mb = [math]::Round((($mine | Measure-Object -Property Length -Sum).Sum / 1MB))
        # Already staged + uploaded by Collect-CrashDumps.ps1 at this point.
        # Keeping them would let CI fill the drive, which has taken Windows CI
        # out of service here before.
        $mine | Remove-Item -Force -ErrorAction SilentlyContinue
        Write-Host "purged $($mine.Count) dump(s) from this run out of $dir (${mb} MB reclaimed)"
    } else {
        Write-Host "no dumps from this run to purge in $dir"
    }
    $left = @(Get-ChildItem "$dir\*.dmp" -ErrorAction SilentlyContinue)
    if ($left.Count -gt 0) {
        Write-Host "note: $($left.Count) older dump(s) left in $dir (predate this run)"
    }
}
exit 0
