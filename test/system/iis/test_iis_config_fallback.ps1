# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# test_iis_config_fallback.ps1 - Pins the pagespeed.config layering contract
# of the IIS module: the machine-global base tier (canonical-first, with the
# legacy IISWebSpeed fallback location) and the per-site override.
#
# The resolution chain (pagespeed/iis/iis_config_util.h; shared by the factory
# parse in iis_module_factory.cpp, the request-time merge in iis_misc.cpp and
# the engage gate in iis_http_module.cpp, so the three can never diverge):
#   1. %ProgramData%\We-Amp\PageSpeed\pagespeed.config   machine-global base
#   2. %ProgramData%\We-Amp\IISWebSpeed\pagespeed.config legacy base, reached
#      only when the canonical tier holds no config (upgrade-from-IISpeed
#      boxes; the MSI preserves the legacy file via NeverOverwrite)
#   3. <site physical path>\pagespeed.config              per-site override,
#      authoritative for the directives it sets
# Within each directory pagespeed.config is preferred over iiswebspeed.config.
#
# The previous version of this test planted a legacy base with 'pagespeed
# off' and asserted the default site went quiet. But the default site has its
# own per-site config (the MSI seeds one into wwwroot), which correctly wins
# tier 3, so the assertion checked the wrong layer and failed on every run
# while the fallback itself worked. This version drives each layer through
# its own throwaway site.
#
# One loader detail shapes the setup: a site ENGAGES only when it has its own
# per-site config file, of either name (OnBeginRequest bails with 'no
# configuration' when SiteConfigExists() is false). A site with no file at all
# is never touched by the module, so nothing from the base tier is observable
# there. The realizable form of a site with no per-site configuration is
# therefore a site whose file exists but sets nothing of its own: every
# directive, including the master switch under test, then falls through to
# the base tier.
#
# The observable is the master switch 'pagespeed on/off', read through the
# X-Page-Speed response header. When the layered options are off the module
# short-circuits the response path and never emits the header (the 'pagespeed
# disabled' branch in iis_http_module.cpp; ProxyFetch::AddPagespeedHeader
# adds it only while enabled). When they are on, every HTML response carries
# it. Each case asserts the header in both directions, so a pass means the
# VALUE of the layer under test reached the site, not a default.
#
# Cases (each provisions its own IIS site, on its own port and directory, and
# removes it in finally, even on failure; machine-global files are stashed
# and restored the same way, and a directory the test created only to hold
# any of this is dropped again when it is left empty):
#   1. BASE TIER GOVERNS: engage-only site. Base says on: the site optimizes.
#      Base flipped to off: the site goes quiet. The base tier's value
#      reaches a site that sets nothing per-site.
#   2. PER-SITE OVERRIDE WINS: same engage-only content plus 'pagespeed off'
#      while the base says on: the site is off. Flip the per-site file back
#      to on: the site optimizes again. This is the precedence the old test
#      tripped over, now asserted deliberately.
#   3. LEGACY LOCATION FALLBACK: canonical base stashed away and legacy
#      IISWebSpeed\pagespeed.config planted with 'off': the engage-only site
#      is off. Flip the legacy file to on: the site optimizes. The factory
#      fell back to the legacy location exactly as an upgrade box does.
#
# Pre-conditions (asserted; aborts with exit 2 when absent):
#   - PageSpeed IIS module installed (canonical base config present)
#   - The base contains exactly one bare 'pagespeed on' master switch line
#   - Default site healthy on $Url (X-Page-Speed present, no init-status)
#
# Run as Administrator (appcmd site and apppool changes, ProgramData writes).
# Windows PowerShell 5.1 compatible; ASCII only.

[CmdletBinding()]
param(
    [string]$Url = "http://localhost/",
    [string]$AppPool = "DefaultAppPool",
    [string]$SiteRoot = "C:\inetpub\cfgfb",
    [int]$PortBase = 8081,
    [int]$PortSite = 8082,
    [int]$PortLegacy = 8083,
    [int]$PollSeconds = 40,
    # Empty -> derived from CommonApplicationData at runtime, the same root
    # the module resolves (SHGetFolderPath(CSIDL_COMMON_APPDATA)).
    [string]$CanonicalConfig = "",
    [string]$CanonicalFallbackConfig = "",
    [string]$LegacyConfig = "",
    [string]$LegacyFallbackConfig = ""
)

