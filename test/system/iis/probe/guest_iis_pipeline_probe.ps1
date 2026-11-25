# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# guest_iis_pipeline_probe.ps1 - The guest half of the IIS pipeline probe
# (probe_iis_html_cache_pipeline.ps1 runs it over PowerShell Direct). Run
# in a guest that has IIS and the PageSpeed module installed, with the probe
# DLL copied to C:\pipeprobe\bin\iis_pipeline_probe.dll, and the features
# Web-Dyn-Compression, Web-Stat-Compression, Web-Http-Tracing and
# Web-Asp-Net45.
#
# -Phase setup     provisions the probe sites, pools and module copies
# -Phase run       drives the request matrix and prints one result block
#                  per case (wire headers, what each probe copy saw)
# -Phase teardown  restores the IIS configuration backup setup took and
#                  removes the probe's files
#
# Every case prints lines "CASE <id> | <what> | <value>". INFO only: the
# probe records behaviour, it does not judge it.
# Windows PowerShell 5.1 compatible; ASCII only.

param(
    [ValidateSet("setup", "run", "teardown")][string]$Phase,
    [int]$Port = 8201
)

$ErrorActionPreference = "Stop"
Import-Module WebAdministration
$Apphost = "MACHINE/WEBROOT/APPHOST"
$Root = "C:\pipeprobe"
$Main = "probe-main"; $Same = "probe-same"; $Other = "probe-other"
$PMain = $Port; $PHost = $Port + 1; $PSame = $Port + 2; $POther = $Port + 3; $PIp = $Port + 4
$Copies = @("PipeProbeA", "PipeProbeB", "PipeProbeF")
# G is a second registration of A's image (one DLL, two module entries).
$Registrations = [ordered]@{ PipeProbeA = "PipeProbeA"; PipeProbeB = "PipeProbeB"; PipeProbeF = "PipeProbeF"; PipeProbeG = "PipeProbeA" }
$NotificationNames = @{ "1" = "BEGIN_REQUEST"; "2" = "AUTHENTICATE_REQUEST"; "4" = "AUTHORIZE_REQUEST"; "8" = "RESOLVE_REQUEST_CACHE"; "16" = "MAP_REQUEST_HANDLER"; "32" = "ACQUIRE_REQUEST_STATE"; "64" = "PRE_EXECUTE_REQUEST_HANDLER"; "128" = "EXECUTE_REQUEST_HANDLER"; "256" = "RELEASE_REQUEST_STATE"; "512" = "UPDATE_REQUEST_CACHE"; "1024" = "LOG_REQUEST"; "2048" = "END_REQUEST"; "536870912" = "SEND_RESPONSE" }
$FrebDir = "$Root\freb"

