# Installing PageSpeed for IIS

**Status: Stable** (since 1.15.0+r18 — see the platform table in `RELEASE_NOTES.md`)

## Requirements

- Windows Server 2016 or later
- IIS 10
- No Visual C++ Redistributable: the module, worker and library link the
  runtime statically

## Install

Download the signed installer **`pagespeed-iis-<version>-win-x64.msi`** from the
[latest GitHub release](https://github.com/We-Amp/mod_pagespeed/releases/latest)
and run it — it installs the native module (`pagespeed_iis.dll`) and registers
it with IIS. Before running it, check it against the release's `SHA256SUMS`
file and confirm its Authenticode signature names We-Amp B.V.

> Building from source uses the public mod_pagespeed source tree and a Windows
> build toolchain (see `DEVELOPER.md`); the signed MSI is the supported path for
> everyone else.

The installer also installs the PageSpeed optimizer, a Windows service that
takes over in-place optimization of images, stylesheets and scripts, and
leaves it **disabled**: until you turn it on, the module behaves exactly as it
does without it. The optimizer needs nothing beyond Windows itself. To turn it
on, see [Running PageSpeed for IIS with the optimizer](daemon-adapter-iis.md).

## Configuration

PageSpeed directives live in `pagespeed.config`, not in `web.config`. The machine-wide `C:\ProgramData\We-Amp\PageSpeed\pagespeed.config` applies to every site; a `pagespeed.config` in a site's root, when present, is used for that site instead. Directives use the `pagespeed <Option> <args>` form, and the file is re-read when it changes. `web.config` is only used for IIS plumbing: module registration and request filtering.

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

Filters are enabled and disabled in `pagespeed.config` (for example `pagespeed EnableFilters collapse_whitespace`). See [iis-limitations.md](iis-limitations.md#architecture) for how the configuration file is resolved, and [Running PageSpeed for IIS with the optimizer](daemon-adapter-iis.md) for the optimizer directives.


### Admin pages and reverse proxies

The admin, statistics, console and message pages (and the start-up diagnostic
page) are served to local requests only, which is the default
(`pagespeed InfoUrlsLocalOnly on`). A request is local when it comes from the
server itself over a loopback address and was not relayed by a reverse proxy.

A request that carries forwarding headers (`X-Forwarded-For`, `Forwarded`,
`X-ARR-ClientIP`, `X-Real-IP` and similar) was relayed by a proxy, so it is not
local, even if the proxy runs on the same server: the pages answer such a
request like any other unknown path. The module only checks that such a header
is present, never what it says. A proxy or tunnel that adds none of these
headers cannot be told from a direct request. Restrict the page paths at the
proxy, or make the proxy add a forwarding header, which the module then
detects (nginx: `proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;`,
HAProxy: `option forwardfor`).

Two directives change who is served:

```
# Serve the admin pages to this host name, whoever asks (also behind a proxy).
pagespeed AdminDomains allow admin.example.com
pagespeed StatisticsDomains allow stats.example.com

# Count requests relayed by a proxy on this server as local (default off).
pagespeed TreatProxiedRequestsAsLocal on
```

- `AdminDomains` covers the admin and cache purge paths, `GlobalAdminDomains`
  the global admin path, and `StatisticsDomains`, `GlobalStatisticsDomains`,
  `ConsoleDomains` and `MessagesDomains` the other pages. A request whose
  `Host` header matches the list is served; other requests follow the local
  rule above. The match is on the `Host` header the site receives, so with
  Application Request Routing turn on "Preserve client host header"; without
  it every relayed request carries the back-end host name, and listing that
  name would serve the pages to every visitor. The `Host` header is set by the
  client: the `*Domains` lists limit access by host name and are not
  authentication, so give the host name only to people who may see the pages.
- `TreatProxiedRequestsAsLocal on` is for a site where every visitor comes
  through a proxy on the server and the pages are protected there. With it off
  (the default), the start-up diagnostic page and the
  `X-Pagespeed-Init-Status` response header are also withheld from relayed
  requests. Do not turn it on together with a module that rewrites the client
  address from a forwarding header: a visitor could then present a loopback
  address.
- `pagespeed InfoUrlsLocalOnly off` serves the pages to everyone unless a
  `*Domains` list is set for that page; protect the paths with IIS IP and
  Domain Restrictions or an authenticated proxy if you use it.

### HTML caching

Rewritten HTML is served as not cacheable by shared caches by default, and it
carries `Vary: Accept, Save-Data, User-Agent` (the headers its image URLs and
inlining depend on; fewer when fewer image filters are on). If you let a CDN or
proxy store the HTML (for example with `ModifyCachingHeaders off`), see "Behind
a shared cache or CDN" in
[daemon-adapter-deployment.md](daemon-adapter-deployment.md), including how to
make the HTML the same for every client with `ImagePreserveURLs on`. If you
enable IIS output caching for rewritten pages, use the user-mode `policy` with
`varyByHeaders` set to `Accept;Save-Data;User-Agent`, and keep
`kernelCachePolicy` off for them: kernel-mode caching cannot vary by request
header.

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
