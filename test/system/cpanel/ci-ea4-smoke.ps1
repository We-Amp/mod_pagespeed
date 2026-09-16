# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# ci-ea4-smoke.ps1 - Hyper-V VM EA4/cPanel smoke test for CI.
#
# Drives one matrix cell of the ea4-cpanel-smoke job:
#   1. Revert the VM to its cPanel-clean checkpoint.
#   2. Start the VM and discover its NAT IP via host ARP-by-MAC.
#   3. SCP the candidate ea-apache24-mod_pagespeed RPM + the target-side
#      smoke script into the guest.
#   4. SSH in and run the smoke script. It exercises:
#        dnf install <rpm> -> /scripts/restartsrv_httpd -> curl + header
#        assertion -> dnf remove -> verify clean removal.
#   5. Capture stdout/stderr + the script's exit code as the test result.
#   6. Stop the VM unconditionally (trap restores clean state on error).
#
# Mirrors test/system/iis/ci-msi-upgrade-test.ps1 but for Linux guests.
# Linux Integration Services on AlmaLinux cloud images doesn't ship the KVP
# daemon by default, so guest IPs are not reported via
# `(Get-VM).NetworkAdapters.IPAddresses`. We discover them via host ARP-by-MAC
# instead - the Default Switch keeps a fresh neighbor entry as soon as the
# guest sends any traffic (DHCP request hits it).
#
# Called from the release lane's ea4-cpanel-smoke job.
#
# --- PowerShell + OpenSSH-on-Windows defensive notes ---
#
# This script runs on a Windows host. It is invoked either by a logged-on
# operator OR by the GH Actions runner service (which itself was launched by
# Windows sshd). The combination of PowerShell call operator + native ssh.exe
# has three landmines we have to walk around:
#
#  1. `& ssh -n root@x cmd` HANGS FOREVER when the PS1 is itself invoked over
#     SSH. The `-n` flag (redirect stdin from /dev/null) does not actually
#     detach the child from PS's inherited stdin handle in that environment.
#     Workaround: Start-Process with -RedirectStandardInput pointing at a
#     real empty file.
#  2. `Start-Process -RedirectStandardInput 'NUL'` does NOT redirect from the
#     Windows null device - PS resolves 'NUL' as a relative file path.
#     Workaround: a real empty temp file in $env:TEMP.
#  3. PowerShell's call operator wraps native-binary stderr as a
#     NativeCommandError ErrorRecord. Under $ErrorActionPreference='Stop'
#     this terminates the script on OpenSSH's "Permanently added '<ip>' to
#     the list of known hosts" message - the FIRST successful ssh kills
#     the run. Even under EAP='Continue', some pipelines (e.g. & scp
#     followed by `if ($LASTEXITCODE)`) still surface the wrap as a script-
#     terminating error in PS 5.1. Workaround: never use the call operator
#     for ssh/scp - always go through Start-Process with stderr captured
#     to a tempfile and filtered/echoed via Write-Host.
#
# Both helpers below (Invoke-Ssh, Invoke-Scp) implement that pattern. They
# return an integer exit code. They never throw on the child's exit status -
# callers check the return value.

param(
    [Parameter(Mandatory=$true)][string]$RpmPath,
    # EL8 revived — see release.yml ea4-build matrix comment.
    [Parameter(Mandatory=$true)][ValidateSet('el9','el8')][string]$Os,
    [string]$ReleaseTag = $env:RELEASE_TAG,
    [string]$ExpectedVersionString = "",
    # Path on the host of the bash smoke script that runs inside the guest.
    [string]$SmokeScript = "$PSScriptRoot\ea4-smoke-target.sh"
)

# $ErrorActionPreference is left at 'Continue' (PS default). Going to 'Stop'
# was the original instinct, but landmine #3 above means EAP=Stop wraps
# OpenSSH's stderr warning as an ErrorRecord that terminates the script on
# the FIRST successful ssh. We instead use -ErrorAction Stop locally on
# Hyper-V cmdlets where we genuinely want hard failure, and rely on
# return-code checks for native binaries (which only ever go through
# Invoke-Ssh / Invoke-Scp anyway). Same lesson as the release.yml gcloud
# calls (mps11 release.yml:1582-1599).
$ErrorActionPreference = 'Continue'

# --- Derive ExpectedVersionString from ReleaseTag if not supplied ---
# Same logic as ci-msi-upgrade-test.ps1: strip leading 'v', strip semver
# build-metadata ('+...').
if ([string]::IsNullOrEmpty($ExpectedVersionString) -and -not [string]::IsNullOrEmpty($ReleaseTag)) {
    $ExpectedVersionString = ($ReleaseTag -replace '^v', '') -replace '\+[^+]+$', ''
    Write-Host "Derived ExpectedVersionString from ReleaseTag: $ExpectedVersionString"
}

