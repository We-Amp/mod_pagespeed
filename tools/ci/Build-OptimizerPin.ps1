# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Builds (or reuses) the Windows optimizer worker and client library at the
    optimizer commit the test pin names, for the IIS system tests.

.DESCRIPTION
    The Windows counterpart of test/system/optimizer_lane.sh. It reads
    optimizer_commit from the pin file (default test\system\optimizer-test-pin.json;
    PAGESPEED_OPTIMIZER_PIN or -Pin names another, such as the release pin
    install\debian\optimizer-pin.json) and provides factory_worker.exe and
    pagespeed.dll built from the optimizer's public source at that commit, with
    MSVC and Bazel from the host, in the optimizer's optimized release
    configuration (--config=windows --config=opt) and with GIT_COMMIT set to
    the pinned commit, so the worker reports the commit it was built from.

    Builds are kept under -LaneDir (CI_OPTIMIZER_LANE_DIR, default
    <system drive>\pagespeed_optimizer_lane), one directory per pin and recipe,
    named <commit12>-<recipe8>. The recipe hash covers this script, the build
    configuration, the cache-library commit this module links, the MSVC
    toolset, the Windows SDK,
    CMake and NASM, so a change to any of them builds again instead of reusing
    an older build. Each directory carries SHA256SUMS and optimizer-commit.txt.
    A directory is reused only when:
      - both files check out and it holds exactly the three files listed;
      - it and its files are owned by the current account, SYSTEM or
        Administrators, and no other account may write to them;
      - none of them is a reparse point (link or junction).
    A directory that fails the check is left in place; the build then goes to
    a fresh directory of its own. A new build is staged and published in one
    rename. One build runs per host at a time; the lock waits at most 15
    minutes. The work tree, the Bazel output root and the tool PATH are the
    same for every run on a host, so Bazel's caches (and the remote cache, when
    BAZEL_REMOTE_CACHE_URL names one) can hit.

    Before a build, the pinned optimizer's MODULE.bazel is checked to link the
    same cache library as this module (tools\ci\check_optimizer_pin_pair.py).

    On success the build directory is printed as the last line, written to
    -ResultFile when given, and exported as PAGESPEED_OPTIMIZER_BUILD_DIR to
    later steps of a GitHub Actions job.

    -Verify DIR only checks DIR against the pin (exit 0 when it checks out),
    prints the checked hashes ("<sha256>  <name>"), and builds nothing; with
    -CopyTo it also copies the build out and checks the copies against those
    hashes.

    The lane directory itself is checked on every run: it must be owned by
    this account, SYSTEM or Administrators, and is given the owner-only ACL
    again when another account may write to it; a lane directory owned by
    anyone else is refused.

.PARAMETER Pin
    The pin file (default: PAGESPEED_OPTIMIZER_PIN, else the test pin).
.PARAMETER LaneDir
    Where builds are kept (default: CI_OPTIMIZER_LANE_DIR, else
    <system drive>\pagespeed_optimizer_lane).
.PARAMETER LogFile
    The Bazel log (default: <LaneDir>\build.log).
.PARAMETER ResultFile
    A file the build directory is written to.
.PARAMETER Verify
    Only check this build directory against the pin.
.PARAMETER CopyTo
    With -Verify: copy the checked build's files into this directory (emptied
    first) and check each copy against the hashes the check computed, so what
    is used is what was checked.
.PARAMETER CheckLaneDir
    Only check (create or repair) the lane directory itself, then stop.
.PARAMETER PrintBuildConfig
    Only print the Bazel configurations the build uses, one per line.
