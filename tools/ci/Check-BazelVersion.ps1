# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Fail when the bazel launcher does not run the Bazel version pinned in
    .bazelversion.

.DESCRIPTION
    Bazelisk releases before 1.19.0 find the workspace root, and with it
    .bazelversion, only through a WORKSPACE file. Without one such a launcher
    silently runs the latest Bazel instead. A plain Bazel binary must
    be exactly the pinned version. Twin of tools/ci/check-bazel-version.sh.

.PARAMETER Launcher
    The bazel launcher to check (bazelisk or bazel).

.PARAMETER Root
    The source tree holding .bazelversion (default: the current directory).
#>
param(
    [string]$Launcher = 'bazel',
    [string]$Root = (Get-Location).Path
)

$want = 'bazel ' + (Get-Content -LiteralPath (Join-Path $Root '.bazelversion') -Raw).Trim()
$ErrorActionPreference = 'Continue'
Push-Location -LiteralPath $Root
try {
    # Collect the whole output before picking the first line: cutting the
    # pipeline short (Select-Object -First 1) can stop the launcher while it
    # is still running, which leaves an exit code of -1 behind in
    # $LASTEXITCODE and fails the calling step.
    $out = @(& $Launcher --version 2>$null)
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($rc -ne 0) {
    $msg = "'$Launcher --version' exited with $rc on runner $env:RUNNER_NAME ($env:RUNNER_OS, $env:RUNNER_ENVIRONMENT). See docs/ci/runner-environment.md."
    Write-Host "::error::$msg"
    throw $msg
}
$got = "$($out | Select-Object -First 1)".Trim()
if ($got -ne $want) {
    $msg = "'$Launcher' on runner $env:RUNNER_NAME ($env:RUNNER_OS, $env:RUNNER_ENVIRONMENT) reports '$got', not '$want' from .bazelversion. A Bazelisk launcher must be 1.19.0 or newer; a plain Bazel must be exactly the pinned version. See docs/ci/runner-environment.md."
    Write-Host "::error::$msg"
    throw $msg
}
Write-Host "bazel launcher ok: $got ($Launcher)"
