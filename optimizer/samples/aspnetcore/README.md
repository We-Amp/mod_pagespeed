# WeAmp.PageSpeed for ASP.NET Core

Drop-in ASP.NET Core middleware that improves Core Web Vitals without touching
your app code. It adds critical CSS inlining, LCP preload injection, lazy
loading, and on-demand WebP/AVIF image transcoding to every HTML response. The
C++23 PageSpeed engine runs in-process via P/Invoke and serves cache hits
zero-copy.

Single-package install. Hot-reloadable config. Optimization runs out of the
box.

Agent install recipe: https://modpagespeed.com/recipes/aspnet-core.md

## Quick Start

**1. Install the package**

```shell
dotnet add package WeAmp.PageSpeed.AspNetCore
```

The matching native binaries for your runtime (`linux-x64`, `linux-arm64`,
`osx-arm64`, or `win-x64`) come in transitively: no separate NativeAssets
package reference required.

**2. Register the middleware in `Program.cs`**

```csharp
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddPageSpeed();

var app = builder.Build();

app.UsePageSpeed();    // before anything that writes the response body
app.UseStaticFiles();

app.Run();
```

No extra wiring required: with no `Worker` section, the worker process starts
automatically and coordinates over an auto-resolved per-process socket. Add a
`Worker` section only to change that — see [Configuration](#configuration).
Optimization is on by default.

> **Before production: lock down the console.** This setup also serves the web
> console at `/console/` and proxies its worker API (`/v1/health`, `/v1/ws`,
> `/v1/cache`, `/v1/stats`, `/v1/capture`, `/v1/config`) from your site's root,
> in every environment, with no authentication. Anyone who can reach your site
> can read the stats and configuration, change the live optimizer configuration
> with `PATCH /v1/config`, and purge the cache. Recommended for production:
> `Enabled: false`, for example in `appsettings.Production.json`. If you need
> the console in production, put your own authentication in front of it — see
> [Security](#security).
>
> ```json
> {
>   "PageSpeed": {
>     "Console": { "Enabled": false }
>   }
> }
> ```

**3. How do I know it's working?**

Three checks, from quickest to most thorough. These examples assume your app
listens on `:5050` (set `ASPNETCORE_URLS=http://localhost:5050` or adjust the
URLs to your port) and serves at least one HTML page and one image.

Hit a content route and look for the `X-PageSpeed` header:

```shell
curl -i http://localhost:5050/
```

```
HTTP/1.1 200 OK
X-PageSpeed: HIT
```

`HIT` means the response was served from the optimized cache; `MISS` means the
worker is building the variant and you'll see `HIT` on the next request. (The
`/console/*` routes are short-circuited before the middleware, so they
intentionally do not carry `X-PageSpeed` — only content routes like `/` and
your assets do.)

Open `http://localhost:5050/console/`. Once a few requests have run,
the Dashboard and Metrics show non-zero counts. If everything reads zero, see
[How do I know it's working?](https://modpagespeed.com/docs/aspnet-getting-started/#verify-the-install)
in the docs.

Confirm image transcoding via content negotiation. We don't rewrite URLs in
2.0: the same `/hero.jpg` URL serves WebP to WebP-capable clients and AVIF to
AVIF-capable ones, selected by the request `Accept` header:

```shell
curl -s -o /dev/null -D - http://localhost:5050/hero.jpg -H 'Accept: image/jpeg'
# Content-Length: 98230   Content-Type: image/jpeg   Vary: Accept, Save-Data, User-Agent

curl -s -o /dev/null -D - http://localhost:5050/hero.jpg -H 'Accept: image/webp'
# Content-Length: 2422    Content-Type: image/webp   Vary: Accept, Save-Data, User-Agent

curl -s -o /dev/null -D - http://localhost:5050/hero.jpg -H 'Accept: image/avif'
# Content-Length: 415     Content-Type: image/avif   Vary: Accept, Save-Data, User-Agent
```

Same URL, materially smaller bytes, and a `Vary: Accept, Save-Data, User-Agent`
header so caches keep the variants apart. (Byte counts are from one sample
image; yours will differ.)

## What It Does

- **HTML optimization:** critical CSS inlining, lazy loading, LCP preload
  injection, third-party preconnect hints
- **Image transcoding:** on-demand conversion to WebP and AVIF, viewport-aware
  resizing, Save-Data support
- **CSS/JS minification:** whitespace removal, comment stripping
- **Zero-copy caching:** cache hits served from the memory-mapped Cyclone cache
  with no copy, up to 36 optimized variants per resource (format × viewport ×
  density × Save-Data)

The middleware buffers HTML responses, passes them through the native
`libpagespeed` library, and notifies the worker process to generate optimized
asset variants asynchronously.

**What is cached.** The middleware stores only responses to anonymous
requests: a request without an `Authorization` or `Proxy-Authorization`
header, without a signed-in user (`HttpContext.User`), to an endpoint without
an authorization requirement such as `[Authorize]` or `RequireAuthorization()`
(unless the endpoint also allows anonymous access). A page served to a
signed-in client is still optimized for that response, and it is not stored.
Responses that set a cookie or carry `Cache-Control: private` or `no-store`
are not stored either. On a cache miss whose response would be stored, the
middleware requests the same URL once more as an anonymous request (no
signed-in user, cookies or credentials, from the client address `224.0.0.0`)
and stores that response; the visitor still gets its own. Your app therefore
handles each response that gets cached twice. Put middleware and endpoints
that admit clients by address or identity after `UsePageSpeed()`, or list
their paths in `ExcludePaths`, and call `UseRouting()` after `UsePageSpeed()`:
an endpoint selected ahead of it is cached only when nothing about its
selection can depend on the client. If your app's GET handlers have side effects,
set `RecordResponses` to `false`: pages are still optimized, and nothing new
is cached.

## Platform Support

| RID         | Status    | Notes                                                    |
| ----------- | --------- | -------------------------------------------------------- |
| linux-x64   | Supported | glibc 2.34+ (RHEL 9 / Ubuntu 22.04 / Debian 12 or newer) |
| linux-arm64 | Supported | glibc 2.34+                                              |
| osx-arm64   | Supported | macOS 13+ (Apple Silicon)                                |
| win-x64     | Supported | Windows 10/11, Server 2019+                              |

Native binaries (`libpagespeed`, `factory_worker`) are bundled with the
matching `WeAmp.PageSpeed.NativeAssets.*` package, pulled in transitively by
`WeAmp.PageSpeed.AspNetCore`. The worker process starts automatically on
application boot. The Linux binaries statically link the C++ runtime
(libc++/libc++abi/libunwind), so no additional shared libraries need to be
present on the host beyond the system glibc.

**On Linux and want a reverse proxy instead of in-process middleware?**
[`WeAmp.PageSpeed.Sidecar`](https://www.nuget.org/packages/WeAmp.PageSpeed.Sidecar)
runs mod_pagespeed 1.15 as a bundled nginx + ngx_pagespeed reverse proxy in front of
Kestrel (Linux-only). Use it when you want the classic nginx module in a sidecar; use
this package for cross-platform in-process optimization with WebP/AVIF.

Each NativeAssets package also ships a `BUILD_INFO.json` file at
`runtimes/<rid>/native/BUILD_INFO.json` with `git_sha`, `git_sha_short`,
`build_timestamp_utc`, and `rid`. It is intended for support correlation and
for verifying package provenance without running the worker.

## Configuration

Add a `PageSpeed` section to `appsettings.json`. Only `Cache` is shown below;
every key in the table is optional and falls back to its default.

```json
{
  "PageSpeed": {
    "Cache": {
      "VolumePath": "/var/cache/pagespeed/volume.dat",
      "VolumeSizeBytes": 1073741824
    }
  }
}
```

Set `Cache.VolumePath` to a location that is writable in your environment. The
default `/var/cache/pagespeed/` assumes a Linux host; on Windows, macOS, or
containers without that path, point it somewhere writable (for example
`./cache/volume.dat` or `%TEMP%`).

**Top-level keys:**

| Setting                  | Default                                               | Description                                                                                                          |
| ------------------------ | ----------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| `Enabled`                | `true`                                                | Enable/disable the middleware (supports hot-reload). The cache volume is opened, and the worker launched, the first time it is `true`, at startup or through a hot reload; switching it off again leaves both in place. |
| `ExcludePaths`           | `["/api/", "/signalr/", "/_blazor/", "/_framework/"]` | URL prefixes the middleware leaves untouched (supports hot-reload)                                                   |
| `CacheMode`              | `Safe`                                                | `Safe`: `must-revalidate` on assets. `Aggressive`: `public` + `stale-if-error` on assets. HTML is always `no-cache`. |
| `MaxResponseBufferBytes` | `5242880` (5 MB)                                      | Responses larger than this pass through unmodified                                                                   |
| `CssMaxAgeSeconds`       | `300`                                                 | `max-age` on CSS/JS cache HIT responses                                                                              |
| `ImageMaxAgeSeconds`     | `1800`                                                | `max-age` on image cache HIT responses                                                                               |

**`Cache` section:**

| Setting                 | Default                           | Description                                              |
| ----------------------- | --------------------------------- | -------------------------------------------------------- |
| `Cache.VolumePath`      | `/var/cache/pagespeed/volume.dat` | Path to the Cyclone cache volume file (must be writable). Not created until `Enabled` is first `true`. |
| `Cache.VolumeSizeBytes` | `1073741824` (1 GB)               | Maximum cache volume size                                |

**`Worker` section:**

| Setting             | Default                   | Description                                                                                                                                                                                                                                                                         |
| ------------------- | ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Worker.AutoStart`  | `true`                    | Launch and manage the worker process once the top-level `Enabled` is true. With `Enabled` false, no worker (and so no worker API or console data) runs until a hot reload sets it to true. Set `false` to run no child worker; pair it with a concrete `Worker.SocketPath` for an external worker. |
| `Worker.SocketPath` | unset                     | Worker-coordination socket. **Leave it unset** (the default) for an auto-resolved per-process socket with coordination on. Set to a concrete path to share one socket with an out-of-process worker. Setting it to `null` or `""` disables coordination and logs a startup warning. |
| `Worker.ApiPort`    | `0` (auto, loopback only) | Loopback port for the worker HTTP API. `0` picks a free loopback port; it does not disable the API. The API runs without authentication either way and is reachable through the console proxy.                                                                                    |

**`Console` section:**

| Setting                | Default    | Description                                                                                                         |
| ---------------------- | ---------- | ------------------------------------------------------------------------------------------------------------------- |
| `Console.Enabled`      | `true`     | Serve the console and proxy its `/v1/*` paths. `false` lets those requests fall through to your app. The console's data comes from the worker, so it also needs the top-level `Enabled` set to `true`. |
| `Console.MountPath`    | `/console` | URL prefix for the in-app console. The proxied `/v1/*` paths do not move with it.                                   |
| `Console.RequireHttps` | `false`    | Return `403` for console and `/v1/*` requests that are not HTTPS. Protects the traffic; it is not authentication. |

Options support hot-reload via `IOptionsMonitor<PageSpeedOptions>`.

## Worker IPC

The middleware ↔ worker channel uses Unix domain sockets on Linux and macOS,
and Windows Named Pipes on win-x64. Selection is automatic; no configuration
required. The worker's HTTP API is bound to `127.0.0.1` either way: on an
ephemeral port by default, or on `Worker.ApiPort` when you set it. Its API is
still reachable from outside through the app's console proxy, which is served
on the app port — see [Security](#security).

## Security

The `/console/` admin console and its `/v1/*` worker-API proxy (`/v1/health`,
`/v1/ws`, `/v1/cache`, `/v1/stats`, `/v1/capture`, `/v1/config`) are on by
default and served on the same origin as your app, in every environment,
**without authentication**. Anyone who can reach your site can read the stats
and configuration, change the live optimizer configuration and purge the
cache. The worker requires `X-Requested-With: XMLHttpRequest` on
state-changing (POST/PATCH) requests; that blocks cross-origin form posts, but
any client that is not a browser can send the header.

Recommended for production: `Enabled: false`, for example in
`appsettings.Production.json` (or `PageSpeed__Console__Enabled=false`). The
console paths then fall through to your app. The top-level `Enabled` switch does
not turn the console off, and a switch through hot reload can take a few seconds
to apply. On IIS, you can set the variable in the site's `web.config`: add only
this `<environmentVariables>` block inside the `<aspNetCore>` element it already
has, and leave that element's other attributes as they are:

```xml
<environmentVariables>
  <environmentVariable name="PageSpeed__Console__Enabled" value="false" />
</environmentVariables>
```

`dotnet publish` regenerates `web.config` unless the project has its own, so
add the file to the project or repeat the edit after each publish.

**Gate the console with your own authentication.** If you need the console in
production, put your own authentication in front of `Console.MountPath` and the
six `/v1/*` paths instead. Register the gate immediately before
`UsePageSpeed()`, after `UseAuthentication()` and after any `UseRewriter()` or
`UsePathBase()`, so it sees the same path the console does. This example
requires a signed-in user in the `Admin` role and uses whatever authentication
scheme your app already registers. At the top of `Program.cs`:

```csharp
using Microsoft.AspNetCore.Authentication;
```

In the pipeline, in place of the plain `app.UsePageSpeed();`:

```csharp
app.UseAuthentication();

string[] pageSpeedConsolePaths =
{
    "/console", // Console.MountPath; change it here if you re-mount the console
    "/v1/health", "/v1/ws", "/v1/cache", "/v1/stats", "/v1/capture", "/v1/config",
};

app.UseWhen(
    ctx => pageSpeedConsolePaths.Any(p => ctx.Request.Path.StartsWithSegments(p)),
    branch => branch.Use(async (ctx, next) =>
    {
        if (ctx.User.Identity?.IsAuthenticated != true)
        {
            await ctx.ChallengeAsync(); // 401, or your scheme's sign-in redirect
            return;
        }
        if (!ctx.User.IsInRole("Admin"))
        {
            await ctx.ForbidAsync(); // 403, or your scheme's access-denied redirect
            return;
        }
        await next();
    }));

app.UsePageSpeed();
```

The gate needs an authentication scheme with a default challenge:

- With no authentication services at all, the app fails at startup:
  `Unable to resolve service for type
  'Microsoft.AspNetCore.Authentication.IAuthenticationSchemeProvider'`.
- With authentication services but no default scheme (`AddAuthentication()`
  alone, or only what `AddRazorPages()` registers), every gated request fails
  with a `500`: `No authenticationScheme was specified, and there was no
  DefaultChallengeScheme found`.

An app without authentication can register cookie authentication before
`builder.Build()`, with `using Microsoft.AspNetCore.Authentication.Cookies;` at
the top of `Program.cs`:

```csharp
builder.Services
    .AddAuthentication(CookieAuthenticationDefaults.AuthenticationScheme)
    .AddCookie();
```

A signed-out request is then redirected to the sign-in path (`/Account/Login`
by default), which your app provides; the admin's sign-in must carry the
`Admin` role.

Windows authentication fits an intranet or IIS site; check a Windows group in
place of `Admin`. On IIS, enable Windows authentication in IIS and register
its scheme, with `using Microsoft.AspNetCore.Server.IISIntegration;`:

```csharp
builder.Services.AddAuthentication(IISDefaults.AuthenticationScheme);
```

On Kestrel, add the `Microsoft.AspNetCore.Authentication.Negotiate` package;
it is not part of the shared framework. Match its major version to your target
framework, for example on .NET 8:

```bash
dotnet add package Microsoft.AspNetCore.Authentication.Negotiate --version 8.0.*
```

Then, with `using Microsoft.AspNetCore.Authentication.Negotiate;`:

```csharp
builder.Services
    .AddAuthentication(NegotiateDefaults.AuthenticationScheme)
    .AddNegotiate();
```

The `/v1/*` paths are fixed: re-mounting the console with `MountPath` does not
move them. Gate exactly these six rather than all of `/v1`, so your own `/v1`
routes keep their own rules.

**Check an existing deployment.** Every release since 2.0.13 serves the console
and all six `/v1/*` paths this way, with no fix release yet; 2.0.4 through
2.0.12 served the console and a smaller set of proxied paths, also without
authentication. From outside your network:

```bash
curl -s -o /dev/null -w '%{http_code}\n' https://<your-domain>/v1/config
```

`404` means the console is off, and `401`, `403` or a `302` to your sign-in
page means it is gated. A `200` needs a look at the body: with the console off,
your own app can answer `/v1/config` (its own route, or a catch-all or SPA
fallback). The console's answer is JSON with keys such as `webp_quality`; if
that is what you get, anyone can read and change the optimizer configuration.

A configuration changed through the console is saved by the worker to
`pagespeed.json`, in the directory of `Cache.VolumePath`. It survives a restart
and stays in force after you turn the console off. Optimized variants built
under the changed settings stay in the cache volume as well. To recover, whether
the console is now off or gated:

1. Stop the app; the worker stops with it.
2. In the directory of `Cache.VolumePath`, delete `pagespeed.json` and the
   cache volume files: every file whose name starts with the volume file's base
   name, for the default `volume.dat` that is `volume-8-<hash>.dat` and, if
   present, `volume.dat.gen` (`volume*`).
3. Start the app. The worker runs on its startup configuration (environment
   variables and flags still apply) and rebuilds the cache.

To check the result, compare `GET /v1/config`, with the console gated, against
the answer of a fresh install.

`Console.MountPath` and `Console.RequireHttps` are not access control:
re-mounting does not move the `/v1/*` paths, and HTTPS does not authenticate
anyone. With the console gated, also avoid loading untrusted third-party
scripts into pages an authenticated admin visits, since a same-origin script
can drive the console API with that admin's session.

**Address-based rules and agent optimization.** With agent optimization on,
the optimizer fetches the site's own pages from this host to build the
markdown variant and `/llms.txt`. `UsePageSpeed()` presents those fetches to
every middleware and endpoint after it as the client address `224.0.0.0`, so a
rule that admits local clients (for example a check that
`HttpContext.Connection.RemoteIpAddress` is loopback) does not admit them. A
fetch is recognized by the optimizer's User-Agent together with a client
address that is loopback or the address the request arrived on; a client
connecting directly from elsewhere keeps its address. Register
`UsePageSpeed()` before middleware that decides by client address, or call
`app.UsePageSpeedAgentFetchLabel()` ahead of that middleware. When the app
runs behind a proxy, `UseForwardedHeaders()` must come before `UsePageSpeed()`
and before `UsePageSpeedAgentFetchLabel()`, so that a fetch is recognized by
the forwarded client address. Pages under such a rule are then
left out of the agent outputs. Rules evaluated before the ASP.NET Core
pipeline, such as IIS `ipSecurity` or a front proxy's own access rules, are
outside this change.

## License

Apache License 2.0, see the LICENSE file in the package.

## Links

- [modpagespeed.com/pricing/](https://modpagespeed.com/pricing/) — plans, pricing, and purchase
- [modpagespeed.com/features/#aspnet-core](https://modpagespeed.com/features/#aspnet-core) — ASP.NET Core overview
- [WeAmp.PageSpeed.Sidecar](https://www.nuget.org/packages/WeAmp.PageSpeed.Sidecar) — mod_pagespeed 1.15 as a bundled-nginx sidecar (Linux)
- [modpagespeed.com](https://www.modpagespeed.com) — product documentation and filter reference
- [we-amp.com](https://www.we-amp.com) — We-Amp B.V.
