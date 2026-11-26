# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# guest_iis_html_conformance.ps1 - The guest half of the IIS HTML cache
# conformance run (rig_iis_html_conformance.ps1 runs it over PowerShell
# Direct). Run in a lab guest with IIS 10 and the PageSpeed MSI installed
# with its optimizer.
#
# -Phase setup     takes an IIS configuration backup and provisions what the
#                  IIS provider (daemon_html_provider_iis.py) drives:
#                    * the IIS features it needs (ASP.NET 4.x, basic
#                      authentication, static and dynamic compression) and,
#                      when the rig brought its installer, the URL Rewrite
#                      module;
#                    * a local user u (password p) for the rows that ask
#                      for server authentication, with the guest's password
#                      rules relaxed to allow it;
#                    * C:\dhc: the site (an ASP.NET origin, DhcOrigin.cs),
#                      its state, logs and file caches;
#                    * pool and site "dhc" on http 80 and https 443 (the
#                      Default Web Site is stopped; a self-signed
#                      certificate for www.example.test), every file
#                      kernel-cacheable on its first hit,
#                      text/markdown in the dynamic compression types;
#                    * the optimizer service in markdown-static mode with
#                      agent-optimize on.
# -Phase run       runs run_iis_html_conformance.py with the embedded
#                  Python in C:\dhc-bin\python and prints its lines
# -Phase teardown  restores the backup, removes the user, the certificate
#                  binding and C:\dhc, and stops the optimizer service if
#                  setup started it
#
# Windows PowerShell 5.1 compatible; ASCII only.

param(
    [ValidateSet("setup", "run", "teardown")][string]$Phase,
    [string]$Rows = ""
)

$ErrorActionPreference = "Stop"
$Root = "C:\dhc"
$Bin = "C:\dhc-bin"
$Backup = "dhc-before"
$Service = "WeAmpPageSpeedOptimizer"
$Volume = "C:\ProgramData\We-Amp\PageSpeed\optimizer\cache"
$Socket = "pagespeed-optimizer"
$AppId = "{6f2d0a7e-2c4b-4c7e-9a51-4d1f2c3b8a90}"
$appcmd = "$env:windir\system32\inetsrv\appcmd.exe"