#>
param(
    [string]$Pin = "",
    [string]$LaneDir = "",
    [string]$LogFile = "",
    [string]$ResultFile = "",
    [string]$Verify = "",
    [string]$CopyTo = "",
    [switch]$CheckLaneDir,
    [switch]$PrintBuildConfig
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$OptimizerRepo = "https://github.com/We-Amp/pagespeed-optimizer.git"
$LaneFiles = @('factory_worker.exe', 'optimizer-commit.txt', 'pagespeed.dll')
# Minutes a run waits for another build on the same host. Below the CI
# step's limit (25 minutes), so this wait's own error is what a stuck build
# shows; a cold build of the pin took about 2 minutes on a build runner.
$LockMinutes = 15
$LockName = 'Global\pagespeed-optimizer-lane-build'
# The optimizer's own Windows release build configurations: an optimized
# worker, so the tests see release encode times, not a debug build's.
$BuildConfigs = @('--config=windows', '--config=opt')

# Rights that let an account change a file or directory, or what it holds.
$WriteMask = [int64](0x2 -bor 0x4 -bor 0x10 -bor 0x40 -bor 0x100 -bor 0x10000 -bor 0x40000 -bor 0x80000 -bor 0x10000000 -bor 0x40000000)
$SystemSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-18')
$AdminsSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')
$UsersSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-545')
$MySid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User

function Write-Info([string]$Message) { Write-Host "[INFO] $Message" }
function Write-Warn([string]$Message) { Write-Host "::warning::$Message" }

# The commit the pin file names: 40 lowercase hex digits, or an error.
function Get-PinCommit([string]$PinFile) {
    try {
        $doc = Get-Content -LiteralPath $PinFile -Raw | ConvertFrom-Json
    } catch {
        throw "cannot read the optimizer pin $PinFile`: $($_.Exception.Message)"
    }
    $commit = "$($doc.optimizer_commit)"
    if ($commit -cnotmatch '^[0-9a-f]{40}$') {
        throw "the optimizer pin $PinFile does not name a full commit hash"
    }
    return $commit
}

function Get-Sha256Hex([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# $null when the ACL of Item lets no one but Owner, SYSTEM and Administrators
# write to it, else the account that may.
function Get-ForeignWriter($Item, $Owner) {
    $acl = $Item.GetAccessControl('Access')
    foreach ($rule in $acl.GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -ne 'Allow') { continue }
        if (([int64]$rule.FileSystemRights -band $WriteMask) -eq 0) { continue }
        $sid = $rule.IdentityReference
        if ($sid -eq $Owner -or $sid -eq $SystemSid -or $sid -eq $AdminsSid) { continue }
        return $sid.Value
    }
    return $null
}

# $null when the build in Dir is complete, unaltered, for Commit, and belongs
# to OwnerSid alone; otherwise the reason it does not check out.
function Test-LaneBuild([string]$Dir, [string]$Commit, $OwnerSid) {
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return "no directory" }
    $dirItem = Get-Item -LiteralPath $Dir -Force
    if ($dirItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { return "the directory is a link" }
    $entries = @(Get-ChildItem -LiteralPath $Dir -Force | ForEach-Object { $_.Name } | Sort-Object)
    $want = @($LaneFiles + 'SHA256SUMS' | Sort-Object)
    if (($entries -join '|') -cne ($want -join '|')) { return "it holds $($entries -join ', ') instead of $($want -join ', ')" }
    $items = @($dirItem) + @($want | ForEach-Object { Get-Item -LiteralPath (Join-Path $Dir $_) -Force })
    foreach ($item in $items) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { return "$($item.Name) is a link" }
        if (-not $item.PSIsContainer -and $item -isnot [IO.FileInfo]) { return "$($item.Name) is not a file" }
        # An elevated administrator's files are owned by Administrators; an
        # account outside that group cannot give its files that owner.
        $owner = $item.GetAccessControl('Owner').GetOwner([System.Security.Principal.SecurityIdentifier])
        if ($owner -ne $OwnerSid -and $owner -ne $SystemSid -and $owner -ne $AdminsSid) {
            return "$($item.Name) is owned by $($owner.Value), not by this account, SYSTEM or Administrators"
        }
        $writer = Get-ForeignWriter $item $OwnerSid
        if ($writer) { return "$($item.Name) is writable by $writer" }
    }
    $lines = @(Get-Content -LiteralPath (Join-Path $Dir 'optimizer-commit.txt') | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($lines.Count -ne 1 -or $lines[0] -cne $Commit) { return "optimizer-commit.txt does not name $Commit" }
    $sums = @(Get-Content -LiteralPath (Join-Path $Dir 'SHA256SUMS') | Where-Object { $_ -ne '' })
    $listed = @()
    $script:LaneHashes = @{}
    foreach ($line in $sums) {
        if ($line -cnotmatch '^([0-9a-f]{64})  ([A-Za-z0-9._-]+)$') { return "SHA256SUMS has a malformed line" }
        $hash = $Matches[1]; $name = $Matches[2]
        if ($LaneFiles -cnotcontains $name) { return "SHA256SUMS lists $name" }
        if ((Get-Sha256Hex (Join-Path $Dir $name)) -cne $hash) { return "$name does not match SHA256SUMS" }
        $script:LaneHashes[$name] = $hash
        $listed += $name
    }
    if ((($listed | Sort-Object) -join '|') -cne ($LaneFiles -join '|')) { return "SHA256SUMS does not list exactly $($LaneFiles -join ', ')" }
    return $null
}

# Owner-only ACL: the owner, SYSTEM and Administrators may change it, users
# may read it.
function Set-LaneAcl([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer) {
        $sec = New-Object System.Security.AccessControl.DirectorySecurity
        $inherit = [System.Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit'
    } else {
        $sec = New-Object System.Security.AccessControl.FileSecurity
        $inherit = [System.Security.AccessControl.InheritanceFlags]'None'
    }
    $none = [System.Security.AccessControl.PropagationFlags]'None'
    $sec.SetOwner($MySid)
    $sec.SetAccessRuleProtection($true, $false)
    foreach ($sid in @($MySid, $SystemSid, $AdminsSid) | Select-Object -Unique) {
        $sec.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($sid, 'FullControl', $inherit, $none, 'Allow')))
    }
    $sec.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($UsersSid, 'ReadAndExecute', $inherit, $none, 'Allow')))
    $item.SetAccessControl($sec)
}

