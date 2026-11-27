# ASP.NET Core PageSpeed Integration

.NET middleware that integrates mod_pagespeed's image optimization and HTML
processing into ASP.NET Core applications via native P/Invoke to `libpagespeed.so`.

## Architecture

Two NuGet packages:

| Package | Purpose |
|---------|---------|
| `WeAmp.PageSpeed` | Low-level P/Invoke bindings to `libpagespeed.so` (C API) |
| `WeAmp.PageSpeed.AspNetCore` | ASP.NET middleware, health checks, worker notification |

The middleware intercepts responses, passes them through the native library for
optimization, and notifies the external worker for async variant generation.

## Directory Structure

- `src/WeAmp.PageSpeed/` - Native bindings (Constants, Enums, Interfaces, P/Invoke)
- `src/WeAmp.PageSpeed.AspNetCore/` - Middleware, health checks, DI extensions
- `test/` - xUnit tests for both packages
- `samples/DemoSite/` - Full demo with Docker Compose (ASP.NET + worker)
- `samples/BasicWebApp/` - Minimal integration example

## Build & Test

```bash
dotnet build WeAmp.PageSpeed.sln      # Build all projects
dotnet test                            # Run all .NET tests
```

## Demo Stack

```bash
cd samples/DemoSite
./run-demo.sh                          # Full stack (ASP.NET + worker) via Compose
./run-demo.sh --standalone             # Single container (no worker, no caching)
./run-demo.sh --build-only             # Build images only
docker compose down -v                 # Stop + clean
```

ASP.NET :5200, worker API :9980 (mapped from :9880), console :9980/console/.
Multi-stage Dockerfile: `build-native` (Bazel → libpagespeed.so), `build-dotnet`,
`build-console` (SvelteKit), `aspnet-runtime`, `worker-runtime`.

Env vars: `PAGESPEED_CACHE_PATH` (default `/shared/cache.vol`),
`PAGESPEED_SOCKET_PATH` (default `/shared/pagespeed.sock`).

## Key Integration Points

- Cache file shared between ASP.NET process and worker via mmap (`enable_mmap_directory`)
- Worker notifications sent over Unix domain socket
- `libpagespeed.so` built from `lib/pagespeed/` (C API wrapper around C++ libraries)