function Setup {
    if (Test-Path "$env:windir\system32\inetsrv\backup\$Backup") {
        throw "the IIS backup $Backup exists from an earlier run; restore and delete it first"
    }
    if (-not (Test-Path "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll")) { throw "the PageSpeed module is not installed" }
    $svc = Get-Service -Name $Service -ErrorAction SilentlyContinue
    if (-not $svc) { throw "the optimizer service is not installed (an MSI built with -OptimizerDir is needed)" }
    $r = Install-WindowsFeature -Name Web-Asp-Net45, Web-Basic-Auth, Web-Dyn-Compression, Web-Stat-Compression
    Write-Output ("SETUP | features | success={0} restart={1}" -f $r.Success, $r.RestartNeeded)
    # The URL Rewrite module, when the rig brought its installer.
    $rewrite = Test-Path "$env:windir\system32\inetsrv\rewrite.dll"
    if (-not $rewrite -and (Test-Path "$Bin\url-rewrite.msi")) {
        $p = Start-Process msiexec.exe -ArgumentList "/i `"$Bin\url-rewrite.msi`" /qn /norestart" -Wait -PassThru
        $rewrite = $p.ExitCode -eq 0 -and (Test-Path "$env:windir\system32\inetsrv\rewrite.dll")
    }
    Write-Output ("SETUP | url rewrite | {0}" -f $(if ($rewrite) { "installed" } else { "absent" }))
    Import-Module WebAdministration
    & $appcmd add backup $Backup | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "taking the IIS configuration backup failed" }

    # A user for basic authentication, with the password the rows name.
    $inf = "$env:TEMP\dhc-secpol.inf"
    & secedit /export /cfg $inf /quiet | Out-Null
    (Get-Content $inf) -replace '^PasswordComplexity\s*=.*', 'PasswordComplexity = 0' -replace '^MinimumPasswordLength\s*=.*', 'MinimumPasswordLength = 0' | Set-Content $inf
    & secedit /configure /db "$env:TEMP\dhc-secpol.sdb" /cfg $inf /areas SECURITYPOLICY /quiet | Out-Null
    & net user u p /add /y 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "creating the local user u failed" }

    foreach ($d in @("$Root\site\App_Code", "$Root\state", "$Root\logs", "$Root\cache", "$Root\cache-flush")) {
        New-Item -ItemType Directory -Force $d | Out-Null
    }
    Copy-Item "$Bin\DhcOrigin.cs" "$Root\site\App_Code\DhcOrigin.cs" -Force
    if ($rewrite) { Set-Content -LiteralPath "$Root\url-rewrite.txt" -Value "1" -Encoding ASCII }
    & icacls.exe $Root /grant "IIS_IUSRS:(OI)(CI)M" | Out-Null

    $cert = New-SelfSignedCertificate -DnsName "www.example.test" -CertStoreLocation "Cert:\LocalMachine\My"
    Set-Content -LiteralPath "$Root\cert.txt" -Value $cert.Thumbprint -Encoding ASCII
    & netsh http add sslcert ipport=0.0.0.0:443 certhash=$($cert.Thumbprint) appid=$AppId | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "binding the certificate to 443 failed" }

    if (Test-Path "IIS:\Sites\dhc") { Remove-Website -Name dhc }
    if (Test-Path "IIS:\AppPools\dhc") { Remove-WebAppPool -Name dhc }
    New-WebAppPool -Name dhc | Out-Null
    Set-ItemProperty "IIS:\AppPools\dhc" -Name managedRuntimeVersion -Value "v4.0"
    & $appcmd set config -section:system.applicationHost/applicationPools "/+[name='dhc'].environmentVariables.[name='PAGESPEED_TEST_HTML_COMMIT_HOLD_FILE',value='$Root\state\commit.hold']" /commit:apphost | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "setting the pool's environment failed" }
    if (Test-Path "IIS:\Sites\Default Web Site") { Stop-Website -Name "Default Web Site" }
    New-Website -Name dhc -Port 80 -HostHeader "www.example.test" -PhysicalPath "$Root\site" -ApplicationPool dhc | Out-Null
    New-WebBinding -Name dhc -Protocol https -Port 443 -HostHeader "www.example.test"
    & $appcmd set config dhc -section:system.webServer/serverRuntime /frequentHitThreshold:1 /commit:apphost | Out-Null
    & $appcmd set config -section:system.webServer/httpCompression "/+dynamicTypes.[mimeType='text/markdown',enabled='True']" /commit:apphost | Out-Null
    & $appcmd set config -section:system.webServer/httpCompression /dynamicCompressionDisableCpuUsage:100 /staticCompressionDisableCpuUsage:100 /minFileSizeForComp:1 /commit:apphost | Out-Null
    # The authentication sections are set per location by the provider.
    & $appcmd unlock config -section:system.webServer/security/authentication/basicAuthentication | Out-Null
    & $appcmd unlock config -section:system.webServer/security/authentication/anonymousAuthentication | Out-Null

    # The optimizer in markdown-static mode with agent-optimize on.
    $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$Service"
    $image = (Get-ItemProperty $key).ImagePath
    if ($image -notmatch '--html-mode') {
        Set-ItemProperty $key -Name ImagePath -Value "$image --html-mode markdown-static --agent-optimize"
    }
    if ($svc.Status -ne "Running") {
        & sc.exe config $Service start= demand | Out-Null
        Start-Service -Name $Service
        Set-Content -LiteralPath "$Root\service-started.txt" -Value "1" -Encoding ASCII
    } else {
        Restart-Service -Name $Service
    }
    (Get-Service -Name $Service).WaitForStatus("Running", [TimeSpan]::FromSeconds(30))
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath "MACHINE/WEBROOT/APPHOST" | ForEach-Object { $_.name })
    Write-Output ("SETUP | modules | first={0} pagespeed_at={1} count={2}" -f $mods[0], [Array]::IndexOf($mods, "PageSpeedModule"), $mods.Count)
    $lib = Get-ChildItem "C:\Program Files\We-Amp\PageSpeed" -Recurse -Filter pagespeed.dll | Select-Object -First 1
    if (-not $lib) { throw "no optimizer client library under the install directory" }
    Set-Content -LiteralPath "$Root\library.txt" -Value $lib.FullName -Encoding ASCII
    Write-Output ("SETUP | done | library={0} os={1}" -f $lib.Name, (Get-CimInstance Win32_OperatingSystem).Caption)
}

function Run {
    $env:PAGESPEED_DHC_IIS_DIR = $Root
    $env:PAGESPEED_DHC_IIS_LIBRARY = (Get-Content "$Root\library.txt").Trim()
    $env:PAGESPEED_DHC_IIS_VOLUME = $Volume
    $env:PAGESPEED_DHC_IIS_SOCKET = $Socket
    $pyArgs = @("$Bin\run_iis_html_conformance.py", "$Bin\system")
    if ($Rows) { $pyArgs += ($Rows -split ",") }
    # Python writes its own errors to stderr: they are lines of the run,
    # not a failure of this script.
    $ErrorActionPreference = "Continue"
    & "$Bin\python\python.exe" @pyArgs 2>&1 | ForEach-Object { "$_" }
}

function Teardown {
    & $appcmd restore backup $Backup | Out-Null
    & $appcmd delete backup $Backup | Out-Null
    & netsh http delete sslcert ipport=0.0.0.0:443 | Out-Null
    if (Test-Path "$Root\cert.txt") {
        $t = (Get-Content "$Root\cert.txt").Trim()
        Remove-Item "Cert:\LocalMachine\My\$t" -ErrorAction SilentlyContinue
    }
    & net user u /delete 2>&1 | Out-Null
    if (Test-Path "$Root\service-started.txt") {
        Stop-Service -Name $Service -ErrorAction SilentlyContinue
        & sc.exe config $Service start= disabled | Out-Null
    }
    Start-Sleep -Seconds 2
    Remove-Item -Recurse -Force $Root -ErrorAction SilentlyContinue
    Write-Output "TEARDOWN | done"
}

switch ($Phase) {
    "setup" { Setup }
    "run" { Run }
    "teardown" { Teardown }
}
