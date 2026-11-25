# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# build_iis_pipeline_probe.ps1 - Builds the IIS pipeline probe module
# (iis_pipeline_probe.cc, test equipment) into a 64-bit DLL with the Visual
# C++ toolchain found by vswhere. No Bazel: the probe has no dependency
# beyond the Windows SDK.
#
#   .\build_iis_pipeline_probe.ps1 -OutDir <dir>
#
# Writes <OutDir>\iis_pipeline_probe.dll. Exit 0 on success.
# Windows PowerShell 5.1 compatible; ASCII only.

[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutDir)

$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "iis_pipeline_probe.cc"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "no Visual C++ toolchain found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = Join-Path $OutDir "iis_pipeline_probe.dll"
$obj = Join-Path $OutDir "iis_pipeline_probe.obj"
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++17 /EHsc /O2 /W3 /LD /MT `"$src`" /Fo`"$obj`" /Fe`"$out`" /link oleaut32.lib ole32.lib"
& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built $out"
