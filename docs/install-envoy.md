# Installing PageSpeed for Envoy

**Status: Experimental**

## Availability

The Envoy HTTP filter is **experimental** and **not packaged for external
install** — no pre-built artifact is published (no apt/dnf package, container
image, or download). It can be built from the public mod_pagespeed
source tree (see `DEVELOPER.md`). CI builds the filter and runs its unit tests every night; the Envoy system tests below do not run in CI. **Contact us** for setup guidance.

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
  - name: pagespeed
    typed_config:
      "@type": type.googleapis.com/pagespeed.Decoder
      # Required legacy fields: any non-empty value.
      key: "placeholder"
      val: "placeholder"
      file_cache_path: "/var/cache/pagespeed/files/"
      log_dir: "/var/log/pagespeed/"
      virtual_hosts:
        - host_pattern: "*"
          options:
            rewrite_level: "CoreFilters"
            enabled_filters: "combine_css,combine_javascript,rewrite_images"
  - name: envoy.filters.http.router
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.router.v3.Router
```

The options are the fields of the `pagespeed.Decoder` message in `pagespeed/envoy/http_filter.proto`; other PageSpeed directives go in a virtual host's `custom_options` map.

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

The Envoy system tests do not run in CI, so no current pass and skip counts are published; the expected skips are listed in [envoy-limitations.md](envoy-limitations.md).

## Known Limitations

See [envoy-limitations.md](envoy-limitations.md) for full details.

Key differences from Apache:
- `X-PSA-Blocking-Rewrite` not supported (Envoy is non-blocking)
- IPRO cache lifetime differs (upstream max-age minus cache time)
- `combine_css` may timeout
- Outbound HTTPS uses system CA store via BoringSSL/libcurl, not Envoy's TLS config
