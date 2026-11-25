# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# provision_iis_lab_guest.ps1 - Creates a Generation-2 Hyper-V lab guest with
# IIS 10 from a Windows Server installation ISO (an evaluation ISO needs no
# product key), so the IIS pipeline probe and the IIS adapter's rows can run
# on more than one Windows Server release. Run ONCE per guest, as
# Administrator on the Hyper-V host.
#
# What it does:
#   * applies the -Edition image of the ISO's install.wim to a new dynamic
#     VHDX (GPT: EFI, MSR, Windows), makes it bootable, and writes an
#     unattend file that sets the Administrator password and skips the
#     out-of-box screens;
#   * creates the VM (Secure Boot with the Microsoft Windows template,
#     automatic checkpoints off) on -SwitchName and boots it;
#   * waits for PowerShell Direct, installs IIS with the features the probe
#     needs (static and dynamic compression, failed-request tracing,
#     ASP.NET 4.x, ISAPI, WebSockets, the management tools) and, when -Msi
#     is given, the PageSpeed module;
#   * shuts the guest down and takes checkpoint -Checkpoint.
#
# The Administrator password is generated here and written to -PasswordFile,
# whose ACL lets only Administrators and SYSTEM read it (the way a runner's
# environment file holds a CI guest's password). It is never printed. The guest is a lab guest: the script refuses the name a CI gate
# restores (CI_IIS_VM_NAME), and refuses a name that already exists.
#
# Usage:
#   .\provision_iis_lab_guest.ps1 -VMName <name> -Iso <server.iso> -PasswordFile <path> [-Msi <module.msi>]
#   .\provision_iis_lab_guest.ps1 ... -ListEditions     # print the ISO's editions and stop
#
# Then, for example:
#   $pw = ConvertTo-SecureString (Get-Content -Raw <path>).Trim() -AsPlainText -Force
#   .\probe_iis_html_cache_pipeline.ps1 -VMName <name> -Credential (New-Object PSCredential('Administrator', $pw))
#
# Windows PowerShell 5.1 compatible; ASCII only.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$VMName,
    [Parameter(Mandatory = $true)][string]$Iso,
    [string]$PasswordFile = "",
    [string]$Edition = "Datacenter Evaluation (Desktop Experience)",
    [string]$Msi = "",
    [string]$VMPath = "",
    [string]$SwitchName = "Default Switch",
    [int]$Cpu = 4,
    [int]$MemoryGB = 6,
    [int]$DiskGB = 60,
    [string]$Checkpoint = "00-iis-ready",
    [switch]$ListEditions
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Say([string]$Line) { Write-Host "[provision] $Line" }