# Creates the lane directory with the owner-only ACL, or checks an existing
# one: a link, or an owner other than this account, SYSTEM or Administrators,
# is refused (a throw); an ACL that lets another account write, or that
# inherits from the parent, is replaced. A sound ACL is left as it is, since
# re-applying it would walk the kept work tree.
function Initialize-LaneDir([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) {
        New-Item -ItemType Directory -Force -Path $Dir | Out-Null
        Set-LaneAcl $Dir
        return
    }
    $item = Get-Item -LiteralPath $Dir -Force
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "the lane directory $Dir is not a plain directory"
    }
    $acl = $item.GetAccessControl()
    $owner = $acl.GetOwner([System.Security.Principal.SecurityIdentifier])
    if ($owner -ne $MySid -and $owner -ne $SystemSid -and $owner -ne $AdminsSid) {
        throw "the lane directory $Dir is owned by $($owner.Value), not by this account, SYSTEM or Administrators"
    }
    $writer = Get-ForeignWriter $item $owner
    if ($writer -or -not $acl.AreAccessRulesProtected) {
        Write-Info "the lane directory's ACL lets another account write ($writer) or inherits; resetting it"
        Set-LaneAcl $Dir
        $writer = Get-ForeignWriter (Get-Item -LiteralPath $Dir -Force) $MySid
        if ($writer) { throw "the lane directory $Dir is still writable by $writer" }
    }
}

# The directory of the first command Name on PATH, or $null.
function Get-ToolDir([string]$Name) {
    $cmd = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1
    if ($cmd) { return (Split-Path -Parent $cmd.Source) }
    return $null
}

