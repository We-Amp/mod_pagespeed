# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# provision-el9-vm.ps1 - create the Generation-2 Hyper-V VM for the
# enforcing-SELinux EL9 upgrade rehearsal and leave it stopped at its
# '00-fresh' checkpoint. Run ONCE per VM, on the Hyper-V host, elevated.
#
# What it produces (mirrors the cPanel rig's EL VMs, minus their SELinux-off
# cloud-init lines -- this VM keeps the image default, ENFORCING):
#   * <VirtualHardDiskPath>\<VMName>.vhdx   dynamic VHDX converted from the
#                                            AlmaLinux 9 GenericCloud qcow2
#                                            with qemu-img (on PATH or via
#                                            WSL), grown to -DiskGB
#   * <VirtualMachinePath>\seeds\<VMName>-seed.iso
#                                            NoCloud seed (label 'cidata'):
#                                            hostname + key-only root SSH,
#                                            built with IMAPI2 (no external
#                                            ISO tool) unless -SeedIso names a
#                                            prebuilt one
#   * the VM: Gen 2, Secure Boot Off by default (On uses the
#     MicrosoftUEFICertificateAuthority template), static memory, automatic
#     checkpoints off, AutomaticStopAction ShutDown, on -SwitchName
#   * first boot over the Default Switch: waits for cloud-init, asserts
#     getenforce == Enforcing, dnf update (skip with -NoUpdate) and installs
#     what the rehearsal needs on every run anyway (httpd + the SELinux
#     tooling), powers off, takes checkpoint -Checkpoint ('00-fresh')
#
# Usage:
#   .\provision-el9-vm.ps1 -ImageQcow2 <path\AlmaLinux-9-GenericCloud-latest.x86_64.qcow2>
#   .\provision-el9-vm.ps1 -DownloadImage           # fetch + sha256-verify into -WorkDir first
#   .\provision-el9-vm.ps1 -ImageVhdx <already converted .vhdx>
#   .\provision-el9-vm.ps1 -BuildSeedOnly           # render + build the seed ISO into -WorkDir, no Hyper-V
#   .\provision-el9-vm.ps1 ... -DryRun              # print the plan, touch nothing
#
# Image source: https://repo.almalinux.org/almalinux/9/cloud/x86_64/images/
# (AlmaLinux-9-GenericCloud-latest.x86_64.qcow2 + CHECKSUM). Any EL9
# GenericCloud image with cloud-init and SELinux enforcing by default works;
# the runbook (README.md) discusses the choice.

param(
    [string]$VMName = 'EL9-SELinux-rehearsal',
    [string]$ImageQcow2 = '',
    [string]$ImageVhdx = '',
    [switch]$DownloadImage,
    [string]$ImageUrl = 'https://repo.almalinux.org/almalinux/9/cloud/x86_64/images/AlmaLinux-9-GenericCloud-latest.x86_64.qcow2',
    [string]$ChecksumUrl = 'https://repo.almalinux.org/almalinux/9/cloud/x86_64/images/CHECKSUM',
    [string]$WorkDir = (Join-Path $env:TEMP 'el9-selinux-rehearsal'),
    [string]$SeedIso = '',
    [string]$SshPublicKeyPath = (Join-Path $env:USERPROFILE '.ssh\id_ed25519.pub'),
    [string]$SshKeyPath = (Join-Path $env:USERPROFILE '.ssh\id_ed25519'),
    [string]$Hostname = 'el9-selinux',
    [string]$SwitchName = 'Default Switch',
    [int]$Cpu = 2,
    [int]$MemoryGB = 4,
    [int]$DiskGB = 32,
    [ValidateSet('On','Off')][string]$SecureBoot = 'Off',
    [string]$Checkpoint = '00-fresh',
    [ValidateSet('auto','wsl','imapi2')][string]$SeedTool = 'auto',
    [switch]$NoUpdate,
    [switch]$BuildSeedOnly,
    [switch]$DryRun
)

# EAP stays 'Continue' (see hv-common.ps1 for why); Hyper-V cmdlets that must
# hard-fail carry -ErrorAction Stop themselves.
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'hv-common.ps1')

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
$transcript = Join-Path $WorkDir "provision-$VMName-$stamp.log"
try { Start-Transcript -Path $transcript -Force | Out-Null } catch { }

function Fail([string]$msg) { Write-Host "::error::$msg"; throw $msg }