function Invoke-Raw([int]$P, [string]$Target, [string]$Method = "GET", [hashtable]$Headers = @{}, [string]$HostHdr = "") {
    if (-not $HostHdr) { $HostHdr = "localhost:$P" }
    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = 20000; $client.SendTimeout = 20000
    $client.Connect("127.0.0.1", $P)
    try {
        $stream = $client.GetStream()
        $req = "$Method $Target HTTP/1.1`r`nHost: $HostHdr`r`nConnection: close`r`n"
        foreach ($k in $Headers.Keys) { $req += "${k}: $($Headers[$k])`r`n" }
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
    $magic = ""
    if ($body.Length -ge 2) { $magic = "{0:x2}{1:x2}" -f $body[0], $body[1] }
    [pscustomobject]@{ Status = $status; Lines = @($lines | Select-Object -Skip 1); Body = $body; Magic = $magic
        Text = [Text.Encoding]::ASCII.GetString($body, 0, [Math]::Min(60, $body.Length)) }
}

function Clear-ProbeLogs { Get-ChildItem "$Root\log" -Filter *.log -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue }

function Read-ProbeLog([string]$Id) {
    $out = @()
    foreach ($f in (Get-ChildItem "$Root\log" -Filter *.log -ErrorAction SilentlyContinue)) {
        foreach ($l in (Get-Content -LiteralPath $f.FullName)) {
            if ($l.StartsWith("id=$Id|")) {
                $t = [uint64](($l -split '\|')[1].Substring(2))
                $out += [pscustomobject]@{ T = $t; Line = $l }
            }
        }
    }
    $out | Sort-Object T | ForEach-Object { $_.Line }
}

function Show([string]$Case, [string]$What, $Value) { Write-Output ("CASE {0} | {1} | {2}" -f $Case, $What, $Value) }

$script:Seq = 0
# One request; prints the wire answer and every probe record for it.
function Probe-Request([string]$Case, [int]$P, [string]$Target, [string]$Method = "GET", [hashtable]$Headers = @{}, [string]$HostHdr = "", [switch]$Quiet) {
    $script:Seq++
    $id = "{0}-{1}" -f $Case, $script:Seq
    $h = @{} + $Headers
    $h["X-Probe-Id"] = $id
    $r = Invoke-Raw $P $Target $Method $h $HostHdr
    Start-Sleep -Milliseconds 400
    Show $Case "request" ("{0} {1} host={2} headers={3}" -f $Method, $Target, $(if ($HostHdr) { $HostHdr } else { "localhost:$P" }), (($Headers.Keys | Sort-Object | ForEach-Object { "$_=" + $Headers[$_] }) -join "; "))
    if ($Quiet) {
        Show $Case "wire.brief" ("{0} len={1} magic={2} {3}" -f $r.Status, $r.Body.Length, $r.Magic, (($r.Lines | Where-Object { $_ -match '^(Content-Type|Content-Encoding|X-Probe-Pid):' }) -join "; "))
    } else {
        Show $Case "wire.status" $r.Status
        foreach ($l in $r.Lines) { Show $Case "wire.header" $l }
        Show $Case "wire.body" ("len={0} magic={1} text={2}" -f $r.Body.Length, $r.Magic, ($r.Text -replace '[^\x20-\x7e]', '.'))
    }
    if (-not $Quiet) { foreach ($l in (Read-ProbeLog $id)) { Show $Case "probe" ($l -replace '^id=[^|]*\|t=\d+\|', '') } }
    $script:Last = $r
}

function Setup {
    $appcmd = "$env:windir\system32\inetsrv\appcmd.exe"
    # Never replace a backup an earlier run left: it holds the clean state.
    if (Test-Path "$env:windir\system32\inetsrv\backup\pipeprobe-before") {
        throw "the IIS backup pipeprobe-before exists from an earlier run; restore and delete it first"
    }
    & $appcmd add backup "pipeprobe-before" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "taking the IIS configuration backup failed" }
    New-Item -ItemType Directory -Force "$Root\log", "$Root\bin", $FrebDir | Out-Null
    # The worker processes write the probe records and the traces.
    foreach ($d in @("$Root\log", $FrebDir)) { & icacls.exe $d /grant "IIS_IUSRS:(OI)(CI)M" | Out-Null }
    foreach ($c in $Copies) { Copy-Item "$Root\bin\iis_pipeline_probe.dll" "$Root\bin\$c.dll" -Force }
    foreach ($s in @($Main, $Same, $Other)) {
        if (Test-Path "IIS:\Sites\$s") { Remove-Website -Name $s }
        if (Test-Path "IIS:\AppPools\$s") { Remove-WebAppPool -Name $s }
    }
    foreach ($s in @($Main, $Other)) { New-WebAppPool -Name $s | Out-Null }
    foreach ($s in @($Main, $Same, $Other)) {
        $dir = "$Root\www\$s"
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$dir\sub", "$dir\cc" | Out-Null
        $para = "<p>   This   paragraph   has   runs   of   spaces   for   the   rewriter   to   collapse.   </p>`r`n"
        [IO.File]::WriteAllText("$dir\page.html", "<html><head><title>probe</title></head><body>`r`n<!-- a comment -->`r`n" + ($para * 40) + "</body></html>`r`n")
        Copy-Item "$dir\page.html" "$dir\sub\page.html"
        Copy-Item "$dir\page.html" "$dir\cc\page.html"
        [IO.File]::WriteAllText("$dir\plain.txt", ("plain text line for compression`r`n" * 150))
        [IO.File]::WriteAllText("$dir\cached.oct", ("output cache body`r`n" * 20))
        [IO.File]::WriteAllText("$dir\stream.aspx", "<%@ Page Language=`"C#`" %><% Response.ContentType = `"text/html`"; Response.Write(`"<html><head><title>s</title></head><body>one   `"); Response.Flush(); System.Threading.Thread.Sleep(100); Response.Write(`"two   `"); Response.Flush(); System.Threading.Thread.Sleep(100); Response.Write(`"three</body></html>`"); %>")
        [IO.File]::WriteAllText("$dir\pagespeed.config", (@(
            "pagespeed on",
            "pagespeed RewriteLevel PassThrough",
            "pagespeed EnableFilters collapse_whitespace,remove_comments"
        ) -join "`r`n") + "`r`n")
    }
    New-Website -Name $Main -Port $PMain -PhysicalPath "$Root\www\$Main" -ApplicationPool $Main | Out-Null
    New-WebBinding -Name $Main -Protocol http -Port $PMain -HostHeader "probe-a.test"
    New-WebBinding -Name $Main -Protocol http -Port $PHost -HostHeader "probe-c.test"
    New-Website -Name $Same -Port $PSame -PhysicalPath "$Root\www\$Same" -ApplicationPool $Main | Out-Null
    New-Website -Name $Other -Port $POther -HostHeader "probe-b.test" -PhysicalPath "$Root\www\$Other" -ApplicationPool $Other | Out-Null
    New-WebBinding -Name $Other -Protocol http -IPAddress "127.0.0.1" -Port $PIp
    foreach ($s in @($Main, $Same, $Other)) {
        # Operator headers that a Markdown answer must not carry.
        foreach ($pair in @(@("X-Probe-Custom", "site-custom"), @("Strict-Transport-Security", "max-age=60"), @("X-Content-Type-Options", "nosniff"))) {
            Add-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/httpProtocol/customHeaders" -Name "." -Value @{ name = $pair[0]; value = $pair[1] }
        }
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/staticContent/clientCache" -Name "cacheControlMode" -Value "UseMaxAge"
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/staticContent/clientCache" -Name "cacheControlMaxAge" -Value "01:00:00"
        Add-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/staticContent" -Name "." -Value @{ fileExtension = ".oct"; mimeType = "text/plain" }
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/urlCompression" -Name "doStaticCompression" -Value $true
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/urlCompression" -Name "doDynamicCompression" -Value $true
        Set-WebConfigurationProperty -PSPath $Apphost -Location $s -Filter "system.webServer/serverRuntime" -Name "frequentHitThreshold" -Value 1
    }
    # An operator header added after the page: no-store from customHeaders.
    Add-WebConfigurationProperty -PSPath $Apphost -Location "$Main/cc" -Filter "system.webServer/httpProtocol/customHeaders" -Name "." -Value @{ name = "Cache-Control"; value = "no-store" }
    # Server-level settings that only take effect at that level.
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "minFileSizeForComp" -Value 256
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "dynamicCompressionDisableCpuUsage" -Value 100
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "staticCompressionDisableCpuUsage" -Value 100
    # Failed request tracing for every status on the main site: the module
    # order of each notification and the compression events.
    Set-ItemProperty "IIS:\Sites\$Main" -Name traceFailedRequestsLogging -Value @{ enabled = $true; directory = $FrebDir; maxLogFiles = 200 }
    Add-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/tracing/traceFailedRequests" -Name "." -Value @{ path = "*" }
    $rule = "system.webServer/tracing/traceFailedRequests/add[@path='*']"
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "$rule/failureDefinitions" -Name "statusCodes" -Value "100-999"
    Add-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "$rule/traceAreas" -Name "." -Value @{ provider = "WWW Server"; areas = "Authentication,Security,Filter,StaticFile,CGI,Compression,Cache,RequestNotifications,Module"; verbosity = "Verbose" }
    # The probe copies: A just before the PageSpeed module, B just after it,
    # F and G (send at PRIORITY_ALIAS_FIRST) last; G shares A's image.
    foreach ($c in $Registrations.Keys) {
        if (-not (Get-WebGlobalModule -Name $c)) { New-WebGlobalModule -Name $c -Image ("$Root\bin\{0}.dll" -f $Registrations[$c]) | Out-Null }
    }
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
    $psIndex = [Array]::IndexOf($mods, "PageSpeedModule")
    if ($psIndex -lt 0) { throw "PageSpeedModule is not in system.webServer/modules" }
    Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value @{ name = "PipeProbeA" } -AtIndex $psIndex
    Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value @{ name = "PipeProbeB" } -AtIndex ($psIndex + 2)
    Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value @{ name = "PipeProbeF" }
    Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value @{ name = "PipeProbeG" }
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
    Write-Output ("SETUP | module order (tail) | {0}" -f (($mods | Select-Object -Last 6) -join ","))
    # The probe sites' ids, so teardown can remove their module caches: a
    # later site that reuses an id must not inherit the probe's entries.
    $ids = @(foreach ($s in @($Main, $Same, $Other)) { (Get-Website -Name $s).id })
    Set-Content -LiteralPath "$Root\site-ids.txt" -Value $ids -Encoding ASCII
    foreach ($id in $ids) { Remove-Item -Recurse -Force ("C:\ProgramData\We-Amp\PageSpeed\cache\{0}.ROOT" -f $id) -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        try { if ((Invoke-Raw $PMain "/plain.txt").Status -eq 200) { break } } catch { }
        Start-Sleep -Milliseconds 500
    }
    Write-Output ("SETUP | sites | {0}" -f ((Get-Website | Where-Object { $_.name -like "probe-*" } | ForEach-Object { "{0} id={1} pool={2} [{3}]" -f $_.name, $_.id, $_.applicationPool, (($_.bindings.Collection | ForEach-Object { $_.bindingInformation }) -join " ") }) -join "; "))
}

function Teardown {
    # Setup took an IIS configuration backup before it changed anything;
    # restoring it reverts every configuration change in one step.
    $appcmd = "$env:windir\system32\inetsrv\appcmd.exe"
    & $appcmd restore backup "pipeprobe-before" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "restoring the IIS configuration backup failed" }
    & $appcmd delete backup "pipeprobe-before" | Out-Null
    Start-Sleep -Seconds 2
    if (Test-Path "$Root\site-ids.txt") {
        foreach ($id in (Get-Content -LiteralPath "$Root\site-ids.txt")) {
            if ($id -match '^\d+$') { Remove-Item -Recurse -Force ("C:\ProgramData\We-Amp\PageSpeed\cache\{0}.ROOT" -f $id) -ErrorAction SilentlyContinue }
        }
        Remove-Item -Force "$Root\site-ids.txt"
    }
    Remove-Item -Recurse -Force "$Root\www", "$Root\log", $FrebDir -ErrorAction SilentlyContinue
    Write-Output ("TEARDOWN | done | configuration restored; probe sites left: {0}" -f @(Get-Website | Where-Object { $_.name -like "probe-*" }).Count)
}

