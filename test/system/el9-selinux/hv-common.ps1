# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# hv-common.ps1 - shared helpers for the EL9 SELinux rehearsal rig
# (provision-el9-vm.ps1 + run-el9-selinux-rehearsal.ps1). Dot-source it:
#
#     . (Join-Path $PSScriptRoot 'hv-common.ps1')
#
# The ssh/scp helpers are the ones test/system/cpanel/ci-ea4-smoke.ps1 grew
# the hard way, lifted verbatim in shape and extended with an explicit key
# path (the runner may execute under a service account whose ~/.ssh is not
# the operator's). The three landmines they walk around, in short:
#   1. `& ssh -n ...` hangs forever when the PS1 is itself invoked over SSH;
#      Start-Process with -RedirectStandardInput pointing at a REAL empty
#      file detaches the child from the inherited stdin handle.
#   2. Start-Process -RedirectStandardInput 'NUL' resolves NUL as a relative
#      path; hence the real zero-byte temp file.
#   3. PowerShell wraps native stderr as NativeCommandError; under EAP=Stop
#      OpenSSH's "Permanently added ..." notice kills the run. So ssh/scp
#      never go through the call operator; stderr is captured to a file,
#      filtered, and echoed with Write-Host. The helpers return an int exit
#      code and never throw on the child's status.
#
# Everything here is Windows PowerShell 5.1 compatible (no ternaries, no ??).

$script:NullStdinFile = $null
$script:TempOutFiles = New-Object System.Collections.ArrayList

function Initialize-SshHelpers {
    # Create the zero-byte stdin file the Start-Process pattern needs. PID in
    # the name so two concurrent drivers on one host do not fight over it.
    $script:NullStdinFile = [IO.Path]::Combine($env:TEMP, "ssh-null-stdin-$PID.txt")
    Set-Content -LiteralPath $script:NullStdinFile -Value '' -NoNewline -Encoding ASCII
}

