# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

<#
.SYNOPSIS
    Setup and manage Full IIS (not IIS Express) for PageSpeed integration tests.

.DESCRIPTION
    This script sets up Full IIS (Windows Server feature) with the PageSpeed native
    module for running integration tests. This is preferred over IIS Express because:
    - Runs as a Windows Service (proper background process)
    - Supports native module registration via GlobalModules
    - More robust process management
    - Works properly over SSH sessions

.PARAMETER Action
    The action to perform: start, stop, status, restart, install, uninstall

.PARAMETER Port
    The port for IIS to listen on (default: 8080)

.PARAMETER SourceRoot
    Path to the mod_pagespeed source root (default: auto-detect)

.PARAMETER NoModule
    Skip installing the PageSpeed module (for baseline testing)

.EXAMPLE
    .\setup_iis_full.ps1 install          # First-time setup
    .\setup_iis_full.ps1 start            # Start IIS (usually already running)
    .\setup_iis_full.ps1 stop             # Stop IIS
    .\setup_iis_full.ps1 status           # Check status
    .\setup_iis_full.ps1 install -NoModule  # Install without PageSpeed module
#>

param(
    [Parameter(Position=0)]
    [ValidateSet('start', 'stop', 'status', 'restart', 'install', 'uninstall')]
    [string]$Action = 'status',

    [int]$Port = 8080,

    [string]$SourceRoot = "",

    [switch]$NoModule
)

$ErrorActionPreference = "Stop"

# Configuration
$SiteName = "PageSpeedTestSite"
$AppPoolName = "PageSpeedTestPool"
$WebRoot = "C:\inetpub\pagespeed_test"
$CacheDir = "C:\pagespeed_cache"
$LogDir = "C:\pagespeed_logs"
# HTTPS test binding. Full IIS only -- run_iis_tests.ps1 exports the
# matching PAGESPEED_HTTPS_PORT for the pytest HTTPS suite.
$HttpsPort = 8443

# Auto-detect source root if not provided
if (-not $SourceRoot) {
    # Try multiple locations
    $scriptParent = $null
    try {
        $scriptItem = Get-Item $PSScriptRoot -ErrorAction SilentlyContinue
        if ($scriptItem -and $scriptItem.Parent -and $scriptItem.Parent.Parent) {
            $scriptParent = $scriptItem.Parent.Parent.FullName
        }
    } catch { }

    $possibleRoots = @(
        "C:\pagespeed",
        $scriptParent,
        "\\host.lan\Data"
    )
    foreach ($root in $possibleRoots) {
        if ($root -and (Test-Path "$root\install\mod_pagespeed_example")) {
            $SourceRoot = $root
            break
        }
    }
    if (-not $SourceRoot) {
        $SourceRoot = "C:\pagespeed"
    }
}

# Paths to test content
$ExampleSource = "$SourceRoot\install\mod_pagespeed_example"
$TestSource = "$SourceRoot\install\mod_pagespeed_test"
$DoNotModifySource = "$SourceRoot\install\do_not_modify"

# Path to built PageSpeed module
$ModulePath = "$SourceRoot\bazel-bin\pagespeed\iis\pagespeed_iis.dll"

function Write-Status {
    param([string]$Message, [string]$Color = "White")
    Write-Host "[IIS Setup] " -NoNewline -ForegroundColor Cyan
    Write-Host $Message -ForegroundColor $Color
}

function Test-IISInstalled {
    try {
        $feature = Get-WindowsFeature -Name Web-Server -ErrorAction Stop
        return $feature.Installed
    } catch {
        # Not on Server, check for IIS service
        $service = Get-Service -Name W3SVC -ErrorAction SilentlyContinue
        return $null -ne $service
    }
}

function Test-AppCmdExists {
    $appcmd = "$env:SystemRoot\System32\inetsrv\appcmd.exe"
    return Test-Path $appcmd
}

function Invoke-AppCmd {
    # Use $args (automatic variable) instead of a named parameter to capture
    # ALL positional arguments. PowerShell's param([string[]]$X) only binds
    # the first positional value; remaining args are silently dropped.
    $appcmd = "$env:SystemRoot\System32\inetsrv\appcmd.exe"
    if (-not (Test-Path $appcmd)) {
        throw "appcmd.exe not found. Is IIS installed?"
    }
    & $appcmd @args 2>&1
}

