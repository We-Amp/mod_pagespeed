# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# guest_iis_html_report.ps1 - The guest half of the IIS HTML cache report
# check (rig_iis_html_report.ps1 runs it over PowerShell Direct). Run in a
# guest with IIS, the features Web-Dyn-Compression and Web-Stat-Compression,
# and the PageSpeed MSI installed with its optimizer.
#
# -Phase setup     takes an IIS configuration backup, starts the optimizer
#                  service, and provisions one pool and four sites that
#                  differ only in their DaemonHtmlCache setting:
#                    htmlrep-off      (Port)      off
#                    htmlrep-report   (Port + 1)  report-only
#                    htmlrep-variants (Port + 2)  serve-variants
#                    htmlrep-nofinal  (Port + 3)  report-only, and the site's
#                                                 web.config removes the
#                                                 PageSpeedFinalView entry
#                  Every site has the same pages, the same customHeaders and
#                  a Cache-Control: no-store customHeaders line on
#                  /nostore.html; static and dynamic compression are on.
# -Phase run       drives the request matrix and prints "RESULT <id> PASS|FAIL
#                  <detail>" lines, then "SUMMARY <passed> <failed>"
# -Phase teardown  restores the backup, stops the optimizer service if setup
#                  started it, and removes the files
#
# The checks:
#   differential       every page and request variant is answered with the same
#                      status, header lines (Date aside) and body bytes on all
#                      four sites
#   decision-lines     report-only and serve-variants log a line for each
#                      page; off logs none
#   customheaders      the operator no-store in customHeaders is a final-view
#                      refusal (iis-store-customheaders-nostore)
#   static-gzip        a page static compression served compressed is
#                      evaluated on its decoded bytes (iis-store-static-gzip)
#   no-final-view      without the second entry the report reads the store
#                      view (no no_store refusal), says so once in the
#                      message history, and the page is still rewritten
#   unavailable        no daemon_html_unavailable for a library that classifies
#
# Windows PowerShell 5.1 compatible; ASCII only.

param(
    [ValidateSet("setup", "run", "teardown")][string]$Phase,
    [int]$Port = 8301
)

$ErrorActionPreference = "Stop"
Import-Module WebAdministration
$Apphost = "MACHINE/WEBROOT/APPHOST"
$Root = "C:\htmlrep"
$Pool = "htmlrep"
$Backup = "htmlrep-before"
$Sites = [ordered]@{
    "htmlrep-off"      = @{ Port = $Port;     Mode = "off" }
    "htmlrep-report"   = @{ Port = $Port + 1; Mode = "report-only" }
    "htmlrep-variants" = @{ Port = $Port + 2; Mode = "serve-variants" }
    "htmlrep-nofinal"  = @{ Port = $Port + 3; Mode = "report-only" }
}
$Volume = "C:\ProgramData\We-Amp\PageSpeed\optimizer\cache"
$Service = "WeAmpPageSpeedOptimizer"

function Invoke-Raw([int]$P, [string]$Target, [string]$Method = "GET", [hashtable]$Headers = @{}) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = 20000; $client.SendTimeout = 20000
    $client.Connect("127.0.0.1", $P)
    try {
        $stream = $client.GetStream()
        $req = "$Method $Target HTTP/1.1`r`nHost: localhost:$P`r`nConnection: close`r`n"
        foreach ($k in ($Headers.Keys | Sort-Object)) { $req += "${k}: $($Headers[$k])`r`n" }
        $req += "`r`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($req)
        $stream.Write($bytes, 0, $bytes.Length)
        $ms = New-Object IO.MemoryStream
        $stream.CopyTo($ms)
    } finally { $client.Close() }
    $all = $ms.ToArray()
    $sep = -1
    for ($i = 0; $i -lt $all.Length - 3; $i++) {
        if ($all[$i] -eq 13 -and $all[$i + 1] -eq 10 -and $all[$i + 2] -eq 13 -and $all[$i + 3] -eq 10) { $sep = $i; break }
    }
    if ($sep -lt 0) { $sep = $all.Length }
    $head = [Text.Encoding]::ASCII.GetString($all, 0, $sep)
    $bodyStart = [Math]::Min($sep + 4, $all.Length)
    $body = New-Object byte[] ($all.Length - $bodyStart)
    [Array]::Copy($all, $bodyStart, $body, 0, $body.Length)
    $lines = $head -split "`r`n"
    $status = 0
    if ($lines[0] -match '^HTTP/\d\.\d (\d{3})') { $status = [int]$Matches[1] }
    [pscustomobject]@{ Status = $status; Lines = @($lines | Select-Object -Skip 1); Body = $body }
}

