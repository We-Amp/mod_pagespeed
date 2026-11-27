# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# guest_iis_serve_html_probe.ps1 - The guest half of the IIS probe for
# serving a stored origin page (probe_iis_html_cache_pipeline.ps1
# -Matrix serve-html runs it over PowerShell Direct). Run in a lab guest
# that has IIS 10, the PageSpeed MSI with its optimizer, the features
# Web-Dyn-Compression, Web-Stat-Compression, Web-Http-Tracing and
# Web-Asp-Net45, and the probe DLL at C:\pipeprobe\bin\iis_pipeline_probe.dll.
# The URL Rewrite module is used when it is installed, or when its
# installer is at C:\pipeprobe\bin\url-rewrite.msi.
#
# The question: when a module answers a request at RESOLVE_REQUEST_CACHE
# with a page it stored earlier, status 200 and the page's stored header
# lines, does the rest of the send path treat that answer as it treats
# the handler's response on a miss -- the PageSpeed module's rewriter
# included -- and does everything that runs after the capture point run
# exactly once?
#
# The probe stands in for that answer. Copy B sits right after the
# PageSpeed module at RESOLVE_REQUEST_CACHE and right before it at
# SEND_RESPONSE, which is where the PageSpeed module's capture reads the
# response. On a miss, B records what it sees at its send (the bytes and
# header table the capture would store, action "capture"). On a hit, B
# answers at RESOLVE_REQUEST_CACHE with those bytes and the stored header
# lines (action "serve_html") and finishes the request, after the PageSpeed
# module's own RESOLVE_REQUEST_CACHE has run for it, as it would for a
# page it then answers itself.
#
# -Phase setup     takes an IIS configuration backup, installs URL Rewrite
#                  when its installer was copied, provisions site
#                  probe-main (pool probe-main, http *:<Port>:probe-a.test),
#                  the probe copies (A before the PageSpeed module, B after
#                  it, F and G with their send at PRIORITY_ALIAS_FIRST, H a
#                  second registration of B's image with its send at
#                  PRIORITY_ALIAS_FIRST), DaemonHtmlCache serve-variants with
#                  AgentOptimize on, the HTML cache test trace, failed-request
#                  tracing, and starts the optimizer service
# -Phase run       drives the matrix and prints one block per case
# -Phase teardown  restores the backup, the optimizer service's command
#                  line and state, and removes the probe's files
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
$Main = "probe-main"
$PMain = $Port
$HostName = "probe-a.test:$Port"
$Backup = "pipeprobe-before"
$Service = "WeAmpPageSpeedOptimizer"
$Volume = "C:\ProgramData\We-Amp\PageSpeed\optimizer\cache"
$Trace = "$Root\state\trace.log"
$FrebDir = "$Root\freb"
$Copies = @("PipeProbeA", "PipeProbeB", "PipeProbeF")
# G is a second registration of A's image, H of B's.
$Registrations = [ordered]@{ PipeProbeA = "PipeProbeA"; PipeProbeB = "PipeProbeB"; PipeProbeF = "PipeProbeF"; PipeProbeG = "PipeProbeA"; PipeProbeH = "PipeProbeB" }
$NotificationNames = @{ "1" = "BEGIN_REQUEST"; "2" = "AUTHENTICATE_REQUEST"; "4" = "AUTHORIZE_REQUEST"; "8" = "RESOLVE_REQUEST_CACHE"; "16" = "MAP_REQUEST_HANDLER"; "32" = "ACQUIRE_REQUEST_STATE"; "64" = "PRE_EXECUTE_REQUEST_HANDLER"; "128" = "EXECUTE_REQUEST_HANDLER"; "256" = "RELEASE_REQUEST_STATE"; "512" = "UPDATE_REQUEST_CACHE"; "1024" = "LOG_REQUEST"; "2048" = "END_REQUEST"; "536870912" = "SEND_RESPONSE" }
# What a hit replays of the stored header table: the sidecar allowlist,
# Cache-Control and Expires verbatim, and Content-Type (the entry's).
$ReplayNames = @("content-security-policy", "access-control-allow-origin", "x-content-type-options", "referrer-policy", "cross-origin-opener-policy", "cross-origin-embedder-policy", "cross-origin-resource-policy", "permissions-policy", "vary", "cache-control", "expires", "content-type")

function Show([string]$Case, [string]$What, $Value) { Write-Output ("CASE {0} | {1} | {2}" -f $Case, $What, $Value) }

function Invoke-Raw([int]$P, [string]$Target, [string]$Method = "GET", [hashtable]$Headers = @{}, [string]$HostHdr = $HostName) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = 30000; $client.SendTimeout = 30000
    $client.Connect("127.0.0.1", $P)
    try {
        $stream = $client.GetStream()
        $req = "$Method $Target HTTP/1.1`r`nHost: $HostHdr`r`nConnection: close`r`n"
        foreach ($k in $Headers.Keys) { $req += "${k}: $($Headers[$k])`r`n" }
        if ($Method -in @("POST", "PUT", "PATCH") -and -not $Headers.ContainsKey("Content-Length")) { $req += "Content-Length: 0`r`n" }
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
    $r = [pscustomobject]@{ Status = $status; Lines = @($lines | Select-Object -Skip 1); Body = $body; Text = "" }
    $r.Text = Body-Text $r
    $r
}