function Reset-PageSpeedTestCache {
    # Recycle the IIS app pool first, then purge the on-disk PageSpeed file
    # cache. Order matters: a w3wp from a prior workflow job may still be
    # running on a persistent Windows runner and holding file handles on cache entries. If we
    # purge before the recycle, Remove-Item partially fails on locked files
    # (silently, under -ErrorAction SilentlyContinue) and the w3wp may also
    # service one more request in the gap, rewriting cache entries with the
    # prior job's Last-Modified header before the recycle drops the
    # in-memory LRU. Recycle first -> brief sleep so w3wp exits -> then
    # purge.
    #
    # mod_pagespeed's rewrite cache is keyed by input URL + content hash --
    # NOT by file mtime -- so a cached rewrite from a previous job hits on
    # the same Puzzle.jpg bytes and serves back the prior job's embedded
    # Last-Modified header, while origin (?PageSpeed=off) honestly serves
    # the freshly-stamped fixture mtime. That divergence is the root cause
    # of.
    #
    # Cache paths purged:
    #   - $CacheDir                                  (test config: C:\pagespeed_cache)
    #   - C:\ProgramData\We-Amp\PageSpeed\cache      (shipped default in
    #     install/iis/pagespeed.config line 17 -- safety net in case a
    #     misconfigured run falls back to the production default)
    #   - C:\PageSpeed\cache                         (legacy IISpeed XML
    #     schema default, installer/pagespeed_schema.xml line 29)
    #
    # The whole block is best-effort but LOUD about partial failures:
    # purge errors get surfaced via Write-Status Yellow so a future
    # ACL/lock regression turns the CI log yellow instead of failing
    # silently. Idempotent and safe on cold first-job runners.

    # Step 1: recycle app pool (drops in-memory rewrite cache + releases
    # file handles so the subsequent Remove-Item can complete).
    try {
        Import-Module WebAdministration -ErrorAction SilentlyContinue
        if (Get-Command Restart-WebAppPool -ErrorAction SilentlyContinue) {
            Restart-WebAppPool -Name $AppPoolName -ErrorAction SilentlyContinue
            Write-Status "Recycled app pool: $AppPoolName" "Gray"
            # Brief drain so w3wp finishes any in-flight request and exits
            # before we touch the cache files it might still be writing.
            Start-Sleep -Seconds 2
        }
    } catch { }

    # Step 2: purge stale on-disk cache. Loud on partial failure.
    foreach ($staleCache in @($CacheDir,
                              "C:\ProgramData\We-Amp\PageSpeed\cache",
                              "C:\PageSpeed\cache")) {
        if (Test-Path $staleCache) {
            $purgeErr = $null
            Remove-Item "$staleCache\*" -Recurse -Force `
                -ErrorAction SilentlyContinue -ErrorVariable purgeErr
            if ($purgeErr -and $purgeErr.Count -gt 0) {
                Write-Status ("Warning: purge of {0} had {1} error(s); first: {2}" -f `
                    $staleCache, $purgeErr.Count, $purgeErr[0]) "Yellow"
            } else {
                Write-Status "Purged stale PageSpeed file cache at $staleCache" "Gray"
            }
            # Defensive post-check: if residual entries remain, surface it
            # so a future lock/ACL regression that bypasses the error stream
            # still shows up yellow in CI logs.
            $residual = Get-ChildItem -Path $staleCache -Recurse -Force `
                -ErrorAction SilentlyContinue | Measure-Object
            if ($residual.Count -gt 0) {
                Write-Status ("Warning: {0} still has {1} entries after purge" -f `
                    $staleCache, $residual.Count) "Yellow"
            }
        }
    }
}

function Initialize-TestEnvironment {
    Write-Status "Initializing test environment..."

    # Recycle app pool + purge stale PageSpeed file cache from prior runs.
    # See Reset-PageSpeedTestCache for the full rationale.
    Reset-PageSpeedTestCache

    # Create directories
    # Include C:\PageSpeed\cache as a safety net -- the legacy IISpeed XML
    # schema (installer/pagespeed_schema.xml line 29) defaults
    # fileCachePath to this path. The shipped install/iis/pagespeed.config
    # uses C:\ProgramData\We-Amp\PageSpeed\cache instead, but cold runners
    # may have either present.
    @($WebRoot, $CacheDir, $LogDir, "C:\PageSpeed", "C:\PageSpeed\cache") | ForEach-Object {
        if (-not (Test-Path $_)) {
            New-Item -ItemType Directory -Path $_ -Force | Out-Null
            Write-Status "Created directory: $_" "Gray"
        }
    }

    # Set permissions on cache and log directories
    # IIS app pools run as IIS AppPool\<pool-name> or as IUSR
    try {
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            "IIS_IUSRS", "FullControl", "ContainerInherit,ObjectInherit", "None", "Allow")

        foreach ($dir in @($CacheDir, $LogDir, "C:\PageSpeed\cache")) {
            $acl = Get-Acl $dir
            $acl.AddAccessRule($rule)
            Set-Acl $dir $acl
        }

        Write-Status "Set IIS_IUSRS permissions on cache/log directories" "Gray"
    } catch {
        Write-Status "Warning: Could not set permissions: $_" "Yellow"
    }

    # Remove stale web.config from previous runs. appcmd set config (called
    # later in New-IISSite) triggers IIS to parse the site's web.config.
    # New-WebConfig writes a fresh one after module installation.
    $staleConfig = "$WebRoot\web.config"
    if (Test-Path $staleConfig) {
        Remove-Item $staleConfig -Force
        Write-Status "Removed stale web.config" "Gray"
    }

    # Also remove stale pagespeed.config from previous runs
    $stalePagespeedConfig = "$WebRoot\pagespeed.config"
    if (Test-Path $stalePagespeedConfig) {
        Remove-Item $stalePagespeedConfig -Force
        Write-Status "Removed stale pagespeed.config" "Gray"
    }

    # Copy test content
    Write-Status "Copying test content..."

    if (Test-Path $ExampleSource) {
        $destExample = "$WebRoot\mod_pagespeed_example"
        if (Test-Path $destExample) { Remove-Item $destExample -Recurse -Force }
        Copy-Item $ExampleSource $destExample -Recurse
        Write-Status "Copied example content to $destExample" "Gray"
    } else {
        Write-Status "Warning: Example source not found at $ExampleSource" "Yellow"
    }

    if (Test-Path $TestSource) {
        $destTest = "$WebRoot\mod_pagespeed_test"
        if (Test-Path $destTest) { Remove-Item $destTest -Recurse -Force }
        Copy-Item $TestSource $destTest -Recurse
        Write-Status "Copied test content to $destTest" "Gray"
    } else {
        Write-Status "Warning: Test source not found at $TestSource" "Yellow"
    }

    if (Test-Path $DoNotModifySource) {
        $destDoNotModify = "$WebRoot\do_not_modify"
        if (Test-Path $destDoNotModify) { Remove-Item $destDoNotModify -Recurse -Force }
        Copy-Item $DoNotModifySource $destDoNotModify -Recurse
        Write-Status "Copied do_not_modify content" "Gray"
    }

    # Refresh LastWriteTime on all copied fixture files so IIS emits a fresh
    # Last-Modified header. The release tarball / repo checkout can deliver
    # files with stale mtimes (e.g. the build date of a prior release baked
    # into the Hyper-V snapshot). mod_pagespeed's extend_cache filter
    # propagates the input's Last-Modified into the rewritten resource for
    # single-input on-the-fly transforms (RewriteDriver::Write +
    # ServerContext::SetDefaultLongCacheHeaders); when the file is old enough
    # that mod_pagespeed treats the cache entry as stale and re-rewrites with
    # a current-time Last-Modified, test_cache_extended_preserves_last_modified
    # sees origin (file mtime) != extended (now) and fails. Refreshing the
    # mtime to install-time anchors both sides to the same recent timestamp.
    $now = Get-Date
    foreach ($contentRoot in @("$WebRoot\mod_pagespeed_example", "$WebRoot\mod_pagespeed_test", "$WebRoot\do_not_modify")) {
        if (Test-Path $contentRoot) {
            Get-ChildItem -Path $contentRoot -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
                try { $_.LastWriteTime = $now } catch { }
            }
        }
    }
    Write-Status "Refreshed LastWriteTime on copied fixtures" "Gray"

    # Create per-directory web.config files to match Apache's debug.conf.
    # Apache sets Cache-Control: no-cache for the no_cache/ directory.
    $noCacheDir = "$WebRoot\mod_pagespeed_test\no_cache"
    if (Test-Path $noCacheDir) {
        @"
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
  <system.webServer>
    <staticContent>
      <clientCache cacheControlMode="DisableCache" />
    </staticContent>
  </system.webServer>
</configuration>
"@ | Set-Content "$noCacheDir\web.config" -Encoding UTF8
        Write-Status "Created no-cache web.config for $noCacheDir" "Gray"
    }

    # Create a simple index.html for testing
    @"