# A trap applies to its whole scope, however far down it is written; keep it
# and its helpers ahead of the first thing that can throw. Cleanup advice is
# only relevant once the VM/VHDX exist ($script:VmTouched).
$script:VmTouched = $false
trap {
    Write-Host "::error::provisioning failed: $_"
    if ($script:VmTouched) {
        if (Get-VM -Name $VMName -ErrorAction SilentlyContinue) { Stop-VmHard -VMName $VMName }
        Write-Host "Nothing was deleted; inspect $transcript, then remove the half-made VM/VHDX by hand before retrying."
    }
    Remove-SshHelpers
    try { Stop-Transcript | Out-Null } catch { }
    exit 1
}

# --- Inputs -----------------------------------------------------------------
Write-Host "=== provision-el9-vm: $VMName ==="
$seedSrcDir = Join-Path $WorkDir "seed-$VMName"
$hvAvailable = [bool](Get-Command Get-VMHost -ErrorAction SilentlyContinue)
$vmHost = $null
if ($hvAvailable) { $vmHost = Get-VMHost -ErrorAction SilentlyContinue }
if (-not $vmHost -and -not ($BuildSeedOnly -or $DryRun)) {
    Fail "Hyper-V PowerShell module not available (or not elevated): run on the Hyper-V host from an elevated PowerShell."
}
$vhdRoot  = if ($vmHost) { $vmHost.VirtualHardDiskPath } else { '<VirtualHardDiskPath>' }
$vmRoot   = if ($vmHost) { $vmHost.VirtualMachinePath }  else { '<VirtualMachinePath>' }
$destVhdx = Join-Path $vhdRoot "$VMName.vhdx"
$seedDir  = Join-Path $vmRoot 'seeds'
$destIso  = if ($SeedIso) { $SeedIso } else { Join-Path $seedDir "$VMName-seed.iso" }
if ($BuildSeedOnly -and -not $SeedIso) { $destIso = Join-Path $seedSrcDir "$VMName-seed.iso" }

# Image: exactly one route.
$imageRoute = ''
if ($ImageVhdx)          { $imageRoute = 'vhdx' }
elseif ($ImageQcow2)     { $imageRoute = 'qcow2' }
elseif ($DownloadImage)  { $imageRoute = 'download' }
if (-not $imageRoute -and -not $BuildSeedOnly) {
    Fail "No image: pass -ImageQcow2 <qcow2>, -ImageVhdx <vhdx> or -DownloadImage (fetches $ImageUrl)."
}
if ($ImageVhdx -and -not (Test-Path -LiteralPath $ImageVhdx)) { Fail "-ImageVhdx not found: $ImageVhdx" }
if ($ImageQcow2 -and -not (Test-Path -LiteralPath $ImageQcow2)) { Fail "-ImageQcow2 not found: $ImageQcow2" }
$qemuRoute = ''
if ($imageRoute -eq 'qcow2' -or $imageRoute -eq 'download') {
    $qemuRoute = Get-QemuImgRoute
    if (-not $qemuRoute -and -not $DryRun) {
        # Convert-QcowToVhdx prints the manual recipe.
        try { Convert-QcowToVhdx -Qcow2Path 'x' -VhdxPath 'y' -Route '' } catch { Fail $_.Exception.Message }
    }
}

# Seed inputs.
if ($SeedIso -and -not (Test-Path -LiteralPath $SeedIso)) { Fail "-SeedIso not found: $SeedIso" }
$pubKey = ''
if (-not $SeedIso) {
    if (-not (Test-Path -LiteralPath $SshPublicKeyPath)) { Fail "SSH public key not found: $SshPublicKeyPath (pass -SshPublicKeyPath)" }
    $pubKey = (Get-Content -LiteralPath $SshPublicKeyPath | Where-Object { $_ -match '^ssh-' } | Select-Object -First 1)
    if (-not $pubKey) { Fail "No 'ssh-...' line in $SshPublicKeyPath" }
}
if (-not (Test-Path -LiteralPath $SshKeyPath) -and -not ($BuildSeedOnly -or $DryRun)) {
    Fail "SSH private key not found: $SshKeyPath (needed to reach the guest on first boot; pass -SshKeyPath)"
}
$templateDir = Join-Path $PSScriptRoot 'cloud-init'
foreach ($t in @('user-data.template','meta-data.template')) {
    if (-not (Test-Path -LiteralPath (Join-Path $templateDir $t))) { Fail "template missing: $templateDir\$t" }
}
$isoRoute = ''
if (-not $SeedIso) {
    switch ($SeedTool) {
        'auto'   { $isoRoute = Get-IsoToolRoute }
        'wsl'    { $isoRoute = 'wsl-genisoimage'; if ((Get-IsoToolRoute) -ne 'wsl-genisoimage') { Fail "-SeedTool wsl: genisoimage not found in WSL (sudo apt-get install -y genisoimage)" } }
        'imapi2' { $isoRoute = 'imapi2' }
    }
}

