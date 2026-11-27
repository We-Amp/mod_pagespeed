# ASP.NET Core: install and verify the mod_pagespeed middleware

Source: https://modpagespeed.com/docs/aspnet-getting-started/ and
https://modpagespeed.com/docs/aspnet-configuration/. Header: `X-PageSpeed`.
Scope: the in-process `WeAmp.PageSpeed.AspNetCore` NuGet package. The
Linux-only `WeAmp.PageSpeed.Sidecar` package is a different integration (header
`X-Page-Speed`), described in the source document.

## 1. Prerequisites

.NET 8 or .NET 10 SDK; linux-x64, linux-arm64, osx-arm64 or win-x64; an
ASP.NET Core application that serves HTML.

```bash
dotnet --list-sdks              # 8.x or 10.x
dotnet --info | grep -i 'RID'   # linux-x64, linux-arm64, osx-arm64 or win-x64
```

PowerShell:

```powershell
dotnet --list-sdks                       # 8.x or 10.x
dotnet --info | Select-String 'RID'      # win-x64
```

## 2. Install

In the project directory:

```bash
dotnet add package WeAmp.PageSpeed.AspNetCore
```

## 3. Minimal configuration

`Program.cs`, with `UsePageSpeed()` before any middleware that writes the
response body:

```csharp
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddPageSpeed();   // binds the "PageSpeed" configuration section

var app = builder.Build();
app.UsePageSpeed();
// the rest of the pipeline
app.Run();
```

`appsettings.json`. The default cache path, `/var/cache/pagespeed/volume.dat`,
needs elevated permissions; for a local run point it at a writable path (a
relative path is created under the app's working directory):

```json
{
  "PageSpeed": {
    "Enabled": true,
    "Cache": { "VolumePath": "pagespeed-cache/volume.dat" }
  }
}
```

The worker process starts automatically; no `Worker` section is needed.

The web console at `/console/` and its `/v1/*` worker API proxy are on by
default, without authentication: anyone who can reach the site can read the
optimizer configuration and change it. Before production, set
`PageSpeed:Console:Enabled` to `false`, or gate those paths with the app's own
authentication, and tell the user which you chose:
https://modpagespeed.com/docs/aspnet-configuration/#console-pagespeedconsole

Production cache paths and every option:
https://modpagespeed.com/docs/aspnet-configuration/

## 4. Verify

Start the app in the background on a free port, wait until it answers, check
the header on a route that returns a full HTML page, then stop the app.

```bash
dotnet run -- --urls http://localhost:<port> > pagespeed-verify.log 2>&1 &
APP_PID=$!
for i in $(seq 1 30); do curl -s -o /dev/null http://localhost:<port>/ && break; sleep 2; done
curl -s -o /dev/null -D - 'http://localhost:<port>/<html-route>?mps-verify=agent' | grep -i '^x-pagespeed:'
kill "$APP_PID"
```

PowerShell:

```powershell
$app = Start-Process dotnet -ArgumentList 'run','--','--urls','http://localhost:<port>' -PassThru -NoNewWindow -RedirectStandardOutput pagespeed-verify.log
foreach ($i in 1..30) { curl.exe -s -o NUL http://localhost:<port>/; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep 2 }
curl.exe -s -o NUL -D - 'http://localhost:<port>/<html-route>?mps-verify=agent' | Select-String '^x-pagespeed:'
Stop-Process -Id $app.Id
```

If the port still answers after the stop, stop the app process that listens on
it.

Pass: an `X-PageSpeed` line, `MISS` on the first request and `HIT` once the
worker has written the variant. The middleware sets the header only on a
`text/html` response it rewrites: non-HTML (the `/` of a fresh
`dotnet new web` app returns plain text), HTML it leaves unchanged and
`/console/*` get no header. Use a route that returns a real HTML page. No
header: https://modpagespeed.com/docs/troubleshooting/

## 5. Rollback

```bash
dotnet remove package WeAmp.PageSpeed.AspNetCore
```

Remove `AddPageSpeed()`, `UsePageSpeed()` and the `using` from `Program.cs`,
the `PageSpeed` section from `appsettings.json`, the cache volume file and
`pagespeed-verify.log`. Pinning a previous version:
https://modpagespeed.com/docs/uninstall/#aspnet-core and
https://modpagespeed.com/docs/uninstall/#roll-back-to-the-previous-release