<!DOCTYPE html>
<html>
<head>
    <title>PageSpeed IIS Test</title>
</head>
<body>
    <h1>PageSpeed IIS Test Server</h1>
    <p>Server is running on port $Port</p>
</body>
</html>
"@ | Set-Content "$WebRoot\index.html"
}

function Install-IISComponents {
    Write-Status "Checking IIS installation..."

    if (-not (Test-IISInstalled)) {
        Write-Status "IIS is not installed. Installing..." "Yellow"
        try {
            # Install IIS with compression support
            # Note: Feature names are Web-Stat-Compression (static) and Web-Dyn-Compression (dynamic)
            Install-WindowsFeature -Name Web-Server,Web-Common-Http,Web-Static-Content,Web-Default-Doc,Web-Http-Errors,Web-Http-Logging,Web-Stat-Compression,Web-Dyn-Compression,Web-Mgmt-Console,Web-Mgmt-Tools -IncludeManagementTools
            Write-Status "IIS installed successfully" "Green"
        } catch {
            throw "Failed to install IIS: $_"
        }
    } else {
        Write-Status "IIS is already installed" "Green"
        # Ensure compression features are installed even if IIS was already present
        try {
            $compressionFeature = Get-WindowsFeature -Name Web-Stat-Compression -ErrorAction SilentlyContinue
            if ($compressionFeature -and -not $compressionFeature.Installed) {
                Write-Status "Installing compression features..." "Yellow"
                Install-WindowsFeature -Name Web-Stat-Compression,Web-Dyn-Compression | Out-Null
            }
        } catch { }
    }

    # Raise http.sys URL segment length limit. Default is 260 (MAX_PATH),
    # which is too short for PageSpeed combined resource URLs that concatenate
    # many filenames into a single path segment (e.g., a+b+c+d.pagespeed.cc.X.css).
    $httpParams = "HKLM:\SYSTEM\CurrentControlSet\Services\HTTP\Parameters"
    $current = Get-ItemProperty -Path $httpParams -Name "UrlSegmentMaxLength" -ErrorAction SilentlyContinue
    if (-not $current -or $current.UrlSegmentMaxLength -lt 4096) {
        Set-ItemProperty -Path $httpParams -Name "UrlSegmentMaxLength" -Value 4096 -Type DWord
        Write-Status "Set http.sys UrlSegmentMaxLength=4096" "Gray"
        # http.sys is a kernel driver -- use net stop/start which handles it
        # more reliably than Stop-Service (which can leave it in StopPending).
        & net stop http /y 2>$null
        Start-Sleep -Seconds 2
        & net start http 2>$null
        Write-Status "Restarted HTTP service for registry change" "Gray"
    }

    # Ensure services are running
    Start-Service WAS -ErrorAction SilentlyContinue
    Start-Service W3SVC -ErrorAction SilentlyContinue
}

