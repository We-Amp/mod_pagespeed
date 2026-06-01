# WeAmp.PageSpeed.AspNetCore

ASP.NET Core middleware for PageSpeed web optimization. Wraps Envoy+PageSpeed as an easy-to-use sidecar with automatic configuration generation, health checks, and ASP.NET Core logging integration.

## Quick Start

### 1. Install the Package

```bash
dotnet add package WeAmp.PageSpeed.AspNetCore
```

### 2. Install envoy_pagespeed Binary

The middleware requires the `envoy_pagespeed` binary to be available. Options:

- **PATH**: Install system-wide and ensure it's in PATH
- **Well-known locations**: `/usr/local/bin/`, `/opt/pagespeed/bin/`, or app directory
- **Configured path**: Set `PageSpeed:Sidecar:BinaryPath` in configuration

### 3. Configure Your Application

```csharp
// Program.cs
using WeAmp.PageSpeed.AspNetCore.DependencyInjection;

var builder = WebApplication.CreateBuilder(args);

// Add PageSpeed with configuration from appsettings.json
builder.Services.AddPageSpeed(builder.Configuration);

var app = builder.Build();

// Optional middleware (reserved for future features)
app.UsePageSpeed();

// Map health check endpoints
app.MapHealthChecks("/health");
app.MapPageSpeedHealthCheck("/health/pagespeed");
app.MapPageSpeedInfo("/pagespeed/info");

app.Run();
```

### 4. Configure appsettings.json

```json
{
  "PageSpeed": {
    "Enabled": true,
    "RewriteLevel": "CoreFilters",
    "Sidecar": {
      "ListenPort": 8080,
      "OriginPort": 5000
    },
    "Domains": {
      "AuthorizedDomains": ["localhost", "*.example.com"]
    },
    "AdminAuth": {
      "Enabled": true
    }
  }
}
```

### 5. Run Your Application

```bash
# Configure Kestrel to listen on the origin port (5000)
dotnet run --urls "http://localhost:5000"

# Access your app through the PageSpeed sidecar
curl http://localhost:8080/
```

## Architecture

```
Client Request (port 8080)
       ↓
Envoy + PageSpeed Filter (sidecar)
       ↓
Optimized Request → Kestrel (port 5000)
       ↓
Response flows back through PageSpeed
       ↓
Optimized HTML/CSS/JS/Images → Client
```

The sidecar:
- Listens on `ListenPort` (default: 8080)
- Forwards requests to your app on `OriginPort` (default: 5000)
- Automatically optimizes HTML, CSS, JavaScript, and images

## Configuration Options

### PageSpeedOptions

| Property | Default | Description |
|----------|---------|-------------|
| `Enabled` | `true` | Enable/disable PageSpeed optimization |
| `RewriteLevel` | `CoreFilters` | Optimization level: PassThrough, CoreFilters, MobilizeFilters, TestingCoreFilters, AllFilters |
| `EnabledFilters` | - | Comma-separated filter names to enable |
| `DisabledFilters` | - | Comma-separated filter names to disable |
| `ExcludePaths` | `[]` | Regex patterns for paths to exclude |

### SidecarOptions (PageSpeed:Sidecar)

| Property | Default | Description |
|----------|---------|-------------|
| `Mode` | `Process` | Sidecar mode: Process, Docker (future), External |
| `ListenPort` | `8080` | Port Envoy listens on |
| `OriginPort` | `5000` | Port Kestrel listens on |
| `AdminPort` | `9901` | Envoy admin interface port |
| `BinaryPath` | - | Path to envoy_pagespeed binary |
| `AutoRestart` | `true` | Auto-restart on crash |
| `MaxRestartAttempts` | `5` | Max restart attempts |
| `StartupTimeoutMs` | `30000` | Startup timeout |

### CacheOptions (PageSpeed:Cache)

| Property | Default | Description |
|----------|---------|-------------|
| `LruCacheSizeKb` | `512000` | In-memory LRU cache size (500 MB) |
| `FileCacheSizeKb` | `10240000` | File cache size limit (10 GB) |
| `FileCachePath` | temp dir | File cache directory |
| `LogDirectory` | temp dir | Log directory |

### RedisOptions (PageSpeed:Redis)

| Property | Default | Description |
|----------|---------|-------------|
| `Host` | `localhost` | Redis server hostname |
| `Port` | `6379` | Redis server port |
| `TimeoutUs` | `5000000` | Operation timeout (5 seconds) |
| `DatabaseIndex` | `0` | Redis database index |
| `TtlSeconds` | `86400` | Cache TTL (24 hours) |

### AdminAuthOptions (PageSpeed:AdminAuth)

| Property | Default | Description |
|----------|---------|-------------|
| `Enabled` | `true` | Enable admin authentication |
| `Token` | auto-generated | Bearer token for admin endpoints |
| `AllowedIps` | private ranges | Allowed IP addresses (CIDR) |
| `RateLimitRpm` | `60` | Rate limit (requests/minute) |

### DomainOptions (PageSpeed:Domains)

| Property | Default | Description |
|----------|---------|-------------|
| `AuthorizedDomains` | `["localhost", "127.0.0.1"]` | Domains authorized for rewriting |
| `RewriteMappings` | `[]` | CDN domain mappings |
| `OriginMappings` | `[]` | Origin domain mappings |

## Health Checks

The package integrates with ASP.NET Core health checks:

```csharp
// Map PageSpeed-specific health check
app.MapPageSpeedHealthCheck("/health/pagespeed");

// Or include in combined health check
app.MapHealthChecks("/health");
```

Health check returns:
- **Healthy**: Sidecar is running and responding
- **Degraded**: Sidecar is starting or restarting
- **Unhealthy**: Sidecar failed or stopped

## Admin Endpoints

When the sidecar is running, these endpoints are available:

| Endpoint | Description |
|----------|-------------|
| `/pagespeed/health` | Health check (no auth required) |
| `/pagespeed_admin` | Admin console (requires auth) |
| `/pagespeed_statistics` | Statistics JSON (requires auth) |

Access admin endpoints with the Bearer token:
```bash
curl -H "Authorization: Bearer <token>" http://localhost:8080/pagespeed_statistics
```

The token is auto-generated and logged at startup, or can be configured via `AdminAuth:Token`.

## Sidecar Modes

### Process Mode (Default)

Spawns `envoy_pagespeed` as a child process. Best for:
- Local development
- Single-server deployments
- Docker containers (include binary in image)

### External Mode

Connects to an externally-managed Envoy. Best for:
- Kubernetes with Envoy sidecar injection
- Service mesh deployments

```csharp
builder.Services.AddPageSpeedExternal(opts =>
{
    opts.Sidecar.ListenPort = 8080;
    opts.Sidecar.OriginPort = 5000;
});
```

## Troubleshooting

### Sidecar won't start

1. Check if `envoy_pagespeed` binary is installed and accessible
2. Verify ports aren't already in use
3. Check logs for startup errors

### No optimization occurring

1. Verify requests go through the sidecar port (8080), not directly to Kestrel (5000)
2. Check `AuthorizedDomains` includes your domain
3. Look for `.pagespeed.` in resource URLs to confirm optimization

### Health check failing

1. Check `/pagespeed/info` endpoint for sidecar state
2. Review sidecar logs in console output
3. Verify network connectivity between ports

## Building from Source

```bash
cd aspnetcore
dotnet build
dotnet test
```

## License

Apache-2.0
