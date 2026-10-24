# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test for fetch-vendor-tarball.ps1. No Pester dependency; needs
# zstd and a tar on PATH (or the Windows tar). Cases:
#   1. a whole tarball with its sidecar is staged, verified and extracted;
#      the shared copy is left alone and nothing stays in the staging dir.
#   2. a truncated tarball is retried, then rejected (exit 1); nothing is
#      extracted and nothing is left behind.
#   3. a whole tarball whose sidecar names another digest is retried, then
#      rejected (exit 1).
#   4. no source has the tarball: exit 4, nothing extracted.
#   5. a truncated tarball that a producer completes during the retries is
#      picked up (exit 0).
#   6. a sound tarball with no sidecar is accepted on the last pass only.
#   7. two readers of the same shared tarball, running concurrently, use
#      private files and both extract (no shared decompression intermediate).
#   8. the staged copy and the decompressed intermediate of a fetch from an
#      old shared tarball look new, so a prune of the staging directory run
#      between the steps (here, from inside the tar step) removes neither.
#   9. a destination holding leftovers of an earlier run (a stale file and
#      directory, a read-only file) and a .git directory: the leftovers go,
#      .git stays, the extraction succeeds (issue #231).
#  10. a junction in the destination (Windows only) is removed as a link and
#      its target is not entered.
#  11. a locked file in the destination (Windows only) fails the fetch
#      (exit 1) and nothing is extracted.
#  12. a tar that fails on a member: on Windows a batch stand-in that the
#      script invokes through cmd.exe exactly as it invokes the real tar,
#      so the member error line arrives plain, the form the real failure
#      takes under Windows PowerShell 5.1; elsewhere a script stand-in
#      whose line carries the 'tar.exe : ' decoration a PowerShell
#      redirection adds. The fetch fails (exit 1), nothing is extracted,
#      and the diagnostic names the member and the same-file refusal.
#  13. the running script sits inside the destination (the workflows clear
#      the workspace they invoked it from): the clear removes it and the
#      extraction still succeeds.
#  14. a junction and a read-only file nested two directories deep in the
#      destination (Windows only) are cleared without entering the link;
#      the case runs twice, once as-is and once with ComSpec cleared so
#      only the careful walker runs.
#  15. the clear refuses what it must not touch: a target outside the
#      workspace without the test switch (its files survive), a
#      drive-relative path (C:), a relative path, a staging directory
#      inside the target, and a link as the target itself.
#
# Run:   powershell -NoProfile -File tools\ci\fetch-vendor-tarball_test.ps1
#   or:  pwsh -NoProfile -File tools/ci/fetch-vendor-tarball_test.ps1
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'fetch-vendor-tarball.ps1'

if (-not (Get-Command zstd -ErrorAction SilentlyContinue)) { Write-Host 'SKIP: zstd not installed'; exit 0 }
$tar = $null
if ([Environment]::OSVersion.Platform -eq 'Win32NT') { $tar = Join-Path $env:SystemRoot 'System32\tar.exe' }
else { $tar = (Get-Command tar -ErrorAction SilentlyContinue).Source }
if (-not $tar) { Write-Host 'SKIP: tar not installed'; exit 0 }