function New-IISSite {
    Write-Status "Configuring IIS site..."

    # Delete existing site if present
    $existingSite = Invoke-AppCmd list site $SiteName 2>$null
    if ($existingSite) {
        Write-Status "Removing existing site: $SiteName" "Gray"
        Invoke-AppCmd delete site $SiteName | Out-Null
    }

    # Delete existing app pool if present
    $existingPool = Invoke-AppCmd list apppool $AppPoolName 2>$null
    if ($existingPool) {
        Write-Status "Removing existing app pool: $AppPoolName" "Gray"
        Invoke-AppCmd delete apppool $AppPoolName | Out-Null
    }

    # Create app pool with no managed code (for native module only)
    Write-Status "Creating application pool: $AppPoolName" "Gray"
    Invoke-AppCmd add apppool /name:$AppPoolName /managedRuntimeVersion:"" /managedPipelineMode:Integrated | Out-Null

    # Configure app pool settings
    Invoke-AppCmd set apppool $AppPoolName /processModel.identityType:ApplicationPoolIdentity | Out-Null
    Invoke-AppCmd set apppool $AppPoolName /autoStart:true | Out-Null

    # Disable idle timeout and periodic recycling for test stability.
    # Without these, the pool may shut down or recycle mid-test, especially
    # under slow conditions like Application Verifier + Page Heap.
    Invoke-AppCmd set apppool $AppPoolName /processModel.idleTimeout:00:00:00 | Out-Null
    Invoke-AppCmd set apppool $AppPoolName /recycling.periodicRestart.time:00:00:00 | Out-Null
    Invoke-AppCmd set apppool $AppPoolName /failure.rapidFailProtection:false | Out-Null

    # Create site
    Write-Status "Creating site: $SiteName on port $Port" "Gray"
    Invoke-AppCmd add site /name:$SiteName /physicalPath:$WebRoot /bindings:"http/*:${Port}:" | Out-Null

    # Assign app pool to site
    Invoke-AppCmd set site $SiteName /applicationDefaults.applicationPool:$AppPoolName | Out-Null

    # Configure MIME types for modern formats
    Write-Status "Configuring MIME types..." "Gray"
    try {
        Invoke-AppCmd set config $SiteName /section:staticContent /+"[fileExtension='.webp',mimeType='image/webp']" 2>$null
    } catch { }
    try {
        Invoke-AppCmd set config $SiteName /section:staticContent /+"[fileExtension='.woff2',mimeType='font/woff2']" 2>$null
    } catch { }

    # Enable static and dynamic compression for the site
    Write-Status "Configuring compression..." "Gray"
    try {
        Invoke-AppCmd set config $SiteName /section:urlCompression /doStaticCompression:true /doDynamicCompression:true 2>$null
    } catch { }

    Write-Status "IIS site configured successfully" "Green"
}

function New-HttpsBinding {
    # Add an HTTPS binding on $HttpsPort with a self-signed localhost cert so the
    # IIS system tests can exercise the PageSpeed module over TLS.
    #
    # CI runs Full IIS (run_iis_tests.ps1 -UseFullIIS), so HTTPS coverage is
    # wired here only; IIS Express (setup_iis_test.ps1) stays HTTP-only.
    #
    # The server-side HTTPS listener the tests hit needs three things:
    #   1. a cert in LocalMachine\My bound to the port via netsh sslcert;
    #   2. read access on the cert PRIVATE KEY for the IIS worker identity --
    #      without it SChannel cannot present the credential and the handshake
    #      fails (event 36870 / SEC_E_NO_CREDENTIALS);
    #   3. trust in LocalMachine\Root (+ an IP:127.0.0.1 SAN).
    # (3) and the IP SAN are forward-looking: the module's WinHTTP sub-resource
    # fetcher (asyncwinhttp.cpp) currently omits WINHTTP_FLAG_SECURE and cannot
    # fetch over TLS, so HTTPS *resource rewriting* is not exercised here yet
    #. These tests cover the HTTPS
    # *listener*: serving, headers, and the admin endpoint over TLS.
    Write-Status "Configuring HTTPS binding on port $HttpsPort..." "Gray"

    $certFriendlyName = "PageSpeedTestHttps"

    # Reuse a non-expired test cert if one already exists (idempotent across
    # repeated installs on a shared CI runner); otherwise mint a fresh one.
    $cert = Get-ChildItem "Cert:\LocalMachine\My" -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -eq $certFriendlyName -and $_.NotAfter -gt (Get-Date) } |
        Select-Object -First 1

    if (-not $cert) {
        Write-Status "Creating self-signed certificate ($certFriendlyName)..." "Gray"
        $cert = New-SelfSignedCertificate `
            -CertStoreLocation "Cert:\LocalMachine\My" `
            -FriendlyName $certFriendlyName `
            -Subject "CN=localhost" `
            -TextExtension @("2.5.29.17={text}DNS=localhost&IPAddress=127.0.0.1") `
            -KeyUsage DigitalSignature, KeyEncipherment `
            -NotAfter (Get-Date).AddYears(5)
    } else {
        Write-Status "Reusing existing self-signed certificate (thumbprint $($cert.Thumbprint))" "Gray"
    }

    $thumb = $cert.Thumbprint

    # Grant the IIS worker identity read on the cert's private key so SChannel
    # can present it during the server-side TLS handshake. New-SelfSignedCertificate
    # ACLs the CNG key to SYSTEM + Administrators only; IIS_IUSRS covers all
    # app-pool identities. Best-effort: a failure surfaces as failing HTTPS
    # tests, not a broken install. (Redirect, don't pipe, to keep $LASTEXITCODE.)
    try {
        $rsaKey = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($cert)
        if ($rsaKey -and $rsaKey.Key -and $rsaKey.Key.UniqueName) {
            $keyPath = Join-Path $env:ProgramData ("Microsoft\Crypto\Keys\" + $rsaKey.Key.UniqueName)
            & icacls $keyPath /grant "IIS_IUSRS:(R)" > $null 2>&1
            if ($LASTEXITCODE -eq 0) {
                Write-Status "Granted IIS_IUSRS read on the cert private key" "Gray"
            } else {
                Write-Status "WARNING: icacls grant on private key returned exit $LASTEXITCODE" "Yellow"
            }
        } else {
            Write-Status "WARNING: could not resolve cert private-key path for ACL grant" "Yellow"
        }
    } catch {
        Write-Status "WARNING: private-key ACL grant failed: $_" "Yellow"
    }

    # Trust the cert in LocalMachine\Root (forward-looking -- see header). Removed
    # again by Uninstall-IISSite so a shared runner is not left mutated.
    $rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
    try {
        $rootStore.Open("ReadWrite")
        if (-not ($rootStore.Certificates | Where-Object { $_.Thumbprint -eq $thumb })) {
            $rootStore.Add($cert)
            Write-Status "Added test certificate to LocalMachine\Root (trusted)" "Gray"
        }
    } finally {
        $rootStore.Close()
    }

    # Add the HTTPS site binding (empty host header -> matches all, no SNI).
    # -join '' coerces appcmd's output to a single string so -notmatch is a
    # regex test (not an array filter) regardless of how many bindings exist.
    $bindings = (Invoke-AppCmd list site $SiteName /text:bindings 2>$null) -join ''
    if ($bindings -notmatch "https/\*:${HttpsPort}:") {
        Invoke-AppCmd set site $SiteName /+"bindings.[protocol='https',bindingInformation='*:${HttpsPort}:']" | Out-Null
        Write-Status "Added https binding *:${HttpsPort}: to $SiteName" "Gray"
    }

    # Bind the cert to the port in HTTP.SYS. Idempotent: delete any stale
    # registration first (a shared runner may carry one from a prior run). The
    # appid is the well-known IIS Manager GUID -- any valid GUID works. Use
    # redirection (not a pipe to Out-Null) so $LASTEXITCODE reflects netsh --
    # piping a native command to a cmdlet leaves $LASTEXITCODE stale (PS #19848).
    $appId = "{4dc3e181-e14b-4a21-b022-59fc669b0914}"
    & netsh http delete sslcert ipport=0.0.0.0:$HttpsPort > $null 2>&1
    $sslAddOut = & netsh http add sslcert ipport=0.0.0.0:$HttpsPort certhash=$thumb appid=$appId certstorename=MY 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Status "WARNING: netsh add sslcert failed (exit $LASTEXITCODE) for 0.0.0.0:$HttpsPort -- $sslAddOut" "Yellow"
    } else {
        Write-Status "Bound certificate $thumb to 0.0.0.0:$HttpsPort" "Green"
    }
}