$isoPath = (Resolve-Path -LiteralPath $Iso).Path
$mounted = $false
$vhdMounted = $false
$vhd = $null
try {
    $img = Mount-DiskImage -ImagePath $isoPath -PassThru
    $mounted = $true
    $isoDrive = ($img | Get-Volume).DriveLetter
    $wim = "${isoDrive}:\sources\install.wim"
    if (-not (Test-Path -LiteralPath $wim)) { throw "the ISO has no sources\install.wim" }
    $images = @(Get-WindowsImage -ImagePath $wim)
    if ($ListEditions) {
        $images | ForEach-Object { "{0}: {1}" -f $_.ImageIndex, $_.ImageName }
        exit 0
    }
    if (-not $PasswordFile) { throw "-PasswordFile is required" }
    if ($env:CI_IIS_VM_NAME -and $VMName -eq $env:CI_IIS_VM_NAME) { throw "-VMName names the guest a CI gate restores; use a lab guest" }
    if (Get-VM -Name $VMName -ErrorAction SilentlyContinue) { throw "a VM named $VMName already exists" }
    $image = $images | Where-Object { $_.ImageName -like "*$Edition*" } | Select-Object -First 1
    if (-not $image) { throw "no edition matching '$Edition' in the ISO (see -ListEditions)" }
    Say "edition $($image.ImageIndex): $($image.ImageName)"

    if (-not $VMPath) { $VMPath = (Get-VMHost).VirtualMachinePath }
    $vmDir = Join-Path $VMPath $VMName
    $vhd = Join-Path $vmDir "$VMName.vhdx"
    New-Item -ItemType Directory -Force -Path $vmDir | Out-Null

    # The guest's Administrator password: random, kept only in a file that
    # Administrators and SYSTEM alone may read.
    $bytes = New-Object byte[] 18
    $rng = New-Object System.Security.Cryptography.RNGCryptoServiceProvider
    $rng.GetBytes($bytes)
    $rng.Dispose()
    $plain = "Aa1!" + ([Convert]::ToBase64String($bytes) -replace '[+/=]', 'x')
    Set-Content -LiteralPath $PasswordFile -Value '' -Encoding ascii
    & icacls.exe $PasswordFile /inheritance:r /grant:r '*S-1-5-32-544:F' '*S-1-5-18:F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "could not restrict the ACL of $PasswordFile" }
    Set-Content -LiteralPath $PasswordFile -Value $plain -Encoding ascii -NoNewline
    # The unattend file's encoded form, which is not encryption: the file is
    # removed from the guest once setup has used it (below).
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($plain + "AdministratorPassword"))
    $plain = $null

    Say "creating $vhd ($DiskGB GB, dynamic)"
    New-VHD -Path $vhd -SizeBytes ([int64]$DiskGB * 1GB) -Dynamic | Out-Null
    $disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
    $vhdMounted = $true
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT
    $efi = New-Partition -DiskNumber $disk.Number -Size 260MB -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -AssignDriveLetter
    Format-Volume -Partition $efi -FileSystem FAT32 -NewFileSystemLabel 'System' -Confirm:$false | Out-Null
    New-Partition -DiskNumber $disk.Number -Size 16MB -GptType '{e3c9e316-0b5c-4db8-817d-f92df00215ae}' | Out-Null
    $win = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
    Format-Volume -Partition $win -FileSystem NTFS -NewFileSystemLabel 'Windows' -Confirm:$false | Out-Null
    $efiDrive = (Get-Partition -DiskNumber $disk.Number -PartitionNumber $efi.PartitionNumber).DriveLetter
    $winDrive = (Get-Partition -DiskNumber $disk.Number -PartitionNumber $win.PartitionNumber).DriveLetter

    Say "applying the image (several minutes)"
    Expand-WindowsImage -ImagePath $wim -Index $image.ImageIndex -ApplyPath "${winDrive}:\" | Out-Null
    & bcdboot.exe "${winDrive}:\Windows" /s "${efiDrive}:" /f UEFI | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "bcdboot failed ($LASTEXITCODE)" }

    $panther = "${winDrive}:\Windows\Panther"
    New-Item -ItemType Directory -Force -Path $panther | Out-Null
    $unattend = @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS"><!-- public assembly key, hygiene-allow-credential -->
      <ComputerName>$($VMName.Substring(0, [Math]::Min(15, $VMName.Length)))</ComputerName>
      <TimeZone>UTC</TimeZone>
    </component>
  </settings>
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS"><!-- public assembly key, hygiene-allow-credential -->
      <UserAccounts>
        <AdministratorPassword>
          <Value>$encoded</Value>
          <PlainText>false</PlainText>
        </AdministratorPassword>
      </UserAccounts>
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <SkipMachineOOBE>true</SkipMachineOOBE>
      </OOBE>
    </component>
    <component name="Microsoft-Windows-International-Core" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS"><!-- public assembly key, hygiene-allow-credential -->
      <InputLocale>en-US</InputLocale>
      <SystemLocale>en-US</SystemLocale>
      <UILanguage>en-US</UILanguage>
      <UserLocale>en-US</UserLocale>
    </component>
  </settings>
