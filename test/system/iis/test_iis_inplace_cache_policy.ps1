# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# test_iis_inplace_cache_policy.ps1 - Pins that responses the module takes up
# for in-place optimization stay out of IIS's own response caches, with IIS's
# caching at its defaults.
#
# HTTP.sys keeps a kernel copy of a response it sees requested often, and that
# copy answers later requests for the URL without the module, to every client
# alike: a plain original would stop optimization for the URL, and an
# optimized image in a format one client accepts would reach clients that do
# not accept it.  The module keeps every response of an in-place request, and
# the answers to its own input fetches, out of IIS's caches; this test reads
# what HTTP.sys holds (`netsh http show cachestate`) after traffic that
# exercises each kind of response.
#
# Runs on the Hyper-V host and drives a guest with the module installed over
# PowerShell Direct; the host is the client.  The test site on $Port leaves
# IIS caching at the server defaults and serves:
#   assets/site.css, assets/head.css   stylesheets
#   assets/x (served as text/css) beside assets/x#y.css
#   assets/pic.png                     the IIS start page image
#   pages/ajax.html, pages/v6.html     HTML pages
# Checks (PASS/FAIL; INFO rows report without judging):
#   defaults   kernel caching is on for the server (precondition)
#   plain      assets/site.css without a query, polled: optimized in place
#              and still optimized after 20 more requests
#   head       HEAD requests for assets/head.css, then GET: optimized
#   left       assets/x%23y.css (left to IIS by the module): IIS's own file
#   image      a client that accepts WebP until the image is optimized, then
#              a client that does not: the second never gets WebP or AVIF
#   rules      files behind a file-level IP rule and a deny-anonymous rule
#              (admitted: a loopback client in the guest; for the latter,
#              Basic authentication): (a) after the admitted client read the
#              plain URL four times within a few seconds, (b) after the module
#              served an in-place answer for it, the host (refused) is still
#              refused and never gets the file's content
#   script     assets/app.js polled: optimized in place
#   ajax       pages/ajax.html requested as an AJAX request (left plain by the
#              module) four times within a few seconds, then by a browser:
#              every browser answer went through the module, and HTTP.sys does
#              not hold the page
#   ipv6       pages/v6.html requested in the guest over IPv6 loopback with a
#              bracketed IPv6 host (and X-Forwarded-Proto, so the module drops
#              the port from the URL it builds): the module rewrites the page
#   keep       assets/doc.pdf and assets/font.woff, types the module does not
#              optimize, requested often: HTTP.sys keeps both (IIS caching
#              stays on for them)
#   headers    INFO: Cache-Control and Expires of the optimized stylesheet
#   cache      HTTP.sys holds none of the site's stylesheets, scripts,
#              images or rule-protected files
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
    [string]$SiteName = "ps-cache-policy",
    [int]$Port = 8098,
    [int]$PollSeconds = 60,
    [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"
$script:Failures = 0
$script:Ran = 0
$BrowserUa = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"
$WebpAccept = "image/avif,image/webp,image/apng,image/*,*/*;q=0.8"
$PlainAccept = "image/png,image/*;q=0.8,*/*;q=0.5"

function Say([string]$Line) {
    Write-Host $Line
    if ($OutFile) { Add-Content -LiteralPath $OutFile -Value $Line -Encoding ASCII }
}

function Check([string]$Case, [bool]$Ok, [string]$Detail) {
    $script:Ran++
    if (-not $Ok) { $script:Failures++ }
    Say ("{0} | {1} | {2}" -f $Case, $Detail, $(if ($Ok) { "PASS" } else { "FAIL" }))
}

# One HTTP/1.1 request over a fresh connection; the body is kept as bytes.
$RawText = @'
function Invoke-Raw([string]$Ip, [int]$Port, [string]$Target, [string]$Method = "GET", [hashtable]$Headers = @{}, [string]$HostHeader = "") {
    $family = [System.Net.Sockets.AddressFamily]::InterNetwork
    if ($Ip.Contains(":")) { $family = [System.Net.Sockets.AddressFamily]::InterNetworkV6 }
    $client = New-Object System.Net.Sockets.TcpClient($family)
    $client.ReceiveTimeout = 20000
    $client.SendTimeout = 20000
    $client.Connect($Ip, $Port)
    try {
        $stream = $client.GetStream()
        if (-not $HostHeader) { $HostHeader = "localhost:$Port" }
        $req = "$Method $Target HTTP/1.1`r`nHost: $HostHeader`r`nConnection: close`r`n"
        foreach ($k in $Headers.Keys) { $req += "${k}: $($Headers[$k])`r`n" }
        $req += "`r`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($req)
        $stream.Write($bytes, 0, $bytes.Length)
        $ms = New-Object IO.MemoryStream
        $stream.CopyTo($ms)
    } finally {
        $client.Close()
    }
    $text = [Text.Encoding]::GetEncoding(28591).GetString($ms.ToArray())
    $i = $text.IndexOf("`r`n`r`n")
    $head = $text.Substring(0, [Math]::Max($i, 0))
    $status = 0
    if (($head -split "`r`n")[0] -match '^HTTP/\d\.\d (\d{3})') { $status = [int]$Matches[1] }
    $type = ([regex]::Match($head, '(?im)^Content-Type:\s*([^;\r\n]+)')).Groups[1].Value.Trim()
    [pscustomobject]@{ Status = $status; Head = $head; Body = $text.Substring($i + 4); Length = $ms.Length - $i - 4; Type = $type }
}
'@
. ([scriptblock]::Create($RawText))

# A stylesheet answer with no comment and no runs of spaces is the optimized one.
function CssOptimized($r) { $r.Status -eq 200 -and $r.Body.Contains("cpt-marker") -and -not $r.Body.Contains("/*") -and -not $r.Body.Contains("   ") }

if (-not $VMName) { Say "PRECONDITION | no -VMName and no CI_IIS_VM_NAME"; exit 2 }
if (-not $Credential) {
    if (-not $env:CI_IIS_VM_ADMIN_PASSWORD) { Say "PRECONDITION | no -Credential and no CI_IIS_VM_ADMIN_PASSWORD"; exit 2 }
    $Credential = New-Object System.Management.Automation.PSCredential("Administrator",
        (ConvertTo-SecureString $env:CI_IIS_VM_ADMIN_PASSWORD -AsPlainText -Force))
}

$session = New-PSSession -VMName $VMName -Credential $Credential
$exitCode = 0
try {
    $setup = Invoke-Command -Session $session -ArgumentList $SiteName, $Port -ScriptBlock {
        param($SiteName, $Port)
        $ErrorActionPreference = "Stop"
        Import-Module WebAdministration
        if (-not (Test-Path "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll")) { return "module not installed" }
        if (-not (Test-Path "C:\inetpub\wwwroot\iisstart.png")) { return "C:\inetpub\wwwroot\iisstart.png missing" }
        $apphost = "MACHINE/WEBROOT/APPHOST"
        $ca = Get-WebConfiguration -PSPath $apphost -Filter "system.webServer/caching"
        $root = "C:\inetpub\$SiteName"
        if (Test-Path "IIS:\Sites\$SiteName") { Remove-Website -Name $SiteName }
        if (Test-Path "IIS:\AppPools\$SiteName") { Remove-WebAppPool -Name $SiteName }
        Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$root\assets", "$root\secure", "$root\pages" | Out-Null
        foreach ($f in "ajax.html", "v6.html") {
            [IO.File]::WriteAllText("$root\pages\$f", "<html><head><title>cpt</title></head><body>`r`n<p>cpt-page</p>`r`n</body></html>`r`n")
        }
        $css = "/* stylesheet */`r`n.cpt-marker   {   color :  #ff0000 ;   margin :  0px  0px ;   }`r`n"
        foreach ($f in "site.css", "head.css", "x") { [IO.File]::WriteAllText("$root\assets\$f", $css) }
        [IO.File]::WriteAllText("$root\assets\x#y.css", "/* the other file */`r`n.cpt-other   {   color :  #00ff00 ;   }`r`n")
        Copy-Item "C:\inetpub\wwwroot\iisstart.png" "$root\assets\pic.png"
        [IO.File]::WriteAllText("$root\assets\app.js", "/* a script */`r`nfunction  cptHello ( name )  {`r`n    var  greeting  =  'hello '  +  name ;`r`n    return  greeting ;`r`n}`r`n")
        [IO.File]::WriteAllText("$root\assets\doc.pdf", "%PDF-1.4`r`n% a document the module does not optimize`r`n%%EOF`r`n")
        [IO.File]::WriteAllText("$root\assets\font.woff", "wOFF a font the module does not optimize")
        foreach ($f in "ip-a.css", "ip-b.css", "auth-a.css", "auth-b.css") {
            [IO.File]::WriteAllText("$root\secure\$f", "/* held */`r`n.cpt-held   {   color :  #0000ff ;   }`r`n")
        }
        [IO.File]::WriteAllText("$root\open-ready.txt", "ready`r`n")
        [IO.File]::WriteAllText("$root\pagespeed.config", (@(
            "pagespeed on",
            "pagespeed RewriteLevel CoreFilters",
            "pagespeed EnableFilters convert_to_webp_lossless,convert_jpeg_to_webp",
            "pagespeed InPlaceResourceOptimization on"
        ) -join "`r`n") + "`r`n")
        New-WebAppPool -Name $SiteName | Out-Null
        New-Website -Name $SiteName -Port $Port -PhysicalPath $root -ApplicationPool $SiteName | Out-Null
        Add-WebConfigurationProperty -PSPath $apphost -Location "$SiteName/assets" -Filter "system.webServer/staticContent" -Name "." -Value @{ fileExtension = "."; mimeType = "text/css" }
        $ipOk = [bool](Get-WindowsFeature Web-IP-Security).Installed
        if ($ipOk) {
            $ipf = "system.webServer/security/ipSecurity"
            foreach ($f in "ip-a.css", "ip-b.css") {
                Set-WebConfigurationProperty -PSPath $apphost -Location "$SiteName/secure/$f" -Filter $ipf -Name "allowUnlisted" -Value $false
                foreach ($a in "127.0.0.1", "::1") {
                    Add-WebConfigurationProperty -PSPath $apphost -Location "$SiteName/secure/$f" -Filter $ipf -Name "." -Value @{ ipAddress = $a; allowed = "true" }
                }
            }
        }
        $auth = [bool]((Get-WindowsFeature Web-Url-Auth).Installed -and (Get-WindowsFeature Web-Basic-Auth).Installed)
        $basic = ""
        if ($auth) {
            $chars = [char[]]'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789'
            $rng = New-Object Random
            $pw = 'Cpt-' + (-join (1..20 | ForEach-Object { $chars[$rng.Next($chars.Length)] })) + '9!'
            if (-not (Get-LocalUser -Name cptuser -ErrorAction SilentlyContinue)) {
                New-LocalUser -Name cptuser -Password (ConvertTo-SecureString $pw -AsPlainText -Force) -PasswordNeverExpires | Out-Null
            } else {
                Set-LocalUser -Name cptuser -Password (ConvertTo-SecureString $pw -AsPlainText -Force)
            }
            foreach ($f in "auth-a.css", "auth-b.css") {
                $loc = "$SiteName/secure/$f"
                Set-WebConfigurationProperty -PSPath $apphost -Location $loc -Filter "system.webServer/security/authentication/basicAuthentication" -Name "enabled" -Value $true
                Add-WebConfigurationProperty -PSPath $apphost -Location $loc -Filter "system.webServer/security/authorization" -Name "." -Value @{ accessType = "Deny"; users = "?" }
            }
            # Kept in the guest session only, for the admitted client there.
            $global:CptBasic = "Basic " + [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("cptuser:$pw"))
        }
        $cache = "C:\ProgramData\We-Amp\PageSpeed\cache\{0}.ROOT" -f (Get-Website -Name $SiteName).id
        Remove-Item -Recurse -Force $cache -ErrorAction SilentlyContinue
        New-NetFirewallRule -DisplayName "$SiteName test" -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow | Out-Null
        $ip = (Get-NetIPAddress -AddressFamily IPv4 |
            Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
            Select-Object -First 1).IPAddress
        if (-not $ip) { return "the guest has no non-loopback IPv4 address" }
        "ok $ip $($ca.enabled) $($ca.enableKernelCache) $auth $ipOk"
    }
    if (-not ("$setup" -like "ok *")) { throw "PRECONDITION: $setup" }
    $parts = "$setup" -split " "
    $GuestIp = $parts[1]
    $AuthOk = ($parts[4] -eq "True")
    $IpOk = ($parts[5] -eq "True")
    Invoke-Command -Session $session -ScriptBlock { param($t) Set-Item function:global:Invoke-Raw ([scriptblock]::Create($t + "`nInvoke-Raw @args")) } -ArgumentList $RawText
    Check "defaults" ($parts[2] -eq "True" -and $parts[3] -eq "True") ("server caching enabled {0}, kernel cache {1}" -f $parts[2], $parts[3])
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Invoke-Raw $GuestIp $Port "/open-ready.txt").Status -ne 200) {
        if ((Get-Date) -gt $deadline) { throw "the site does not answer the host" }
        Start-Sleep -Milliseconds 500
    }
    $Browser = @{ "User-Agent" = $BrowserUa; "Accept" = "text/css,*/*;q=0.1" }

    # --- plain: a stylesheet without a query, polled --------------------------
    $first = 0; $n = 0
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline) {
        $n++
        if (CssOptimized (Invoke-Raw $GuestIp $Port "/assets/site.css")) { $first = $n; break }
        Start-Sleep -Milliseconds 500
    }
    Check "plain" ($first -gt 0) ("/assets/site.css optimized in place at request {0} of {1}" -f $first, $n)
    $plainAfter = 0
    for ($i = 0; $i -lt 20; $i++) {
        if (-not (CssOptimized (Invoke-Raw $GuestIp $Port "/assets/site.css" "GET" $Browser))) { $plainAfter++ }
        Start-Sleep -Milliseconds 300
    }
    Check "plain" ($first -gt 0 -and $plainAfter -eq 0) ("/assets/site.css, 20 more requests, plain answers {0}" -f $plainAfter)
    $hd = Invoke-Raw $GuestIp $Port "/assets/site.css" "GET" $Browser
    $cc = ([regex]::Match($hd.Head, '(?im)^Cache-Control:\s*(.+)$')).Groups[1].Value.Trim()
    $ex = ([regex]::Match($hd.Head, '(?im)^Expires:\s*(.+)$')).Groups[1].Value.Trim()
    $vy = ([regex]::Match($hd.Head, '(?im)^Vary:\s*(.+)$')).Groups[1].Value.Trim()
    Say ("INFO headers | /assets/site.css optimized {0}: Cache-Control [{1}] Expires [{2}] Vary [{3}]" -f (CssOptimized $hd), ($cc -replace 'max-age=\d+', 'max-age=N'), $(if ($ex) { "present" } else { "absent" }), $vy)
    # The same stylesheet under a query: optimized in place on any build, so
    # its caching headers compare across builds.
    $q = $null
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline) {
        $x = Invoke-Raw $GuestIp $Port "/assets/site.css?h=1" "GET" $Browser
        if (CssOptimized $x) { $q = $x; break }
        Start-Sleep -Milliseconds 500
    }
    if ($q) {
        $cc = ([regex]::Match($q.Head, '(?im)^Cache-Control:\s*(.+)$')).Groups[1].Value.Trim()
        $ex = ([regex]::Match($q.Head, '(?im)^Expires:\s*(.+)$')).Groups[1].Value.Trim()
        $vy = ([regex]::Match($q.Head, '(?im)^Vary:\s*(.+)$')).Groups[1].Value.Trim()
        Say ("INFO headers | /assets/site.css?h=1 optimized: Cache-Control [{0}] Expires [{1}] Vary [{2}]" -f ($cc -replace 'max-age=\d+', 'max-age=N'), $(if ($ex) { "present" } else { "absent" }), $vy)
    } else { Say "INFO headers | /assets/site.css?h=1 never optimized" }

    # --- head: HEAD requests first ---------------------------------------------
    for ($i = 0; $i -lt 4; $i++) { $null = Invoke-Raw $GuestIp $Port "/assets/head.css" "HEAD" $Browser; Start-Sleep -Milliseconds 300 }
    $h = $null
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline) {
        $x = Invoke-Raw $GuestIp $Port "/assets/head.css" "GET" $Browser
        if (CssOptimized $x) { $h = $x; break }
        Start-Sleep -Milliseconds 500
    }
    Check "head" ($h -ne $null) ("/assets/head.css after HEAD requests, optimized in place: {0}" -f ($h -ne $null))

    # --- script: a script without a query, polled ----------------------------
    $js = $null
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline) {
        $x = Invoke-Raw $GuestIp $Port "/assets/app.js" "GET" $Browser
        if ($x.Status -eq 200 -and $x.Body.Contains("cptHello") -and -not $x.Body.Contains("/*") -and -not $x.Body.Contains("  ")) { $js = $x; break }
        Start-Sleep -Milliseconds 500
    }
    Check "script" ($js -ne $null) ("/assets/app.js optimized in place: {0}" -f ($js -ne $null))

    # --- ajax: a page the module leaves plain for one request -----------------
    # The module answers an HTML page with X-Page-Speed; a copy IIS kept from
    # the plain AJAX answer has none.
    $Ajax = @{ "User-Agent" = $BrowserUa; "X-Requested-With" = "XMLHttpRequest" }
    $Page = @{ "User-Agent" = $BrowserUa; "Accept" = "text/html" }
    for ($i = 0; $i -lt 4; $i++) { $null = Invoke-Raw $GuestIp $Port "/pages/ajax.html" "GET" $Ajax; Start-Sleep -Milliseconds 300 }
    $plainPages = 0
    for ($i = 0; $i -lt 6; $i++) {
        $x = Invoke-Raw $GuestIp $Port "/pages/ajax.html" "GET" $Page
        if ($x.Status -ne 200 -or $x.Head -notmatch '(?im)^X-Page-Speed:') { $plainPages++ }
        Start-Sleep -Milliseconds 300
    }
    Check "ajax" ($plainPages -eq 0) ("/pages/ajax.html after 4 AJAX requests, 6 browser requests, answers without the module: {0}" -f $plainPages)
    $pagesHeld = @(Invoke-Command -Session $session -ScriptBlock {
        (& netsh http show cachestate) | Where-Object { $_ -match "URL:" } | ForEach-Object { ($_ -replace '^\s*URL:\s*', '').Trim() }
    }) | Where-Object { $_ -like ("*:{0}/pages/ajax.html" -f $Port) }
    Check "ajax" (@($pagesHeld).Count -eq 0) ("/pages/ajax.html held by HTTP.sys: {0}" -f (@($pagesHeld).Count -gt 0))

    # --- ipv6: a request over IPv6 with a bracketed IPv6 host -------------------
    $v6 = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock {
        param($Port)
        $hdr = @{ "User-Agent" = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120.0"; "X-Forwarded-Proto" = "http" }
        $r = Invoke-Raw "::1" $Port "/pages/v6.html" "GET" $hdr ("[::1]:{0}" -f $Port)
        "{0} {1}" -f $r.Status, ($r.Head -match '(?im)^X-Page-Speed:')
    }
    Check "ipv6" ("$v6" -eq "200 True") ("/pages/v6.html over [::1]: status and X-Page-Speed [{0}]" -f $v6)

    # --- keep: types the module does not optimize keep IIS's caching ------------
    foreach ($t in "/assets/doc.pdf", "/assets/font.woff") {
        for ($i = 0; $i -lt 5; $i++) { $null = Invoke-Raw $GuestIp $Port $t "GET" $Browser; Start-Sleep -Milliseconds 300 }
    }
    $kept = @(Invoke-Command -Session $session -ScriptBlock {
        (& netsh http show cachestate) | Where-Object { $_ -match "URL:" } | ForEach-Object { ($_ -replace '^\s*URL:\s*', '').Trim() }
    })
    foreach ($t in "/assets/doc.pdf", "/assets/font.woff") {
        $held = @($kept | Where-Object { $_ -like ("*:{0}{1}" -f $Port, $t) }).Count -gt 0
        Check "keep" $held ("{0} requested 5 times, held by HTTP.sys: {1}" -f $t, $held)
    }

    # --- left: a request the module leaves to IIS ------------------------------
    for ($i = 0; $i -lt 5; $i++) {
        $x = Invoke-Raw $GuestIp $Port "/assets/x%23y.css" "GET" $Browser
        Check "left" ($x.Status -eq 200 -and $x.Body.Contains("cpt-other")) ("/assets/x%23y.css -> {0}, IIS's own file {1}" -f $x.Status, $x.Body.Contains("cpt-other"))
        Start-Sleep -Milliseconds 300
    }

    # --- image: two clients ---------------------------------------------------
    $WebpClient = @{ "User-Agent" = $BrowserUa; "Accept" = $WebpAccept }
    $PlainClient = @{ "User-Agent" = $BrowserUa; "Accept" = $PlainAccept }
    $orig = Invoke-Raw $GuestIp $Port "/assets/pic.png" "GET" $PlainClient
    $w = $null
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline) {
        $x = Invoke-Raw $GuestIp $Port "/assets/pic.png" "GET" $WebpClient
        if ($x.Status -eq 200 -and ($x.Type -ne "image/png" -or $x.Length -lt $orig.Length)) { $w = $x; break }
        Start-Sleep -Milliseconds 500
    }
    Say ("INFO image | client accepting WebP: optimized {0}, type {1}, {2} of {3} bytes" -f ($w -ne $null), $(if ($w) { $w.Type } else { "-" }), $(if ($w) { $w.Length } else { "-" }), $orig.Length)
    $bad = @()
    for ($i = 0; $i -lt 10; $i++) {
        $x = Invoke-Raw $GuestIp $Port "/assets/pic.png" "GET" $PlainClient
        if ($x.Status -ne 200 -or $x.Type -match "webp|avif") { $bad += ("{0} {1}" -f $x.Status, $x.Type) }
        Start-Sleep -Milliseconds 300
    }
    Check "image" ($bad.Count -eq 0) ("client without WebP, 10 requests, answers it cannot use: {0} [{1}]" -f $bad.Count, ($bad -join "; "))

    # --- rules: files behind file-level rules ------------------------------------
    function Held([string]$Label) {
        $h = @(Invoke-Command -Session $session -ScriptBlock {
            (& netsh http show cachestate) | Where-Object { $_ -match "URL:" } | ForEach-Object { ($_ -replace '^\s*URL:\s*', '').Trim() }
        })
        Say ("INFO cache {0} | HTTP.sys holds [{1}]" -f $Label, ($h -join " "))
    }
    function Admitted([string]$Path, [bool]$Basic) {
        Invoke-Command -Session $session -ArgumentList $Port, $Path, $Basic -ScriptBlock {
            param($Port, $Path, $Basic)
            $hdr = @{ "User-Agent" = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120.0" }
            if ($Basic) { $hdr["Authorization"] = $global:CptBasic }
            $r = Invoke-Raw "127.0.0.1" $Port $Path "GET" $hdr
            "{0} {1}" -f $r.Status, $r.Body.Contains("cpt-held")
        }
    }
    $families = @()
    if ($IpOk) { $families += ,@("ip", $false) } else { Say "rules ip | Web-IP-Security (IP and Domain Restrictions) not installed | SKIP" }
    if ($AuthOk) { $families += ,@("auth", $true) } else { Say "rules auth | Web-Url-Auth or Web-Basic-Auth not installed | SKIP" }
    foreach ($fam in $families) {
        $kind = $fam[0]; $basic = $fam[1]
        # (a) the admitted client reads the plain URL four times within a few seconds.
        $pa = "/secure/$kind-a.css"
        $reads = @(); for ($i = 0; $i -lt 4; $i++) { $reads += (Admitted $pa $basic); Start-Sleep -Milliseconds 400 }
        Say ("INFO rules {0} (a) | admitted reads of {1}: {2}" -f $kind, $pa, ($reads -join ", "))
        Held "rules $kind (a)"
        for ($i = 0; $i -lt 3; $i++) {
            $r = Invoke-Raw $GuestIp $Port $pa "GET" $Browser
            Check "rules $kind (a)" ($r.Status -ge 400 -and -not $r.Body.Contains("cpt-held")) ("host, {0} -> {1}, held content {2}" -f $pa, $r.Status, $r.Body.Contains("cpt-held"))
        }
        # (b) the admitted client reads until the module has served an in-place answer.
        $pb = "/secure/$kind-b.css"
        $reads = @(); for ($i = 0; $i -lt 8; $i++) { $reads += (Admitted $pb $basic); Start-Sleep -Milliseconds 400 }
        $st = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock { param($Port) $s = Invoke-Raw "127.0.0.1" $Port "/pagespeed_admin/statistics"; ([regex]::Match($s.Body, '"ipro_served":\s*(\d+)')).Groups[1].Value }
        Say ("INFO rules {0} (b) | admitted reads of {1}: {2}; ipro_served now {3}" -f $kind, $pb, ($reads -join ", "), $st)
        Held "rules $kind (b)"
        for ($i = 0; $i -lt 3; $i++) {
            $r = Invoke-Raw $GuestIp $Port $pb "GET" $Browser
            Check "rules $kind (b)" ($r.Status -ge 400 -and -not $r.Body.Contains("cpt-held")) ("host, {0} -> {1}, held content {2}" -f $pb, $r.Status, $r.Body.Contains("cpt-held"))
        }
    }

    # --- cache: what HTTP.sys holds ---------------------------------------------
    $held = @(Invoke-Command -Session $session -ScriptBlock {
        (& netsh http show cachestate) | Where-Object { $_ -match "URL:" } | ForEach-Object { ($_ -replace '^\s*URL:\s*', '').Trim() }
    })
    Say ("INFO cache | HTTP.sys holds [{0}]" -f ($held -join " "))
    $ours = @($held | Where-Object { $_ -match (":{0}/(assets|secure)/" -f $Port) -and $_ -notmatch "\.(pdf|woff)$" })
    Check "cache" ($ours.Count -eq 0) ("in-place URLs of the site held by HTTP.sys: {0} [{1}]" -f $ours.Count, ($ours -join " "))

    if ($script:Failures -gt 0) { $exitCode = 1 }
} catch {
    Say ("ERROR | {0}" -f $_.Exception.Message)
    if ($exitCode -eq 0) { $exitCode = 2 }
} finally {
    try {
        Invoke-Command -Session $session -ArgumentList $SiteName -ScriptBlock {
            param($SiteName)
            Import-Module WebAdministration
            foreach ($loc in "$SiteName/assets", "$SiteName/secure/ip-a.css", "$SiteName/secure/ip-b.css", "$SiteName/secure/auth-a.css", "$SiteName/secure/auth-b.css") {
                try { Remove-WebConfigurationLocation -PSPath "MACHINE/WEBROOT/APPHOST" -Name $loc -ErrorAction SilentlyContinue } catch { }
            }
            Remove-LocalUser -Name cptuser -ErrorAction SilentlyContinue
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
