---
title: 'ASP.NET Core settings'
description: 'Configuration reference for the mod_pagespeed ASP.NET Core middleware: appsettings.json PageSpeed options, cache and worker settings, and hot reload.'
order: 28
group: 'Configure'
lastUpdated: 2026-11-15
---

The mod_pagespeed ASP.NET Core middleware (`WeAmp.PageSpeed.AspNetCore`) is
configured from the standard ASP.NET Core sources: appsettings.json, environment
variables, or any `IConfiguration` provider. It binds its options to the
`PageSpeed` section via `IServiceCollection.AddPageSpeed()`.

## Basic configuration

Add a `PageSpeed` section to your appsettings.json:

```json
{
  "PageSpeed": {
    "Enabled": true,
    "CacheMode": "Safe",
    "Cache": {
      "VolumePath": "/var/cache/pagespeed/volume.dat",
      "VolumeSizeBytes": 1073741824
    },
    "Html": {
      "EnableCriticalCss": true,
      "EnableLazyLoad": true,
      "EnableAsyncCss": false
    },
    "Worker": {
      "AutoStart": true
      // "ApiPort": 9191  // Worker management API, loopback only. The middleware
      // starts the worker with --api-no-auth, so never publish this port.
    },
    "Console": {
      "Enabled": false // The console is public by default; see Console below.
    }
  }
}
```

## Options

The middleware binds to a `PageSpeedOptions` instance with four nested sections:
`Cache`, `Html`, `Worker`, and `Console`. All options support hot reload via
`IOptionsMonitor<PageSpeedOptions>`.

### Top-level (`PageSpeed`)

| Option                   | Type     | Default                                               | Description                                                                                                                                                                                                  |
| ------------------------ | -------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `Enabled`                | bool     | `true`                                                | Master switch. When false, the middleware passes through all responses unchanged. The cache volume is opened, and the worker launched, the first time it is true, at startup or through a hot reload; switching it off again leaves both in place. |
| `CacheMode`              | enum     | `Safe`                                                | Cache-Control header strategy on cache HIT. `Safe` adds `must-revalidate` on assets; `Aggressive` adds `public, stale-if-error`. HTML always gets `no-cache`. See [Choose a cache mode](/docs/cache-modes/). |
| `CssMaxAgeSeconds`       | int      | `300`                                                 | Max-age for CSS and JS cache HIT responses, in seconds.                                                                                                                                                      |
| `ImageMaxAgeSeconds`     | int      | `1800`                                                | Max-age for image cache HIT responses, in seconds.                                                                                                                                                           |
| `HtmlMaxAgeSeconds`      | int      | `0`                                                   | Reserved. HTML always receives `no-cache` regardless of this value.                                                                                                                                          |
| `MaxResponseBufferBytes` | int      | `5242880` (5 MB)                                      | Max response body size to buffer. Larger responses pass through unmodified.                                                                                                                                  |
| `MaxAssetCacheBytes`     | int      | `10485760` (10 MB)                                    | Max asset (image/CSS/JS) size to cache. Larger assets pass through without caching or worker notification.                                                                                                   |
| `ExcludePaths`           | string[] | `["/api/", "/signalr/", "/_blazor/", "/_framework/"]` | URL path prefixes the middleware skips entirely. Override to add your own prefixes.                                                                                                                          |

### Cache (`PageSpeed:Cache`)

The cache is a single mmap'd Cyclone volume file — not a directory of fragments.

| Option                 | Type   | Default                           | Description                                                             |
| ---------------------- | ------ | --------------------------------- | ----------------------------------------------------------------------- |
| `VolumePath`           | string | `/var/cache/pagespeed/volume.dat` | Required. Path to the Cyclone cache volume file. Created the first time `Enabled` is true. |
| `VolumeSizeBytes`      | ulong  | `1073741824` (1 GiB)              | Volume size in bytes.                                                   |
| `EnableChecksum`       | bool   | `true`                            | Verify checksums on cache reads.                                        |
| `RamCacheSizeBytes`    | long   | `67108864` (64 MiB)               | In-process RAM cache size in bytes. Set to `0` to disable.              |
| `MaxMetadataSizeBytes` | long   | `8192` (8 KiB)                    | Per-entry metadata size cap, in bytes.                                  |