# --- Plan -------------------------------------------------------------------
Write-Host "VM name:        $VMName"
Write-Host "Image route:    $imageRoute$(if ($qemuRoute) { " (qemu-img via $qemuRoute)" })"
if ($imageRoute -eq 'download') { Write-Host "Image URL:      $ImageUrl" }
if ($ImageQcow2) { Write-Host "Image qcow2:    $ImageQcow2" }
if ($ImageVhdx)  { Write-Host "Image vhdx:     $ImageVhdx" }
Write-Host "VM disk:        $destVhdx ($DiskGB GB dynamic)"
Write-Host "Seed ISO:       $destIso$(if ($SeedIso) { ' (prebuilt)' } else { " (rendered from cloud-init\*.template, built with $isoRoute)" })"
Write-Host "Guest:          hostname=$Hostname, root login with key $(Split-Path $SshPublicKeyPath -Leaf), SELinux left ENFORCING"
Write-Host "Shape:          Gen 2, SecureBoot $SecureBoot, $Cpu vCPU, $MemoryGB GB static, switch '$SwitchName'"
Write-Host "Checkpoint:     $Checkpoint (VM left Off)"
Write-Host "First boot:     cloud-init wait, assert Enforcing, $(if ($NoUpdate) { 'NO dnf update' } else { 'dnf update' }), install httpd + SELinux tooling, poweroff"
Write-Host "Work dir:       $WorkDir"
if ($DryRun) { Write-Host "dry run: nothing executed"; try { Stop-Transcript | Out-Null } catch { }; exit 0 }

# --- Seed ISO ---------------------------------------------------------------
function Build-Seed {
    New-Item -ItemType Directory -Path $seedSrcDir -Force | Out-Null
    Get-ChildItem -LiteralPath $seedSrcDir -File | Remove-Item -Force
    $instanceId = "$($VMName.ToLower())-$stamp"
    $ud = Get-Content -LiteralPath (Join-Path $templateDir 'user-data.template') -Raw
    $md = Get-Content -LiteralPath (Join-Path $templateDir 'meta-data.template') -Raw
    $ud = $ud.Replace('@@HOSTNAME@@', $Hostname).Replace('@@SSH_PUBLIC_KEY@@', $pubKey)
    $md = $md.Replace('@@HOSTNAME@@', $Hostname).Replace('@@INSTANCE_ID@@', $instanceId)
    Write-Utf8NoBomLf -Path (Join-Path $seedSrcDir 'user-data') -Text $ud
    Write-Utf8NoBomLf -Path (Join-Path $seedSrcDir 'meta-data') -Text $md
    # Guard the one property this seed exists for.
    if ($ud -match '(?m)^\s*-\s*(setenforce|sed .*SELINUX=)') { Fail "user-data would change SELinux mode; this VM must stay enforcing" }
    $isoDir = Split-Path -Parent $destIso
    New-Item -ItemType Directory -Path $isoDir -Force | Out-Null
    if ($isoRoute -eq 'wsl-genisoimage') {
        New-CloudInitSeedIsoWsl -SourceDir $seedSrcDir -OutputPath $destIso
    } else {
        New-CloudInitSeedIso -SourceDir $seedSrcDir -OutputPath $destIso
    }
    Write-Host "Seed ISO written ($isoRoute): $destIso ($((Get-Item -LiteralPath $destIso).Length) bytes; instance-id $instanceId)"
}
if (-not $SeedIso) { Build-Seed }
if ($BuildSeedOnly) {
    Write-Host "=== seed only: done (rendered files in $seedSrcDir) ==="
    try { Stop-Transcript | Out-Null } catch { }
    exit 0
}