function New-WebConfig {
    Write-Status "Creating web.config and pagespeed.config..."

    # Write web.config with IIS settings only (no <pagespeed> XML section).
    # The IISpeed-adopted module reads pagespeed.config flat files.
    $webConfig = @"
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
  <system.webServer>
    <defaultDocument enabled="true">
      <files>
        <clear />
        <add value="index.html" />
      </files>
    </defaultDocument>

    <directoryBrowse enabled="false" />

    <httpErrors errorMode="Detailed" />

    <httpProtocol>
      <customHeaders>
        <add name="X-PageSpeed-Test" value="IIS-Full" />
      </customHeaders>
    </httpProtocol>

    <!-- Required for PageSpeed combined resource URLs containing '+' -->
    <security>
      <requestFiltering allowDoubleEscaping="true">
        <requestLimits maxUrl="16384" maxQueryString="8192" />
      </requestFiltering>
    </security>

  </system.webServer>

</configuration>
"@

    $webConfig | Set-Content "$WebRoot\web.config" -Encoding UTF8
    Write-Status "web.config written to $WebRoot\web.config" "Gray"

    # Generate pagespeed.config flat file (IISpeed format)
    $hasModule = (Test-Path $ModulePath) -and (-not $NoModule)

    if ($hasModule) {
        $pagespeedConfig = @"
pagespeed on
pagespeed RewriteLevel CoreFilters
pagespeed FileCachePath $CacheDir
pagespeed Statistics on
pagespeed StatisticsLogging on
pagespeed EnableCachePurge on
pagespeed RateLimitBackgroundFetches on
pagespeed InPlaceResourceOptimization on
pagespeed FetchHttps enable,allow_self_signed
pagespeed CriticalImagesBeaconEnabled false
pagespeed BlockingRewriteKey psatest
pagespeed Library 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js
pagespeed MessageBufferSize 100000
pagespeed AdminPath /pagespeed_admin
pagespeed StatisticsPath /pagespeed_statistics
pagespeed GlobalStatisticsPath /pagespeed_global_statistics
pagespeed ConsolePath /pagespeed_console
pagespeed MessagesPath /pagespeed_message
pagespeed GlobalAdminPath /pagespeed_global_admin
"@

        # Site-level config
        $pagespeedConfig | Set-Content "$WebRoot\pagespeed.config" -Encoding UTF8
        Write-Status "pagespeed.config written to $WebRoot\pagespeed.config" "Gray"

        # Server-level config at %ProgramData%\We-Amp\IISWebSpeed\
        $serverConfigDir = "$env:ProgramData\We-Amp\IISWebSpeed"
        if (-not (Test-Path $serverConfigDir)) {
            New-Item -ItemType Directory -Path $serverConfigDir -Force | Out-Null
            Write-Status "Created server config directory: $serverConfigDir" "Gray"
        }
        $pagespeedConfig | Set-Content "$serverConfigDir\pagespeed.config" -Encoding UTF8
        Write-Status "pagespeed.config written to $serverConfigDir\pagespeed.config" "Gray"

        # Copy pagespeed.config to the no_cache test directory so the module
        # recognises it even when IIS treats it as a separate application
        # context due to its local web.config (observed on Windows 10).
        $noCacheDir = "$WebRoot\mod_pagespeed_test\no_cache"
        if (Test-Path $noCacheDir) {
            Copy-Item "$WebRoot\pagespeed.config" "$noCacheDir\pagespeed.config" -Force
            Write-Status "Copied pagespeed.config to $noCacheDir" "Gray"
        }

        Write-Status "PageSpeed module will be enabled (flat-file config)" "Cyan"
    } else {
        if ($NoModule) {
            Write-Status "PageSpeed module disabled by -NoModule flag" "Yellow"
        } else {
            Write-Status "PageSpeed module not found at $ModulePath" "Yellow"
            Write-Status "Tests will run without PageSpeed optimizations" "Yellow"
        }
    }
}
function Install-PageSpeedModule {
    if ($NoModule) {
        Write-Status "Skipping PageSpeed module installation (-NoModule)" "Yellow"
        return
    }

    if (-not (Test-Path $ModulePath)) {
        Write-Status "PageSpeed module not found at: $ModulePath" "Yellow"
        Write-Status "Build it with: bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis_dll" "Gray"
        return
    }

    Write-Status "Installing PageSpeed native module..."

    # Uninstall existing module first to release the DLL lock, then stop
    # IIS completely (WAS + W3SVC) and wait for w3wp to exit before overwrite.
    try { Invoke-AppCmd uninstall module PageSpeedModule 2>$null } catch { }
    try { Invoke-AppCmd delete module PageSpeedModule 2>$null } catch { }
    Stop-Service W3SVC -Force -ErrorAction SilentlyContinue
    Stop-Service WAS -Force -ErrorAction SilentlyContinue
    # Wait for w3wp.exe to fully exit (it holds the DLL lock)
    Get-Process w3wp -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Status "Waiting for w3wp (PID $($_.Id)) to exit..." "Gray"
        $_ | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
    }

    # Copy DLL to system location with retry (defender/prefetch can hold brief locks)
    $systemModulePath = "C:\Windows\System32\inetsrv\pagespeed_iis.dll"
    $copied = $false
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try {
            Copy-Item $ModulePath $systemModulePath -Force
            $copied = $true
            break
        } catch {
            Write-Status "Copy attempt $attempt failed: $_ -- retrying in 2s..." "Yellow"
            Start-Sleep -Seconds 2
        }
    }
    if (-not $copied) {
        throw "Failed to copy $ModulePath to $systemModulePath after 5 attempts"
    }
    Write-Status "Copied module to $systemModulePath" "Gray"

    # Restart IIS services
    Start-Service WAS -ErrorAction SilentlyContinue
    Start-Service W3SVC -ErrorAction SilentlyContinue

    try {
        # Install as global native module (/add:true enables it for all sites)
        Invoke-AppCmd install module /name:PageSpeedModule /image:$systemModulePath /add:true
        Write-Status "Registered PageSpeedModule as global module" "Green"
    } catch {
        # If install fails (e.g. already exists), try just adding it
        try {
            Invoke-AppCmd add module /name:PageSpeedModule /type: /preCondition:
            Write-Status "Added PageSpeedModule (was already installed)" "Yellow"
        } catch {
            Write-Status "Warning: Could not register module: $_" "Yellow"
        }
    }
}