function Get-PythonExe {
    $dir = Get-ToolDir 'python'
    if ($dir) { return (Join-Path $dir 'python.exe') }
    if ($env:CI_WIN_PYTHON_DIR -and (Test-Path -LiteralPath (Join-Path $env:CI_WIN_PYTHON_DIR 'python.exe'))) {
        return (Join-Path $env:CI_WIN_PYTHON_DIR 'python.exe')
    }
    throw "no usable python on PATH (see docs/ci/runner-environment.md, CI_WIN_PYTHON_DIR)"
}

# The build environment: tools found on PATH, put in a fixed order on a PATH
# of their own, then the Visual Studio developer environment on top. The same
# host gives the same PATH, INCLUDE and LIB on every run, which Bazel's action
# keys include.
function Get-BuildEnv {
    $bazelDir = Get-ToolDir 'bazelisk'
    $bazelExe = if ($bazelDir) { Join-Path $bazelDir 'bazelisk.exe' } else { $null }
    if (-not $bazelExe) {
        $bazelDir = Get-ToolDir 'bazel'
        if ($bazelDir) { $bazelExe = Join-Path $bazelDir 'bazel.exe' }
    }
    if (-not $bazelExe) { throw "neither bazelisk nor bazel is on PATH (see docs/ci/runner-environment.md, CI_WIN_TOOLS_DIR)" }
    $bash = $null
    foreach ($b in @($env:BAZEL_SH, (Join-Path $env:SystemDrive 'msys64\usr\bin\bash.exe'))) {
        if ($b -and (Test-Path -LiteralPath $b -PathType Leaf)) { $bash = $b; break }
    }
    if (-not $bash) { throw "no MSYS2 bash for Bazel (BAZEL_SH, or msys64 on the system drive)" }
    $msys = Split-Path -Parent $bash
    $gitDir = Get-ToolDir 'git'
    if (-not $gitDir) { throw "git is not on PATH" }
    $dirs = New-Object System.Collections.Generic.List[string]
    foreach ($d in @($msys, (Get-ToolDir 'ninja'), $bazelDir, $gitDir, (Get-ToolDir 'cmake'),
                     (Get-ToolDir 'nasm'), (Split-Path -Parent (Get-PythonExe)),
                     (Join-Path $env:SystemRoot 'System32'), $env:SystemRoot,
                     (Join-Path $env:SystemRoot 'System32\Wbem'),
                     (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0'))) {
        if ($d -and -not $dirs.Contains($d)) { $dirs.Add($d) }
    }
    $basePath = $dirs -join ';'

    $vsDir = $null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vsDir = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null |
            Select-Object -First 1
    }
    if (-not $vsDir -or -not (Test-Path -LiteralPath "$vsDir\VC\Auxiliary\Build\vcvars64.bat")) {
        throw "no Visual Studio installation with the C++ tools found"
    }
    $vcvars = "$vsDir\VC\Auxiliary\Build\vcvars64.bat"
    $saved = $env:PATH
    $vars = @{}
    try {
        $env:PATH = $basePath
        # Full path: an MSYS2 cmd on PATH would shadow System32\cmd.exe.
        & (Join-Path $env:SystemRoot 'System32\cmd.exe') /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
            if ($_ -match '^(PATH|INCLUDE|LIB|LIBPATH|VCToolsVersion|WindowsSDKVersion|VCINSTALLDIR)=(.*)$') { $vars[$Matches[1]] = $Matches[2] }
        }
    } finally {
        $env:PATH = $saved
    }
    if (-not $vars['PATH'] -or -not $vars['INCLUDE'] -or -not $vars['LIB']) { throw "vcvars64.bat gave no developer environment" }
    # MSYS2 and Ninja ahead of the developer PATH, as the optimizer's own
    # Windows build has them (its genrule CMake builds use them).
    $front = @($msys)
    $ninja = Get-ToolDir 'ninja'
    if ($ninja -and $ninja -ne $msys) { $front += $ninja }
    $vars['PATH'] = (($front -join ';') + ';' + $vars['PATH'])
    $vars['BAZEL_SH'] = $bash
    $vars['BAZEL_EXE'] = $bazelExe
    return $vars
}

