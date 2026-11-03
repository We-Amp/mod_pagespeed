# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# test_iis_self_fetch_access.ps1 - Pins that the PageSpeed module's own
# fetches do not pass IIS address restrictions (IP and Domain Restrictions).
#
# The module fetches the inputs of an optimized resource from the same server,
# over the address the request arrived on. A rule that admits the server's own
# addresses (loopback and the address the request arrived on) must not admit
# that fetch on behalf of a client the rule refuses; otherwise a refused client
# reads the restricted resource by asking for an optimized URL whose input
# lies under the rule.
#
# Runs on the Hyper-V host and drives a guest with the module installed over
# PowerShell Direct, because the refused client must be a different machine.
# Observed on the IIS 10 / Windows Server 2016 lab, not established as general
# facts: inside one Windows host every non-loopback address was the server's
# own, a connection to loopback from a non-loopback source was refused, and an
# IIS rule admitting 127.0.0.1 admitted all of 127.0.0.0/8.
#
# In the guest the test provisions its own site on $Port with
#   open/plain.css, open/page.html (links plain.css and ../local_only/held.css)
#   open/primed.html (links plain.css and ../local_only/primed.css)
#   local_only/held.css, local_only/held.txt, local_only/primed.css
# and a location rule on local_only/: unlisted addresses denied, 127.0.0.1,
# ::1 and the guest's own address admitted. The host is the refused client.
# Checks:
#   control       a loopback client in the guest reads local_only/ (200)
#   direct        the host is refused local_only/held.css (403)
#   open          the host gets the open stylesheet optimized
#   held          the host never gets held.css's content through the optimized
#                 URL /open/A...,_local_only,_held.css.pagespeed.cf.0.css,
#                 asked past the count after which IIS may cache a response
#   page          the open page never links an optimized URL carrying it
#   primed        after a loopback client in the guest read primed.css, the
#                 host still never gets its content through the optimized URLs
#                 naming it relatively, absolutely and combined, nor through
#                 open/primed.html with stylesheet combining and inlining on;
#                 the loopback client still reads it
#   copier        the host sending the module's User-Agent is refused
#   admin         the module's admin page: 200 to loopback in the guest, not
#                 200 to the host
#   log           the IIS request log shows the module's fetches with client
#                 address 224.0.0.0 (403 for held.css, 200 for plain.css) and
#                 the host's copied User-Agent with the host's own address
# The site, its rule, its directory and its guest firewall rule are removed in
# finally, even on failure.
#
# Exit codes: 0 all checks passed; 1 a check failed; 2 precondition not met.
#
# Run as Administrator on the Hyper-V host. The guest Administrator credential
# is -Credential, or else the runner environment's CI_IIS_VM_ADMIN_PASSWORD
# (docs/ci/runner-environment.md); it is never printed. Windows PowerShell 5.1
# compatible; ASCII only.

[CmdletBinding()]
param(
    [string]$VMName = $env:CI_IIS_VM_NAME,
    [System.Management.Automation.PSCredential]$Credential = $null,
    [string]$SiteName = "ps-self-fetch",
    [int]$Port = 8093,
    [int]$PollSeconds = 60,
    [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"
$script:Failures = 0
$script:Ran = 0
$OpenMarker = "sfa-open-marker"
$HeldMarker = "sfa-held-marker"
$PrimedMarker = "sfa-primed-marker"
# ",_" is an escaped "/"; "A." is the stylesheet filter's name prefix; the
# hash is not checked when the module serves an optimized URL.
$OpenOptimized = "/open/A.plain.css.pagespeed.cf.0.css"
$HeldOptimized = "/open/A...,_local_only,_held.css.pagespeed.cf.0.css"
$FetchUa = "WinHttp mod_pagespeed "

function Say([string]$Line) {
    Write-Host $Line
    if ($OutFile) { Add-Content -LiteralPath $OutFile -Value $Line -Encoding ASCII }
}

function Check([string]$Case, [bool]$Ok, [string]$Detail) {
    $script:Ran++
    if (-not $Ok) { $script:Failures++ }
    Say ("{0} | {1} | {2}" -f $Case, $Detail, $(if ($Ok) { "PASS" } else { "FAIL" }))
}

# One HTTP/1.1 GET over a fresh connection; returns status, head, body.
# Defined as text so the same function runs on the host and in the guest.
$RawText = @'
function Invoke-Raw([string]$Ip, [int]$Port, [string]$Path, [hashtable]$Headers = @{}) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = 20000
    $client.SendTimeout = 20000
    $client.Connect($Ip, $Port)
    try {
        $stream = $client.GetStream()
        $req = "GET $Path HTTP/1.1`r`nHost: localhost:$Port`r`nConnection: close`r`n"
        foreach ($k in $Headers.Keys) { $req += "${k}: $($Headers[$k])`r`n" }
        $req += "`r`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($req)
        $stream.Write($bytes, 0, $bytes.Length)
        $ms = New-Object IO.MemoryStream
        $stream.CopyTo($ms)
    } finally {
        $client.Close()
    }
    $text = [Text.Encoding]::UTF8.GetString($ms.ToArray())
    $parts = $text -split "`r`n`r`n", 2
    $status = 0
    if ((($parts[0] -split "`r`n")[0]) -match '^HTTP/\d\.\d (\d{3})') { $status = [int]$Matches[1] }
    [pscustomobject]@{ Status = $status; Head = $parts[0]; Body = $(if ($parts.Count -gt 1) { $parts[1] } else { "" }) }
}
'@
. ([scriptblock]::Create($RawText))

function Wait-For([scriptblock]$Probe, [string]$What) {
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline) {
        $value = & $Probe
        if ($value) { return $value }
        Start-Sleep -Milliseconds 500
    }
    throw "timed out waiting for $What"
}

