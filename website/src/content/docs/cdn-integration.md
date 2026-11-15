---
title: 'Integrate with a CDN'
description: 'Run mod_pagespeed behind Cloudflare, CloudFront or Fastly: how Vary headers affect edge caching, per-CDN cache-key settings and keeping hit rates high.'
order: 24
group: 'Configure'
lastUpdated: 2026-11-15
---

mod_pagespeed runs behind any CDN. The one thing to get right is how your CDN
treats the `Vary` headers mod_pagespeed emits — get it wrong and edge hit rates
collapse. This page explains that interaction and gives concrete settings for
Cloudflare, CloudFront, and Fastly.

## Vary: User-Agent

mod_pagespeed serves different optimized variants at the same URL based on client
capabilities (image format support, viewport class, pixel density, encoding).
To tell downstream caches about this, it emits Vary headers:

| Content type                                  | Vary header                                 |
| --------------------------------------------- | ------------------------------------------- |
| Images (optimizer's nginx module, containers) | `Accept, Save-Data, User-Agent, Sec-CH-DPR` |
| Images (ASP.NET Core middleware)              | `Accept, Save-Data, User-Agent`             |
| HTML                                          | `Accept-Encoding, User-Agent`               |
| CSS / JS                                      | `Accept-Encoding`                           |

From 2.5.2 the native Apache, nginx and IIS modules with the optimizer worker
attached send the same list on images the optimizer worker chose:
`Vary: Accept, Save-Data, User-Agent, Sec-CH-DPR`, on the 200 and on the 304
(with `Accept-Encoding` where compression applies). Up to and including 2.5.0
they named only `Accept`. A response that carries the origin's own bytes, such as the first
request for a URL, carries no `Vary: Accept` unless the origin sent one.

Internally, mod_pagespeed classifies User-Agent strings into a small number of
capability dimensions: 3 viewport classes (mobile, tablet, desktop) × 2 pixel
densities = 6 distinct variants. This makes the variation finite and cacheable.
(See [viewport-aware image optimization](/blog/viewport-aware-image-optimization/)
for how those viewport classes drive the image pipeline.)

However, most CDNs treat `Vary: User-Agent` as effectively uncacheable because
they see every unique User-Agent string as a different cache key — not the 6
capability classes that mod_pagespeed actually uses.

### Impact

- **Browser caching** works correctly regardless. The same browser always sends
  the same User-Agent, so resources stay cached within a session.
- **CDN edge caching** is impaired for images and HTML. The CDN stores many
  near-identical variants, reducing hit rates. A CDN that ignores
  `Vary: User-Agent` (Cloudflare does, by default) can do worse on the
  optimizer's nginx module: it caches the viewport-sized copy the first
  visitor got and serves it to every device class.
- **CSS/JS** is unaffected — it uses only `Vary: Accept-Encoding`, which CDNs
  handle well.

The [safe cache mode](/docs/cache-modes/) limits the cost of low CDN hit rates:
its short TTLs and ETag-based 304 revalidation keep the bandwidth overhead
small. See [cache-control headers](/docs/cache-control/) for the TTL and
revalidation directives behind this behavior.

## Image format and the Accept header {#image-format-negotiation}

An image keeps its URL; the format of the body is chosen per request from the
`Accept` header. For a `.jpg` or `.png` URL, once the optimizer has written the
copies:

| The request's `Accept`                                            | Body served                              |
| ----------------------------------------------------------------- | ---------------------------------------- |
| names `image/avif`                                                | AVIF                                     |
| names `image/webp`, or contains `image/*` or `*/*` (but not AVIF) | WebP                                     |
| is absent or empty                                                | WebP: treated exactly like `*/*`         |
| anything else, for example `image/jpeg` or `image/png` only       | the original format, optimized in place |

So a client that sends `Accept: */*`, `Accept: image/*` or no `Accept` header at
all receives WebP for a `.jpg` URL. That includes `curl`, `wget` and most HTTP
libraries, which send `Accept: */*` by default. This is deliberate: under RFC
9110 §12.5.1, `*/*` and `image/*` say the client takes any image type, and a
request with no `Accept` header means the same thing. Some browsers rely on it:
Chrome on iOS, for example, sends `Accept: */*` for images without naming WebP.
AVIF is served only when the request names `image/avif`; a wildcard never
selects it.

Two details of the matching: quality values are not weighed, so a listed type
counts even with `;q=0`, and a wildcard anywhere in the header wins over the
specific types next to it, so `Accept: image/jpeg, */*` still gets WebP. When no
copy in the negotiated format exists at the client's size class (not written
yet, or declined by the quality check), the request gets the original format
at that size; a client that names `image/avif` is never given WebP instead. On the native modules without the
optimizer worker (the module's own in-place path), none of this applies: an
image optimized in place is the same bytes for every client and carries no
`Vary: Accept`.

A response whose format was chosen this way carries `Vary: Accept` (see the
table above), so a cache that honours `Vary` keeps the formats apart. A CDN
that ignores `Vary: Accept` caches whichever format the first visitor got, and
can hand an AVIF or WebP body to a client that cannot decode it.

To cache images at the edge, key them on `Accept`, normalized with the same
mapping the optimizer uses, so the edge holds three copies per URL instead of
one per distinct `Accept` string:

- `avif` when `Accept` contains `image/avif`;
- `webp` when it contains `image/webp`, `image/*` or `*/*`, or is absent;
- `original` otherwise.

Forward the normalized value as the request's `Accept` (for example
`image/avif`, `image/webp` or `image/jpeg`), so the origin answers for the
class the edge cached it under. If you cannot do that, do not cache images at
the edge.

