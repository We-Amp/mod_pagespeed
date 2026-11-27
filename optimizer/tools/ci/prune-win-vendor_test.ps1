# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test for prune-win-vendor.ps1. No Pester dependency.
# Builds a temp vendor dir with aged and fresh files and checks what the
# prune keeps:
#   * this run's tarball, sidecar and decompressed .tar, however old (and
#     the tarball is re-stamped);
#   * a fresh tarball of another SHA, and its aged sidecar;
#   * fresh temp files (a live run's scp or zstd output);
#   * unrelated files;
#   * everything, when the dir is the shared one (-SharedDir).
# and what it removes: an aged tarball of another SHA with its sidecar, an
# orphaned aged sidecar, aged *.tmp, *.tar.zst.tar, *.tar.zst.*.tar and
# *.tar.zst.*.stage files; and that a skewed clock disables the prune. On Windows
# it also checks a delete failure (a file held open without delete
# sharing): a warning, the job goes on, and that tarball keeps its sidecar.
#
# Run:   powershell -NoProfile -File tools\ci\prune-win-vendor_test.ps1
#   or:  pwsh -NoProfile -File tools/ci/prune-win-vendor_test.ps1
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'prune-win-vendor.ps1'

$fails = 0
function Pass { param([string]$Name) Write-Host ("PASS: " + $Name) }
function Fail { param([string]$Name) Write-Host ("FAIL: " + $Name); $script:fails += 1 }

