# mod_pagespeed 1.15.0 Release Notes

**Release date:** 2026-06-01
**Status:** Stable

## Overview

mod_pagespeed 1.15.0 renumbers the maintained Apache-lineage line from 1.1 to
1.15. 1.15 is the direct successor to Google's final mod_pagespeed
release (1.14.36.1) and reads as the newest maintained build on the package and
version surfaces (dnf/yum/apt, WHM EasyApache 4) where the prior "1.1" numbering
looked older than Google's line.

This is a version-identity change: there is **no functional change** from the
1.1.0 line. Package names (`mod-pagespeed`, `nginx-module-pagespeed`,
`ea-apache24-mod_pagespeed`) and repository channels are unchanged, so existing
install commands keep working. The `X-Mod-Pagespeed` response header now reports
`1.15.0.0`. For the full change history see [CHANGELOG.md](CHANGELOG.md).

---

_Historical release notes for earlier releases follow._

# mod_pagespeed 1.1.0-beta.1 Release Notes

**Release date:** 2026-02-25
**Status:** Pre-release (beta)

## Overview

This is the first release of mod_pagespeed under We-Amp stewardship. The
codebase has been extensively modernized: new build system (Bazel 7.x),
modern C++ (C++20/23), and three new deployment platforms alongside the
original Apache module.

## Platform Support

| Platform | Module | Status | Tested |
|----------|--------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable | 195 pass, 14 skip |
| **Nginx 1.26/1.27** | `ngx_pagespeed_module.so` | Stable | 169 pass, 40 skip |
| **Envoy** | `pagespeed_filter.so` / `envoy_pagespeed` | Experimental | 189 pass, 20 skip |
| **IIS 10+** | `pagespeed_iis.dll` | Experimental | 350 pass (Python), 19 pass (C++) |

## Quick Start

### Apache

```bash
# Ubuntu/Debian
sudo dpkg -i mod-pagespeed-beta_1.1.0-beta.1_amd64.deb
sudo a2enmod pagespeed
sudo systemctl restart apache2

# RHEL/Rocky
sudo rpm -i mod-pagespeed-beta-1.1.0-beta.1.x86_64.rpm
sudo systemctl restart httpd
```

### Nginx

```bash
tar xzf ngx_pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
sudo cp ngx_pagespeed-1.1.0-beta.1/ngx_pagespeed_module.so /usr/lib/nginx/modules/
# Add to nginx.conf: load_module modules/ngx_pagespeed_module.so;
sudo nginx -t && sudo systemctl restart nginx
```

### Envoy

```bash
tar xzf envoy-pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
cd envoy-pagespeed-1.1.0-beta.1
sudo mkdir -p /var/cache/pagespeed
./envoy_pagespeed -c pagespeed-envoy.yaml.sample
```

### IIS

Requires: Visual C++ Redistributable 2022

```powershell
Expand-Archive pagespeed-iis-1.1.0-beta.1-win-x64.zip C:\inetpub\pagespeed
Stop-Service W3SVC
New-WebGlobalModule -Name PageSpeedModule -Image 'C:\inetpub\pagespeed\pagespeed_iis.dll'
Start-Service W3SVC
```

### Build from Source

```bash
docker compose up -d
docker compose exec dev bash
bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so
```

See [docs/install-apache.md](docs/install-apache.md), [docs/install-nginx.md](docs/install-nginx.md),
[docs/install-envoy.md](docs/install-envoy.md), or [docs/install-iis.md](docs/install-iis.md) for
detailed instructions.

## What's New

- **Bazel build system** replacing GYP/gyp_chromium
- **C++20/C++23** with Clang + GCC 13 libstdc++
- **Cyclone cache** for high-performance disk caching
- **CurlUrlAsyncFetcher** replacing Serf (linked against BoringSSL)
- **Envoy filter** with IPRO, HTML rewriting, Redis cache, Prometheus metrics
- **Nginx dynamic module** for 1.26.x and 1.27.x
- **IIS native module** with full HTML rewriting and IPRO
- **Python test framework** shared across all platforms
- **GitHub Actions CI/CD** with automated release packaging
- **Docker development environment**

See [CHANGELOG.md](CHANGELOG.md) for full details.

## Known Limitations

### Envoy (Experimental)

- `combine_css` may timeout due to async worker pool coordination
- IPRO cache lifetime differs from Apache behavior
- `X-PSA-Blocking-Rewrite` header not supported (Envoy is non-blocking)
- See [docs/envoy-limitations.md](docs/envoy-limitations.md)

### IIS (Experimental)

- IPRO async cache returns misses (Redis/disk caches not functional for IPRO)
- Beacon data silently discarded
- Requires `allowDoubleEscaping="true"` in IIS configuration
- See [docs/iis-limitations.md](docs/iis-limitations.md)

### Nginx

- Lazyload images filter times out on some configurations
- Inline preview images filter times out
- See [docs/test-catalog.md](docs/test-catalog.md) for per-platform skip details

## Reporting Issues

Please report issues at: https://github.com/we-amp/mod_pagespeed/issues

Include:
- Platform (Apache/Nginx/Envoy/IIS) and version
- Operating system and version
- Configuration snippet (relevant PageSpeed directives)
- Steps to reproduce
