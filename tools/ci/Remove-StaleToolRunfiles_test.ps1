# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test for Remove-StaleToolRunfiles.ps1. No Pester dependency.
# Builds a temp output base with:
#   * a tool runfiles tree holding a junction to a populated sibling
#     directory (with a sentinel file) and a dangling junction: removed, and
#     the sentinel and its directory must survive;
#   * a tool runfiles tree without links: kept;
#   * a test (non-exec) runfiles tree holding a junction: kept;
# and checks that an output base outside WIN_BAZEL_OUTPUT_ROOT is refused.
#
# Run:   powershell -NoProfile -File tools\ci\Remove-StaleToolRunfiles_test.ps1
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'Remove-StaleToolRunfiles.ps1'

$fails = 0
function Pass { param([string]$Name) Write-Host ("PASS: " + $Name) }
function Fail { param([string]$Name) Write-Host ("FAIL: " + $Name); $script:fails += 1 }
function New-Junction { param([string]$Link, [string]$Target)
  & $env:ComSpec /c "mklink /J `"$Link`" `"$Target`"" | Out-Null
  if (-not (Test-Path -LiteralPath $Link)) { throw "mklink /J failed for $Link" }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("stale-tool-runfiles-test-" + [guid]::NewGuid().ToString('N'))
$savedRoot = $env:WIN_BAZEL_OUTPUT_ROOT
try {
  $base = Join-Path $root 'abcd1234'
  $out  = Join-Path $base 'execroot\_main\bazel-out'
  $exec = Join-Path $out 'x64_windows-opt-exec-ST-0123\bin\external\tool+'
  $tgt  = Join-Path $out 'x64_windows-fastbuild\bin\pagespeed'

  # The link target: a populated directory with a sentinel.
  $keepDir = Join-Path $exec 'real'
  New-Item -ItemType Directory -Force -Path $keepDir | Out-Null
  $sentinel = Join-Path $keepDir 'sentinel.txt'
  Set-Content -LiteralPath $sentinel -Value 'keep me' -Encoding ascii

  # 1. A stale tool tree: a junction to the populated sibling, a dangling one.
  $stale = Join-Path $exec 'tool.exe.runfiles'
  New-Item -ItemType Directory -Force -Path (Join-Path $stale 'tool+') | Out-Null
  Set-Content -LiteralPath (Join-Path $stale 'MANIFEST') -Value 'x' -Encoding ascii
  New-Junction (Join-Path $stale 'tool+\real') $keepDir
  New-Junction (Join-Path $stale 'tool+\tool.exe') (Join-Path $exec 'not-there-yet.exe')

  # 2. A clean tool tree (no links).
  $clean = Join-Path $exec 'other.exe.runfiles'
  New-Item -ItemType Directory -Force -Path (Join-Path $clean 'tool+') | Out-Null
  Set-Content -LiteralPath (Join-Path $clean 'tool+\other.exe') -Value 'x' -Encoding ascii

  # 3. A test runfiles tree with a junction (target configuration).
  $testTree = Join-Path $tgt 'some_test.exe.runfiles'
  New-Item -ItemType Directory -Force -Path $testTree | Out-Null
  New-Junction (Join-Path $testTree 'real') $keepDir

  $env:WIN_BAZEL_OUTPUT_ROOT = $root
  $log = & $script -OutputBase $base 6>&1 | Out-String
  if (-not (Test-Path -LiteralPath $stale)) { Pass '1: stale tool tree removed' } else { Fail '1: stale tool tree still there' }
  if (Test-Path -LiteralPath $sentinel) { Pass '1: link target contents intact' } else { Fail '1: link target contents were deleted' }
  if ($log -match 'removed: 1') { Pass '1: reports one tree' } else { Fail "1: unexpected report: $log" }
  if (Test-Path -LiteralPath (Join-Path $clean 'tool+\other.exe')) { Pass '2: clean tool tree kept' } else { Fail '2: clean tool tree removed' }
  if (Test-Path -LiteralPath (Join-Path $testTree 'real')) { Pass '3: test runfiles tree kept' } else { Fail '3: test runfiles tree touched' }

  # 4. Refuses an output base outside WIN_BAZEL_OUTPUT_ROOT.
  $env:WIN_BAZEL_OUTPUT_ROOT = Join-Path $root 'elsewhere'
  $refused = $false
  try { & $script -OutputBase $base 6>&1 | Out-Null } catch { $refused = $true }
  if ($refused) { Pass '4: foreign output base refused' } else { Fail '4: foreign output base accepted' }
} finally {
  $env:WIN_BAZEL_OUTPUT_ROOT = $savedRoot
  # rmdir removes the junctions as links, never their targets.
  if (Test-Path -LiteralPath $root) { & $env:ComSpec /c "rmdir /s /q `"$root`"" | Out-Null }
}

if ($fails -ne 0) { Write-Host "$fails case(s) failed"; exit 1 }
Write-Host 'All Remove-StaleToolRunfiles cases passed'
exit 0
