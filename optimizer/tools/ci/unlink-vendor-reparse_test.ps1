# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test for unlink-vendor-reparse.ps1. No Pester dependency.
# Builds a temp workspace whose vendor\bazel-external is a junction to a temp
# target holding sentinel.txt, plus a normal vendor\keep\a.txt, and checks:
#   * the junction is removed;
#   * the junction's target (and its sentinel) survives;
#   * normal vendor files are untouched;
#   * a workspace without vendor\ exits 0;
#   * the output reports exactly 1 reparse point.
#
# Run:   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci\unlink-vendor-reparse_test.ps1
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'unlink-vendor-reparse.ps1'

$fails = 0
function Pass { param([string]$Name) Write-Host ("PASS: " + $Name) }
function Fail { param([string]$Name) Write-Host ("FAIL: " + $Name); $script:fails += 1 }

$roots = New-Object System.Collections.Generic.List[string]
function New-TempRoot {
  $p = Join-Path ([System.IO.Path]::GetTempPath()) ("unlink-vendor-reparse-test-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $p | Out-Null
  $roots.Add($p)
  return $p
}

try {
  $root   = New-TempRoot
  $ws     = Join-Path $root 'ws'
  $target = Join-Path $root 'target'
  $vendor = Join-Path $ws 'vendor'
  $link   = Join-Path $vendor 'bazel-external'
  $keep   = Join-Path $vendor 'keep'
  New-Item -ItemType Directory -Path $target, $keep | Out-Null
  [System.IO.File]::WriteAllText((Join-Path $target 'sentinel.txt'), 'x')
  [System.IO.File]::WriteAllText((Join-Path $keep 'a.txt'), 'x')
  New-Item -ItemType Junction -Path $link -Target $target | Out-Null
  # A junction inside the junction target: the script must never list there.
  $other = Join-Path $root 'other'
  New-Item -ItemType Directory -Path $other | Out-Null
  [System.IO.File]::WriteAllText((Join-Path $other 'o.txt'), 'x')
  $inner = Join-Path $target 'inner'
  New-Item -ItemType Junction -Path $inner -Target $other | Out-Null

  $out = $null
  $code = 0
  if (Test-Path -LiteralPath $script) {
    $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File $script -Workspace $ws 2>&1 | Out-String)
    $code = $LASTEXITCODE
  } else {
    Write-Host ("script missing: " + $script)
    $code = 127
  }

  if (-not (Test-Path -LiteralPath $link)) { Pass 'bazel-external junction removed' } else { Fail 'bazel-external junction removed' }
  if (Test-Path -LiteralPath (Join-Path $target 'sentinel.txt')) { Pass 'target sentinel survives' } else { Fail 'target sentinel survives' }
  if (Test-Path -LiteralPath (Join-Path $keep 'a.txt')) { Pass 'normal vendor files untouched' } else { Fail 'normal vendor files untouched' }

  $empty = Join-Path $root 'empty-ws'
  New-Item -ItemType Directory -Path $empty | Out-Null
  if ((Test-Path -LiteralPath $inner) -and (Test-Path -LiteralPath (Join-Path $other 'o.txt'))) { Pass 'junction inside the target survives' } else { Fail 'junction inside the target survives' }

  # bazel-external as a real directory holding one junction and one plain file.
  $ws2 = Join-Path $root 'ws2'
  $bx2 = Join-Path $ws2 'vendor\bazel-external'
  $t2  = Join-Path $root 't2'
  New-Item -ItemType Directory -Path $bx2, $t2 | Out-Null
  [System.IO.File]::WriteAllText((Join-Path $t2 's2.txt'), 'x')
  [System.IO.File]::WriteAllText((Join-Path $bx2 'plain.txt'), 'x')
  New-Item -ItemType Junction -Path (Join-Path $bx2 'j') -Target $t2 | Out-Null
  if (Test-Path -LiteralPath $script) { $null = (& powershell -NoProfile -ExecutionPolicy Bypass -File $script -Workspace $ws2 2>&1 | Out-String) }
  if ((-not (Test-Path -LiteralPath (Join-Path $bx2 'j'))) -and (Test-Path -LiteralPath $bx2) -and (Test-Path -LiteralPath (Join-Path $bx2 'plain.txt')) -and (Test-Path -LiteralPath (Join-Path $t2 's2.txt'))) { Pass 'real bazel-external: inner junction unlinked, directory and files kept' } else { Fail 'real bazel-external: inner junction unlinked, directory and files kept' }

  # vendor itself a junction: unlinked, target kept.
  $ws3 = Join-Path $root 'ws3'
  New-Item -ItemType Directory -Path $ws3 | Out-Null
  New-Item -ItemType Junction -Path (Join-Path $ws3 'vendor') -Target $t2 | Out-Null
  if (Test-Path -LiteralPath $script) { $null = (& powershell -NoProfile -ExecutionPolicy Bypass -File $script -Workspace $ws3 2>&1 | Out-String) }
  if ((-not (Test-Path -LiteralPath (Join-Path $ws3 'vendor'))) -and (Test-Path -LiteralPath (Join-Path $t2 's2.txt'))) { Pass 'vendor junction unlinked, target kept' } else { Fail 'vendor junction unlinked, target kept' }

  # Dangling vendor junction (target gone): still unlinked, exit 0.
  $ws4 = Join-Path $root 'ws4'
  $t4  = Join-Path $root 't4'
  New-Item -ItemType Directory -Path $ws4, $t4 | Out-Null
  New-Item -ItemType Junction -Path (Join-Path $ws4 'vendor') -Target $t4 | Out-Null
  Remove-Item -LiteralPath $t4 -Force
  $code4 = 127
  if (Test-Path -LiteralPath $script) {
    $null = (& powershell -NoProfile -ExecutionPolicy Bypass -File $script -Workspace $ws4 2>&1 | Out-String)
    $code4 = $LASTEXITCODE
  }
  $stillThere = $true
  try { $null = [IO.File]::GetAttributes((Join-Path $ws4 'vendor')) } catch { $stillThere = $false }
  if ($code4 -eq 0 -and -not $stillThere) { Pass 'dangling vendor junction is unlinked' } else { Fail 'dangling vendor junction is unlinked' }

  $emptyCode = 127
  if (Test-Path -LiteralPath $script) {
    $null = (& powershell -NoProfile -ExecutionPolicy Bypass -File $script -Workspace $empty 2>&1 | Out-String)
    $emptyCode = $LASTEXITCODE
  }
  if ($emptyCode -eq 0) { Pass 'absent vendor dir exits 0' } else { Fail 'absent vendor dir exits 0' }

  if ($code -eq 0 -and $out -match 'unlinked 1 reparse point\(s\)') { Pass 'output reports 1 reparse point' } else { Fail ('output reports 1 reparse point (exit ' + $code + ', output: ' + $out + ')') }
}
finally {
  foreach ($r in $roots) {
    # Drop any junction first so cleanup never follows it into a target.
    foreach ($j in (Get-ChildItem -LiteralPath $r -Recurse -Force -ErrorAction SilentlyContinue | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })) {
      try { [IO.Directory]::Delete($j.FullName, $false) } catch { }
    }
    Remove-Item -LiteralPath $r -Recurse -Force -ErrorAction SilentlyContinue
  }
}

if ($fails -gt 0) { Write-Host ("FAILED: " + $fails); exit 1 }
Write-Host 'ALL PASS'
exit 0
