# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# run-el9-selinux-rehearsal.ps1 - drive one enforcing-SELinux EL9 run of the
# 1.15 -> 1.16 upgrade rehearsal on the Hyper-V VM provision-el9-vm.ps1 made.
#
# Flow (the IIS and cPanel rigs' shape, for a Linux guest):
#   1. Restore-VMSnapshot -Snapshot ('00-fresh'), Start-VM, find the guest IP
#      by MAC on the Default Switch, wait for root SSH.
#   2. Stage the payload on the guest (-GuestDir): the guest driver
#      (el9-selinux-rehearsal.sh), the fixture image, the 1.16 rpm pair when
#      the upgrade source is local, the draft SELinux policy sources when
#      -Policy draft.
#   3. Run the driver over SSH; its stdout streams into the console and into
#      <artifacts>\guest-run.log.
#   4. scp the guest's artifacts directory back (ausearch/audit2allow/sealert
#      dumps, daemon journal, httpd dumps, labels).
#   5. Stop the VM and restore -Snapshot again so the next run starts fresh
#      (-KeepRunning skips this for a hands-on look).
#
# Upgrade source, one of:
#   -PackagesDir <dir>   exactly one pagespeed-optimizer-*.rpm and one
#                        mod-pagespeed-*.rpm (rc.14 candidates, or locally
#                        built with install/rpm/build.sh). -Rc is optional
#                        (the guest derives it from the module rpm).
#   -RepoUrl <url>       upgrade from a package repository (the staging
#                        endpoint). -Rc required. Credentials, when the
#                        endpoint needs them, come from PACKAGES_USER /
#                        PACKAGES_PASS in the environment; they travel to the
#                        guest in a 0600 env file, never on a command line.
#   (neither)            download the pair for tag v<Rc> from the GitHub
#                        release with gh (authenticated), sha256-verified
#                        against the release's SHA256SUMS. No gpg check on
#                        this path (the container rehearsal does that).
#
# Usage:
#   .\run-el9-selinux-rehearsal.ps1 -Rc 1.16.0-rc.14
#   .\run-el9-selinux-rehearsal.ps1 -PackagesDir D:\pkgs\rc14
#   .\run-el9-selinux-rehearsal.ps1 -PackagesDir D:\pkgs\rc14 -Policy draft -PolicySrc <daemon repo>\deploy\selinux
#   .\run-el9-selinux-rehearsal.ps1 -Rc 1.16.0-rc.14 -RepoUrl https://.../staging
#   .\run-el9-selinux-rehearsal.ps1 ... -DryRun     # stage + print, no Hyper-V, no ssh
#
# Exit code: the guest driver's (0 PASS, 1 FAIL, 2 usage/preflight), or 1 on
# a runner-side failure.

param(
    [string]$VMName = 'EL9-SELinux-rehearsal',
    [string]$Snapshot = '00-fresh',
    [string]$Rc = '',
    [string]$PackagesDir = '',
    [string]$RepoUrl = '',
    [string]$ReleaseRepo = 'We-Amp/mod_pagespeed',
    [ValidateSet('none','draft')][string]$Policy = 'none',
    [string]$PolicySrc = '',
    [ValidateSet('rw-content','none')][string]$BaselineSelinux = 'rw-content',
    [switch]$DisableDontaudit,
    [string]$WorkDir = (Join-Path $env:TEMP 'el9-selinux-rehearsal'),
    [string]$ArtifactsDir = '',
    [string]$SshKeyPath = (Join-Path $env:USERPROFILE '.ssh\id_ed25519'),
    [string]$SwitchName = 'Default Switch',
    [string]$GuestDir = '/tmp/el9-rehearsal',
    [switch]$KeepRunning,
    [switch]$DryRun
)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'hv-common.ps1')

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $ArtifactsDir) { $ArtifactsDir = Join-Path $WorkDir "artifacts\$stamp" }
New-Item -ItemType Directory -Path $ArtifactsDir -Force | Out-Null
$transcript = Join-Path $ArtifactsDir 'runner-transcript.log'
try { Start-Transcript -Path $transcript -Force | Out-Null } catch { }

function Fail([string]$msg) { Write-Host "::error::$msg"; throw $msg }

