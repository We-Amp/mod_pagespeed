# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# test_iis_access_rules.ps1 - Pins that IIS access rules apply to everything
# the PageSpeed module serves, including responses it serves from its cache.
#
# For each rule below, one anonymous client is admitted and one is refused,
# and both ask for the same URL after the admitted client has warmed the
# module's cache for it:
#   - an optimized resource URL (.pagespeed.), and
#   - a stylesheet the module optimizes in place.
# The admitted client must keep getting the optimized response; the refused
# client must get the refusal IIS gives for that rule, never the optimized
# bytes.
#
# Rules (each set on this test's own site only, and only when the IIS
# feature that provides it is installed; a missing feature skips its case):
#   ip     IP and Domain Restrictions: the guest's own non-loopback address
#          is denied (403); 127.0.0.1 is admitted.
#   verb   URL Authorization: HEAD is denied to all users (401); GET is
#          admitted.
#   header Request Filtering: a request header longer than one byte is
#          denied (404); a request without it is admitted.
# Before each case the rule is shown to be in force on a plain text file the
# module does not handle, so a refusal seen on a module URL is the rule's and
# a missing refusal is not a rule that never took effect.
#
# The test provisions its own site, application pool and directory, and
# removes them (and its rules) in finally, even on failure. The admitted
# responses' headers are written to the output so runs can be compared.
#
# Exit codes: 0 all run cases passed; 1 a case failed; 2 precondition not
# met (module missing, site never engaged, or no case could run).
#
# Run as Administrator inside a Windows Server VM with the module installed.
# Windows PowerShell 5.1 compatible; ASCII only.

