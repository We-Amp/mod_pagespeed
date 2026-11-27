# IIS: install and verify mod_pagespeed (native IIS module)

Source: https://modpagespeed.com/docs/install-iis/ and
https://modpagespeed.com/docs/iis-configuration/. Header: `X-Page-Speed`.
The IIS module ships as a signed MSI on the module's GitHub release. All
commands run in an elevated PowerShell on the Windows Server host.

## 1. Prerequisites

Windows Server 2016 or later, IIS 10, 64-bit (no Visual C++
Redistributable needed; the runtime is built in), and no IISpeed on the host: both register the same
handler, so uninstall IISpeed first and run `iisreset`.

```powershell
(Get-CimInstance Win32_OperatingSystem).Caption
(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\InetStp').MajorVersion   # 10 or later
(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' -ErrorAction SilentlyContinue).Installed   # 1
$uninstall = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
Get-ItemProperty $uninstall -ErrorAction SilentlyContinue | Where-Object DisplayName -like 'IISpeed*' | Select-Object DisplayName           # nothing
Get-ItemProperty $uninstall -ErrorAction SilentlyContinue | Where-Object DisplayName -like '*PageSpeed for IIS*' | Select-Object DisplayName   # nothing
Import-Module WebAdministration
Get-Website | Select-Object Name, State, PhysicalPath   # the sites on this host
```

Stop and ask if the module is already installed. On a host with more than one
site, ask the operator which sites to optimize.

## 2. Install