</unattend>
"@
    Set-Content -LiteralPath (Join-Path $panther 'unattend.xml') -Value $unattend -Encoding UTF8
    $encoded = $null
    Dismount-VHD -Path $vhd
    $vhdMounted = $false
} finally {
    if ($vhdMounted) { Dismount-VHD -Path $vhd -ErrorAction SilentlyContinue }
    if ($mounted) { Dismount-DiskImage -ImagePath $isoPath -ErrorAction SilentlyContinue | Out-Null }
}

Say "creating the VM"
New-VM -Name $VMName -Generation 2 -Path $VMPath -VHDPath $vhd -MemoryStartupBytes ([int64]$MemoryGB * 1GB) -SwitchName $SwitchName | Out-Null
Set-VMProcessor -VMName $VMName -Count $Cpu
Set-VMFirmware -VMName $VMName -EnableSecureBoot On -SecureBootTemplate MicrosoftWindows
Set-VM -Name $VMName -AutomaticCheckpointsEnabled $false -AutomaticStartAction Nothing -AutomaticStopAction ShutDown
Start-VM -Name $VMName

$cred = New-Object System.Management.Automation.PSCredential('Administrator',
    (ConvertTo-SecureString (Get-Content -LiteralPath $PasswordFile -Raw).Trim() -AsPlainText -Force))
Say "waiting for the guest (first boot and setup, up to 30 minutes)"
$deadline = (Get-Date).AddMinutes(30)
$ready = $false
while ((Get-Date) -lt $deadline) {
    try {
        $h = Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock { $env:COMPUTERNAME } -ErrorAction Stop
        if ($h) { $ready = $true; break }
    } catch { }
    Start-Sleep -Seconds 15
}
if (-not $ready) { throw "the guest did not answer PowerShell Direct within 30 minutes" }

Say "installing IIS"
$features = @('Web-Server', 'Web-Static-Content', 'Web-Default-Doc', 'Web-Http-Errors', 'Web-Http-Logging',
              'Web-Stat-Compression', 'Web-Dyn-Compression', 'Web-Http-Tracing', 'Web-Asp-Net45',
              'Web-ISAPI-Ext', 'Web-ISAPI-Filter', 'Web-WebSockets', 'Web-Mgmt-Console', 'Web-Scripting-Tools')
$r = Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock {
    param($names)
    # The unattend file holds the encoded password; setup is done with it.
    Remove-Item -LiteralPath "$env:windir\Panther\unattend.xml" -Force -ErrorAction SilentlyContinue
    $res = Install-WindowsFeature -Name $names
    "$($res.Success) $($res.RestartNeeded)"
} -ArgumentList (, $features)
Say "Install-WindowsFeature: $r"

if ($Msi) {
    Say "installing the module"
    $s = New-PSSession -VMName $VMName -Credential $cred
    try {
        Invoke-Command -Session $s -ScriptBlock { New-Item -ItemType Directory -Force C:\artifacts | Out-Null }
        Copy-Item -ToSession $s -Path $Msi -Destination 'C:\artifacts\module.msi' -Force
        $code = Invoke-Command -Session $s -ScriptBlock {
            $p = Start-Process msiexec.exe -ArgumentList '/i C:\artifacts\module.msi /qn /l*v C:\artifacts\module-install.log' -Wait -PassThru
            $p.ExitCode
        }
        Say "msiexec exit $code"
        if ($code -ne 0 -and $code -ne 3010) { throw "the module install failed ($code)" }
    } finally {
        Remove-PSSession $s
    }
}

$os = Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock {
    $v = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    "$($v.ProductName) build $($v.CurrentBuild).$($v.UBR), IIS $((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\InetStp').VersionString)"
}
Say "guest: $os"
Stop-VM -Name $VMName
Checkpoint-VM -Name $VMName -SnapshotName $Checkpoint
Say "done: $VMName is off at checkpoint $Checkpoint"