**The native modules name the full list from 2.5.2.** Behind the native
Apache, nginx or IIS module with the optimizer worker attached, an image
served at its original URL is chosen by the device class and Save-Data as
well as by `Accept`. These responses now carry
`Vary: Accept, Save-Data, User-Agent, Sec-CH-DPR` on Apache, nginx and IIS,
so a shared cache no longer hands one device's image to another; up to and
including 2.5.0 the response named only `Accept`. A cache that honours `Vary`
(Varnish, Fastly, nginx) now keeps more copies of each image, so its hit rate
for them drops; keying it on a device class wins that back. A cache that
ignores `Vary` by default (Cloudflare, Amazon CloudFront) still hands one
device's image to another until it is configured to key on the device class,
`Accept` and `Save-Data`, or to skip these images. After upgrading, purge the
images a shared cache holds once, since copies cached before the upgrade
still carry `Vary: Accept` alone.

If a non-browser client must receive the original format:

- **Send an `Accept` header that names only that format**, with no `*/*` or
  `image/*`, for example `Accept: image/jpeg`. This is the per-request switch;
  there is no setting that serves the original format to wildcard requests.
- **On the native Apache, nginx and IIS modules, append `?PageSpeed=off`** to
  the URL to get the response as the origin serves it. The Docker reverse proxy
  does not read this parameter.
