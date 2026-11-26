# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# rig_iis_html_report.ps1 - Checks DaemonHtmlCache report-only on a real IIS:
# that the module's output is the same in every mode, that the decision log
# reads the response as IIS sends it (customHeaders included) through the
# PageSpeedFinalView module entry, that a statically compressed page is
# evaluated on its decoded bytes, and what happens when a site removes that
# entry. The checks are listed in guest_iis_html_report.ps1.
#
# Copies the guest script (and, with -Msi, the package, which it installs
# first) into a lab guest over PowerShell Direct, runs its setup, run and
# teardown phases, and prints every line. The teardown restores the IIS
# configuration backup the setup took.
#
# Exit codes: 0 every check passed; 1 a check failed or the run broke;
# 2 a precondition was not met.
#
# Run as Administrator on the Hyper-V host, against a lab guest. -VMName and
# -Credential (the guest Administrator) are required; the credential is never
# printed. The script refuses the guest a CI gate restores (CI_IIS_VM_NAME,
# docs/ci/runner-environment.md). The guest needs Web-Dyn-Compression and
# Web-Stat-Compression (-InstallFeatures adds them), and, unless -Msi is given, the PageSpeed MSI with its
# optimizer installed. Windows PowerShell 5.1 compatible; ASCII only.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$VMName,
    [Parameter(Mandatory = $true)][System.Management.Automation.PSCredential]$Credential,
    [string]$Msi = "",
    # Installs the two compression features first (a lab guest only).
    [switch]$InstallFeatures,
    [int]$Port = 8301,
    [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"

function Say([string]$Line) {
    Write-Host $Line
    if ($OutFile) { Add-Content -LiteralPath $OutFile -Value $Line -Encoding ASCII }
}

if ($env:CI_IIS_VM_NAME -and $VMName -eq $env:CI_IIS_VM_NAME) {
    Say "PRECONDITION | -VMName names the guest a CI gate restores; use a lab guest"
    exit 2
}
if ($Msi -and -not (Test-Path -LiteralPath $Msi)) {
    Say "PRECONDITION | -Msi $Msi does not exist"
    exit 2
}

$guestScript = Join-Path $PSScriptRoot "guest_iis_html_report.ps1"
$session = New-PSSession -VMName $VMName -Credential $Credential
$exitCode = 0
$sawSummary = $false
try {
    if ($InstallFeatures) {
        $lines = Invoke-Command -Session $session -ScriptBlock {
            $r = Install-WindowsFeature -Name Web-Dyn-Compression, Web-Stat-Compression
            "FEATURES | installed | success={0} restart={1}" -f $r.Success, $r.RestartNeeded
        }
        foreach ($l in $lines) { Say "$l" }
    }
    $pre = Invoke-Command -Session $session -ScriptBlock {
        foreach ($f in @("Web-Dyn-Compression", "Web-Stat-Compression")) {
            $w = Get-WindowsFeature -Name $f
            if ($w -eq $null -or -not $w.Installed) { return "feature $f not installed" }
        }
        if (Test-Path "$env:windir\system32\inetsrv\backup\htmlrep-before") {
            return "an earlier run left the IIS backup htmlrep-before: restore it (appcmd restore backup htmlrep-before), delete it, then run again"
        }
        New-Item -ItemType Directory -Force "C:\htmlrep-bin" | Out-Null
        "ok"
    }
    if ("$pre" -ne "ok") { Say "PRECONDITION | $pre"; exit 2 }
    Copy-Item -ToSession $session -Path $guestScript -Destination "C:\htmlrep-bin\guest_iis_html_report.ps1" -Force
    if ($Msi) {
        $name = Split-Path -Leaf $Msi
        Copy-Item -ToSession $session -Path $Msi -Destination "C:\htmlrep-bin\$name" -Force
        $lines = Invoke-Command -Session $session -ArgumentList $name -ScriptBlock {
            param($Name)
            $p = Start-Process msiexec.exe -ArgumentList "/i `"C:\htmlrep-bin\$Name`" /qn /l*v `"C:\htmlrep-bin\$Name.log`"" -Wait -PassThru
            "INSTALL | msiexec exit | $($p.ExitCode)"
            "INSTALL | module version | " + (Get-Item "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll").VersionInfo.FileVersion
        }
        foreach ($l in $lines) { Say "$l" }
    }
    try {
        foreach ($phase in @("setup", "run")) {
            $lines = Invoke-Command -Session $session -ArgumentList $phase, $Port -ScriptBlock {
                param($Phase, $Port)
                & "C:\htmlrep-bin\guest_iis_html_report.ps1" -Phase $Phase -Port $Port
            }
            foreach ($l in $lines) {
                Say "$l"
                if ("$l" -match '^SUMMARY (\d+) (\d+)$') {
                    $sawSummary = $true
                    if ([int]$Matches[2] -gt 0 -or [int]$Matches[1] -eq 0) { $exitCode = 1 }
                }
            }
        }
        if (-not $sawSummary) { Say "ERROR | the run printed no summary"; $exitCode = 1 }
    } finally {
        $lines = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock {
            param($Port)
            & "C:\htmlrep-bin\guest_iis_html_report.ps1" -Phase teardown -Port $Port
            Remove-Item -Recurse -Force "C:\htmlrep-bin" -ErrorAction SilentlyContinue
        }
        foreach ($l in $lines) { Say "$l" }
    }
} catch {
    Say ("ERROR | {0}" -f $_.Exception.Message)
    $exitCode = 1
} finally {
    Remove-PSSession $session
}
exit $exitCode