$fails = 0
function Pass { param([string]$Name) Write-Host ("PASS: " + $Name) }
function Fail { param([string]$Name) Write-Host ("FAIL: " + $Name); $script:fails += 1 }

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("fetch-vendor-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null

function New-Dir { param([string]$Leaf) $p = Join-Path $root $Leaf; New-Item -ItemType Directory -Path $p | Out-Null; return $p }
function Sha { param([string]$P) return (Get-FileHash -LiteralPath $P -Algorithm SHA256).Hash.ToLowerInvariant() }

# A real tarball: a tiny workspace with GIT_COMMIT and one larger file so a
# truncation lands inside the zstd frame.
$src = New-Dir 'src'
[System.IO.File]::WriteAllText((Join-Path $src 'GIT_COMMIT'), 'abcdef0123456789')
$rnd = New-Object byte[] 400000
(New-Object System.Random 7).NextBytes($rnd)
[System.IO.File]::WriteAllBytes((Join-Path $src 'blob.bin'), $rnd)
$name = 'mod_pagespeed-abc1234.tar.zst'
$goodTar = Join-Path $root 'good.tar'
& $tar cf $goodTar -C $src .
$good = Join-Path $root $name
& zstd -q -f $goodTar -o $good
$goodLen = (Get-Item -LiteralPath $good).Length

function New-Shared {
  param([string]$Leaf, [string]$Kind)
  $d = New-Dir $Leaf
  $f = Join-Path $d $name
  if ($Kind -eq 'partial') {
    $bytes = [System.IO.File]::ReadAllBytes($good)
    [System.IO.File]::WriteAllBytes($f, $bytes[0..([int]($goodLen / 2))])
    "$(Sha $good)  $name" | Set-Content -LiteralPath "$f.sha256" -Encoding ascii
  } elseif ($Kind -eq 'wrong-digest') {
    Copy-Item -LiteralPath $good -Destination $f
    ('0' * 64 + "  $name") | Set-Content -LiteralPath "$f.sha256" -Encoding ascii
  } elseif ($Kind -eq 'no-sidecar') {
    Copy-Item -LiteralPath $good -Destination $f
  } else {
    Copy-Item -LiteralPath $good -Destination $f
    "$(Sha $good)  $name" | Set-Content -LiteralPath "$f.sha256" -Encoding ascii
  }
  return $d
}

function Invoke-Fetch {
  param([string]$Shared, [string]$Dest, [string]$Stage, [int]$Retries = 2, [int]$Backoff = 0, [string]$Tag = 't1')
  & $script -Name $name -Dest $Dest -StageDir $Stage -SearchDirs @($Shared) -Retries $Retries -BackoffSeconds $Backoff -Tag $Tag -TarExe $tar -AllowTargetOutsideWorkspaceForTest 6>&1 | Out-Null
  return $LASTEXITCODE
}
function Stage-Empty { param([string]$Stage) return (-not (Test-Path -LiteralPath $Stage)) -or (@(Get-ChildItem -LiteralPath $Stage -Force).Count -eq 0) }

try {
# 1
$sh = New-Shared 'shared1' 'good'; $dest = Join-Path $root 'dest1'; $stage = Join-Path $root 'stage1'
$rc = Invoke-Fetch $sh $dest $stage
if ($rc -eq 0 -and (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '1: staged, verified, extracted' } else { Fail "1: exit $rc" }
if ((Sha (Join-Path $sh $name)) -eq (Sha $good)) { Pass '1: shared copy untouched' } else { Fail '1: shared copy changed' }
if (Stage-Empty $stage) { Pass '1: staging dir emptied' } else { Fail '1: staging dir not emptied' }

# 2
$sh = New-Shared 'shared2' 'partial'; $dest = Join-Path $root 'dest2'; $stage = Join-Path $root 'stage2'
$rc = Invoke-Fetch $sh $dest $stage
if ($rc -eq 1 -and -not (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT')) -and (Stage-Empty $stage)) { Pass '2: partial retried, then rejected; nothing left' } else { Fail "2: exit $rc" }

# 3
$sh = New-Shared 'shared3' 'wrong-digest'; $dest = Join-Path $root 'dest3'; $stage = Join-Path $root 'stage3'
$rc = Invoke-Fetch $sh $dest $stage
if ($rc -eq 1 -and -not (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '3: digest mismatch retried, then rejected' } else { Fail "3: exit $rc" }

# 4
$empty = New-Dir 'shared4'; $dest = Join-Path $root 'dest4'; $stage = Join-Path $root 'stage4'
$rc = Invoke-Fetch $empty $dest $stage
if ($rc -eq 4 -and -not (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '4: unreachable exits 4' } else { Fail "4: exit $rc" }

# 5: a producer completes the file while the reader is retrying.
$sh = New-Shared 'shared5' 'partial'; $dest = Join-Path $root 'dest5'; $stage = Join-Path $root 'stage5'
$job = Start-Job -ArgumentList $good, (Join-Path $sh $name), "$(Sha $good)  $name" -ScriptBlock {
  param($g, $f, $line)
  Start-Sleep -Seconds 1
  Copy-Item -LiteralPath $g -Destination "$f.new"
  Move-Item -LiteralPath "$f.new" -Destination $f -Force
}
$rc = Invoke-Fetch $sh $dest $stage 4 2
Wait-Job $job | Out-Null; Remove-Job $job -Force
if ($rc -eq 0 -and (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '5: completed tarball picked up on a retry' } else { Fail "5: exit $rc" }

# 6
$sh = New-Shared 'shared6' 'no-sidecar'; $dest = Join-Path $root 'dest6'; $stage = Join-Path $root 'stage6'
$rc = Invoke-Fetch $sh $dest $stage 2 0
if ($rc -eq 0 -and (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '6: no sidecar accepted on the last pass' } else { Fail "6: exit $rc" }

# 7
$sh = New-Shared 'shared7' 'good'; $stage = Join-Path $root 'stage7'
$d1 = Join-Path $root 'dest7a'; $d2 = Join-Path $root 'dest7b'
# Reader A runs as a background job while reader B runs here.
$jobA = Start-Job -ArgumentList $script, $name, $d1, $stage, $sh, $tar -ScriptBlock {
  param($s, $n, $d, $st, $shared, $t)
  & $s -Name $n -Dest $d -StageDir $st -SearchDirs @($shared) -Retries 2 -BackoffSeconds 0 -Tag 'jobA' -TarExe $t -AllowTargetOutsideWorkspaceForTest 6>&1 | Out-Null
  return $LASTEXITCODE
}
$rc2 = Invoke-Fetch $sh $d2 $stage 2 0 'jobB'
$rc1 = @(Receive-Job -Job $jobA -Wait)[-1]
Remove-Job $jobA -Force
if ($rc1 -eq 0 -and $rc2 -eq 0 -and (Test-Path -LiteralPath (Join-Path $d1 'GIT_COMMIT')) -and (Test-Path -LiteralPath (Join-Path $d2 'GIT_COMMIT'))) { Pass '7: two readers each extract from private files' } else { Fail "7: exits $rc1 $rc2" }
if (-not (Test-Path -LiteralPath (Join-Path $sh "$name.tar"))) { Pass '7: no decompression intermediate in the shared dir' } else { Fail '7: intermediate left in the shared dir' }

# 8: the shared tarball is hours old; a prune that runs while the fetch is
# between decompression and extraction (from a stand-in for tar) must keep
# both private files.
$sh = New-Shared 'shared8' 'good'; $dest = Join-Path $root 'dest8'; $stage = Join-Path $root 'stage8'
foreach ($f8 in @((Join-Path $sh $name), (Join-Path $sh "$name.sha256"))) { (Get-Item -LiteralPath $f8).LastWriteTime = (Get-Date).AddHours(-5) }
$marker = Join-Path $root 'marker8'
$wrap = Join-Path $root 'tar-wrap8.ps1'
$wrapText = @'
& '@PRUNE@' -VendorDir '@STAGE@' -ShortSha fffffff -SharedDir 'Z:\no\such\shared' 6>&1 | Out-Null
$kept = @(Get-ChildItem -LiteralPath '@STAGE@' -File | Where-Object { $_.Name -like '*.stage' -or $_.Name -like '*.tar' }).Count
Set-Content -LiteralPath '@MARKER@' -Value $kept
& '@TAR@' @args
exit $LASTEXITCODE
'@
$wrapText = $wrapText.Replace('@PRUNE@', (Join-Path $here 'prune-win-vendor.ps1')).Replace('@STAGE@', $stage).Replace('@MARKER@', $marker).Replace('@TAR@', $tar)
Set-Content -LiteralPath $wrap -Value $wrapText -Encoding ascii
$tar8 = $wrap
if ([Environment]::OSVersion.Platform -eq 'Win32NT') {
  # On Windows the fetch invokes tar through cmd.exe (its redirection
  # writes stderr plain), and cmd.exe cannot execute a .ps1: a thin batch
  # stand-in delegates to the script above, arguments passed through.
  $psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $wrapCmd = Join-Path $root 'tar-wrap8.cmd'
  $batch = @(
    '@echo off',
    ('"' + $psExe + '" -NoProfile -ExecutionPolicy Bypass -File "' + $wrap + '" %*'),
    'exit /b %errorlevel%'
  )
  Set-Content -LiteralPath $wrapCmd -Value $batch -Encoding ascii
  $tar8 = $wrapCmd
}
& $script -Name $name -Dest $dest -StageDir $stage -SearchDirs @($sh) -Retries 2 -BackoffSeconds 0 -Tag 'prune8' -TarExe $tar8 -AllowTargetOutsideWorkspaceForTest 6>&1 | Out-Null
$rc = $LASTEXITCODE
$kept8 = if (Test-Path -LiteralPath $marker) { (Get-Content -LiteralPath $marker -TotalCount 1).Trim() } else { 'none' }
if ($kept8 -eq '2') { Pass '8: a prune keeps the staged copy and the intermediate of a live fetch' } else { Fail "8: files surviving the prune: $kept8" }
if ($rc -eq 0 -and (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '8: fetch from an old tarball extracts' } else { Fail "8: exit $rc" }

# 9: leftovers of an earlier run in the destination (a stale file, a stale
# directory tree, a read-only file) and a .git directory: the clear takes
# the leftovers, keeps .git, and the extraction succeeds.
$sh = New-Shared 'shared9' 'good'; $dest = New-Dir 'dest9'; $stage = Join-Path $root 'stage9'
[System.IO.File]::WriteAllText((Join-Path $dest 'stale.txt'), 'old')
New-Item -ItemType Directory -Path (Join-Path $dest 'staledir\nested') -Force | Out-Null
[System.IO.File]::WriteAllText((Join-Path $dest 'staledir\nested\deep.txt'), 'old')
New-Item -ItemType Directory -Path (Join-Path $dest '.git') | Out-Null
[System.IO.File]::WriteAllText((Join-Path $dest '.git\HEAD'), 'ref: refs/heads/master')
[System.IO.File]::WriteAllText((Join-Path $dest 'readonly.txt'), 'old')
Set-ItemProperty -LiteralPath (Join-Path $dest 'readonly.txt') -Name IsReadOnly -Value $true
$rc = Invoke-Fetch $sh $dest $stage
$ok = ($rc -eq 0) -and (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT')) `
  -and (-not (Test-Path -LiteralPath (Join-Path $dest 'stale.txt'))) `
  -and (-not (Test-Path -LiteralPath (Join-Path $dest 'staledir'))) `
  -and (-not (Test-Path -LiteralPath (Join-Path $dest 'readonly.txt'))) `
  -and (Test-Path -LiteralPath (Join-Path $dest '.git\HEAD'))
if ($ok) { Pass '9: leftovers cleared (read-only included), .git kept, extracted' } else { Fail "9: exit $rc" }

# 10: a junction in the destination is removed as a link; its target is not
# entered (Windows only: junctions do not exist elsewhere).
if ([Environment]::OSVersion.Platform -eq 'Win32NT') {
  $sh = New-Shared 'shared10' 'good'; $dest = New-Dir 'dest10'; $stage = Join-Path $root 'stage10'
  $target = New-Dir 'target10'
  [System.IO.File]::WriteAllText((Join-Path $target 'keep.txt'), 'keep')
  New-Item -ItemType Junction -Path (Join-Path $dest 'link') -Value $target | Out-Null
  $rc = Invoke-Fetch $sh $dest $stage
  $ok = ($rc -eq 0) -and (-not (Test-Path -LiteralPath (Join-Path $dest 'link'))) `
    -and (Test-Path -LiteralPath (Join-Path $target 'keep.txt'))
  if ($ok) { Pass '10: a junction is removed as a link, its target kept' } else { Fail "10: exit $rc" }
}

# 11: a locked file in the destination: the clear fails, names the file,
# and nothing is extracted (Windows only: an open file can still be
# unlinked elsewhere).
if ([Environment]::OSVersion.Platform -eq 'Win32NT') {
  $sh = New-Shared 'shared11' 'good'; $dest = New-Dir 'dest11'; $stage = Join-Path $root 'stage11'
  $locked = Join-Path $dest 'locked.bin'
  [System.IO.File]::WriteAllText($locked, 'held')
  $h = [System.IO.File]::Open($locked, 'Open', 'Read', 'None')
  try {
    $rc = Invoke-Fetch $sh $dest $stage
    if ($rc -eq 1 -and (Test-Path -LiteralPath $locked) -and -not (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '11: a locked file fails the clear, nothing extracted' } else { Fail "11: exit $rc" }
  } finally { $h.Close() }
}

# 12: tar fails on a member. On Windows the stand-in is a batch file, so
# the script invokes it exactly as it invokes the real tar (through
# cmd.exe, whose redirection writes the member error line plain); that
# is the form the real failure takes under Windows PowerShell 5.1.
# Elsewhere the stand-in is a script whose error line carries the
# 'tar.exe : ' decoration a PowerShell redirection adds, so the
# matcher's tolerance for that prefix is proven too. The fetch fails,
# nothing is extracted, and the diagnostic names the member and the
# same-file refusal.
$sh = New-Shared 'shared12' 'good'; $dest = New-Dir 'dest12'; $stage = Join-Path $root 'stage12'
if ([Environment]::OSVersion.Platform -eq 'Win32NT') {
  $wrap = Join-Path $root 'tar-wrap12.cmd'
  @'
@echo off
echo ./GIT_COMMIT: Refusing to overwrite archive: Input/output error 1>&2
exit /b 1
'@ | Set-Content -LiteralPath $wrap -Encoding ascii
} else {
  $wrap = Join-Path $root 'tar-wrap12.ps1'
  @'
Write-Error 'tar.exe : ./GIT_COMMIT: Refusing to overwrite archive: Input/output error'
exit 1
'@ | Set-Content -LiteralPath $wrap -Encoding ascii
}
$out = & $script -Name $name -Dest $dest -StageDir $stage -SearchDirs @($sh) -Retries 2 -BackoffSeconds 0 -Tag 't12' -TarExe $wrap -AllowTargetOutsideWorkspaceForTest 6>&1 | ForEach-Object { "$_" }
$rc = $LASTEXITCODE
$named = @($out | Where-Object { $_ -like '*refused to write*GIT_COMMIT*' })
if ($rc -eq 1 -and (-not (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) -and $named.Count -ge 1) { Pass '12: a tar member failure names the member and the same-file check' } else { Fail "12: exit $rc, diagnostic lines: $($named.Count)" }

# 13: the running script itself sits under the destination (the workflows
# clear the workspace they invoked it from): the clear removes its file and
# the extraction still succeeds.
$sh = New-Shared 'shared13' 'good'; $dest = New-Dir 'dest13'; $stage = Join-Path $root 'stage13'
New-Item -ItemType Directory -Path (Join-Path $dest 'tools\ci') -Force | Out-Null
Copy-Item -LiteralPath $script -Destination (Join-Path $dest 'tools\ci\fetch-vendor-tarball.ps1')
& (Join-Path $dest 'tools\ci\fetch-vendor-tarball.ps1') -Name $name -Dest $dest -StageDir $stage -SearchDirs @($sh) -Retries 2 -BackoffSeconds 0 -TarExe $tar -AllowTargetOutsideWorkspaceForTest 6>&1 | Out-Null
$rc = $LASTEXITCODE
if ($rc -eq 0 -and (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '13: the script deletes its own copy and still extracts' } else { Fail "13: exit $rc" }

# 14: a junction and a read-only file nested two directories deep inside
# the destination, so the rmdir pass and the careful walker both have to
# walk a directory containing them. The link is removed as a link (its
# target survives), the read-only file goes, the extraction succeeds.
# Run twice: once as-is, once with ComSpec cleared so only the walker
# runs (Windows only: junctions do not exist elsewhere).
if ([Environment]::OSVersion.Platform -eq 'Win32NT') {
  $sh = New-Shared 'shared14' 'good'; $stage = Join-Path $root 'stage14'
  foreach ($pass in @('rmdir', 'walker')) {
    $dest = New-Dir ("dest14-" + $pass)
    $outside = New-Dir ("outside14-" + $pass)
    [System.IO.File]::WriteAllText((Join-Path $outside 'keep.txt'), 'keep')
    New-Item -ItemType Directory -Path (Join-Path $dest 'sub\deeper') -Force | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $dest 'sub\deeper\link') -Value $outside | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $dest 'sub\ro.txt'), 'ro')
    Set-ItemProperty -LiteralPath (Join-Path $dest 'sub\ro.txt') -Name IsReadOnly -Value $true
    $savedComSpec = $env:ComSpec
    if ($pass -eq 'walker') { Remove-Item Env:\ComSpec -ErrorAction SilentlyContinue }
    try {
      $rc = Invoke-Fetch $sh $dest $stage 2 0 ("t14-" + $pass)
    } finally {
      if ($savedComSpec) { $env:ComSpec = $savedComSpec }
    }
    $ok = ($rc -eq 0) -and (-not (Test-Path -LiteralPath (Join-Path $dest 'sub'))) `
      -and (Test-Path -LiteralPath (Join-Path $outside 'keep.txt')) `
      -and (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))
    if ($ok) { Pass "14: nested link and read-only file cleared ($pass pass), outside target kept" } else { Fail "14 ($pass): exit $rc" }
  }
}

# 15: the clear refuses what it must not touch. GITHUB_WORKSPACE points
# at a directory of the test's own, so the refusals cannot hide behind
# an unset variable. Every refusal exits 1 before creating or deleting
# anything.
$sh = New-Shared 'shared15' 'good'; $stage = Join-Path $root 'stage15'
$dest = New-Dir 'dest15'
[System.IO.File]::WriteAllText((Join-Path $dest 'sentinel.txt'), 'keep')
$wsWas = $env:GITHUB_WORKSPACE
$env:GITHUB_WORKSPACE = Join-Path $root 'ws15'
try {
  # a: outside the workspace, no test switch.
  & $script -Name $name -Dest $dest -StageDir $stage -SearchDirs @($sh) -Retries 1 -BackoffSeconds 0 -Tag 't15a' -TarExe $tar 6>&1 | Out-Null
  $rc = $LASTEXITCODE
  if ($rc -eq 1 -and (Test-Path -LiteralPath (Join-Path $dest 'sentinel.txt')) -and -not (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '15a: a target outside the workspace is refused, its files survive' } else { Fail "15a: exit $rc" }

  # b: drive-relative (C: names the current directory on that drive).
  & $script -Name $name -Dest 'C:' -StageDir $stage -SearchDirs @($sh) -Retries 1 -BackoffSeconds 0 -Tag 't15b' -TarExe $tar 6>&1 | Out-Null
  $rc = $LASTEXITCODE
  if ($rc -eq 1) { Pass '15b: a drive-relative target is refused' } else { Fail "15b: exit $rc" }

  # c: relative (no directory of that name may appear anywhere).
  & $script -Name $name -Dest 'rel-target-15' -StageDir $stage -SearchDirs @($sh) -Retries 1 -BackoffSeconds 0 -Tag 't15c' -TarExe $tar 6>&1 | Out-Null
  $rc = $LASTEXITCODE
  if ($rc -eq 1 -and -not (Test-Path 'rel-target-15')) { Pass '15c: a relative target is refused, nothing created' } else { Fail "15c: exit $rc" }

  # d: the staging directory sits inside the target (even with the test
  # switch, which lifts only the workspace equality).
  & $script -Name $name -Dest $dest -StageDir (Join-Path $dest 'innerstage') -SearchDirs @($sh) -Retries 1 -BackoffSeconds 0 -Tag 't15d' -TarExe $tar -AllowTargetOutsideWorkspaceForTest 6>&1 | Out-Null
  $rc = $LASTEXITCODE
  if ($rc -eq 1 -and (Test-Path -LiteralPath (Join-Path $dest 'sentinel.txt')) -and -not (Test-Path -LiteralPath (Join-Path $dest 'innerstage'))) { Pass '15d: a stage dir inside the target is refused' } else { Fail "15d: exit $rc" }

  # e: the target itself is a link (Windows only).
  if ([Environment]::OSVersion.Platform -eq 'Win32NT') {
    $outside = New-Dir 'outside15'
    [System.IO.File]::WriteAllText((Join-Path $outside 'keep.txt'), 'keep')
    $linkDest = Join-Path $root 'dest15link'
    New-Item -ItemType Junction -Path $linkDest -Value $outside | Out-Null
    & $script -Name $name -Dest $linkDest -StageDir $stage -SearchDirs @($sh) -Retries 1 -BackoffSeconds 0 -Tag 't15e' -TarExe $tar -AllowTargetOutsideWorkspaceForTest 6>&1 | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -eq 1 -and (Test-Path -LiteralPath (Join-Path $outside 'keep.txt'))) { Pass '15e: a link as the target is refused' } else { Fail "15e: exit $rc" }
  }
} finally {
  if ($wsWas) { $env:GITHUB_WORKSPACE = $wsWas } else { Remove-Item Env:\GITHUB_WORKSPACE -ErrorAction SilentlyContinue }
}
} finally {
  Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue
}

if ($fails -ne 0) { Write-Host "$fails case(s) failed"; exit 1 }
Write-Host 'All fetch-vendor-tarball cases passed'
exit 0