function Set-Caching([bool]$On) {
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/caching" -Name "enabled" -Value $On
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/caching" -Name "enableKernelCache" -Value $On
}

function Freb-Order([string]$Case, [string]$Url) {
    # The newest trace for $Url: the module of every notification in order,
    # and each header, compression and cache event with the module it ran in.
    Start-Sleep -Seconds 2
    $files = Get-ChildItem $FrebDir -Recurse -Filter fr*.xml -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
    foreach ($f in $files) {
        [xml]$x = Get-Content -LiteralPath $f.FullName -Raw
        $req = $x.failedRequest
        if ($req -eq $null -or -not ($req.url -like "*$Url")) { continue }
        $current = ""
        $seen = @()
        foreach ($e in $x.failedRequest.Event) {
            $name = $e.RenderingInfo.Opcode
            $data = @{}
            foreach ($d in $e.EventData.Data) { $data[$d.Name] = $d.'#text' }
            if ($name -eq "NOTIFY_MODULE_START") {
                $n = $NotificationNames["" + $data["Notification"]]
                if (-not $n) { $n = "" + $data["Notification"] }
                $current = "{0}/{1}{2}" -f $data["ModuleName"], $n, $(if ($data["fIsPostNotification"] -eq "true") { "(post)" } else { "" })
                if ($n -in @("RESOLVE_REQUEST_CACHE", "SEND_RESPONSE", "MAP_REQUEST_HANDLER")) { $seen += $current }
            } elseif ($name -like "*COMPRESSION*" -or $name -like "*CACHE*" -or $name -eq "GENERAL_SET_RESPONSE_HEADER" -or $name -eq "GENERAL_RESPONSE_HEADERS" -or $name -eq "GENERAL_FLUSH_RESPONSE_START") {
                $vals = ($data.Keys | Sort-Object | ForEach-Object { "$_=" + $data[$_] }) -join " "
                if ($vals.Length -gt 300) { $vals = $vals.Substring(0, 300) }
                Show $Case "freb.event" ("in {0}: {1} {2}" -f $current, $name, $vals)
            }
        }
        Show $Case "freb.order" ($seen -join " > ")
        return
    }
    Show $Case "freb.order" "no trace found"
}

