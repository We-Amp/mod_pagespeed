---
title: 'Envoy HTTP filter (experimental)'
description: 'PageSpeed for Envoy is an experimental HTTP filter built from the public source repository, not a packaged release: configuration, checks, known limits.'
order: 8
group: 'Install'
lastUpdated: 2026-10-30
datePublished: 2026-10-08
---

PageSpeed for Envoy is **experimental** and not packaged: no deb, rpm,
container image or download is published for it. The filter is built from the
[public source repository](https://github.com/We-Amp/mod_pagespeed) (its
`DEVELOPER.md` has the build setup), and we ask that you
[contact us](/contact/) before running it anywhere that matters. The packaged,
supported platforms are Apache, nginx and IIS; start at
[Getting started](/docs/getting-started/).

This page documents how the filter is configured once a build is in place, how
to confirm it runs, and where its behaviour differs from Apache and nginx.

## Configuration

The filter sits in the Envoy listener's `http_filters` chain. The repository's
`pagespeed-envoy.yaml` is a complete, commented example; the essential part is
the filter entry with its cache, log, admin and domain settings:

```yaml
http_filters:
  - name: pagespeed
    typed_config:
      "@type": type.googleapis.com/pagespeed.Decoder
      key: "placeholder" # required by the current schema
      val: "placeholder"
      file_cache_path: "/var/cache/pagespeed/files/"
      log_dir: "/var/log/pagespeed/"
      lru_cache_kb_per_process: 512000
      file_cache_size_kb: 10240000
      admin_auth:
        enabled: true
        token: "<your-token>" # openssl rand -hex 32
        allowed_ips: ["10.0.0.0/8", "127.0.0.1/32"]
        rate_limit_rpm: 60
      domains:
        authorized_domains: ["*.example.com", "localhost"]
```

The example file also shows a Redis cache backend for distributed caching, the
circuit breaker for resource fetching and CDN domain mapping. The admin pages
are at `/pagespeed_admin`, `/pagespeed_statistics`, `/pagespeed_global_admin`
and `/pagespeed_global_statistics`; with `admin_auth` enabled they require the
bearer token and an allowed source address.

## Verify it runs

```bash
curl -I http://localhost:8080/
```

The response carries the header the shared rewriting path adds,
`X-Page-Speed: on`. The value is generic by default; set the server-wide
[`ExposeVersionInHeaders`](/docs/configuration/#expose-version-in-headers) option
to send the version instead (a build made without the workspace-status script
then shows a placeholder). The health endpoint answers at
`/pagespeed/health`, and Prometheus metrics are on Envoy's admin port at
`/stats/prometheus`: `pagespeed.requests_total`,
`pagespeed.html_rewrites_total`, `pagespeed.ipro_cache_hits` and
`pagespeed.rewrite_latency_ms` are the ones to watch.

## Known limitations

Envoy is event-driven and the filter brings its own fetcher; both show in a few
places:

- `X-PSA-Blocking-Rewrite` is not supported: there is no blocking rewrite. Poll
  for the optimized result instead.
- Outbound HTTPS uses the system CA store through BoringSSL and libcurl, not
  Envoy's TLS configuration.
- In-place resource optimization returns the upstream `max-age` minus the time
  the resource spent in the cache rather than `implicit_cache_ttl_ms`, and
  does not produce the `PSA-aj` ETag pattern.
- The `PageSpeedFilters` request header is not respected for in-place resource
  requests.
- The `resource_404_count` statistic is not tracked.
- Responses may be chunked differently, so `Content-Length` handling differs
  from Apache.

## Next steps

- [Getting started](/docs/getting-started/): the packaged integrations
- [Configuration reference](/docs/configuration/): the shared directive
  reference