function Header-Values($R, [string]$Name) {
    @($R.Lines | Where-Object { $_ -match ('^' + [regex]::Escape($Name) + ':\s?(.*)$') } | ForEach-Object { ($_ -split ':\s?', 2)[1] })
}

# The body as text, gunzipped when the response says gzip.
function Body-Text($R) {
    $bytes = $R.Body
    $ce = @(Header-Values $R "Content-Encoding")
    if ($ce.Count -gt 0 -and $ce[0] -match 'gzip' -and $bytes.Length -gt 0) {
        try {
            $in = New-Object IO.MemoryStream (, $bytes)
            $gz = New-Object IO.Compression.GZipStream ($in, [IO.Compression.CompressionMode]::Decompress)
            $out = New-Object IO.MemoryStream
            $gz.CopyTo($out)
            $bytes = $out.ToArray()
        } catch { return "(gzip unreadable)" }
    }
    [Text.Encoding]::UTF8.GetString($bytes)
}

function Sha([string]$Text) {
    $h = [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))
    (($h | Select-Object -First 8 | ForEach-Object { "{0:x2}" -f $_ }) -join "")
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

function Trace-Count { if (Test-Path $Trace) { @(Get-Content -LiteralPath $Trace).Count } else { 0 } }
function Trace-Since([int]$N) { if (Test-Path $Trace) { @(Get-Content -LiteralPath $Trace) | Select-Object -Skip $N } }

function Get-Stats {
    $r = Invoke-Raw $PMain "/pagespeed_admin/statistics"
    $s = @{}
    foreach ($l in ($r.Text -split "`n")) {
        if ($l -match '^\s*(daemon_[a-z0-9_]+):?\s+(-?\d+)\s*$') { $s[$Matches[1]] = [int64]$Matches[2] }
    }
    $s
}

function Show-StatsDelta([string]$Case, $Before, $After) {
    $moved = @()
    foreach ($k in ($After.Keys | Sort-Object)) {
        $d = $After[$k] - [int64]$Before[$k]
        if ($d -ne 0) { $moved += "$k+$d" }
    }
    Show $Case "stats.moved" $(if ($moved.Count) { $moved -join " " } else { "none" })
}

# Facts of a response body that say which stages ran on it.
function Body-Facts($R) {
    $t = $R.Text
    "len={0} sha={1} comment={2} space_runs={3} links={4} ob_marks={5} hit_marker={6}" -f $t.Length, (Sha $t), ($t -match '<!-- a comment -->'), ($t -match '   '), ([regex]::Matches($t, '<a ')).Count, ([regex]::Matches($t, '#ob')).Count, ($t -match 'probe hit')
}

$script:Seq = 0
function Probe-Request([string]$Case, [string]$Target, [string]$Method = "GET", [hashtable]$Headers = @{}, [switch]$Quiet) {
    $script:Seq++
    $id = "{0}-{1}" -f $Case, $script:Seq
    $h = @{} + $Headers
    $h["X-Probe-Id"] = $id
    $r = Invoke-Raw $PMain $Target $Method $h
    Start-Sleep -Milliseconds 400
    if ($Quiet) {
        Show $Case "wire.brief" ("{0} {1} {2}" -f $r.Status, (Body-Facts $r), (($r.Lines | Where-Object { $_ -match '^(Content-Type|Content-Encoding):' }) -join "; "))
    } else {
        Show $Case "request" ("{0} {1} headers={2}" -f $Method, $Target, (($Headers.Keys | Sort-Object | ForEach-Object { "$_=" + $Headers[$_] }) -join "; "))
        Show $Case "wire.status" $r.Status
        foreach ($l in $r.Lines) { Show $Case "wire.header" $l }
        Show $Case "wire.body" (Body-Facts $r)
        foreach ($l in (Read-ProbeLog $id)) { Show $Case "probe" ($l -replace '^id=[^|]*\|t=\d+\|', '') }
    }
    $r
}

# The multiset of header lines, compared by name: Date and Server by
# presence only, everything else by value. INFO only.
function Compare-Wire([string]$Case, $Miss, $Hit) {
    $names = @(@($Miss.Lines) + @($Hit.Lines) | Where-Object { $_ -match '^[^:]+:' } | ForEach-Object { ($_ -split ':', 2)[0].ToLower() } | Sort-Object -Unique)
    foreach ($n in $names) {
        $a = @(Header-Values $Miss $n); $b = @(Header-Values $Hit $n)
        if ($n -in @("date", "server")) {
            $v = if ($a.Count -eq $b.Count) { "same count" } else { "count differs" }
        } elseif (($a -join "`n") -ceq ($b -join "`n")) {
            $v = "same"
        } elseif ($a.Count -eq 0) {
            $v = "hit only"
        } elseif ($b.Count -eq 0) {
            $v = "miss only"
        } else {
            $v = "differs"
        }
        Show $Case "compare.header" ("{0} | {1} | miss=[{2}] hit=[{3}]" -f $n, $v, ($a -join " || "), ($b -join " || "))
    }
    Show $Case "compare.status" ("miss={0} hit={1}" -f $Miss.Status, $Hit.Status)
    Show $Case "compare.body" $(if ($Miss.Text -ceq $Hit.Text) { "equal" } else { "differs: miss " + (Body-Facts $Miss) + " / hit " + (Body-Facts $Hit) })
}