function Uninstall-PageSpeedModule {
    Write-Status "Uninstalling PageSpeed module..."

    try {
        Invoke-AppCmd uninstall module PageSpeedModule 2>$null
        Write-Status "Unregistered PageSpeedModule" "Gray"
    } catch { }


    $systemModulePath = "C:\Windows\System32\inetsrv\pagespeed_iis.dll"
    if (Test-Path $systemModulePath) {
        Remove-Item $systemModulePath -Force -ErrorAction SilentlyContinue
        Write-Status "Removed $systemModulePath" "Gray"
    }
}

function Start-IISSite {
    Write-Status "Starting IIS..."

    # Ensure services are running
    Start-Service WAS -ErrorAction SilentlyContinue
    Start-Service W3SVC -ErrorAction SilentlyContinue

    # Start app pool
    try {
        Invoke-AppCmd start apppool $AppPoolName | Out-Null
    } catch { }

    # Start site
    try {
        Invoke-AppCmd start site $SiteName | Out-Null
    } catch { }

    # Wait for server to be ready.
    # When the PageSpeed module is installed, use /pagespeed_admin/ as the
    # readiness endpoint -- it is handled exclusively by the module, so:
    #   404 -> module DLL not yet loaded by IIS
    #   200 + X-Pagespeed-Init-Status header -> module loaded but its
    #     ProcessContext init failed (cache-path, post-config, etc.) and it
    #     is serving the local-only diagnostic page. Either still warming
    #     up, or we hit a real failure that needs operator attention; treat
    #     as not-ready and keep polling until either the header disappears
    #     (success path) or the budget expires.
    #   200 + admin HTML (no X-Pagespeed-Init-Status) -> module fully
    #     initialized, ready for tests.
    # This avoids a race under Application Verifier + Page Heap where IIS
    # serves static files on "/" before the module is active, making the
    # readiness check pass while the module is still initializing.
    $hasModule = (Test-Path $ModulePath) -and (-not $NoModule)
    $readinessUrl = if ($hasModule) {
        "http://localhost:$Port/pagespeed_admin/"
    } else {
        "http://localhost:$Port/"
    }
    Write-Status "Waiting for server to be ready ($readinessUrl)..."
    # Budget raised from 60 to 180 attempts. The readiness CONDITION is
    # unchanged -- we still only declare ready when /pagespeed_admin/ returns
    # 200 WITHOUT an X-Pagespeed-Init-Status header (i.e. ProcessContext init
    # actually succeeded). The larger budget only gives a heavily-loaded
    # shared Windows runner (several runners share one box) more wall-clock for a slow-but-eventually-successful init,
    # instead of `exit 1`-ing the whole job at 60s. A genuinely broken init
    # (header never clears, or DLL never loads) still fails -- just later --
    # so this does not mask a real init regression. (IIS init-load flake.)
    $maxWait = 180
    $waited = 0
    # Track readiness with an explicit flag rather than inferring it from
    # `$waited -ge $maxWait` -- otherwise a success on the very last attempt
    # (where $waited == $maxWait at break) would be misread as a timeout.
    $ready = $false
    while ($waited -lt $maxWait) {
        Start-Sleep -Seconds 1
        $waited++

        try {
            $response = Invoke-WebRequest -Uri $readinessUrl -UseBasicParsing -TimeoutSec 2 -ErrorAction SilentlyContinue
            if ($response.StatusCode -eq 200) {
                # The init-status header is only present on the local-only
                # diagnostic page (X-Pagespeed-Init-Status: cache-path-empty
                # | cache-path-missing | cache-path-not-writable |
                # post-config-failed | startup-failed). If the header is
                # set, the module is up but init has not (yet) succeeded.
                $initStatus = $null
                if ($response.Headers -and $response.Headers.ContainsKey("X-Pagespeed-Init-Status")) {
                    $initStatus = $response.Headers["X-Pagespeed-Init-Status"]
                }
                if ($initStatus) {
                    if ($waited % 5 -eq 0) {
                        Write-Status "Module still initializing or init failed (X-Pagespeed-Init-Status=$initStatus, attempt $waited/$maxWait)..." "Yellow"
                    }
                    continue
                }
                Write-Status "Server is ready on port $Port (after $waited attempts)!" "Green"
                $ready = $true
                break
            }
        } catch {
            # Server not ready yet (404 = module not loaded, connection refused = IIS starting)
            if ($waited % 5 -eq 0) {
                Write-Status "Still waiting for module to load (attempt $waited/$maxWait)..." "Yellow"
            }
        }
    }

    if (-not $ready) {
        Write-Status "Timeout waiting for server to start" "Red"
        Get-ServerStatus
        exit 1
    }

    # Warm up the rewrite pipeline before the test suite starts.
    #
    # The readiness gate above only confirms ProcessContext init succeeded
    # (the admin diagnostic page no longer carries X-Pagespeed-Init-Status).
    # It does NOT confirm the *rewrite* path is warm: the first request that
    # actually exercises a filter pays a one-time cost (rewrite-driver setup,
    # first file-cache writes, PSOL worker spin-up). On a loaded runner that
    # cold cost has been observed to push the CSS-combiner / flatten cluster
    # of tests past their 30s (x multiplier) fetch_until budgets -- the named
    # "init slow under load" flake (7-12 of 326 tests TimeoutError at 60s
    # while the server still returns 200).
    #
    # We prime that path ONCE here, outside any per-test budget, by polling
    # the canonical combine_css example until it actually rewrites. This is
    # best-effort: it asserts NOTHING and never fails setup. If the module
    # is genuinely broken, the readiness gate above and the tests themselves
    # still fail exactly as before -- so this cannot hide a real regression;
    # it only removes the cold-start tax from the first few real tests.
    if ($hasModule) {
        $warmUrl = "http://localhost:$Port/mod_pagespeed_example/combine_css.html?PageSpeedFilters=combine_css"
        Write-Status "Warming up rewrite pipeline ($warmUrl)..."
        $warmAttempts = 30
        $warmed = $false
        for ($w = 1; $w -le $warmAttempts; $w++) {
            try {
                $wr = Invoke-WebRequest -Uri $warmUrl -UseBasicParsing -TimeoutSec 5 -ErrorAction SilentlyContinue
                # `.pagespeed.cc.` / `.pagespeed.cf.` in the body means the
                # combiner has produced a rewritten resource -- the pipeline
                # is hot. Match the same evidence the test asserts on.
                if ($wr -and $wr.StatusCode -eq 200 -and ($wr.Content -match '\.pagespeed\.(cc|cf)\.')) {
                    $warmed = $true
                    Write-Status "Rewrite pipeline warm (after $w probe(s))." "Green"
                    break
                }
            } catch { }
            Start-Sleep -Seconds 1
        }
        if (-not $warmed) {
            # Non-fatal: the suite still runs (each test has its own
            # multiplier-scaled fetch_until budget). We only log so a slow
            # warm-up is visible in the setup log when triaging.
            Write-Status "Warm-up did not observe a rewritten resource within $warmAttempts probe(s); continuing (tests have their own budgets)." "Yellow"
        }
    }

    # Display environment variables for tests
    Write-Status ""
    Write-Status "Test environment ready. Set these environment variables:" "Cyan"
    Write-Status '  $env:PAGESPEED_HOST = "localhost"' "White"
    Write-Status "  `$env:PAGESPEED_PORT = '$Port'" "White"
    Write-Status '  $env:PAGESPEED_TEST_ROOT = "/mod_pagespeed_test"' "White"
    Write-Status '  $env:PAGESPEED_EXAMPLE_ROOT = "/mod_pagespeed_example"' "White"
    Write-Status "  `$env:PAGESPEED_CACHE_DIR = '$CacheDir'" "White"
    Write-Status '  $env:PAGESPEED_SERVER_TYPE = "iis"' "White"

    $hasModule = (Test-Path $ModulePath) -and (-not $NoModule)
    if ($hasModule) {
        Write-Status '  $env:PAGESPEED_STATS_ENABLED = "1"' "White"
    } else {
        Write-Status '  $env:PAGESPEED_STATS_ENABLED = "0"' "White"
    }
}

