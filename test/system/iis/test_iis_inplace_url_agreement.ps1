# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# test_iis_inplace_url_agreement.ps1 - Pins that in-place optimization on IIS
# serves and records a response only under the path IIS itself routes the
# request on.
#
# IIS routes a request, matches its <location> sections and applies request
# filtering on the decoded path HTTP.sys hands it; the module keys the
# in-place cache on its own parse of the request URL.  Spellings both read as
# the same path are optimized in place; any other spelling gets IIS's own
# answer and never another path's in-place entry.
#
# Runs on the Hyper-V host and drives a guest with the module installed over
# PowerShell Direct; the host is a client the file-level rules refuse, a
# loopback client in the guest one the IP rule admits.
#
# In the guest the test provisions its own site on $Port with
#   open/site.css, open/x (served as text/css) beside open/x#y.css,
#   open/100%.css, and held.css (one marker each) under ip/, auth/, ext/,
#   seq/ and hseg/, each behind a file-level rule:
#     ip/held.css    <location> ipSecurity: unlisted denied, loopback admitted
#     auth/held.css  <location> authorization: anonymous users denied
#                    (needs Web-Url-Auth; skipped without it)
#     ext/held.cxx   request filtering: file extension .cxx denied (added
#                    after a loopback client read ext/held.cxx?x)
#     seq/held.css   request filtering: URL sequence "heldseq" denied
#                    (the file is seq/heldseq.css)
#     hseg/held.css  request filtering: hidden segment "hseg"
# Checks (PASS/FAIL; INFO rows report without judging):
#   control      plain URLs: optimized in place; each rule refuses the host
#   query        site.css%3Fv=1 after site.css?v=1 was optimized: not that entry
#   rules        after a loopback client read ip/held.css?x (recorded, not
#                optimized: the module's own fetch is refused by the rule), the
#                host's held.css%3Fx, held.css%3F and the absolute-URL
#                form never carry the held marker; the same for every rule
#   percent      %253F is refused by IIS and never served an entry; %25 in a
#                file name names that file
#   windows      trailing dot, trailing space, case, 8.3 name (of
#                open/stylesheet-long.css): the first answer to each is never
#                an optimized entry; %5C and %2F reach the path IIS routes on
#                and are reported as INFO
#   hash         x%23y.css gets x#y.css, never x's optimized entry
#   pagespeed    INFO: .pagespeed. names with %3F, %23, %25 against the plain
#                name
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
    [string]$SiteName = "ps-url-agree",
    [int]$Port = 8094,
    [int]$PollSeconds = 60,
    [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"
$script:Failures = 0
$script:Ran = 0
$Markers = @{
    open = "iua-open-marker"; x = "iua-x-marker"; hash = "iua-hash-marker"
    pct = "iua-pct-marker"; ip = "iua-ip-marker"; auth = "iua-auth-marker"
    ext = "iua-ext-marker"; seq = "iua-seq-marker"; hseg = "iua-hseg-marker"
    long = "iua-long-marker"
}
$Held = [ordered]@{
    ip = "/ip/held.css"; auth = "/auth/held.css"; ext = "/ext/held.cxx"
    seq = "/seq/heldseq.css"; hseg = "/hseg/held.css"
}

function Say([string]$Line) {
    Write-Host $Line
    if ($OutFile) { Add-Content -LiteralPath $OutFile -Value $Line -Encoding ASCII }
}

function Check([string]$Case, [bool]$Ok, [string]$Detail) {
    $script:Ran++
    if (-not $Ok) { $script:Failures++ }
    Say ("{0} | {1} | {2}" -f $Case, $Detail, $(if ($Ok) { "PASS" } else { "FAIL" }))
}

# One HTTP/1.1 GET over a fresh connection; $Target is sent as is on the
# request line. Defined as text so the same function runs on the host and in
# the guest.
$RawText = @'
function Invoke-Raw([string]$Ip, [int]$Port, [string]$Target, [hashtable]$Headers = @{}) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = 20000
    $client.SendTimeout = 20000
    $client.Connect($Ip, $Port)
    try {
        $stream = $client.GetStream()
        $req = "GET $Target HTTP/1.1`r`nHost: localhost:$Port`r`nConnection: close`r`n"
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

# The markers a response carries, as a sorted, comma-separated list.
function MarkersIn([string]$Body) {
    $found = @()
    foreach ($k in $Markers.Keys) { if ($Body.Contains($Markers[$k])) { $found += $k } }
    ($found | Sort-Object) -join ","
}

# An optimized copy has no comments and no runs of spaces.
function Optimized($r, [string]$Marker) {
    $r.Status -eq 200 -and $r.Body.Contains($Marker) -and -not $r.Body.Contains("/*") -and -not $r.Body.Contains("  ")
}
function OptimizedAny($r) {
    foreach ($k in $Markers.Keys) { if (Optimized $r $Markers[$k]) { return $true } }
    $false
}

# What the module says about a URL that was not optimized: the in-place
# counters and the message history lines that name it, read in the guest.
function Show-Diagnostics([string]$Path) {
    $last = Invoke-Raw $GuestIp $Port $Path
    Say ("DIAG | {0} last answer {1} [{2}] len {3}" -f $Path, $last.Status, (MarkersIn $last.Body), $last.Body.Length)
    $d = Invoke-Command -Session $session -ArgumentList $Port, $Path -ScriptBlock {
        param($Port, $Path)
        $st = Invoke-Raw "127.0.0.1" $Port "/pagespeed_admin/statistics"
        $mh = Invoke-Raw "127.0.0.1" $Port "/pagespeed_admin/message_history"
        $leaf = ($Path -split "/")[-1]
        @([regex]::Matches($st.Body, '"(ipro_[a-z_]+|in_place_[a-z_]+|css_filter_[a-z_]+|num_rewrites_[a-z_]+)":\s*(\d+)') | ForEach-Object { "stat " + $_.Groups[1].Value + "=" + $_.Groups[2].Value }) +
        @([regex]::Matches($mh.Body, '"message":"([^"]*)"') | ForEach-Object { $_.Groups[1].Value } | Where-Object { $_ -like "*$leaf*" } | Select-Object -Last 15 | ForEach-Object { "msg " + $_ })
    }
    foreach ($l in $d) { Say ("DIAG | {0}" -f $l) }
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
    Invoke-Command -Session $session -ScriptBlock { param($t) Set-Item function:global:Invoke-Raw ([scriptblock]::Create($t + "`nInvoke-Raw @args")) } -ArgumentList $RawText

    $setup = Invoke-Command -Session $session -ArgumentList $SiteName, $Port, $Markers -ScriptBlock {
        param($SiteName, $Port, $Markers)
        $ErrorActionPreference = "Stop"
        Import-Module WebAdministration
        if (-not (Test-Path "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll")) { return "module not installed" }
        $f = Get-WindowsFeature -Name Web-IP-Security
        if ($f -eq $null -or -not $f.Installed) { return "feature Web-IP-Security not installed" }
        $urlAuth = [bool]((Get-WindowsFeature -Name Web-Url-Auth).Installed)
        $root = "C:\inetpub\$SiteName"
        if (Test-Path "IIS:\Sites\$SiteName") { Remove-Website -Name $SiteName }
        if (Test-Path "IIS:\AppPools\$SiteName") { Remove-WebAppPool -Name $SiteName }
        Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$root\open", "$root\ip", "$root\auth", "$root\ext", "$root\seq", "$root\hseg" | Out-Null
        function Css([string]$Path, [string]$Marker) {
            [IO.File]::WriteAllText($Path, "/* stylesheet */`r`n.$Marker   {   margin :  0 ;   }`r`n")
        }
        Css "$root\open\site.css" $Markers.open
        Css "$root\open\x" $Markers.x
        Css "$root\open\x#y.css" $Markers.hash
        Css "$root\open\100%.css" $Markers.pct
        Css "$root\open\stylesheet-long.css" $Markers.long
        Css "$root\ip\held.css" $Markers.ip
        Css "$root\auth\held.css" $Markers.auth
        Css "$root\ext\held.cxx" $Markers.ext
        Css "$root\seq\heldseq.css" $Markers.seq
        Css "$root\hseg\held.css" $Markers.hseg
        [IO.File]::WriteAllText("$root\open\ready.txt", "ready`r`n")
        [IO.File]::WriteAllText("$root\pagespeed.config", (@(
            "pagespeed on",
            "pagespeed RewriteLevel PassThrough",
            "pagespeed EnableFilters rewrite_css",
            "pagespeed InPlaceResourceOptimization on"
        ) -join "`r`n") + "`r`n")
        New-WebAppPool -Name $SiteName | Out-Null
        New-Website -Name $SiteName -Port $Port -PhysicalPath $root -ApplicationPool $SiteName | Out-Null
        $apphost = "MACHINE/WEBROOT/APPHOST"
        # Every request of the test reaches the module: no IIS output or
        # kernel cache answers a repeated request on its own.
        Set-WebConfigurationProperty -PSPath $apphost -Location $SiteName -Filter "system.webServer/caching" -Name "enabled" -Value $false
        Set-WebConfigurationProperty -PSPath $apphost -Location $SiteName -Filter "system.webServer/caching" -Name "enableKernelCache" -Value $false
        # Extensionless open/x and the .cxx file are stylesheets.
        foreach ($pair in @(@("$SiteName/open", "."), @("$SiteName/ext", ".cxx"))) {
            Add-WebConfigurationProperty -PSPath $apphost -Location $pair[0] -Filter "system.webServer/staticContent" -Name "." -Value @{ fileExtension = $pair[1]; mimeType = "text/css" }
        }
        # File-level rules.
        $ipf = "system.webServer/security/ipSecurity"
        Set-WebConfigurationProperty -PSPath $apphost -Location "$SiteName/ip/held.css" -Filter $ipf -Name "allowUnlisted" -Value $false
        foreach ($a in "127.0.0.1", "::1") {
            Add-WebConfigurationProperty -PSPath $apphost -Location "$SiteName/ip/held.css" -Filter $ipf -Name "." -Value @{ ipAddress = $a; allowed = "true" }
        }
        if ($urlAuth) {
            Add-WebConfigurationProperty -PSPath $apphost -Location "$SiteName/auth/held.css" -Filter "system.webServer/security/authorization" -Name "." -Value @{ accessType = "Deny"; users = "?" }
        }
        $rf = "system.webServer/security/requestFiltering"
        Add-WebConfigurationProperty -PSPath $apphost -Location "$SiteName/seq" -Filter "$rf/denyUrlSequences" -Name "." -Value @{ sequence = "heldseq" }
        Add-WebConfigurationProperty -PSPath $apphost -Location $SiteName -Filter "$rf/hiddenSegments" -Name "." -Value @{ segment = "hseg" }
        $cache = "C:\ProgramData\We-Amp\PageSpeed\cache\{0}.ROOT" -f (Get-Website -Name $SiteName).id
        Remove-Item -Recurse -Force $cache -ErrorAction SilentlyContinue
        New-NetFirewallRule -DisplayName "$SiteName test" -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow | Out-Null
        $ip = (Get-NetIPAddress -AddressFamily IPv4 |
            Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
            Select-Object -First 1).IPAddress
        if (-not $ip) { return "the guest has no non-loopback IPv4 address" }
        $short = ""
        $line = (& cmd /c dir /x "$root\open\stylesheet-long.css") | Where-Object { $_ -match 'stylesheet-long\.css' } | Select-Object -First 1
        if ($line -match '\s([A-Z0-9~_]{1,8}\.[A-Z0-9]{1,3})\s+stylesheet-long\.css') { $short = $Matches[1] }
        "ok $ip $urlAuth $short"
    }
    if (-not ("$setup" -like "ok *")) { throw "PRECONDITION: $setup" }
    $parts = "$setup" -split " "
    $GuestIp = $parts[1]; $UrlAuth = ($parts[2] -eq "True"); $Short = $(if ($parts.Count -gt 3) { $parts[3] } else { "" })
    Say ("SETUP | guest {0}, site {1} on {2}, url authorization {3}, short name [{4}]" -f $GuestIp, $SiteName, $Port, $UrlAuth, $Short)
    $null = Wait-For { (Invoke-Raw $GuestIp $Port "/open/ready.txt").Status -eq 200 } "the site to answer the host"
    $Abs = "http://localhost:$Port"

    # --- control ---------------------------------------------------------
    $o = $null
    try { $o = Wait-For { $x = Invoke-Raw $GuestIp $Port "/open/site.css?v=1"; if (Optimized $x $Markers.open) { $x } } "site.css?v=1 optimized" } catch { }
    Check "control open" ($o -ne $null) ("host, /open/site.css?v=1 optimized in place: {0}" -f ($o -ne $null))
    foreach ($pair in @(@("/open/site.css", $Markers.open), @("/open/stylesheet-long.css", $Markers.long))) {
        $o = $null
        try { $o = Wait-For { $x = Invoke-Raw $GuestIp $Port $pair[0]; if (Optimized $x $pair[1]) { $x } } "$($pair[0]) optimized" } catch { }
        Check "control open" ($o -ne $null) ("host, {0} optimized in place: {1}" -f $pair[0], ($o -ne $null))
        if ($o -eq $null) { Show-Diagnostics $pair[0] }
    }
    foreach ($k in $Held.Keys) {
        if ($k -eq "auth" -and -not $UrlAuth) { Say "control auth | Web-Url-Auth not installed | SKIP"; continue }
        if ($k -eq "ext") { continue }  # checked once its rule is added, below
        $r = Invoke-Raw $GuestIp $Port $Held[$k]
        Check "control $k" ($r.Status -ge 400 -and -not $r.Body.Contains($Markers[$k])) ("host, {0} -> {1}" -f $Held[$k], $r.Status)
    }

    # --- query: an encoded ? names a file, not the query --------------------
    for ($i = 0; $i -lt 3; $i++) {
        $r = Invoke-Raw $GuestIp $Port "/open/site.css%3Fv=1"
        Check "query" (-not (Optimized $r $Markers.open)) ("host, /open/site.css%3Fv=1 -> {0} [{1}] optimized {2}" -f $r.Status, (MarkersIn $r.Body), (Optimized $r $Markers.open))
    }

    # --- rules -----------------------------------------------------------
    # The IP rule admits loopback: a client in the guest reads the file with a
    # query, so the module records an in-place entry for that URL.  The entry
    # stays unoptimized: the module's own fetch of a file behind the rule is
    # refused like any other client the rule does not list.
    $admOk = 0
    for ($i = 0; $i -lt 3; $i++) {
        $x = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock { param($Port) Invoke-Raw "127.0.0.1" $Port "/ip/held.css?x" }
        if ($x.Status -eq 200 -and $x.Body.Contains("iua-ip-marker")) { $admOk++ }
        Start-Sleep -Milliseconds 750
    }
    Check "rules entry" ($admOk -eq 3) ("loopback client in the guest, /ip/held.css?x read {0} of 3 times" -f $admOk)
    # An entry recorded before the file-extension rule existed.
    $extOk = 0
    for ($i = 0; $i -lt 3; $i++) {
        $x = Invoke-Command -Session $session -ArgumentList $Port -ScriptBlock { param($Port) Invoke-Raw "127.0.0.1" $Port "/ext/held.cxx?x" }
        if ($x.Status -eq 200 -and $x.Body.Contains("iua-ext-marker")) { $extOk++ }
        Start-Sleep -Milliseconds 750
    }
    Check "rules entry" ($extOk -eq 3) ("loopback client in the guest, /ext/held.cxx?x before its rule, read {0} of 3 times" -f $extOk)
    Invoke-Command -Session $session -ArgumentList $SiteName -ScriptBlock {
        param($SiteName)
        Import-Module WebAdministration
        Add-WebConfigurationProperty -PSPath "MACHINE/WEBROOT/APPHOST" -Location "$SiteName/ext" -Filter "system.webServer/security/requestFiltering/fileExtensions" -Name "." -Value @{ fileExtension = ".cxx"; allowed = "false" }
    }
    $null = Wait-For { (Invoke-Raw $GuestIp $Port "/ext/held.cxx").Status -eq 404 } "the file-extension rule to refuse the host"
    $r = Invoke-Raw $GuestIp $Port "/ext/held.cxx"
    Check "control ext" ($r.Status -ge 400 -and -not $r.Body.Contains($Markers.ext)) ("host, /ext/held.cxx -> {0}" -f $r.Status)
    # Client-sent headers named like the module's own server variables.
    $hdr = @{ "X-PRISTINE-PATH" = "/ip/held.css"; "X-PRISTINE-URL" = "$Abs/ip/held.css?x" }
    for ($i = 0; $i -lt 2; $i++) {
        $r = Invoke-Raw $GuestIp $Port "/ip/held.css%3Fx" $hdr
        Check "rules headers" (-not $r.Body.Contains($Markers.ip)) ("host with X-PRISTINE-* headers, /ip/held.css%3Fx -> {0} [{1}]" -f $r.Status, (MarkersIn $r.Body))
    }
    foreach ($k in $Held.Keys) {
        if ($k -eq "auth" -and -not $UrlAuth) { Say "rules auth | Web-Url-Auth not installed | SKIP"; continue }
        $p = $Held[$k]
        foreach ($t in @("$p%3Fx", "$p%3F", "$Abs$p%3Fx", "$p%3F%3Fx")) {
            for ($i = 0; $i -lt 2; $i++) {
                $r = Invoke-Raw $GuestIp $Port $t
                Check "rules $k" (-not $r.Body.Contains($Markers[$k])) ("host, {0} -> {1} [{2}]" -f $t, $r.Status, (MarkersIn $r.Body))
            }
        }
    }

    # --- percent ---------------------------------------------------------
    $r = Invoke-Raw $GuestIp $Port "/open/site.css%253Fv=1"
    Check "percent double" ($r.Status -ge 400 -and -not $r.Body.Contains($Markers.open)) ("host, /open/site.css%253Fv=1 -> {0} [{1}]" -f $r.Status, (MarkersIn $r.Body))
    $pc = $null
    try { $pc = Wait-For { $x = Invoke-Raw $GuestIp $Port "/open/100%25.css"; if ($x.Status -eq 200 -and (MarkersIn $x.Body) -eq "pct") { $x } } "100%25.css" } catch { }
    Check "percent name" ($pc -ne $null) ("host, /open/100%25.css -> its own file: {0}, optimized {1}" -f ($pc -ne $null), $(if ($pc) { Optimized $pc $Markers.pct } else { $false }))

    # --- windows spellings -------------------------------------------------
    $spellings = @("/open/site.css.", "/open/site.css%20", "/open/SITE.CSS", "/OPEN/Site.Css?v=1",
                   "/ip/held.css.", "/ip/held.css%20", "/ip/HELD.CSS?x")
    if ($Short) { $spellings += "/open/$Short"; $spellings += "/open/$($Short)?v=1" } else { Say "windows 8.3 | the volume has no short names | SKIP" }
    # Each spelling is sent once, before anything could have recorded under
    # its own key, so an optimized answer could only be another key's entry.
    foreach ($t in $spellings) {
        $r = Invoke-Raw $GuestIp $Port $t
        $m = MarkersIn $r.Body
        $opt = OptimizedAny $r
        Check "windows" (-not $opt) ("host, {0} -> {1} [{2}] optimized {3}" -f $t, $r.Status, $m, $opt)
        if ($t -like "/ip*" -and $m -eq "ip") { Say ("INFO windows | IIS itself answered {0} with the held file" -f $t) }
    }

    # HTTP.sys decodes an escaped slash or backslash to the path IIS routes
    # on, so the module reads the same path and the same rules applied.
    foreach ($t in @("/open%5Csite.css", "/open%2Fsite.css", "/ip%5Cheld.css?x", "/ip%2Fheld.css?x")) {
        $r = Invoke-Raw $GuestIp $Port $t
        Say ("INFO separators | host, {0} -> {1} [{2}] optimized {3}" -f $t, $r.Status, (MarkersIn $r.Body), (OptimizedAny $r))
    }

    # --- hash ------------------------------------------------------------
    $xo = $null
    try { $xo = Wait-For { $x = Invoke-Raw $GuestIp $Port "/open/x"; if (Optimized $x $Markers.x) { $x } } "/open/x optimized" } catch { }
    Check "hash entry" ($xo -ne $null) ("host, /open/x optimized in place: {0}" -f ($xo -ne $null))
    for ($i = 0; $i -lt 3; $i++) {
        $r = Invoke-Raw $GuestIp $Port "/open/x%23y.css"
        Check "hash" ($r.Status -eq 200 -and (MarkersIn $r.Body) -eq "hash") ("host, /open/x%23y.css -> {0} [{1}]" -f $r.Status, (MarkersIn $r.Body))
    }

    # --- pagespeed names (report only) -------------------------------------
    foreach ($t in @("/open/A.site.css.pagespeed.cf.0.css", "/open/A.site.css%3Fx.pagespeed.cf.0.css",
                     "/open/A.site.css%23x.pagespeed.cf.0.css", "/open/A.site%25.css.pagespeed.cf.0.css",
                     "/open/A.site%2573.css.pagespeed.cf.0.css", "/open/A.%73ite.css.pagespeed.cf.0.css")) {
        $r = Invoke-Raw $GuestIp $Port $t
        Say ("INFO pagespeed | host, {0} -> {1} [{2}] optimized {3}" -f $t, $r.Status, (MarkersIn $r.Body), (Optimized $r $Markers.open))
    }

    if ($script:Failures -gt 0) { $exitCode = 1 }
} catch {
    Say ("ERROR | {0}" -f $_.Exception.Message)
    if ($exitCode -eq 0) { $exitCode = 2 }
} finally {
    try {
        Invoke-Command -Session $session -ArgumentList $SiteName -ScriptBlock {
            param($SiteName)
            Import-Module WebAdministration
            $apphost = "MACHINE/WEBROOT/APPHOST"
            foreach ($loc in "$SiteName/ip/held.css", "$SiteName/auth/held.css", "$SiteName/ext", "$SiteName/seq", "$SiteName/open", $SiteName) {
                try { Remove-WebConfigurationLocation -PSPath $apphost -Name $loc -ErrorAction SilentlyContinue } catch { }
            }
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