# --- Guards -----------------------------------------------------------------
if (Get-VM -Name $VMName -ErrorAction SilentlyContinue) {
    Fail "VM '$VMName' already exists. Remove it first if you mean to re-provision: Remove-VM -Name '$VMName' -Force; Remove-Item '$destVhdx'"
}
if (Test-Path -LiteralPath $destVhdx) {
    Fail "Target VHDX already exists: $destVhdx (left over from a removed VM? delete it, or pick another -VMName)"
}
if (-not (Get-VMSwitch -Name $SwitchName -ErrorAction SilentlyContinue)) {
    Fail "Hyper-V switch '$SwitchName' not found (Get-VMSwitch lists the available ones)"
}

# --- Image ------------------------------------------------------------------
$script:VmTouched = $true
New-Item -ItemType Directory -Path (Split-Path -Parent $destVhdx) -Force | Out-Null
switch ($imageRoute) {
    'download' {
        $imgDir = Join-Path $WorkDir 'images'
        New-Item -ItemType Directory -Path $imgDir -Force | Out-Null
        $imgName = Split-Path $ImageUrl -Leaf
        $ImageQcow2 = Join-Path $imgDir $imgName
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        $sums = (Invoke-WebRequest -Uri $ChecksumUrl -UseBasicParsing -ErrorAction Stop).Content
        $want = ''
        foreach ($line in ($sums -split "`r?`n")) {
            if ($line -match '^SHA256 \((?<f>[^)]+)\) = (?<h>[0-9a-fA-F]{64})' -and $Matches['f'] -eq $imgName) { $want = $Matches['h'].ToLower() }
        }
        if (-not $want) { Fail "no SHA256 line for $imgName in $ChecksumUrl" }
        $have = ''
        if (Test-Path -LiteralPath $ImageQcow2) { $have = (Get-FileHash -LiteralPath $ImageQcow2 -Algorithm SHA256).Hash.ToLower() }
        if ($have -ne $want) {
            Write-Host "Downloading $ImageUrl -> $ImageQcow2 ..."
            Invoke-WebRequest -Uri $ImageUrl -OutFile $ImageQcow2 -UseBasicParsing -ErrorAction Stop
            $have = (Get-FileHash -LiteralPath $ImageQcow2 -Algorithm SHA256).Hash.ToLower()
        } else {
            Write-Host "Image already present and sha256-verified: $ImageQcow2"
        }
        if ($have -ne $want) { Fail "sha256 mismatch for $imgName (want $want, have $have)" }
        Write-Host "sha256 OK: $have"
        Write-Host "Converting qcow2 -> VHDX ($qemuRoute) ..."
        Convert-QcowToVhdx -Qcow2Path $ImageQcow2 -VhdxPath $destVhdx -Route $qemuRoute
    }
    'qcow2' {
        Write-Host "Converting qcow2 -> VHDX ($qemuRoute) ..."
        Convert-QcowToVhdx -Qcow2Path $ImageQcow2 -VhdxPath $destVhdx -Route $qemuRoute
    }
    'vhdx' {
        # Never boot the pristine converted image: copy it, boot the copy.
        Write-Host "Copying $ImageVhdx -> $destVhdx ..."
        Copy-Item -LiteralPath $ImageVhdx -Destination $destVhdx -ErrorAction Stop
    }
}
if (-not (Test-Path -LiteralPath $destVhdx)) { Fail "VHDX not produced at $destVhdx" }
Resize-VHD -Path $destVhdx -SizeBytes ([int64]$DiskGB * 1GB) -ErrorAction Stop
Write-Host "VHDX ready: $destVhdx ($DiskGB GB; cloud-init grows the root filesystem into it on first boot)"

