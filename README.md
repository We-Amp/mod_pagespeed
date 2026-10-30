# mod_pagespeed

Web performance optimization middleware that automatically applies 40+
optimization filters to web pages, including image compression/resizing,
CSS/JS minification, cache extension, and more.

Originally created by Google, now maintained by [We-Amp](https://we-amp.com).

## Platforms

| Platform | Module | Status |
|----------|--------|--------|
| **Apache 2.4+** | `mod_pagespeed.so` | Stable |
| **Nginx (per-distro stock)** | `ngx_pagespeed_module.so` | Stable |
| **Envoy** | `pagespeed_filter.so` / `envoy_pagespeed` | Experimental |
| **IIS 10+** | `pagespeed_iis.dll` | Stable |

## Features

- Image optimization: compression, resizing, format conversion (WebP)
- CSS & JavaScript: concatenation, minification, inlining
- Cache extension, domain sharding, domain rewriting
- Deferred loading of JavaScript and image resources
- DNS prefetching, preload hints
- 40+ configurable optimization filters
- Security patches for the CVEs that accumulated against the archived
  upstream, hardened builds, and active supply-chain management

## Quick Start

### Pre-built packages

All packages and installers are on the
[downloads page](https://modpagespeed.com/download/): signed apt/dnf
packages for Apache (`mod-pagespeed`) and Nginx (`nginx-module-pagespeed`) via
`packages.modpagespeed.com`, and the signed IIS MSI. The Apache module
package pairs with the `pagespeed-optimizer` daemon package at the same
version and pulls it in as a dependency. The nginx dynamic
module ships prebuilt and signed for Debian 12/13 and Ubuntu 22.04/24.04
(amd64 + arm64), each pinned to that distribution's stock nginx version.
Debian 11 stays on the 1.15 line.
Then see the per-platform installation guides:

- [Apache](docs/install-apache.md)
- [Nginx](docs/install-nginx.md)
- [IIS](docs/install-iis.md)
- [Envoy](docs/install-envoy.md) — experimental

### Configuration and operations

Full product documentation lives on modpagespeed.com:

- [Filter selection](https://modpagespeed.com/docs/filter-selection/) -- rewrite levels and per-filter enable/disable
- [Filter reference](https://modpagespeed.com/docs/filters/) -- what each filter does and which level enables it
- [Configuration](https://modpagespeed.com/docs/configuration/) -- directives and baseline setup
- [Admin console](https://modpagespeed.com/docs/admin-console/) -- statistics, cache inspection, and purging

### Build from Source

All builds run inside a Docker container (provides Clang, GCC 13, Bazel 8.x):

```bash
# Start development environment (includes Redis and Memcached)
docker compose up -d
docker compose exec dev bash

# Build for your target platform
bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so                   # Apache
bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_module.so # Nginx
bazel build --config=envoy-filter //pagespeed/envoy:envoy_pagespeed              # Envoy (Envoy 1.40)

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

See [DEVELOPER.md](DEVELOPER.md) for build configurations, native builds,
debugging and common problems, and [CONTRIBUTING.md](CONTRIBUTING.md) for
how to report bugs and send pull requests.

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
- [DEVELOPER.md](DEVELOPER.md) -- Developer guide: build, test, debugging
- [CONTRIBUTING.md](CONTRIBUTING.md) -- Reporting bugs and sending pull requests
- [docs/](docs/) -- Installation guides, platform limitations, test catalog

## Security

mod_pagespeed is enrolled in [Anthropic's OSS Scanner](https://github.com/anthropics/oss-scanner/blob/main/projects/mod_pagespeed/project.yaml). The [threat model](.oss-scanner/threat_model.md) is in this repository.

[![Fuzzing Status](https://oss-fuzz-build-logs.storage.googleapis.com/badges/mod_pagespeed.svg)](https://bugs.chromium.org/p/oss-fuzz/issues/list?sort=-opened&can=1&q=proj:mod_pagespeed)

mod_pagespeed has been enrolled in [OSS-Fuzz](https://github.com/google/oss-fuzz/tree/master/projects/mod_pagespeed), Google's fuzzing service for open-source projects, since &lt;MONTH YEAR&gt;, and is fuzzed continuously. Report security issues to security@modpagespeed.com (see [SECURITY.md](SECURITY.md)).

## License

Apache License 2.0. See [LICENSE](LICENSE) for details.

## Links

| | |
|---|---|
| Source | https://github.com/we-amp/mod_pagespeed |
| Issues | https://github.com/we-amp/mod_pagespeed/issues |
| Downloads | https://modpagespeed.com/download/ |
| Upgrade from open-source | https://modpagespeed.com/docs/migrating-to-2-1/ |
| Support | https://modpagespeed.com/contact/ |

## Background

mod_pagespeed was created at Google in 2010 and went on to power web
performance optimization across hundreds of thousands of sites. We-Amp's
role is documented in primary sources: the
[Apache Incubator PageSpeed proposal](https://cwiki.apache.org/confluence/display/INCUBATOR/PageSpeedProposal)
lists We-Amp B.V. as a founding committer organization alongside Google,
and
[Google's 2013 ngx_pagespeed announcement](https://developers.googleblog.com/en/speed-up-your-sites-with-pagespeed-for-nginx/)
named We-Amp among the module's contributors. After Google archived the
project, We-Amp continued development in the releases this repository
carries.
