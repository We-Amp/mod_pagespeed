---
title: 'Is it working?'
description: 'Confirm mod_pagespeed is active: the response header per server, MISS then HIT, ?PageSpeed=off, the admin console and the worker health endpoint.'
order: 2
group: 'Start here'
lastUpdated: 2026-10-30
datePublished: 2026-10-08
faq:
  - q: 'Which response header tells me mod_pagespeed is active?'
    a: 'It depends on the integration. The Apache module adds `X-Mod-Pagespeed`, the nginx and IIS modules add `X-Page-Speed`, each carrying `on` by default from 2.7.0 (the version only when you set `ExposeVersionInHeaders`). The Docker reverse proxy and the ASP.NET Core middleware add `X-PageSpeed` with the cache outcome: `MISS` on the first request, `HIT` once the worker has written the optimized variant.'
  - q: 'Why is the first request a MISS and the second a HIT?'
    a: 'Optimization happens off the request path. The first request serves the original bytes, stores them in the cache and notifies the worker; the worker writes the optimized variant; the next request for the same URL and client class is served from the cache.'
  - q: 'How do I see a page without optimization for one request?'
    a: 'On the Apache, nginx and IIS modules, append `?PageSpeed=off` to the URL; `?ModPagespeed=off` is the older spelling and is accepted on every platform too. The values `on`, `off`, `unplugged` and `noscript` need no token. The Docker reverse proxy does not read these parameters.'
---

Request any HTML page twice with GET and read one header. Which header you
look for depends on how you run mod_pagespeed.

```bash
curl -s -o /dev/null -D - http://localhost/
curl -s -o /dev/null -D - http://localhost/
```

Use GET, not `curl -I`: the Docker reverse proxy does not cache HEAD requests,
so repeating a HEAD there never turns into a HIT.

## The header, per integration

| Integration                  | Header            | Value                                              | Means                                                       |
| ---------------------------- | ----------------- | -------------------------------------------------- | ----------------------------------------------------------- |
| Apache module                | `X-Mod-Pagespeed` | `on` (the version with `ExposeVersionInHeaders`)   | the module handled the response                             |
| nginx module (native)        | `X-Page-Speed`    | `on` (the version with `ExposeVersionInHeaders`)   | the module handled the response                             |
| IIS module                   | `X-Page-Speed`    | `on` (the version with `ExposeVersionInHeaders`)   | the module handled the response                             |
| Docker / nginx reverse proxy | `X-PageSpeed`     | `MISS`                                             | proxied to the origin, stored, the worker notified          |
|                              |                   | `HIT`                                              | served from the cache                                       |
|                              |                   | `STALE`, with `X-PageSpeed-Stale: if-error`        | the origin failed; a stale cached entry was served instead  |
|                              |                   | `REVALIDATED`, with `X-PageSpeed-Revalidation: 304` | the origin confirmed the cached entry; served from the cache |
|                              |                   | `async-css-loader`                                 | the small loader script the module serves itself            |
| ASP.NET Core middleware      | `X-PageSpeed`     | `MISS`, then `HIT`                                 | as the reverse proxy; the `/console/*` routes carry no header |

