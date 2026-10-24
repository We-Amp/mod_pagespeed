# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test for Prune-WinDiskCache.ps1. No Pester dependency.
# Builds a temp disk cache with files of known size and age and checks that
# the prune removes the oldest files first until ac + cas fit the cap, goes
# through an action-cache entry before a content blob of the same age, keeps
# everything when under the cap or when no cap is given, leaves files outside
# ac/ and cas/ alone, and does not fail on a missing directory or a bad cap.
#
# Run:   powershell -NoProfile -File tools\ci\Prune-WinDiskCache_test.ps1
#   or:  pwsh -NoProfile -File tools/ci/Prune-WinDiskCache_test.ps1
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'Prune-WinDiskCache.ps1'

$fails = 0
function Pass { param([string]$Name) Write-Host ("PASS: " + $Name) }
function Fail { param([string]$Name) Write-Host ("FAIL: " + $Name); $script:fails += 1 }
function Check { param([string]$Name, [bool]$Ok) if ($Ok) { Pass $Name } else { Fail $Name } }

$roots = New-Object System.Collections.Generic.List[string]
function New-Cache {
  $p = Join-Path ([System.IO.Path]::GetTempPath()) ("prune-win-diskcache-test-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path (Join-Path $p 'ac\ab'), (Join-Path $p 'cas\cd') | Out-Null
  $roots.Add($p)
  return $p
}
function New-Blob {
  param([string]$Path, [int]$Bytes, [double]$AgeHours)
  [System.IO.File]::WriteAllBytes($Path, (New-Object byte[] $Bytes))
  (Get-Item -LiteralPath $Path).LastWriteTime = (Get-Date).AddHours(-$AgeHours)
}
function Run-Prune {
  param([string]$Dir, [string]$Cap)
  $out = & $script -CacheDir $Dir -MaxSize $Cap 2>&1 | Out-String
  return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

try {
# 1: oldest first until it fits. Five 1 KB files, cap 3K: the two oldest go.
$c = New-Cache
New-Blob "$c\cas\cd\h1" 1024 50
New-Blob "$c\cas\cd\h2" 1024 40
New-Blob "$c\cas\cd\h3" 1024 30
New-Blob "$c\ac\ab\h4" 1024 20
New-Blob "$c\cas\cd\h5" 1024 10
New-Blob "$c\elsewhere.txt" 4096 99
$r = Run-Prune $c '3K'
Check 'oldest-first: exit 0' ($r.Code -eq 0 -or $null -eq $r.Code)
Check 'oldest-first: two oldest removed' (-not (Test-Path "$c\cas\cd\h1") -and -not (Test-Path "$c\cas\cd\h2"))
Check 'oldest-first: three newest kept' ((Test-Path "$c\cas\cd\h3") -and (Test-Path "$c\ac\ab\h4") -and (Test-Path "$c\cas\cd\h5"))
Check 'oldest-first: file outside ac/ and cas/ untouched' (Test-Path "$c\elsewhere.txt")
Check 'oldest-first: log has before and after' ($r.Out -match '5120 bytes before' -and $r.Out -match '3072 bytes kept')

# 2: at the same age, the action-cache entry goes first.
$c = New-Cache
$t = (Get-Date).AddHours(-5)
New-Blob "$c\ac\ab\a1" 1024 5
New-Blob "$c\cas\cd\c1" 1024 5
(Get-Item "$c\ac\ab\a1").LastWriteTime = $t
(Get-Item "$c\cas\cd\c1").LastWriteTime = $t
$r = Run-Prune $c '1K'
Check 'tie: ac entry removed' (-not (Test-Path "$c\ac\ab\a1"))
Check 'tie: cas blob kept' (Test-Path "$c\cas\cd\c1")

# 3: under the cap, nothing goes.
$c = New-Cache
New-Blob "$c\cas\cd\h1" 1024 50
$r = Run-Prune $c '1M'
Check 'under cap: kept' (Test-Path "$c\cas\cd\h1")

# 4: no cap, nothing goes.
$c = New-Cache
New-Blob "$c\cas\cd\h1" 1024 50
$r = Run-Prune $c ''
Check 'no cap: kept' (Test-Path "$c\cas\cd\h1")

# 5: an unparsable cap warns and keeps everything.
$r = Run-Prune $c 'lots'
Check 'bad cap: kept' (Test-Path "$c\cas\cd\h1")
Check 'bad cap: warning' ($r.Out -match 'cannot read the size cap')

# 6: a missing directory is not an error.
$r = Run-Prune (Join-Path ([System.IO.Path]::GetTempPath()) 'prune-win-diskcache-missing') '1K'
Check 'missing dir: exit 0' ($r.Code -eq 0 -or $null -eq $r.Code)
Check 'missing dir: logged' ($r.Out -match 'does not exist')
# 7: a lowercase unit and a raw byte value of more than six digits parse.
$c = New-Cache
New-Blob "$c\cas\cd\h1" 1024 50
$r = Run-Prune $c '20g'
Check 'lowercase unit: kept' ((Test-Path "$c\cas\cd\h1") -and $r.Out -notmatch 'cannot read')
$r = Run-Prune $c '20000000000'
Check 'raw bytes over six digits: parsed' ($r.Out -notmatch 'cannot read')
Check 'raw bytes over six digits: kept' (Test-Path "$c\cas\cd\h1")
New-Blob "$c\cas\cd\h2" 1024 10
$r = Run-Prune $c '1k'
Check 'lowercase unit: over cap prunes oldest' (-not (Test-Path "$c\cas\cd\h1") -and (Test-Path "$c\cas\cd\h2"))
} finally {
  foreach ($p in $roots) { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($fails -gt 0) { Write-Host "$fails check(s) failed"; exit 1 }
Write-Host "all checks passed"
exit 0