$ErrorActionPreference = 'Stop'
$appcmd = "$env:SystemRoot\system32\inetsrv\appcmd.exe"

$programData = [System.Environment]::GetFolderPath('CommonApplicationData')
if (-not $CanonicalConfig) {
    $CanonicalConfig = Join-Path $programData "We-Amp\PageSpeed\pagespeed.config"
}
if (-not $CanonicalFallbackConfig) {
    $CanonicalFallbackConfig = Join-Path $programData "We-Amp\PageSpeed\iiswebspeed.config"
}
if (-not $LegacyConfig) {
    $LegacyConfig = Join-Path $programData "We-Amp\IISWebSpeed\pagespeed.config"
}
if (-not $LegacyFallbackConfig) {
    $LegacyFallbackConfig = Join-Path $programData "We-Amp\IISWebSpeed\iiswebspeed.config"
}

# The probe page: plain HTML, no sub-resources. The X-Page-Speed header is
# added as soon as the response is sniffed as HTML while the layered options
# are enabled, so no origin round-trip beyond the page itself is needed (the
# factory's loopback fetcher hardcodes port 80; probe sites live on their own
# ports and must not depend on it).
$ProbeHtml = @'
<!DOCTYPE html>
<html>
<head><title>config fallback probe</title></head>
<body><p>pagespeed config fallback layering probe</p><!-- probe --></body>
</html>
'@

# --- Helpers -------------------------------------------------------------

# Text helpers for the variants this test plants: ReadAllText/WriteAllText
# default to UTF-8 without a BOM, so the shipped config's UTF-8 comments
# survive the derivation (Get-Content/Set-Content under 5.1 would transcode
# them). The machine-global files themselves are stashed as raw bytes below,
# so a box whose own config carries a BOM or non-UTF-8 bytes gets it back
# unchanged, byte for byte.
function Read-ConfigText {
    param([string]$Path)
    return [System.IO.File]::ReadAllText($Path)
}
function Write-ConfigText {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text)
}

function Get-PsState {
    param([string]$u)
    try {
        $r = Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 5
        return @{ StatusCode = [int]$r.StatusCode; Headers = $r.Headers }
    } catch {
        if ($_.Exception.Response) {
            return @{ StatusCode = [int]$_.Exception.Response.StatusCode; Headers = @{} }
        }
        return $null
    }
}

function Test-PageSpeedState {
    # $true only for a real 200 response whose X-Page-Speed presence matches
    # $wantPresent. A dead site, a non-200 status or a diagnostic
    # X-Pagespeed-Init-Status never satisfies either direction, so a broken
    # setup cannot fake a pass on the 'absent' assertions.
    param([string]$u, [bool]$wantPresent)
    $r = Get-PsState -u $u
    if ($null -eq $r -or $r.StatusCode -ne 200) { return $false }
    if ($r.Headers['X-Pagespeed-Init-Status']) { return $false }
    $has = [bool]$r.Headers['X-Page-Speed']
    return ($has -eq $wantPresent)
}

function Wait-ForPageSpeedState {
    param([string]$u, [bool]$wantPresent, [int]$timeoutSec)
    $elapsed = 0
    while ($elapsed -lt $timeoutSec) {
        if (Test-PageSpeedState -u $u -wantPresent $wantPresent) { return $true }
        Start-Sleep -Seconds 2
        $elapsed += 2
    }
    return $false
}

function Assert-PageSpeedState {
    param([string]$u, [bool]$wantPresent, [string]$desc)
    if (-not (Wait-ForPageSpeedState -u $u -wantPresent $wantPresent -timeoutSec $PollSeconds)) {
        $r = Get-PsState -u $u
        $obs = "<no response>"
        if ($null -ne $r) {
            $obs = "status=$($r.StatusCode), X-Page-Speed='$($r.Headers['X-Page-Speed'])', " +
                "X-Pagespeed-Init-Status='$($r.Headers['X-Pagespeed-Init-Status'])'"
        }
        $wanted = "absent"
        if ($wantPresent) { $wanted = "present" }
        throw "FAIL: $desc - expected X-Page-Speed $wanted at $u within ${PollSeconds}s; observed: $obs"
    }
    Write-Host "[config-fallback] PASS: $desc"
}

