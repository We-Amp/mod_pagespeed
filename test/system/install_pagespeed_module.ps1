# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

<#
.SYNOPSIS
    Install PageSpeed native module in IIS.

.DESCRIPTION
    This script builds (optionally) and installs the PageSpeed native module
    (pagespeed_iis.dll) into IIS as a global module.

.PARAMETER Build
    Build the module before installing

.PARAMETER Uninstall
    Uninstall the module instead of installing

.PARAMETER SourceRoot
    Path to the mod_pagespeed source root (default: C:\pagespeed)

.PARAMETER Force
    Force reinstallation even if module is already installed

.EXAMPLE
    .\install_pagespeed_module.ps1              # Install pre-built module
    .\install_pagespeed_module.ps1 -Build       # Build and install
    .\install_pagespeed_module.ps1 -Uninstall   # Remove module
#>

param(
    [switch]$Build,
    [switch]$Uninstall,
    [string]$SourceRoot = "C:\pagespeed",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# Configuration
$ModuleName = "PageSpeedModule"
$BazelTarget = "//pagespeed/iis:pagespeed_iis_dll"
$RelativeModulePath = "bazel-bin\pagespeed\iis\pagespeed_iis.dll"
$SystemModulePath = "C:\Windows\System32\inetsrv\pagespeed_iis.dll"

function Write-Status {
    param([string]$Message, [string]$Color = "White")
    Write-Host "[PageSpeed Module] " -NoNewline -ForegroundColor Magenta
    Write-Host $Message -ForegroundColor $Color
}

function Invoke-AppCmd {
    param([string[]]$Arguments)
    $appcmd = "$env:SystemRoot\System32\inetsrv\appcmd.exe"
    if (-not (Test-Path $appcmd)) {
        throw "appcmd.exe not found. Is IIS installed?"
    }
    & $appcmd @Arguments 2>&1
}

function Test-ModuleInstalled {
    try {
        $modules = Invoke-AppCmd list module 2>$null
        return ($modules | Where-Object { $_ -match $ModuleName }) -ne $null
    } catch {
        return $false
    }
}

function Build-Module {
    Write-Status "Building PageSpeed IIS module..."

    # Check if source exists
    if (-not (Test-Path "$SourceRoot\WORKSPACE")) {
        throw "Source root not found at $SourceRoot. Make sure the source is mounted."
    }

    # Change to source directory
    Push-Location $SourceRoot
    try {
        # Load MSVC environment if available
        $vsPath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"
        if (Test-Path "$vsPath\vcvarsall.bat") {
            Write-Status "Loading MSVC environment..." "Gray"
            cmd /c "`"$vsPath\vcvarsall.bat`" x64 && set" | ForEach-Object {
                if ($_ -match "^([^=]+)=(.*)$") {
                    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
                }
            }
        }

        # Build with Bazel
        Write-Status "Running: bazel build --config=windows --config=clang-cl $BazelTarget" "Gray"
        $result = & bazel build --config=windows --config=clang-cl $BazelTarget 2>&1

        if ($LASTEXITCODE -ne 0) {
            Write-Status "Build failed:" "Red"
            $result | Write-Host
            throw "Bazel build failed with exit code $LASTEXITCODE"
        }

        Write-Status "Build successful" "Green"
    } finally {
        Pop-Location
    }
}

function Install-Module {
    $modulePath = Join-Path $SourceRoot $RelativeModulePath

    if (-not (Test-Path $modulePath)) {
        throw "Module DLL not found at $modulePath. Build it first with -Build flag."
    }

    Write-Status "Installing PageSpeed module..."

    # Check if already installed
    if ((Test-ModuleInstalled) -and (-not $Force)) {
        Write-Status "Module is already installed. Use -Force to reinstall." "Yellow"
        return
    }

    # Stop IIS to safely replace DLL
    Write-Status "Stopping IIS..." "Gray"
    Stop-Service W3SVC -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2

    try {
        # Remove existing registration if any
        if (Test-ModuleInstalled) {
            Write-Status "Removing existing module registration..." "Gray"
            try {
                Invoke-AppCmd uninstall module $ModuleName 2>$null | Out-Null
            } catch { }
        }

        # Copy DLL to system location
        Write-Status "Copying DLL to $SystemModulePath" "Gray"
        Copy-Item $modulePath $SystemModulePath -Force

        # Register as global module
        Write-Status "Registering module in IIS..." "Gray"
        Invoke-AppCmd install module /name:$ModuleName /image:$SystemModulePath /add:false | Out-Null

        Write-Status "Module installed successfully" "Green"

        # Verify registration
        if (Test-ModuleInstalled) {
            Write-Status "Module registration verified" "Green"
        } else {
            Write-Status "Warning: Module may not be properly registered" "Yellow"
        }
    } finally {
        # Restart IIS
        Write-Status "Starting IIS..." "Gray"
        Start-Service W3SVC -ErrorAction SilentlyContinue
    }
}

function Uninstall-Module {
    Write-Status "Uninstalling PageSpeed module..."

    # Stop IIS first
    Write-Status "Stopping IIS..." "Gray"
    Stop-Service W3SVC -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2

    try {
        # Remove module registration
        if (Test-ModuleInstalled) {
            Write-Status "Removing module registration..." "Gray"
            try {
                Invoke-AppCmd uninstall module $ModuleName 2>$null | Out-Null
            } catch { }
        } else {
            Write-Status "Module was not registered" "Gray"
        }

        # Remove DLL
        if (Test-Path $SystemModulePath) {
            Write-Status "Removing DLL from $SystemModulePath" "Gray"
            Remove-Item $SystemModulePath -Force
        }

        Write-Status "Module uninstalled successfully" "Green"
    } finally {
        # Restart IIS
        Write-Status "Starting IIS..." "Gray"
        Start-Service W3SVC -ErrorAction SilentlyContinue
    }
}

function Get-ModuleStatus {
    Write-Status "PageSpeed Module Status:"

    # Check DLL in source
    $sourceDll = Join-Path $SourceRoot $RelativeModulePath
    if (Test-Path $sourceDll) {
        $info = Get-Item $sourceDll
        Write-Status "  Built DLL: $sourceDll" "Gray"
        Write-Status "    Size: $([math]::Round($info.Length / 1MB, 2)) MB" "Gray"
        Write-Status "    Modified: $($info.LastWriteTime)" "Gray"
    } else {
        Write-Status "  Built DLL: Not found" "Yellow"
    }

    # Check DLL in system
    if (Test-Path $SystemModulePath) {
        $info = Get-Item $SystemModulePath
        Write-Status "  Installed DLL: $SystemModulePath" "Green"
        Write-Status "    Size: $([math]::Round($info.Length / 1MB, 2)) MB" "Gray"
        Write-Status "    Modified: $($info.LastWriteTime)" "Gray"
    } else {
        Write-Status "  Installed DLL: Not found" "Yellow"
    }

    # Check registration
    if (Test-ModuleInstalled) {
        Write-Status "  IIS Registration: Registered" "Green"
    } else {
        Write-Status "  IIS Registration: Not registered" "Yellow"
    }
}

# Main execution
if ($Uninstall) {
    Uninstall-Module
} else {
    if ($Build) {
        Build-Module
    }
    Install-Module
}

# Show status
Get-ModuleStatus
