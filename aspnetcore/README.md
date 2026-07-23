# WeAmp.PageSpeed.Sidecar

Add **mod_pagespeed 1.15** to your ASP.NET Core app with two lines of middleware. Your
Kestrel server stays the public front door — it owns the listening socket, auth, and
routing exactly like a normal ASP.NET Core app. A bundled, matched nginx with the
ngx_pagespeed module runs on loopback behind your app, optimizing responses before they
return to clients: it recompresses images (jpeg/png/webp) and, through opt-in filters,
transcodes to and recompresses AVIF; it minifies CSS and JS, inlines and extracts critical
CSS, and rewrites HTML for Core Web Vitals. Requests that
can't be optimized — and any request while the optimizer isn't running — pass straight
through your app un-optimized.

```csharp
builder.Services.AddPageSpeed(builder.Configuration);
// ...
app.UsePageSpeed();
```

That's the whole setup — no domain list and no separate front-proxy to configure. This is
the default **Inverse** topology (see
[Sidecar modes](#sidecar-modes) for the classic front-proxy alternative).

> **Linux only** (`linux-x64`, `linux-arm64`). Not sure which edition fits? See
> [mod_pagespeed 1.15 vs ModPageSpeed 2.0](https://modpagespeed.com/pricing). On Windows,
> use the IIS module; on macOS or in containers where bundling nginx isn't wanted, use
> [`WeAmp.PageSpeed.AspNetCore`](https://www.nuget.org/packages/WeAmp.PageSpeed.AspNetCore)
> (ModPageSpeed 2.0). The bundled nginx module includes the opt-in AVIF filters. Enable
> `convert_jpeg_to_avif` for photographic JPEGs; `convert_to_avif_lossless`,
> `convert_to_avif_animated` and `recompress_avif` cover flat-palette, animated and
> already-AVIF sources.

## How it works

In the default **Inverse** mode, your ASP.NET Core app is the public front door. The
package binds its public port (`Sidecar.ListenPort`, `8080` by default) and routes requests
through your pipeline exactly like a normal app. It serves plain HTTP; for HTTPS, front it
with a TLS terminator or load balancer (see [Public binding](#public-binding)).

When the `UsePageSpeed()` middleware runs in the pipeline:

1. For an optimizable response, the middleware streams the request over loopback (via
   YARP's `IHttpForwarder`) to the bundled nginx, which runs loopback-only.
2. nginx applies the ngx_pagespeed optimizations, then `proxy_pass`-es back to a second,
   private raw-origin Kestrel endpoint on loopback, where the middleware bypasses itself.
   The loop break is automatic and internal — you don't configure it.
3. The optimized response streams back through your app to the client.

Non-optimizable requests, and any request received while the sidecar isn't running, pass
straight through to your app un-optimized. Optimization is always additive: if nginx is
down, your app still serves traffic.

The loopback hop to nginx is plain HTTP. The middleware injects exactly one validated
`X-Forwarded-Proto` header so optimized asset URLs root at the public scheme your client
used. The package generates its own `nginx.conf`, runs nginx as a background service
(start, health, auto-restart, graceful stop), and registers ASP.NET Core health checks.

## Install

```bash
dotnet add package WeAmp.PageSpeed.Sidecar
```

This automatically pulls the matched native engine package
(`WeAmp.PageSpeed.Sidecar.NativeAssets.Linux`, which bundles the nginx + ngx_pagespeed
binaries for `linux-x64` and `linux-arm64`). Publish with a Linux RID so the bundled
nginx ships next to your app. A framework-dependent publish with no `-r` omits the nginx
binary and the sidecar fails to start:

```bash
dotnet publish -c Release -r linux-x64    # or: -r linux-arm64
```

With no license key, optimization runs in evaluation mode and adds an
`X-PageSpeed-Warn: unlicensed` header. That is expected; set `PageSpeed:LicenseKey` to
clear it.

## Usage

Register the services and add the middleware. The optimizer runs out of process in the
bundled nginx, not in-process.

```csharp
var builder = WebApplication.CreateBuilder(args);

// Reads the "PageSpeed" configuration section. Mode defaults to Inverse. By default the package binds and serves your app's public port (Sidecar.ListenPort, 8080) and runs the optimizer behind it. See "Public binding" to own the bind yourself.
builder.Services.AddPageSpeed(builder.Configuration);

var app = builder.Build();
app.UsePageSpeed();
app.MapHealthChecks("/health");
app.Run();
```

Or configure in code:

```csharp
builder.Services.AddPageSpeed(options =>
{
    options.RewriteLevel = "CoreFilters";
    options.LicenseKey = builder.Configuration["PageSpeed:LicenseKey"]; // BYOL, from a secret
});
```

You do **not** list domains to make optimization work. By default the optimizer rewrites
same-origin resources for any host your app serves, with zero configuration (`localhost`
included, for local dev). `Domains.AuthorizedDomains` is not a setup step — see
[Host authorization](#host-authorization) for the advanced cases where it applies.

`AddPageSpeed` registers a health check named `pagespeed` (tagged `pagespeed`, `sidecar`)
into the ASP.NET Core health-check set, so the `app.MapHealthChecks("/health")` endpoint
above reflects bundled-nginx liveness. To expose a PageSpeed-only endpoint, call
`app.MapPageSpeedHealthCheck("/health/pagespeed")` (it filters on the `pagespeed` tag).

### Public binding

By default (`Sidecar.OwnPublicPort = true`) the package binds the public endpoint on
`0.0.0.0:Sidecar.ListenPort` (default `8080`) and runs the loopback optimizer behind it.
The public port and the private optimizer endpoint are both bound by the package, so there
is nothing to coordinate. The package serves plain HTTP; for HTTPS, front it with a TLS
terminator or load balancer.

To own the public bind yourself, set `Sidecar.OwnPublicPort = false` and configure your
endpoint via `Kestrel:Endpoints` in `appsettings.json` (which supports HTTPS certificates).
Note: `--urls`, `UseUrls`, and `ASPNETCORE_URLS` do **not** bind the public port in this
mode — Kestrel overrides those hosting URLs once the sidecar adds its loopback endpoint, so
use `Kestrel:Endpoints`.

### Configuration (`appsettings.json`)

```json
{
  "PageSpeed": {
    "Enabled": true,
    "RewriteLevel": "CoreFilters",
    "Sidecar": {
      "OriginPort": 0
    },
    "Cache": {
      "FileCachePath": "/var/cache/pagespeed",
      "FileCacheSizeKb": 10240000
    },
    "AdminAuth": { "Enabled": true }
  }
}
```

The defaults above are the Inverse defaults. You can omit the `Sidecar` block entirely;
it's shown to name the keys.

| Key | Default | Required? | Notes |
|---|---|---|---|
| `Enabled` | `true` | optional | Master switch for optimization. |
| `RewriteLevel` | `CoreFilters` | optional | One of `PassThrough`, `CoreFilters`, `OptimizeForBandwidth`, `MobilizeFilters`, `TestingCoreFilters`, `AllFilters`. |
| `EnabledFilters` | unset | optional | Comma-separated filter names to enable on top of `RewriteLevel` (e.g. `"rewrite_css,rewrite_javascript,recompress_images"`). Spaces are stripped and the list is checked for syntax only (`[-+a-z0-9_,]`); the filter names themselves are validated by the optimization engine at startup. |
| `DisabledFilters` | unset | optional | Comma-separated filter names to turn off. Disabling wins over enabling, including over filters implied by `RewriteLevel`. Same syntax rules as `EnabledFilters`. |
| `Sidecar.Mode` | `Inverse` | optional | `Inverse` (default — your middleware is the public front door, nginx optimizes on loopback), `Process` (classic front-proxy — bundled nginx is the public front door, Kestrel the private origin), `External` (connect to an operator-managed nginx). `Docker` is reserved for a future release and fails config validation. |
| `Sidecar.ListenPort` | `8080` | optional | The public port the package binds in Inverse (the default, since `OwnPublicPort` is `true`) and the public nginx port in Process. |
| `Sidecar.OwnPublicPort` | `true` | optional | Inverse only. When `true` (default), the package binds the public endpoint on `0.0.0.0:Sidecar.ListenPort`. Set `false` to own the bind yourself via `Kestrel:Endpoints` (note: `--urls` / `UseUrls` / `ASPNETCORE_URLS` are overridden once the loopback endpoint is added and will not bind publicly). Ignored in Process/External. |
| `Sidecar.UseLaunchShim` | `true` | optional | Inverse/Process, Linux only. On by default: launches the bundled nginx through the native PR_SET_PDEATHSIG shim so a hard SIGKILL/OOM/container-hard-stop of the host can't orphan nginx (and leave it holding its loopback port). Degrades gracefully on packages built without the shim binary. Set `false` to force a direct launch. |
| `Sidecar.OriginPort` | `0` | optional | The private raw-origin loopback port. `0` = auto-select. In Inverse it is **always loopback-TCP, never a Unix socket** (the loop break needs an authoritative `LocalPort`). In Process/External, `0` prefers a private Unix-domain socket and falls back to an ephemeral loopback port. You normally leave this at `0`. |
| `Sidecar.NginxLoopbackPort` | `0` | optional | Inverse only. The loopback-only port nginx listens on. `0` = auto. Ignored in Process/External. |
| `Sidecar.RestrictToAuthorizedHosts` | `false` | optional | Inverse only, defense-in-depth. When `true`, the middleware optimizes **only** hosts in `Domains.AuthorizedDomains` (loopback always allowed); every other host is served un-optimized. When `false` (default), all hosts are optimized (forward-all). Ignored in Process/External. |
| `Sidecar.AllowPublicAdmin` | `false` | optional | Inverse only. When `true`, exposes the `/pagespeed_*` admin endpoints from the public front door; requires `AdminAuth.Enabled = true` (enforced by config validation). When `false` (default), those paths return 404 from the public endpoint. Ignored in Process/External. |
| `Domains.AuthorizedDomains` | `["localhost", "127.0.0.1"]` | optional | Hosts authorized for **cross-origin** rewriting (e.g. a CDN or separate asset host). A request's own same-origin resources are always authorized without listing the host here, so this is **not** required for normal optimization. In Inverse it becomes a strict host allowlist only when `Sidecar.RestrictToAuthorizedHosts = true`; otherwise it governs the cross-origin/CDN rewriting preview. The default seed is inert for same-origin. |
| `Cache.FileCachePath` | system temp dir | optional | On-disk cache location for optimized resources. |
| `Cache.FileCacheSizeKb` | `10240000` (10 GB) | optional | File-cache size cap, in KB. |
| `AdminAuth.Enabled` | `true` | optional | Gates the `/pagespeed_*` admin endpoints behind a bearer token. Required to be `true` if `Sidecar.AllowPublicAdmin = true` (the bearer is the only gate for public access). |

Source `LicenseKey` and `AdminAuth.Token` from a secret store or environment variable.
Do not put them in `appsettings.json`.

### Host authorization

Optimization is forward-all by default: any host your app serves is optimized with no
domain configuration, because mod_pagespeed implicitly authorizes each request's own
same-origin resources. You do not need to enumerate your hostnames.

Two advanced cases use `Domains.AuthorizedDomains`:

- **Defense-in-depth.** Set `Sidecar.RestrictToAuthorizedHosts = true` to optimize only
  the hosts you list (loopback is always allowed); all other hosts are served
  un-optimized.
- **Cross-origin / CDN rewriting** (preview). When a future release rewrites asset URLs
  onto a separate origin, that origin must be listed here. Same-origin resources never
  need a listing.

## Sidecar modes

`Sidecar.Mode` selects the topology:

- **`Inverse`** (default) — Your ASP.NET Core middleware is the public front door; the
  bundled nginx runs loopback-only as an optimize-proxy behind it. The package binds the
  public port (`Sidecar.ListenPort`, `8080`) and serves plain HTTP; front it with a TLS
  terminator for HTTPS. Use this for an existing ASP.NET Core app where Kestrel stays the
  front door: add the middleware, set the port if you want, deploy.
- **`Process`** — The classic front-proxy. The bundled nginx is the public front door and
  Kestrel is the private origin behind it. nginx serves plain HTTP, so front it with a
  TLS terminator or load balancer. In this mode `Sidecar.ListenPort` is the public nginx
  port and `Sidecar.OriginPort` `0` prefers a private Unix-domain socket. Still supported;
  select it with `Sidecar.Mode = "Process"`.
- **`External`** — Connect to an nginx you manage yourself. Port semantics match Process.
- **`Docker`** — Reserved for a future release; fails config validation today.

## Container & runtime requirements

The bundled nginx + module are glibc ELF binaries built for a glibc ≥ 2.31 floor, so they
run on Debian 11/12, Ubuntu 20.04+, RHEL/Alma 9, and Amazon Linux 2023. `libstdc++` is
statically linked and the build has no OpenSSL dependency, so there is no libstdc++ or
libssl/libcrypto host requirement. The binaries link a small set of host shared libraries:

| Host library | Provided by | Notes |
|---|---|---|
| `libpcre.so.3` | `libpcre3` | nginx PCRE — **missing from `-chiseled`/`-distroless` images** |
| `libz.so.1` | `zlib1g` | present on standard .NET images |
| `libcrypt.so.1` | `libcrypt1` | present on standard .NET images |

- **Recommended base image:** `mcr.microsoft.com/dotnet/aspnet:8.0` (Debian); add the one
  missing lib with `apt-get install -y libpcre3`.
- **Not supported:** `-chiseled` / `-distroless` .NET images (no `libpcre3`) and
  Alpine/musl images (the binaries are glibc-linked). Use a Debian/Ubuntu glibc base.
  nginx still runs in Inverse mode — on loopback — so the same base-image requirements
  apply.

If a required library is missing, the sidecar fails fast at startup: nginx `-t` reports a
shared-library load error which the sidecar surfaces as a startup exception.

## Deployment & shutdown

Launch your app as `dotnet YourApp.dll` (the default `ENTRYPOINT` for the `dotnet publish`
container tooling and `mcr.microsoft.com/dotnet/aspnet` images). The runtime then forwards
`SIGTERM` (from `docker stop` / Kubernetes / systemd) to the host's graceful shutdown,
which stops the bundled nginx cleanly. Launching the apphost directly (`./YourApp`) does
not reliably propagate `SIGTERM`, so the bundled nginx can survive shutdown and keep
holding its loopback port. Use `dotnet YourApp.dll` in production. By default the bundled
nginx is launched through a `PR_SET_PDEATHSIG` shim (`Sidecar.UseLaunchShim`, on by
default) as a second line of defense: it prevents the bundled nginx from being orphaned
if the host process is hard-killed (SIGKILL, OOM, container hard-stop).

## Verify it's working

In Inverse mode (the default), curl the public port the package binds
(`Sidecar.ListenPort`, `8080` by default) and look for the optimizer's response header:

```bash
curl -sI http://localhost:8080/ | grep -i x-page-speed
```

In Process mode, curl the public nginx port (`Sidecar.ListenPort`) instead.

A present `X-Page-Speed` header confirms the optimizer is in the path. During evaluation
you will also see `X-PageSpeed-Warn: unlicensed` — that is expected, not an error; set
`PageSpeed:LicenseKey` to clear it. For per-filter rewrite counts, check
`/pagespeed_statistics` (see below for how it's exposed).

## Admin endpoints

The `/pagespeed_*` admin endpoints are `/pagespeed_admin`, `/pagespeed_statistics`,
`/pagespeed_global_statistics`, `/pagespeed_console`, and `/pagespeed_message`.

In Inverse mode (the default) these endpoints return **404 from the public front door**.
They are not exposed by default — your middleware is the public door and shields nginx.
To expose them, set `Sidecar.AllowPublicAdmin = true`, which requires
`AdminAuth.Enabled = true` (the bearer token is the only gate for public access; config
validation enforces this pairing). Retrieve the generated token via
`ISidecarManager.AdminToken`, or set a fixed `AdminAuth.Token` from a secret.

In Process mode the endpoints are gated to the loopback ACL (`AdminAuth.AllowedIps`,
default loopback only) and, when `AdminAuth.Enabled`, behind the bearer token. Widen
`AdminAuth.AllowedIps` only if you understand the exposure.

## Licensing

The product is commercially licensed under the Business Source License 1.1 (BUSL-1.1; see
the bundled `LICENSE`). Production use requires a BUSL-1.1 license. Without a license key,
optimization runs in evaluation mode and adds an `X-PageSpeed-Warn: unlicensed` header.
Provide your license token via `PageSpeed:LicenseKey` (BYOL) to clear the warning; the
sidecar writes it next to the cache and the nginx worker remains the cryptographic
authority.

Under BUSL-1.1, each version becomes available under the Apache License 2.0 four years
after its first public release (the **Change Date**; see the packaged `LICENSE` for the
exact date and terms).

## Platform support

| Platform | Support |
|---|---|
| `linux-x64`, `linux-arm64` | ✅ this package |
| Windows | ❌ use the IIS module |
| macOS / other | ❌ use [`WeAmp.PageSpeed.AspNetCore`](https://www.nuget.org/packages/WeAmp.PageSpeed.AspNetCore) (ModPageSpeed 2.0) |

See [mod_pagespeed 1.15 vs ModPageSpeed 2.0 — which fits](https://modpagespeed.com/pricing)
and the [mod_pagespeed 1.15 overview](https://modpagespeed.com/1.1/).

---

© 2024–2026 We-Amp B.V. Licensed under the Business Source License 1.1 (BUSL-1.1).
