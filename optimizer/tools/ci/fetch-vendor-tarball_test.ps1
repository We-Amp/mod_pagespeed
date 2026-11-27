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
$name = 'pagespeed-optimizer-abc1234.tar.zst'
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
  & $script -Name $name -Dest $Dest -StageDir $Stage -SearchDirs @($Shared) -Retries $Retries -BackoffSeconds $Backoff -Tag $Tag -TarExe $tar 6>&1 | Out-Null
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
  & $s -Name $n -Dest $d -StageDir $st -SearchDirs @($shared) -Retries 2 -BackoffSeconds 0 -Tag 'jobA' -TarExe $t 6>&1 | Out-Null
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
& $script -Name $name -Dest $dest -StageDir $stage -SearchDirs @($sh) -Retries 2 -BackoffSeconds 0 -Tag 'prune8' -TarExe $wrap 6>&1 | Out-Null
$rc = $LASTEXITCODE
$kept8 = if (Test-Path -LiteralPath $marker) { (Get-Content -LiteralPath $marker -TotalCount 1).Trim() } else { 'none' }
if ($kept8 -eq '2') { Pass '8: a prune keeps the staged copy and the intermediate of a live fetch' } else { Fail "8: files surviving the prune: $kept8" }
if ($rc -eq 0 -and (Test-Path -LiteralPath (Join-Path $dest 'GIT_COMMIT'))) { Pass '8: fetch from an old tarball extracts' } else { Fail "8: exit $rc" }
} finally {
  Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue
}

if ($fails -ne 0) { Write-Host "$fails case(s) failed"; exit 1 }
Write-Host 'All fetch-vendor-tarball cases passed'
exit 0