function Stop-IISSite {
    Write-Status "Stopping IIS site..."

    try {
        Invoke-AppCmd stop site $SiteName 2>$null | Out-Null
        Write-Status "Stopped site: $SiteName" "Gray"
    } catch { }

    try {
        Invoke-AppCmd stop apppool $AppPoolName 2>$null | Out-Null
        Write-Status "Stopped app pool: $AppPoolName" "Gray"
    } catch { }

    Write-Status "IIS site stopped" "Green"
}

function Get-ServerStatus {
    Write-Status "IIS Status:"

    # Check services
    $w3svc = Get-Service -Name W3SVC -ErrorAction SilentlyContinue
    if ($w3svc) {
        $color = if ($w3svc.Status -eq 'Running') { "Green" } else { "Yellow" }
        Write-Status "  W3SVC service: $($w3svc.Status)" $color
    } else {
        Write-Status "  W3SVC service: Not installed" "Red"
        return
    }

    # Check app pool
    try {
        $poolStatus = Invoke-AppCmd list apppool $AppPoolName
        if ($poolStatus) {
            $state = if ($poolStatus -match "state:(\w+)") { $matches[1] } else { "Unknown" }
            $color = if ($state -eq "Started") { "Green" } else { "Yellow" }
            Write-Status "  App pool '$AppPoolName': $state" $color
        } else {
            Write-Status "  App pool '$AppPoolName': Not found" "Yellow"
        }
    } catch {
        Write-Status "  App pool '$AppPoolName': Error checking status" "Yellow"
    }

    # Check site
    try {
        $siteStatus = Invoke-AppCmd list site $SiteName
        if ($siteStatus) {
            $state = if ($siteStatus -match "state:(\w+)") { $matches[1] } else { "Unknown" }
            $color = if ($state -eq "Started") { "Green" } else { "Yellow" }
            Write-Status "  Site '$SiteName': $state" $color
        } else {
            Write-Status "  Site '$SiteName': Not found" "Yellow"
        }
    } catch {
        Write-Status "  Site '$SiteName': Error checking status" "Yellow"
    }

    # Try to connect
    try {
        $response = Invoke-WebRequest -Uri "http://localhost:$Port/" -UseBasicParsing -TimeoutSec 5 -ErrorAction SilentlyContinue
        Write-Status "  HTTP response: $($response.StatusCode)" "Green"

        # Check for PageSpeed headers
        $psHeader = $response.Headers["X-Mod-Pagespeed"]
        if ($psHeader) {
            Write-Status "  PageSpeed version: $psHeader" "Cyan"
        } else {
            Write-Status "  PageSpeed module: Not active (no X-Mod-Pagespeed header)" "Yellow"
        }

        $testHeader = $response.Headers["X-PageSpeed-Test"]
        if ($testHeader) {
            Write-Status "  Test header: $testHeader" "Gray"
        }
    } catch {
        Write-Status "  HTTP response: Cannot connect to port $Port" "Red"
    }

    # Check module registration
    Write-Status ""
    Write-Status "Module status:"
    try {
        $modules = Invoke-AppCmd list module 2>$null
        $hasPageSpeed = $modules | Where-Object { $_ -match "PageSpeedModule" }
        if ($hasPageSpeed) {
            Write-Status "  PageSpeedModule: Registered" "Green"
        } else {
            Write-Status "  PageSpeedModule: Not registered" "Yellow"
        }
    } catch {
        Write-Status "  Could not check module status" "Yellow"
    }

    # Check DLL
    $systemModulePath = "C:\Windows\System32\inetsrv\pagespeed_iis.dll"
    if (Test-Path $systemModulePath) {
        Write-Status "  Module DLL: $systemModulePath exists" "Green"
    } elseif (Test-Path $ModulePath) {
        Write-Status "  Module DLL: Built at $ModulePath (not installed)" "Yellow"
    } else {
        Write-Status "  Module DLL: Not found" "Yellow"
    }
}