# What a hit replays, from what copy B recorded on the miss: the replay
# names in the stored order, then Age. -Alter marks the body, so a later
# response shows where its bytes came from.
function Make-Replay([string]$From, [string]$To, [int]$Age = 7, [switch]$Alter) {
    $hdrs = Get-Content -LiteralPath "$Root\stored\$From.hdrs"
    $keep = @($hdrs | Where-Object { $_ -match '^([^:]+):' -and $ReplayNames -contains $Matches[1].ToLower() })
    $keep += "Age: $Age"
    [IO.File]::WriteAllText("$Root\stored\$To.replay", (($keep -join "`r`n") + "`r`n"))
    $body = [IO.File]::ReadAllBytes("$Root\stored\$From.body")
    if ($Alter) {
        $t = [Text.Encoding]::UTF8.GetString($body).Replace("<title>probe</title>", "<title>probe hit</title>")
        $body = [Text.Encoding]::UTF8.GetBytes($t)
    }
    [IO.File]::WriteAllBytes("$Root\stored\$To.body", $body)
    Show "replay" $To ("from {0}: {1} bytes; lines {2}" -f $From, $body.Length, ($keep -join " ;; "))
}

function Freb-Order([string]$Case, [string]$Url) {
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
                if ($n -in @("RESOLVE_REQUEST_CACHE", "SEND_RESPONSE")) { $seen += $current }
            } elseif ($name -like "*COMPRESSION*" -or $name -eq "GENERAL_SET_RESPONSE_HEADER" -or $name -like "URL_REWRITE*" -or $name -like "*OUTBOUND*" -or $name -like "*REWRITE*") {
                $vals = ($data.Keys | Sort-Object | ForEach-Object { "$_=" + $data[$_] }) -join " "
                if ($vals.Length -gt 240) { $vals = $vals.Substring(0, 240) }
                Show $Case "freb.event" ("in {0}: {1} {2}" -f $current, $name, $vals)
            }
        }
        Show $Case "freb.order" ($seen -join " > ")
        return
    }
    Show $Case "freb.order" "no trace found"
}

function Kernel-Hits([string]$Path) {
    $t = (& netsh http show cachestate "url=http://probe-a.test:$PMain$Path") -join " "
    if ($t -match 'Hit count:\s*(\d+)') { "cached, hit count " + $Matches[1] } else { "not in the kernel cache" }
}

# How many requests of a case reached user mode (probe A's begin).
function Reached([string]$CasePrefix) {
    $n = 0
    foreach ($f in (Get-ChildItem "$Root\log" -Filter "PipeProbeA-*.log" -ErrorAction SilentlyContinue)) {
        foreach ($l in (Get-Content -LiteralPath $f.FullName)) {
            if ($l -like "id=$CasePrefix-*" -and $l -like "*|ev=begin|*") { $n++ }
        }
    }
    $n
}

function Set-Caching([bool]$On) {
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/caching" -Name "enabled" -Value $On
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/caching" -Name "enableKernelCache" -Value $On
}

function Wait-Site {
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        try { if ((Invoke-Raw $PMain "/plain.txt").Status -eq 200) { return } } catch { }
        Start-Sleep -Milliseconds 500
    }
}

function Recycle {
    Restart-WebAppPool -Name $Main
    Start-Sleep -Seconds 2
    Wait-Site
}

function Rewrite-Installed { Test-Path "$env:windir\system32\inetsrv\rewrite.dll" }

$PageHtml = "<html><head><title>probe</title></head><body>`r`n<!-- a comment -->`r`n<p>   A   paragraph   with   runs   of   spaces.   </p>`r`n" +
    "<p><a href=`"/one.html`">one</a> <a href=`"/two.html`">two</a> <a href=`"/three.html`">three</a></p>`r`n" +
    ("<p>   Filler   text   for   the   rewriter   to   collapse.   </p>`r`n" * 30) + "</body></html>`r`n"

