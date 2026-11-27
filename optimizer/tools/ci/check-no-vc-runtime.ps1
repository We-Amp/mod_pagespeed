# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The shipped Windows binaries need no Visual C++ runtime.
#
# factory_worker.exe and pagespeed.dll must run on a stock Windows Server,
# which has the in-box UCRT (api-ms-win-crt-*) but no Visual C++ runtime. One
# archive built against the DLL runtime (/MD, or a Rust staticlib without
# crt-static) is enough to add vcruntime140.dll to the import table, and the
# binary then fails to start there with 0xC0000135.
#
# Usage: check-no-vc-runtime.ps1 -Binaries <path>[,<path>...]
# Exits 1 when a binary is missing, its import table cannot be read or is
# empty, or it imports any Visual C++ runtime DLL.
param([Parameter(Mandatory = $true)][string[]]$Binaries)
$ErrorActionPreference = 'Continue'

# dumpbin ships with the MSVC tools; a job's PATH need not carry them (Bazel
# finds the compiler on its own), so ask the installer.
$dumpbin = (Get-Command dumpbin -ErrorAction SilentlyContinue).Source
if (-not $dumpbin) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $dumpbin = & $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
  }
}
if (-not $dumpbin) {
  Write-Host "::error::dumpbin.exe not found (no MSVC tools on this runner?)"
  exit 1
}

# Every Visual C++ runtime DLL: release and debug, and the add-ons
# (msvcp140_atomic_wait, vcruntime140_threads, concrt, vcomp, vccorlib).
$runtimePattern = '^(vcruntime|msvcp|concrt|vcomp|vccorlib|ucrtbased)'
$bad = 0
foreach ($bin in $Binaries) {
  $name = Split-Path $bin -Leaf
  if (-not (Test-Path $bin)) {
    Write-Host "::error::$name not found at $bin"
    $bad++
    continue
  }
  $out = & $dumpbin /nologo /dependents $bin
  if ($LASTEXITCODE -ne 0) {
    Write-Host "::error::dumpbin failed on $name (exit $LASTEXITCODE)"
    $bad++
    continue
  }
  # Only the import list: dumpbin's own "Dump of file <path>" header also
  # ends in .dll and would otherwise be read as an import.
  $deps = @($out | ForEach-Object { $_.Trim() } | Where-Object { ($_ -match '\.dll$') -and ($_ -notmatch '^Dump of file ') })
  Write-Host "$name imports: $($deps -join ', ')"
  if ($deps.Count -eq 0) {
    Write-Host "::error::no imports read from $name; the check would prove nothing"
    $bad++
    continue
  }
  $runtime = @($deps | Where-Object { $_ -match $runtimePattern })
  if ($runtime.Count -gt 0) {
    Write-Host "::error::$name imports the Visual C++ runtime: $($runtime -join ', ')"
    $bad++
  }
}
if ($bad -gt 0) { exit 1 }
Write-Host "No Visual C++ runtime imported by: $(($Binaries | ForEach-Object { Split-Path $_ -Leaf }) -join ', ')"