# The recipe hash: everything besides the commit that shapes the build.
function Get-Recipe($BuildEnv) {
    $parts = New-Object System.Collections.Generic.List[string]
    $parts.Add((Get-Sha256Hex $PSCommandPath))
    $cyclone = Select-String -LiteralPath (Join-Path $RepoRoot 'bazel\repositories.bzl') -Pattern '^\s*CYCLONE_COMMIT\s*=' |
        Select-Object -First 1
    $parts.Add("$($cyclone.Line.Trim())")
    $parts.Add("configs=$($BuildConfigs -join ' ')")
    $parts.Add("msvc=$($BuildEnv['VCToolsVersion'])")
    $parts.Add("sdk=$($BuildEnv['WindowsSDKVersion'])")
    $saved = $env:PATH
    try {
        $env:PATH = $BuildEnv['PATH']
        foreach ($tool in 'cmake', 'nasm') {
            $v = ''
            try {
                $flag = if ($tool -eq 'nasm') { '-v' } else { '--version' }
                $v = "$(& $tool $flag 2>$null | Select-Object -First 1)".Trim()
            } catch { }
            $parts.Add("$tool=$v")
        }
    } finally {
        $env:PATH = $saved
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($parts -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hex = ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join ''
    } finally {
        $sha.Dispose()
    }
    return $hex.Substring(0, 8)
}

function Invoke-Native([string]$Exe, [string[]]$Arguments, [string]$Log) {
    $ErrorActionPreference = 'Continue'
    & $Exe @Arguments 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            # A native stderr line; an empty one carries only the type name.
            $m = $_.Exception.Message
            if ($m -eq 'System.Management.Automation.RemoteException') { '' } else { $m }
        } else { "$_" }
    } | Out-File -FilePath $Log -Append -Encoding utf8
    return $LASTEXITCODE
}