function Setup {
    if (Test-Path "$env:windir\system32\inetsrv\backup\$Backup") {
        throw "the IIS backup $Backup exists from an earlier run; restore and delete it first"
    }
    if (-not (Get-Service -Name $Service -ErrorAction SilentlyContinue)) { throw "the optimizer service is not installed (an MSI built with its optimizer is needed)" }
    if (-not (Rewrite-Installed) -and (Test-Path "$Root\bin\url-rewrite.msi")) {
        $p = Start-Process msiexec.exe -ArgumentList "/i `"$Root\bin\url-rewrite.msi`" /qn /norestart" -Wait -PassThru
        Write-Output ("SETUP | url rewrite install | exit {0}" -f $p.ExitCode)
        if ($p.ExitCode -eq 0) { Set-Content -LiteralPath "$Root\rewrite-installed-by-probe.txt" -Value "1" -Encoding ASCII }
    }
    $appcmd = "$env:windir\system32\inetsrv\appcmd.exe"
    & $appcmd add backup $Backup | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "taking the IIS configuration backup failed" }
    New-Item -ItemType Directory -Force "$Root\log", "$Root\bin", "$Root\stored", "$Root\state", "$Root\cache", "$Root\logs", $FrebDir | Out-Null
    foreach ($d in @("$Root\log", "$Root\stored", "$Root\state", "$Root\cache", "$Root\logs", $FrebDir)) { & icacls.exe $d /grant "IIS_IUSRS:(OI)(CI)M" | Out-Null }
    foreach ($c in $Copies) { Copy-Item "$Root\bin\iis_pipeline_probe.dll" "$Root\bin\$c.dll" -Force }

    # The optimizer in markdown-static mode with agent-optimize on; its
    # command line and state are put back at teardown.
    $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$Service"
    $image = (Get-ItemProperty $key).ImagePath
    Set-Content -LiteralPath "$Root\service-image.txt" -Value $image -Encoding ASCII
    Set-Content -LiteralPath "$Root\service-state.txt" -Value ((Get-Service $Service).Status.ToString() + "," + (Get-CimInstance Win32_Service -Filter "Name='$Service'").StartMode) -Encoding ASCII
    if ($image -notmatch '--html-mode') { Set-ItemProperty $key -Name ImagePath -Value "$image --html-mode markdown-static --agent-optimize" }
    if ((Get-Service $Service).Status -eq "Running") { Restart-Service -Name $Service } else { & sc.exe config $Service start= demand | Out-Null; Start-Service -Name $Service }
    (Get-Service -Name $Service).WaitForStatus("Running", [TimeSpan]::FromSeconds(30))

    if (Test-Path "IIS:\Sites\$Main") { Remove-Website -Name $Main }
    if (Test-Path "IIS:\AppPools\$Main") { Remove-WebAppPool -Name $Main }
    New-WebAppPool -Name $Main | Out-Null
    Set-ItemProperty "IIS:\AppPools\$Main" -Name managedRuntimeVersion -Value "v4.0"
    & $appcmd set config -section:system.applicationHost/applicationPools "/+[name='$Main'].environmentVariables.[name='PAGESPEED_TEST_HTML_TRACE_FILE',value='$Trace']" /commit:apphost | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "setting the pool's environment failed" }

    $dir = "$Root\www\$Main"
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    foreach ($s in @("sub", "rw", "rw2", "kern", "kern2", "oc", "oc2", "p")) { New-Item -ItemType Directory -Force "$dir\$s" | Out-Null }
    foreach ($f in @("page.html", "sub\page.html", "rw\page.html", "rw2\page.html", "kern\page.html", "kern2\page.html", "oc\page.html", "oc2\page.html", "p\q.html")) {
        [IO.File]::WriteAllText("$dir\$f", $PageHtml)
    }
    [IO.File]::WriteAllText("$dir\plain.txt", ("plain text line for compression`r`n" * 150))
    [IO.File]::WriteAllText("$dir\unsafe.ashx", (@(
        '<%@ WebHandler Language="C#" Class="ProbeUnsafe" %>',
        'using System.Web;',
        'public class ProbeUnsafe : IHttpHandler {',
        '  public bool IsReusable { get { return false; } }',
        '  public void ProcessRequest(HttpContext c) {',
        '    int status = 200; int.TryParse(c.Request.QueryString["status"], out status);',
        '    c.Response.StatusCode = status == 0 ? 200 : status;',
        '    c.Response.TrySkipIisCustomErrors = true;',
        '    string loc = c.Request.QueryString["loc"]; if (!string.IsNullOrEmpty(loc)) c.Response.AppendHeader("Location", loc);',
        '    string cl = c.Request.QueryString["cloc"]; if (!string.IsNullOrEmpty(cl)) c.Response.AppendHeader("Content-Location", cl);',
        '    c.Response.ContentType = "text/html";',
        '    if (status != 204 && status != 304) c.Response.Write("<html><body>unsafe " + c.Request.HttpMethod + "</body></html>");',
        '  }',
        '}') -join "`r`n"))
    [IO.File]::WriteAllText("$dir\pagespeed.config", (@(
        "pagespeed on",
        "pagespeed RewriteLevel PassThrough",
        "pagespeed EnableFilters collapse_whitespace,remove_comments",
        "pagespeed CacheFragment probe",
        "pagespeed LogDir `"$Root\logs`"",
        "pagespeed FileCachePath `"$Root\cache`"",
        "pagespeed Statistics on",
        "pagespeed DaemonSocketPath pagespeed-optimizer",
        "pagespeed DaemonVolumePath `"$Volume`"",
        "pagespeed DaemonHtmlCache serve-variants",
        "pagespeed AgentOptimize on",
        "pagespeed InPlaceResourceOptimization off"
    ) -join "`r`n") + "`r`n")
    # Output-cache profiles for .html: oc and oc2 (the kernel cache too).
    foreach ($s in @("oc", "oc2")) {
        [IO.File]::WriteAllText("$dir\$s\web.config", '<?xml version="1.0" encoding="utf-8"?><configuration><system.webServer><caching enabled="true" enableKernelCache="true"><profiles><add extension=".html" policy="CacheUntilChange" kernelCachePolicy="CacheUntilChange" location="Any" /></profiles></caching></system.webServer></configuration>')
    }
    # A non-idempotent outbound rule pair: a marker appended to every <a
    # href>, and one appended to a response header (absent: the rule writes
    # it). Applied twice, a link carries #ob#ob.
    if (Rewrite-Installed) {
        $rules = '<?xml version="1.0" encoding="utf-8"?><configuration><system.webServer><rewrite><outboundRules>' +
            '<rule name="probe-mark-links" preCondition="probe-html"><match filterByTags="A" pattern="^(.*)$" /><action type="Rewrite" value="{R:1}#ob" /></rule>' +
            '<rule name="probe-mark-header"><match serverVariable="RESPONSE_X_Probe_Outbound" pattern="^(.*)$" /><action type="Rewrite" value="{R:1}+ob" /></rule>' +
            '<preConditions><preCondition name="probe-html"><add input="{RESPONSE_CONTENT_TYPE}" pattern="^text/html" /></preCondition></preConditions>' +
            '</outboundRules></rewrite></system.webServer></configuration>'
        foreach ($s in @("rw", "rw2")) { [IO.File]::WriteAllText("$dir\$s\web.config", $rules) }
    }
    New-Website -Name $Main -Port $PMain -HostHeader "probe-a.test" -PhysicalPath $dir -ApplicationPool $Main | Out-Null
    # Operator headers: written by ProtocolSupportModule after the
    # PageSpeed module's send. X-Content-Type-Options is on the sidecar
    # allowlist.
    foreach ($pair in @(@("X-Probe-Custom", "site-custom"), @("X-Content-Type-Options", "nosniff"))) {
        Add-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/httpProtocol/customHeaders" -Name "." -Value @{ name = $pair[0]; value = $pair[1] }
    }
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/staticContent/clientCache" -Name "cacheControlMode" -Value "UseMaxAge"
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/staticContent/clientCache" -Name "cacheControlMaxAge" -Value "00:05:00"
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/urlCompression" -Name "doStaticCompression" -Value $true
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/urlCompression" -Name "doDynamicCompression" -Value $true
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/serverRuntime" -Name "frequentHitThreshold" -Value 1
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "minFileSizeForComp" -Value 256
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "dynamicCompressionDisableCpuUsage" -Value 100
    Set-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/httpCompression" -Name "staticCompressionDisableCpuUsage" -Value 100
    # Every verb reaches the .ashx handler (the unsafe-method cases).
    Add-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/handlers" -Name "." -AtIndex 0 -Value @{ name = "probe-ashx-all-verbs"; path = "*.ashx"; verb = "*"; type = "System.Web.UI.SimpleHandlerFactory"; preCondition = "integratedMode" }
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
    if ($mods -contains "WebDAVModule") { Remove-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/modules" -Name "." -AtElement @{ name = "WebDAVModule" } }
    Set-ItemProperty "IIS:\Sites\$Main" -Name traceFailedRequestsLogging -Value @{ enabled = $true; directory = $FrebDir; maxLogFiles = 500 }
    Add-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "system.webServer/tracing/traceFailedRequests" -Name "." -Value @{ path = "*" }
    $rule = "system.webServer/tracing/traceFailedRequests/add[@path='*']"
    Set-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "$rule/failureDefinitions" -Name "statusCodes" -Value "100-999"
    Add-WebConfigurationProperty -PSPath $Apphost -Location $Main -Filter "$rule/traceAreas" -Name "." -Value @{ provider = "WWW Server"; areas = "Authentication,Security,Filter,StaticFile,CGI,Compression,Cache,RequestNotifications,Module" + $(if (Rewrite-Installed) { ",Rewrite" } else { "" }); verbosity = "Verbose" }
    foreach ($c in $Registrations.Keys) {
        if (-not (Get-WebGlobalModule -Name $c)) { New-WebGlobalModule -Name $c -Image ("$Root\bin\{0}.dll" -f $Registrations[$c]) | Out-Null }
    }
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
    $psIndex = [Array]::IndexOf($mods, "PageSpeedModule")
    if ($psIndex -lt 0) { throw "PageSpeedModule is not in system.webServer/modules" }
    Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value @{ name = "PipeProbeA" } -AtIndex $psIndex
    Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value @{ name = "PipeProbeB" } -AtIndex ($psIndex + 2)
    foreach ($c in @("PipeProbeF", "PipeProbeG", "PipeProbeH")) { Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value @{ name = $c } }
    $ids = @((Get-Website -Name $Main).id)
    Set-Content -LiteralPath "$Root\site-ids.txt" -Value $ids -Encoding ASCII
    foreach ($id in $ids) { Remove-Item -Recurse -Force ("C:\ProgramData\We-Amp\PageSpeed\cache\{0}.ROOT" -f $id) -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
    Wait-Site
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
    Write-Output ("SETUP | modules | {0}" -f ($mods -join ","))
    $iis = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\InetStp").VersionString
    $rw = if (Rewrite-Installed) { (Get-Item "$env:windir\system32\inetsrv\rewrite.dll").VersionInfo.FileVersion } else { "absent" }
    Write-Output ("SETUP | environment | os={0} build={1} iis={2} url_rewrite={3} module={4}" -f (Get-CimInstance Win32_OperatingSystem).Caption, [Environment]::OSVersion.Version, $iis, $rw, (Get-Item "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll").VersionInfo.FileVersion)
}

function Teardown {
    $appcmd = "$env:windir\system32\inetsrv\appcmd.exe"
    & $appcmd restore backup $Backup | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "restoring the IIS configuration backup failed" }
    & $appcmd delete backup $Backup | Out-Null
    if (Test-Path "$Root\service-image.txt") {
        $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$Service"
        Set-ItemProperty $key -Name ImagePath -Value ((Get-Content -LiteralPath "$Root\service-image.txt" -Raw).Trim())
        $state = ((Get-Content -LiteralPath "$Root\service-state.txt" -Raw).Trim()) -split ","
        if ($state[0] -ne "Running") { Stop-Service -Name $Service -ErrorAction SilentlyContinue } else { Restart-Service -Name $Service -ErrorAction SilentlyContinue }
        $mode = @{ "Auto" = "auto"; "Manual" = "demand"; "Disabled" = "disabled" }[$state[1]]
        if ($mode) { & sc.exe config $Service start= $mode | Out-Null }
    }
    Start-Sleep -Seconds 2
    if (Test-Path "$Root\site-ids.txt") {
        foreach ($id in (Get-Content -LiteralPath "$Root\site-ids.txt")) {
            if ($id -match '^\d+$') { Remove-Item -Recurse -Force ("C:\ProgramData\We-Amp\PageSpeed\cache\{0}.ROOT" -f $id) -ErrorAction SilentlyContinue }
        }
    }
    $rwNote = if (Test-Path "$Root\rewrite-installed-by-probe.txt") { "; the URL Rewrite module this run installed stays installed" } else { "" }
    Remove-Item -Recurse -Force "$Root\www", "$Root\log", "$Root\stored", "$Root\state", "$Root\cache", "$Root\logs", $FrebDir -ErrorAction SilentlyContinue
    Write-Output ("TEARDOWN | done | configuration restored; probe sites left: {0}{1}" -f @(Get-Website | Where-Object { $_.name -like "probe-*" }).Count, $rwNote)
}