### HTML processing (`PageSpeed:Html`)

Each rewrite is a boolean toggle — no rewrite-level abstraction, no filter strings.
Toggles default to "on" except `EnableSpeculationRules` and `EnableAsyncCss`,
which default to `false` in the middleware. `EnableAsyncCss` is opt-in: the
in-process middleware emits the async-CSS loader only when you set it to **true**
**and** critical-CSS inlining actually produces inlined rules for the page. The
toggle is a required precondition, not a label on behavior that happens anyway
(see the row below). The "on by default whenever critical CSS is present"
behavior belongs to the nginx/worker front-end, which is a different code path —
the in-process .NET middleware does not default async-CSS on.

| Option                   | Type | Default          | What it does                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| ------------------------ | ---- | ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `EnableCriticalCss`      | bool | `true`           | Extract above-the-fold CSS and inline it; defer the rest.                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `EnableLazyLoad`         | bool | `true`           | Add `loading="lazy"` to below-the-fold images. "Below the fold" is by DOM source order, not CSS layout, and how many leading body images are exempt depends on LCP detection. When a valid LCP candidate is found, that image gets `fetchpriority="high"` and body images #1–#3 are exempt as an above-fold safety guard (lazy starts at the 4th image). When there is no LCP candidate, only the first body image is protected (it gets `fetchpriority="high"`) and lazy-loading starts at the 2nd image. |
| `EnableImageDimensions`  | bool | `true`           | Add explicit `width`/`height` to images that omit them.                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `EnableLcpPreload`       | bool | `true`           | Emit a `<link rel="preload">` hint for the detected LCP image.                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `EnablePreconnect`       | bool | `true`           | Emit `<link rel="preconnect">` hints for referenced third-party origins.                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `EnableSpeculationRules` | bool | `false`          | Emit a speculation-rules script for in-page prefetching.                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `EnableAsyncCss`         | bool | `false`          | Defer render-blocking stylesheets via a content-hashed async loader. Defaults `false` (opt-in), because the host must also serve the loader path. The middleware emits the loader only when this is `true` **and** critical-CSS inlining produced inlined rules for the page — both conditions are required. (The nginx/worker front-end is a separate code path with its own, stricter condition: it defers only for a page whose above-the-fold appearance it has confirmed unchanged against the stylesheet being served. The in-process middleware has no browser and so cannot make that confirmation; its byte-based check plus this opt-in flag are what gate it.)                                                  |
| `EnableScriptDeferral`   | bool | `true`           | Defer scripts based on browser-coverage analysis. See [Browser analysis](/docs/browser-analysis/).                                                                                                                                                                                                                                                                                                                                                                                                         |
| `Viewport`               | enum | `Desktop`        | Viewport class used for capability-mask classification. `Mobile`, `Tablet`, or `Desktop`.                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `MaxHtmlSizeBytes`       | int  | `5242880` (5 MB) | Max HTML size to process. Larger documents pass through.                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `MaxCssSizeBytes`        | int  | `2097152` (2 MB) | Max CSS size to process. Larger stylesheets pass through.                                                                                                                                                                                                                                                                                                                                                                                                                                                  |

### Worker (`PageSpeed:Worker`)

The middleware optimizes inline; the worker process runs out-of-process for
expensive jobs (image transcoding, critical-CSS extraction) and exposes a
diagnostic console.