$roots = New-Object System.Collections.Generic.List[string]
function New-TempRoot {
  $p = Join-Path ([System.IO.Path]::GetTempPath()) ("prune-win-vendor-test-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $p | Out-Null
  $roots.Add($p)
  return $p
}

function New-File {
  param([string]$Dir, [string]$Name, [double]$AgeHours)
  $p = Join-Path $Dir $Name
  [System.IO.File]::WriteAllText($p, 'x')
  (Get-Item -LiteralPath $p).LastWriteTime = (Get-Date).AddHours(-$AgeHours)
  return $p
}

$cur = 'aaaaaaa'
$pre = 'pagespeed-optimizer-'

try {
# 1: the keep/remove matrix.
$root = New-TempRoot
$expectKept = @(
  (New-File $root "$pre$cur.tar.zst" 5),                     # this run's tarball
  (New-File $root "$pre$cur.tar.zst.sha256" 5),              # ...and its sidecar
  (New-File $root "$pre$cur.tar.zst.tar" 5),                 # ...and its .tar
  (New-File $root "${pre}bbbbbbb.tar.zst" 0.5),              # fresh, other SHA
  (New-File $root "${pre}bbbbbbb.tar.zst.sha256" 5),         # aged sidecar of a kept tarball
  (New-File $root "${pre}ccccccc.tar.zst.sha256" 0.5),       # fresh orphan sidecar
  (New-File $root "${pre}ddddddd.tar.zst.111-1.tmp" 0.2),    # live run's scp temp
  (New-File $root "${pre}ddddddd.tar.zst.sha256.111-1.tmp" 0.2),
  (New-File $root "${pre}ddddddd.tar.zst.tar" 0.2),          # live run's zstd output
  (New-File $root "${pre}mmmmmmm.tar.zst.444-1-5.stage" 0.2), # live job's staged copy
  (New-File $root "${pre}mmmmmmm.tar.zst.444-1-5.tar" 0.2),   # live job's intermediate
  (New-File $root "unrelated.tar.zst" 9),                    # not ours
  (New-File $root "${pre}eeeeeee.tar.zst.keep" 9)            # not a pattern we own
)
$expectGone = @(
  (New-File $root "${pre}fffffff.tar.zst" 3),                # aged, other SHA
  (New-File $root "${pre}fffffff.tar.zst.sha256" 3),         # ...its sidecar
  (New-File $root "${pre}ggggggg.tar.zst.sha256" 3),         # aged orphan sidecar
  (New-File $root "${pre}hhhhhhh.tar.zst.222-3.tmp" 3),      # cancelled scp
  (New-File $root "${pre}hhhhhhh.tar.zst.sha256.222-3.tmp" 3),
  (New-File $root "${pre}iiiiiii.tar.zst.tar" 3),            # cancelled extract
  (New-File $root "${pre}lllllll.tar.zst.333-1-77.stage" 3), # cancelled staged copy
  (New-File $root "${pre}lllllll.tar.zst.333-1-77.stage.sha256" 3),
  (New-File $root "${pre}lllllll.tar.zst.333-1-77.tar" 3)    # cancelled per-job intermediate
)
& $script -VendorDir $root -ShortSha $cur -SharedDir 'Z:\no\such\shared' | Out-Null
foreach ($p in $expectKept) {
  if (Test-Path -LiteralPath $p) { Pass ("1: kept " + (Split-Path -Leaf $p)) } else { Fail ("1: removed " + (Split-Path -Leaf $p)) }
}
foreach ($p in $expectGone) {
  if (Test-Path -LiteralPath $p) { Fail ("1: kept " + (Split-Path -Leaf $p)) } else { Pass ("1: removed " + (Split-Path -Leaf $p)) }
}
$age = (Get-Date) - (Get-Item -LiteralPath (Join-Path $root "$pre$cur.tar.zst")).LastWriteTime
if ($age.TotalMinutes -lt 5) { Pass '1: this run''s tarball re-stamped' } else { Fail "1: this run's tarball not re-stamped (age $($age.TotalMinutes) min)" }

# 2: the shared dir is left alone.
$root = New-TempRoot
$old = New-File $root "${pre}fffffff.tar.zst" 3
& $script -VendorDir $root -ShortSha $cur -SharedDir ($root + '\') | Out-Null
if (Test-Path -LiteralPath $old) { Pass '2: shared dir untouched' } else { Fail '2: pruned the shared dir' }

# 3: a missing vendor dir is a no-op that exits 0.
$missing = Join-Path ([System.IO.Path]::GetTempPath()) ("prune-win-vendor-missing-" + [guid]::NewGuid().ToString('N'))
& $script -VendorDir $missing -ShortSha $cur | Out-Null
if ($LASTEXITCODE -eq 0) { Pass '3: missing dir exits 0' } else { Fail "3: missing dir exit $LASTEXITCODE" }

# 3b: a clock that disagrees with the file times disables the prune. The
# skew limit below zero makes any probe read as skewed.
$root = New-TempRoot
$old = New-File $root "${pre}fffffff.tar.zst" 3
$out = & $script -VendorDir $root -ShortSha $cur -SharedDir 'Z:\no\such\shared' -MaxSkewSeconds -1 6>&1 | Out-String
if ((Test-Path -LiteralPath $old) -and $out -match 'not pruning') { Pass '3b: skewed clock disables the prune' } else { Fail "3b: skew guard: $out" }
if (-not (Get-ChildItem -LiteralPath $root -Force | Where-Object { $_.Name -like '.clock-probe.*' })) { Pass '3b: probe file removed' } else { Fail '3b: probe file left behind' }

# 4: a delete failure (Windows only: elsewhere an open file can be deleted).
if ([Environment]::OSVersion.Platform -eq 'Win32NT') {
  $root = New-TempRoot
  $locked = New-File $root "${pre}jjjjjjj.tar.zst" 3
  $side = New-File $root "${pre}jjjjjjj.tar.zst.sha256" 3
  $other = New-File $root "${pre}kkkkkkk.tar.zst" 3
  $h = [System.IO.File]::Open($locked, 'Open', 'Read', 'None')
  try {
    $out = & $script -VendorDir $root -ShortSha $cur -SharedDir 'Z:\no\such\shared' 6>&1 | Out-String
    $rc = $LASTEXITCODE
  } finally { $h.Dispose() }
  if ($rc -eq 0) { Pass '4: delete failure exits 0' } else { Fail "4: delete failure exit $rc" }
  if ($out -match '::warning::prune-win-vendor: could not remove') { Pass '4: delete failure warns' } else { Fail "4: no warning: $out" }
  if ((Test-Path -LiteralPath $locked) -and (Test-Path -LiteralPath $side)) { Pass '4: undeletable tarball keeps its sidecar' } else { Fail '4: undeletable tarball lost its sidecar' }
  if (-not (Test-Path -LiteralPath $other)) { Pass '4: the rest still pruned' } else { Fail '4: one failure stopped the prune' }
} else {
  Write-Host 'SKIP: 4: delete failure (needs Windows file sharing semantics)'
}
} finally {
  foreach ($r in $roots) { Remove-Item -Recurse -Force -LiteralPath $r -ErrorAction SilentlyContinue }
}

if ($fails -ne 0) { Write-Host "$fails case(s) failed"; exit 1 }
Write-Host 'All prune-win-vendor cases passed'
exit 0