[CmdletBinding()]
param(
    [string]$SiteName = "ps-access-rules",
    [string]$SiteRoot = "C:\inetpub\ps-access-rules",
    [int]$Port = 8091,
    [int]$PollSeconds = 60,
    [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"
Import-Module WebAdministration
$AppHost = "MACHINE/WEBROOT/APPHOST"
$script:Failures = 0
$script:Ran = 0

function Say([string]$Line) {
    Write-Host $Line
    if ($OutFile) { Add-Content -LiteralPath $OutFile -Value $Line -Encoding ASCII }
}

# One HTTP/1.1 request over a fresh connection; returns status, head, body.
function Invoke-Raw([string]$Ip, [string]$Path, [string]$Method = "GET",
                    [hashtable]$Headers = @{}) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = 20000
    $client.SendTimeout = 20000
    $client.Connect($Ip, $Port)
    try {
        $stream = $client.GetStream()
        $req = "$Method $Path HTTP/1.1`r`nHost: localhost:$Port`r`nConnection: close`r`n"
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
    $head = $parts[0]
    $body = if ($parts.Count -gt 1) { $parts[1] } else { "" }
    $status = 0
    $first = ($head -split "`r`n")[0]
    if ($first -match '^HTTP/\d\.\d (\d{3})') { $status = [int]$Matches[1] }
    [pscustomobject]@{ Status = $status; Head = $head; Body = $body }
}

# The stylesheet the module optimizes, and the form it optimizes it to.
$Css = "/* stylesheet for the access rule test */`r`n.probe   {   margin :  0 ;   }`r`n"
$Optimized = ".probe{margin:0}"
function Is-Optimized($r) {
    $r.Status -eq 200 -and $r.Body.Contains($Optimized) -and -not $r.Body.Contains("/*")
}

function Wait-For([scriptblock]$Probe, [string]$What) {
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline) {
        $value = & $Probe
        if ($value) { return $value }
        Start-Sleep -Milliseconds 500
    }
    throw "timed out waiting for $What"
}

function Clear-Rules {
    foreach ($section in "system.webServer/security/ipSecurity",
                         "system.webServer/security/authorization",
                         "system.webServer/security/requestFiltering") {
        Clear-WebConfiguration -PSPath $AppHost -Location $SiteName -Filter $section -ErrorAction SilentlyContinue
    }
}

function Feature-Installed([string]$Name) {
    $f = Get-WindowsFeature -Name $Name -ErrorAction SilentlyContinue
    return ($f -ne $null -and $f.Installed)
}

# One rule: how to set it, which client it admits and which it refuses.
$GuestIp = (Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    Select-Object -First 1).IPAddress
$Rules = @(
    @{ Name = "ip"; Feature = "Web-IP-Security"; Refusal = 403
       Apply = { Add-WebConfigurationProperty -PSPath $AppHost -Location $SiteName `
                   -Filter "system.webServer/security/ipSecurity" -Name "." `
                   -Value @{ ipAddress = $GuestIp; allowed = "false" } }
       Admitted = @{ Ip = "127.0.0.1"; Method = "GET"; Headers = @{} }
       Refused  = @{ Ip = $GuestIp; Method = "GET"; Headers = @{} } },
    @{ Name = "verb"; Feature = "Web-Url-Auth"; Refusal = 401
       Apply = { Add-WebConfigurationProperty -PSPath $AppHost -Location $SiteName `
                   -Filter "system.webServer/security/authorization" -Name "." `
                   -Value @{ accessType = "Deny"; users = "*"; verbs = "HEAD" } }
       Admitted = @{ Ip = "127.0.0.1"; Method = "GET"; Headers = @{} }
       Refused  = @{ Ip = "127.0.0.1"; Method = "HEAD"; Headers = @{} } },
    @{ Name = "header"; Feature = "Web-Filtering"; Refusal = 404
       Apply = { Add-WebConfigurationProperty -PSPath $AppHost -Location $SiteName `
                   -Filter "system.webServer/security/requestFiltering/requestLimits/headerLimits" -Name "." `
                   -Value @{ header = "X-Access-Rule-Test"; sizeLimit = 1 } }
       Admitted = @{ Ip = "127.0.0.1"; Method = "GET"; Headers = @{} }
       Refused  = @{ Ip = "127.0.0.1"; Method = "GET"; Headers = @{ "X-Access-Rule-Test" = "refused" } } }
)

function Ask($Who, [string]$Path) {
    Invoke-Raw $Who.Ip $Path $Who.Method $Who.Headers
}

function Check([string]$Case, [bool]$Ok, [string]$Detail) {
    $script:Ran++
    if (-not $Ok) { $script:Failures++ }
    Say ("{0} | {1} | {2}" -f $Case, $Detail, $(if ($Ok) { "PASS" } else { "FAIL" }))
}

function Header-Summary($r) {
    $keep = @()
    foreach ($line in ($r.Head -split "`r`n")) {
        if ($line -match '^(Content-Type|Content-Length|Cache-Control|ETag|Expires|Last-Modified|Vary|X-Page-Speed|Content-Encoding):') {
            $keep += $line.Trim()
        }
    }
    ($keep | Sort-Object) -join "; "
}

if (-not (Test-Path "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll")) {
    Say "PRECONDITION | module not installed"
    exit 2
}

$exitCode = 0
try {
    # Site setup.
    if (Test-Path "IIS:\Sites\$SiteName") { Remove-Website -Name $SiteName }
    if (Test-Path "IIS:\AppPools\$SiteName") { Remove-WebAppPool -Name $SiteName }
    New-Item -ItemType Directory -Force "$SiteRoot\styles" | Out-Null
    [IO.File]::WriteAllText("$SiteRoot\styles\probe.css", $Css)
    [IO.File]::WriteAllText("$SiteRoot\styles\linked.css", $Css)
    [IO.File]::WriteAllText("$SiteRoot\plain.txt", "plain`r`n")
    [IO.File]::WriteAllText("$SiteRoot\page.html",
        "<html><head><link rel=`"stylesheet`" href=`"styles/linked.css`"></head><body>page</body></html>`r`n")
    [IO.File]::WriteAllText("$SiteRoot\pagespeed.config", (@(
        "pagespeed on",
        "pagespeed RewriteLevel PassThrough",
        "pagespeed EnableFilters rewrite_css",
        "pagespeed InPlaceResourceOptimization on"
    ) -join "`r`n") + "`r`n")
    New-WebAppPool -Name $SiteName | Out-Null
    New-Website -Name $SiteName -Port $Port -PhysicalPath $SiteRoot -ApplicationPool $SiteName | Out-Null
    Clear-Rules

    $admitted = @{ Ip = "127.0.0.1"; Method = "GET"; Headers = @{} }

    # The optimized resource URL, from the module's own rewrite of the page.
    $resource = Wait-For {
        $r = Ask $admitted "/page.html"
        if ($r.Body -match 'href="(styles/[^"]*\.pagespeed\.[^"]*)"') { "/" + $Matches[1] }
    } "the page to reference an optimized stylesheet"
    Say "SETUP | optimized resource url $resource"
    $inplace = "/styles/probe.css"
    $null = Wait-For { Is-Optimized (Ask $admitted $resource) } "the optimized resource"
    $null = Wait-For { Is-Optimized (Ask $admitted $inplace) } "the in-place optimized stylesheet"
    Say "SETUP | both URLs optimized and cached"

    foreach ($rule in $Rules) {
        if (-not (Feature-Installed $rule.Feature)) {
            Say ("{0} | SKIP | feature {1} not installed" -f $rule.Name, $rule.Feature)
            continue
        }
        & $rule.Apply
        try {
            # The rule is in force: the refused client is refused a file the
            # module never handles, and the admitted client is not.
            $null = Wait-For { (Ask $rule.Refused "/plain.txt").Status -eq $rule.Refusal } "rule $($rule.Name) to take effect"
            $control = Ask $rule.Admitted "/plain.txt"
            Check "$($rule.Name) control" ($control.Status -eq 200) ("admitted client plain file -> {0}" -f $control.Status)

            foreach ($kind in @(@{ Name = "resource"; Path = $resource }, @{ Name = "in-place"; Path = $inplace })) {
                # Warm: the admitted client asks several times, past the count
                # after which IIS may keep a response in its kernel cache.
                $warm = $null
                for ($i = 0; $i -lt 4; $i++) { $warm = Ask $rule.Admitted $kind.Path }
                Check "$($rule.Name) $($kind.Name) admitted" (Is-Optimized $warm) ("status {0}, optimized {1}; {2}" -f $warm.Status, (Is-Optimized $warm), (Header-Summary $warm))
                $refused = Ask $rule.Refused $kind.Path
                $leaked = $refused.Body.Contains($Optimized)
                Check "$($rule.Name) $($kind.Name) refused" ($refused.Status -eq $rule.Refusal -and -not $leaked) ("status {0} (expect {1}), optimized body {2}" -f $refused.Status, $rule.Refusal, $leaked)
                $again = Ask $rule.Admitted $kind.Path
                Check "$($rule.Name) $($kind.Name) admitted after" (Is-Optimized $again) ("status {0}" -f $again.Status)
            }
        } finally {
            Clear-Rules
        }
        $null = Wait-For { (Ask $rule.Refused "/plain.txt").Status -eq 200 } "rule $($rule.Name) to be removed"
    }

    if ($script:Ran -eq 0) {
        Say "PRECONDITION | no rule's IIS feature is installed; nothing ran"
        $exitCode = 2
    } elseif ($script:Failures -gt 0) {
        $exitCode = 1
    }
} catch {
    Say ("ERROR | {0}" -f $_.Exception.Message)
    if ($exitCode -eq 0) { $exitCode = 2 }
} finally {
    Clear-Rules
    try { Remove-WebConfigurationLocation -Name $SiteName -ErrorAction SilentlyContinue } catch { }
    if (Test-Path "IIS:\Sites\$SiteName") { Remove-Website -Name $SiteName }
    if (Test-Path "IIS:\AppPools\$SiteName") { Remove-WebAppPool -Name $SiteName }
    Remove-Item -Recurse -Force $SiteRoot -ErrorAction SilentlyContinue
}
Say ("RESULT | ran {0}, failed {1}, exit {2}" -f $script:Ran, $script:Failures, $exitCode)
exit $exitCode
