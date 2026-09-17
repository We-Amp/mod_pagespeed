# Installing PageSpeed for IIS

**Status: Stable** (since 1.15.0+r18 — see the platform table in `RELEASE_NOTES.md`)

## Requirements

- Windows Server 2019 or later
- IIS 10+
- Visual C++ Redistributable 2022

## Install

Download the signed installer **`pagespeed-iis-<version>-win-x64.msi`** from the
[downloads page](https://modpagespeed.com/1.1/docs/downloads/) and run it — it
installs the native module (`pagespeed_iis.dll`) and registers it with IIS.

> Building from source uses the public mod_pagespeed source tree and a Windows
> build toolchain (see `DEVELOPER.md`); the signed MSI is the supported path for
> everyone else.

## Configuration

PageSpeed for IIS is configured via `web.config` XML files.

**Important:** If the module is registered globally (via `New-WebGlobalModule`),
do NOT add it again in the site's `web.config` -- this causes 500.19 errors.

Minimal site `web.config`:
```xml
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <system.webServer>
    <!-- Required for PageSpeed combined resource URLs containing '+' -->
    <security>
      <requestFiltering allowDoubleEscaping="true" />
    </security>
  </system.webServer>
</configuration>
```

PageSpeed for IIS is configured automatically on module load. The module
applies default optimizations (image compression, CSS/JS minification,
cache extension) to all responses. The admin UI is available at
`/pagespeed_admin` for monitoring and diagnostics.

Filter configuration is managed through the `web.config` XML. For advanced
configuration options, see the [CLAUDE.md](../CLAUDE.md#windowsiis-development)
development guide.

## Verification

```powershell
curl.exe -I http://localhost/
# Should show an X-PageSpeed: <version> response header
```

## Updating

After rebuilding the DLL, IIS must be restarted:
```powershell
Stop-Service W3SVC
Copy-Item bazel-bin\pagespeed\iis\pagespeed_iis.dll C:\inetpub\pagespeed\ -Force
Start-Service W3SVC
```

Or recycle the app pool:
```powershell
Restart-WebAppPool DefaultAppPool
```

## Running Tests

### C++ Unit Tests (on Windows)

```powershell
bazel test --config=windows --config=clang-cl //test/pagespeed/iis:all
```

### Python Integration Tests (on Windows)

```powershell
cd test\iis
powershell -ExecutionPolicy Bypass -File Setup-IISExpress.ps1 -Port 8080
powershell -ExecutionPolicy Bypass -File Start-IISExpress.ps1 -Background -Wait -Port 8080

cmd /c "set PAGESPEED_PORT=8080& set IIS_EXPRESS=1& set PAGESPEED_TEST_ROOT=& set PAGESPEED_EXAMPLE_ROOT=& python -m pytest test/iis/ -v"

powershell -ExecutionPolicy Bypass -File Stop-IISExpress.ps1
```

### System Tests from Linux Host

```bash
./test/system/run_iis_tests.sh sanity
```

## Known Limitations

See [iis-limitations.md](iis-limitations.md) for full details.

Key limitations:
- External cache is Redis only (no Memcached on Windows)
- Shared-memory statistics/caches are per worker process (no cross-process
  sharing between `w3wp.exe` workers)
- Incremental HTML flushing is compiled out (output is buffered)
