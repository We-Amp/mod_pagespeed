# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test for the reuse check of Build-OptimizerPin.ps1 (-Verify). No
# Pester dependency, no build. Builds a temp lane directory the way the script
# publishes one (three files, SHA256SUMS, an owner-only ACL) and checks that
# it checks out, and that each of these makes it fail:
#   * a binary that no longer matches SHA256SUMS;
#   * optimizer-commit.txt naming another commit;
#   * an extra file, and a missing file;
#   * a SHA256SUMS line for a file that is not part of the build;
#   * an ACL that lets Users write;
#   * the directory being a junction to a sound build;
# that a pin without a full commit hash is refused, that -Verify prints the
# hashes it checked and -CopyTo copies exactly the checked files (and nothing
# from a build that does not check out), that the lane directory is created
# with, or reset to, the owner-only ACL, and that the build uses the
# optimized release configuration (--config=opt).
#
# Run:   powershell -NoProfile -File tools\ci\Build-OptimizerPin_test.ps1
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'Build-OptimizerPin.ps1'

$fails = 0
function Pass { param([string]$Name) Write-Host ("PASS: " + $Name) }
function Fail { param([string]$Name) Write-Host ("FAIL: " + $Name); $script:fails += 1 }

$commit = 'abcdef0123456789abcdef0123456789abcdef01'
$other = '0123456789abcdef0123456789abcdef01234567'
$names = @('factory_worker.exe', 'optimizer-commit.txt', 'pagespeed.dll')
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value

function Get-Hex([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Write-Sums([string]$Dir, [string[]]$List) {
    $lines = foreach ($n in $List) { "$(Get-Hex (Join-Path $Dir $n))  $n" }
    [IO.File]::WriteAllText((Join-Path $Dir 'SHA256SUMS'), (($lines -join "`n") + "`n"))
}

# An owner-only ACL, as the script publishes it.
function Set-OwnerOnly([string]$Path) {
    & icacls.exe $Path /inheritance:r /grant:r "*$($me):(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-32-545:(OI)(CI)RX" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls failed on $Path" }
}

function New-Build([string]$Dir, [string]$Commit) {
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $Dir 'factory_worker.exe'), 'worker')
    [IO.File]::WriteAllText((Join-Path $Dir 'pagespeed.dll'), 'library')
    [IO.File]::WriteAllText((Join-Path $Dir 'optimizer-commit.txt'), "$Commit`n")
    Write-Sums $Dir $names
    Set-OwnerOnly $Dir
    # Files inherit the directory's ACL.
    foreach ($n in $names + 'SHA256SUMS') { & icacls.exe (Join-Path $Dir $n) /reset | Out-Null }
}