if (-not $VMName) { Say "PRECONDITION | no -VMName and no CI_IIS_VM_NAME"; exit 2 }
if (-not $Credential) {
    if (-not $env:CI_IIS_VM_ADMIN_PASSWORD) { Say "PRECONDITION | no -Credential and no CI_IIS_VM_ADMIN_PASSWORD"; exit 2 }
    $Credential = New-Object System.Management.Automation.PSCredential("Administrator",
        (ConvertTo-SecureString $env:CI_IIS_VM_ADMIN_PASSWORD -AsPlainText -Force))
}

$session = New-PSSession -VMName $VMName -Credential $Credential
$exitCode = 0
try {
    # The same request function in the guest, for its loopback client.
    Invoke-Command -Session $session -ScriptBlock { param($t) Set-Item function:global:Invoke-Raw ([scriptblock]::Create($t + "`nInvoke-Raw @args")) } -ArgumentList $RawText

    # Guest setup: site, files, firewall opening for the host.
    $setup = Invoke-Command -Session $session -ArgumentList $SiteName, $Port, $OpenMarker, $HeldMarker, $PrimedMarker -ScriptBlock {
        param($SiteName, $Port, $OpenMarker, $HeldMarker, $PrimedMarker)
        $ErrorActionPreference = "Stop"
        Import-Module WebAdministration
        if (-not (Test-Path "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll")) { return "module not installed" }
        $f = Get-WindowsFeature -Name Web-IP-Security
        if ($f -eq $null -or -not $f.Installed) { return "feature Web-IP-Security not installed" }
        $root = "C:\inetpub\$SiteName"
        if (Test-Path "IIS:\Sites\$SiteName") { Remove-Website -Name $SiteName }
        if (Test-Path "IIS:\AppPools\$SiteName") { Remove-WebAppPool -Name $SiteName }
        New-Item -ItemType Directory -Force "$root\open", "$root\local_only" | Out-Null
        [IO.File]::WriteAllText("$root\open\plain.css", "/* open stylesheet */`r`n.$OpenMarker   {   margin :  0 ;   }`r`n")
        [IO.File]::WriteAllText("$root\open\page.html",
            "<html><head><link rel=`"stylesheet`" href=`"plain.css`"><link rel=`"stylesheet`" href=`"../local_only/held.css`"></head><body>open page</body></html>`r`n")
        [IO.File]::WriteAllText("$root\local_only\held.css", "/* held stylesheet */`r`n.$HeldMarker   {   margin :  0 ;   }`r`n")
        [IO.File]::WriteAllText("$root\local_only\held.txt", "$HeldMarker`r`n")
        [IO.File]::WriteAllText("$root\local_only\primed.css", "/* primed stylesheet */`r`n.$PrimedMarker   {   margin :  0 ;   }`r`n")
        [IO.File]::WriteAllText("$root\open\primed.html",
            "<html><head><link rel=`"stylesheet`" href=`"plain.css`"><link rel=`"stylesheet`" href=`"../local_only/primed.css`"></head><body>open page</body></html>`r`n")
        [IO.File]::WriteAllText("$root\open\ready.txt", "ready`r`n")
        [IO.File]::WriteAllText("$root\pagespeed.config", (@(
            "pagespeed on",
            "pagespeed RewriteLevel PassThrough",
            "pagespeed EnableFilters rewrite_css"
        ) -join "`r`n") + "`r`n")
        New-WebAppPool -Name $SiteName | Out-Null
        New-Website -Name $SiteName -Port $Port -PhysicalPath $root -ApplicationPool $SiteName | Out-Null
        # Start from an empty module cache for this site (IIS gives a site of
        # one name the same id), so an earlier run's results are not served.
        $cache = "C:\ProgramData\We-Amp\PageSpeed\cache\{0}.ROOT" -f (Get-Website -Name $SiteName).id
        Remove-Item -Recurse -Force $cache -ErrorAction SilentlyContinue
        New-NetFirewallRule -DisplayName "$SiteName test" -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow | Out-Null
        $ip = (Get-NetIPAddress -AddressFamily IPv4 |
            Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
            Select-Object -First 1).IPAddress
        if (-not $ip) { return "the guest has no non-loopback IPv4 address" }
        "ok $ip"
    }
    if (-not ("$setup" -like "ok *")) { throw "PRECONDITION: $setup" }
    $GuestIp = ("$setup" -split " ")[1]
    $StartUtc = (Get-Date).ToUniversalTime().AddSeconds(-5)
    Say "SETUP | guest $GuestIp, site $SiteName on $Port"
    # Readiness on a file of its own: no client asks for the stylesheets
    # directly, so their content reaches the module only through its fetches.
    $null = Wait-For { (Invoke-Raw $GuestIp $Port "/open/ready.txt").Status -eq 200 } "the site to answer the host"

    # The rule: unlisted addresses denied; loopback and the guest's own address admitted.
    Invoke-Command -Session $session -ArgumentList $SiteName, $GuestIp -ScriptBlock {
        param($SiteName, $GuestIp)
        Import-Module WebAdministration
        $loc = "$SiteName/local_only"; $f = "system.webServer/security/ipSecurity"
        Set-WebConfigurationProperty -PSPath "MACHINE/WEBROOT/APPHOST" -Location $loc -Filter $f -Name "allowUnlisted" -Value $false
        foreach ($a in "127.0.0.1", "::1", $GuestIp) {
            Add-WebConfigurationProperty -PSPath "MACHINE/WEBROOT/APPHOST" -Location $loc -Filter $f -Name "." -Value @{ ipAddress = $a; allowed = "true" }
        }
    }
    $null = Wait-For { (Invoke-Raw $GuestIp $Port "/local_only/held.txt").Status -eq 403 } "the rule to refuse the host"
    $c = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock { param($Port) Invoke-Raw "127.0.0.1" $Port "/local_only/held.txt" }
    Check "control" ($c.Status -eq 200 -and $c.Body.Contains($HeldMarker)) ("loopback client in the guest, held.txt -> {0}" -f $c.Status)

    $r = Invoke-Raw $GuestIp $Port "/local_only/held.css"
    Check "direct" ($r.Status -eq 403 -and -not $r.Body.Contains($HeldMarker)) ("host, held.css -> {0}, marker {1}" -f $r.Status, $r.Body.Contains($HeldMarker))

    $open = $null
    try {
        $open = Wait-For { $x = Invoke-Raw $GuestIp $Port $OpenOptimized; if ($x.Status -eq 200 -and $x.Body.Contains($OpenMarker) -and -not $x.Body.Contains("/*")) { $x } } "the open stylesheet optimized"
    } catch { }
    Check "open" ($open -ne $null) ("host, {0} optimized -> {1}" -f $OpenOptimized, $(if ($open) { $open.Status } else { "never" }))

    $leaks = 0; $statuses = @()
    for ($i = 0; $i -lt 8; $i++) {
        $h = Invoke-Raw $GuestIp $Port $HeldOptimized
        $statuses += $h.Status
        if ($h.Body.Contains($HeldMarker)) { $leaks++ }
        Start-Sleep -Milliseconds 750
    }
    Check "held" ($leaks -eq 0 -and -not ($statuses -contains 200)) ("host, {0} -> statuses {1}, responses with the held marker {2}" -f $HeldOptimized, ($statuses -join ","), $leaks)

    $page = $null
    for ($i = 0; $i -lt 6; $i++) { $page = Invoke-Raw $GuestIp $Port "/open/page.html"; Start-Sleep -Milliseconds 750 }
    $hrefs = @(); $hrefLeak = $false
    foreach ($m in [regex]::Matches($page.Body, 'href="([^"]*held[^"]*)"')) {
        $href = $m.Groups[1].Value; $hrefs += $href
        if ($href -match '\.pagespeed\.') {
            $target = if ($href.StartsWith("/")) { $href } elseif ($href -match '^https?://[^/]+(/.*)$') { $Matches[1] } else { "/open/" + $href }
            if ((Invoke-Raw $GuestIp $Port $target).Body.Contains($HeldMarker)) { $hrefLeak = $true }
        }
    }
    Check "page" ($page.Status -eq 200 -and -not $hrefLeak) ("host, page.html -> {0}, held hrefs [{1}], leak {2}" -f $page.Status, ($hrefs -join " "), $hrefLeak)

    # Priming: a client the rule admits reads primed.css first, so the server
    # has served it (and the module may have stored that response) before the
    # host asks for anything naming it.
    $p1 = $null
    for ($i = 0; $i -lt 3; $i++) {
        $p1 = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock { param($Port) Invoke-Raw "127.0.0.1" $Port "/local_only/primed.css" }
        Start-Sleep -Milliseconds 750
    }
    Check "primed control" ($p1.Status -eq 200 -and $p1.Body.Contains($PrimedMarker)) ("loopback client in the guest, primed.css -> {0}" -f $p1.Status)
    $primedUrls = @(
        "/open/A...,_local_only,_primed.css.pagespeed.cf.0.css",
        "/open/A.,hlocalhost,3A$Port,_local_only,_primed.css.pagespeed.cf.0.css",
        "/open/plain.css+..,_local_only,_primed.css.pagespeed.cc.0.css")
    $pLeaks = @()
    for ($i = 0; $i -lt 4; $i++) {
        foreach ($u in $primedUrls) {
            $x = Invoke-Raw $GuestIp $Port $u
            if ($x.Body.Contains($PrimedMarker)) { $pLeaks += ("{0} -> {1}" -f $u, $x.Status) }
        }
        Start-Sleep -Milliseconds 750
    }
    Check "primed urls" ($pLeaks.Count -eq 0) ("host, optimized URLs naming primed.css; responses with the marker: {0}" -f ($pLeaks -join "; "))
    $pPageLeak = $false; $pHrefs = @()
    for ($i = 0; $i -lt 8; $i++) {
        $pp = Invoke-Raw $GuestIp $Port "/open/primed.html?PageSpeedFilters=combine_css,inline_css,rewrite_css"
        if ($pp.Body.Contains($PrimedMarker)) { $pPageLeak = $true }
        foreach ($m in [regex]::Matches($pp.Body, 'href="([^"]*\.pagespeed\.[^"]*)"')) { $pHrefs += $m.Groups[1].Value }
        Start-Sleep -Milliseconds 750
    }
    foreach ($href in ($pHrefs | Select-Object -Unique)) {
        $target = if ($href -match '^https?://[^/]+(/.*)$') { $Matches[1] } elseif ($href.StartsWith("/")) { $href } else { "/open/" + $href }
        if ((Invoke-Raw $GuestIp $Port $target).Body.Contains($PrimedMarker)) { $pPageLeak = $true }
    }
    Check "primed page" (-not $pPageLeak) ("host, primed.html and its optimized hrefs [{0}], marker seen {1}" -f (($pHrefs | Select-Object -Unique) -join " "), $pPageLeak)
    $p2 = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock { param($Port) Invoke-Raw "127.0.0.1" $Port "/local_only/primed.css" }
    Check "primed after" ($p2.Status -eq 200 -and $p2.Body.Contains($PrimedMarker)) ("loopback client in the guest, primed.css after -> {0}" -f $p2.Status)

    $cp = Invoke-Raw $GuestIp $Port "/local_only/held.css" @{ "User-Agent" = $FetchUa }
    Check "copier" ($cp.Status -eq 403 -and -not $cp.Body.Contains($HeldMarker)) ("host with the module's User-Agent, held.css -> {0}" -f $cp.Status)

    $a1 = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock { param($Port) Invoke-Raw "127.0.0.1" $Port "/pagespeed_admin/" }
    $a2 = Invoke-Raw $GuestIp $Port "/pagespeed_admin/"
    Check "admin" ($a1.Status -eq 200 -and $a2.Status -ne 200) ("admin page: loopback in the guest {0}, host {1}" -f $a1.Status, $a2.Status)

    # The request log, this run's lines with the module's marker in the User-Agent.
    $rows = @(Invoke-Command -Session $session -ArgumentList $SiteName, $StartUtc -ScriptBlock {
        param($SiteName, $StartUtc)
        Import-Module WebAdministration
        & netsh http flush logbuffer | Out-Null
        Start-Sleep -Seconds 2
        $site = Get-Website -Name $SiteName
        $dir = Join-Path ([Environment]::ExpandEnvironmentVariables($site.logFile.directory)) ("W3SVC{0}" -f $site.id)
        foreach ($file in (Get-ChildItem -LiteralPath $dir -Filter "*.log" -ErrorAction SilentlyContinue)) {
            $fields = @()
            foreach ($line in (Get-Content -LiteralPath $file.FullName)) {
                if ($line -like "#Fields:*") { $fields = ($line.Substring(9).Trim() -split " "); continue }
                if ($line.StartsWith("#") -or -not $fields) { continue }
                $v = $line -split " "; $row = @{}
                for ($i = 0; $i -lt $fields.Count -and $i -lt $v.Count; $i++) { $row[$fields[$i]] = $v[$i] }
                $when = [datetime]::MinValue
                if (-not [datetime]::TryParse(("{0} {1}" -f $row["date"], $row["time"]), [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$when)) { continue }
                if ($when -lt $StartUtc) { continue }
                if (("" + $row["cs(User-Agent)"]).Contains("mod_pagespeed")) {
                    [pscustomobject]@{ Stem = $row["cs-uri-stem"]; Ip = $row["c-ip"]; Status = $row["sc-status"]; Ua = $row["cs(User-Agent)"] }
                }
            }
        }
    })
    foreach ($l in $rows) { Say ("LOG | {0} c-ip={1} sc-status={2} ua={3}" -f $l.Stem, $l.Ip, $l.Status, $l.Ua) }
    $copier = @($rows | Where-Object { $_.Stem -eq "/local_only/held.css" -and $_.Ua -like "WinHttp+mod_pagespeed*" -and $_.Ip -ne "224.0.0.0" -and $_.Ip -ne $GuestIp -and $_.Ip -notlike "127.*" -and $_.Ip -ne "::1" })
    $heldModule = @($rows | Where-Object { $_.Stem -eq "/local_only/held.css" -and ($_.Ip -eq "224.0.0.0" -or $_.Ip -eq $GuestIp -or $_.Ip -like "127.*" -or $_.Ip -eq "::1") })
    $heldBad = @($heldModule | Where-Object { $_.Ip -ne "224.0.0.0" -or $_.Status -ne "403" })
    Check "log held" ($heldModule.Count -gt 0 -and $heldBad.Count -eq 0) ("module fetches of held.css {0}, not 224.0.0.0 with 403: {1}" -f $heldModule.Count, $heldBad.Count)
    $openModule = @($rows | Where-Object { $_.Stem -eq "/open/plain.css" })
    $openBad = @($openModule | Where-Object { $_.Ip -ne "224.0.0.0" -or $_.Status -ne "200" })
    Check "log open" ($openModule.Count -gt 0 -and $openBad.Count -eq 0) ("module fetches of plain.css {0}, not 224.0.0.0 with 200: {1}" -f $openModule.Count, $openBad.Count)
    Check "log copier" ($copier.Count -ge 1) ("host's copied User-Agent logged with the host's address: {0}" -f $copier.Count)

    if ($script:Failures -gt 0) { $exitCode = 1 }
} catch {
    Say ("ERROR | {0}" -f $_.Exception.Message)
    if ($exitCode -eq 0) { $exitCode = 2 }
} finally {
    try {
        Invoke-Command -Session $session -ArgumentList $SiteName -ScriptBlock {
            param($SiteName)
            Import-Module WebAdministration
            Clear-WebConfiguration -PSPath "MACHINE/WEBROOT/APPHOST" -Location "$SiteName/local_only" -Filter "system.webServer/security/ipSecurity" -ErrorAction SilentlyContinue
            try { Remove-WebConfigurationLocation -Name "$SiteName/local_only" -ErrorAction SilentlyContinue } catch { }
            if (Test-Path "IIS:\Sites\$SiteName") { Remove-Website -Name $SiteName }
            if (Test-Path "IIS:\AppPools\$SiteName") { Remove-WebAppPool -Name $SiteName }
            Remove-NetFirewallRule -DisplayName "$SiteName test" -ErrorAction SilentlyContinue
            Remove-Item -Recurse -Force "C:\inetpub\$SiteName" -ErrorAction SilentlyContinue
        }
    } catch { Say ("CLEANUP | {0}" -f $_.Exception.Message) }
    Remove-PSSession $session
}
Say ("RESULT | ran {0}, failed {1}, exit {2}" -f $script:Ran, $script:Failures, $exitCode)
exit $exitCode