# --- VM ---------------------------------------------------------------------
Write-Host "Creating VM '$VMName' ..."
New-VM -Name $VMName -Generation 2 -MemoryStartupBytes ([int64]$MemoryGB * 1GB) `
       -VHDPath $destVhdx -SwitchName $SwitchName -Path $vmRoot -ErrorAction Stop | Out-Null
Set-VM -Name $VMName -ProcessorCount $Cpu -StaticMemory `
       -AutomaticStartAction Nothing -AutomaticStopAction ShutDown `
       -AutomaticCheckpointsEnabled $false -CheckpointType Standard -ErrorAction Stop
if ($SecureBoot -eq 'On') {
    Set-VMFirmware -VMName $VMName -EnableSecureBoot On -SecureBootTemplate 'MicrosoftUEFICertificateAuthority' -ErrorAction Stop
} else {
    Set-VMFirmware -VMName $VMName -EnableSecureBoot Off -ErrorAction Stop
}
Add-VMDvdDrive -VMName $VMName -Path $destIso -ErrorAction Stop
Set-VMFirmware -VMName $VMName -FirstBootDevice (Get-VMHardDiskDrive -VMName $VMName -ErrorAction Stop) -ErrorAction Stop
Enable-VMIntegrationService -VMName $VMName -Name 'Guest Service Interface' -ErrorAction SilentlyContinue
Write-Host "VM created."

# --- First boot -------------------------------------------------------------
Initialize-SshHelpers
try {
    Write-Host "=== First boot ==="
    Start-VM -Name $VMName -ErrorAction Stop
    $mac = (Get-VMNetworkAdapter -VMName $VMName -ErrorAction Stop | Select-Object -First 1).MacAddress
    Write-Host "Waiting for guest IP (MAC $mac) on '*$SwitchName*' ..."
    $ip = Get-GuestIp -Mac $mac -InterfaceAlias "*$SwitchName*"
    Write-Host "Guest IP: $ip"
    Wait-Ssh -Ip $ip -KeyPath $SshKeyPath -TimeoutSec 300 | Out-Null
    Write-Host "SSH ready (root@$ip)"

    Write-Host "--- cloud-init status --wait ---"
    $rc = Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -ConnectTimeoutSec 30 -Command 'cloud-init status --wait; echo "cloud-init exit=$?"; cat /root/first-boot-getenforce.txt 2>/dev/null'
    if ($rc -ne 0) { Fail "cloud-init status query failed over ssh (exit $rc)" }

    Write-Host "--- identity ---"
    [void](Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -Command 'head -2 /etc/os-release; uname -r; sestatus | head -3')
    $tmp = [IO.Path]::GetTempFileName()
    $rc = Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -Command 'getenforce' -LogFile $tmp
    $mode = (Get-Content -LiteralPath $tmp -Raw -ErrorAction SilentlyContinue)
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    if ($rc -ne 0 -or -not $mode -or $mode.Trim() -ne 'Enforcing') {
        Fail "guest is not SELinux-enforcing after first boot (getenforce: '$($mode)'). Wrong image, or the seed changed the mode."
    }
    Write-Host "getenforce: Enforcing"

    if (-not $NoUpdate) {
        Write-Host "--- dnf -y update (this can take several minutes) ---"
        $rc = Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -ConnectTimeoutSec 30 -Command 'dnf -y -q update 2>&1 | tail -20; echo "dnf update exit=${PIPESTATUS[0]}"'
        if ($rc -ne 0) { Fail "dnf update failed over ssh (exit $rc)" }
    }
    Write-Host "--- installing the web server and the SELinux tooling the rehearsal uses ---"
    $rc = Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -ConnectTimeoutSec 30 -Command 'dnf -y -q --setopt=install_weak_deps=False install httpd curl python3 audit policycoreutils policycoreutils-python-utils setools-console 2>&1 | tail -20 && systemctl enable -q auditd && echo "tooling installed"'
    if ($rc -ne 0) { Fail "tooling install failed over ssh (exit $rc)" }
    [void](Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -Command 'rpm -q httpd audit policycoreutils-python-utils setools-console; getenforce')

    Write-Host "--- poweroff ---"
    [void](Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -ConnectTimeoutSec 5 -Command 'systemctl poweroff')
    if (-not (Wait-VmState -VMName $VMName -State 'Off' -TimeoutSec 180)) {
        Write-Host "VM did not power off cleanly; forcing"
        Stop-VM -Name $VMName -Force -ErrorAction SilentlyContinue
        if (-not (Wait-VmState -VMName $VMName -State 'Off' -TimeoutSec 60)) { Stop-VmHard -VMName $VMName }
    }
    Write-Host "VM is Off."

    Write-Host "=== Checkpoint '$Checkpoint' ==="
    Checkpoint-VM -Name $VMName -SnapshotName $Checkpoint -ErrorAction Stop
    Get-VMSnapshot -VMName $VMName -ErrorAction Stop | Select-Object Name, CreationTime | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host "=== provision-el9-vm: DONE -- '$VMName' is Off at checkpoint '$Checkpoint' ==="
} finally {
    Remove-SshHelpers
    try { Stop-Transcript | Out-Null } catch { }
}
exit 0
