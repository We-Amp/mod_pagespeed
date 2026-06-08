# Installing PageSpeed for Envoy

**Status: Experimental**

## Availability

The Envoy HTTP filter is **experimental** and **not packaged for external
install** — no pre-built artifact is published (no apt/dnf package, container
image, or download). It can be built from the public mod_pagespeed
source tree (see `DEVELOPER.md`). **Contact us** for setup guidance.

The reference below documents how the filter is configured once a build is in
place.

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
# Should show an X-PageSpeed: <version> response header
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