$VMName = "CP-$($Os.ToUpper())-base"
$Snapshot = "01-cpanel-clean-$Os"

Write-Host "=== ea4-cpanel-smoke ($Os) ==="
Write-Host "VM:        $VMName"
Write-Host "Snapshot:  $Snapshot"
Write-Host "RPM:       $RpmPath"
Write-Host "Release:   $ReleaseTag"
Write-Host "Expected:  $ExpectedVersionString"
Write-Host ""

if (-not (Test-Path $RpmPath))      { throw "RPM not found: $RpmPath" }
if (-not (Test-Path $SmokeScript))  { throw "Smoke script not found: $SmokeScript" }

# --- Validate VM + snapshot exist (Hyper-V cmdlets: hard-fail locally) ---
$vm = Get-VM -Name $VMName -ErrorAction SilentlyContinue
if (-not $vm) { throw "Hyper-V VM '$VMName' not found on $env:COMPUTERNAME" }

$snap = Get-VMSnapshot -VMName $VMName -Name $Snapshot -ErrorAction SilentlyContinue
if (-not $snap) {
    throw "Checkpoint '$Snapshot' not found on $VMName - has cPanel been installed on the base VM yet? Install it, then take the checkpoint this script restores."
}

# --- Stdin/cleanup state for Start-Process ssh/scp ---
# Empty temp file. Start-Process treats -RedirectStandardInput as a path and
# will not accept 'NUL'; a real zero-byte file is the portable workaround.
# PID in the name so two concurrent runs on the same host (e.g. el8 + el9
# matrix on the same runner) don't fight over it. Cleaned up in the finally
# at the bottom of the script.
$script:NullStdinFile = [IO.Path]::Combine($env:TEMP, "ssh-null-stdin-$PID.txt")
Set-Content -LiteralPath $script:NullStdinFile -Value '' -NoNewline -Encoding ASCII

# Track temp output files for paranoid cleanup if the script aborts mid-run.
$script:TempOutFiles = New-Object System.Collections.ArrayList

function Invoke-NativeOverSsh {
    # Shared core for Invoke-Ssh and Invoke-Scp. Runs $Exe with $Args via
    # Start-Process, redirected stdin/stdout/stderr to temp files. Returns
    # an int exit code. Echoes stdout to host. Filters and echoes stderr
    # (drops the well-known OpenSSH "Permanently added ..." noise). Never
    # raises on child exit code.
    param(
        [Parameter(Mandatory)] [string]$Exe,
        [Parameter(Mandatory)] [string[]]$ProcArgs,
        [string]$Tag = $Exe
    )
    $outFile = [IO.Path]::GetTempFileName()
    $errFile = [IO.Path]::GetTempFileName()
    [void]$script:TempOutFiles.Add($outFile)
    [void]$script:TempOutFiles.Add($errFile)
    try {
        # -ErrorAction Stop ON Start-Process is intentional: we want a hard
        # error if ssh.exe / scp.exe cannot be launched at all (PATH issue
        # on the runner, missing OpenSSH client feature, etc.). The trap
        # at the top of the script will catch that and turn off the VM.
        # The CHILD's stderr does NOT come through this channel - it's
        # routed to $errFile - so EAP=Stop here is safe.
        $proc = Start-Process -FilePath $Exe `
                              -ArgumentList $ProcArgs `
                              -RedirectStandardInput  $script:NullStdinFile `
                              -RedirectStandardOutput $outFile `
                              -RedirectStandardError  $errFile `
                              -Wait -NoNewWindow -PassThru `
                              -ErrorAction Stop

        # PS 5.1 Start-Process sometimes returns a Process whose ExitCode
        # property is $null when the parent ran into a race with the child
        # exiting. Belt-and-braces: WaitForExit() then re-read.
        if ($null -eq $proc.ExitCode) {
            try { $proc.WaitForExit() } catch { }
        }
        $rc = $proc.ExitCode
        if ($null -eq $rc) { $rc = -1 }  # treat unknowable as failure

        $stdout = Get-Content -LiteralPath $outFile -Raw -ErrorAction SilentlyContinue
        $stderr = Get-Content -LiteralPath $errFile -Raw -ErrorAction SilentlyContinue
        if ($stdout) {
            $s = $stdout.TrimEnd()
            if ($s) { Write-Host $s }
        }
        if ($stderr) {
            # Filter OpenSSH's expected host-key noise. -split "`r?`n" so
            # we survive CRLF tempfile contents from Windows OpenSSH.
            $lines = $stderr -split "`r?`n" | Where-Object {
                $_ -and ($_ -notmatch 'Permanently added .* to the list of known hosts')
            }
            if ($lines.Count -gt 0) {
                Write-Host "[$Tag stderr] $($lines -join [Environment]::NewLine)"
            }
        }
        return [int]$rc
    } finally {
        Remove-Item -LiteralPath $outFile,$errFile -Force -ErrorAction SilentlyContinue
        [void]$script:TempOutFiles.Remove($outFile)
        [void]$script:TempOutFiles.Remove($errFile)
    }
}