- **Exclude the URLs from optimization** with
  [`pagespeed_disallow`](/docs/worker-configuration/#pagespeed_disallow) on the
  Docker reverse proxy, or [`Disallow`](/docs/configuration/#disallow) on the
  native modules. This applies to every client.
- **Turn off image transcoding** in the optimizer worker with
  [`--disable-image`](/docs/worker-configuration/#--disable-image). This also
  applies to every client and stops all image optimization.

## Per-CDN recommendations

Purge cached HTML in any CDN after upgrading. See
[Hydrated pages](/docs/hydrated-pages/).

### Cloudflare

Cloudflare does not key its cache on `Vary: Accept` or `Vary: User-Agent`
unless you configure it to: by default an image is cached once, for every
client. For images, choose one of:

- **The Vary setting in a Cache Rule** (all plans): give the `Accept` header
  the `normalize` action, and Cloudflare keeps a copy per normalized `Accept`
  value for responses that send `Vary: Accept`. Leave its media type list
  empty: narrowing it can put a request with no `Accept` header (served WebP)
  and one that names only `image/jpeg` (served the original) under one key.
- **Vary for Images** (Pro, Business and Enterprise plans, configured through
  the API only): declare that `.jpg`, `.jpeg` and `.png` have `image/avif` and
  `image/webp` variants, and Cloudflare caches each format separately.
  Requests with `Accept: */*` or no `Accept` header bypass that cache, because
  the origin answers them with WebP for a `.jpg` URL rather than the original
  type; they are served correctly, just not from the edge.
- **A custom cache key on the `Accept` header** (Enterprise plans).
- **Bypass the edge cache for images** with a Cache Rule.

See Cloudflare's [Vary](https://developers.cloudflare.com/cache/concepts/vary/)
and
[Vary for Images](https://developers.cloudflare.com/cache/advanced-configuration/vary-for-images/)
documentation for the current setup. For HTML, mod_pagespeed's safe mode
(short TTLs + `must-revalidate`) is a good fit.

### AWS CloudFront

CloudFront caches separate copies only for the request headers named in the
cache policy, and does not act on the origin's `Vary: Accept`. A header that is
not in the policy is not forwarded as sent: CloudFront removes `Accept`, and
replaces `User-Agent` with `Amazon CloudFront`. So with no `Accept` in the
cache policy, every client gets WebP (an absent `Accept` counts as `*/*`) and
none gets AVIF.

Add `Accept` to the cache policy for images, and normalize it first with a
CloudFront Function on the viewer request (the mapping above), so the cache
key holds three values instead of every browser's `Accept` string. Do not add
the `CloudFront-Is-*-Viewer` device headers for images: mod_pagespeed does not
read them, so they split the cache without changing the bytes.

**Recommendation:** Key images on the normalized `Accept` alone. Leave CSS/JS
caching as-is.

### Fastly

Fastly (Varnish-based) respects `Vary` faithfully, creating separate cache
entries per unique Vary combination. With `Vary: User-Agent`, this means one
variant per User-Agent string, and Fastly limits the number of variants per
object; its `vcl_hash` reference recommends moving the variation into the cache
key instead when there are many.

`Vary: Accept` is honoured the same way: every distinct `Accept` string gets
its own cache entry.

**Recommendation:** Use custom VCL to normalize `req.http.Accept` into the
three classes above (`image/avif`, `image/webp`, the original type) and
`User-Agent` into capability classes, and add both to the cache key in
`vcl_hash`, or bypass the cache for images. Do not strip `User-Agent` from
`Vary` and rely on mod_pagespeed's classification: the origin only classifies
on a cache miss, so Fastly would serve the first device class's copy to
everyone.

### Other CDNs / no CDN

If mod_pagespeed is your edge (no CDN in front), the Vary headers work correctly.
Downstream browser caches and ISP proxies handle `Vary: User-Agent` at the
per-client level, which produces correct behavior since each client has a stable
User-Agent.

## Behind a TLS terminator, load balancer or CDN {#behind-a-tls-terminator-load-balancer-or-cdn}

When a proxy in front of the server ends TLS, or a load balancer or CDN forwards requests to it over plain HTTP, the module sees an `http` request for a name that visitors know as `https`. Three things need attention: URLs that the module writes in absolute form come out with `http` (mixed-content warnings on an HTTPS page), the beacon is injected with its `http` address, and the CDN or load balancer has to route `.pagespeed.` requests to the right server.

**1. Tell the module the visitor's scheme.** The module takes the scheme of a request from the connection, unless [`RespectXForwardedProto`](/docs/configuration/#respectxforwardedproto) is on and the request carries an `X-Forwarded-Proto` header, in which case the scheme in that header wins. The option is off by default. It reads the header on Apache and nginx and accepts only `http` and `https`; any other value is ignored and a warning is logged. It changes the scheme only: the module reads no other `X-Forwarded-*` header, so `X-Forwarded-Host` and `X-Forwarded-Port` do not rename the host or the port. The proxy must therefore pass the public name in the `Host` header.

The proxy that terminates TLS sets the two headers (many load balancers and CDNs add `X-Forwarded-Proto` themselves; check what yours sends):

```nginx
# The nginx in front, which holds the certificate
location / {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-Proto $scheme;
}
```

and the server that runs the module trusts it:

```nginx
# nginx with the module
pagespeed RespectXForwardedProto on;
```

```apache
# Apache with the module
ModPagespeedRespectXForwardedProto on
```

Have the proxy overwrite the header rather than append to it, and do not expose the module's port to clients: with the option on, a client that can reach the module directly can choose the scheme of the URLs it generates. With the option on, the module also picks the `https` address of [`BeaconUrl`](/docs/configuration/#beaconurl) for pages it sees as HTTPS; the default is one relative path for both schemes, which needs no change. `BeaconUrl` accepts one address or two separated by a space (the second is the `https` one); a single absolute `http:` address gets the same address with `https:` for HTTPS pages.

**2. Check how the module fetches your resources.** The module downloads each CSS, JavaScript and image file it optimizes by its URL. For a host your configuration does not name, which is usually the case for the host a page was requested on, it sends that fetch to itself, on the port and scheme the request arrived on, with the public name kept in the `Host` header. The fetch therefore does not go back out through the load balancer, and needs no setup, as long as this server answers requests for that name on that port. A hostname you name in the configuration (with `Domain`, `MapOriginDomain` or `MapRewriteDomain`) is fetched by that name instead, through DNS and the network, so a CDN hostname is fetched from the CDN. Use [`MapOriginDomain`](/docs/domain-configuration/#maporigindomain) when the originals must come from somewhere other than that name, for example `pagespeed MapOriginDomain 127.0.0.1:8080 static.example.com;` (Apache: `ModPagespeedMapOriginDomain`) to fetch `static.example.com` from a local server. Rewritten URLs in the HTML still name `static.example.com`; only the fetch target changes. [`LoadFromFile`](/docs/configuration/#loadfromfile) reads files from disk and needs no fetch. For `https` origins see [HTTPS configuration](/docs/https-configuration/).

**3. Send `.pagespeed.` requests to the server that runs the module.** When you serve resources from a CDN hostname with [`MapRewriteDomain`](/docs/domain-configuration/#maprewritedomain), that hostname's origin must be the server that runs the module, because only the module can answer a `.pagespeed.` request. A CDN that pulls from a separate storage bucket gets a 404 for them (see [Why does a `.pagespeed.` URL return 404?](/docs/troubleshooting/#optimized-resources-return-404)). Authorize the CDN hostname with [`Domain`](/docs/configuration/#domain) so that the module may fetch and rewrite the resources named under it.

**Caching the HTML in front of the module.** The module sends HTML so that shared caches do not keep it. If a CDN or cache in front is meant to store rewritten HTML, read [Downstream caching with nginx proxy_cache and Varnish](/docs/downstream-caching/) first: it explains how to key the cache and how the module purges stale pages. For images behind a CDN, key on the normalized `Accept` header as described [above](#image-format-negotiation).