function Remove-SshHelpers {
    if ($script:NullStdinFile) {
        Remove-Item -LiteralPath $script:NullStdinFile -Force -ErrorAction SilentlyContinue
    }
    foreach ($f in @($script:TempOutFiles)) {
        if ($f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
    }
}

function Invoke-NativeOverSsh {
    # Run $Exe with $ProcArgs via Start-Process, stdin/stdout/stderr redirected
    # to files. Echoes stdout (and, optionally, appends it to $LogFile);
    # filters the OpenSSH host-key notice out of stderr. Returns the int exit
    # code; -1 when it cannot be known.
    param(
        [Parameter(Mandatory)] [string]$Exe,
        [Parameter(Mandatory)] [string[]]$ProcArgs,
        [string]$Tag = $Exe,
        [string]$LogFile = ''
    )
    if (-not $script:NullStdinFile) { Initialize-SshHelpers }
    $outFile = [IO.Path]::GetTempFileName()
    $errFile = [IO.Path]::GetTempFileName()
    [void]$script:TempOutFiles.Add($outFile)
    [void]$script:TempOutFiles.Add($errFile)
    try {
        # -ErrorAction Stop here is deliberate: a missing ssh.exe/scp.exe is a
        # host problem worth a hard error. The child's stderr goes to $errFile,
        # not through this channel.
        $proc = Start-Process -FilePath $Exe `
                              -ArgumentList $ProcArgs `
                              -RedirectStandardInput  $script:NullStdinFile `
                              -RedirectStandardOutput $outFile `
                              -RedirectStandardError  $errFile `
                              -Wait -NoNewWindow -PassThru `
                              -ErrorAction Stop
        if ($null -eq $proc.ExitCode) { try { $proc.WaitForExit() } catch { } }
        $rc = $proc.ExitCode
        if ($null -eq $rc) { $rc = -1 }

        $stdout = Get-Content -LiteralPath $outFile -Raw -ErrorAction SilentlyContinue
        $stderr = Get-Content -LiteralPath $errFile -Raw -ErrorAction SilentlyContinue
        if ($stdout) {
            $s = $stdout.TrimEnd()
            if ($s) {
                Write-Host $s
                if ($LogFile) { Add-Content -LiteralPath $LogFile -Value $s -Encoding UTF8 }
            }
        }
        if ($stderr) {
            $lines = $stderr -split "`r?`n" | Where-Object {
                $_ -and ($_ -notmatch 'Permanently added .* to the list of known hosts')
            }
            if ($lines.Count -gt 0) {
                $msg = "[$Tag stderr] $($lines -join [Environment]::NewLine)"
                Write-Host $msg
                if ($LogFile) { Add-Content -LiteralPath $LogFile -Value $msg -Encoding UTF8 }
            }
        }
        return [int]$rc
    } finally {
        Remove-Item -LiteralPath $outFile,$errFile -Force -ErrorAction SilentlyContinue
        [void]$script:TempOutFiles.Remove($outFile)
        [void]$script:TempOutFiles.Remove($errFile)
    }
}

function Get-SshCommonArgs {
    param([int]$ConnectTimeoutSec = 10, [string]$KeyPath = '')
    $a = @(
        '-o', 'BatchMode=yes',
        '-o', "ConnectTimeout=$ConnectTimeoutSec",
        '-o', 'StrictHostKeyChecking=accept-new',
        '-o', 'UserKnownHostsFile=NUL'
    )
    if ($KeyPath) { $a += @('-i', (Quote-Arg $KeyPath), '-o', 'IdentitiesOnly=yes') }
    return $a
}

function Quote-Arg {
    # Start-Process joins -ArgumentList with spaces and does not quote; a
    # local path with a space must arrive as one argument.
    param([string]$s)
    if ($s -match '\s') { return '"' + $s + '"' } else { return $s }
}

function Invoke-Ssh {
    # Run a remote command; returns ssh's exit code (the remote command's on
    # success, 255 on an ssh-side failure).
    param(
        [Parameter(Mandatory)] [string]$Ip,
        [Parameter(Mandatory)] [string]$Command,
        [int]$ConnectTimeoutSec = 10,
        [string]$RemoteUser = 'root',
        [string]$KeyPath = '',
        [string]$LogFile = ''
    )
    $argList = @('-n') + (Get-SshCommonArgs -ConnectTimeoutSec $ConnectTimeoutSec -KeyPath $KeyPath) + @("$RemoteUser@$Ip", $Command)
    return Invoke-NativeOverSsh -Exe 'ssh' -ProcArgs $argList -Tag 'ssh' -LogFile $LogFile
}

function Invoke-Scp {
    # Copy files either way (local -> "user@ip:/path" or the reverse).
    # -Recurse adds -r. Returns scp's exit code.
    param(
        [Parameter(Mandatory)] [string]$Source,
        [Parameter(Mandatory)] [string]$Destination,
        [switch]$Recurse,
        [int]$ConnectTimeoutSec = 10,
        [string]$KeyPath = ''
    )
    $argList = @()
    if ($Recurse) { $argList += '-r' }
    $argList += (Get-SshCommonArgs -ConnectTimeoutSec $ConnectTimeoutSec -KeyPath $KeyPath)
    $argList += @((Quote-Arg $Source), (Quote-Arg $Destination))
    return Invoke-NativeOverSsh -Exe 'scp' -ProcArgs $argList -Tag 'scp'
}

function Test-Tcp22 {
    # Bounded TCP/22 probe; Test-NetConnection's dead-host timeout is slow.
    param([string]$Ip, [int]$TimeoutMs = 2000)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect($Ip, 22, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs)) { return $false }
        $client.EndConnect($iar); return $true
    } catch { return $false } finally { $client.Close() }
}

function Get-GuestIp {
    # Discover the guest's IP by MAC from the host neighbor table, liveness-
    # gated on TCP/22. AlmaLinux cloud images ship no hypervkvpd, so
    # (Get-VM).NetworkAdapters.IPAddresses stays empty; the Default Switch's
    # ARP table fills as soon as the guest DHCPs. After a host reboot the
    # switch re-randomizes its /20 and stale Permanent entries linger, hence
    # Reachable/Stale first and only an address that answers on 22.
    param(
        [Parameter(Mandatory)] [string]$Mac,
        [int]$TimeoutSec = 240,
        [string]$InterfaceAlias = '*Default*'
    )
    $macFmt = ($Mac -replace '(..)','$1-').TrimEnd('-')
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $cands = Get-NetNeighbor -InterfaceAlias $InterfaceAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                 Where-Object { $_.LinkLayerAddress -eq $macFmt -and $_.IPAddress -ne '0.0.0.0' } |
                 Sort-Object { switch ([string]$_.State) { 'Reachable' {0} 'Stale' {1} 'Permanent' {2} default {3} } }
        foreach ($c in $cands) {
            if (Test-Tcp22 -Ip $c.IPAddress) { return $c.IPAddress }
        }
        Start-Sleep -Seconds 3
    }
    throw "Timed out after ${TimeoutSec}s waiting for a LIVE guest IP (MAC $macFmt, answering TCP/22) in the host neighbor table for '$InterfaceAlias'"
}