| Option       | Type    | Default | Description                                                                                                                                                                                                                                                                                                                                                            |
| ------------ | ------- | ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `AutoStart`  | bool    | `true`  | Resolve the `factory_worker` binary from the NuGet package and launch it as a child process, but only while the top-level `Enabled` switch is true. With `Enabled` false, no worker runs until a hot reload sets it to true. Set `false` to launch no child process; pair it with a concrete `SocketPath` for an external worker, which is used whatever `Enabled` says. |
| `SocketPath` | string? | unset   | Worker-coordination socket. **Leave it unset** (the default) and the middleware auto-resolves a per-process socket with coordination on — no configuration needed. Set it to a concrete path to share one socket with an out-of-process worker (split-process deployments). Setting it to `null` or `""` disables worker coordination and logs a loud startup warning. |
| `ApiPort`    | int     | `0`     | Loopback TCP port for the worker's HTTP management API. `0` does not disable the API: the middleware picks a free loopback port. Either way the worker runs with API authentication off, and the console proxies its `/v1/*` paths from your site (see [Console](#console-pagespeedconsole)). **Never publish this port.**                                               |
| `LogLevel`   | string? | `null`  | Worker log level: `debug`, `info`, `warning`, or `error`. `null` keeps the worker's default (`info`).                                                                                                                                                                                                                                                                  |
| `ConsoleDir` | string? | `null`  | Filesystem path to the diagnostic-console SPA assets. When `null` and `AutoStart` is true, the middleware auto-discovers a `console` directory next to `factory_worker`. Set to `""` to disable. This only stops the worker's own loopback copy of the console, not the middleware's `/console/`; use [`Console.Enabled`](#console-pagespeedconsole) for that.                                    |

Whenever the worker runs, its management API runs too, bound to loopback,
with authentication off. With the top-level `Enabled` set to `false`, no worker
is launched, so its management API is not running either. You do not need to
set `Worker.ApiPort` to use the console: the middleware serves it on your
app's own port and proxies its API calls to the worker. That proxy is what
the [Console](#console-pagespeedconsole) section is about.

### Console (`PageSpeed:Console`)

The middleware serves the web console at `MountPath` on your app's own port
and proxies the console's worker API calls, `/v1/health`, `/v1/ws`,
`/v1/cache`, `/v1/stats`, `/v1/capture` and `/v1/config`, to the worker.
The `/v1/*` paths are fixed; `MountPath` does not move them.

| Option         | Type   | Default    | Description                                                                                                                                                         |
| -------------- | ------ | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Enabled`      | bool   | `true`     | Serve the console and proxy its `/v1/*` paths. When `false`, those requests fall through to your app. The top-level `Enabled` switch does not turn the console off, but the console's data comes from the worker, which is launched only once the top-level `Enabled` is `true`; until then its `/v1/*` paths answer `503`. |
| `MountPath`    | string | `/console` | Path the console is served at, for example `/_pagespeed/console`. Must start with `/` and must not be `/`; an invalid value disables the console with a warning.    |
| `RequireHttps` | bool   | `false`    | Return `403` for console and `/v1/*` requests that do not arrive over HTTPS. This protects the traffic, not the access: it is not authentication.                   |

:::warning[No authentication by default]
With the defaults, the console and its `/v1/*` paths answer anyone who can
reach your site, in every environment. They can read the stats and the
optimizer configuration, change the live configuration with
`PATCH /v1/config`, and purge the cache. The worker's own API token does not
apply here, and the only check on a change is an `X-Requested-With` header
that any client can send.
:::

Recommended for production: `Enabled: false`, for example in
`appsettings.Production.json` or as `PageSpeed__Console__Enabled=false`. On
IIS, you can set the variable in the site's `web.config`: add only this
`<environmentVariables>` block inside the `<aspNetCore>` element it already
has, and leave that element's other attributes as they are:

```xml
<environmentVariables>
  <environmentVariable name="PageSpeed__Console__Enabled" value="false" />
</environmentVariables>
```

`dotnet publish` regenerates `web.config` unless the project has its own, so
add the file to the project or repeat the edit after each publish.

`Enabled` follows [hot reload](#hot-reload), so you can switch the console on
for a support session and off again without a restart. A switch through hot
reload can take a few seconds to apply.

#### Gate the console with your own authentication

If you need the console in production, put your own authentication in front of
`MountPath` and the six `/v1/*` paths instead. Register the gate immediately
before `UsePageSpeed()`, after `UseAuthentication()` and after any
`UseRewriter()` or `UsePathBase()`, so it sees the same path the console does.
This example requires a signed-in user in the `Admin` role and uses whatever
authentication scheme your app already registers. At the top of `Program.cs`:

```csharp
using Microsoft.AspNetCore.Authentication;
```

In the pipeline, in place of the plain `app.UsePageSpeed();`:

```csharp
app.UseAuthentication();

string[] pageSpeedConsolePaths =
{
    "/console", // PageSpeed:Console:MountPath; change it here if you re-mount the console
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

#### Check an existing deployment

Every release since 2.0.13 serves the console and all six `/v1/*` paths this
way. The middleware does not authenticate these paths, so gate them as shown
above. 2.0.4 through 2.0.12 served the console and a smaller set of proxied
paths, also without authentication. From outside your network:

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

### Worker coordination and `SocketPath`

For the default in-process deployment, leave the `Worker` section empty. With
`Enabled` `true`, `AutoStart` `true` and `SocketPath` unset, the middleware
launches the worker and connects to it over an auto-resolved per-process
socket — image and critical-CSS optimization are on with no extra
configuration. While `Enabled` is `false`, the middleware does not launch
`factory_worker`. When a hot reload sets `Enabled` to `true`, the worker is
launched then, together with the cache volume, with no restart needed; setting
`Enabled` back to `false` leaves the running worker and the open cache in
place. `AutoStart` `false` means this host never launches a
child, whatever `Enabled` says.

There are two reasons to set `SocketPath` explicitly:

- **A concrete path** points the middleware at a socket you manage, so it can
  talk to a worker you run as a separate process (a split-process deployment
  where the worker lives in its own container or unit). The path is used
  verbatim.
- **`null` or an empty string** disables worker coordination entirely. The
  middleware still optimizes inline, but no out-of-process variants
  (image transcoding, critical-CSS extraction) are built, and the Dashboard
  stays at zero. The worker logs a startup warning so this state is never
  silent. Only set this if you specifically want inline-only behavior.

To run the worker as a fully separate process, pair a concrete `SocketPath`
with `AutoStart` `false` so the middleware connects to your externally managed
worker instead of launching its own. The middleware uses that socket whatever
the top-level `Enabled` says.

The application and the worker then share the cache volume, so they must run
on one host, and should run in one PID namespace. The cache is safe across PID
namespaces, but older builds recover a cross-process write lock from a holder
whose process the kernel reports gone, and a live holder in another
container's PID namespace looks gone to them; one namespace keeps an upgrade
safe while old and new builds share the volume. In Kubernetes, put both
containers in one pod with `shareProcessNamespace: true`; in compose, give
both `pid: 'service:<anchor>'` for a small container that only holds the
namespace.

## Hot reload

Configuration changes are picked up automatically through ASP.NET Core's
`IOptionsMonitor<PageSpeedOptions>` pattern. Edit appsettings.json while the
application is running — changes take effect on the next request. No restart
required.

This includes toggling the master switch, flipping any `Html.Enable*` toggle,
adjusting cache TTLs, and switching cache modes. Cache contents are preserved
across configuration changes. Worker-process options (`AutoStart`, `SocketPath`,
`ApiPort`) apply at the next worker (re)start. The first time the master switch
is on, at startup or through a reload, PageSpeed opens the cache volume and
launches the worker; switching it off later leaves both in place, so switching
it on again needs no restart either.

Validation runs on startup and on every reload via `IValidateOptions<PageSpeedOptions>`.
Negative TTLs or unknown `CacheMode` values cause the application to fail fast
on startup, and reload errors are logged.

## Environment-specific configuration

Use ASP.NET Core's standard environment layering. Create
`appsettings.Development.json` to override settings during development:

```json
{
  "PageSpeed": {
    "Enabled": false
  }
}
```

This disables optimization in development while leaving the rest of the
pipeline intact. Production uses the base appsettings.json.

## Environment variables

All options can be set via environment variables using the standard ASP.NET Core
double-underscore convention. Nested keys use `__` as the separator:

```bash
PageSpeed__Enabled=true
PageSpeed__CacheMode=Aggressive
PageSpeed__Cache__VolumePath=/data/pagespeed/volume.dat
PageSpeed__Cache__VolumeSizeBytes=2147483648
PageSpeed__Html__EnableCriticalCss=true
PageSpeed__Html__EnableSpeculationRules=false
PageSpeed__Worker__ApiPort=0
PageSpeed__Console__Enabled=false
```

Environment variables take precedence over appsettings.json, following ASP.NET
Core's standard configuration precedence.

## Rewrites you control

There is no rewrite-level abstraction and no filter-name list. Every rewrite is
a boolean toggle on `PageSpeed:Html` — see the table above. To match the
behaviour of a `PassThrough` configuration in 1.1, set `Enabled` to `false` (or
flip the individual `Enable*` toggles you want off). To bias toward
bandwidth-only rewrites, leave image and CSS toggles on and turn off
`EnableCriticalCss`, `EnableLcpPreload`, and `EnableSpeculationRules`. Turning
off `EnableCriticalCss` also suppresses async-CSS, since the middleware only
emits the async-CSS loader when critical-CSS inlining produces inlined rules
(and `EnableAsyncCss` already defaults `false`, so it is off unless you opt in).

Image transcoding (WebP, AVIF, resizing) and CSS/JS minification are handled by
the worker process and are not gated by `Html.Enable*` toggles. They run for
every cached asset.

## Cache volume

Optimized resources are stored in a single memory-mapped Cyclone volume file
defined by `Cache.VolumePath`. By default the volume lives at
`/var/cache/pagespeed/volume.dat` and is sized to 1 GiB. In containers or a
[production deployment](/docs/production-deployment/), point it at a persistent volume:

```json
{
  "PageSpeed": {
    "Cache": {
      "VolumePath": "/data/pagespeed/volume.dat",
      "VolumeSizeBytes": 4294967296
    }
  }
}
```

The parent directory must exist and be writable by the application process. The
volume file is created on first run and grown to `VolumeSizeBytes`. Reusing the
same file across restarts preserves the cache; pointing at a fresh path starts
cold.

While `Enabled` is `false`, the volume file and its directory are not created.
The first time `Enabled` is `true`, at startup or through a hot reload, the
volume is opened (and created when it does not exist yet), with no restart
needed. Turning `Enabled` off later leaves the open volume in place.

## Health checks

The middleware participates in ASP.NET Core health checks. Expose the standard
endpoint to surface its status:

```csharp
app.MapHealthChecks("/healthz");
```

The check reports `Healthy`, with the cache entry count and the middleware
version, while the cache is operational, and `Unhealthy` when the cache cannot
be opened. Before PageSpeed has been enabled, no cache is open: the check then
reports `Healthy` with the description `PageSpeed is disabled`. Use this endpoint for container orchestration liveness and readiness
probes.

## Common patterns

### Disable optimization for specific paths

The middleware skips any request whose path starts with a prefix in
`ExcludePaths`. The defaults already cover `/api/`, `/signalr/`, `/_blazor/`,
and `/_framework/`. Add your own:

```json
{
  "PageSpeed": {
    "ExcludePaths": ["/api/", "/signalr/", "/_blazor/", "/_framework/", "/webhooks/", "/healthz"]
  }
}
```

You can also exclude paths with standard ASP.NET Core middleware ordering —
register `UsePageSpeed()` after the routes that should bypass it, or use
`IApplicationBuilder.MapWhen` to conditionally skip the middleware.

### Bandwidth-only rewrites

Disable HTML-mutating toggles while leaving the worker's image and CSS
minification untouched:

```json
{
  "PageSpeed": {
    "Html": {
      "EnableCriticalCss": false,
      "EnableLcpPreload": false,
      "EnableAsyncCss": false,
      "EnableSpeculationRules": false,
      "EnableScriptDeferral": false
    }
  }
}
```

This is the closest equivalent to 1.1's `OptimizeForBandwidth` — safe for sites
with strict CSP policies that disallow inlined CSS.

## See also

- [Install ASP.NET Core middleware](/docs/aspnet-getting-started/) — installation and first setup
- [Production deployment](/docs/production-deployment/) — hardening, persistent cache volumes, and worker placement
- [Choose a cache mode](/docs/cache-modes/) — `Safe` vs `Aggressive` semantics
- [Troubleshoot common issues](/docs/troubleshooting/) — worker startup and cache
- [Software license](/license/) — Apache License 2.0
