<#
.SYNOPSIS
  Stand up an isolated IIS site/pool serving the ASan-instrumented pagespeed_iis.dll,
  for the shutdown/recycle memory-bug stress rig.

.DESCRIPTION
  Mirrors the hand-built D:\stress349 rig that first surfaced the
  __asan_default_options drive-letter crash-loop -- promoted into the repo so the
  recipe is version-controlled instead of living on one build host's D: drive.

  Why a DEDICATED pool/site + why we remove the CI PageSpeedModule:
  PageSpeedModule registers a process-wide GL_PRE_BEGIN_REQUEST handler. Two
  pagespeed DLLs in one w3wp cross-talk, so the server-level CI module is
  uninstalled here (its exact globalModules image path is snapshotted to
  -RigDir\ci-module-image.txt) and restored by cleanup_iis_asan_rig.ps1. Note
  setup_iis_full.ps1 reinstalls PageSpeedModule from scratch every IIS-systest
  run, so a missed restore self-heals on the next run -- but we restore anyway.

  The ASan runtime (clang_rt.asan_dynamic-x86_64.dll) is copied ALONGSIDE the
  module so the Windows loader resolves it from the module's own directory.
  ASAN_OPTIONS is baked into the DLL via __asan_default_options in
  pagespeed/iis/dll_main.cc (incl. the log_path quoting fix) -- w3wp does
  not inherit the CI shell env, so there is nothing to set here.