# Moves RewriteModule to just before the first probe copy (before the
# PageSpeed module) or to the end of the list (after it), then recycles.
function Move-RewriteModule([string]$Where) {
    $entry = Get-WebConfiguration "system.webServer/modules/add[@name='RewriteModule']" -PSPath $Apphost
    $pre = $entry.preCondition
    Remove-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -AtElement @{ name = "RewriteModule" }
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
    $value = @{ name = "RewriteModule" }
    if ($pre) { $value["preCondition"] = $pre }
    if ($Where -eq "before") {
        Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value $value -AtIndex ([Array]::IndexOf($mods, "PipeProbeA"))
    } else {
        Add-WebConfigurationProperty -PSPath $Apphost -Filter "system.webServer/modules" -Name "." -Value $value
    }
    Recycle
}

function Module-Order {
    $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
    $rwi = [Array]::IndexOf($mods, "RewriteModule"); $psi = [Array]::IndexOf($mods, "PageSpeedModule")
    "RewriteModule at {0}, PageSpeedModule at {1}, PageSpeedFinalView at {2} of {3}" -f $rwi, $psi, [Array]::IndexOf($mods, "PageSpeedFinalView"), $mods.Count
}

# A miss recorded by B, then the hit B answers from that record.
function Miss-Then-Hit([string]$Case, [string]$Target, [hashtable]$Extra = @{}, [string]$Stored = "", [switch]$Freb) {
    if (-not $Stored) { $Stored = $Case }
    $miss = Probe-Request "${Case}_miss" $Target "GET" ($Extra + @{ "X-Probe-Act" = "B.capture"; "X-Probe-Stored" = $Stored })
    if ($Freb) { Freb-Order "${Case}_miss" $Target }
    Make-Replay $Stored $Stored
    $t0 = Trace-Count; $s0 = Get-Stats
    $hit = Probe-Request "${Case}_hit" $Target "GET" ($Extra + @{ "X-Probe-Act" = "B.serve_html"; "X-Probe-Stored" = $Stored })
    Start-Sleep -Milliseconds 600
    $s1 = Get-Stats
    if ($Freb) { Freb-Order "${Case}_hit" $Target }
    foreach ($l in (Trace-Since $t0)) { Show "${Case}_hit" "trace" ($l -replace '^\d+ \d+ ', '') }
    Show-StatsDelta "${Case}_hit" $s0 $s1
    Compare-Wire $Case $miss $hit
    @($miss, $hit)
}

