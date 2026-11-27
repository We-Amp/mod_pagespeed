# SPDX-License-Identifier: Apache-2.0
# Smoke tests for invalidate-stale-artifacts.ps1.
# Mirrors invalidate-stale-artifacts_test.sh. No Pester dependency.
#
# Run:   pwsh -NoProfile -File tools/ci/invalidate-stale-artifacts_test.ps1
#   or:  powershell -NoProfile -File tools\ci\invalidate-stale-artifacts_test.ps1
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'invalidate-stale-artifacts.ps1'

function New-TempRoot {
  $p = Join-Path ([System.IO.Path]::GetTempPath()) ("invalidate-test-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $p | Out-Null
  return $p
}

function Write-Meta {
  param([string]$Dir, [string]$Sha)
  New-Item -ItemType Directory -Force -Path $Dir | Out-Null
  ('{{"commit":"{0}","branch":"main","run":"x"}}' -f $Sha) | Set-Content -Path (Join-Path $Dir 'metadata.json') -Encoding ascii
}

function Pass { param([string]$Name) Write-Host ("ok: " + $Name) }
function Fail { param([string]$Name) Write-Error ("FAIL: " + $Name); exit 1 }

function Invoke-Script {
  param([string]$Staging, [string]$Expected)
  & $script -Staging $Staging -Expected $Expected | Out-Null
}

# 1: matching SHA -- staging survives
$root = New-TempRoot; $staging = Join-Path $root 'a'
Write-Meta $staging 'abc1234567890'
New-Item -ItemType File -Path (Join-Path $staging 'libpagespeed.dll') | Out-Null
Invoke-Script -Staging $staging -Expected 'abc1234567890'
if (-not (Test-Path (Join-Path $staging 'libpagespeed.dll'))) { Fail '1: staging wiped when SHAs match' }
Pass '1: matching SHA preserves staging'
Remove-Item -Recurse -Force $root

# 2: mismatched SHA -- staging wiped
$root = New-TempRoot; $staging = Join-Path $root 'a'
Write-Meta $staging 'abc1234567890'
New-Item -ItemType File -Path (Join-Path $staging 'libpagespeed.dll') | Out-Null
Invoke-Script -Staging $staging -Expected 'deadbeef1234'
if (Test-Path $staging) { Fail '2: staging survived wipe when SHAs mismatch' }
Pass '2: mismatched SHA invalidates staging'
Remove-Item -Recurse -Force $root

# 3: missing metadata -- no-op, staging untouched
$root = New-TempRoot; $staging = Join-Path $root 'a'
New-Item -ItemType Directory -Force -Path $staging | Out-Null
New-Item -ItemType File -Path (Join-Path $staging 'libpagespeed.dll') | Out-Null
Invoke-Script -Staging $staging -Expected 'abc1234567890'
if (-not (Test-Path (Join-Path $staging 'libpagespeed.dll'))) { Fail '3: staging without metadata was wiped' }
Pass '3: missing metadata is a no-op'
Remove-Item -Recurse -Force $root

# 4: absent staging dir -- no-op, exits 0
$root = New-TempRoot; $staging = Join-Path $root 'missing'
Invoke-Script -Staging $staging -Expected 'abc1234567890'
Pass '4: absent staging dir is a no-op'
Remove-Item -Recurse -Force $root

# 5: malformed metadata (no commit key) -- treated as mismatch, wiped
$root = New-TempRoot; $staging = Join-Path $root 'a'
New-Item -ItemType Directory -Force -Path $staging | Out-Null
'{"branch":"main"}' | Set-Content -Path (Join-Path $staging 'metadata.json') -Encoding ascii
New-Item -ItemType File -Path (Join-Path $staging 'libpagespeed.dll') | Out-Null
Invoke-Script -Staging $staging -Expected 'abc1234567890'
if (Test-Path $staging) { Fail '5: malformed metadata was not wiped' }
Pass '5: malformed metadata invalidates staging'
Remove-Item -Recurse -Force $root

# 6: pretty-printed JSON (whitespace around ':') -- matching SHA preserves staging
$root = New-TempRoot; $staging = Join-Path $root 'a'
New-Item -ItemType Directory -Force -Path $staging | Out-Null
@'
{
  "commit": "abc1234567890",
  "branch": "main",
  "run": "x"
}
'@ | Set-Content -Path (Join-Path $staging 'metadata.json') -Encoding ascii
New-Item -ItemType File -Path (Join-Path $staging 'libpagespeed.dll') | Out-Null
Invoke-Script -Staging $staging -Expected 'abc1234567890'
if (-not (Test-Path (Join-Path $staging 'libpagespeed.dll'))) { Fail '6: pretty-printed matching SHA wiped staging' }
Pass '6: pretty-printed JSON parsed (matching SHA preserves)'
Remove-Item -Recurse -Force $root

Write-Host 'ALL PASS'
