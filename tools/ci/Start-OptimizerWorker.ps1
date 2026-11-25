# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Runs the pinned optimizer worker for the IIS system tests, and stops it.

.DESCRIPTION
    start: takes factory_worker.exe and pagespeed.dll built from the optimizer
    commit the test pin names (test\system\optimizer-test-pin.json, or the pin
    file PAGESPEED_OPTIMIZER_PIN names): the build directory -BuildDir names,
    checked with tools\ci\Build-OptimizerPin.ps1 -Verify, or, without
    -BuildDir, the one Build-OptimizerPin.ps1 builds or reuses. With
    -FromRelease it fetches the optimizer release's Windows zip named by
    install\debian\optimizer-pin.json instead, through
    install\iis\fetch-optimizer-windows.ps1 (SHA256SUMS checked against the
    pin, the zip checked against SHA256SUMS). It logs the C API version the
    client library reports (ps_version_minor), starts factory_worker.exe as a
    background process with a cache volume under -Root and the management API on
    127.0.0.1:<ApiPort> behind a token generated for this run, waits for
    /v1/health, and exports to later steps of the job (GITHUB_ENV) and to the calling
    session (the process environment), so a script run right after it sees them:
        PAGESPEED_OPTIMIZER_API         host:port of the management API
        PAGESPEED_OPTIMIZER_API_TOKEN   the run's token (masked in the log)
        PAGESPEED_OPTIMIZER_VOLUME      the volume path (DaemonVolumePath)
        PAGESPEED_OPTIMIZER_SOCKET      the pipe name (DaemonSocketPath)
        PAGESPEED_OPTIMIZER_DIR         the worker and client library in use
                                        (setup_iis_full.ps1 copies pagespeed.dll
                                        next to the module)
    A fetch or start failure fails the step; nothing is skipped silently.

    stop: stops the worker this script started (the process is checked to still
    be a factory_worker.exe from -Root before it is signalled) and removes the
    pagespeed.dll that setup_iis_full.ps1 placed beside the IIS module.

.PARAMETER BuildDir
    A build directory of tools\ci\Build-OptimizerPin.ps1 for the pinned commit.
    It is checked again before use; one that does not check out fails the step.

.PARAMETER FromRelease
    Use the released optimizer that install\debian\optimizer-pin.json names
    instead of a build from the test pin.

.PARAMETER LocalRelease
    Testing only: runs a locally built worker; refused under GitHub Actions.
    A directory holding factory_worker.exe, pagespeed.dll and
    optimizer-commit.txt (the one optimizer commit they were built from). The
    two binaries are copied into the release directory under -Root in place of
    the pinned release, which is then not fetched.
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('start', 'stop')][string]$Action,
    [string]$Root = "C:\pagespeed_optimizer",
    [int]$ApiPort = 18765,
    [string]$Socket = "pagespeed-optimizer",
    [string]$BuildDir,
    [switch]$FromRelease,
    [string]$LocalRelease
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$PidFile = Join-Path $Root "worker.pid"
$WorkerDir = Join-Path $Root "release"
$VolumeDir = Join-Path $Root "volume"
$Volume = Join-Path $VolumeDir "cache"
$ModuleDll = "C:\Windows\System32\inetsrv\pagespeed.dll"

function Set-JobEnv([string]$Name, [string]$Value) {
    if ($env:GITHUB_ENV) {
        "$Name=$Value" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    }
    Set-Item -Path "Env:$Name" -Value $Value
}

