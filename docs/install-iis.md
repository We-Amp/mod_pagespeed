# Installing PageSpeed for IIS

**Status: Experimental**

## Requirements

- Windows Server 2019 or later
- IIS 10+
- Visual C++ Redistributable 2022

## Install Pre-built

```powershell
# Extract package
Expand-Archive pagespeed-iis-1.1.0-beta.1-win-x64.zip C:\inetpub\pagespeed

# Stop IIS
Stop-Service -Name W3SVC

# Register the module globally
New-WebGlobalModule -Name PageSpeedModule -Image 'C:\inetpub\pagespeed\pagespeed_iis.dll'

# Start IIS
Start-Service -Name W3SVC
```

## Build from Source

On a Windows machine with Bazel and clang-cl:

```powershell
bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll
```

Or via the remote development setup from Linux:

```bash
./windows-dev/start-windows-dev.sh
./windows-dev/wait-for-windows.sh
./windows-dev/build-on-windows.sh //pagespeed/iis:pagespeed_iis.dll
```

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
# Should show: X-PageSpeed: 1.1.0-beta.1
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
- IPRO async cache stubs return misses
- Beacon data silently discarded
- Redis only (no Memcached on Windows)
- Single `IisServerContext` per application pool