function Invoke-Verify([string]$Dir, [string]$PinFile) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Pin $PinFile -Verify $Dir 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Out = $out }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("optimizer-pin-test-" + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $pin = Join-Path $root 'pin.json'
    [IO.File]::WriteAllText($pin, "{`"optimizer_commit`": `"$commit`"}")

    $good = Join-Path $root 'good'
    New-Build $good $commit
    $r = Invoke-Verify $good $pin
    if ($r.Code -eq 0) { Pass 'a sound build checks out' } else { Fail "a sound build checks out: $($r.Out)" }

    $d = Join-Path $root 'tampered'
    New-Build $d $commit
    [IO.File]::WriteAllText((Join-Path $d 'pagespeed.dll'), 'other library')
    $r = Invoke-Verify $d $pin
    if ($r.Code -ne 0 -and $r.Out -match 'does not match SHA256SUMS') { Pass 'a changed binary is refused' } else { Fail "a changed binary is refused: $($r.Out)" }

    $d = Join-Path $root 'commit'
    New-Build $d $other
    $r = Invoke-Verify $d $pin
    if ($r.Code -ne 0 -and $r.Out -match 'optimizer-commit.txt') { Pass 'a build of another commit is refused' } else { Fail "a build of another commit is refused: $($r.Out)" }

    $d = Join-Path $root 'extra'
    New-Build $d $commit
    [IO.File]::WriteAllText((Join-Path $d 'extra.dll'), 'x')
    $r = Invoke-Verify $d $pin
    if ($r.Code -ne 0 -and $r.Out -match 'holds') { Pass 'an extra file is refused' } else { Fail "an extra file is refused: $($r.Out)" }

    $d = Join-Path $root 'missing'
    New-Build $d $commit
    Remove-Item -LiteralPath (Join-Path $d 'factory_worker.exe') -Force
    $r = Invoke-Verify $d $pin
    if ($r.Code -ne 0 -and $r.Out -match 'holds') { Pass 'a missing file is refused' } else { Fail "a missing file is refused: $($r.Out)" }

    $d = Join-Path $root 'listed'
    New-Build $d $commit
    Write-Sums $d @('factory_worker.exe', 'optimizer-commit.txt', 'pagespeed.dll', 'SHA256SUMS')
    $r = Invoke-Verify $d $pin
    if ($r.Code -ne 0 -and $r.Out -match 'SHA256SUMS') { Pass 'a checksum list naming another file is refused' } else { Fail "a checksum list naming another file is refused: $($r.Out)" }

    $d = Join-Path $root 'writable'
    New-Build $d $commit
    & icacls.exe (Join-Path $d 'pagespeed.dll') /grant "*S-1-5-32-545:M" | Out-Null
    $r = Invoke-Verify $d $pin
    if ($r.Code -ne 0 -and $r.Out -match 'writable by S-1-5-32-545') { Pass 'a file Users may write is refused' } else { Fail "a file Users may write is refused: $($r.Out)" }

    $d = Join-Path $root 'junction'
    & $env:ComSpec /c "mklink /J `"$d`" `"$good`"" | Out-Null
    $r = Invoke-Verify $d $pin
    if ($r.Code -ne 0 -and $r.Out -match 'link') { Pass 'a junction to a sound build is refused' } else { Fail "a junction to a sound build is refused: $($r.Out)" }

    $bad = Join-Path $root 'bad-pin.json'
    [IO.File]::WriteAllText($bad, '{"optimizer_commit": "main"}')
    $r = Invoke-Verify $good $bad
    if ($r.Code -ne 0 -and $r.Out -match 'full commit hash') { Pass 'a pin without a full commit hash is refused' } else { Fail "a pin without a full commit hash is refused: $($r.Out)" }
    # -Verify prints the hashes it checked; -CopyTo copies the build out and
    # checks the copies against those hashes.
    $lines = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $script -Pin $pin -Verify $good 2>$null | ForEach-Object { "$_".Trim() } | Where-Object { $_ -match '^[0-9a-f]{64}  ' })
    $want = @($names | ForEach-Object { "$(Get-Hex (Join-Path $good $_))  $_" })
    if (($lines -join '|') -ceq ($want -join '|')) { Pass '-Verify prints the hashes it checked' } else { Fail "-Verify prints the hashes it checked: $($lines -join ' / ')" }
    $dest = Join-Path $root 'copy'
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Pin $pin -Verify $good -CopyTo $dest 2>&1 | Out-String
    $copied = @(Get-ChildItem -LiteralPath $dest -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name } | Sort-Object)
    $same = ($copied -join '|') -ceq ($names -join '|')
    if ($same) { foreach ($n in $names) { if ((Get-Hex (Join-Path $dest $n)) -cne (Get-Hex (Join-Path $good $n))) { $same = $false } } }
    if ($LASTEXITCODE -eq 0 -and $same) { Pass '-CopyTo copies exactly the checked files' } else { Fail "-CopyTo copies exactly the checked files: $r" }
    $dest2 = Join-Path $root 'copy-bad'
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Pin $pin -Verify (Join-Path $root 'tampered') -CopyTo $dest2 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -and -not (Test-Path -LiteralPath (Join-Path $dest2 'pagespeed.dll'))) { Pass '-CopyTo copies nothing from a build that does not check out' } else { Fail "-CopyTo copies nothing from a build that does not check out: $r" }

    # The lane directory: created owner-only; an existing one that lets Users
    # write, or inherits, is reset; a sound one is left alone.
    $lane = Join-Path $root 'lane-new'
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Pin $pin -LaneDir $lane -CheckLaneDir 2>&1 | Out-String
    $acl = Get-Acl -LiteralPath $lane
    if ($LASTEXITCODE -eq 0 -and $acl.AreAccessRulesProtected) { Pass 'a new lane directory gets the owner-only ACL' } else { Fail "a new lane directory gets the owner-only ACL: $r" }
    $lane = Join-Path $root 'lane-open'
    New-Item -ItemType Directory -Force -Path $lane | Out-Null
    & icacls.exe $lane /grant "*S-1-5-32-545:(OI)(CI)M" | Out-Null
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Pin $pin -LaneDir $lane -CheckLaneDir 2>&1 | Out-String
    $acl = Get-Acl -LiteralPath $lane
    $usersWrite = @($acl.Access | Where-Object { $_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-32-545' -and ([int64]$_.FileSystemRights -band 0x2) })
    if ($LASTEXITCODE -eq 0 -and $acl.AreAccessRulesProtected -and $usersWrite.Count -eq 0) { Pass 'an existing lane directory that Users may write is reset' } else { Fail "an existing lane directory that Users may write is reset: $r" }
    $r = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Pin $pin -LaneDir $lane -CheckLaneDir 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -and $r -notmatch 'resetting') { Pass 'a sound lane directory is left as it is' } else { Fail "a sound lane directory is left as it is: $r" }

    # The build is the optimizer's optimized release configuration, not a
    # debug (fastbuild) build.
    $cfg = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $script -PrintBuildConfig 2>&1 | ForEach-Object { "$_".Trim() })
    if ($LASTEXITCODE -eq 0 -and $cfg -contains '--config=opt' -and $cfg -contains '--config=windows') { Pass 'the build uses --config=windows --config=opt' } else { Fail "the build uses --config=windows --config=opt: $($cfg -join ' ')" }
} finally {
    if (Test-Path -LiteralPath (Join-Path $root 'junction')) { & $env:ComSpec /c "rmdir `"$(Join-Path $root 'junction')`"" | Out-Null }
    & icacls.exe $root /reset /T /Q 2>$null | Out-Null
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

if ($fails -gt 0) { Write-Host "$fails check(s) failed"; exit 1 }
Write-Host "all checks passed"
exit 0