function Invoke-Ssh {
    # Run a remote command. Returns int exit code from ssh (which is the
    # remote command's exit code on success, or 255 on ssh-side failure).
    param(
        [Parameter(Mandatory)] [string]$Ip,
        [Parameter(Mandatory)] [string]$Command,
        [int]$ConnectTimeoutSec = 10,
        [string]$RemoteUser = 'root'
    )
    $argList = @(
        '-n',
        '-o', 'BatchMode=yes',
        '-o', "ConnectTimeout=$ConnectTimeoutSec",
        '-o', 'StrictHostKeyChecking=accept-new',
        '-o', 'UserKnownHostsFile=NUL',
        "$RemoteUser@$Ip",
        $Command
    )
    return Invoke-NativeOverSsh -Exe 'ssh' -ProcArgs $argList -Tag 'ssh'
}

function Invoke-Scp {
    # Copy a local file to the guest. Returns int exit code from scp.
    # scp does not accept -n, but the empty-stdin file solves the same
    # parent-stdin problem ssh -n was meant to solve.
    param(
        [Parameter(Mandatory)] [string]$LocalPath,
        [Parameter(Mandatory)] [string]$RemotePath,   # e.g. "root@1.2.3.4:/tmp/foo"
        [int]$ConnectTimeoutSec = 10
    )
    $argList = @(
        '-o', 'BatchMode=yes',
        '-o', "ConnectTimeout=$ConnectTimeoutSec",
        '-o', 'StrictHostKeyChecking=accept-new',
        '-o', 'UserKnownHostsFile=NUL',
        $LocalPath,
        $RemotePath
    )
    return Invoke-NativeOverSsh -Exe 'scp' -ProcArgs $argList -Tag 'scp'
}

# --- Hyper-V helpers ---
function Stop-VmHard {
    Write-Host "Stopping VM '$VMName'..."
    Stop-VM -Name $VMName -TurnOff -Force -ErrorAction SilentlyContinue
}

# trap catches genuine exceptions (Hyper-V failures, our own throws). The
# Invoke-Ssh/Invoke-Scp helpers DO NOT throw on child exit code, so the
# trap will not fire on the OpenSSH "Permanently added" stderr noise. We
# exit 1 from the trap so the trap-then-keep-going PS default does not
# silently swallow the failure.
trap { if (Get-Command Stop-VmHard -ErrorAction SilentlyContinue) { Stop-VmHard }; exit 1 }

# Bounded TCP/22 reachability probe. Test-NetConnection's dead-host timeout is
# slow + not controllable, so connect directly with an explicit short timeout.
function Test-Tcp22 {
    param([string]$Ip, [int]$TimeoutMs = 2000)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect($Ip, 22, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs)) { return $false }
        $client.EndConnect($iar); return $true
    } catch { return $false } finally { $client.Close() }
}

function Get-GuestIp {
    # Discover the guest's CURRENT IP by MAC from the host neighbor table, but
    # only ever return an address that actually answers on TCP/22. After a host
    # reboot the Hyper-V Default Switch re-randomizes its /20, so the neighbor
    # table can carry STALE `Permanent` entries from a prior boot's lease whose
    # IPs still fall inside the new /20. A plain `Select -First 1` would then hand
    # back a DEAD address and Wait-Ssh would time out against a non-existent host
    # (the v1.15.0+r14 el8/el9 smoke failure: host rebooted, switch moved to
    # 192.0.2.0/24, two Permanent ARP entries per MAC, the stale one sorted
    # first). Liveness-gating discovery is reboot-proof: try Reachable/Stale
    # before Permanent, and only return an IP that responds on 22.
    param([string]$Mac, [int]$TimeoutSec = 180)
    $macFmt = ($Mac -replace '(..)','$1-').TrimEnd('-')
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $cands = Get-NetNeighbor -InterfaceAlias '*Default*' -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                 Where-Object { $_.LinkLayerAddress -eq $macFmt -and $_.IPAddress -ne '0.0.0.0' } |
                 Sort-Object { switch ([string]$_.State) { 'Reachable' {0} 'Stale' {1} 'Permanent' {2} default {3} } }
        foreach ($c in $cands) {
            if (Test-Tcp22 -Ip $c.IPAddress) { return $c.IPAddress }
        }
        Start-Sleep -Seconds 3
    }
    throw "Timed out after ${TimeoutSec}s waiting for a LIVE guest IP (MAC $macFmt, answering TCP/22) in host neighbor table"
}