# A trap applies to its whole scope, however far down it is written, so it
# and everything it calls are defined before the first thing that can throw.
# The VM is only touched (and so only restored) once $script:VmTouched is set.
$script:VmTouched = $false
function Restore-CleanState {
    if ($KeepRunning) { Write-Host "(-KeepRunning: VM left as is)"; return }
    Stop-VmHard -VMName $VMName
    Restore-VMSnapshot -VMName $VMName -Name $Snapshot -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "VM restored to '$Snapshot'"
}
trap {
    Write-Host "::error::runner failed: $_"
    if ($script:VmTouched) { Restore-CleanState }
    Remove-SshHelpers
    try { Stop-Transcript | Out-Null } catch { }
    exit 1
}

Write-Host "=== el9-selinux upgrade rehearsal: $VMName @ $Snapshot ==="

# --- Inputs -----------------------------------------------------------------
$repoRoot   = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$driver     = Join-Path $PSScriptRoot 'el9-selinux-rehearsal.sh'
$fixtureImg = Join-Path $repoRoot 'install\mod_pagespeed_example\images\Puzzle.jpg'
foreach ($p in @($driver, $fixtureImg)) { if (-not (Test-Path -LiteralPath $p)) { Fail "missing: $p" } }
if (-not (Test-Path -LiteralPath $SshKeyPath)) { Fail "SSH private key not found: $SshKeyPath (pass -SshKeyPath)" }
if ($PackagesDir -and $RepoUrl) { Fail "-PackagesDir and -RepoUrl are mutually exclusive" }
if ($Rc -and $Rc -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$') { Fail "-Rc '$Rc' is not X.Y.Z[-prerelease]" }
if ($RepoUrl -and -not $Rc) { Fail "-RepoUrl needs -Rc (the version under test, for the header assertion)" }
if ($Policy -eq 'draft') {
    if (-not $PolicySrc) { Fail "-Policy draft needs -PolicySrc <dir with pagespeed-optimizer.te + .fc> (the daemon repo's deploy\selinux)" }
    foreach ($f in @('pagespeed-optimizer.te','pagespeed-optimizer.fc')) {
        if (-not (Test-Path -LiteralPath (Join-Path $PolicySrc $f))) { Fail "missing in -PolicySrc: $f" }
    }
}

# --- Upgrade source ---------------------------------------------------------
$source = ''
if ($PackagesDir) {
    if (-not (Test-Path -LiteralPath $PackagesDir)) { Fail "-PackagesDir not found: $PackagesDir" }
    $source = 'packages-dir'
} elseif ($RepoUrl) {
    $source = 'repo-url'
} else {
    if (-not $Rc) { Fail "pass -Rc <version> (release download), -PackagesDir <dir> or -RepoUrl <url>" }
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { Fail "gh not found; needed to download the release assets (or pass -PackagesDir)" }
    $source = 'github-release'
    $PackagesDir = Join-Path $WorkDir "pkgs-v$Rc"
}
function Assert-PackagePair([string]$dir) {
    $opt = @(Get-ChildItem -LiteralPath $dir -Filter 'pagespeed-optimizer-*.rpm' -File -ErrorAction SilentlyContinue)
    $mod = @(Get-ChildItem -LiteralPath $dir -Filter 'mod-pagespeed-*.rpm' -File -ErrorAction SilentlyContinue)
    if ($opt.Count -ne 1) { Fail "expected exactly one pagespeed-optimizer-*.rpm in $dir, found $($opt.Count)" }
    if ($mod.Count -ne 1) { Fail "expected exactly one mod-pagespeed-*.rpm in $dir, found $($mod.Count)" }
    return @($opt[0], $mod[0])
}
if ($source -eq 'github-release') {
    New-Item -ItemType Directory -Path $PackagesDir -Force | Out-Null
    Get-ChildItem -LiteralPath $PackagesDir -File -ErrorAction SilentlyContinue | Remove-Item -Force
    # GitHub spells the package version's '~' as '.' in asset names; the
    # patterns are the container rehearsal's, verbatim.
    $pkgvSan = $Rc -replace '-', '.'
    $patterns = @("pagespeed-optimizer-$Rc-1.x86_64.rpm", "mod-pagespeed-$pkgvSan-*.x86_64.rpm", 'SHA256SUMS')
    Write-Host "Downloading v$Rc assets from $ReleaseRepo into $PackagesDir ..."
    if (-not $DryRun) {
        foreach ($p in $patterns) {
            & gh release download "v$Rc" -R $ReleaseRepo -D $PackagesDir --clobber -p $p 2>&1 | ForEach-Object { "$_" } | Write-Host
            if ($LASTEXITCODE -ne 0) { Fail "gh release download failed for pattern '$p' (does v$Rc carry x86_64 rpm assets?)" }
        }
        $sums = Get-Content -LiteralPath (Join-Path $PackagesDir 'SHA256SUMS')
        foreach ($rpm in (Assert-PackagePair $PackagesDir)) {
            $have = (Get-FileHash -LiteralPath $rpm.FullName -Algorithm SHA256).Hash.ToLower()
            $want = ''
            foreach ($line in $sums) {
                $parts = $line -split '\s+', 2
                if ($parts.Count -eq 2) {
                    $n = $parts[1].Trim().TrimStart('*')
                    if ($n -eq $rpm.Name -or ($n -replace '~', '.') -eq $rpm.Name) { $want = $parts[0].ToLower() }
                }
            }
            if (-not $want) { Fail "no SHA256SUMS entry for $($rpm.Name)" }
            if ($have -ne $want) { Fail "sha256 mismatch for $($rpm.Name): want $want have $have" }
            Write-Host "sha256 OK: $($rpm.Name)"
        }
    }
}
$pair = $null
if ($PackagesDir -and (-not $DryRun -or $source -eq 'packages-dir')) { $pair = Assert-PackagePair $PackagesDir }

# --- Stage the payload locally ---------------------------------------------
$payload = Join-Path $WorkDir 'payload'
if (Test-Path -LiteralPath $payload) { Remove-Item -LiteralPath $payload -Recurse -Force }
New-Item -ItemType Directory -Path $payload -Force | Out-Null
Copy-Item -LiteralPath $driver -Destination (Join-Path $payload 'el9-selinux-rehearsal.sh')
Copy-Item -LiteralPath $fixtureImg -Destination (Join-Path $payload 'Puzzle.jpg')
if ($pair) {
    New-Item -ItemType Directory -Path (Join-Path $payload 'pkgs') -Force | Out-Null
    foreach ($rpm in $pair) { Copy-Item -LiteralPath $rpm.FullName -Destination (Join-Path $payload 'pkgs') }
}
if ($Policy -eq 'draft') {
    New-Item -ItemType Directory -Path (Join-Path $payload 'selinux') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PolicySrc 'pagespeed-optimizer.te') -Destination (Join-Path $payload 'selinux')
    Copy-Item -LiteralPath (Join-Path $PolicySrc 'pagespeed-optimizer.fc') -Destination (Join-Path $payload 'selinux')
}
$credFile = ''
if ($source -eq 'repo-url' -and $env:PACKAGES_USER) {
    $credFile = Join-Path $payload 'repo-credentials.env'
    Write-Utf8NoBomLf -Path $credFile -Text "PACKAGES_USER='$($env:PACKAGES_USER)'`nPACKAGES_PASS='$($env:PACKAGES_PASS)'`n"
}