function Kernel-Hits([string]$Path) {
    $t = (& netsh http show cachestate "url=http://localhost:$PMain$Path") -join " "
    if ($t -match 'Hit count:\s*(\d+)') { "cached, hit count " + $Matches[1] } else { "not in the kernel cache" }
}

function Run {
    # No IIS response cache answers a request on its own until the cache
    # cases below turn them on.
    Set-Caching $false
    Clear-ProbeLogs
    $mdHdr = @{ "Accept" = "text/markdown"; "Accept-Encoding" = "gzip, deflate" }

    # 1, 2, 5: static files and an HTML page the module rewrites, gzip asked.
    Probe-Request "static_txt" $PMain "/plain.txt" "GET" @{ "Accept-Encoding" = "gzip" }
    Probe-Request "static_txt_2" $PMain "/plain.txt" "GET" @{ "Accept-Encoding" = "gzip" }
    Freb-Order "static_txt_2" "/plain.txt"
    Probe-Request "html" $PMain "/page.html" "GET" @{ "Accept-Encoding" = "gzip" }
    Probe-Request "html_2" $PMain "/page.html" "GET" @{ "Accept-Encoding" = "gzip" }
    Freb-Order "html_2" "/page.html"
    Probe-Request "html_noae" $PMain "/page.html" "GET" @{}

    # 4: a streamed origin (three flushes) the module rewrites.
    Probe-Request "aspx_stream" $PMain "/stream.aspx" "GET" @{ "Accept-Encoding" = "gzip" }
    # An operator no-store added by customHeaders, against both views.
    Probe-Request "html_cc_nostore" $PMain "/cc/page.html" "GET" @{ "Accept-Encoding" = "gzip" }

    # 1, 5: a module answer at RESOLVE_REQUEST_CACHE, with and without a strip,
    # and the no-compress marks.
    Probe-Request "serve" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve" })
    Freb-Order "serve" "/page.html"
    Probe-Request "serve_strip_A" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve,A.strip" })
    Probe-Request "serve_strip_B" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve,B.strip" })
    Probe-Request "serve_strip_F" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve,F.strip" })
    Probe-Request "serve_strip_nc_ae" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve,A.strip,A.nc_ae" })
    Probe-Request "serve_strip_nc_sv" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve,A.strip,A.nc_sv" })
    Probe-Request "serve_strip_nc_ce" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve,F.strip,A.nc_ce" })
    Probe-Request "serve_strip_head" $PMain "/page.html" "HEAD" ($mdHdr + @{ "X-Probe-Act" = "A.serve,A.strip,A.nc_ae" })
    # The candidate recipe: answer at the module's position, no-compress by
    # server variable, strip from a second registration of the same image
    # whose send runs at PRIORITY_ALIAS_FIRST.
    Probe-Request "recipe_200" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve,A.nc_sv,G.strip" })
    Freb-Order "recipe_200" "/page.html"
    Probe-Request "recipe_head" $PMain "/page.html" "HEAD" ($mdHdr + @{ "X-Probe-Act" = "A.serve,A.nc_sv,G.strip" })
    Probe-Request "recipe_304" $PMain "/page.html" "GET" ($mdHdr + @{ "If-None-Match" = "W/`"psm-probe`""; "X-Probe-Act" = "A.serve304,A.nc_sv,G.strip" })
    Probe-Request "plain_304_nostrip" $PMain "/page.html" "GET" ($mdHdr + @{ "X-Probe-Act" = "A.serve304,A.nc_sv" })
    # A static text file, compressible: the no-compress marks on the origin
    # path, where static compression applies.
    Probe-Request "static_nc_ae" $PMain "/plain.txt" "GET" @{ "Accept-Encoding" = "gzip"; "X-Probe-Act" = "A.nc_ae" }
    Probe-Request "static_nc_sv" $PMain "/plain.txt" "GET" @{ "Accept-Encoding" = "gzip"; "X-Probe-Act" = "A.nc_sv" }

    # 6: writes after a PENDING send (the module's rewrite hand-back).
    Probe-Request "rw" $PMain "/plain.txt" "GET" @{ "X-Probe-Act" = "A.rw" }
    Probe-Request "rwflush" $PMain "/plain.txt" "GET" @{ "X-Probe-Act" = "A.rwflush" }

    # 3: bindings as each worker process reads them.
    Probe-Request "bindings_main" $PMain "/plain.txt" "GET" @{ "X-Probe-Act" = "A.bindings" }
    Probe-Request "bindings_same_pool" $PSame "/plain.txt" "GET" @{ "X-Probe-Act" = "A.bindings" }
    Probe-Request "bindings_other_pool" $POther "/plain.txt" "GET" @{ "X-Probe-Act" = "A.bindings" } "probe-b.test:$POther"
    Probe-Request "binding_host" $PMain "/plain.txt" "GET" @{} "probe-a.test:$PMain" -Quiet
    Probe-Request "binding_unlisted_host" $PMain "/plain.txt" "GET" @{} "evil.test:$PMain" -Quiet
    foreach ($pool in @($Main, $Other)) {
        $cfg = "C:\inetpub\temp\appPools\$pool\$pool.config"
        if (Test-Path $cfg) {
            $names = @(Select-String -LiteralPath $cfg -Pattern '<site name="([^"]+)"' -AllMatches | ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value })
            Show "bindings_isolation" "pool $pool config sites" ($names -join ",")
        } else {
            Show "bindings_isolation" "pool $pool config" "not found"
        }
    }

    # 9: request-path forms, as IIS cooks them and as the module's global
    # hook keeps them.
    foreach ($t in @("/page.html", "/PAGE.HTML", "/sub/../page.html", "/sub/%2E%2E/page.html", "/sub%2Fpage.html", "/sub%5Cpage.html", "/page.html.", "//page.html", "/sub//page.html", "/page%2Ehtml", "/sub/./page.html", "/%70age.html")) {
        Probe-Request ("path " + $t) $PMain $t "GET" @{}
    }

    # 8: the kernel and output caches.
    Set-Caching $true
    Clear-ProbeLogs
    for ($i = 0; $i -lt 4; $i++) { Probe-Request "kernel_txt" $PMain "/plain.txt" "GET" @{} -Quiet }
    Show "kernel_cache" "hits after 4 plain GETs" (Kernel-Hits "/plain.txt")
    Probe-Request "kernel_txt_markdown" $PMain "/plain.txt" "GET" @{ "Accept" = "text/markdown"; "X-Probe-Act" = "A.serve" }
    Show "kernel_cache" "hits after a GET with Accept: text/markdown" (Kernel-Hits "/plain.txt")
    Probe-Request "kernel_txt_markdown_bare" $PMain "/plain.txt" "GET" @{ "Accept" = "text/markdown" } -Quiet
    Show "kernel_cache" "hits after a second GET with Accept: text/markdown" (Kernel-Hits "/plain.txt")
    Probe-Request "kernel_txt_plain_again" $PMain "/plain.txt" "GET" @{} -Quiet
    Show "kernel_cache" "hits after one more plain GET" (Kernel-Hits "/plain.txt")
    Probe-Request "kernel_txt_markdown_wildcard" $PMain "/plain.txt" "GET" @{ "Accept" = "text/markdown, */*;q=0.1"; "X-Probe-Act" = "A.serve" }
    Show "kernel_cache" "hits after a GET with Accept: text/markdown, */*;q=0.1" (Kernel-Hits "/plain.txt")
    for ($i = 0; $i -lt 4; $i++) { Probe-Request "kernel_html" $PMain "/sub/page.html" "GET" @{} -Quiet }
    Probe-Request "kernel_html_markdown" $PMain "/sub/page.html" "GET" @{ "Accept" = "text/markdown"; "X-Probe-Act" = "A.serve" }
    for ($i = 0; $i -lt 4; $i++) { Probe-Request "kernel_serve_nodisarm" $PMain "/sub/x.md" "GET" @{ "X-Probe-Act" = "A.serve,A.nodisarm" } -Quiet }
    for ($i = 0; $i -lt 4; $i++) { Probe-Request "kernel_serve_disarm" $PMain "/sub/y.md" "GET" @{ "X-Probe-Act" = "A.serve" } -Quiet }
    $state = (& netsh http show cachestate) -join " "
    Show "kernel_cache" "netsh cachestate" (($state -replace '\s+', ' ').Substring(0, [Math]::Min(1500, ($state -replace '\s+', ' ').Length)))
    $counts = @{}
    foreach ($f in (Get-ChildItem "$Root\log" -Filter "PipeProbeA-*.log")) {
        foreach ($l in (Get-Content -LiteralPath $f.FullName)) {
            if ($l -match '^id=([a-z_]+)-\d+\|.*\|ev=begin\|') { $counts[$Matches[1]] = 1 + [int]$counts[$Matches[1]] }
        }
    }
    foreach ($k in ($counts.Keys | Sort-Object)) { Show "kernel_cache" "requests that reached the module pipeline" ("{0}={1}" -f $k, $counts[$k]) }

    Add-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/caching/profiles" -Name "." -Value @{ extension = ".oct"; policy = "CacheForTimePeriod"; duration = "00:00:30"; kernelCachePolicy = "DontCache"; location = "Any" }
    Clear-ProbeLogs
    for ($i = 0; $i -lt 3; $i++) { Probe-Request "output_cache" $PMain "/cached.oct" "GET" @{} -Quiet }
    Freb-Order "output_cache" "/cached.oct"
    Probe-Request "output_cache_markdown" $PMain "/cached.oct" "GET" @{ "Accept" = "text/markdown"; "X-Probe-Act" = "A.serve" }
    Freb-Order "output_cache_markdown" "/cached.oct"
    $counts = @{}
    foreach ($f in (Get-ChildItem "$Root\log" -Filter "PipeProbeA-*.log")) {
        foreach ($l in (Get-Content -LiteralPath $f.FullName)) {
            if ($l -match '^id=([a-z_]+)-\d+\|.*\|ev=(begin|resolve_cache)\|') { $k = $Matches[1] + "." + $Matches[2]; $counts[$k] = 1 + [int]$counts[$k] }
        }
    }
    foreach ($k in ($counts.Keys | Sort-Object)) { Show "output_cache" "probe A notifications" ("{0}={1}" -f $k, $counts[$k]) }
    Remove-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/caching/profiles" -Name "." -AtElement @{ extension = ".oct" }
    Set-Caching $false

    # 7: a web garden.
    Set-ItemProperty "IIS:\AppPools\$Main" -Name processModel.maxProcesses -Value 2
    Restart-WebAppPool -Name $Main
    Start-Sleep -Seconds 2
    $pids = @{}
    for ($i = 0; $i -lt 40; $i++) {
        $null = Probe-Request "garden" $PMain "/plain.txt" "GET" @{ "X-Probe-Act" = "A.pid" } -Quiet
        $r = $script:Last
        $p = ($r.Lines | Where-Object { $_ -like "X-Probe-Pid:*" }) -replace 'X-Probe-Pid:\s*', ''
        $pids["$p"] = 1 + [int]$pids["$p"]
    }
    Show "garden" "responses per worker process" (($pids.Keys | Sort-Object | ForEach-Object { "pid#{0}={1}" -f ([Array]::IndexOf(@($pids.Keys | Sort-Object), $_)), $pids[$_] }) -join " ")
    Show "garden" "w3wp processes for the pool" (@(Get-CimInstance Win32_Process -Filter "Name='w3wp.exe'" | Where-Object { $_.CommandLine -like "*$Main*" }).Count)
    Set-ItemProperty "IIS:\AppPools\$Main" -Name processModel.maxProcesses -Value 1
    Restart-WebAppPool -Name $Main
}

switch ($Phase) {
    "setup" { Setup }
    "run" { Run }
    "teardown" { Teardown }
}
