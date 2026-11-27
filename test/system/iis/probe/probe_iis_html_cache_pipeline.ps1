# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# probe_iis_html_cache_pipeline.ps1 - Records, on a real IIS, the pipeline
# facts the HTML cache depends on: where other modules' response headers
# (customHeaders, clientCache) appear relative to the PageSpeed module's
# send notification, whether a module can remove them from its own answer,
# where static and dynamic compression run and how one response opts out,
# whether writes after a PENDING send re-enter the send notification, what
# the more-data flag looks like for a streamed origin, how a worker process
# reads its sites' bindings under application pool isolation, how a web
# garden spreads requests, how the kernel and output caches interact with a
# module answer, and how IIS cooks unusual request-path forms.
#
# Builds iis_pipeline_probe.cc on this host, copies it into a guest that has
# IIS and the PageSpeed module installed (over PowerShell Direct), installs
# four registrations beside the PageSpeed module, drives the request matrix in the
# guest and prints its records, then restores the guest's IIS configuration
# from the backup it took first. Every result line is INFO; the probe
# records behaviour and does not judge it.
#
# Exit codes: 0 the matrix ran; 2 precondition not met; 1 anything else.
#
# Run as Administrator on the Hyper-V host, against a lab guest. -VMName
# and -Credential (the guest Administrator) are required; the credential is
# never printed. The guest is changed for the run, so the script refuses
# the guest a CI gate restores (CI_IIS_VM_NAME, docs/ci/runner-environment.md),
# and refuses a guest that still holds the configuration backup of an
# earlier run that did not finish. Windows PowerShell 5.1 compatible;
# ASCII only.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$VMName,
    [Parameter(Mandatory = $true)][System.Management.Automation.PSCredential]$Credential,
    [string]$WorkDir = (Join-Path $env:TEMP "iis-pipeline-probe"),
    [int]$Port = 8201,
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

& (Join-Path $PSScriptRoot "build_iis_pipeline_probe.ps1") -OutDir $WorkDir
$dll = Join-Path $WorkDir "iis_pipeline_probe.dll"
$guestScript = Join-Path $PSScriptRoot "guest_iis_pipeline_probe.ps1"

$session = New-PSSession -VMName $VMName -Credential $Credential
$exitCode = 0
try {
    $pre = Invoke-Command -Session $session -ScriptBlock {
        if (-not (Test-Path "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll")) { return "module not installed" }
        # A backup left by an earlier run that did not finish holds the
        # clean configuration; never replace it.
        if (Test-Path "$env:windir\system32\inetsrv\backup\pipeprobe-before") {
            return "an earlier run left the IIS backup pipeprobe-before: restore it (appcmd restore backup pipeprobe-before), delete it, then run again"
        }
        foreach ($f in @("Web-Dyn-Compression", "Web-Stat-Compression", "Web-Http-Tracing", "Web-Asp-Net45")) {
            $w = Get-WindowsFeature -Name $f
            if ($w -eq $null -or -not $w.Installed) { return "feature $f not installed" }
        }
        New-Item -ItemType Directory -Force "C:\pipeprobe\bin" | Out-Null
        "ok"
    }
    if ("$pre" -ne "ok") { Say "PRECONDITION | $pre"; exit 2 }
    Copy-Item -ToSession $session -Path $dll -Destination "C:\pipeprobe\bin\iis_pipeline_probe.dll" -Force
    Copy-Item -ToSession $session -Path $guestScript -Destination "C:\pipeprobe\bin\guest_iis_pipeline_probe.ps1" -Force
    $version = Invoke-Command -Session $session -ScriptBlock { (Get-Item "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll").VersionInfo.FileVersion }
    Say "SETUP | module version | $version"
    try {
        foreach ($phase in @("setup", "run")) {
            $lines = Invoke-Command -Session $session -ArgumentList $phase, $Port -ScriptBlock {
                param($Phase, $Port)
                & "C:\pipeprobe\bin\guest_iis_pipeline_probe.ps1" -Phase $Phase -Port $Port
            }
            foreach ($l in $lines) { Say "$l" }
        }
    } finally {
        $lines = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock {
            param($Port)
            & "C:\pipeprobe\bin\guest_iis_pipeline_probe.ps1" -Phase teardown -Port $Port
            Remove-Item -Recurse -Force "C:\pipeprobe" -ErrorAction SilentlyContinue
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