function Wait-Ssh {
    param(
        [Parameter(Mandatory)] [string]$Ip,
        [int]$TimeoutSec = 180,
        [string]$RemoteUser = 'root',
        [string]$KeyPath = ''
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $rc = Invoke-Ssh -Ip $Ip -Command 'exit' -ConnectTimeoutSec 5 -RemoteUser $RemoteUser -KeyPath $KeyPath
        if ($rc -eq 0) { return $true }
        Start-Sleep -Seconds 3
    }
    throw "SSH never came up at $RemoteUser@$Ip within ${TimeoutSec}s (key: $KeyPath)"
}

function Wait-VmState {
    param(
        [Parameter(Mandatory)] [string]$VMName,
        [Parameter(Mandatory)] [string]$State,
        [int]$TimeoutSec = 180
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $vm = Get-VM -Name $VMName -ErrorAction Stop
        if ([string]$vm.State -eq $State) { return $true }
        Start-Sleep -Seconds 3
    }
    return $false
}

function Stop-VmHard {
    param([Parameter(Mandatory)] [string]$VMName)
    Write-Host "Stopping VM '$VMName' (TurnOff)..."
    Stop-VM -Name $VMName -TurnOff -Force -ErrorAction SilentlyContinue
}

function ConvertTo-WslPath {
    # D:\x\y -> /mnt/d/x/y via wslpath (handles any drive and odd characters).
    param([Parameter(Mandatory)] [string]$WindowsPath)
    $p = & wsl.exe -e wslpath -a "$WindowsPath" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $p) { throw "wslpath failed for '$WindowsPath'" }
    return ([string]$p).Trim()
}

function Get-QemuImgRoute {
    # Where can qemu-img run: 'windows' (on PATH), 'wsl' (inside the default
    # WSL distro), or '' (nowhere -- the caller prints the manual step).
    if (Get-Command qemu-img -ErrorAction SilentlyContinue) { return 'windows' }
    if (Get-Command wsl.exe -ErrorAction SilentlyContinue) {
        & wsl.exe -e sh -c 'command -v qemu-img >/dev/null 2>&1' 2>$null
        if ($LASTEXITCODE -eq 0) { return 'wsl' }
    }
    return ''
}

function Convert-QcowToVhdx {
    # qcow2 -> dynamic VHDX with qemu-img, on Windows or through WSL.
    param(
        [Parameter(Mandatory)] [string]$Qcow2Path,
        [Parameter(Mandatory)] [string]$VhdxPath,
        [string]$Route = ''
    )
    if (-not $Route) { $Route = Get-QemuImgRoute }
    switch ($Route) {
        'windows' {
            & qemu-img convert -p -f qcow2 -O vhdx -o subformat=dynamic "$Qcow2Path" "$VhdxPath"
            if ($LASTEXITCODE -ne 0) { throw "qemu-img convert failed (exit $LASTEXITCODE)" }
        }
        'wsl' {
            $src = ConvertTo-WslPath $Qcow2Path
            $dst = ConvertTo-WslPath $VhdxPath
            & wsl.exe -e qemu-img convert -p -f qcow2 -O vhdx -o subformat=dynamic "$src" "$dst"
            if ($LASTEXITCODE -ne 0) { throw "qemu-img convert (via WSL) failed (exit $LASTEXITCODE)" }
        }
        default {
            throw @"
qemu-img is not available on this host (neither on PATH nor inside WSL).
Convert the cloud image once, by hand, then re-run with -ImageVhdx:
  WSL:      sudo apt-get install -y qemu-utils
            qemu-img convert -p -f qcow2 -O vhdx -o subformat=dynamic <image>.qcow2 <image>.vhdx
  Windows:  winget install SoftwareFreedomConservancy.QEMU   (adds qemu-img.exe)
"@
        }
    }
}

function Get-IsoToolRoute {
    # 'wsl-genisoimage' when genisoimage exists in the default WSL distro (the
    # tool the existing EL seeds on the host were built with: Joliet + Rock
    # Ridge, exact file names), else 'imapi2' (built into Windows).
    if (Get-Command wsl.exe -ErrorAction SilentlyContinue) {
        & wsl.exe -e sh -c 'command -v genisoimage >/dev/null 2>&1' 2>$null
        if ($LASTEXITCODE -eq 0) { return 'wsl-genisoimage' }
    }
    return 'imapi2'
}