function Recycle-AppPool {
    param([string]$pool)
    & $appcmd recycle apppool /apppool.name:$pool | Out-Null
    # Recycle is asynchronous; give the worker a moment to spin up before the
    # first poll fires a request.
    Start-Sleep -Seconds 3
}

function Invoke-AppCmdQuiet {
    # For idempotent calls (delete on a missing object, start on a started
    # site) where appcmd diagnostics on stderr are expected. Under
    # $ErrorActionPreference='Stop' a redirected stderr line would raise
    # NativeCommandError in Windows PowerShell 5.1, so drop EAP for the call.
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $appcmd @args 2>$null | Out-Null
    } finally {
        $ErrorActionPreference = $eap
    }
}

function New-ProbeSite {
    param([string]$Name, [int]$Port, [string]$ConfigText)
    $dir = Join-Path $SiteRoot $Name
    # Idempotent clean slate: drop any leftover site (and its port binding)
    # from an interrupted earlier run before recreating it.
    Invoke-AppCmdQuiet delete site $Name
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Write-ConfigText -Path (Join-Path $dir "index.html") -Text $ProbeHtml
    Write-ConfigText -Path (Join-Path $dir "pagespeed.config") -Text $ConfigText
    try {
        & $appcmd add site /name:$Name /physicalPath:$dir /bindings:"http/*:${Port}:" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "appcmd add site failed for $Name on port $Port (binding conflict?)"
        }
        & $appcmd set site $Name /applicationDefaults.applicationPool:$AppPool | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "appcmd set site failed for $Name (app pool $AppPool)"
        }
        Invoke-AppCmdQuiet start site $Name
    } catch {
        # Never leave a half-provisioned site behind on a failed setup.
        Invoke-AppCmdQuiet delete site $Name
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
        throw
    }
    Write-Host "[config-fallback] provisioned site $Name on port $Port rooted at $dir"
    return @{ Name = $Name; Port = $Port; Dir = $dir; ConfigPath = (Join-Path $dir "pagespeed.config") }
}

function Remove-ProbeSite {
    param([string]$Name)
    Invoke-AppCmdQuiet delete site $Name
    $dir = Join-Path $SiteRoot $Name
    if (Test-Path -LiteralPath $dir) {
        Start-Sleep -Seconds 1
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $dir) {
            Write-Host "::warning::could not fully remove $dir (locked handles); leftover will be overwritten by the next run"
        }
    }
    Write-Host "[config-fallback] removed site $Name"
}

# Stash of machine-global config files, held as raw bytes so a restore is
# byte-exact (a text round-trip would drop a BOM and mangle any non-UTF-8
# byte in a box's own config). Restore is LIFO so nested cases unwind in
# reverse order; a file the test planted (not stashed) is deleted again.
$script:stash = @()
function Stash-ConfigFile {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        $script:stash += @{ Path = $Path; Existed = $true; Bytes = $bytes }
        Remove-Item -LiteralPath $Path -Force
        Write-Host "[config-fallback] stashed $Path"
    } else {
        $script:stash += @{ Path = $Path; Existed = $false; Bytes = $null }
    }
}
function Restore-StashedConfigs {
    for ($i = $script:stash.Count - 1; $i -ge 0; $i--) {
        $e = $script:stash[$i]
        if ($e.Existed) {
            [System.IO.File]::WriteAllBytes($e.Path, $e.Bytes)
            Write-Host "[config-fallback] restored $($e.Path)"
        } elseif (Test-Path -LiteralPath $e.Path) {
            Remove-Item -LiteralPath $e.Path -Force -ErrorAction SilentlyContinue
            Write-Host "[config-fallback] removed planted $($e.Path)"
        }
    }
    $script:stash = @()
}

# --- Pre-condition checks ------------------------------------------------