function Get-Text([int]$P, [string]$Target) {
    $r = Invoke-Raw $P $Target
    [pscustomobject]@{ Status = $r.Status; Text = [Text.Encoding]::UTF8.GetString($r.Body) }
}

$script:Passed = 0
$script:Failed = 0
function Check([string]$Id, [bool]$Ok, [string]$Detail) {
    if ($Ok) { $script:Passed++; $verdict = "PASS" } else { $script:Failed++; $verdict = "FAIL" }
    Write-Output ("RESULT {0} {1} {2}" -f $Id, $verdict, $Detail)
}

function Setup {
    $appcmd = "$env:windir\system32\inetsrv\appcmd.exe"
    if (Test-Path "$env:windir\system32\inetsrv\backup\$Backup") {
        throw "the IIS backup $Backup exists from an earlier run; restore and delete it first"
    }
    if (-not (Test-Path "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll")) { throw "the PageSpeed module is not installed" }
    if (-not (Get-WebGlobalModule -Name "PageSpeedFinalView")) { throw "the PageSpeedFinalView global module is not registered" }
    $svc = Get-Service -Name $Service -ErrorAction SilentlyContinue
    if (-not $svc) { throw "the optimizer service is not installed (an MSI built with -OptimizerDir is needed)" }
    & $appcmd add backup $Backup | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "taking the IIS configuration backup failed" }
    New-Item -ItemType Directory -Force $Root | Out-Null
    if ($svc.Status -ne "Running") {
        & sc.exe config $Service start= demand | Out-Null
        Start-Service -Name $Service
        Set-Content -LiteralPath "$Root\service-started.txt" -Value "1" -Encoding ASCII
    }
    (Get-Service -Name $Service).WaitForStatus("Running", [TimeSpan]::FromSeconds(30))

    foreach ($s in $Sites.Keys) {
        if (Test-Path "IIS:\Sites\$s") { Remove-Website -Name $s }
    }
    if (Test-Path "IIS:\AppPools\$Pool") { Remove-WebAppPool -Name $Pool }
    New-WebAppPool -Name $Pool | Out-Null
    Set-ItemProperty "IIS:\AppPools\$Pool" -Name managedRuntimeVersion -Value ""

    $para = "<p>   This   paragraph   has   runs   of   spaces   for   the   rewriter   to   collapse.   </p>`r`n"
    $page = "<html><head><title>report</title></head><body>`r`n<!-- a comment -->`r`n" + ($para * 4) + "</body></html>`r`n"
    $large = "<html><head><title>gzip</title></head><body>`r`n<!-- a comment -->`r`n" + ($para * 60) + "</body></html>`r`n"
    foreach ($s in $Sites.Keys) {
        $dir = "$Root\www\$s"
        New-Item -ItemType Directory -Force $dir, "$Root\logs\$s", "$Root\cache\$s" | Out-Null
        foreach ($name in @("plain.html", "nostore.html", "cookie.html", "head.html", "markdown.html", "bare.html")) {
            [IO.File]::WriteAllText("$dir\$name", $page)
        }
        [IO.File]::WriteAllText("$dir\gzip.html", $large)
        # One modification time for every copy: a page the module leaves
        # alone (a HEAD) carries Last-Modified and an entity tag made from it.
        Get-ChildItem $dir -Filter *.html | ForEach-Object { $_.LastWriteTimeUtc = [DateTime]::new(2026, 1, 1, 0, 0, 0, [DateTimeKind]::Utc) }
        # The per-site file is layered last over the machine-wide one. ASCII:
        # the module's configuration reader has no byte order mark handling.
        $cfg = @(
            "pagespeed DaemonSocketPath pagespeed-optimizer",
            "pagespeed DaemonVolumePath `"$Volume`"",
            "pagespeed DaemonHtmlCache $($Sites[$s].Mode)",
            "pagespeed LogDir `"$Root\logs\$s`"",
            "pagespeed FileCachePath `"$Root\cache\$s`"",
            "pagespeed MessageBufferSize 200000",
            "pagespeed RewriteLevel PassThrough",
            "pagespeed EnableFilters collapse_whitespace,remove_comments"
        ) -join "`r`n"
        Set-Content -LiteralPath "$dir\pagespeed.config" -Value $cfg -Encoding ASCII
        if ($s -eq "htmlrep-nofinal") {
            Set-Content -LiteralPath "$dir\web.config" -Encoding ASCII -Value (@(
                '<?xml version="1.0" encoding="utf-8"?>',
                '<configuration><system.webServer><modules>',
                '<remove name="PageSpeedFinalView" />',
                '</modules></system.webServer></configuration>') -join "`r`n")
        }
        New-Website -Name $s -Port $Sites[$s].Port -PhysicalPath $dir -ApplicationPool $Pool | Out-Null
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/staticContent/clientCache" -Name "cacheControlMode" -Value "UseMaxAge"
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/staticContent/clientCache" -Name "cacheControlMaxAge" -Value "01:00:00"
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/urlCompression" -Name "doStaticCompression" -Value $true
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/urlCompression" -Name "doDynamicCompression" -Value $true
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/serverRuntime" -Name "frequentHitThreshold" -Value 1
        Add-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/httpProtocol/customHeaders" -Name "." -Value @{ name = "X-Site-Custom"; value = "1" }
        Add-WebConfigurationProperty -PSPath $Apphost -Location "$s/nostore.html" -Filter "system.webServer/httpProtocol/customHeaders" -Name "." -Value @{ name = "Cache-Control"; value = "no-store" }
    }
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "minFileSizeForComp" -Value 256
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "dynamicCompressionDisableCpuUsage" -Value 100
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "staticCompressionDisableCpuUsage" -Value 100
    & icacls.exe $Root /grant "IIS_IUSRS:(OI)(CI)M" | Out-Null
    $ids = @(foreach ($s in $Sites.Keys) { (Get-Website -Name $s).id })
    Set-Content -LiteralPath "$Root\site-ids.txt" -Value $ids -Encoding ASCII
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
    Write-Output ("SETUP | modules | first={0} pagespeed_at={1} count={2}" -f $mods[0], [Array]::IndexOf($mods, "PageSpeedModule"), $mods.Count)
    $deadline = (Get-Date).AddSeconds(90)
    foreach ($s in $Sites.Keys) {
        while ((Get-Date) -lt $deadline) {
            try { if ((Invoke-Raw $Sites[$s].Port "/plain.html").Status -eq 200) { break } } catch { }
            Start-Sleep -Milliseconds 500
        }
    }
    Write-Output ("SETUP | sites | {0}" -f (($Sites.Keys | ForEach-Object { "{0}={1}:{2}" -f $_, $Sites[$_].Port, $Sites[$_].Mode }) -join " "))
}

function Teardown {
    $appcmd = "$env:windir\system32\inetsrv\appcmd.exe"
    & $appcmd restore backup $Backup | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "restoring the IIS configuration backup failed" }
    & $appcmd delete backup $Backup | Out-Null
    if (Test-Path "$Root\service-started.txt") {
        Stop-Service -Name $Service -ErrorAction SilentlyContinue
        & sc.exe config $Service start= disabled | Out-Null
    }
    Start-Sleep -Seconds 2
    Remove-Item -Recurse -Force $Root -ErrorAction SilentlyContinue
    Write-Output ("TEARDOWN | done | configuration restored; report sites left: {0}" -f @(Get-Website | Where-Object { $_.name -like "htmlrep-*" }).Count)
}

# One response as compared across the sites: status, header lines without
# Date (each response has its own), and the body.
function Snapshot($r, [string]$Site) {
    # IIS's own error pages name the site's physical path, port and id:
    # compared with those taken out, and their length with it (as the Apache and nginx
    # lanes take the host name out).
    $text = [Text.Encoding]::GetEncoding(28591).GetString($r.Body)
    $plain = $text.Replace($Site, "SITE").Replace([string]$Sites[$Site].Port, "PORT").Replace([string](Get-Website -Name $Site).id, "ID")
    $shrink = $text.Length - $plain.Length
    $body = [Text.Encoding]::GetEncoding(28591).GetBytes($plain)
    $lines = @()
    foreach ($l in $r.Lines) {
        if ($l -match '^(?i)date:') { continue }
        if ($shrink -ne 0 -and $l -match '^(?i)content-length:\s*(\d+)$') { $l = "Content-Length: $([int64]$Matches[1] - $shrink)" }
        $lines += $l
    }
    $hash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($body)).Replace("-", "").Substring(0, 16)
    "{0}|{1}|{2}|{3}" -f $r.Status, ($lines -join "~"), $body.Length, $hash
}

function Read-Decisions([string]$Site) {
    $out = @()
    foreach ($f in (Get-ChildItem "$Root\logs\$Site\daemon-html" -Filter "decisions-*.jsonl" -ErrorAction SilentlyContinue)) {
        foreach ($l in (Get-Content -LiteralPath $f.FullName)) {
            if ($l.Trim()) { $out += ($l | ConvertFrom-Json) }
        }
    }
    $out
}

function Decision([string]$Site, [string]$Path) {
    $deadline = (Get-Date).AddSeconds(10)
    do {
        $hit = @(Read-Decisions $Site | Where-Object { $_.path -eq $Path })
        if ($hit.Count -gt 0) { return $hit[0] }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    $null
}

function Stat([int]$P, [string]$Name) {
    $t = (Get-Text $P "/pagespeed_admin/statistics").Text
    if ($t -match "(?m)^$Name\s*:\s*(\d+)") { return [int64]$Matches[1] }
    if ($t -match "$Name\D{1,40}?(\d+)") { return [int64]$Matches[1] }
    -1
}

function Run {
    $requests = [ordered]@{
        "plain.html"    = @{ Method = "GET";  Headers = @{ "Accept-Encoding" = "identity" } }
        "gzip.html"     = @{ Method = "GET";  Headers = @{ "Accept-Encoding" = "gzip" } }
        "cookie.html"   = @{ Method = "GET";  Headers = @{ "Accept-Encoding" = "identity"; "Cookie" = "a=b" } }
        "head.html"     = @{ Method = "HEAD"; Headers = @{ "Accept-Encoding" = "identity" } }
        "markdown.html" = @{ Method = "GET";  Headers = @{ "Accept-Encoding" = "identity"; "Accept" = "text/markdown, text/html;q=0.5" } }
        # Markdown alone: IIS's static file handler answers 406, on every site.
        "bare.html"     = @{ Method = "GET";  Headers = @{ "Accept-Encoding" = "identity"; "Accept" = "text/markdown" } }
        "nostore.html"  = @{ Method = "GET";  Headers = @{ "Accept-Encoding" = "identity" } }
    }
    $before = Stat $Sites["htmlrep-report"].Port "daemon_html_unavailable"

    # Static compression compresses a file on its first hit (the threshold is
    # 1) and serves the compressed copy from its cache on the next; the
    # warm-up pass makes every later answer the steady one.
    foreach ($pass in @("warm", "compare")) {
        $snap = @{}
        foreach ($s in $Sites.Keys) {
            foreach ($page in $requests.Keys) {
                $req = $requests[$page]
                $r = Invoke-Raw $Sites[$s].Port "/$page" $req.Method $req.Headers
                $snap["$s|$page"] = Snapshot $r $s
                if ($pass -eq "compare" -and $s -eq "htmlrep-off") {
                    Write-Output ("INFO | {0} | status={1} lines={2}" -f $page, $r.Status, (($r.Lines | Where-Object { $_ -notmatch '^(?i)date:' }) -join "; "))
                }
            }
        }
        if ($pass -eq "compare") {
            foreach ($page in $requests.Keys) {
                $want = $snap["htmlrep-off|$page"]
                foreach ($s in $Sites.Keys) {
                    if ($s -eq "htmlrep-off") { continue }
                    $got = $snap["$s|$page"]
                    Check "differential:$s/$page" ($got -eq $want) $(if ($got -eq $want) { "identical to off" } else { "off=[$want] $s=[$got]" })
                }
            }
        }
    }

    # The page is rewritten on every site (the module answered it).
    $r = Invoke-Raw $Sites["htmlrep-nofinal"].Port "/plain.html"
    $text = [Text.Encoding]::ASCII.GetString($r.Body)
    Check "no-final-view:rewritten" ($r.Status -eq 200 -and ($r.Lines -match '^(?i)x-page-speed:').Count -gt 0 -and $text -notmatch 'a comment') ("status={0} x-page-speed={1}" -f $r.Status, (($r.Lines -match '^(?i)x-page-speed:') -join ","))

    # Decision lines.
    foreach ($page in @("plain.html", "cookie.html", "markdown.html", "gzip.html", "nostore.html")) {
        foreach ($s in @("htmlrep-report", "htmlrep-variants", "htmlrep-nofinal")) {
            $d = Decision $s "/$page"
            Check "decision-lines:$s/$page" ($d -ne $null) $(if ($d) { "mode={0} action={1} reason={2} size={3} lifetime_s={4} cc={5} vhost_added={6}" -f $d.mode, $d.action, $d.reason, $d.size, $d.lifetime_s, $d.cc, $d.vhost_added } else { "no line" })
        }
    }
    $offLines = @(Read-Decisions "htmlrep-off")
    Check "decision-lines:htmlrep-off" ($offLines.Count -eq 0) ("{0} lines" -f $offLines.Count)
    $heads = @(Read-Decisions "htmlrep-report" | Where-Object { $_.path -eq "/head.html" })
    Check "decision-lines:head-not-evaluated" ($heads.Count -eq 0) ("{0} lines for the HEAD" -f $heads.Count)

    # The operator no-store from customHeaders: a final-view refusal.
    foreach ($s in @("htmlrep-report", "htmlrep-variants")) {
        $d = Decision $s "/nostore.html"
        $plain = Decision $s "/plain.html"
        Check "customheaders:$s" ($d -ne $null -and $d.action -eq "refuse" -and $d.reason -eq "no_store" -and $plain -ne $null -and $plain.reason -ne "no_store") ("nostore: action={0} reason={1}; plain: action={2} reason={3}" -f $d.action, $d.reason, $plain.action, $plain.reason)
    }

    # Static compression ran before the module (its cache holds the file), and
    # the page was evaluated on the decoded bytes: the line's size is the
    # file's, and it is not refused for its encoding.
    $len = (Get-Item "$Root\www\htmlrep-report\gzip.html").Length
    $cached = @(Get-ChildItem "$env:SystemDrive\inetpub\temp\IIS Temporary Compressed Files" -Recurse -Filter "gzip.html*" -ErrorAction SilentlyContinue)
    foreach ($s in @("htmlrep-report", "htmlrep-variants")) {
        $d = Decision $s "/gzip.html"
        $plain = Decision $s "/plain.html"
        Check "static-gzip:$s" ($cached.Count -gt 0 -and $d -ne $null -and [int64]$d.size -eq $len -and $d.reason -ne "encoding" -and $d.action -eq $plain.action -and $d.reason -eq $plain.reason) ("compressed copies={0} size={1} file={2} action={3} reason={4} plain_action={5}" -f $cached.Count, $d.size, $len, $d.action, $d.reason, $plain.action)
    }

    # Without the second entry: the store view only, so the customHeaders
    # no-store is not seen; logged once.
    $d = Decision "htmlrep-nofinal" "/nostore.html"
    $plain = Decision "htmlrep-nofinal" "/plain.html"
    Check "no-final-view:store-view" ($d -ne $null -and $d.reason -ne "no_store" -and $plain -ne $null -and $d.action -eq $plain.action) ("nostore: action={0} reason={1}; plain: action={2}" -f $d.action, $d.reason, $plain.action)
    $hist = (Get-Text $Sites["htmlrep-nofinal"].Port "/pagespeed_admin/message_history").Text
    $mentions = ([regex]::Matches($hist, "PageSpeedFinalView module entry did not run")).Count
    Check "no-final-view:logged-once" ($mentions -eq 1) ("{0} message lines" -f $mentions)
    $histReport = (Get-Text $Sites["htmlrep-report"].Port "/pagespeed_admin/message_history").Text
    $other = ([regex]::Matches($histReport, "PageSpeedFinalView module entry did not run")).Count
    Check "no-final-view:not-logged-with-entry" ($other -eq 0) ("{0} message lines on the report site" -f $other)
    $errors = ([regex]::Matches($hist, "(?i)corrupted inflation|could not read a response body chunk")).Count
    Check "no-final-view:no-html-errors" ($errors -eq 0) ("{0} HTML path errors" -f $errors)

    $after = Stat $Sites["htmlrep-report"].Port "daemon_html_unavailable"
    $evaluated = Stat $Sites["htmlrep-report"].Port "daemon_html_evaluated"
    Check "unavailable" ($before -ge 0 -and $after -eq $before -and $evaluated -gt 0) ("daemon_html_unavailable {0} -> {1}; daemon_html_evaluated {2}" -f $before, $after, $evaluated)

    Write-Output ("SUMMARY {0} {1}" -f $script:Passed, $script:Failed)
}

switch ($Phase) {
    "setup" { Setup }
    "run" { Run }
    "teardown" { Teardown }
}