function Wait-Ssh {
    param([string]$Ip, [int]$TimeoutSec = 120)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $rc = Invoke-Ssh -Ip $Ip -Command 'exit' -ConnectTimeoutSec 5
        if ($rc -eq 0) { return $true }
        Start-Sleep -Seconds 3
    }
    throw "SSH never came up at root@$Ip within ${TimeoutSec}s"
}

# Main body wrapped in try/finally so we always clean up the null-stdin
# tempfile and any orphaned tempfiles even if a throw bubbles past the
# trap. (trap + exit 1 is for hard failures; the finally below runs even
# on success.)
$exitCode = 0
try {
    # --- Step 1: Revert + start ---
    Write-Host "=== Revert to '$Snapshot' ==="
    Restore-VMSnapshot -VMName $VMName -Name $Snapshot -Confirm:$false -ErrorAction Stop
    Start-VM -Name $VMName -ErrorAction Stop

    $mac = (Get-VMNetworkAdapter -VMName $VMName -ErrorAction Stop | Select-Object -First 1).MacAddress
    Write-Host "Waiting for guest IP (MAC $mac)..."
    $ip = Get-GuestIp -Mac $mac
    Write-Host "Guest IP: $ip"

    Write-Host "Waiting for SSH at root@$ip..."
    Wait-Ssh -Ip $ip
    Write-Host "SSH ready"

    # --- Step 2: Stage artifacts in guest ---
    Write-Host "=== Staging artifacts in guest ==="
    $rc = Invoke-Ssh -Ip $ip -Command 'mkdir -p /tmp/ea4-smoke && rm -rf /tmp/ea4-smoke/*'
    if ($rc -ne 0) { throw "Failed to prep /tmp/ea4-smoke on guest (exit $rc)" }

    $rpmLeaf = Split-Path $RpmPath -Leaf
    $rc = Invoke-Scp -LocalPath $RpmPath     -RemotePath "root@${ip}:/tmp/ea4-smoke/$rpmLeaf"
    if ($rc -ne 0) { throw "scp RPM failed (exit $rc)" }
    $rc = Invoke-Scp -LocalPath $SmokeScript -RemotePath "root@${ip}:/tmp/ea4-smoke/ea4-smoke-target.sh"
    if ($rc -ne 0) { throw "scp smoke script failed (exit $rc)" }
    # NOTE: we deliberately do NOT rely on chmod +x. scp from a Windows
    # source filesystem can land the destination with mode 0644 regardless
    # of `chmod +x`'s exit code (NTFS has no Unix mode bit to mirror), and
    # the symptom is `bash: /tmp/.../*.sh: Permission denied` at run time.
    # Invoking the target script as an argument to bash sidesteps the
    # exec-bit requirement entirely. Belt-and-braces chmod kept for the
    # case where scp DID preserve mode (e.g. from a UNIX-mode-aware source)
    # so a future cleanup pass can verify the file is +x.
    [void](Invoke-Ssh -Ip $ip -Command 'chmod +x /tmp/ea4-smoke/ea4-smoke-target.sh')

    # --- Step 3: Run the smoke script ---
    Write-Host "=== Running smoke script in guest ==="
    $env_EVS = $ExpectedVersionString
    $env_RT  = $ReleaseTag
    $rc = Invoke-Ssh -Ip $ip -ConnectTimeoutSec 30 -Command `
        "RPM_NAME='$rpmLeaf' EXPECTED_VERSION_STRING='$env_EVS' RELEASE_TAG='$env_RT' bash /tmp/ea4-smoke/ea4-smoke-target.sh"

    if ($rc -ne 0) {
        Write-Host "::error::ea4-smoke-target.sh exited $rc on $VMName"
        Write-Host "=== Last 50 lines of Apache error_log ==="
        [void](Invoke-Ssh -Ip $ip -Command 'tail -50 /etc/apache2/logs/error_log 2>/dev/null || tail -50 /var/log/apache2/error_log 2>/dev/null || true')
        Write-Host "=== Last 50 lines of pagespeed log ==="
        [void](Invoke-Ssh -Ip $ip -Command 'cat /var/log/pagespeed/*.log 2>/dev/null | tail -50 || true')
        $exitCode = $rc
    }

    # --- Step 4: Stop VM regardless of outcome ---
    Stop-VmHard

    if ($exitCode -eq 0) {
        Write-Host "=== ea4-cpanel-smoke ($Os) PASSED ==="
    }
} finally {
    # Always remove the null-stdin tempfile; remove any orphaned out/err
    # tempfiles that escaped the per-call finally (paranoia).
    Remove-Item -LiteralPath $script:NullStdinFile -Force -ErrorAction SilentlyContinue
    foreach ($f in @($script:TempOutFiles)) {
        if ($f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
    }
}

exit $exitCode