function Stop-Worker {
    if (Test-Path -LiteralPath $PidFile) {
        $workerPid = 0
        try { $workerPid = [int](Get-Content -LiteralPath $PidFile -TotalCount 1) } catch { }
        $proc = if ($workerPid -gt 0) { Get-Process -Id $workerPid -ErrorAction SilentlyContinue } else { $null }
        if ($workerPid -gt 0 -and -not $proc) {
            Write-Host "the optimizer worker (pid $workerPid) was already gone"
        }
        # Only a worker of this lane: the number may have been reused since.
        if ($proc -and $proc.ProcessName -eq 'factory_worker' -and
            $proc.Path -and $proc.Path.StartsWith($Root, [System.StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $workerPid -Force -ErrorAction SilentlyContinue
            [void]$proc.WaitForExit(15000)
            Write-Host "stopped the optimizer worker (pid $workerPid)"
        }
        Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
    }
}

if ($Action -eq 'stop') {
    Stop-Worker
    Remove-Item -LiteralPath $ModuleDll -Force -ErrorAction SilentlyContinue
    exit 0
}

# A locally built worker, for testing on a lab host only. Checked before
# anything is stopped or started.
$localCommit = $null
if ($LocalRelease) {
    if ($env:GITHUB_ACTIONS) {
        Write-Host "::error::-LocalRelease is for testing only and is refused under GitHub Actions; no worker was started"
        exit 1
    }
    foreach ($name in @('factory_worker.exe', 'pagespeed.dll', 'optimizer-commit.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $LocalRelease $name) -PathType Leaf)) {
            Write-Host "::error::-LocalRelease $LocalRelease has no $name; no worker was started"
            exit 1
        }
    }
    $lines = @(Get-Content -LiteralPath (Join-Path $LocalRelease 'optimizer-commit.txt') |
        ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($lines.Count -ne 1 -or $lines[0] -cnotmatch '^[0-9a-f]{40}$') {
        Write-Host "::error::optimizer-commit.txt in $LocalRelease must hold exactly one 40-hex commit; no worker was started"
        exit 1
    }
    $localCommit = $lines[0]
    Write-Host "using the locally built optimizer worker from $LocalRelease (optimizer commit $localCommit)"
}

# --- start ---
Stop-Worker
if (Get-NetTCPConnection -State Listen -LocalPort $ApiPort -ErrorAction SilentlyContinue) {
    Write-Host "::error::port $ApiPort is already in use on this host; the optimizer worker needs it for its management API"
    exit 1
}

New-Item -ItemType Directory -Force -Path $Root | Out-Null

# Every run starts on an empty volume: no variants, generation or persisted
# worker settings from an earlier run. Failing to clear it fails the step.
if (Test-Path -LiteralPath $VolumeDir) {
    try {
        Remove-Item -LiteralPath $VolumeDir -Recurse -Force -ErrorAction Stop
    } catch {
        Write-Host "::error::could not clear the optimizer volume directory from an earlier run: $($_.Exception.Message)"
        exit 1
    }
    if (Test-Path -LiteralPath $VolumeDir) {
        Write-Host "::error::the optimizer volume directory from an earlier run is still there after removal"
        exit 1
    }
}

if ($localCommit) {
    # The two binaries take the place of the fetched release; nothing is
    # fetched.
    try {
        New-Item -ItemType Directory -Force -Path $WorkerDir | Out-Null
        foreach ($name in @('factory_worker.exe', 'pagespeed.dll')) {
            Copy-Item -LiteralPath (Join-Path $LocalRelease $name) -Destination (Join-Path $WorkerDir $name) -Force -ErrorAction Stop
        }
    } catch {
        Write-Host "::error::could not copy the locally built worker into $WorkerDir`: $($_.Exception.Message)"
        exit 1
    }
} elseif ($FromRelease) {
    $fetch = Join-Path $RepoRoot "install\iis\fetch-optimizer-windows.ps1"
    $rc = 1
    try {
        & $fetch -OutDir $WorkerDir
        $rc = $LASTEXITCODE
    } catch {
        Write-Host "fetch failed: $($_.Exception.Message)"
    }
    if ($rc -ne 0) {
        Write-Host "::error::could not fetch the pinned optimizer release for Windows (fetch-optimizer-windows.ps1 exit $rc); the IIS system tests need the worker, so this is a failure, not a skip"
        exit 1
    }
    $abiCommit = "$((Get-Content -LiteralPath (Join-Path $RepoRoot 'install\debian\optimizer-pin.json') -Raw | ConvertFrom-Json).optimizer_commit)"
} else {
    # The build from the test pin: the directory handed over, checked again,
    # or the one the lane builds or reuses now.
    $lane = Join-Path $RepoRoot "tools\ci\Build-OptimizerPin.ps1"
    if ($BuildDir) {
        & $lane -Verify $BuildDir
        if ($LASTEXITCODE -ne 0) {
            Write-Host "::error::the optimizer build handed to this step does not check out; no worker was started"
            exit 1
        }
    } else {
        $BuildDir = & $lane | Select-Object -Last 1
        if ($LASTEXITCODE -ne 0 -or -not $BuildDir) {
            Write-Host "::error::no optimizer build at the test pin; the IIS system tests need the worker, so this is a failure, not a skip"
            exit 1
        }
    }
    try {
        if (Test-Path -LiteralPath $WorkerDir) { Remove-Item -LiteralPath $WorkerDir -Recurse -Force -ErrorAction Stop }
        New-Item -ItemType Directory -Force -Path $WorkerDir | Out-Null
        foreach ($name in @('factory_worker.exe', 'pagespeed.dll', 'optimizer-commit.txt')) {
            Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination (Join-Path $WorkerDir $name) -Force -ErrorAction Stop
        }
        # The copies are the job's own: the IIS worker processes read them
        # and the stop action removes them.
        Get-ChildItem -LiteralPath $WorkerDir -File | ForEach-Object { $_.IsReadOnly = $false }
        # What runs is what was checked: each copy against the build's
        # SHA256SUMS.
        foreach ($line in @(Get-Content -LiteralPath (Join-Path $BuildDir 'SHA256SUMS') | Where-Object { $_ -ne '' })) {
            if ($line -cnotmatch '^([0-9a-f]{64})  ([A-Za-z0-9._-]+)$') { throw "malformed SHA256SUMS line" }
            $copy = Join-Path $WorkerDir $Matches[2]
            if ((Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Matches[1]) {
                throw "the copy of $($Matches[2]) does not match the build's SHA256SUMS"
            }
        }
    } catch {
        Write-Host "::error::could not copy the pinned optimizer build into $WorkerDir`: $($_.Exception.Message)"
        exit 1
    }
    $abiCommit = "$(Get-Content -LiteralPath (Join-Path $WorkerDir 'optimizer-commit.txt') -TotalCount 1)".Trim()
    Write-Host "using the optimizer worker built from the test pin (optimizer commit $abiCommit)"
}
if ($localCommit) { $abiCommit = $localCommit }
$worker = Join-Path $WorkerDir "factory_worker.exe"

# The C API version the client library reports, so a run shows which library
# it loaded.
$python = Get-Command python -CommandType Application -ErrorAction SilentlyContinue |
    Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1
if (-not $python) {
    Write-Host "::error::no usable python on PATH to read the client library's C API version"
    exit 1
}
$abi = & {
    $ErrorActionPreference = 'Continue'
    "$(& $python.Source -I -c 'import ctypes,sys; l=ctypes.CDLL(sys.argv[1]); print(str(l.ps_version_major())+chr(46)+str(l.ps_version_minor()))' (Join-Path $WorkerDir 'pagespeed.dll') 2>$null)".Trim()
}
if ($abi -notmatch '^[0-9]+\.([0-9]+)$') {
    Write-Host "::error::cannot read the C API version of $(Join-Path $WorkerDir 'pagespeed.dll')"
    exit 1
}
$short = if ($abiCommit.Length -ge 12) { $abiCommit.Substring(0, 12) } else { $abiCommit }
Write-Host "Optimizer client library at $short`: C API $abi (ps_version_minor $($Matches[1]))"

# The volume directory: the IIS worker processes read the volume and record
# into it, so they may modify what the worker creates there.
New-Item -ItemType Directory -Force -Path $VolumeDir | Out-Null
& icacls.exe $VolumeDir /grant "IIS_IUSRS:(OI)(CI)M" | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "::error::could not grant IIS_IUSRS modify on the optimizer volume directory"
    exit 1
}

# A token for this run only, never written to a file in the repository.
$bytes = New-Object byte[] 32
$rng = New-Object System.Security.Cryptography.RNGCryptoServiceProvider
$rng.GetBytes($bytes)
$rng.Dispose()
$token = ($bytes | ForEach-Object { $_.ToString('x2') }) -join ''
Write-Host "::add-mask::$token"

$evidence = if ($env:PAGESPEED_EVIDENCE_DIR) { $env:PAGESPEED_EVIDENCE_DIR } else { $Root }
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$outLog = Join-Path $evidence "optimizer-worker.out.log"
$errLog = Join-Path $evidence "optimizer-worker.err.log"

# The token travels in the environment, not on the command line.
$env:PAGESPEED_API_TOKEN = $token
try {
    $proc = Start-Process -FilePath $worker -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
        -ArgumentList @(
            "--socket", $Socket,
            "--cache-dir", ('"' + $VolumeDir + '"'),
            "--cache-size", "268435456",
            "--api-bind", "127.0.0.1",
            "--api-port", "$ApiPort",
            "--log-level", "info")
} finally {
    Remove-Item Env:PAGESPEED_API_TOKEN -ErrorAction SilentlyContinue
}
$proc.Id | Out-File -FilePath $PidFile -Encoding ascii

# Wait for the management API.
$healthy = $false
for ($i = 0; $i -lt 60; $i++) {
    if ($proc.HasExited) { break }
    try {
        $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 `
            -Uri "http://127.0.0.1:$ApiPort/v1/health" `
            -Headers @{ Authorization = "Bearer $token" }
        if ($r.StatusCode -eq 200) { $healthy = $true; break }
    } catch { }
    Start-Sleep -Milliseconds 500
}
if (-not $healthy) {
    Write-Host "::error::the optimizer worker did not answer /v1/health (exited: $($proc.HasExited))"
    foreach ($f in @($outLog, $errLog)) {
        if (Test-Path -LiteralPath $f) { Get-Content -LiteralPath $f -Tail 40 }
    }
    Stop-Worker
    exit 1
}
Write-Host "optimizer worker up (pid $($proc.Id)), management API on 127.0.0.1:$ApiPort"

Set-JobEnv 'PAGESPEED_OPTIMIZER_API' "127.0.0.1:$ApiPort"
Set-JobEnv 'PAGESPEED_OPTIMIZER_API_TOKEN' $token
Set-JobEnv 'PAGESPEED_OPTIMIZER_VOLUME' $Volume
Set-JobEnv 'PAGESPEED_OPTIMIZER_SOCKET' $Socket
Set-JobEnv 'PAGESPEED_OPTIMIZER_DIR' $WorkerDir
exit 0