Write-Host "[config-fallback] precondition: canonical base config present ($CanonicalConfig)"
if (-not (Test-Path -LiteralPath $CanonicalConfig)) {
    Write-Host "[config-fallback] canonical base config not present; cannot exercise the layering (module not installed?)"
    exit 2
}

Write-Host "[config-fallback] precondition: default site healthy at $Url"
if (-not (Wait-ForPageSpeedState -u $Url -wantPresent $true -timeoutSec $PollSeconds)) {
    $r = Get-PsState -u $Url
    $obs = "<no response>"
    if ($null -ne $r) {
        $obs = "status=$($r.StatusCode), X-Page-Speed='$($r.Headers['X-Page-Speed'])', " +
            "X-Pagespeed-Init-Status='$($r.Headers['X-Pagespeed-Init-Status'])'"
    }
    Write-Host "[config-fallback] baseline not healthy: $obs"
    exit 2
}

# Everything the test writes is derived from the shipped base content, so it
# must carry exactly one bare master switch line to flip or strip. The regex
# deliberately matches only the bare switch, not multi-token directives like
# 'pagespeed RewriteLevel ...' or comment lines mentioning it, and matches
# case-insensitively because that is how the loader reads the directive and
# its value (StringCaseEqual in iis_configuration.cpp and rewrite_options.cc).
# One pattern string drives the count check and both replacements, so they can
# never disagree on case or anchoring.
$canonicalContent = Read-ConfigText -Path $CanonicalConfig
$masterSwitchPattern = '(?im)^\s*pagespeed\s+on\s*$'
$switchMatches = [regex]::Matches($canonicalContent, $masterSwitchPattern)
if ($switchMatches.Count -ne 1) {
    Write-Host "[config-fallback] the base config must contain exactly one bare 'pagespeed on' master switch line; found $($switchMatches.Count) in $CanonicalConfig"
    exit 2
}

# -replace honors the pattern's embedded (?i), so both replacements are
# exactly as case-insensitive as the count check. The strip also consumes the
# line terminator, so it removes the whole switch line, never a prefix of a
# longer line that merely starts with 'pagespeed on'.
$canonicalOff = $canonicalContent -replace $masterSwitchPattern, 'pagespeed off'
# The engage-only site config: the shipped base content minus the master
# switch line (with its newline), so the site engages (the gate only requires
# the file to exist) but sets nothing of its own.
$engageConfig = $canonicalContent -replace ($masterSwitchPattern + '\r?\n?'), ''
$engageOff = $engageConfig.TrimEnd() + "`r`npagespeed off`r`n"
$engageOn = $engageConfig.TrimEnd() + "`r`npagespeed on`r`n"

# The probe-site root is dropped at teardown only when this run created it
# and every probe directory below it is gone, so the test leaves behind no
# directory it did not find.
$siteRootExisted = Test-Path -LiteralPath $SiteRoot

# --- Cases ---------------------------------------------------------------