#>
param(
  [string]$RigDir      = $(if (Test-Path 'D:\') { 'D:\iis-asan-rig' } else { 'C:\iis-asan-rig' }),
  [Parameter(Mandatory=$true)][string]$DllSourceDir,   # dir holding the freshly built ASan pagespeed_iis.dll (+ .pdb)
  [string]$AsanRuntime = '',                            # clang_rt.asan_dynamic-x86_64.dll; auto-detected if empty
  [int]$Port           = 18180,
  [string]$PoolName    = 'PageSpeedAsanStressPool',
  [string]$SiteName    = 'PageSpeedAsanStressSite',
  [string]$ModuleName  = 'PageSpeedAsanModule',
  [string]$RepoDir     = $(Get-Location),               # repo root, for tools/stress/stress_shutdown.py gen-corpus
  [string]$CoredumpDir = 'C:\CrashDumps',
  [int]$DumpCount      = 5
)
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$ac = "$env:SystemRoot\System32\inetsrv\appcmd.exe"

# 0. Hermetic-IIS preamble ( validation-run finding). The runner is
# shared with the CI workflow's Windows AppVerif / IIS jobs: a LEFTOVER w3wp from a prior
# job -- possibly spawned under full Application Verifier -- can idle into our
# stress window and die there (verifier Leak stop at DLL unload), which our
# machine-wide WER LocalDumps + event sweep then attribute to this rig
# (observed: DefaultAppPool worker from the AppVerif job, vrfcore stop 0x900).
# So: clear any verifier IFEO residue for w3wp.exe, restart IIS to kill every
# leftover worker NOW, and let WER settle -- all BEFORE the rig-start marker is
# written, so any events/dumps from those terminations land outside the sweep
# window (the sweep's skew buffer is 30s; we settle for 60s).
$ifeo = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\w3wp.exe'
if (Test-Path $ifeo) {
  $vals = Get-ItemProperty $ifeo
  foreach ($name in @('GlobalFlag','VerifierDlls','VerifierFlags')) {
    if ($vals.PSObject.Properties.Name -contains $name) {
      Write-Warning "IFEO residue on w3wp.exe: $name=$($vals.$name) -- removing (left by a prior AppVerif run?)"
      Remove-ItemProperty -Path $ifeo -Name $name -ErrorAction SilentlyContinue
    }
  }
}
Write-Host "iisreset to clear leftover w3wp instances from prior jobs..."
& iisreset /restart | Out-Null
Write-Host "iisreset exit: $LASTEXITCODE; settling 60s so leftover-worker WER events/dumps land outside the sweep window"
Start-Sleep -Seconds 60

function Resolve-AsanRuntime {
  param([string]$Explicit)
  if ($Explicit -and (Test-Path $Explicit)) { return $Explicit }
  $cands = @(
    "C:\Program Files\LLVM\lib\clang\19\lib\windows\clang_rt.asan_dynamic-x86_64.dll",
    "C:\Program Files\LLVM\lib\clang\18\lib\windows\clang_rt.asan_dynamic-x86_64.dll",
    "C:\Windows\System32\clang_rt.asan_dynamic-x86_64.dll"
  )
  foreach ($c in $cands) { if (Test-Path $c) { return $c } }
  # Last resort: search the LLVM tree
  $found = Get-ChildItem "C:\Program Files\LLVM" -Recurse -Filter 'clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($found) { return $found.FullName }
  return $null
}

# 1. Directories
@($RigDir, "$RigDir\html", "$RigDir\cache", "$RigDir\bin", "$RigDir\logs") | ForEach-Object {
  New-Item -ItemType Directory -Path $_ -Force | Out-Null
}
New-Item -ItemType File -Path "$RigDir\logs\error.log" -Force | Out-Null

# 1b. Rig-start marker: sweep_iis_asan_rig.ps1 derives its event/dump window from
# this, so the sweep covers the whole rig lifetime (setup included) in ONE
# timezone. Previously the sweep window was sweep-start-based and only matched the
# stress window via an accidental UTC-vs-local skew (rig gap 2).
"started $(Get-Date -Format o)" | Set-Content "$RigDir\logs\rig.setup.started"

# 1c. WER LocalDumps for w3wp.exe (rig gap 1). procdump -e -w attaches
# to the FIRST w3wp instance only, so under recycle churn nearly every crashing
# worker was a later, unmonitored instance: 8 w3wp faults, zero dumps captured.
# LocalDumps is consulted by WER per-crash, so EVERY w3wp instance is covered.
# cleanup_iis_asan_rig.ps1 deletes the key. The AppVerif lanes (the CI workflow
# win-appverif, nightly-iis-appverif) now arm the same key via
# tools/ci/Set-WerLocalDumps.ps1, so this is no longer the only writer: both
# sides arm idempotently, tolerate the key already being gone at cleanup, and
# scope any dump purge to their own run window by timestamp. The schedules are
# still disjoint, so an actual overlap should not occur; this just makes one
# non-destructive.
$werKey = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\w3wp.exe'
New-Item -ItemType Directory -Path $CoredumpDir -Force | Out-Null
New-Item -Path $werKey -Force | Out-Null
Set-ItemProperty -Path $werKey -Name DumpFolder -Value $CoredumpDir -Type ExpandString
Set-ItemProperty -Path $werKey -Name DumpType   -Value 2 -Type DWord           # 2 = full dump (heap needed for UAF triage)
Set-ItemProperty -Path $werKey -Name DumpCount  -Value $DumpCount -Type DWord
Write-Host "WER LocalDumps enabled for w3wp.exe -> $CoredumpDir (full dumps, keep $DumpCount)"

# 2. ACL: IIS_IUSRS full control on the tree (the app-pool identity is a member)
$rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
  "IIS_IUSRS","FullControl","ContainerInherit,ObjectInherit","None","Allow")
$acl = Get-Acl $RigDir
$acl.AddAccessRule($rule)
Set-Acl $RigDir $acl
Write-Host "ACL set on $RigDir"

# 3. Copy ASan module (+pdb) and the ASan runtime alongside it
$srcDll = Join-Path $DllSourceDir 'pagespeed_iis.dll'
if (-not (Test-Path $srcDll)) { Write-Error "ASan module not found at $srcDll"; exit 1 }
Copy-Item $srcDll "$RigDir\bin\pagespeed_iis.dll" -Force
$srcPdb = Join-Path $DllSourceDir 'pagespeed_iis.pdb'
if (Test-Path $srcPdb) { Copy-Item $srcPdb "$RigDir\bin\pagespeed_iis.pdb" -Force }
$rt = Resolve-AsanRuntime -Explicit $AsanRuntime
if (-not $rt) { Write-Error "Could not locate clang_rt.asan_dynamic-x86_64.dll (pass -AsanRuntime)"; exit 1 }
Copy-Item $rt "$RigDir\bin\" -Force
Write-Host "Module copied ($((Get-Item "$RigDir\bin\pagespeed_iis.dll").Length) bytes); ASan runtime from $rt"

# 4. Generate the self-contained rewrite corpus into <docroot>\stress (8 pages, 4 css, 4 js, 6 png)
#    Resolve a REAL python: prefer the explicit installs -- `Get-Command python`
#    first hits the WindowsApps App-Execution-Alias stub, which fails to run.
$py = @("C:\Program Files\Python312\python.exe","$env:LOCALAPPDATA\Programs\Python\Python312\python.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $py) { $py = (Get-Command python -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch 'WindowsApps' } | Select-Object -First 1).Source }
if (-not $py) { Write-Error "No usable python found (WindowsApps stub excluded)"; exit 1 }
& $py "$RepoDir\tools\stress\stress_shutdown.py" gen-corpus --out "$RigDir\html\stress"
if ($LASTEXITCODE -ne 0) { Write-Error "gen-corpus failed (exit $LASTEXITCODE)"; exit 1 }

# 5. web.config (allowDoubleEscaping for .pagespeed combined URLs, as the CI IIS tests do)
@'
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
  <system.webServer>
    <defaultDocument enabled="true"><files><clear /><add value="index.html" /></files></defaultDocument>
    <directoryBrowse enabled="false" />
    <httpErrors errorMode="Detailed" />
    <security>
      <requestFiltering allowDoubleEscaping="true">
        <requestLimits maxUrl="16384" maxQueryString="8192" />
      </requestFiltering>
    </security>
  </system.webServer>
</configuration>
'@ | Set-Content "$RigDir\html\web.config" -Encoding UTF8

# 6. Site-level pagespeed.config (flat file in docroot, the design record)
@"
pagespeed on
pagespeed RewriteLevel CoreFilters
pagespeed FileCachePath $RigDir\cache
pagespeed Statistics on
pagespeed StatisticsLogging on
pagespeed EnableCachePurge on
pagespeed RateLimitBackgroundFetches on
pagespeed InPlaceResourceOptimization on
pagespeed CriticalImagesBeaconEnabled false
pagespeed BlockingRewriteKey psatest
pagespeed MessageBufferSize 100000
pagespeed StatisticsPath /mod_pagespeed_statistics
pagespeed MessagesPath /mod_pagespeed_message
"@ | Set-Content "$RigDir\html\pagespeed.config" -Encoding UTF8

'<!doctype html><html><head><title>iis-asan-rig</title></head><body>ok</body></html>' |
  Set-Content "$RigDir\html\index.html" -Encoding UTF8
Write-Host "content + config written"

# 7. Snapshot then remove the CI PageSpeedModule for run isolation (restored by cleanup)
Write-Host "--- pre-existing PageSpeedModule state ---"
$gm = (& $ac list config -section:system.webServer/globalModules) | Select-String "name=`"PageSpeedModule`""
$ciImage = $null
if ($gm) {
  # Extract image="..." from the globalModules entry so cleanup restores the SAME path
  $m = [regex]::Match($gm.Line, 'image="([^"]+)"')
  if ($m.Success) { $ciImage = $m.Groups[1].Value }
}
if ($ciImage) {
  Set-Content -Path "$RigDir\ci-module-image.txt" -Value $ciImage -NoNewline
  Write-Host "Snapshotted CI PageSpeedModule image: $ciImage"
} else {
  Write-Host "No CI PageSpeedModule currently registered (nothing to snapshot/restore)"
}
& $ac uninstall module PageSpeedModule 2>$null
Write-Host "uninstall CI PageSpeedModule exit: $LASTEXITCODE"

# 8. Dedicated pool + site (mirrors setup_iis_full.ps1 New-IISSite: no idle timeout, no periodic recycle,
#    rapidFailProtection off so our deliberate recycles never trip pool-shutdown)
& $ac delete site $SiteName    2>$null | Out-Null
& $ac delete apppool $PoolName 2>$null | Out-Null
& $ac add apppool /name:$PoolName /managedRuntimeVersion:"" /managedPipelineMode:Integrated
& $ac set apppool $PoolName /processModel.identityType:ApplicationPoolIdentity
& $ac set apppool $PoolName /autoStart:true
& $ac set apppool $PoolName /processModel.idleTimeout:00:00:00
& $ac set apppool $PoolName /recycling.periodicRestart.time:00:00:00
& $ac set apppool $PoolName /failure.rapidFailProtection:false
& $ac add site /name:$SiteName /physicalPath:"$RigDir\html" /bindings:"http/*:${Port}:"
& $ac set site $SiteName /applicationDefaults.applicationPool:$PoolName
Write-Host "site+pool created ($SiteName on :$Port, pool $PoolName)"

# 9. Register the ASan module in globalModules only (/add:false), then enable it site-level.
#    The site-level enable MUST go through `set config .../+[name=...] /commit:apphost`
#    (a location tag in applicationHost.config); `add module /app.name:` is refused
#    (exit 33) because system.webServer/modules is locked at the parent level.
& $ac install module /name:$ModuleName /image:"$RigDir\bin\pagespeed_iis.dll" /add:false
Write-Host "install ASan module exit: $LASTEXITCODE"
& $ac set config "$SiteName/" -section:system.webServer/modules "/+[name='$ModuleName']" /commit:apphost
Write-Host "site-level enable ASan module exit: $LASTEXITCODE"

# 10. Start
& $ac start apppool $PoolName
& $ac start site $SiteName
& $ac list site $SiteName
& $ac list apppool $PoolName
Write-Host "SETUP DONE (rig at $RigDir)"