Find the current MSI on https://modpagespeed.com/download/ (section "Direct
downloads"; it links the installer on the module's GitHub release), download it
and the release's `SHA256SUMS` from the same release, and stop on a checksum
mismatch or on an Authenticode signature that is not valid or not from
publisher "We-Amp B.V.". The Authenticode signature is the check for the MSI
itself; a `SHA256SUMS` fetched from the same release only catches a damaged
download. To also check the checksum file, verify the release's
`SHA256SUMS.asc` against the release signing key (fingerprint
`411D EFDB 5713 51D2 B25A  EFCC 9C02 B5BD CCE7 5313`, key:
https://github.com/We-Amp/mod_pagespeed/blob/master/RELEASE-SIGNING-KEY.asc,
steps: https://github.com/We-Amp/mod_pagespeed/blob/master/SECURITY.md#verifying-a-release)
on a host with gpg; that key is not the apt/dnf repository key.

```powershell
$ProgressPreference = 'SilentlyContinue'
$page = (Invoke-WebRequest 'https://modpagespeed.com/download/' -UseBasicParsing).Content
$url = [regex]::Match($page, 'https://github\.com/We-Amp/mod_pagespeed/releases/download/[^/"]+/pagespeed-iis-[^/"]+-win-x64\.msi').Value
if (-not $url) { throw 'IIS MSI link not found on /download/' }
$base = $url.Substring(0, $url.LastIndexOf('/') + 1)
$name = $url.Substring($url.LastIndexOf('/') + 1)
$dir = Join-Path $env:TEMP 'pagespeed-install'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$msi = Join-Path $dir $name
Invoke-WebRequest ($base + $name) -OutFile $msi -UseBasicParsing
Invoke-WebRequest ($base + 'SHA256SUMS') -OutFile (Join-Path $dir 'SHA256SUMS') -UseBasicParsing
$line = Select-String -Path (Join-Path $dir 'SHA256SUMS') -Pattern ('\s\*?' + [regex]::Escape($name) + '$') | Select-Object -First 1
if (-not $line) { throw "$name is not listed in SHA256SUMS" }
$expected = $line.Line.Split(' ')[0]
if ((Get-FileHash -Algorithm SHA256 $msi).Hash -ne $expected) { throw "SHA256 mismatch for $name" }
$sig = Get-AuthenticodeSignature $msi
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch '^CN=We-Amp B\.V\.,') { throw "Authenticode check failed for ${name}: $($sig.Status) $($sig.SignerCertificate.Subject)" }
$msi
```

Running the MSI takes every IIS site on the server down for the duration — the
installer stops WAS and W3SVC and force-kills `w3wp.exe` workers, and in-flight
requests are dropped — for an install, an upgrade, a repair and an uninstall alike. Ask
the operator for a maintenance window before this step and before an uninstall.

Install silently and fail on any exit code other than 0 or 3010 (3010: installed,
a reboot is pending; tell the operator):

```powershell
$log = Join-Path $dir 'install.log'
$p = Start-Process msiexec.exe -ArgumentList "/i `"$msi`" /qn /norestart /l*v `"$log`"" -Wait -PassThru
if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { throw "msiexec failed with exit code $($p.ExitCode); log: $log" }
iisreset
```

The installer registers the module as a native HTTP module and creates the
default cache directory, `%ProgramData%\We-Amp\PageSpeed\cache`. From 2.5.0
on it also installs the optimizer worker as the Windows service
`WeAmpPageSpeedOptimizer`, left disabled; the module works without it.

## 3. Minimal configuration

A site is optimized only when its own application root holds a
`pagespeed.config`. The server-level file
`%ProgramData%\We-Amp\PageSpeed\pagespeed.config` (the installer creates it)
sets defaults for every site that has its own `pagespeed.config`; it turns
nothing on by itself. The installer also puts a copy with `pagespeed on` in the
Default Web Site root, so that site is on after install.

For each site the operator chose:

```powershell
Import-Module WebAdministration
$site = '<site-name>'
$root = [Environment]::ExpandEnvironmentVariables((Get-Website | Where-Object Name -eq $site).PhysicalPath)
$cfg = Join-Path $root 'pagespeed.config'
if (Test-Path $cfg) { Get-Content $cfg } else { Set-Content -Path $cfg -Value 'pagespeed on' -Encoding Ascii; "created $cfg" }
Get-WebApplication -Site $site | Select-Object Path, PhysicalPath   # nested applications
```

If the file already exists, do not overwrite it: it must contain
`pagespeed on`; otherwise stop and ask. A nested IIS application is optimized
only with its own `pagespeed.config` in its own physical path; ask before
adding those. The cache path and other defaults come from the server-level
file. Path matching and the directive list:
https://modpagespeed.com/docs/iis-configuration/

## 4. Verify

```powershell
(Invoke-WebRequest '<site-url>/?mps-verify=agent' -UseBasicParsing).Headers['X-Page-Speed']
```

Pass: a value is printed, normally `on` (the version only with `ExposeVersionInHeaders on`); the header is on every HTML response the
module handles. A new per-site file is picked up on the next request; if
nothing is printed, run `iisreset` once and verify again. Still nothing:
https://modpagespeed.com/docs/troubleshooting/#no-x-mod-pagespeed-or-x-page-speed-header

## 5. Rollback

```powershell
# Off for one site: remove its own pagespeed.config. A server-level
# `pagespeed off` does not turn off a site whose own file says `pagespeed on`.
$root = [Environment]::ExpandEnvironmentVariables((Get-Website | Where-Object Name -eq '<site-name>').PhysicalPath)
$cfg = Join-Path $root 'pagespeed.config'
$createdByThisRun = $true   # $false when the file was there before this run
if ($createdByThisRun) { Remove-Item $cfg } else { Rename-Item $cfg 'pagespeed.config.disabled' }

# Remove the module with the same MSI, then restart IIS
$dir = Join-Path $env:TEMP 'pagespeed-install'
$msi = Join-Path $dir 'pagespeed-iis-<version>-win-x64.msi'
if (-not (Test-Path $msi)) { throw 'MSI not found in TEMP; uninstall mod_pagespeed from Apps and features instead' }
$p = Start-Process msiexec.exe -ArgumentList "/x `"$msi`" /qn /norestart /l*v `"$dir\uninstall.log`"" -Wait -PassThru
if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { throw "msiexec failed with exit code $($p.ExitCode)" }
iisreset
```

The cache and configuration under `%ProgramData%\We-Amp\` and the per-site
`pagespeed.config` files stay on disk. Rolling back a release:
https://modpagespeed.com/docs/uninstall/#roll-back-to-the-previous-release