try {
    # Case 1: base tier governs a site that sets nothing per-site.
    $site1 = $null
    try {
        $site1 = New-ProbeSite -Name "cfgfb-base" -Port $PortBase -ConfigText $engageConfig
        $u1 = "http://localhost:$PortBase/"
        Recycle-AppPool -pool $AppPool
        Assert-PageSpeedState -u $u1 -wantPresent $true -desc "case 1: base 'on' reaches an engage-only site (X-Page-Speed present)"

        Stash-ConfigFile -Path $CanonicalConfig
        try {
            Write-ConfigText -Path $CanonicalConfig -Text $canonicalOff
            Recycle-AppPool -pool $AppPool
            Assert-PageSpeedState -u $u1 -wantPresent $false -desc "case 1: base flipped to 'off' reaches the same site (X-Page-Speed absent)"
        } finally {
            Restore-StashedConfigs
        }
    } finally {
        if ($null -ne $site1) { Remove-ProbeSite -Name $site1.Name }
    }

    # Case 2: the per-site override wins over the base tier. The base is back
    # to its shipped 'on' (restored above); this site carries its own 'off'.
    $site2 = $null
    try {
        $site2 = New-ProbeSite -Name "cfgfb-site" -Port $PortSite -ConfigText $engageOff
        $u2 = "http://localhost:$PortSite/"
        Recycle-AppPool -pool $AppPool
        Assert-PageSpeedState -u $u2 -wantPresent $false -desc "case 2: per-site 'off' wins while the base says 'on' (X-Page-Speed absent)"

        Write-ConfigText -Path $site2.ConfigPath -Text $engageOn
        Recycle-AppPool -pool $AppPool
        Assert-PageSpeedState -u $u2 -wantPresent $true -desc "case 2: flipping the per-site file to 'on' re-engages optimizing (X-Page-Speed present)"
    } finally {
        if ($null -ne $site2) { Remove-ProbeSite -Name $site2.Name }
    }

    # Case 3: the legacy location fallback. The loader only reaches the legacy
    # tier when the canonical tier holds no config of either name, so stash
    # both canonical filenames (plus any pre-existing legacy files, which an
    # upgrade box may carry), then plant legacy pagespeed.config with 'off'.
    $legacyDir = Split-Path -Parent $LegacyConfig
    $legacyDirExisted = Test-Path -LiteralPath $legacyDir
    $site3 = $null
    try {
        Stash-ConfigFile -Path $CanonicalConfig
        Stash-ConfigFile -Path $CanonicalFallbackConfig
        Stash-ConfigFile -Path $LegacyConfig
        Stash-ConfigFile -Path $LegacyFallbackConfig
        New-Item -ItemType Directory -Path $legacyDir -Force | Out-Null
        Write-ConfigText -Path $LegacyConfig -Text $canonicalOff
        Write-Host "[config-fallback] planted legacy base with 'pagespeed off' at $LegacyConfig (canonical tier absent)"

        $site3 = New-ProbeSite -Name "cfgfb-legacy" -Port $PortLegacy -ConfigText $engageConfig
        $u3 = "http://localhost:$PortLegacy/"
        Recycle-AppPool -pool $AppPool
        Assert-PageSpeedState -u $u3 -wantPresent $false -desc "case 3: legacy base 'off' governs an engage-only site (X-Page-Speed absent)"

        Write-ConfigText -Path $LegacyConfig -Text $canonicalContent
        Recycle-AppPool -pool $AppPool
        Assert-PageSpeedState -u $u3 -wantPresent $true -desc "case 3: flipping the legacy base to 'on' re-engages optimizing (X-Page-Speed present)"
    } finally {
        if ($null -ne $site3) { Remove-ProbeSite -Name $site3.Name }
        Restore-StashedConfigs
        # Drop the legacy config directory itself when this case created it
        # and restoring left it empty; an upgrade box already had it, and any
        # file it held came back above.
        if (-not $legacyDirExisted -and (Test-Path -LiteralPath $legacyDir) -and
            -not (Get-ChildItem -LiteralPath $legacyDir -Force)) {
            Remove-Item -LiteralPath $legacyDir -Force -ErrorAction SilentlyContinue
            Write-Host "[config-fallback] removed empty legacy config dir $legacyDir"
        }
    }

    Write-Host "[config-fallback] ALL CASES PASSED"
} finally {
    # Even on failure: recycle with everything restored and confirm the
    # default site is back to its healthy optimizing state. Warning-only, so
    # a cleanup problem cannot mask the original failure.
    Recycle-AppPool -pool $AppPool
    if (-not (Wait-ForPageSpeedState -u $Url -wantPresent $true -timeoutSec $PollSeconds)) {
        Write-Host "::warning::default site at $Url did not return to a healthy optimizing state after cleanup"
    }
    # Drop the probe-site root too when this run created it and no probe
    # directory is left below it (a locked handle can leave one, see
    # Remove-ProbeSite).
    if (-not $siteRootExisted -and (Test-Path -LiteralPath $SiteRoot) -and
        -not (Get-ChildItem -LiteralPath $SiteRoot -Force)) {
        Remove-Item -LiteralPath $SiteRoot -Force -ErrorAction SilentlyContinue
        Write-Host "[config-fallback] removed empty probe-site root $SiteRoot"
    }
}

exit 0