# Fetches the pinned source into Work\src, builds the two targets, and stages
# them with their checksums into Stage. Returns $true on success.
function Invoke-LaneBuild($BuildEnv, [string]$Commit, [string]$PinFile, [string]$Work, [string]$Stage, [string]$Log) {
    $src = Join-Path $Work 'src'
    $python = Get-PythonExe
    $saved = @{}
    foreach ($k in 'PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'BAZEL_SH') { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
    try {
        foreach ($k in 'PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'BAZEL_SH') {
            if ($BuildEnv.ContainsKey($k)) { [Environment]::SetEnvironmentVariable($k, $BuildEnv[$k]) }
        }
        # A kept work tree: its fixed path keeps Bazel's output base, and so
        # its local caches, from one pin to the next.
        # git clean below also removes the tree's bazel-* junctions; git
        # removes a junction without following it, so the output base they
        # point into survives. Do not replace it with a recursive delete.
        if (-not (Test-Path -LiteralPath (Join-Path $src '.git'))) {
            New-Item -ItemType Directory -Force -Path $src | Out-Null
            if ((Invoke-Native 'git' @('-C', $src, 'init', '-q') $Log) -ne 0) { Write-Warn "git init failed"; return $false }
        }
        if ((Invoke-Native 'git' @('-C', $src, 'fetch', '-q', '--depth', '1', $OptimizerRepo, $Commit) $Log) -ne 0 -or
            (Invoke-Native 'git' @('-C', $src, 'checkout', '-q', '--force', '--detach', 'FETCH_HEAD') $Log) -ne 0 -or
            (Invoke-Native 'git' @('-C', $src, 'clean', '-q', '-ffdx') $Log) -ne 0) {
            Write-Warn "cannot fetch the optimizer source at $($Commit.Substring(0, 12))"
            return $false
        }
        $head = & { $ErrorActionPreference = 'Continue'; "$(& git -C $src rev-parse HEAD 2>$null)".Trim() }
        if ($head -cne $Commit) { Write-Warn "the fetched optimizer source is at $head, not $Commit"; return $false }
        # The worker reports the commit in GIT_COMMIT; the source tree carries
        # a placeholder, which a kept output base would otherwise keep
        # reporting for every later pin.
        [IO.File]::WriteAllText((Join-Path $src 'GIT_COMMIT'), $Commit)

        if ((Invoke-Native $python @('-I', (Join-Path $RepoRoot 'tools\ci\check_optimizer_pin_pair.py'), $PinFile,
                (Join-Path $src 'MODULE.bazel'), (Join-Path $RepoRoot 'bazel\repositories.bzl')) $Log) -ne 0) {
            Write-Warn "the pinned optimizer links another cache library than this module"
            Get-Content -LiteralPath $Log -Tail 20 | ForEach-Object { Write-Host $_ }
            return $false
        }

        $root = if ($env:CI_WIN_BAZEL_OUTPUT_ROOT) { $env:CI_WIN_BAZEL_OUTPUT_ROOT } else { Join-Path $env:SystemDrive 'b' }
        $startup = @('--nohome_rc', "--output_user_root=$($root -replace '\\', '/')")
        $jobs = 'HOST_CPUS*.75'
        if ($env:CI_SLOT_CPUS -match '^[1-9][0-9]{0,3}$') { $jobs = $env:CI_SLOT_CPUS }
        $common = $BuildConfigs + @("--jobs=$jobs", '--nostamp', '--show_progress_rate_limit=30')
        if ($env:CI_WIN_CACHE_DIR) { $common += "--disk_cache=$($env:CI_WIN_CACHE_DIR -replace '\\', '/')" }
        if ($env:BAZEL_REMOTE_CACHE_URL) { $common += "--remote_cache=$($env:BAZEL_REMOTE_CACHE_URL)" }
        $targets = @('//src/worker:factory_worker', '//lib/pagespeed:libpagespeed.so')
        $bazel = $BuildEnv['BAZEL_EXE']
        Push-Location $src
        try {
            # A server left from an earlier build would keep its old environment.
            [void](Invoke-Native $bazel ($startup + @('shutdown')) $Log)
            $rc = Invoke-Native $bazel ($startup + @('build') + $common + $targets) $Log
            if ($rc -ne 0) {
                Write-Warn "the optimizer build failed (exit $rc); see $Log"
                Get-Content -LiteralPath $Log -Tail 30 | ForEach-Object { Write-Host $_ }
                return $false
            }
            Select-String -LiteralPath $Log -Pattern '^INFO: [0-9]+ processes' | Select-Object -Last 1 |
                ForEach-Object { Write-Info ($_.Line -replace '^INFO: ', '') }
            $bin = & {
                # Bazel's progress lines go to stderr; only stdout is the path.
                $ErrorActionPreference = 'Continue'
                "$(& $bazel @startup info @common bazel-bin 2>$null)".Trim()
            }
        } finally {
            [void](Invoke-Native $bazel ($startup + @('shutdown')) $Log)
            Pop-Location
        }
        $worker = Join-Path $bin 'src\worker\factory_worker.exe'
        $lib = Join-Path $bin 'lib\pagespeed\libpagespeed.so'
        if (-not (Test-Path -LiteralPath $worker) -or -not (Test-Path -LiteralPath $lib)) {
            Write-Warn "the build reported success but its outputs are missing"
            return $false
        }
        Copy-Item -LiteralPath $worker -Destination (Join-Path $Stage 'factory_worker.exe')
        Copy-Item -LiteralPath $lib -Destination (Join-Path $Stage 'pagespeed.dll')
        [IO.File]::WriteAllText((Join-Path $Stage 'optimizer-commit.txt'), "$Commit`n")
        $sums = foreach ($name in $LaneFiles) { "$(Get-Sha256Hex (Join-Path $Stage $name))  $name" }
        [IO.File]::WriteAllText((Join-Path $Stage 'SHA256SUMS'), (($sums -join "`n") + "`n"))
        Set-LaneAcl $Stage
        foreach ($name in $LaneFiles + 'SHA256SUMS') {
            $f = Join-Path $Stage $name
            Set-LaneAcl $f
            Set-ItemProperty -LiteralPath $f -Name IsReadOnly -Value $true
        }
        return $true
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}

function Remove-Tree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    Get-ChildItem -LiteralPath $Path -Recurse -Force -File -ErrorAction SilentlyContinue |
        ForEach-Object { $_.IsReadOnly = $false }
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue
}

# --- main ---
if ($PrintBuildConfig) {
    $BuildConfigs | ForEach-Object { Write-Output $_ }
    exit 0
}
if (-not $Pin) { $Pin = if ($env:PAGESPEED_OPTIMIZER_PIN) { $env:PAGESPEED_OPTIMIZER_PIN } else { Join-Path $RepoRoot 'test\system\optimizer-test-pin.json' } }
try {
    $commit = Get-PinCommit $Pin
} catch {
    Write-Host "::error::$($_.Exception.Message)"
    exit 1
}
$short = $commit.Substring(0, 12)

if ($Verify) {
    $why = Test-LaneBuild $Verify $commit $MySid
    if ($why) {
        Write-Host "::error::the optimizer build $Verify does not check out for $short`: $why"
        exit 1
    }
    Write-Info "the optimizer build for $short checks out"
    $verified = $script:LaneHashes
    if ($CopyTo) {
        try {
            if (Test-Path -LiteralPath $CopyTo) { Remove-Item -LiteralPath $CopyTo -Recurse -Force -ErrorAction Stop }
            New-Item -ItemType Directory -Force -Path $CopyTo | Out-Null
            foreach ($name in $LaneFiles) {
                $copy = Join-Path $CopyTo $name
                Copy-Item -LiteralPath (Join-Path $Verify $name) -Destination $copy -Force -ErrorAction Stop
                Set-ItemProperty -LiteralPath $copy -Name IsReadOnly -Value $false
                # Against the hashes taken during the check, not a new read
                # of the build's SHA256SUMS.
                if ((Get-Sha256Hex $copy) -cne $verified[$name]) {
                    throw "the copy of $name does not match the checked build"
                }
            }
        } catch {
            Write-Host "::error::could not copy the checked optimizer build into $CopyTo`: $($_.Exception.Message)"
            exit 1
        }
        Write-Info "copied the checked build into $CopyTo"
    }
    foreach ($name in $LaneFiles) { Write-Output "$($verified[$name])  $name" }
    exit 0
}

if (-not $LaneDir) { $LaneDir = if ($env:CI_OPTIMIZER_LANE_DIR) { $env:CI_OPTIMIZER_LANE_DIR } else { Join-Path $env:SystemDrive 'pagespeed_optimizer_lane' } }
if (-not $LogFile) { $LogFile = Join-Path $LaneDir 'build.log' }
if ($CheckLaneDir) {
    try { Initialize-LaneDir $LaneDir } catch { Write-Host "::error::$($_.Exception.Message)"; exit 1 }
    Write-Info "the lane directory checks out"
    exit 0
}
Write-Info "Optimizer pin: $commit (from $Pin)"

$result = $null
$mutex = $null
$held = $false
try {
    Initialize-LaneDir $LaneDir
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogFile) | Out-Null
    $buildEnv = Get-BuildEnv
    $key = "$short-$(Get-Recipe $buildEnv)"
    $final = Join-Path $LaneDir $key

    # Refresh first, so a prune cannot take a build between check and use.
    if (Test-Path -LiteralPath $final -PathType Container) {
        try { (Get-Item -LiteralPath $final -Force).LastWriteTime = Get-Date } catch { }
    }
    if (-not (Test-LaneBuild $final $commit $MySid)) {
        Write-Info "Optimizer worker for $key already built"
        $result = $final
    } else {
        # One build at a time per host: wait for a concurrent build, then
        # reuse what it published.
        try {
            $mutex = New-Object System.Threading.Mutex($false, $LockName)
        } catch {
            # An account without the right to create global objects (a lab
            # login, not a runner service) locks within its own session.
            $mutex = New-Object System.Threading.Mutex($false, ($LockName -replace '^Global\\', 'Local\'))
        }
        try {
            $held = $mutex.WaitOne([TimeSpan]::FromMinutes($LockMinutes))
        } catch {
            # Abandoned by a build that died: the lock is now this one's.
            if ($_.Exception.InnerException -is [System.Threading.AbandonedMutexException] -or
                $_.Exception -is [System.Threading.AbandonedMutexException]) { $held = $true } else { throw }
        }
        if (-not $held) {
            Write-Host "::error::another optimizer build on this host held the lock for $LockMinutes minutes"
            exit 1
        }
        if (-not (Test-LaneBuild $final $commit $MySid)) {
            Write-Info "Optimizer worker for $key was built by a concurrent lane"
            $result = $final
        } else {
            $fresh = $false
            if (Test-Path -LiteralPath $final) {
                # Not ours or not sound: never reuse it, never remove it.
                Write-Warn "$final exists but does not check out ($(Test-LaneBuild $final $commit $MySid)); building into a fresh directory"
                $fresh = $true
            }
            $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
            $stage = Join-Path $LaneDir ($(if ($fresh) { '.fresh-' } else { '.stage-' }) + "$key-$tag")
            New-Item -ItemType Directory -Path $stage | Out-Null
            Write-Info "Building the optimizer worker at $key..."
            Set-Content -LiteralPath $LogFile -Value "optimizer lane build $key" -Encoding ascii
            $started = Get-Date
            $ok = $false
            try {
                $ok = Invoke-LaneBuild $buildEnv $commit $Pin (Join-Path $LaneDir 'work') $stage $LogFile
            } catch {
                Write-Warn "the optimizer build failed: $($_.Exception.Message)"
            }
            if (-not $ok) {
                Remove-Tree $stage
                Write-Host "::error::the optimizer worker at $key could not be built; the IIS system tests need it, so this is a failure, not a skip"
                exit 1
            }
            Write-Info ("built in {0:N0} s" -f ((Get-Date) - $started).TotalSeconds)
            $target = $stage
            if (-not $fresh) {
                try {
                    [IO.Directory]::Move($stage, $final)
                    $target = $final
                } catch {
                    Remove-Tree $stage
                    $target = $final
                }
            }
            $why = Test-LaneBuild $target $commit $MySid
            if ($why) {
                Write-Host "::error::the new optimizer build does not check out: $why"
                exit 1
            }
            $result = $target
            # Drop this account's builds no lane on this host used for two
            # weeks, and leftovers of interrupted or one-off builds.
            $now = Get-Date
            Get-ChildItem -LiteralPath $LaneDir -Directory -Force | Where-Object {
                $_.FullName -ne $target -and
                $_.GetAccessControl('Owner').GetOwner([System.Security.Principal.SecurityIdentifier]) -eq $MySid -and
                ((($_.Name -cmatch '^[0-9a-f]{12}-[0-9a-f]{8}$') -and $_.Name -cne $key -and ($now - $_.LastWriteTime).TotalDays -gt 14) -or
                 (($_.Name -like '.stage-*' -or $_.Name -like '.fresh-*') -and ($now - $_.LastWriteTime).TotalDays -gt 1))
            } | ForEach-Object { Remove-Tree $_.FullName }
        }
    }
} catch {
    Write-Host "::error::the optimizer lane failed: $($_.Exception.Message)"
    exit 1
} finally {
    if ($held) { $mutex.ReleaseMutex() }
    if ($mutex) { $mutex.Dispose() }
}

if ($ResultFile) { [IO.File]::WriteAllText($ResultFile, "$result`n") }
if ($env:GITHUB_ENV) { "PAGESPEED_OPTIMIZER_BUILD_DIR=$result" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append }
Write-Output $result
exit 0
