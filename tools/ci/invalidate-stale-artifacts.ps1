# SPDX-License-Identifier: BUSL-1.1
# Ported from pagespeed-optimizer tools/ci/ (corp the design record hub) -- keep the copies in sync.
# Windows counterpart to invalidate-stale-artifacts.sh. Wipes a local CI
# artifacts staging dir whose metadata.json reports a different commit SHA
# than expected, protecting against concurrent-PR contamination on dedicated
# shared runners.
param(
  [Parameter(Mandatory=$true)][string]$Staging,
  [Parameter(Mandatory=$true)][string]$Expected
)

$meta = Join-Path $Staging 'metadata.json'
if (-not (Test-Path $meta)) { exit 0 }

try {
  $localSha = (Get-Content $meta -Raw | ConvertFrom-Json).commit
} catch {
  $localSha = $null
}

if ($localSha -ne $Expected) {
  $shown = if ($localSha) { $localSha } else { '<unknown>' }
  Write-Host ("::warning::Local {0} is for {1}, expected {2} -- invalidating" -f $Staging, $shown, $Expected)
  if (Test-Path $Staging) { Remove-Item $Staging -Recurse -Force }
}