function Run {
    Set-Caching $false
    Clear-ProbeLogs
    # Warm the worker and the engine.
    for ($i = 0; $i -lt 3; $i++) { $null = Invoke-Raw $PMain "/sub/page.html" }
    Show "env" "module order" (Module-Order)

    # 1 + 2: does the PageSpeed module's send rewrite a page answered at
    # RESOLVE_REQUEST_CACHE, and is that answer captured?
    $null = Miss-Then-Hit "core" "/page.html" @{} "page" -Freb
    # The same with a changed body: a capture of the hit would store it.
    Make-Replay "page" "page_hit_marked" -Alter
    $s0 = Get-Stats; $t0 = Trace-Count
    $null = Probe-Request "core_marked_hit" "/page.html" "GET" @{ "X-Probe-Act" = "B.serve_html"; "X-Probe-Stored" = "page_hit_marked" }
    Start-Sleep -Milliseconds 600
    foreach ($l in (Trace-Since $t0)) { Show "core_marked_hit" "trace" ($l -replace '^\d+ \d+ ', '') }
    Show-StatsDelta "core_marked_hit" $s0 (Get-Stats)
    # Module context set at RESOLVE_REQUEST_CACHE under B's id: seen at B's
    # first send and at H's (B's image, send first) after the rewriter
    # resumed the request.
    $null = Probe-Request "ctx_hit" "/page.html" "GET" @{ "X-Probe-Act" = "B.serve_html,B.ctx"; "X-Probe-Stored" = "page" }

    # 3: compression on a hit as on a miss.
    $null = Miss-Then-Hit "gzip" "/sub/page.html" @{ "Accept-Encoding" = "gzip" } "sub_gz"
    # The hit replays bytes the capture holds inflated (the module inflates
    # a statically compressed page before it stores it).
    $null = Miss-Then-Hit "gzip_plain_store" "/sub/page.html" @{} "sub_plain"
    $hitgz = Probe-Request "gzip_plain_store_hit_ae" "/sub/page.html" "GET" @{ "Accept-Encoding" = "gzip"; "X-Probe-Act" = "B.serve_html"; "X-Probe-Stored" = "sub_plain" }

    # 3: URL Rewrite outbound rules, in both module orders.
    if (Rewrite-Installed) {
        Show "rw" "module order (installed)" (Module-Order)
        $null = Miss-Then-Hit "rw_installed" "/rw/page.html" @{} "rw_installed" -Freb
        $null = Miss-Then-Hit "rw_installed_gz" "/rw/page.html" @{ "Accept-Encoding" = "gzip" } "rw_installed_gz"
        $mods = @(Get-WebConfiguration "system.webServer/modules/add" -PSPath $Apphost | ForEach-Object { $_.name })
        $other = if ([Array]::IndexOf($mods, "RewriteModule") -gt [Array]::IndexOf($mods, "PageSpeedModule")) { "before" } else { "after" }
        Move-RewriteModule $other
        Show "rw" "module order (moved $other)" (Module-Order)
        $null = Miss-Then-Hit "rw_moved" "/rw2/page.html" @{} "rw_moved" -Freb
        $null = Miss-Then-Hit "rw_moved_gz" "/rw2/page.html" @{ "Accept-Encoding" = "gzip" } "rw_moved_gz"
        # What the module stored, read back through its Markdown copy:
        # whether the links carry the outbound marker.
        foreach ($t in @("/rw/page.html", "/rw2/page.html")) {
            $md = $null
            for ($i = 0; $i -lt 40; $i++) {
                $md = Invoke-Raw $PMain $t "GET" @{ "Accept" = "text/markdown" }
                if ((Header-Values $md "Content-Type") -match 'markdown') { break }
                Start-Sleep -Milliseconds 750
            }
            Show "rw_markdown" $t ("status={0} type={1} links={2}" -f $md.Status, ((Header-Values $md "Content-Type") -join ","), ((([regex]::Matches($md.Text, '\]\(([^)]*)\)')) | ForEach-Object { $_.Groups[1].Value }) -join " "))
        }
    } else {
        Show "rw" "url rewrite" "absent: outbound cases skipped"
    }

    # 4: method, final status and Location at the top of the PageSpeed
    # module's send, for unsafe methods (B's send runs right before it).
    $unsafe = @(
        @("POST", "/unsafe.ashx?status=200"), @("PUT", "/unsafe.ashx?status=204"), @("DELETE", "/unsafe.ashx?status=200"),
        @("PATCH", "/unsafe.ashx?status=200"), @("POST", "/unsafe.ashx?status=303&loc=/page.html&cloc=/item"),
        @("POST", "/unsafe.ashx?status=500"), @("POST", "/unsafe.ashx?status=302&loc=http://other.test/x"),
        @("post", "/unsafe.ashx?status=200"), @("POST", "/page.html"), @("OPTIONS", "/unsafe.ashx?status=200"))
    $t0 = Trace-Count
    $k = 0
    foreach ($u in $unsafe) {
        $k++
        $r = Invoke-Raw $PMain $u[1] $u[0] @{ "X-Probe-Id" = "unsafe$k-0" }
        Start-Sleep -Milliseconds 400
        Show "unsafe$k" "request" ("{0} {1} -> {2} {3}" -f $u[0], $u[1], $r.Status, (($r.Lines | Where-Object { $_ -match '^(Location|Content-Location):' }) -join "; "))
        foreach ($l in (Read-ProbeLog "unsafe$k-0")) {
            if ($l -match '\|mod=PipeProbeB\|ev=send\|' -or $l -match '\|ev=resolve_cache\|') { Show "unsafe$k" "probe" ($l -replace '^id=[^|]*\|t=\d+\|', '') }
        }
    }
    foreach ($l in (Trace-Since $t0)) { if ($l -match 'unsafe|page\.html') { Show "unsafe" "module trace" ($l -replace '^\d+ \d+ ', '') } }

    # 5: IIS's response caches and a page answered at RESOLVE_REQUEST_CACHE.
    Set-Caching $true
    Recycle
    Clear-ProbeLogs
    $null = Miss-Then-Hit "kseed" "/kern/page.html" @{} "kseed"
    Make-Replay "kseed" "kmark" -Alter
    foreach ($c in @(@("kern", "/kern/page.html", "B.serve_html,B.nodisarm"), @("kern2", "/kern2/page.html", "B.serve_html"), @("oc", "/oc/page.html", "B.serve_html,B.nodisarm"), @("oc2", "/oc2/page.html", "B.serve_html"))) {
        for ($i = 0; $i -lt 4; $i++) { $null = Probe-Request "$($c[0])_hit" $c[1] "GET" @{ "X-Probe-Act" = $c[2]; "X-Probe-Stored" = "kmark" } -Quiet }
        $marked = 0
        for ($i = 0; $i -lt 4; $i++) {
            $r = Probe-Request "$($c[0])_plain" $c[1] "GET" @{} -Quiet
            if ($r.Text -match 'probe hit') { $marked++ }
        }
        Show $c[0] "after 4 hits ($($c[2])) and 4 plain GETs" ("hits reached the pipeline {0}/4; plain GETs reached it {1}/4; plain GETs carrying the hit's bytes {2}/4; kernel cache: {3}" -f (Reached "$($c[0])_hit"), (Reached "$($c[0])_plain"), $marked, (Kernel-Hits $c[1]))
    }
    for ($i = 0; $i -lt 4; $i++) { $null = Probe-Request "ktxt" "/plain.txt" "GET" @{} -Quiet }
    Show "ktxt" "control: a static file the module leaves alone" ("reached the pipeline {0}/4; kernel cache: {1}" -f (Reached "ktxt"), (Kernel-Hits "/plain.txt"))
    Set-Caching $false
    Recycle

    # 6: HEAD on the hit path.
    $hm = Probe-Request "head_miss" "/page.html" "HEAD" @{}
    $hh = Probe-Request "head_hit" "/page.html" "HEAD" @{ "X-Probe-Act" = "B.serve_html"; "X-Probe-Stored" = "page" }
    Compare-Wire "head" $hm $hh
    $gm = Probe-Request "get_vs_head" "/page.html" "GET" @{ "X-Probe-Act" = "B.serve_html"; "X-Probe-Stored" = "page" } -Quiet
    Compare-Wire "head_vs_get_hit" $gm $hh

    # 6: path forms. Each is a first request for its spelling of p/q.html:
    # what IIS and the module's global hook hold at RESOLVE_REQUEST_CACHE,
    # and what the HTML cache did with the response (stored, refused).
    Clear-ProbeLogs
    $forms = @("/p/q.html", "/P/Q.HTML", "/p/Q.html", "/x/../p/q.html", "/x/%2E%2E/p/q.html", "/p%2Fq.html", "/p%5Cq.html", "//p/q.html", "/p//q.html", "/p/q%2Ehtml", "/p/./q.html", "/%70/q.html", "/p/q.html;x=1", "/p/q.html/x", "/p/q.html.", "/p/q.html%20", "/p/q.html::`$DATA", "/p/Q~1.HTM", "/p/q.html?a=1", "/p/q.html?A=1")
    $k = 0
    foreach ($f in $forms) {
        $k++
        $s0 = Get-Stats
        $r = Invoke-Raw $PMain $f "GET" @{ "X-Probe-Id" = "form$k-0" }
        Start-Sleep -Milliseconds 700
        $s1 = Get-Stats
        Show "form$k" "request" ("GET {0} -> {1}" -f $f, $r.Status)
        foreach ($l in (Read-ProbeLog "form$k-0")) { if ($l -match '\|mod=PipeProbeA\|ev=(begin|resolve_cache)\|') { Show "form$k" "probe" ($l -replace '^id=[^|]*\|t=\d+\|', '') } }
        Show-StatsDelta "form$k" $s0 $s1
    }
}

switch ($Phase) {
    "setup" { Setup }
    "run" { Run }
    "teardown" { Teardown }
}