On the Apache, nginx and IIS modules the header appears on every response the
module handles, whether or not anything on that page was rewritten. Its
presence confirms the module is loaded and `pagespeed on` applies to that
site; the [admin console](#the-admin-console) tells you what it did.

From 2.7.0 the value is `on` by default: the header confirms the module is
active and does not disclose which version runs (2.6.0 and earlier send the
version). To send the version instead, set the server-wide
[`ExposeVersionInHeaders`](/docs/configuration/#expose-version-in-headers) option
(Apache: `ModPagespeedExposeVersionInHeaders on`; nginx:
`pagespeed ExposeVersionInHeaders on;`; IIS: `pagespeed ExposeVersionInHeaders on`,
without the semicolon, in the server-wide `pagespeed.config`). Add it in the same step as the upgrade,
never before: older releases reject the directive.
A custom [`XHeaderValue`](/docs/configuration/#xheadervalue) takes precedence
over both. The Docker reverse proxy and the ASP.NET Core middleware never send a
version in `X-PageSpeed`; they report only the cache outcome.

## First MISS, then HIT

The Docker reverse proxy and the ASP.NET Core middleware optimize off the
request path. The first request for a URL serves the original bytes, stores
them in the cache and notifies the worker; the worker builds the optimized
variants in the background; the next request for the same URL and client
class is served from the cache with `X-PageSpeed: HIT`. A second `MISS` a
moment later usually means the worker had not finished yet. Wait and retry
before you look for a fault.

To see an optimization rather than a cache outcome, ask for an image in a
format the browser would accept. The URL stays the same; the bytes and the
`Content-Type` change:

```bash
curl -s -o /dev/null -D - http://localhost/<your-image>.jpg -H 'Accept: image/jpeg'
# Content-Type: image/jpeg   — original
curl -s -o /dev/null -D - http://localhost/<your-image>.jpg -H 'Accept: image/webp'
# Content-Type: image/webp   — smaller, once the worker has processed it
```

On the native module with the optimizer worker attached, the admin console's
Statistics page shows the same thing as counters: `ipro_daemon_served` rises
as in-place requests are answered from the worker's cache, and
`ipro_daemon_fallthrough` counts the ones the ordinary path answered.

## Turn it off for one request

On the Apache, nginx and IIS modules, append `?PageSpeed=off` to any URL to
get the response as the origin serves it. `?ModPagespeed=off` is the older
spelling; both names are read on every platform. The values `on`, `off`,
`unplugged` and `noscript` are honoured without a token; any other query
option needs the `RequestOptionOverride` token set on the server. Compare the
two responses to see what the module changed on that page.

The Docker reverse proxy does not read these parameters. Compare an optimized
and an original response with the `Accept` header instead, as above.

## The admin console

The Apache, nginx and IIS modules serve an admin console at `/pagespeed_admin/`
(one virtual host) and `/pagespeed_global_admin/` (the whole server). Its
Overview page answers the three questions at a glance: is the module working,
is the optimizer worker healthy, and what have they saved. Both paths can
purge caches and show configuration, so the shipped examples allow localhost
only; see [Admin console setup](/docs/admin-console/#setup) before you widen
that.

## Worker health

The optimizer worker answers two health probes:

- **The health socket**, always on. It is the notification socket's path with
  `.health` appended: `/run/pagespeed-optimizer/notify.sock.health` for the
  packages, `/data/pagespeed.sock.health` in the container images. It answers
  one line starting with `OK`, or `DEGRADED` when the worker runs without its
  cache:

  ```bash
  socat -u -T 4 UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.health -
  # OK 5/128 notifs=1542 variants=986 proactive=724 errors=3 cache_entries=2048 inflight=3
  ```

- **`GET /v1/health`** on the management API, which is off until you enable a
  transport. On the packages, set `OPTIMIZER_OPTS=--api-socket` in
  `/etc/default/pagespeed-optimizer` and restart; the API then listens on
  `/run/pagespeed-optimizer/api.sock`, and `/v1/health` needs no token on any
  transport:

  ```bash
  curl --unix-socket /run/pagespeed-optimizer/api.sock http://localhost/v1/health
  ```

  The answer carries `status`, `ready`, a `checks` table, `version`,
  `uptime_seconds`, `connections`, `inflight`, `browser_sandbox` and
  `syscall_filter`. `ready: true` is the field to probe.

The worker also serves the [web console](/docs/workbench/) at `/console/` on
the same API. [Monitoring](/docs/monitoring/) covers the metrics behind these
probes and what to alert on.

## If something is off

| You see                                        | Start here                                                                                                      |
| ---------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| No header at all                               | [No X-Mod-Pagespeed or X-Page-Speed header](/docs/troubleshooting/#no-x-mod-pagespeed-or-x-page-speed-header) |
| `MISS` on every request                        | [Cache miss on every request](/docs/troubleshooting/#cache-miss-on-every-request)                             |
| `MISS` on every reload in Chrome               | [MISS on every reload in Chrome](/docs/troubleshooting/#x-pagespeed-miss-on-every-reload-in-chrome)           |
| The header is there, images stay unchanged     | [Worker not processing content](/docs/troubleshooting/#worker-not-processing-content)                         |
| WebP or AVIF never appears                     | [Images not converting to WebP/AVIF](/docs/troubleshooting/#images-not-converting-to-webpavif)                |
| The ASP.NET Core dashboard reads zero          | [ASP.NET Core dashboard shows zeros](/docs/troubleshooting/#aspnet-core-dashboard-shows-zeros)                |

The HTML attributes, `.pagespeed.` URLs and the beacon endpoint you may meet in
a page's source are explained on [PageSpeed markers](/pagespeed-markers/).
