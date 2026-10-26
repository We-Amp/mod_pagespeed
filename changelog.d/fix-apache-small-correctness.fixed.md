- **Apache: a proxy request that fails `ModPagespeedProxyAuth` is now
  rejected with a real 403.** With the directive set to a cookie name and
  no redirect, a client without the cookie was answered with `200` and an
  empty body, which downstream caches could store. The response now
  carries status 403.
- **Apache: the module's own 404 responses now use the site's
  `ErrorDocument`.** Unknown `.pagespeed.` resources and missing static
  assets were answered with Apache's built-in error page, ignoring an
  `ErrorDocument 404` configured for the site. Those requests now return
  `HTTP_NOT_FOUND`, so the error document applies; the
  `resource_404_count` and `slurp_404_count` statistics are still counted.
- **Apache: responses proxied with `ModPagespeedMapProxyDomain` no longer
  pass the origin's cookies to browsers.** The origin's `Set-Cookie` and
  other hop-by-hop headers were copied through to the client under the
  proxying site's own domain. They are now removed before the response
  headers are handed to Apache; on proxied HTML this also drops the
  module's own experiment-assignment cookie, which shares that response
  with the origin's headers.
- **Apache: `<Files>` and `<FilesMatch>` sections now apply to `.pagespeed.`
  URLs.** The module's map-to-storage hook ran only the directory walk, so
  per-file configuration was skipped when a resource was served through its
  rewritten URL. The file walk runs as well now, with the real resource
  name in place of the one-character stand-in the directory walk truncates
  it to.
- **Apache: no per-request memory growth on keep-alive loopback
  connections, and deprecated-filter warnings are logged once.** The
  loopback request rewrite allocated from the connection pool on every
  request, so long-lived keep-alive connections grew without bound with
  `MaxKeepAliveRequests 0`; the allocations now come from the request pool.
  A deprecated filter enabled in `.htaccess` also logged a warning on every
  request to that directory; the warning now fires once per filter name per
  configuration load, and once per process for per-request configuration
  such as `.htaccess` files and `PageSpeedFilters` query parameters.