function Uninstall-IISSite {
    Write-Status "Uninstalling test site..."

    Stop-IISSite

    # Remove the HTTPS test artifacts so a shared runner is not left
    # mutated: the HTTP.SYS sslcert registration (not removed by deleting the
    # site), and the self-signed cert from BOTH LocalMachine\My and \Root.
    try {
        & netsh http delete sslcert ipport=0.0.0.0:$HttpsPort > $null 2>&1
    } catch { }
    foreach ($storeName in @("My", "Root")) {
        try {
            $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, "LocalMachine")
            $store.Open("ReadWrite")
            $store.Certificates |
                Where-Object { $_.FriendlyName -eq "PageSpeedTestHttps" } |
                ForEach-Object { $store.Remove($_) }
            $store.Close()
        } catch { }
    }

    # Remove site
    try {
        Invoke-AppCmd delete site $SiteName 2>$null | Out-Null
        Write-Status "Deleted site: $SiteName" "Gray"
    } catch { }

    # Remove app pool
    try {
        Invoke-AppCmd delete apppool $AppPoolName 2>$null | Out-Null
        Write-Status "Deleted app pool: $AppPoolName" "Gray"
    } catch { }

    # Uninstall module
    Uninstall-PageSpeedModule

    Write-Status "Test site uninstalled" "Green"
}

function Install-IISSiteComplete {
    Install-IISComponents
    Initialize-TestEnvironment
    Install-PageSpeedModule
    New-IISSite
    New-HttpsBinding
    New-WebConfig
    Start-IISSite
}

# Main entry point
switch ($Action) {
    'install' {
        Install-IISSiteComplete
    }
    'uninstall' {
        Uninstall-IISSite
    }
    'start' {
        Start-IISSite
    }
    'stop' {
        Stop-IISSite
    }
    'status' {
        Get-ServerStatus
    }
    'restart' {
        Stop-IISSite
        Start-Sleep -Seconds 2
        Start-IISSite
    }
}
