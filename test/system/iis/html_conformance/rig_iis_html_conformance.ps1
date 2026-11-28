# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# rig_iis_html_conformance.ps1 - Runs the HTML cache conformance rows
# (test/system/daemon_html/conformance_cases.json, every live row IIS takes
# part in) against a real IIS in a lab guest, through the IIS provider
# (test/system/pagespeed_test_framework/daemon_html_provider_iis.py).
#
# Copies into the guest over PowerShell Direct: the runner core, the
# provider, the cases, the origin (DhcOrigin.cs), the guest script, an
# embeddable Python (-PythonZip, the python.org "embed-amd64" archive), the
# brotli decoder for the rows that check a br body (-BrotliWheel, the PyPI
# "Brotli" wheel for that Python, cp312 win_amd64, unpacked beside it: the
# guest image has no brotli library, and the guest is restored to a
# checkpoint between runs) and, with -Msi, the package, which it installs
# first. Then runs the guest
# script's setup, run and teardown phases and prints every line. The
# teardown restores the IIS configuration backup the setup took. The MSI
# must carry the optimizer built from the test pin
# (tools/ci/Build-OptimizerPin.ps1).
#
# Exit codes: 0 every row passed; 1 a row failed or the run broke;
# 2 a precondition was not met.
#
# Run as Administrator on the Hyper-V host, against a lab guest. -VMName
# and -Credential (the guest Administrator) are required; the credential is
# never printed. The script refuses the guest a CI gate restores
# (CI_IIS_VM_NAME, docs/ci/runner-environment.md). Windows PowerShell 5.1
# compatible; ASCII only.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$VMName,
    [Parameter(Mandatory = $true)][System.Management.Automation.PSCredential]$Credential,
    [Parameter(Mandatory = $true)][string]$PythonZip,
    # Without it the rows that decode a br body fail.
    [string]$BrotliWheel = "",
    [string]$Msi = "",
    # The IIS URL Rewrite module's installer (Microsoft's rewrite_amd64
    # package), for the rows that need it; without it they fail.
    [string]$UrlRewriteMsi = "",
    # Comma-separated row ids; all rows when empty.
    [string]$Rows = "",
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
foreach ($f in @($PythonZip) + @($(if ($Msi) { $Msi })) + @($(if ($BrotliWheel) { $BrotliWheel }))) {
    if (-not (Test-Path -LiteralPath $f)) { Say "PRECONDITION | $f does not exist"; exit 2 }
}

$here = $PSScriptRoot
$system = Split-Path -Parent (Split-Path -Parent $here)
$framework = Join-Path $system "pagespeed_test_framework"
$stage = Join-Path ([IO.Path]::GetTempPath()) ("dhc-stage-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force "$stage\system\pagespeed_test_framework", "$stage\system\daemon_html" | Out-Null
foreach ($m in @("daemon_html_conformance.py", "daemon_html_provider_iis.py", "stats.py")) {
    Copy-Item (Join-Path $framework $m) "$stage\system\pagespeed_test_framework\$m"
}
# The package's own __init__ imports the HTTP client the other tests use,
# which the embedded Python has not: the run needs only these three modules.
Set-Content -LiteralPath "$stage\system\pagespeed_test_framework\__init__.py" -Value "" -Encoding ASCII
Copy-Item (Join-Path $system "daemon_html\conformance_cases.json") "$stage\system\daemon_html\"
foreach ($f in @("DhcOrigin.cs", "run_iis_html_conformance.py", "guest_iis_html_conformance.ps1", "iis_rows.json")) {
    Copy-Item (Join-Path $here $f) "$stage\$f"
}
Copy-Item -LiteralPath $PythonZip "$stage\python.zip"
# A wheel is a zip archive; Expand-Archive takes only the .zip name.
if ($BrotliWheel) { Copy-Item -LiteralPath $BrotliWheel "$stage\brotli.zip" }
if ($UrlRewriteMsi) { Copy-Item -LiteralPath $UrlRewriteMsi "$stage\url-rewrite.msi" }

$session = New-PSSession -VMName $VMName -Credential $Credential
$exitCode = 0
$sawSummary = $false
try {
    $pre = Invoke-Command -Session $session -ScriptBlock {
        if (Test-Path "$env:windir\system32\inetsrv\backup\dhc-before") {
            return "an earlier run left the IIS backup dhc-before: restore it (appcmd restore backup dhc-before), delete it, then run again"
        }
        Remove-Item -Recurse -Force "C:\dhc-bin" -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "C:\dhc-bin" | Out-Null
        "ok"
    }
    if ("$pre" -ne "ok") { Say "PRECONDITION | $pre"; exit 2 }
    Copy-Item -ToSession $session -Path "$stage\*" -Destination "C:\dhc-bin\" -Recurse -Force
    Invoke-Command -Session $session -ScriptBlock {
        Expand-Archive -LiteralPath "C:\dhc-bin\python.zip" -DestinationPath "C:\dhc-bin\python" -Force
        # The embedded Python's path holds its own directory: the wheel's
        # module goes there.
        if (Test-Path -LiteralPath "C:\dhc-bin\brotli.zip") {
            Expand-Archive -LiteralPath "C:\dhc-bin\brotli.zip" -DestinationPath "C:\dhc-bin\python" -Force
        }
    }
    if ($Msi) {
        $name = Split-Path -Leaf $Msi
        Copy-Item -ToSession $session -Path $Msi -Destination "C:\dhc-bin\$name" -Force
        $lines = Invoke-Command -Session $session -ArgumentList $name -ScriptBlock {
            param($Name)
            $p = Start-Process msiexec.exe -ArgumentList "/i `"C:\dhc-bin\$Name`" /qn /l*v `"C:\dhc-bin\$Name.log`"" -Wait -PassThru
            "INSTALL | msiexec exit | $($p.ExitCode)"
            "INSTALL | module version | " + (Get-Item "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll").VersionInfo.FileVersion
        }
        foreach ($l in $lines) { Say "$l" }
    }
    try {
        foreach ($phase in @("setup", "run")) {
            $lines = Invoke-Command -Session $session -ArgumentList $phase, $Rows -ScriptBlock {
                param($Phase, $Rows)
                & "C:\dhc-bin\guest_iis_html_conformance.ps1" -Phase $Phase -Rows $Rows
            }
            foreach ($l in $lines) {
                Say "$l"
                if ("$l" -match '^SUMMARY (\d+) (\d+) (\d+)$') {
                    $sawSummary = $true
                    if ([int]$Matches[2] -gt 0 -or [int]$Matches[1] -eq 0) { $exitCode = 1 }
                }
            }
        }
        if (-not $sawSummary) { Say "ERROR | the run printed no summary"; $exitCode = 1 }
    } finally {
        $lines = Invoke-Command -Session $session -ScriptBlock {
            & "C:\dhc-bin\guest_iis_html_conformance.ps1" -Phase teardown
            Remove-Item -Recurse -Force "C:\dhc-bin" -ErrorAction SilentlyContinue
        }
        foreach ($l in $lines) { Say "$l" }
    }
} catch {
    Say ("ERROR | {0}" -f $_.Exception.Message)
    $exitCode = 1
} finally {
    Remove-PSSession $session
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
}
exit $exitCode