function New-CloudInitSeedIsoWsl {
    # genisoimage inside WSL: -joliet -rock so the guest sees the exact names.
    param(
        [Parameter(Mandatory)] [string]$SourceDir,
        [Parameter(Mandatory)] [string]$OutputPath,
        [string]$VolumeLabel = 'cidata'
    )
    $files = @(Get-ChildItem -LiteralPath $SourceDir -File)
    if ($files.Count -eq 0) { throw "New-CloudInitSeedIsoWsl: no files in $SourceDir" }
    # wslpath needs the directory to exist; the ISO itself does not yet.
    $outDir = ConvertTo-WslPath (Split-Path -Parent $OutputPath)
    $out = "$outDir/$(Split-Path -Leaf $OutputPath)"
    $wslFiles = @($files | ForEach-Object { ConvertTo-WslPath $_.FullName })
    & wsl.exe -e genisoimage -quiet -output "$out" -volid $VolumeLabel -joliet -rock -input-charset utf-8 @wslFiles
    if ($LASTEXITCODE -ne 0) { throw "genisoimage (via WSL) failed (exit $LASTEXITCODE)" }
    if (-not (Test-Path -LiteralPath $OutputPath)) { throw "New-CloudInitSeedIsoWsl: $OutputPath was not written" }
}

function New-CloudInitSeedIso {
    # Build a NoCloud seed ISO (volume label 'cidata') from the files in
    # $SourceDir (user-data, meta-data, optional network-config) with the
    # IMAPI2 COM API that ships with Windows -- no oscdimg/mkisofs needed.
    # ISO9660 + Joliet (IMAPI2 writes no Rock Ridge and stores Joliet names
    # with a ';1' version suffix; Linux isofs strips that suffix, so the guest
    # sees 'user-data'). Preferred when available: New-CloudInitSeedIsoWsl.
    param(
        [Parameter(Mandatory)] [string]$SourceDir,
        [Parameter(Mandatory)] [string]$OutputPath,
        [string]$VolumeLabel = 'cidata'
    )
    if (-not ('El9Rehearsal.IsoWriter' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
namespace El9Rehearsal {
  public static class IsoWriter {
    public static void Write(string path, object comStream) {
      IStream stream = (IStream)comStream;
      using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write)) {
        byte[] buf = new byte[1024 * 1024];
        IntPtr pRead = Marshal.AllocHGlobal(4);
        try {
          while (true) {
            stream.Read(buf, buf.Length, pRead);
            int n = Marshal.ReadInt32(pRead);
            if (n <= 0) break;
            fs.Write(buf, 0, n);
          }
        } finally { Marshal.FreeHGlobal(pRead); }
      }
    }
  }
}
'@
    }
    $files = @(Get-ChildItem -LiteralPath $SourceDir -File)
    if ($files.Count -eq 0) { throw "New-CloudInitSeedIso: no files in $SourceDir" }
    $fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
    $fsi.FileSystemsToCreate = 3          # FsiFileSystemISO9660 (1) + FsiFileSystemJoliet (2)
    $fsi.VolumeName = $VolumeLabel
    foreach ($f in $files) {
        $stream = New-Object -ComObject ADODB.Stream
        $stream.Type = 1                  # adTypeBinary
        $stream.Open()
        $stream.LoadFromFile($f.FullName)
        $fsi.Root.AddFile($f.Name, $stream)
        $stream.Close()
    }
    $result = $fsi.CreateResultImage()
    [El9Rehearsal.IsoWriter]::Write($OutputPath, $result.ImageStream)
    if (-not (Test-Path -LiteralPath $OutputPath)) { throw "New-CloudInitSeedIso: $OutputPath was not written" }
}

function Write-Utf8NoBomLf {
    # cloud-init wants '#cloud-config' as the very first bytes: no BOM (PS 5.1
    # Set-Content -Encoding UTF8 writes one) and LF line endings.
    param([Parameter(Mandatory)] [string]$Path, [Parameter(Mandatory)] [string]$Text)
    $lf = $Text -replace "`r`n", "`n"
    [IO.File]::WriteAllText($Path, $lf, (New-Object System.Text.UTF8Encoding($false)))
}
