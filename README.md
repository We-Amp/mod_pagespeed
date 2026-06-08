# mod_pagespeed

Web performance optimization middleware that automatically applies 40+
optimization filters to web pages, including image compression/resizing,
CSS/JS minification, cache extension, and more.

Originally created by Google, now maintained by [We-Amp](https://we-amp.com).

## Platforms

| Platform | Module | Status |
|----------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable |
| **Nginx 1.26/1.27** | `ngx_pagespeed_module.so` | Stable |
| **Envoy** | `pagespeed_filter.so` / `envoy_pagespeed` | Experimental |
| **IIS 10+** | `pagespeed_iis.dll` | Experimental |

## Features

- Image optimization: compression, resizing, format conversion (WebP)
- CSS & JavaScript: concatenation, minification, inlining
- Cache extension, domain sharding, domain rewriting
- Deferred loading of JavaScript and image resources
- DNS prefetching, preload hints
- 40+ configurable optimization filters

## Quick Start

### Pre-built packages

All packages and installers are on the
[downloads page](https://modpagespeed.com/1.1/docs/downloads/): signed apt/dnf
packages for Apache (`mod-pagespeed`) and Nginx (`nginx-module-pagespeed`) via
`packages.modpagespeed.com`, and the signed IIS MSI. Then see the per-platform
installation guides:

- [Apache](docs/install-apache.md)
- [Nginx](docs/install-nginx.md)
- [IIS](docs/install-iis.md)
- [Envoy](docs/install-envoy.md) — experimental

### Configuration and operations

Full product documentation lives on modpagespeed.com:

- [Filter selection](https://modpagespeed.com/1.1/docs/filter-selection/) -- rewrite levels and per-filter enable/disable
- [Filter reference](https://modpagespeed.com/1.1/docs/filter-reference/) -- what each filter does and which level enables it
- [Configuration](https://modpagespeed.com/1.1/docs/configuration/) -- directives and baseline setup
- [Admin console](https://modpagespeed.com/1.1/docs/admin-console/) -- statistics, cache inspection, and purging

### Build from Source

All builds run inside a Docker container (provides Clang, GCC 13, Bazel 7.x):

```bash
# Start development environment (includes Redis and Memcached)
docker compose up -d
docker compose exec dev bash

# Build for your target platform
bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so                   # Apache
bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_module.so # Nginx
bazel build --config=clang-libstdcxx13 //pagespeed/envoy:envoy_pagespeed         # Envoy

# Run C++ unit tests
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/... //test/net/...

docker compose down
```

IIS builds require Windows with clang-cl:
```powershell
bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll
```

See [CLAUDE.md](CLAUDE.md) for detailed build configuration options and
architecture documentation.

## System Tests

```bash
./test/system/run_system_tests.sh          # Apache
./test/system/run_nginx_tests.sh           # Nginx
./test/system/run_envoy_tests.sh           # Envoy
./test/system/run_iis_tests.sh sanity      # IIS (from Linux, requires Windows VM)
```

## Documentation

- [CHANGELOG.md](CHANGELOG.md) -- Release history
- [RELEASE_NOTES.md](RELEASE_NOTES.md) -- Current release details
- [CLAUDE.md](CLAUDE.md) -- Build system, architecture, and development guide
- [docs/](docs/) -- Installation guides, platform limitations, test catalog

## License

Apache License 2.0. See [LICENSE](LICENSE) for details.

## Links

| | |
|---|---|
| Source | https://github.com/we-amp/mod_pagespeed |
| Issues | https://github.com/we-amp/mod_pagespeed/issues |
| Downloads | https://modpagespeed.com/1.1/docs/downloads/ |