# Guest command line.
$guestArgs = @("--policy $Policy", "--baseline-selinux $BaselineSelinux", "--artifacts $GuestDir/artifacts")
if ($Rc) { $guestArgs += "--rc $Rc" }
if ($source -eq 'repo-url') { $guestArgs += "--repo-url '$RepoUrl'" }
if ($DisableDontaudit) { $guestArgs += '--disable-dontaudit' }
$guestCmd = "bash $GuestDir/el9-selinux-rehearsal.sh $($guestArgs -join ' ')"
if ($credFile) { $guestCmd = "set -a; . $GuestDir/repo-credentials.env; set +a; chmod 0600 $GuestDir/repo-credentials.env; $guestCmd" }

# --- Plan -------------------------------------------------------------------
Write-Host "VM / snapshot:   $VMName / $Snapshot"
Write-Host "Upgrade source:  $source$(if ($PackagesDir) { " ($PackagesDir)" } elseif ($RepoUrl) { " ($RepoUrl)" })"
if ($pair) { foreach ($rpm in $pair) { Write-Host "                 $($rpm.Name)" } }
Write-Host "Version (-Rc):   $(if ($Rc) { $Rc } else { '(derived from the module rpm on the guest)' })"
Write-Host "Policy:          $Policy$(if ($PolicySrc) { " ($PolicySrc)" })"
Write-Host "Baseline SELinux: $BaselineSelinux; dontaudit $(if ($DisableDontaudit) { 'disabled' } else { 'as shipped' })"
Write-Host "Payload:         $payload -> $GuestDir"
Write-Host "Artifacts:       $ArtifactsDir"
$guestCmdMasked = $guestCmd -replace "PACKAGES_PASS='[^']*'", "PACKAGES_PASS='***'"
Write-Host "Guest command:   $guestCmdMasked"
if ($DryRun) {
    Write-Host "dry run: nothing executed (no Hyper-V, no ssh)"
    if ($credFile) { Remove-Item -LiteralPath $credFile -Force }
    try { Stop-Transcript | Out-Null } catch { }
    exit 0
}

