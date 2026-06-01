# Installing PageSpeed for Envoy

**Status: Experimental**

## Requirements

- Linux x86_64
- 32GB+ RAM for building from source

## Install Pre-built

### Standalone binary

```bash
tar xzf envoy-pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
cd envoy-pagespeed-1.1.0-beta.1

# Edit the sample config for your environment
vi pagespeed-envoy.yaml.sample

# Create cache directory
sudo mkdir -p /var/cache/pagespeed
sudo chown $(whoami) /var/cache/pagespeed

./envoy_pagespeed -c pagespeed-envoy.yaml.sample
```

### Shared library with existing Envoy

The shared library can be used with an existing Envoy deployment. Copy it
to a location Envoy can load:

```bash
tar xzf envoy-pagespeed-1.1.0-beta.1-linux-x86_64.tar.gz
sudo cp envoy-pagespeed-1.1.0-beta.1/pagespeed_filter.so /usr/local/lib/
```

Configure your Envoy to load the PageSpeed filter in its configuration.
The standalone binary is recommended for most deployments.

## Build from Source

```bash
docker compose up -d
docker compose exec dev bash

# Standalone binary (~212MB)
bazel build --config=clang-libstdcxx13 --jobs=4 //pagespeed/envoy:envoy_pagespeed

# Shared library (~113MB)
bazel build --config=clang-libstdcxx13 --jobs=4 //pagespeed/envoy:pagespeed_filter.so
```

Use `--jobs=2` to `--jobs=4` if memory-constrained.

## Configuration

See `pagespeed-envoy.yaml` in the repository root for a complete example.

Key configuration sections:
- PageSpeed filter configuration
- Admin authentication (token, IP allowlist, rate limiting)
- Redis cache backend
- Circuit breaker for resource fetching
- Prometheus metrics at `/stats/prometheus`
- Health endpoint at `/pagespeed/health`

### Minimal configuration snippet

```yaml
http_filters:
  - name: envoy.filters.http.pagespeed
    typed_config:
      "@type": type.googleapis.com/pagespeed.envoy.PagespeedFilterConfig
      pagespeed_options:
        - "ModPagespeed on"
        - "ModPagespeedFileCachePath /var/cache/pagespeed"
        - "ModPagespeedEnableFilters combine_css,combine_javascript,rewrite_images"
```

## Verification

```bash
curl -I http://localhost:8080/
# Should show: X-PageSpeed: 1.1.0-beta.1
```

## Key Metrics

- `pagespeed.html_rewrites_total` -- Total HTML pages rewritten
- `pagespeed.ipro_cache_hits` -- IPRO cache hit count
- `pagespeed.rewrite_latency_ms` -- Rewrite processing time

## System Tests

```bash
./test/system/run_envoy_tests.sh
./test/system/run_envoy_tests.sh -k sanity  # Quick check
```

Expected: 189 pass, 20 skip, 0 fail.

## Known Limitations

See [envoy-limitations.md](envoy-limitations.md) for full details.

Key differences from Apache:
- `X-PSA-Blocking-Rewrite` not supported (Envoy is non-blocking)
- IPRO cache lifetime differs (upstream max-age minus cache time)
- `combine_css` may timeout
- Outbound HTTPS uses system CA store via BoringSSL/libcurl, not Envoy's TLS config