# --- VM + snapshot must exist (hard fail) -----------------------------------
$vm = Get-VM -Name $VMName -ErrorAction SilentlyContinue
if (-not $vm) { Fail "Hyper-V VM '$VMName' not found on this host -- run provision-el9-vm.ps1 first (see README.md)" }
if (-not (Get-VMSnapshot -VMName $VMName -Name $Snapshot -ErrorAction SilentlyContinue)) {
    Fail "checkpoint '$Snapshot' not found on $VMName -- provision-el9-vm.ps1 creates it"
}

Initialize-SshHelpers
$exitCode = 1
try {
    # --- 1. Revert + start ---
    Write-Host "=== Revert to '$Snapshot' and start ==="
    $script:VmTouched = $true
    Restore-VMSnapshot -VMName $VMName -Name $Snapshot -Confirm:$false -ErrorAction Stop
    Start-VM -Name $VMName -ErrorAction Stop
    $mac = (Get-VMNetworkAdapter -VMName $VMName -ErrorAction Stop | Select-Object -First 1).MacAddress
    Write-Host "Waiting for guest IP (MAC $mac) ..."
    $ip = Get-GuestIp -Mac $mac -InterfaceAlias "*$SwitchName*"
    Write-Host "Guest IP: $ip"
    Wait-Ssh -Ip $ip -KeyPath $SshKeyPath | Out-Null
    Write-Host "SSH ready (root@$ip)"

    # --- 2. Stage ---
    Write-Host "=== Staging payload in $GuestDir ==="
    $rc = Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -Command "rm -rf $GuestDir"
    if ($rc -ne 0) { Fail "could not clear $GuestDir on the guest (exit $rc)" }
    # scp -r <dir> <missing remote dir> copies the CONTENTS under that name.
    $rc = Invoke-Scp -Recurse -KeyPath $SshKeyPath -ConnectTimeoutSec 30 -Source $payload -Destination "root@${ip}:$GuestDir"
    if ($rc -ne 0) { Fail "scp of the payload failed (exit $rc)" }
    if ($credFile) { Remove-Item -LiteralPath $credFile -Force -ErrorAction SilentlyContinue }
    [void](Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -Command "ls -la $GuestDir $GuestDir/pkgs $GuestDir/selinux 2>/dev/null; getenforce")

    # --- 3. Run ---
    Write-Host "=== Running the guest driver ==="
    $guestLog = Join-Path $ArtifactsDir 'guest-run.log'
    $exitCode = Invoke-Ssh -Ip $ip -KeyPath $SshKeyPath -ConnectTimeoutSec 30 -Command $guestCmd -LogFile $guestLog
    Write-Host "guest driver exit code: $exitCode"

    # --- 4. Collect ---
    Write-Host "=== Collecting artifacts ==="
    $rc = Invoke-Scp -Recurse -KeyPath $SshKeyPath -ConnectTimeoutSec 30 -Source "root@${ip}:$GuestDir/artifacts" -Destination $ArtifactsDir
    if ($rc -ne 0) { Write-Host "::warning::scp of the guest artifacts failed (exit $rc)" }
    Get-ChildItem -LiteralPath $ArtifactsDir -Recurse -File | Select-Object FullName, Length | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

    # --- 5. Reset ---
    Restore-CleanState
    if ($exitCode -eq 0) {
        Write-Host "=== el9-selinux upgrade rehearsal PASSED ($Policy policy) ==="
    } else {
        Write-Host "::error::el9-selinux upgrade rehearsal FAILED (guest exit $exitCode) -- see $guestLog and $ArtifactsDir\artifacts"
    }
} finally {
    Remove-SshHelpers
    try { Stop-Transcript | Out-Null } catch { }
}
exit $exitCode
