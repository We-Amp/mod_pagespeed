# Changelog

All notable changes to mod_pagespeed are documented in this file.

## Unreleased

Changed: in the ASP.NET Core middleware (WeAmp.PageSpeed.AspNetCore), a response fetched for a visitor whose request carries cookies is shared with other visitors only when the origin marks it shareable. A page (any response that is not a stylesheet, script or image) needs `Cache-Control: public` or a single `s-maxage` above zero; `s-maxage=0`, a malformed value, a repeated `s-maxage` or `must-revalidate` alone does not count. A cached page is served to a visitor with cookies only with the same mark; otherwise the request goes to the application. The same applies to the page's Markdown copy for agents and its Early Hints. A stylesheet, script or image fetched with cookies is cached only when the origin states its lifetime (`max-age`, `s-maxage`, or `Expires` without a `Cache-Control` header); the middleware's default lifetime does not apply. An empty `Cookie` header counts as a cookie. Sites whose pages are requested with cookies (for example analytics or consent cookies) and are not marked `public` see more requests reach the application; add `public` to pages that are the same for every visitor to keep them cached. Requests with an `Authorization` header are handled as before.

Changed: the worker's `--help` output now lists the `--rsl-cap-*` options, which it already accepted.

- Fixed a shared-cache issue that could serve a page personalised for one visitor to another visitor. Update recommended.

Changed: in the nginx module, a response fetched for a visitor whose request carries cookies is shared with other visitors only when the origin marks it shareable. A page (any response that is not a stylesheet, script or image, including XHTML, JSON and plain text) needs `Cache-Control: public` or a single `s-maxage` above zero; `s-maxage=0`, a malformed value or a repeated `s-maxage` does not count. A cached page is served to a visitor with cookies only with the same mark; otherwise the request goes to the origin. The same applies to the page's Markdown copy, its Early Hints, and `/llms.txt`. A stylesheet, script or image fetched with cookies is cached only when the origin states its lifetime (`max-age`, `s-maxage` or `Expires`). Requests with an `Authorization` header are handled as before. The cache starts empty after the upgrade: entries stored by earlier versions are not used. See UPGRADING.md.

Changed: the JPEG quality of a desktop (full-size) image variant no longer depends on the smaller mobile and tablet variants built before it. Desktop and other full-size JPEG variants can come out larger or smaller than before. A normal-client WebP or AVIF variant is now always compared against the full-size JPEG, so it is no longer refused because of an earlier viewport.

## 2.8.1 - 2026-11-08

Changed: the Helm chart 0.3.15 is published with this release; its default
image version is 2.8.1.

Fixed: the cache detail view of the admin console and `GET /v1/cache/alternates` find entries stored per site. The endpoint takes the `site` listed by `GET /v1/cache/urls`. In 2.8.0 every such entry showed as not in the index. Affected: 2.8.0.

Fixed: pages built with Tailwind CSS v4 get inlined critical CSS and deferred stylesheets again. In 2.8.0 those pages kept render-blocking stylesheets and no inlined critical CSS. Affected: 2.8.0.

Known issue: `ps_css_validate` in the C API and `CssProcessor.Validate` in the ASP.NET Core package still reject Tailwind CSS v4 stylesheets, as in 2.8.0. The critical CSS fix above does not change them.

Fixed: pages whose stylesheet is larger than 64 KiB get critical CSS and deferred stylesheets again. In 2.8.0 those pages kept render-blocking stylesheets and no inlined critical CSS. Pages were still served correctly. Affected: 2.8.0.

## 2.8.0 - 2026-11-05

Changed: the Helm chart 0.3.14 is published with this release; its default
image version is 2.8.0.

Security: an optimized page could carry content from a stylesheet, image or other page that the server serves only to some clients. Page variants are now built only from content the site serves to any client. Affected: 2.7.0 and earlier. Update recommended.

Changed: before the optimizer first uses a cached stylesheet, image or page to build another page's optimized version or `/llms.txt`, it makes one request for it to confirm that the site serves it to any client, and confirms it again every few minutes. Content it cannot confirm is left out, and a stylesheet left out stays render-blocking. Pages served under a host name the web server has not confirmed for the site can lose more. In the nginx module, that includes every page of a catch-all block such as `server_name _;`: until the name is confirmed, those pages are optimized without other cached files, so they get no critical CSS from external stylesheets and no image dimensions, and their stylesheets keep their `@import` rules. To confirm a name, list it in the server block's `server_name` or in the optimizer's host alias file (`pagespeed-hosts.conf`). With the ASP.NET Core package, an app whose `AllowedHosts` is `*` (the project template default) or not set confirms no host name, but its pages still use a stylesheet or image the package cached for a visitor who was not signed in, so they keep these optimizations as long as those files are served to visitors who are not signed in; a stylesheet or image cached only for signed-in requests is still not used. Setting `AllowedHosts` to the site's host names confirms them. Stylesheets that mod_pagespeed passes to the optimizer keep their `@import` rules.

Security: with agent optimization on, pages that the server's address rules restrict to local clients could appear in the outputs for agents, in the nginx module and the ASP.NET Core package. Affected: 2.7.0 and earlier. Update recommended for sites with agent optimization on.

Changed: the optimizer's own agent fetches (for the markdown variant and `/llms.txt`) now appear as the client address `224.0.0.0` to access rules when they arrive from the server's own host: in the nginx module to `allow` and `deny`, `$remote_addr` and the access log, and with the ASP.NET Core package in `HttpContext.Connection.RemoteIpAddress` for middleware after `UsePageSpeed()` (or after `UsePageSpeedAgentFetchLabel()`, for middleware that must run earlier). Pages under a rule that admits only local clients are no longer included in the agent outputs. Behind a proxy, the ASP.NET Core package needs `UseForwardedHeaders()` before `UsePageSpeed()`.

Changed: image variants are now checked against the original image. Large noisy photos are no longer smoothed before encoding; their variants keep the original's detail and are larger than before, in some cases much larger, though still always smaller than the source. Some noisy photos no longer get an AVIF variant and are served the JPEG variant instead, and when no smaller variant meets the quality bar the original is served. The `--denoise-*` options and their config keys are deprecated and ignored, and the statistics field `content_analysis.denoised` and the metric `pagespeed_images_denoised_total` are deprecated and always 0.

Security: in the nginx module, a client that the server's access rules refuse could still receive responses served from the optimizer's cache. Affected: 2.7.0 and earlier. Update recommended.

Changed: in the nginx module, a response from the optimizer's cache is now served only after nginx's access checks (`allow` and `deny`, `auth_basic`, `auth_request` and other access modules) have admitted the request, whatever caching the origin allowed. The async-CSS loader script and the Web Bot Auth public counter now follow the same access rules; in a location that requires credentials, exempt their paths for clients that should reach them. As before, `limit_req` and `limit_conn` count cache hits.

Changed: when a page's stylesheet stays render-blocking, because the page has
not been confirmed for deferral, the browser sandbox is off, or async CSS is
turned off with `--no-async-css`, the worker no longer also inlines a copy of
the above-the-fold CSS next to it. Those pages are served smaller, and the
stylesheet link is unchanged; first paint still waits for the full
stylesheet, as before. A page whose stylesheet is applied by script, or
sits after the page content, keeps the inlined block. Pages confirmed for
deferral are unchanged, and a page that becomes confirmed gets the inlined
block and the deferred stylesheet together. If you set `--no-async-css`,
pages no longer get an inline above-the-fold CSS block while stylesheets stay
render-blocking, so check your pages look right; inlining without deferral is
no longer available.

Security: in the nginx module, some responses the origin marks as not storable could enter the shared cache and be served to other visitors. Affected: 2.7.0 and earlier. Update recommended.

Security: in the nginx module with agent optimization on, a client could make the optimizer fetch pages from hosts the site does not serve, including internal ones, and read parts of them back. Affected: 2.7.0 and earlier. Update recommended for sites with agent optimization on.

Changed: in the nginx module, agent optimization (the synthesized `/llms.txt` and the markdown variant for agents) now applies only to host names that the server block lists in `server_name` (exact, wildcard or regular-expression names) or that the optimizer's host alias file names. A listed name with a port other than the one nginx itself listens on counts as unlisted, unless the host alias file names that host and port; behind a port mapping or a proxy the port visitors use is not nginx's listen port, so list that host and port in the alias file to keep agent optimization. Under any other name, for example on a catch-all `server_name _;` block or when a site is reached by its address, `/llms.txt` comes from the origin, agents get the normal page, and while agent optimization is on HTML is cached but not optimized. List every name a site is reached by in its `server_name` to keep agent optimization for it.

Security: with the ASP.NET Core package and agent optimization on, a client could make the optimizer fetch from hosts the site does not serve, including internal ones. Affected: 2.7.0 and earlier. Update recommended for ASP.NET Core sites with agent optimization on.

Changed: with the ASP.NET Core package, agent optimization now applies only to host names listed in the application's `AllowedHosts`. When `AllowedHosts` is `*` or not set, agents get the normal page, and while agent optimization is on HTML is cached but not optimized; list the site's host names in `AllowedHosts` to keep both.

Security: the ASP.NET Core package no longer stores responses to signed-in requests in its cache, so they cannot be served to other clients. Affected: 2.7.0 and earlier. Update recommended.

Changed: with the ASP.NET Core package, a response is cached only when the request was anonymous: no `Authorization` or `Proxy-Authorization` header, no signed-in user, and no authorization requirement on the endpoint (endpoints that allow anonymous access are still cached). Pages and assets requested only by signed-in clients are no longer cached across clients; a page served to a signed-in client is still optimized for that response. Existing cache entries are rebuilt with the cache cold start of this version.

Security: reduced differences between the HTML parser and browsers in where some elements end. Affected: 2.7.0 and earlier. Update recommended.

Security: hardened HTML parsing so that inline SVG and MathML are read as browsers read them, and optimizations no longer treat their content as page styles or scripts. Affected: 2.7.0 and earlier. Update recommended.

Security: critical CSS is no longer written into a page when it contains text that a browser could read as markup. Affected: 2.7.0 and earlier. Update recommended.

Changed: the cache starts cold once, in every component and for every kind of stored entry, at the update. This version stores everything under new cache keys, so every page, stylesheet, script and image is fetched from the origin once more and optimized again on its first request, and Markdown versions for agents, Early Hints, browser analysis results and llms.txt are built again; entries stored by earlier versions are not used and stay in the cache file until they are evicted. Update the worker, the nginx module and the ASP.NET Core package together: while earlier and current versions share one cache, neither uses the other's entries, so responses are served from the origin or without optimization until every server runs this version. No purge is needed when updating; before a rollback, see the Upgrade entry below. The configuration files beside the cache file are kept.

Upgrade: no cache purge is needed when updating the optimizer; the update starts its cache cold. Before downgrading the optimizer to an earlier release, purge its cache fully through the cache API (`POST /v1/cache/purge` with `{"scope": "all", "confirm": "purge-all"}`), or stop the worker and remove its cache volume, because an older optimizer would read entries stored before the update.

Changed: a page whose critical CSS contains text that a browser could read as markup is served without the inline critical CSS, and its stylesheets stay render-blocking. Range media queries are not affected when a space follows each `<`.

Changed: the optimizer now keeps the work of each site that records into it apart. A notification can name the site it belongs to, and everything the optimizer stores, reads, deduplicates or purges for it stays within that site. Notifications without a site behave as before. `POST /v1/cache/purge`, `/v1/cache/reprocess`, `/v1/cache/content` and `/v1/cache/urls` take an optional `site`; a purge without one covers every site the optimizer has stored work for, and its response lists the count per site. The optimizer remembers those sites in `pagespeed-sites.conf` beside `pagespeed-hosts.conf`.

Changed: agent optimization (the markdown variant for agents and the synthesized `/llms.txt`) now runs only for notifications whose sender has checked the host name against the names the site serves: the nginx module checks `server_name` and the host alias file, and the ASP.NET Core package checks `AllowedHosts`, as described above. For a notification without that check, for example a reprocess requested through the API, the optimizer builds no markdown variant and no `/llms.txt`, and still optimizes the page itself.

Upgrade: the worker notification protocol is now version 6. Reload nginx, Apache or IIS after upgrading the optimizer. Until the web server reloads, its notifications are refused and counted, and nothing new is optimized; serving is not affected.

Added: the C API (PS_API 1.12) gains site identity: `PS_SITE_ID_CHARS`, the `PS_NOTIFY_FLAG_HOST_VOUCHED` and `PS_NOTIFY_FLAG_FROM_MODULE` flags, the `site` and `notify_flags` fields of `ps_notify_params_t`, and `ps_cache_read_best_site`, `ps_cache_read_alternate_site` and `ps_cache_write_original_site`. A caller of `ps_notify_worker_ex` built against an earlier header is still served, without a site, and the library logs that once per process.

Changed: CSS minification keeps `/*! … */` comments and comments containing `@license` or `@preserve`.

Fixed: plain http ASP.NET Core sites now get critical CSS from their external stylesheets. For C API embedders, `ps_html_config_t` gains a `scheme` field (PS_API 1.12): initialize with `ps_html_config_init_auto` and set it to the page's scheme; a config from `ps_html_config_init` keeps reading under `https`.

Fixed: print stylesheets no longer change how the page looks on screen when critical CSS is on. Rules from a stylesheet or `<style>` element with `media="print"` could be inlined as screen rules, so a print layout that hides the header and footer also hid them on screen. The inlined CSS now keeps each stylesheet's media. Affects the ASP.NET Core package and, for pages that get inlined critical CSS, the worker.

Fixed: ASP.NET Core: a separately run optimizer worker configured with `Worker.SocketPath` and `AutoStart` off now receives notifications from the middleware. Before, the middleware dropped every notification in this setup, so the worker built no optimized variants (images, critical CSS) for the site.

Fixed: a visitor whose browser did not ask for reduced data no longer receives the reduced-quality Save-Data image when the normal image for that screen is not ready yet; it gets a normal image instead. This is a fix to how existing variants are chosen. It adds no new variant.

Changed: lazy-loaded images now also get `decoding="async"`, unless the page already sets a decoding attribute; the LCP candidate image and the first iframe are left as they are.

Changed: fetches for agent-ready copies ask the origin for a compressed response, using less origin bandwidth; requires a curl build with compression support, as standard builds have.

Added: the admin console now shows how much of the saving on responses served from the optimizer's cache came from compression applied by mod_pagespeed and how much from its optimization; your web server's own compression on other responses is not counted. The two parts, plus a share not yet attributed, add up to the total saving. The total now counts HTML that the optimizer makes larger, for example by inlining above-the-fold CSS or adding preload hints; earlier versions counted that as zero, so the total can read lower than before without anything optimizing less. After the update the caches start cold, so the split fills in as pages are re-optimized; until then the console shows the total only.

Fixed: the JavaScript minifier now leaves some scripts in legacy multibyte encodings unminified, keeping the original code.

Changed: XHTML pages (served as application/xhtml+xml) are served without the inline critical CSS block, and their stylesheets stay render-blocking.

## 2.7.0 - 2026-10-31

Changed: the Helm chart 0.3.13 is published with this release; its default
image version is 2.7.0.

Fixed: in the nginx module, HTTP/1.1 requests over TLS were cached under the `http` scheme, so they shared entries with plain-HTTP requests for the same host and URL and did not share them with HTTP/2 and HTTP/3 requests. The scheme now comes from the connection's TLS state for every protocol, so requests over TLS use `https` and plain HTTP uses `http`. Entries cached by an earlier version for HTTP/1.1 over TLS are fetched again from the origin. `pagespeed_trust_x_forwarded_proto` behaves as before.

Changed: the worker now logs a warning when the deprecated `quality_cap_margin` setting is set in the configuration file or through a configuration update; the setting has no effect. Use `no_quality_cap` to turn the quality cap off.

Fixed: on Windows, a cache reset no longer fails when the web server module or the ASP.NET Core package is reading the cache's generation file at that moment.

Fixed: with the ASP.NET Core package, the `/console/` admin UI can now reach the optimizer worker from host-network containers on some container runtimes, where it previously failed with a bad gateway error.

Fixed: on Windows the cache now works when the cache directory path contains non-ASCII characters (the Cyclone cache library was updated).

Fixed: Early Hints (`103`) are now sent over HTTP/2 and HTTP/3, not only HTTP/1.1, when the module is built against nginx 1.29.0 or newer, so browsers on typical TLS deployments can start preloading stylesheets while the page is still being prepared. Early Hints are also sent for clean URLs without a file extension (such as `/about`) when the client asks for HTML; previously only URLs ending in `.html` or `/` got them.

Fixed: over HTTP/3 the nginx module looked up cached pages under an empty host name, so HTTP/3 requests never found the entries stored for their site. They now use the request's host, as HTTP/1.1 and HTTP/2 do.

Fixed: critical CSS now keeps the above-the-fold rules of minified `@supports(...)` blocks written with no space before the parenthesis; previously those blocks were left out.

Fixed: JavaScript minification now keeps licence comments (`/*! ... */`, `@license` and `@preserve`) in the output instead of dropping them, so the licence notices of bundled libraries survive.

Fixed: JavaScript minification now handles ES modules that use `let` destructuring in `for` loops (such as `for (let {a, b} of items)`); previously such scripts were left unminified.

Fixed: the EXIF orientation reader for JPEG images tightens its bounds handling of malformed metadata.

Fixed: with the ASP.NET Core package, the ETag on a response served from the cache is now derived from the response body. Previously it was built from the insertion time and the length, so two different bodies of the same length cached in the same second shared an ETag and a client could keep stale content. Identical content keeps the same ETag, so conditional requests still get `304 Not Modified`. Clients revalidate once after upgrading.

Security: with the ASP.NET Core package, headers the application sets (such as
`Content-Security-Policy`, `Access-Control-Allow-Origin` and
`X-Content-Type-Options`) could be missing from responses served from the
cache. Cached responses now carry them, and a response with a header that
cannot be replayed from the cache is no longer cached. Update recommended.
Entries cached by an earlier version are fetched again from the origin.

Changed: the ASP.NET Core package and the optimizer worker must be upgraded
together. A worker older than 2.6.0 does not carry the new cache entry
information across the variants it writes, so the package fetches those
variants again from the origin instead of serving them from the cache.

Fixed: when the C library could not open the cache (`ps_cache_open`), `ps_last_error_message()` said only "Failed to create cache". It now includes the cache's own reason, for example a refused volume file or a path that cannot be created, so applications and the web server modules can log why the cache did not open. The C interface is unchanged.

Fixed: images from an origin that sends neither `ETag` nor `Last-Modified` were served without an `ETag` when the optimizer left them as they were, because they were already optimal (common for PNG and JPEG), or because their type is not optimized and its compressed copies were already stored, so clients could not revalidate them and always downloaded the full image. These images now carry an `ETag` and answer a matching `If-None-Match` with `304 Not Modified`.

Fixed: the script-coverage analysis now takes coverage only for scripts the analysis browser actually received. A script that could not be loaded is reported as having no coverage data, so it no longer gets a coverage figure and a deferral recommendation it did not earn.

Fixed: with the nginx module, when the cache volume failed to open because another process (such as the optimizer worker) was holding it, the module deleted the volume and created a new one. The other process kept writing to the deleted file, so the two sides stopped sharing cached entries until that process restarted. The module now deletes and recreates the volume only when it is damaged. In any other case it leaves the volume in place, logs the reason and serves without the cache until the next successful open.

Fixed: the ASP.NET Core package's worker host now logs when the optimizer worker it started exits on its own. A non-zero exit is logged as an error with the exit code, how long the worker ran, and the last lines of its error output; a clean exit is logged as a warning. A normal shutdown, including a worker that exits just ahead of the host, logs one information line. Before this, a worker that failed at startup (for example because its API port was already in use) left nothing in the application log.

Fixed: WebP and PNG images that carry an EXIF orientation (a photo taken with the camera turned) were optimized sideways, because only JPEG orientation was honoured. The orientation in the WebP `EXIF` chunk and the PNG `eXIf` chunk is now applied, so converted and resized variants display upright, and the width and height the optimizer adds to images match the upright image. A PNG with an orientation tag that would be optimized in its own format is served as the origin sent it, so its tag is kept.

Changed: the ASP.NET Core NuGet packages now carry the repository URL and commit and embed Source Link information, so a debugger can step into the middleware source and nuget.org links to the repository.

## 2.6.0 - 2026-10-28

Fixed: pages from an origin that sends neither `ETag` nor `Last-Modified` were served without an `ETag` when the optimizer left the page, stylesheet or script unchanged, so clients could not revalidate them and always downloaded the full page. These resources carry an `ETag` again and answer a matching `If-None-Match` with `304 Not Modified`.

Fixed: the optimizer worker's `--help` text said `--agent-optimize-llms-txt` implies `--agent-optimize`. It does not: `/llms.txt` is published only when both flags are set, so the help now says the flag requires `--agent-optimize`.

Fixed: on Windows, a full cache purge (`POST /v1/cache/purge` with
`"scope": "all"`) could report success without removing anything while IIS or
an ASP.NET Core application had the cache open. A full purge now replaces the
volume, as it does on Linux, by moving the held file aside, so the empty cache
is in place at once and the web server picks it up within about a second. If an
older web server module or ASP.NET Core package still holds the volume without
allowing the move, the purge reports that instead of reporting success; the
cache keeps serving and its generation is not advanced. Update or recycle that
application, then purge again. A failed full purge also no longer risks
stopping the optimizer.

Fixed: when the browser's renderer crashed during a screenshot or waterfall
capture, the capture endpoints answered 504 Gateway Timeout straight away, as if
the page had timed out. A crashed renderer is now reported as 502 Bad Gateway
(`BAD_GATEWAY`) with a message naming the crashed target; 504 is kept for
captures that really time out.

Fixed: with the nginx module, a compressed (gzip or Brotli) optimized response
served right after the origin confirmed it unchanged (a 304 cache revalidation,
visible as `X-PageSpeed: REVALIDATED`) was sent without its `Content-Encoding`
header, so browsers and other clients received compressed bytes labelled as
plain content. The response now carries the same `Content-Encoding` as a normal
cache hit.

Fixed: the default worker thread count now follows the container's CPU limit.
Before, a container limited with `docker run --cpus=12` (or a Kubernetes CPU
limit) on a large host still started a thread pool sized for the whole host.
The default is now the smallest of the CPUs the worker may run on and the
container's CPU limit, between 2 and 128; an explicit `--num-threads` still
wins.

Fixed: the optimizer worker's recovery for a cache volume that failed to open
deleted the configured cache path, which is not where the volume file lives:
the real file has the cache format and geometry in its name, so the deletion
removed nothing and the retry reopened the same damaged volume. The site then
ran unoptimized until the file was removed by hand, on every restart. The
recovery now removes the real volume files for a damaged volume. For a
permission, symlink or I/O error, or a volume another running process still
holds, nothing is deleted: the reason is logged and the worker starts in
degraded mode. For a held volume, restart the web server so the cache can be
reset.

Fixed: request headers the nginx module adds for conditional revalidation now
carry a lowercase key and hash like every other request header, so later
request processing handles them correctly.
Fixed: on Windows, the client library's notification sender could not reach an
optimizer worker running under a different account than the caller, for example
an IIS application pool identity with the worker under a service account. It
asked for more access to the worker's pipe than the worker grants other
accounts, and the connection failed with access denied. It now asks only for
read and write access. The ASP.NET Core middleware's notification client
had the same problem under an IIS application pool identity and is fixed the
same way.

Added: releases of the host packages now include a software bill of materials (`pagespeed-optimizer-<version>.spdx.json`, SPDX 2.3) and a VEX file (`pagespeed-optimizer-<version>.vex.json`, OpenVEX) that lists known vulnerabilities in bundled components that do not affect these packages, with the reason for each. Both files are listed in `SHA256SUMS`. Each package also has a build provenance attestation and an SBOM attestation, recorded in the public Sigstore transparency log, and `SHA256SUMS` now has a Sigstore signature, `SHA256SUMS.sigstore.json`, next to the existing PGP signature. To check a package, use the `gh attestation verify` and `cosign verify-blob` commands in RELEASING.md. The existing check with `gpg` and `sha256sum` works as before. See RELEASING.md for all verification commands.

Fixed: with the nginx module and proactive image variants turned on
(`PAGESPEED_PROACTIVE_IMAGE_VARIANTS=true` in the container, or the worker flag
`--proactive-image-variants`), browsers could be served the original upload,
unoptimized and in its original format: desktop browsers for images up to 768
pixels wide, whether they accepted WebP, AVIF or only JPEG, and tablet browsers
that accept only JPEG (or the image's own format) for images up to 480 pixels
wide. This started in 2.5.1. The optimizer skipped the desktop and tablet
copies of such an image because they were identical to the copies for a
smaller screen, and since 2.5.1 a browser is served an image for its own
screen size rather than another one. Those copies are now stored too. After the upgrade an affected image is rebuilt the next time a
desktop browser requests it; no purge is needed. Sites without proactive image
variants, the default, were not affected.

Fixed: an optimized script, stylesheet, image or page whose origin sends
neither `ETag` nor `Last-Modified` could be answered `304 Not Modified` after
it changed, when the new optimized version had the same size. Browsers then
kept the old version. Optimized responses now carry a validator derived from
the source content. When no content identity is available, no `ETag` is sent,
so a stale copy can never be confirmed. After the upgrade, clients may
download some optimized resources once more, because their validators change;
resources without any validator are revalidated with a full response until
the optimizer has processed them again.

Fixed: with gzip compression turned off, the optimizer no longer recompresses
and rewrites the stored copy of an image type it cannot optimize (such as an
icon) after the optimizer restarts; the same holds with brotli turned off.

Changed: when a WebP or AVIF image comes out larger than the optimized JPEG (or
other original-format image) of the same size, the optimizer now tries up to
two smaller encodes for WebP, one for AVIF, at the same visual quality target
before falling back to the original format. More browsers that accept WebP or
AVIF get the newer format, and never more bytes than the original-format image
they would otherwise get. The extra work happens only for those images. The
recovery applies when an image is optimized afresh, not to an image whose
original-format copy is already cached, until the cached copy expires or is
purged. `/v1/stats`
(and the Prometheus metrics) now report how many WebP and AVIF images were not
smaller than the original-format image (`image_size_refused`) and how many of
those a smaller encode recovered (`image_budget_rescued`).

Fixed: with the nginx module, a response revalidated with the origin carried
the origin's `ETag`, while a cache hit for the same bytes carried the module's
own, so a browser holding the cached copy got a full response each time the
entry was revalidated. Both now carry the same `ETag`, and the browser gets a
`304 Not Modified`.

Fixed: configuration updates through the API (`PATCH /v1/config`) no longer
pause the worker while the new settings are saved to disk. On a slow or busy
disk, health checks and other requests are now answered without waiting for the
save, and the update still returns only once the change is saved. If saving
fails, for example because the disk is full, the response now carries a
warning: the change stays in force until the next restart. Several rapid
updates are saved together.

Changed: the optimizer worker's log lines and `--version` output, and the nginx
module's load notice, now name the product mod_pagespeed. The version number and
build commit are unchanged.

Fixed: when an origin that was already cached and optimized starts sending
`Vary: Accept` for a URL, the optimized copies stored before the change were
kept and kept being served until a purge. They are now dropped at the next
origin refresh, and the URL is served as the origin sends it.
Fixed: C API: `ps_notify_worker` now reports an invalid-argument error for a
notification that cannot be serialized instead of success. This includes a
scheme other than "http" or "https", which `ps_notify_worker` now rejects up
front exactly like `ps_notify_worker_ex` always did; previously such a call
opened a connection, sent nothing, and returned success. Embedders of the
public C API are affected; the nginx module does not use this entry point and
is not affected.

Changed: the Helm chart 0.3.12 is published with this release; its default
image version is 2.6.0.

## 2.5.2 - 2026-10-21

Security: the bundled libpng is updated to 1.6.58, which fixes memory-safety
issues in PNG decoding, one of them reachable on arm64 when an origin PNG with
a palette is optimized (CVE-2026-33636, CVE-2026-33416, CVE-2026-34757).
**Update recommended.**

Changed: the Helm chart 0.3.11, whose changes are listed under 2.5.1, is
published with this release; its default image version is 2.5.2.

## 2.5.1 - 2026-10-21

Security: the nginx bundled in the container images (`pagespeed-nginx` and
the combined image) is updated to 1.30.6, which fixes two memory-safety issues
in nginx (CVE-2026-15176, CVE-2026-93479). This also carries the earlier
1.30.5 security update (CVE-2026-90439), which the images had not yet picked
up. Update recommended.

Changed: the Helm chart (0.3.11) defaults `podSecurityContext.fsGroup: 918`,
the worker's `pagespeed` uid/gid, so the shared cache volume arrives
group-owned before any container starts, which is what the worker needs
when it runs without its root phase. Where the worker entrypoint still runs its root phase it
re-establishes the same 918:918 ownership, so behaviour there is unchanged.
The chart also stops shipping an nginx ConfigMap nothing read: the nginx
image renders its configuration from a template baked into the image plus
environment variables, and the ConfigMap carried a directive the module no
longer accepts, so it never applied and wiring it in would fail nginx's
config check. Installing 0.3.11 renders one fewer object and adds the fsGroup
field; nothing else changes. 2.5.1 shipped no chart: chart 0.3.11 is
published with 2.5.2, with default image version 2.5.2. Chart repository index updates are now
append-only, so publishing a chart version no longer rewrites the `created`
dates, digests or metadata of the versions already published.

Fixed: installing the nginx or Apache module package before
`pagespeed-optimizer`, or both in one transaction, left the web server's user
out of group `pagespeed`, so the web server could not reach the worker. The
`pagespeed-optimizer` package now adds each existing web-server user
(`www-data`, `nginx`, `apache`) to the group when it installs or upgrades, in
any install order. When it adds a user it says so: restart the web server
once to pick up the new group. Upgrading from 2.5.0 repairs an install that
was affected.

Fixed: in the ASP.NET Core middleware (`WeAmp.PageSpeed.AspNetCore`) on Linux
and macOS, a full cache purge (`POST /v1/cache/purge` with `"scope": "all"`,
the console's purge everything) now takes effect within about a second. The
middleware kept using the cache volume the purge had deleted and went on
answering `X-PageSpeed: HIT` with pre-purge content until the application
restarted. It now notices the worker's volume reset and reopens the cache, as
the nginx module does; responses already being sent from the old volume
complete normally. If the cache cannot be reopened, the middleware logs a
warning and keeps serving the previous volume, pre-purge entries included,
retrying every second until the reopen succeeds (the nginx module serves
uncached instead). On Windows a full purge may still not take effect while the
application is running.

Fixed: a phone whose browser accepts WebP but not AVIF could be served the
full-size desktop image instead of the phone-size one, several times larger.
Image selection now prefers an image at the client's size over one in its
preferred format at another size, so phones get a phone-size image and
desktops a desktop-size one. After the upgrade the change applies to cached
images as soon as they are served again.

Fixed: with `Save-Data: on`, a full-size JPEG could be larger than the one sent
without it, so turning Save-Data on cost more data. The Save-Data JPEG is now
encoded at the Save-Data quality, and the optimizer no longer stores any
Save-Data image that is not smaller than the normal one for the same client.

Fixed: WebP and AVIF images were stored and served even when they were larger
than the optimized JPEG (or other original-format image) of the same size, so
a browser that accepts the newer formats could download several times more
than one that does not. Such a WebP or AVIF image is now discarded and those
browsers get the smaller image.

Fixed: with the nginx module, desktop browsers that accept only JPEG (or
another original format) were served the original upload unoptimized, because
the optimizer never replaced the copy the module stores for them. The optimizer
now builds the optimized image for those browsers too, and keeps the upload as
the reference it rebuilds variants from.

Upgrading: image variants stored by 2.5.0 and earlier, including Save-Data
JPEGs larger than the normal ones and WebP or AVIF images larger than the
JPEG, keep being served until their cache entry is refreshed or purged. Purge
the cache once after upgrading: in the console use purge everything, or send
`POST /v1/cache/purge` with `{"scope": "all"}` to the management API. Variants
are rebuilt as images are requested again.

Added: the GitHub releases of the host packages (`optimizer-v*`) and of the
NuGet packages (`v*`) carry `SHA256SUMS.asc`, a detached signature of
`SHA256SUMS` made with the We-Amp release signing key (fingerprint
`411D EFDB 5713 51D2 B25A EFCC 9C02 B5BD CCE7 5313`), the key that already signs
the module releases. Its public half is `RELEASE-SIGNING-KEY.asc` in this
repository. Releases up to and including 2.5.0 have no signature.

## 2.5.0 - 2026-10-19

Changed: one version number. The host packages move from 1.19.0 to 2.5.0
and the product from 2.4.0 to 2.5.0; from this release on `pagespeed-optimizer`,
the serving-module packages, the container images, the NuGet packages
(`WeAmp.PageSpeed`, `WeAmp.PageSpeed.AspNetCore` and the
`WeAmp.PageSpeed.NativeAssets.*` packages), the Helm chart's application
version and the version header all carry 2.5.0. A package pin, image tag,
NuGet version range or header match on `1.` needs updating before the
upgrade. The C API, the configuration and the module-to-worker handshake are
unchanged; your configuration carries over as it is.

Security: disk cache: damaged or stale cache contents could be served, and a
damaged cache file could terminate the process. Affected: the optimizer worker,
the nginx module and the `WeAmp.PageSpeed.NativeAssets.*` NuGet packages, up to
and including 2.4.0. Update recommended.

Security: specially crafted GIF, PNG, WebP or JPEG images could make the
optimizer worker exit or consume unbounded memory. Oversized and malformed
images are now rejected up front and a failed conversion keeps the original
image. Affected: the optimizer worker and the `WeAmp.PageSpeed.NativeAssets.*`
NuGet packages up to and including 2.4.0. Update recommended.

Security: crafted JavaScript or HTML handled by the optimizer worker could end a
script block early or produce malformed attribute output. The JavaScript and
HTML parsing kernels are now synced from the module repository and carry its
fixes. Affected: the optimizer worker and the `WeAmp.PageSpeed.NativeAssets.*`
NuGet packages up to and including 2.4.0. Update recommended.

Changed: the disk cache moves to on-disk format v9 of the Cyclone cache library
in the optimizer worker, the nginx module and the `WeAmp.PageSpeed.NativeAssets.*`
NuGet packages. The cache starts cold on the first start after the upgrade: it
opens a new file and the previous file stays on disk until you delete it. The
optimizer's cache directory generation moves from 2 to 3: the default cache
directory is now `/var/cache/pagespeed-optimizer/v3` (2.4.0 used `.../v2`), and
a web-server configuration that names `.../v2/cache` explicitly must be changed
to `.../v3/cache`; a module and worker on different generations refuse to
attach and say so in the log. During a rolling restart the old and new versions
use separate files, so disk use temporarily doubles and the hit rate stays low
until every process runs the new version. Prefer an off-peak window for the
upgrade.

Security, known issue: the ASP.NET Core middleware (`WeAmp.PageSpeed.AspNetCore`)
serves its web console at `/console/` and proxies the worker API (`/v1/health`,
`/v1/ws`, `/v1/cache`, `/v1/stats`, `/v1/capture`, `/v1/config`) from your
site's root, in every environment, with no authentication, so anyone who can
reach the site can read and change the live optimizer configuration and purge
the cache. Affected: 2.0.13 and later, including this release; 2.0.4 through
2.0.12 served the console and a smaller set of proxied paths, also without
authentication. Set `PageSpeed:Console:Enabled` to `false` in production
(recommended), or put your own authentication in front of the console path and
those six `/v1/*` paths, immediately before `UsePageSpeed()`. A configuration
changed through the open console is saved to `pagespeed.json` in the directory
of `Cache.VolumePath` and stays in force after either fix, as do the variants
built under it: stop the app, delete `pagespeed.json` and the cache volume
files (`volume*` for the default `volume.dat`) in that directory, and start it
again. `Worker.ApiPort` `0` does not disable the worker API: it picks a free
loopback port, and the console proxy reaches it either way. The production
guidance in the documentation shows how to check your own site from outside.

Changed: the nginx module now also caches responses that carry static policy
and resource headers, and replays them on cache hits: HTTP Strict Transport
Security, the frame and XSS protection headers, the reporting policy headers,
`Link`, static CORS headers, `Content-Language`, `Content-Disposition`,
`Alt-Svc`, `Allow`, `X-Powered-By` and `X-Robots-Tag`. A `Link` header that
carries a nonce is never cached. A header whose value changes per request, such
as a request id or `Set-Cookie`, still passes the response through uncached,
and the error-log notice still names the header. A `Link` preload the origin
sent is emitted once on a cache hit, even when the module adds its own preload
hints.

## 2.4.0 — 2026-10-14

This release is a security and reliability update for the nginx module in
the container images and Helm chart: cache hits now carry the origin's
security and CORS headers, including `Content-Security-Policy`. That module
also stops stale serves of responses the origin marked `must-revalidate`,
`proxy-revalidate` or `s-maxage`, and serves the full stored response, not a
bare `304`, to a client that sent no conditional request after the origin
confirms a stale copy. Update recommended; see Changed below before
upgrading.

Fixed: when the origin confirms a stale cached response with a 304 that makes
it no longer cacheable (for example a `Vary: *` line), nginx now answers a
client that sent no conditional request with a normal `200` and the full body,
once, and then drops the cached copy. Previously such a client could receive a
bodyless `304 Not Modified` it had not asked for. A client whose own
`If-None-Match` matches still receives a `304`.

Fixed: nginx now serves a stale cached response that the origin confirms is
still current (a 304 to the module's own revalidation request) to a client
that sent no conditional request as a normal `200` with the full body, as RFC
9111 section 4.3.4 requires. Previously such a client could receive a bodyless
`304 Not Modified` it had not asked for. A client whose own `If-None-Match`
matches the response's ETag still receives a `304`. Separately, a stale
response served because the origin returned a 5xx error now carries the
status line `HTTP/1.1 200 OK`; it used to carry the origin's error status line
together with the cached body.

Fixed: responses served from the nginx cache now carry the
`Content-Security-Policy`, `Access-Control-Allow-Origin`,
`X-Content-Type-Options`, `Referrer-Policy`, `Cross-Origin-*`,
`Permissions-Policy` and `Vary` headers the origin sent with them. This holds
for fresh cache hits, for stale copies served while a refresh is in flight
or while the origin returns an error, and for copies revalidated with the
origin. Previously these headers were missing on cache hits. Responses
cached by an earlier release regain the headers as each cached URL next
expires: at that point the module fetches the URL from the origin in full
once instead of revalidating it, so expect one full origin fetch per cached
URL in the first cache lifetime after upgrading. A re-recorded URL gets a
new validator, so clients miss one conditional request for it (they receive
the full response once instead of a 304). Responses the origin marks
`no-cache` keep being revalidated and regain the headers when their content
changes. To restore the headers on every cached URL at once, purge the cache
after upgrading. If you added any of these headers on the nginx server with
`add_header` to work around them missing on cache hits, remove that
workaround, or clients will receive them twice. When the stored header lines
of a cached URL cannot be read, the request is fetched from the origin and
the lines are stored again.

Fixed: nginx no longer serves a stale cached response whose origin sent
`must-revalidate`, `proxy-revalidate` or `s-maxage` while a refresh is in
flight or when the origin returns an error; those requests go to the origin
instead (RFC 9111 section 5.2.2). Sites that relied on stale serves of such
responses will see a few more origin fetches around expiry.

Changed: the nginx module no longer caches a response whose headers a cache
hit could not reproduce. A response that carries a header outside the set a
cached copy can carry (for example `X-Powered-By`, `X-Frame-Options` or a
custom `X-` header) is now passed through from the origin on every request
instead of being cached without that header. Responses whose
`Content-Security-Policy` contains a per-response nonce are never cached.
Responses that carry only `Content-Security-Policy`,
`Access-Control-Allow-Origin`, `X-Content-Type-Options`, `Referrer-Policy`,
`Cross-Origin-*`, `Permissions-Policy` and `Vary` beside the usual caching
headers are cached as before, and the module now stores those header lines
with the cached copy (see the next entry). To keep caching a
response that carries another header, remove it with `proxy_hide_header`,
or set it on the nginx server with `add_header`, which nginx applies to
cache hits as well. Six new counters on `/v1/metrics` show the effect:
`pagespeed_headers_sidecar_fall_through_total` (responses not cached because
of such a header), `pagespeed_headers_sidecar_never_optimized_total`
(nonce-bearing policies), `pagespeed_headers_sidecar_stored_total` (header
blocks stored with a cached copy), `pagespeed_headers_sidecar_write_failed_total`,
`pagespeed_headers_sidecar_replayed_total` (cache hits that sent stored
header lines) and `pagespeed_headers_sidecar_missing_total` (requests
fetched from the origin because the stored lines could not be read). The
serve statistics file changes format and is recreated once on upgrade, so
these and the other serve counters start from zero. Upgrade the nginx
module and the optimizer together: an upgraded module does not open the
statistics file of an optimizer that has not been upgraded yet, and records
no serve statistics (caching and serving are unaffected) until the optimizer
is upgraded too; an older module keeps writing to the replaced file until
nginx is reloaded.

Fixed: when the optimizer worker improves HTML served through the nginx
module in the container images and Helm chart, it now also honours a
`Content-Security-Policy` the origin sent as a response header, not only one
in a `<meta>` tag. An inline element is added only when every policy present
allows it. Pages recorded without stored header lines behave as before.

Fixed: the optimizer worker's deb and rpm packages no longer repeat the
cache notices on every configure once they no longer apply. The notice about
the old `v1` cache path now appears only while a loaded web-server
configuration file still names that path, and the notice about a
cold-started cache only while the worker has not yet written a `v2` cache or
the configuration still names a path from before 2.1. Each notice is one
line, so apt and dnf no longer cut it off, and it names the directive to
check: `pagespeed DaemonVolumePath` on nginx, `ModPagespeedDaemonVolumePath`
on Apache.

## 2.3.0 — 2026-10-12

This release fixes stale copies of CSS, JavaScript and image files after
they changed on the origin; updating is recommended for every installation.
Also new: a free edge-SEO rule pack template for the `rule_packs` filter of
mod_pagespeed 1.18.0. With module 1.18.0 on Apache, nginx and IIS the
optimizer no longer serves HTML; see Changed below.

Fixed: after a file changed on the origin, mod_pagespeed could keep sending
the previous version of a CSS, JavaScript or image file until the old copy's
cache lifetime ran out. For CSS and JavaScript every client was affected. For
images, the converted WebP and AVIF copies could stay stale for every client
that accepts them, and with `DaemonServeStoredEncodings on` so could the gzip
and brotli copies, while other clients already got the new version. The
optimizer now replaces its stored copies as soon as the web server has
recorded the new version. If you have been resetting the optimizer's cache
after each deploy to work around this, that is no longer needed.

Fixed: with `gzip_level` or `brotli_level` set to 0, the optimizer never
learned whether a CSS, JavaScript or unsupported-image file had really
changed. After the origin changed, each re-record of the file made it purge
and rebuild its stored copies, at most once per URL every 10 seconds, and
clients received unoptimized bytes until the rebuild finished. The optimizer
now tracks the file whenever the encodings that are turned on have been
stored, so a file whose bytes did not change is simply refreshed instead of
rebuilt.

Fixed: the JavaScript minifier now handles a comment between the `*` of an
object or class generator method and its name. Such scripts were previously
left unminified when the method was named `await` or `yield`; they are now
minified like the same code without the comment, and the output no longer
differs between passes.

Fixed: the Debian package no longer aborts configuration when
`systemctl daemon-reload` fails during install or upgrade; the RPM scriptlet
is guarded the same way for consistency.

Added: a free edge-SEO rule pack template, `packs/edge-seo/pack.example.json`,
with a README, for the `rule_packs` filter of mod_pagespeed 1.18.0 and later.
It lists one placeholder host in report mode and one rule per kind
(canonical, title, description, hreflang, JSON-LD); replace the example
values with your own. It is not signed.

Added: the worker's `--help` output now lists `--agent-render-allow-hosts`
with its default and meaning. The flag itself is unchanged.

Changed: with mod_pagespeed 1.18.0 on Apache, nginx and IIS the optimizer no
longer serves HTML. Those modules rewrite every HTML response themselves, so
the optimizer's own HTML improvements (such as critical CSS and LCP hints)
are not applied there. CSS, JavaScript and images are served by the
optimizer as before.

Changed: the bundled crossbeam-epoch library is updated to 0.9.20
(RUSTSEC-2026-0204), so dependency scanners no longer report it.

Changed: the packaged systemd unit describes the service as the "PageSpeed
Optimizer Worker" (what `systemctl status` shows) and the README calls the
optimizer process the worker throughout. Names that keep the word daemon for
compatibility (the `pagespeed_daemon.conf` file, CLI flags, API paths and
metric names) are unchanged.

## 2.2.0 — 2026-10-06

Update recommended: three security fixes, a fix for optimized copies lost
from a shared cache, a new on-disk cache format (the cache starts empty
once), and more accurate critical CSS. The security fixes cover the
management API and, on Windows, a local privilege escalation; each entry
below names the affected versions. Also new: `GET /v1/logs`, serve
statistics per host and per encoding, a Windows service mode for the
optimizer, and an nginx check that the module and the optimizer use the same
cache format.

Fixed: the container health checks in the Docker Compose file and the Helm
chart now wait up to 4 seconds for the optimizer's answer, inside their 5
second timeout. Until now they gave up after half a second whatever the
timeout said, so an optimizer that was busy for longer than that was reported
unhealthy: under Kubernetes the pod left the Service after three such checks
and was restarted after six. The Helm chart's startup, readiness and liveness
probes (chart 0.3.6) and the Compose healthcheck all changed the same way
(`socat -u -T 4 UNIX-CONNECT:<health socket> -`). If you copied the old probe
into your own manifests, replace it: a probe of the form
`echo '' | socat - UNIX-CONNECT:...` has the half-second limit.

Fixed: the optimizer no longer exits when a client closes its connection
before the reply has been written; only that one connection is closed. On
Linux the optimizer stopped, so the container restarted and the work in
progress was lost; in the combined image the web server in the same
container stopped with it, so the site was briefly unavailable. Affected:
every 2.0 and 2.1 release on Linux whenever the optimizer is not the first
process of its container — the combined image (2.0.22 and later), a
container started with `--init`, a pod or Compose project that shares the
process namespace, or a wrapper script that does not `exec` it — or, outside
a container, whenever it is not run by the packaged systemd service (for
example started by hand or by another supervisor). The packaged systemd
service and the worker image started as the container's own first process
were not affected.

Fixed: when a web server recorded a URL's original at the same moment the
optimizer wrote the optimized copy of that URL, the optimized copy could be
dropped from the cache without any error, and the URL was then served
unoptimized. The bundled cache library now notices that another process
changed the entry between a write's two steps and redoes the write. This
fix does not itself change the cache's on-disk format (the format change in
this release is described below), and nothing needs to be configured. Every
process
that shares the cache needs the same library version to be protected: a
process still on the older library can drop a copy the same way, so update
the optimizer and the web server module together.

Changed: the bundled cache library no longer starts a background monitor
thread and an idle worker thread for every open cache; they never had work
to do. It also closes a moment, right after an entry was recorded again,
in which the in-memory tier could briefly serve the entry's previous
version, and a process that exits while a cache is still open no longer
risks an abnormal termination on exit.

Fixed: a full purge of the optimizer's cache (`purge-all`) that ran while
other requests were still reading from or writing to the cache could crash
the optimizer. Usually nothing visible happened; rarely the optimizer
stopped and was restarted by its service manager or container runtime,
losing the work in progress. The bundled cache library now lets a purge
finish safely around reads and writes that are still in flight: a write
that comes too late is dropped and the content is optimized again on a
later request. A purge can now take a little longer while a write is being
completed, normally not noticeably. No damage to cache files is known; an
entry damaged this way would be rejected when it is read. Affects every 2.0
and 2.1 release.

Changed: the bundled cache library is updated for mod_pagespeed 1.1, which
shares the cache with this optimizer. Its web server workers could hang
right after a restart and keep their slot; this product was not affected,
because no process here forks while it keeps a cache open for the new
process to use. The update also fixes a crash at exit for programs that hash
cache keys with OpenSSL; neither product's builds do. One effect here: when
the optimizer starts a helper process (the browser, or a short check at
start-up) it may wait for a cache background task that is running at that
moment, normally for microseconds and at most for one flush of the cache
file to disk. The library update does not change the cache's on-disk format,
the optimizer and web server modules on the previous and the new library
version can share a cache, and nothing needs to be configured.

Fixed: a stylesheet or script whose optimized copy went missing from the
cache while its compressed copies stayed was served unoptimized until it
was purged. The optimizer now notices on the next notification for the URL
and processes it again, which writes the copy back, at most once per URL
and client class (viewport, pixel density, Save-Data) per 10 seconds. The
re-processing is counted when it starts: as
`notifications.missing_copy_healed` in GET /v1/stats, as
`pagespeed_notifications_missing_copy_healed_total` in GET /v1/metrics, and
as `missing_copy_healed` in the management `STATS` line. Content that is
served unoptimized on purpose (already minimal, not parseable, pinned by an
integrity attribute) is not affected. The rule covers a copy the running
optimizer wrote itself since it last judged the URL; a copy it found
already in place (after a restart, or when the URL is requested by another
client class) and that goes missing later is still restored only when the
URL's stored original expires and is recorded again.

Changed: the help text of
`pagespeed_notifications_dedup_heal_rate_limited_total` in GET /v1/metrics
now says that the series counts notifications dropped after either kind of
heal, and when the table tracking recent heals is full; the series also
moves for the new missing-copy heal.

Changed: the help text of the per-host serve-savings series in GET
/v1/metrics now states the host-name rule their `host` label values
follow; the values and the series are unchanged.

Added: the C API gains `ps_serve_stats_record_hit_host`, which records a
cache serve exactly like `ps_serve_stats_record_hit` and attributes it to
the host the front end served the response for. Serves recorded without a
host are attributed to "other". The shared serve-statistics file moves to a
new layout version, for this and for the split by encoding described below:
its counters — including the serve savings the admin console shows — restart
at zero once, when an updated worker first starts, and a front end or worker
built against the previous layout stops recording into it until it is
updated too.

Added: GET /v1/stats reports serve savings per host
(`serve_savings_by_host`): optimized HITs, original and served bytes for up
to 32 hosts, drawn from the first 64 distinct hosts seen since the optimizer
started and ordered by HITs, and everything else — the remaining hosts and
HITs a front end recorded without a host — under `other`. GET /v1/metrics
carries the same numbers with a `host` label.

Added: GET /v1/stats now reports how many entries of each type the
optimizer judged already optimal — no smaller variant could be produced —
with their original sizes, so a resource that serves at its original size
because it was already minimal shows up as exactly that instead of as zero
savings. The document also states when the worker process started and how
long it has been up, so one read answers "since when" for every counter in
it.

Added: GET /v1/stats now reports each content type's cache serves split by
the transfer encoding actually served (identity, gzip or brotli), next to
the totals it already reported. A serving front end that never serves the
stored compressed variants becomes visible as an all-identity row instead
of an unexplained saving. The shared counter file this comes from changes
layout with it; the one-time counter restart is described in the C API entry
above.

Fixed: the post-install scripts of the Linux packages no longer warn
about outdated optimizer paths in backup or leftover web-server
configuration files the web server does not load, such as a `.bak` copy
under Apache's `conf-available/`. The update notice now fires only when
a file the server actually loads still references the old paths.

Changed: repeated authentication timeouts on the management API are now
logged at most once a minute. The bundled console stops reconnecting to a
stream the server refused for a missing token, until a token is set.

Security: a request to the management API could stop the optimizer.
Affected: versions 2.0.0 through 2.1.0, and the optimizer packages through
1.16.0, when the management API is enabled. Update recommended.

Fixed: the live log stream (`/v1/ws/logs`) no longer loses lines without
saying so. When more than 64 lines arrived within one batch interval (100 ms),
a connected client received the first 64 and the rest never reached it, and
nothing on the stream showed that; long lines ran into a second limit of 64
KiB per interval. The stream now sends every line the optimizer still holds
(its most recent 2000), in order, as fast as the client reads them. A client
only misses lines that left that buffer before they could be sent: it reads
too slowly, or more than 2000 lines arrived at once. It then receives an
`overflow` message in their place with the number of lines missed
(`dropped_count`) and their sequence numbers (`first_seq`, `last_seq`). The
`log` and `snapshot` messages are unchanged. `GET /v1/logs` was not affected.
Log lines also cost the optimizer less while stream clients are connected:
each line is prepared once for all of them, when it is sent, instead of twice
per client.

Added: `GET /v1/logs?since=<seq>&limit=<n>` reads the optimizer's recent log
over plain HTTP, for clients that cannot hold a WebSocket open. Each page
holds at most 500 entries and 512 KiB, oldest first after `since` or the
newest without it; `next_since` is the cursor for the next read, `more` says
another page is waiting, `gap` says entries were dropped between reads,
`shed_total` counts entries the optimizer could not keep, and `stream_id`
changes when the optimizer restarts. Entries are numbered from 1, so
`since=0` reads from the beginning of the log.

Changed: an optimizer log message carries at most 4 KiB of text; a longer
message is cut on a character boundary and ends in "…[truncated N bytes]".
Log entries on the live log stream carry a `seq` number that starts at 1
and increases by one per entry while the process runs.

Security: with `--api-read-open`, more data could be read without the API
token than intended. Read-open now opens exactly the documented read
endpoints and the `stats`/`events` streams, and nothing else. Connection
limits for clients that have not authenticated are tightened. A read-only
console or script without a token no longer reads the optimizer log; give
such clients the token. The unix socket and `--api-no-auth` are unchanged.
Affected: versions 2.0.0 through 2.1.0, and the optimizer packages through
1.16.0, when `--api-read-open` is set. Update recommended.

Changed: browser analysis now reports a phone's or a tablet's User-Agent on
those viewports. Until now every analysis render, and the capture endpoint,
reported the headless desktop browser's User-Agent on every viewport, so a
site that adapts to the User-Agent (server-side, or in a page script reading
`navigator.userAgent` or client hints) was analysed in its desktop variant
for phone and tablet visitors too. The phone viewport (375 px) now sends
Chrome on an Android phone (`... Android 10; K ... Mobile Safari/537.36`,
client hints `mobile: true`, platform `Android`) and the tablet viewport (768
px) Chrome on an Android tablet (the same without `Mobile`, client hint
`mobile: false`), with `navigator.platform` `Linux armv81`: the strings the
front end classifies as the phone and the tablet class, so the variant a
render analyses is the one that class is served. The tablet is an Android
tablet rather than an iPad because iPadOS Safari sends a desktop User-Agent,
which the front end serves the desktop variant. The desktop viewport (1440 px)
is unchanged and keeps the browser's own User-Agent. The page analysis,
script coverage and font scan renders, and the CSS coverage and deferral
validation renders, now also write the page into a fresh blank window
created after the device emulation: `'ontouchstart' in window` is now true on
the phone and tablet viewports there, as `matchMedia` and
`navigator.maxTouchPoints` already were. In Chromium's `--headless=new` mode
(how the worker image runs its Chromium) the page analysis's
largest-contentful-paint and layout-shift observers now run as well; they
never ran in that mode before, so, now that page analysis reads the page
(the next entry), it reports the page's LCP element there too, its
first-paint time changes, and on the desktop viewport it reports the page's
layout shifts (on the phone and tablet viewports Chromium's mobile emulation
marks every shift as following user input, so their layout-shift score stays
0). The LCP
element is stored in the profile but decides nothing; the other two are not
stored. The fresh window also roughly halves the desktop renders (from about 2.05 s to
1.06 s in page analysis, the font scan and the validation render, measured
on Chromium 145); it costs about 13 ms per render with `--headless=new` and
under 1 ms with chrome-headless-shell. `navigator.userAgentData` exists only
in a secure context, so page scripts in those renders see the User-Agent
string and platform but no client hints; the capture endpoint, which loads
the page from its URL, sends them.
The async-CSS probe sends the same User-Agents from the same setting. Every
phone and tablet validation record made before this change stops matching
once and the page is validated again, through the same rate-limited path as
other revalidations; desktop records are not affected.

Fixed: browser analysis now reads the page it renders. Two bugs made several
analysis passes read nothing, or wait out their timeout, on every page. First,
page analysis collected its results as soon as Chromium reported the analysis
tab idle, and Chromium reports the blank tab the page is written into as idle
before the page is written, so it read an empty blank document: it returned
no images, no above-the-fold elements and no largest-contentful-paint element,
so the stored profile's measured fold and image dimensions were always empty
and the critical CSS was derived from the extractor's own estimate of the
fold. It now collects once the written page has settled: at the page's
network idle, or 2 seconds after the page's load event when the network never
goes idle (a page that polls or sends beacons at least every half second),
whichever comes first. That takes about 0.6 s on the phone and tablet
viewports and 2 s on the desktop one (measured on Chromium 145) instead of
returning at once. The font glyph scan had the same defect and the same fix;
the worker does not run it today. Second, every analysis render blocks the
requests it does not serve itself, but asked Chromium to do so with a
parameter name Chromium rejects, so a blocked request was never answered: the
page never finished loading, and a script blocked that way stopped the page
from being parsed any further. The deferral-validation render, which waits
for the page to finish loading, ran into its 60 second timeout on any page
with an image or another subresource at an absolute URL, so such pages were
never validated and their stylesheets stayed render-blocking. Script
coverage stopped at the first blocked synchronous script, so the scripts
after it got no verdict; they now get one, and a script found safe to defer
is now deferred on the served page like any other. Blocked requests now fail
at once, as intended, and a refusal from Chromium is logged as a warning
instead of being dropped. Existing profiles and validation records stay in
use until they are analysed again: each was derived, and validated, against
the fold of its own analysis.

Changed: browser analysis now sees phones and tablets as touch devices. The
analysis renders for the phone (375 px) and tablet (768 px) viewports already
laid pages out as mobile devices, but emulated no touch screen, so in those
renders `@media (hover: hover)` and `(pointer: fine)` rules applied and
`(hover: none)` and `(pointer: coarse)` rules did not, the reverse of a phone
or tablet visitor's browser: a rule that shows a control only on a touch
screen was never in the phone's critical CSS, and the deferral check could
confirm a phone's fold in a state no phone shows. The CSS coverage render,
page analysis, the deferral-validation render, script coverage, the font glyph
scan and the capture endpoint now emulate a touch screen with five touch
points on both viewports, from the one setting the async-CSS probe reads as
well, so `(hover: none)`, `(pointer: coarse)`, `(any-hover: none)` and
`(any-pointer: coarse)` match in every render and `navigator.maxTouchPoints`
is 5 (and, since the User-Agent change above, `'ontouchstart' in window` is
true in every render). The desktop viewport (1440 px) keeps `hover: hover`,
`pointer: fine` and no touch points. The critical CSS for
phones and tablets follows: it now keeps `(hover: none)` / `(pointer: coarse)`
blocks (and their `any-` forms) that match an element near the top of the
page and leaves out `(hover: hover)` / `(pointer: fine)` blocks, which those
visitors never match, so on pages built with Tailwind CSS v4 the phone and
tablet blocks no longer carry the `hover:` variant blocks; a bar fixed to the
screen only under `(hover: none)` now counts as on a phone's fold. The desktop
critical CSS keeps both kinds, as before: a desktop User-Agent can be an
iPad's (iPadOS Safari sends a desktop User-Agent) or a touch laptop's, and a
rule it does not match costs nothing in the desktop render. A block placed
after the page's stylesheets (a page using cascade layers whose order cannot
be proven) keeps both kinds for every device, as it keeps every `@media`
block any window could match. Every phone and tablet validation record made
before this change stops matching once, so a validated page keeps its phone
and tablet stylesheet render-blocking until the page it was validated on is
analysed again, which the worker queues right away and rate-limits as for
other revalidations. Desktop records are not affected.

Fixed: on a page with a `<base href>`, the stylesheet bytes the critical-CSS
work reads were fetched from the wrong place. The combined stylesheet the
critical block is derived from and confirmed against, and the copy of the
page the analysis browser renders, resolved each `<link rel="stylesheet">`
against the page's own directory instead of the `<base>`: under `<base
href="/sub/">` a link to `a.css` read `/a.css` instead of `/sub/a.css`, and
under a `<base>` on another host (for example a CDN) a link to
`/css/site.css` was read from the page's host, where it is not, so the sheet
counted as missing, the page was marked for revalidation on every request and
its stylesheet never deferred. Both now resolve the link exactly as the
browser does, the same way the scanner already matched sheets, and the
preload hints and SRI pins for those links follow the same rule. A
same-host `<base>` with an explicit default port (`:443` on https) is still
the page's own host. One shape without a `<base href>` changes as well: a
stylesheet link with a protocol-relative `href` such as
`//cdn.example.com/c.css` was looked up under a key the cache never holds, so
it always counted as missing and the page was marked for revalidation on
every request; it is now read as that host's `/c.css` (and
`//example.com/d.css` as the page's own `/d.css`), which ends that loop. No
stored validation record is affected by either change: such pages could not
be validated while their sheet counted as missing. Pages without a `<base
href>` and without protocol-relative stylesheet links, or with a `<base>` on
the page's own host and directory, are unaffected.

Fixed: the CSS minifier deleted an empty statement inside a parenthesized or
bracketed group in a declaration value: `a{--x:(a;;b)}` was served as
`a{--x:(a;b)}`, and `[a;;b]` or `f(a;;b)` lost a `;` the same way. A custom
property stores its value as written, and `var()` and `getPropertyValue()`
read it back verbatim, so the page saw a different value than the author
wrote. The minifier's declaration splitter ended a declaration at every `;`,
while CSS Syntax 3 ends one only at a `;` outside parentheses and brackets;
the splitter now tracks that depth, as the trailing-semicolon trim already
did. The same scan also opened a rule block on a `{` inside an unclosed
parenthesis or bracket (the shape the CSS fuzz lane found; browsers apply
nothing after such a parenthesis, so that face changed no page) and now
passes such text through unchanged; the block matcher likewise no longer
closes a rule at a `}` inside an unclosed bracket. Ordinary empty statements between declarations
(`a{b:c;;d:e}`) are still removed.

Changed: a page whose hero image is loaded by script gets no image hint. The
common lazy-loading shape puts an `<img data-src="hero.jpg" class="lazy">`
without a `src` in the hero container, followed by a `<noscript>` copy of it.
Now that the `<noscript>` copy is no longer the candidate (see the
`<noscript>` image entry below), the next image on the page, which is often
below the fold, would otherwise take the preload link, the Early Hint and the
`fetchpriority="high"`. The optimizer recognises this shape and emits no
image hint for such a page: nothing in the markup says which bytes the
loader will fetch, and a wrong high-priority hint costs more than none. In plain words, what changes:
a hero container (a `<header>`, `<main>`, `<section>` or `<article>`, or an
element with a hero-like class) whose only image is such a placeholder with
its `<noscript>` copy gets no image hint, and neither does any image in a
container that was already open at that point, for example everything else
inside the `<main>` that wraps the hero. A later sibling section with an
ordinary hero image does get hinted, as before, so a lazy-loaded logo in the
`<header>` does not silence the real hero below it; this includes a content
`<section>` or `<article>` further down the page, whose image is hinted as it
was before. A lazy placeholder outside
a hero container, or one without the `<noscript>` copy, or a page whose first
image is simply later in the document, is handled as before. The
`fetchpriority="high"` given to the first image when no candidate is found no
longer goes to a placeholder without a source (an image with a `srcset`, or
inside a `<picture>` with a `<source srcset>`, has one), and such a
placeholder among the first three body images is left as the author wrote it.
What counts as the placeholder: an `<img>` with a lazy data attribute or a
lazy-loader class and no usable `src` (absent, empty or `data:`); one with a
lazy data attribute whose `src` is a stand-in file
(`<img src="/blank.gif" data-src="/hero.jpg">`, with a name such as blank,
placeholder, pixel, spacer, 1x1, transparent, loading or lazy); and, inside a
hero container, one with a lazy data attribute and any other real `src` (a
low-quality preview) when its `<noscript>` copy names a different image (the
same image spelled as an absolute URL does not count as different). Without
such a copy, or with a lazy class alone, an image with a real `src` is
handled as before. In every case the copy has to be a visible image: a
tracking pixel's `<noscript>` fallback or a hidden image in the same
container is not the copy of the hero, so such a page keeps its hero hint.
A small icon inside the hero container (declared width x height under
10,000 px², for example a 48x48 call-to-action icon) does not become the
hint for a hero loaded by script either: such a page gets no image hint, as
a hero without the icon does.

Fixed: the inlined critical CSS block no longer carries a fragment of an
at-rule whose prelude has a `;` inside parentheses (`@supports (a;b) { … }`,
or an anonymous layer written `@l\61yer (a;b) { … }` inside an `@media`
block copied whole). The optimizer read the `;` as the end of a statement and
emitted the rest (`b) { … }`) as a rule of its own; browsers discard that
fragment, so this only removes bytes from the block.

Fixed: critical CSS no longer drops rules whose selectors contain a space or
a `>`, `+` or `~` inside square brackets or parentheses, such as
`li:nth-child(2n+1)`, `input[type=text i]`, `.note:not(.a > .b)` or
`[class~=active]`. The optimizer split such selectors in the middle and could
then not match them to the page, so the elements at the top of the page lost
those styles until the stylesheet loaded. Rules that style the children of an
element, such as Tailwind CSS v4's `space-y-*` and `divide-y` spacing
utilities, are now kept when that element is near the top of the page. Rules
scoped to a class or id that appears nowhere on the page, such as Tailwind
Typography's `.prose` rules on a page without an article, or Bootstrap's
`.input-group` and `.dropup` rules on a page without those components, are
left out, which makes the critical CSS smaller on most pages. Classes that
scripts commonly add to the page's root element before it is shown (`dark`,
`light`, `js`, and any class on `html`, `body` or `:root`) do not count as
missing.

Changed: browser analysis now renders the tablet viewport (768 px wide) as a
mobile device, as tablets in portrait lay out pages, instead of as a desktop
window. The CSS coverage render, page analysis, the deferral-validation render
and the other analysis renders all take this from one setting, which the
async-CSS probe reads as well. On a page whose content is wider than 768 px at
that size, tablets keep the page at device width but let the overflowing
content widen the layout viewport, which fixed-position elements follow; the
analysis now sees that layout. Every
tablet validation record made before this change stops matching once, so a
validated page keeps its tablet stylesheet render-blocking until the page it
was validated on is analysed again, which the worker queues right away and
rate-limits as for other revalidations. Phone and desktop records are not
affected.

Fixed: a backslash-escaped quote in a selector no longer hides the rest of
the stylesheet from the critical CSS. Tailwind CSS v4 writes a `'` in a class
name as `\'`; shadcn/ui's Button uses such a class
(`[&_svg:not([class*='size-'])]:size-4`). The optimizer read that quote as the
start of a text string that never ended, so every rule after it was skipped:
the critical CSS of such a page could lose most of the rules for the top of
the page, and the page would flash unstyled while its stylesheet loaded. The
optimizer now reads backslash escapes as CSS defines them, also for escaped
braces, commas and combinators in class names, and ends a string that is
missing its closing quote at the end of the line, as browsers do. The same fix
applies where browser analysis reads the CSS a page used and where unused CSS
is removed. An unquoted `url(...)` containing a quote no longer starts a text
string either. Pages whose stylesheets escape a quote, a brace or a comma
outside a string are validated again once, instead of keeping their
stylesheet render-blocking until their browser profile expires (24 hours by
default): the earlier validation of such a page was made against critical CSS
that missed most of its rules. Other pages keep their validation.

Fixed: on phones and tablets, the page no longer shifts sideways while its
deferred stylesheet loads because of an image, video or icon far below the
fold. An image without a width, a YouTube embed with `width="560"`, or an
inline `<svg>` without a width (300 px by default) can be wider than a phone
screen until the stylesheet shrinks it. The inlined critical CSS only carried
rules for elements near the top of the page, so before the full stylesheet
arrived such an element further down could make the page wider than the
screen. A phone browser then widens the page's layout to fit it, which moves
the whole visible area, including floating buttons, partly off-screen. For
phones and tablets, the rules that apply to these elements (`<img>`,
`<iframe>`, `<video>`, `<canvas>`, `<embed>`, `<object>`, `<audio controls>`
and `<svg>`, unless the markup gives them a width of at most 320 px) are now
part of the critical CSS wherever the elements are on the page. They add at
most 8 KiB, and never enough to push the critical CSS past the size limits
above which it is not inlined at all. The desktop critical CSS is unchanged.
Existing validations of deferred stylesheets are kept rather than redone: the
added rules only size elements as the full stylesheet already does once it
loads, and redoing them would validate nearly every page with an image
again.

Fixed: browser analysis now measures the page the way a browser with
JavaScript sees it, so content inside `<noscript>` no longer counts as part of
the fold. The CSS coverage render and the render that validates stylesheet
deferral keep running with JavaScript turned off (part of the defense against
server-side request forgery, and it keeps renders repeatable), and a browser
with JavaScript off displays `<noscript>` content and applies its CSS. A
"please enable JavaScript" banner at the top of the page therefore took the
top of the fold in those two renders, and pushed the content that browsers with
JavaScript show there out of view. The optimizer now removes the `<noscript>`
elements, with everything inside them, from the documents those two renders
load, reading the page the way a browser with JavaScript reads it (including
inline SVG and MathML, comments and scripts). The renders that run JavaScript
get the page unchanged. If the optimizer cannot be sure where a `<noscript>`
ends (for example one that is never closed), it does not render the result: it
skips the coverage render and does not validate the page, so the stylesheet
stays render-blocking. A check guards the removal against the optimizer's own
HTML scanner, an independent reading of the same page: `<body>` and the 200
elements after it must be the same elements in the same order, and across the
whole page the removal is refused when two elements in a row, or more than
three in all, are missing or read as other elements. The removal follows the
browser's rules for misnested markup (end tags across open elements, a
paragraph closed by a block element, an `<a>`, `<nobr>`, `<button>` or
`<form>` inside an open one, inline SVG and MathML, and the `<svg>` holding a
`<template>` that streaming React and Next.js pages emit). Deferral
validation is also refused when a `<noscript>` and an HTML comment overlap in
a way that the optimizer's serving path and a browser read differently, since
the critical CSS block could then be placed somewhere else in the validated
page than in the served one; the placement that validation replays reads HTML
comments exactly as the serving path does. The validation render is also
refused when the full
page renders as a single colour, since a blank page compares equal to any
other blank page. The critical CSS extractor no longer matches the page's CSS
against elements inside `<noscript>`, `<template>`, `<noembed>` or
`<noframes>`. The page served to visitors is unchanged: it keeps its
`<noscript>` elements, including the `<noscript>` stylesheet copy the optimizer
adds for deferred stylesheets. The page-structure hash that keys browser
profiles is unchanged, so no page is analyzed again early because of this. A
page whose `<noscript>` holds something a browser without JavaScript shows (a
`<style>`, a stylesheet that does not load for JavaScript browsers anyway,
visible elements or text) or acts on (a `<meta http-equiv=refresh>`, which
the validation render used to follow) was validated against the wrong fold.
Its validation
record no longer matches, so the page keeps its stylesheet render-blocking until
it is validated again, which the worker queues right away for the page the
record was made on. Pages whose `<noscript>` only holds hidden tracking
elements, such as the Google Tag Manager iframe or a 1x1 tracking pixel, keep
their records. A new `noscript_strip_refusals` counter in BROWSER-STATUS
counts the analyses whose coverage render was skipped.

Fixed: CSS inside `<noscript>` no longer reaches the inlined critical CSS
block. The optimizer read `<style>` elements and `<link rel="stylesheet">`
inside `<noscript>` as part of the page's CSS, so rules meant only for
browsers without JavaScript could be copied into the block that every browser
gets. The same now applies to `<template>`, `<noembed>`, `<noframes>` and
`<math>`, and to a `<link>` inside inline `<svg>`; a `<style>` inside inline
`<svg>` still counts, because it styles the whole page. The same goes for a
`<style>` or stylesheet `<link>` whose `type` browsers do not treat as CSS,
such as `type="text/tailwindcss"`, `text/plain` or `text/x-less`: browsers
ignore these elements, and so does the optimizer now. One case still counts:
the loadCSS pattern, where a `<link rel="preload" as="style">` with an
`onload` handler turns itself into the stylesheet and a `<noscript>` link
declares the same stylesheet for browsers without JavaScript. That stylesheet
is read once, at the position of the preload, which is where browsers apply
it; the two links are matched by the address they resolve to, so `/css/a.css`,
`css/a.css` and `http://example.com/css/a.css` on the same page are one
stylesheet, and under a `<base href>` on another host `/css/a.css` is that
host's stylesheet, as browsers resolve it. A preload without an `onload`
handler does not make a `<noscript>` link count. The other `<noscript>` stylesheets are no longer sent as Early
Hints, no longer rank as render-blocking when choosing preconnect origins, and
a `<link rel="stylesheet">` inside `<noscript>` is no longer deferred. On
pages with other CSS in `<noscript>`, the CSS that deferral was validated
against changes, so a validated page stops deferring its stylesheet until it
is validated again. It no longer waits for the browser profile to expire (24
hours by default) for that: when the page a profile was validated on is served
with a stylesheet that no longer matches the record, for this reason or
because the stylesheet was changed, the worker now queues a new browser
analysis for the page's template straight away, at most twice per template per
profile lifetime and at least 30 seconds apart. Other pages that share the
template do not trigger it, since their own inline styles always differ from
the validated page's. Until that analysis finishes, the page keeps its
stylesheet render-blocking, and a critical CSS block over 64 KiB is not
inlined. Pages with a stylesheet link inside `<noscript>`, or whose only
`<style>` is inside `<noscript>`, get a new browser profile on their next
request, because the profile is keyed on a page-structure hash that counts
stylesheets.

Changed: the critical CSS extractor now reads media-query range syntax such as
`@media (width >= 48rem)`, which Tailwind CSS v4 emits for its
breakpoints, as well as `not`, `or` and nested conditions. It leaves out a
width-conditioned `@media` block only when no window of the visitor's device
class can match it. The device class comes from the User-Agent, and the
windows a class can have overlap: a phone is 0 to 980 px wide (landscape, and
pages without a `<meta name="viewport">`), a tablet 0 to 1480 px, and a
desktop browser any width. The desktop block used to leave out `max-width`
blocks for phone widths, and a desktop browser in a narrow window showed the
desktop layout until the stylesheet loaded; it now keeps them. On a page whose
CSS uses a cascade layer and whose layer order the optimizer cannot prove (see
the cascade-layer entries below), the block goes after the stylesheets, so the
extractor keeps every `@media` block that can match any window, for every
device class; so does a page whose CSS has an anonymous layer (`@layer { }`
or `@import … layer`), even when its order is proven. A phone zoomed out
past 980 px can no longer lose a responsive override for as long as the page
is open.

Changed: `@media` blocks in the inlined critical CSS are now filtered rule by
rule, like the rest of the stylesheet. A block whose rules match nothing above
the fold is left out, unless one of them cannot be matched against elements
at all (a `:root:not([data-theme=light])` dark palette, `[data-theme]`,
`::selection`), and a large block keeps only the rules the fold needs and
those that cannot be matched;
before, every block that could apply was copied whole unless it sat inside a
large cascade layer block. On a stock Bootstrap 5.3.3 page the desktop block
shrinks from 105 KB, which was over the 64 KB inline limit, to 53 KB.

Fixed: `@media not print { ... }` and `@media screen, print { ... }` apply on
screen, but the critical CSS extractor left them out of the inlined block
because the query contained the word `print`. Only blocks that no screen can
match, such as `@media print` and `@media only print`, are left out now.

Fixed: pages whose stylesheets use CSS cascade layers, which includes every
Tailwind CSS v4 page, now also get the inlined critical CSS block before their
stylesheets, so the full stylesheet wins every tie once it loads and a
responsive rule the block left out is no longer overridden. The order of
layers is set by where each layer is first mentioned, so the block now starts
with an `@layer a, b, c;` statement that lists every layer of the page in the
order the browser meets them in the original page: across all `<link>` and
`<style>` stylesheets in document order, including layers declared by
`@import … layer(x)` and nested layers such as `a.b`. That statement fixes
the page's own layer order before anything else can. A block that uses only
anonymous `@layer { }` rules gets the statement too, so its `!important`
declarations cannot outrank the page's named layers. The stylesheets counted
are the ones the optimizer reads for the inlined block (see the `<noscript>`
entry above), in the order browsers apply them: a `<style>` or `<link>` whose
`type` browsers do not treat as CSS (such as `text/tailwindcss`), stylesheets
inside `<noscript>`, `<template>`, `<noembed>` or `<noframes>`, a `<link>`
inside `<svg>` or `<math>`, a `<style>` inside `<math>`, and a
`rel="preload" as="style"` link without an `onload` handler are not counted.
A loadCSS preload (`rel="preload" as="style"` with an `onload` handler) counts
at its own position, which is where browsers apply it.

The optimizer only does this when it can prove the order; otherwise the block
stays at the end of `<head>`, as before. It keeps the old place when a
stylesheet on the page is not available to it (a stylesheet from another
domain, one not cached yet, an `@import` it cannot read, a loadCSS preload
without a `<noscript>` copy of its stylesheet, or an alternate, disabled or
titled stylesheet, since browsers apply only the preferred set of titled
stylesheets); when a layer is first declared inside `@media`,
`@supports` or another conditional rule, or in a stylesheet with a `media`
attribute other than `all` (browsers do not register a layer whose condition
does not match, so its place depends on the visitor's window); when an
anonymous `@layer { }` comes before a named layer; when a layer name or an
at-rule keyword is written with an escape; when a stray `}` or `;` sits where
a rule may start (browsers then drop the rule after it); and when a script
that may insert a stylesheet comes before a stylesheet that declares a new
layer. Such a script is a parser-blocking external script (no `async`,
`defer` or `type="module"`), which can `document.write` one, or an inline
script that mentions `document.write`, `insertRule`, `adoptedStyleSheets`,
`CSSStyleSheet`, `@layer`, or creates a `style` or `link` element. Remaining
risk: a stylesheet that an `async`, `defer` or module script inserts, or an
inline script inserts without any of those signs, is not detected, and the
statement overrides the layer order such a stylesheet would have set. A layer
only that stylesheet declares ends up after the ones the statement lists, and
a stylesheet inserted before the page's first stylesheet that declares the
page's layers in a different order (for example `@layer b, a;` ahead of a
stylesheet that declares `a` then `b`) no longer changes their order. A
stylesheet inserted at the very start of `<head>`, before the inlined block,
is not affected. A script the optimizer itself defers counts as the
parser-blocking script it was.

The empirical critical-CSS check places the block the same way and renders
the full stylesheet with the same statement in front, and on pages whose
stylesheet uses layers its record is now bound to the page's layer order as
well as to the stylesheet bytes. Records of such pages therefore stop matching
once; the page the profile was made on then queues a new browser analysis
straight away, as for a changed stylesheet, and until it finishes the page's
stylesheets stay render-blocking. Records of pages without layers are not
affected. `@import` rules inside inline
`<style>` elements now resolve against the page's `<base href>`, as browsers
do, for the combined stylesheet and for the layer order. Re-processing a page
replaces the block and its statement; nothing accumulates. Scripts that read
`document.styleSheets[0]` or `querySelector('style')` on such pages now get the
inlined block.

Fixed: on pages whose cascade layer order the optimizer cannot prove, the
inlined critical CSS block now leaves out every rule inside an anonymous
cascade layer (`@layer { … }`). There the block goes after the stylesheets, so
its anonymous layer is registered after the stylesheet's, and for normal
declarations a later layer wins whatever the selectors: the block's
`.x{display:none}` beat the stylesheet's `.x.open{display:block}` for as long
as the page was open. Anonymous layers nested in rules the block copies whole
are left out too, and so is every at-rule whose keyword is written with an
escape (`@l\61yer { … }`, which a browser reads as `@layer { … }`), whatever
the keyword decodes to. Rules in named layers and outside layers stay, and
pages whose block goes before the stylesheets keep their anonymous-layer rules
(without their `!important` declarations, see the placement entry below). The
empirical critical-CSS check derives the block the same way. On pages whose
stylesheet has an anonymous layer and
whose block goes after the stylesheets, validation records are bound to that
(the layer-order binding gains a `u2` marker), so they stop matching once and
the page the profile was made on queues a new browser analysis straight away.

Fixed: on pages that load a stylesheet with the loadCSS pattern (a
`<link rel="preload" as="style">` whose `onload` handler turns it into a
stylesheet), the inlined critical CSS block now goes before that link instead
of after it. Browsers apply the stylesheet at the link's position, so a block
placed after it won every tie against that stylesheet for as long as the page
stayed open. The empirical critical-CSS check places the block the same way.

Fixed: the inlined critical CSS block now comes before the stylesheet it was
taken from, instead of at the end of `<head>`. The block carries a copy of
some of the stylesheet's rules, trimmed to the above-the-fold content and to
the visitor's device class (mobile, tablet or desktop). When the block came
after the stylesheet, its copies won every tie against the real stylesheet
once that loaded, so a responsive rule the block had left out lost to the
general rule it kept, for as long as the page stayed open: a phone in
landscape, or a narrow desktop window, could keep desktop spacing on elements
the stylesheet restyles for that width. With the block first, the full
stylesheet decides as soon as it applies, and the block only shapes the first
paint. The block goes before the page's first `<link rel="stylesheet">` or
`<style>` in `<head>`; it stays after `<meta charset>`, a `<meta>`
Content-Security-Policy and `<base>`, and is never placed inside `<noscript>`.
A page with no stylesheet in `<head>` gets the block at the end of `<head>`, as
before. One exception: on a page whose CSS uses cascade layers (every
Tailwind CSS v4 page does) the block goes first only when the optimizer can
prove the page's layer order, as the cascade-layer entry above describes;
otherwise it stays at the end of `<head>`, after the page's stylesheets. The
order of layers is set by where each layer is first mentioned, and a block
placed first without that proof would list its layers in the order the
optimizer read the page's CSS, which can differ from the page's own order. A
block that goes first leaves out `!important` declarations that sit inside an
anonymous cascade layer (`@layer { … }`), in every spelling a browser reads
as `!important`, and anonymous `@import … layer` rules: each anonymous layer
is a separate layer, the block's is registered first, and for `!important`
declarations the earlier layer wins, so the block's copy would beat the
stylesheet's own `!important` override for as long as the page stayed open.
Scripts that read `document.styleSheets[0]` or `querySelector('style')` now
get the inlined block, not the page's first stylesheet, when the block goes
first.

Fixed: the critical CSS the optimizer inlines without a browser profile now
covers the above-the-fold content on pages with a large `<head>`. The
heuristic extractor counted its above-the-fold element budget from the start
of the document, so on a page whose `<head>` holds more elements than the
budget (dozens of `<meta>`, `<link>` and `<script>` elements is normal today)
the budget was spent before the first visible element and the inlined block
carried none of the fold's layout rules. The budget now starts at `<body>`,
its default grows from 25 to 300 elements (utility-class markup spends
hundreds of elements on a header, its menus and a hero), and elements that a
`position: fixed` rule selects — chat launchers, cookie banners, floating
buttons at the end of `<body>` — are treated as above the fold wherever they
sit in the document, together with their contents. The inlined block gets
larger on such pages (on one measured home page it grows from 25 KB to
45 KB against 118 KB of stylesheet), and the deferral gate is unchanged: a
stylesheet is still only made non-render-blocking with a validated browser
profile. The C API defaults `ps_html_config_t.critical_css_max_elements` and
`ps_critical_css_config_t.max_elements` change from 25 to 300 to match.
In the optimizer worker (not the C API's `ps_html_process` /
`ps_critical_css_extract`, which have no inline cap), the block is also now
subject to the same inline budget on both derivation paths: it is not inlined
(and the stylesheet stays render-blocking) when it would be 60% or more of a
combined stylesheet of 15 KB or more (the `--async-css-min-deferred-bytes`
threshold; a smaller sheet is still inlined in full as before), or when it
is larger than 64 KiB while the stylesheet stays render-blocking. A deferral
authorised by a validated browser profile is never vetoed by the 64 KiB cap.
The new `critical_css_skipped_byte_cap` counter on `/v1/stats` counts the
blocks the cap dropped.

Pages with a validated browser profile derive their block from the measured
fold added to the estimate, so their block grows in the same direction; a
validation record is keyed on the stylesheet alone and keeps authorising
deferral of the larger block until the page is re-validated. The direction
is a superset of what was validated, never a subset.

Security: on Windows, a local privilege-escalation issue is fixed: code
already running locally on a Windows host could gain the privileges of an
application that uses ModPageSpeed. Affected: the
`WeAmp.PageSpeed.AspNetCore` and `WeAmp.PageSpeed.NativeAssets.Windows`
NuGet packages (the latter carries `pagespeed.dll`), versions 2.0.0 through
2.1.0, and the Windows optimizer built from them. Update recommended. Linux
and macOS are not affected.

Changed: the optimizer's cache moved to a new on-disk format, and with it
to a new directory. The cache storage library now checksums documents with
CRC-32C (hardware-accelerated on x86-64 and ARMv8) and keeps the previous
pass of a full cache readable until its bytes are actually overwritten, so a
URL's original and its variants stay servable across a cache wrap. A
directory holds one format, so the default cache directory is now
`/var/cache/pagespeed-optimizer/v2` (2.1.0 used `.../v1`), and the
optimizer publishes `cache_dir_generation=2` in `pagespeed-shared.conf`.
After the upgrade the cache starts empty and refills as traffic arrives. The
`v1` directory is left on disk, which keeps a rollback to 2.1.0 warm; delete
it once you will not roll back. The packages print a notice when they find
it. If your web-server configuration names the cache path
(`pagespeed_cache_path`, `ModPagespeedDaemonVolumePath`), change `v1` to
`v2`. Upgrade the optimizer and the serving module together: a pair
linking different cache formats opens two different files instead of
sharing one. See UPGRADING.md.

Added: the nginx module checks that the optimizer uses the same cache format
it does. It compares its own cache-directory generation with the
`cache_dir_generation` the optimizer publishes in `pagespeed-shared.conf`. If
they differ (an optimizer rolled back, or the two packages upgraded
separately), nginx keeps serving but turns its cache off: requests go
straight to the origin, and nothing is read from or written to the cache,
and the optimizer is not notified. It logs one error that names both
generations, says which side is older, and gives the fix (upgrade both to the
same release, or run both from the same image tag). Before, such a pair
opened two different cache files, never shared a cache, and logged nothing.
The check runs at start and on every reload, and again whenever the optimizer
rewrites its shared config, so a finished rolling upgrade turns the cache
back on without an nginx reload. An optimizer that publishes no generation
(older than 2.1.0) gets one warning and the cache is used as before. The new
nginx variables `$pagespeed_cache_generation` (`match`, `mismatch` or
`unknown`), `$pagespeed_cache_generation_module` and
`$pagespeed_cache_generation_optimizer` expose the result for a log format
or a status location.

Fixed: in the container deployments the optimizer and nginx could overwrite
each other's cache writes. They ran in separate PID namespaces, and the cache
library recovered a cross-process write lock from a holder whose PID looked
gone, which a live process in another PID namespace does. The library now
tells a live holder from a dead one by a lock the kernel holds for each
process, which works across PID namespaces. Older builds still judge by PID,
so the compose files now run both services in the PID namespace of a small
`pidns` container, which keeps an upgrade safe while an old and a new build
share the named cache volume. `pidns` runs the digest-pinned Kubernetes pause
image, so an image upgrade never recreates it. Restarting the worker or nginx
leaves the other running; stopping or recreating `pidns` stops both, and
`docker compose up -d` brings all three back. The Helm chart (0.3.5) sets
`shareProcessNamespace: true` on the pod, for the same one-namespace layout
(its per-pod cache never mixes builds). Custom compose files that share the
volume across an upgrade should do the same.

Changed: the optimizer's systemd unit admits `mincore(2)` in its system-call
allow-list. The cache library now checks whether data is already in memory
before it issues a readahead hint, including when it reopens its cache at
start. Without it, a host that masked the browser-analysis profile (which
also admits `mincore`) would have the optimizer killed with `SIGSYS`.

Fixed: under heavy write traffic from both the web server and the optimizer,
one process could take over a cross-process cache lock that another, merely
descheduled, process still held, and the two writes could overlap. A process
now waits for a live holder. A write or removal that has waited 250 ms in
total behind other holders gives up and reports the cache as busy; the write
is dropped and redone on the next miss, and a purge reports failure instead
of success.

Fixed: the Windows optimizer worker (`factory_worker.exe`) starts on a
Windows Server that has no Visual C++ runtime installed. It used to fail at
once with a missing-DLL error (exit code 0xC0000135) for any argument,
`--help` included, unless the Visual C++ 2015-2022 redistributable was
present. The worker now carries the runtime itself, as `pagespeed.dll`
already did, so nothing beyond the operating system is needed.

Added: the Windows optimizer worker runs as a Windows service. Register it
with `--service` on its command line and the Service Control Manager starts
it, sees it as running once it is ready, and stops it gracefully (the same
shutdown Ctrl+C gives) on a service stop or at system shutdown. A start the
worker refuses is reported with the worker's exit code. A service has no
console, so `--log-file PATH`, valid only with `--service`, appends the log
to a file that can be read while the service runs; a log file that cannot be
opened stops the service with service-specific exit code 90. Run from a
console, `--service` is refused with a message saying so.

Fixed: more images now get a WebP or AVIF variant. The optimizer encodes a
variant, measures its perceptual quality, and re-encodes at a different
quality when the measurement falls outside the accepted band; a variant that
never reaches the band is not served. When the measurement came out BELOW the
band, the search used to raise the quality by a fixed amount per attempt, so
within its attempt budget it could only look a short distance above where it
started -- and an image whose acceptable quality lay further up was refused
even though an acceptable encode existed. Raising the quality now halves the
range that is left on each attempt instead, which reaches the top of the
quality range without spending more encodes, so those images get their
variant. An image the optimizer has to encode near the top of the range to
satisfy the band is what gains; the case this was diagnosed on was a
photograph stored as a PNG, which was being served as the original PNG to
every client.

An image that already received a variant may now receive a slightly
different one, usually slightly larger: the optimizer stops at the first
encode that lands inside the band, and it now arrives at the band from a
different direction, so it settles on a different encode than before.

Two things are deliberately unchanged. The rule about what is servable is the
same: an image that cannot reach the band is still refused. And the other
direction is the same: when a variant measures ABOVE the band the optimizer
still corrects it downward by at most the distance it always did, because an
over-quality variant only costs bytes while an under-quality one is visible,
so that correction stays cautious. Variants that were previously refused
because the upward search could not reach them are retried the next time the
optimizer works on that URL, with no cache clearing needed.

Fixed: an image inside `<noscript>` is no longer treated as the page's largest
image. A browser that runs scripts never creates the elements inside a
`<noscript>` (nor those inside `<template>`, `<noembed>` or `<noframes>`), so
the preload link, the Early Hint and the `fetchpriority="high"` the optimizer
emitted for such an image fetched a file the page never shows, at the highest
priority, ahead of the real content. The common case is the lazy-loading
fallback `<img data-src="hero.jpg" class="lazy"><noscript><img
src="hero.jpg"></noscript>`, where the optimizer picked the `<noscript>` copy.
Such images are now passed over: the next image the page does create is the
candidate, or none when there is no other. For a hero that is loaded by
script (an `<img data-src>` without a `src`, plus the `<noscript>` copy) that
next image may well be below the fold, so such a page gets no image hint at
all; the script-loaded-hero entry above describes that rule. The
`<noscript>` image also no
longer takes the `fetchpriority="high"` given to the first image when no
candidate is found, no longer counts toward the first images that are kept
from lazy-loading, and is left as the author wrote it, without
`loading="lazy"`; the same goes for an `<iframe>` inside `<noscript>`.

## 2.1.0 — 2026-09-17

Changed (plan for this before you upgrade): **the disk cache starts empty.**
This release moves the optimizer to a new on-disk cache format. The new
binary opens a new cache file instead of converting the old one, so every
deployment begins with a cold cache that refills as traffic arrives. Nothing
is lost that cannot be rebuilt and no configuration change is needed, but
expect a temporary drop in cache hit rate and a matching rise in origin
fetches and optimization work until traffic has warmed the cache again --
so upgrade at a quiet hour if your origin is sensitive to that.

Check your free space before you start. During the upgrade both the old and
the new cache file exist, so the cache directory needs room for a second
volume: free space at least equal to your configured cache size, on top of
whatever the existing volume already occupies. If the directory has that
room, leave the old file where it is -- that is what makes a rollback warm --
and delete it once you are confident you will not roll back. **If it does
not, stop the optimizer, delete the old cache file, and only then start the
new one.** The new volume is extended to its full configured size while the
optimizer starts, ahead of any cleanup, so starting first on a directory
sized for a single volume can exhaust the disk. The file is allocated
sparsely, so that failure would not show up at the upgrade: it would show up
hours later, mid-traffic, as the cache warms into space that is not there.

If a web server on the same host carries the serving module, start the
optimizer before it: until the optimizer has created its new volume the module
refuses to start rather than degrading, so the web server does not come up.
At boot the packaged unit now handles the order for you -- see UPGRADING.md.

Downgrading is warm provided you still have the old file -- an earlier binary
reopens it untouched, and nothing in this release removes it: a full cache
purge (`POST /v1/cache/purge` with `"scope":"all"`) and the automatic recovery
the web-server module runs when it cannot open the cache both clear only the
volumes of the format they are running, so the rollback survives them. (A
volume still sitting at the un-fingerprinted legacy path, written by a build
that predates format-stamped filenames, carries no format to respect and is
still cleared.) The old
file is therefore yours to delete, whenever you are confident you will not
roll back to the build that wrote it.

Fixed: on a host that was momentarily busy when the optimizer started, the
browser-analysis sandbox probe could declare the sandbox unavailable for the
daemon's whole lifetime. The probe gives its test process a bounded wait, and
when that wait elapsed without an answer -- say because a loaded machine
simply never scheduled the probe inside it -- the refusal was reported with
the timeout's generic remedy instead of the real cause. A probe attempt that
times out, or whose process cannot be started under resource pressure, is now
retried before any outcome is reported, so a transient stall resolves to the
answer the next attempt reaches: when a syscall filter is responsible, the
refusal again names the SIGSYS and the remedy that actually fixes it.

Fixed: a URL whose optimized variants were re-recorded many times could
permanently stop accepting new ones. Every re-record used to leave the
superseded copy linked behind the new one, so the stored chain for that URL
grew on every refresh while the number of distinct variants stayed put,
until it reached its limit and every further write to that URL was refused.
The URL then froze: reads kept succeeding, so nothing looked wrong, but the
browser was served an increasingly stale version that was never replaced.
Superseded copies are now unlinked as the replacement is written, so the
chain no longer grows with refreshes. A chain that still reaches the limit
recovers by itself only in the narrow case where the URL holds that one
variant and nothing else; a URL that also holds its original -- the ordinary
case for an optimized page -- is not covered by that reset, and is recovered
instead by clearing the entry at the next revalidation.

Fixed: after content was re-optimized, replaced or purged, the in-memory
cache tier could keep serving the superseded copy until it happened to be
evicted. The stale in-memory entry is now dropped when the content behind it
is re-recorded or removed, so a refresh takes effect immediately instead of
after an unpredictable delay.

Changed: the cache enforces a 64 MB ceiling on a single stored object. The
limit was already documented but was never applied; an over-size write is now
rejected before anything is buffered, which also bounds how much of one
response the cache holds in memory at once. Almost nothing reaches it in
practice: cached originals are capped at 16 MiB well before this, so it
applies only to an optimized variant, and an optimized variant that large is
outside anything the optimizer produces.

Changed: the bundled Cyclone cache library is licensed under the Apache
License 2.0, matching the rest of the distribution. `NOTICE` and the SBOM
record it.

Changed: license — the source tree is now under the Apache License 2.0 (was
BUSL-1.1).

Changed: mod_pagespeed 2.1 is licensed under the Apache License 2.0. Every feature is
available to everyone. The console shows a single dismissible pointer to
support subscriptions. A 2.0 configuration starts unchanged (see
UPGRADING.md).

Changed: the NOTICE product line and the SBOM root package now use the
mod_pagespeed 2.1 identity; the SBOM file is `sbom/mod_pagespeed-2.1.spdx.json`.

Added (host packages): the deb and rpm packages ship `THIRD-PARTY-NOTICES`
next to `LICENSE` and `NOTICE` (`/usr/share/doc/pagespeed-optimizer/` on
Debian and Ubuntu, `/usr/share/licenses/pagespeed-optimizer/` on Enterprise
Linux), so the notices of the components statically linked into the daemon
travel with the binaries the way the Docker images and the NuGet packages
already carried them. `THIRD-PARTY-NOTICES` now also lists the Rust standard
library and the `flo_curves`, `color_quant`, `scoped_threadpool` and
`miniz_oxide` crates that the SVG vectorizer links.

Fixed: a cache purge, and the automatic recovery the web-server module runs
when it cannot open the cache, no longer delete a cache file left by a
previous release's on-disk format. Both cleared every cache file belonging to
this cache whatever format it was written in, so either one silently removed
the volume an operator was keeping in order to roll back. Files from another
format are now left alone -- they are not this build's data to remove -- which
means they stay on disk until you delete them yourself.

Fixed: a URL whose stored variants could no longer be replaced or removed --
the cache accepted the removal and kept serving the same stale entry -- was
served unoptimized while being re-optimized over and over, and never
recovered. The optimizer now clears such a URL's cache entry outright at the
next revalidation, so the URL is optimized again from the fresh origin
response instead of repeating the cycle.

Fixed: the optimizer's systemd unit now starts ahead of `apache2.service` /
`httpd.service` / `nginx.service` and counts as started only once its notify
socket exists
(bounded 30 s wait), so a web server that carries the module directives and
boots on the same host finds the daemon on its first attempt.

Removed: the `/v1/license/*` endpoints (404), the `license` object and
`checks.license` entry in `/v1/health`, and the `x-pagespeed-warn` response
header. Monitoring that keyed on `checks.license.pass` should key on `ready`
/ `status` instead.

Changed: `agent_optimize` (the same-URL markdown variant for AI agents), the
synthesized `/llms.txt` index and the browser-analysis suite are enabled by
their operator flags alone (`--agent-optimize`, `--agent-optimize-llms-txt`,
`--enable-browser-analysis`). The `agent_optimize_entitled` key in
`pagespeed-shared.conf` keeps its name for compatibility with 2.0-era readers
and mirrors the operator flag.

Removed: instance telemetry. The daemon no longer derives a machine id or
writes `pagespeed.instance-id`, and it makes no outbound request of its own
accord; the only egress is what the operator configures (origin fetches, the
Web Bot Auth and RSL-CAP key directories). A `pagespeed.instance-id` file a
2.0 install left behind is left in place and never read.

Changed (host packages): the optimizer daemon's systemd unit now **enforces**
its system-call allow-list by default. Earlier 2.1 release candidates shipped
the unit log-only, with enforcement an opt-in drop-in the operator installed
after a soak; the allow-list — systemd's `@system-service` group minus
`@privileged` and `@resources`, plus `RestrictNamespaces=yes` — now lives in
the unit itself, and the kernel terminates the daemon (`SIGSYS`, exit status
31) on the first call outside it. The list is in the unit rather than a
drop-in on purpose: no drop-in can be installed "alone" any more, which
retires the earlier warning that the browser-analysis file must never be
installed without its companion. The flip rests on the syscall census across
three distributions and the package's own verification, which on every build
runs the real daemon and a real headless Chrome under the enforcing profile
and asserts no kill; the unit keeps `SystemCallLog=~@system-service`, so calls
outside the group are still recorded. What changes on upgrade: the host
enforces from the service restart the package performs on install. Browser
analysis works under it with no extra step: the `20-browser-analysis.conf`
re-admission list for headless Chrome's own sandbox ships installed and
active as a vendor drop-in next to the unit (it only adds to the allow-list);
a host without browser analysis can mask it with an empty same-named file
under `/etc/systemd/system/pagespeed-optimizer.service.d/` to take the
narrower daemon-only profile. Opting out of enforcement: copy
`/usr/share/doc/pagespeed-optimizer/90-syscall-filter-off.conf.example` into
that `.service.d/` directory, `systemctl daemon-reload`, restart — it resets
both directives, and it survives `apt upgrade` and `dnf upgrade` because the
package never writes under `/etc/systemd`. Confirm
either posture with `systemctl show -p SystemCallFilter pagespeed-optimizer`
(a long allow-list when enforcing, `SystemCallFilter=~` when opted out); the
daemon's own `syscall_filter` health field says `"filtered"` in both. A kill
by the profile reads `code=dumped, status=31/SYS` in `systemctl status` (no
dump is written), or `Chrome exited (status=0, signal=31)` in the journal
when the browser was the victim; the kernel's audit record (type `1326`)
names the call by number — `ausearch -m SECCOMP -i` where `auditd` runs,
`journalctl -k | grep 'type=1326'` where it does not (a kill carries `sig=31`;
`sig=0` records are the log filter's entries), `ausyscall <N>` to name a bare number. Update recommended for host
installs.

Fixed: origin refresh no longer false-purges unchanged origins every
freshness cycle (#1503). Both front ends fire the origin-refreshed sentinel
unconditionally on an age-expired re-fetch, and the worker purged the URL's
whole variant set on arrival — and with the identity/original-format slot
holding the worker's own proactive variant (flagged `kFlagWorkerProcessed`),
the inline rebuild was refused, so every TTL expiry cost a purge plus a
deferred re-optimization of identical bytes (on a module-serves front end
that never serves stale, a recurring dead window with nothing optimized to
serve at all). The worker now decides changed-vs-unchanged first, hashing
the pristine origin reference — the durable-original sentinel (`0x0C`) read
by exact id, else a genuine flag-clear identity, never the worker-processed
slot — against the stored content-hash oracle. An unchanged origin with a
freshly re-recorded reference gets its variant set restamped in place from
the fresh origin metadata (no purge, no rebuild), where "fresh" is
discriminated relatively — the re-record postdates the variant set's own
stamps — so an Age-backdated re-record from a CDN-fronted origin is
recognised as the re-record rather than deferred forever; an unchanged
reference
whose re-record has not landed yet (the 1.16 module sends the sentinel ahead
of its re-record) defers the decision to the record+notify that follows,
where the image branch now runs its content-hash check over the pristine
reference as well and restamps on a match. A genuinely changed origin still
purges and rebuilds exactly as before. Text resources (CSS/JS, and HTML
without `agent_optimize`) carry no content-hash oracle and keep the
per-cycle purge — the false-purge fix covers URLs with the oracle (all
images, agent-markdown HTML). New counters: `origin_refresh.unchanged`
and `origin_refresh.deferred` in `/v1/stats`.

Added: `GET /v1/stats` (and the `/v1/ws/stats` stream, which shares the same
builder) now carries a `serve_classes` block whenever the serve-stats mmap is
available: the five-class serve partition (`optimized`, `original_cold`,
`original_pending`, `original_declined`, `original_skew`), the orthogonal
`notify_suppressed` counter, and the `class_unrecognized` /
`flags_unrecognized` drop counters that keep the partition honest. These
counters are written by the serving front end — live where the 1.16 module
(mod_pagespeed) fronts the cache, still all-zero under an nginx 2.0 or
ASP.NET front end, where all-zero means "not instrumented", not "nothing
happened". No struct or ABI change; the block is read from the existing v8
serve-stats layout.

Fixed: `systemctl reload pagespeed-optimizer` terminated the daemon instead
of reloading it. The unit declared no `ExecReload=`, so systemd fell back to
sending SIGHUP to the main process, and the daemon — which installed no
SIGHUP handler — died on it; with `Restart=on-failure` it came straight
back, so every "reload" was an unannounced restart that dropped in-flight
optimizations and reopened the cache volume. Reload has no honest semantics
for this daemon — its settings are fixed at startup, and the hot-reloadable
subset lives behind `PATCH /v1/config` — so the daemon now ignores SIGHUP
and the unit declares an `ExecReload=` that fails loudly and points at
`systemctl restart pagespeed-optimizer`. Update recommended for host
installs.

Fixed: a package upgrade left `/etc/default/pagespeed-optimizer` at 0644
root:root. The file is one of the two `EnvironmentFile`s the optimizer unit
reads, and the design gate for those files is 0640 root:pagespeed (the other,
`/etc/pagespeed-optimizer/daemon.env`, carries the generated API/PURGE tokens
and was already enforced). The gap was the upgrade path: dpkg and rpm replace
an operator-unedited `/etc/default` file with the payload's mode and
ownership, and nothing re-asserted the gate afterwards — fresh installs were
fine, upgrades were not. The packages now ship the file at 0640, the
maintainer scripts re-pin the mode on every install and upgrade (an edited
file keeps its contents; only the mode is re-pinned), and a tmpfiles.d `z`
line re-scopes the group to `pagespeed` on every install, upgrade and boot.
Update recommended for host installs.

Fixed: the companion `20-browser-analysis.conf` drop-in shipped as an
example for deployments that run browser analysis under the opt-in enforcing
syscall profile did not re-admit everything headless Chrome's sandbox needs:
with the 1.16.0-rc.10 copy installed, enforcement killed Chrome at launch and
browser analysis backed off to unavailable. The drop-in has now been measured
against a running Chromium under the packaged service unit and completed; the
enforcing profile itself and the default unit are unchanged. One name arrived
after that measurement: `mincore`, which Chromium's MemoryInfra thread calls
in a burst every few minutes — every shorter observation window
records zero calls, so the miss only surfaced when a burst happened to land
inside a CI render cycle and the enforcing profile killed the browser
process. The name is now re-admitted. Host installs:
the package now suggests a browser and the packaged `/etc/default` file and
the browser-analysis documentation say which package to install and how to
point the daemon at it. Update recommended if you enabled the enforcing
drop-in together with browser analysis.

Fixed: with browser analysis enabled and a persistent profile directory, the
headless browser could not start after the optimizer's container was
recreated (an image upgrade, a `docker compose up` that changed the service,
a host reboot) or after a host's name changed: the profile kept a lock from
the previous browser, every launch exited immediately with status 21, and
browser analysis stayed down until the lock was removed by hand. The
optimizer now removes a lock that belongs to another host or to a process
that is no longer running before each launch (and logs the previous owner;
a profile directory must not be shared between optimizer instances),
names a locked profile in the launch-failure log line instead of only the
exit status, and clears the failure count and retry delay reported under
`browser` in `/v1/health` once a launch has stayed up for a minute. Update
recommended if browser analysis is enabled.
Fixed: the management API serves the web console's static bundle — `GET` and
`HEAD` requests to `/console` and paths under it — without requiring the API
token, matching the documented behavior. The bundle carries no operational
data; every `/v1/*` data endpoint, and every non-GET/HEAD request, still
requires the token when one is configured.

Fixed: `GET /v1/health` and `/v1/stats` report the connection figures of the
management API listener itself in `connections` — the cap it actually enforces
(32 by default) and its live active count — rather than the notification
listener's separate `--max-connections` limit (128 by default).

Fixed: with browser analysis enabled, the 1.16.0-rc.9 container image could
not start the headless browser. Every launch failed immediately and the
optimizer retried it every few seconds, so browser-driven optimizations never
ran and the host accumulated a core dump per attempt. The optimizer now hands
the browser an environment that matches the unprivileged user it runs as, and
a browser that keeps failing at launch is retried with an increasing delay
(up to five minutes) with a single log line naming the condition; `/v1/health`
reports the failure count and the current retry delay under `browser`. A
latent fault in the browser-exit path that could stop the optimizer itself
after a browser exit is closed as well. Affects 1.16.0-rc.9 only. Update
recommended if browser analysis is enabled.

Security: the optimizer daemon's management API is no longer
open by omission, and headless Chrome no longer runs unsandboxed. Two
changes, both with an operator-visible default flip.

The management API now enforces one rule while parsing its configuration:
remote is never unauthenticated, and unauthenticated is never remote. Any
combination that breaks it is a refusal to start, with a message naming the
flag that would make the requested posture legal. Previously an unset token
made every endpoint answer without a credential -- including cache purge and
the cached-URL inventory -- and an unset token also silently opened all read
endpoints. Both are closed: an absent token now fails closed, and
`--api-read-open` is an explicit choice rather than a side effect of a
credential someone forgot. A new `--api-socket` serves the same HTTP API over
a group-scoped unix socket (mode 0660) as the recommended local transport: a
connection accepted there is an authenticated peer, because reaching the
socket already means being in the daemon's group, so no token is asked for on
reads or on cache purge and the daemon binds no TCP port at all. A TCP bind
stays available and is loopback-only unless `--api-allow-remote` and a token
are both set; the two transports are mutually exclusive, and `--api-bind`
takes an IPv4 literal (hostnames and IPv6 are refused by name at startup). Package installs now generate
`PAGESPEED_API_TOKEN` and `PAGESPEED_PURGE_TOKEN` into
`/etc/pagespeed-optimizer/daemon.env` (0640 `root:pagespeed`) once, at install
time, never overwriting an existing value -- so cache purge over the
management socket works on a stock install instead of answering "no purge
token configured", and enabling the API does not require inventing a
credential. Update recommended; review your configuration if you exposed the
management API.

Headless Chrome now runs with its own sandbox. `--no-sandbox` was previously
passed unconditionally, so every headless render of untrusted page content ran
without kernel-enforced isolation -- a review item open in this codebase since
2024. A new `--browser-sandbox` takes `require` (the default) or `off`. In
`require` mode the daemon probes at startup: if the sandbox is available Chrome
runs sandboxed, and if it is not, browser analysis refuses to initialize with
one message naming the cause and the remedy while the daemon keeps serving
without it. It never falls back to an unsandboxed browser, and there is
deliberately no mode that does. `off` is the documented opt-out and is loud:
a warning at startup and on every browser start. `GET /v1/health` and
`/v1/stats` report the resolved state as `browser_sandbox` (`"on"`,
`"unavailable"`, `"off"`, `"disabled"`), so a monitoring check can see an
unsandboxed browser without reading logs. Chrome's profile directory is also
now pinned by the daemon (0700, inside the service runtime directory) instead
of left to Chrome to choose.

Two follow-ups to that gate make its diagnostics honest. When the browser
sandbox probe is refused, the daemon now distinguishes a kernel syscall filter
denying it from a probe that simply did not answer in time -- previously both
reported the same "did not complete", which sent operators looking at the wrong
thing. The syscall-filter case names the kind of setting that produces it and
points at the browser-analysis documentation, so it is actionable rather than a
dead end. And `GET /v1/health` and `/v1/stats` now report `syscall_filter`
(`"filtered"`, `"none"`, `"unknown"`), read from the kernel's view of the
running process: a monitoring check can see whether a deployment is running
under a kernel syscall filter without reading a unit file on the host. It is
read-only reporting and not a setting -- the daemon does not install the filter
itself. Note that `"filtered"` means *a* filter is attached, not that any
particular profile is: a container started with your runtime's default seccomp
profile already reports `"filtered"`, and so does a stock package install on a
host, because the unit below attaches one.

The optimizer daemon's systemd unit now ships a hardened profile. The kernel
and host surfaces the daemon never needs are closed off -- kernel tunables,
modules and logs, control groups, the system clock, the hostname, other users'
processes, SUID/SGID bits, realtime scheduling and personality changes -- the
address families it may use are restricted to the four it actually uses, and
core dumps are disabled, because a dump of this process would contain cache
contents and credentials. Syscall filtering ships in **log-only** mode: the
unit records every system call outside systemd's normal system-service set and
lets it proceed, so an operator can read a full traffic cycle's worth of
evidence out of the audit log before turning on enforcement (where those
records land depends on whether an audit daemon is running -- the packaged
documentation gives both reads and the check that decides which applies). Enforcement is then a
drop-in the package installs as an example, plus a companion drop-in for
deployments that run browser analysis, meant to re-admit what headless
Chrome's own sandbox needs (the rc.10 copy was incomplete; see the entry
above). Install the first without the second and the daemon still
starts and still serves; browser analysis refuses to initialize and names the
restriction, rather than falling back to an unsandboxed browser.

To be precise about what changes on upgrade: **no system call is killed by
default** -- the enforcing profile is opt-in and arrives only with that
drop-in. (Superseded within this release: enforcement is now the default and
the opt-in drop-in is retired -- see the "Changed (host packages)" entry at
the top of this section.) The rest of the hardening above does ship enabled,
and is a behaviour change. The daemon can now open only unix, netlink and IPv4/IPv6
sockets; it can no longer write kernel tunables, load modules, read the
kernel log, change the system clock or hostname, acquire SUID/SGID bits or
realtime scheduling; it cannot see other users' processes in `/proc`; and it
writes no core dumps. If you depend on one of those -- a core dump for crash
triage is the realistic case -- restore it with a drop-in of your own.
Containers are covered from the other side, since a container has no systemd
and the unit profile does not apply there at all: the package now ships a
Chrome-compatible container seccomp profile. Update recommended.

Breaking for container users, 2.0 -> 2.1 (migration notes):

- **Helm pod seccomp profile.** Was: `podSecurityContext` was empty, so pods
  inherited whatever the cluster defaulted to -- on many clusters that is
  seccomp *unconfined*, which is weaker than what a plain `docker run` gives
  you. Now: the chart sets `seccompProfile.type: RuntimeDefault`. If a
  workload of yours needs a syscall the runtime's default profile blocks, opt
  out with `podSecurityContext: {seccompProfile: null}` in your values --
  Helm deep-merges values, so setting the key to `null` is what removes it;
  an empty `podSecurityContext: {}` will NOT override the chart default.

- **Management API.** Was: the container published the API on `0.0.0.0`, and
  the token was optional -- when unset, every endpoint including cache purge
  answered without a credential. Now: the image defaults to a loopback bind, a
  token is required, and a non-loopback bind refuses to start without both
  `PAGESPEED_API_ALLOW_REMOTE=true` and a token. If you deliberately published
  the API, set both and put it behind your reverse proxy's own authentication
  as well. When the API is enabled and you supply no token, the entrypoint now
  generates one per container start and prints it once to the container log —
  along with a generated PURGE token, so cache invalidation works out of the
  box. **Both are printed to stdout: if you ship container logs off-host, you
  are shipping those credentials with them.** Set `PAGESPEED_API_TOKEN` and
  `PAGESPEED_PURGE_TOKEN` yourself to keep them out of the log and stable
  across restarts.
- **Browser sandbox.** Chrome declines to sandbox itself as root. The images no
  longer run the optimizer as root (see below), so the `require` default is now
  satisfiable in a container -- but the container runtime's default seccomp
  profile also has to permit user namespaces. Where it does not, browser
  analysis refuses to start and `/v1/health` reports
  `browser_sandbox: "unavailable"` naming the cause; the daemon keeps serving.
  Run with a Chrome-compatible seccomp profile, or set
  `PAGESPEED_BROWSER_SANDBOX=off` to accept an unsandboxed browser deliberately.
- **Shared cache volume.** The worker and nginx images now share the cache
  through group `pagespeed` at a fixed **GID 918**, present in both, instead of
  making the volume world-writable. The optimizer in the worker and combined
  images runs as the unprivileged `pagespeed` user (UID 918); nginx's worker
  processes join the group. `/data` is 2750 -- readable and traversable by the
  group, writable by nobody but the optimizer, because the serving module only
  ever opens files the optimizer authored and never creates any. The cache
  volume and sockets are 0660, and nothing on the volume is world-readable or
  world-writable any more. The optimizer also starts with no capabilities and
  with new privileges disabled, so the drop cannot be walked back.

  Existing deployments do not need to change anything -- the entrypoint still
  starts as root, prepares the volume and drops privileges itself -- but three
  cases need attention. If you run the worker with an explicit `user:` /
  `--user`, use `918:918`, and make sure a bind-mounted data directory is owned
  by it. On Kubernetes set `securityContext.fsGroup: 918`. If you mount the data
  directory into a *third* container that reads the cache, add `918` to that
  container's groups.

  A volume created before this release is migrated in place on first start: the
  entrypoint takes ownership of the cache files the optimizer authors, logs each
  one, and keeps the existing cache rather than abandoning it and cold-starting.
  Only root-owned cache content is migrated -- that is the previous release's
  optimizer, the one identity whose authorship is not in question; content owned
  by any other user is refused rather than adopted. Where the entrypoint cannot
  take ownership safely it refuses **before** the optimizer starts, exits 78,
  names the path, and prints the one-line remedy -- rather than exiting into a
  restart loop whose only symptom is that the web server never starts -- and
  leaves the data directory unreadable, so nothing can reach the volume while it
  is in that state. Files already taken over before the refusal keep their new
  ownership; the printed remedy is safe to run over them. Exit 78 always means
  the configuration or the volume rather than a transient fault, which is worth
  distinguishing in restart alerting. Set
  `PAGESPEED_ADOPT_VOLUME=off` to opt out of the in-place migration and be told
  what to fix by hand instead.

Package installs are unaffected by both: the API ships disabled, and browser
analysis is off by default.

Security: the packaged optimizer daemon no longer runs
as root, and nothing it creates is world-accessible anymore. The
`pagespeed-optimizer` deb/rpm now creates a dedicated unprivileged
`pagespeed` system user (via sysusers.d), runs the daemon as
`User=pagespeed` with an empty capability set, and moves the cache into a
versioned directory `/var/cache/pagespeed-optimizer/v1` (3770 setgid+sticky,
`pagespeed:pagespeed`, tmpfiles.d). The cache volume, notify/health
sockets, and serve-stats mmap are now explicitly 0660 owner+group (were
world-writable 0666/0777 under the old `UMask=0000` unit), and
`pagespeed-shared.conf` is 0640 (was world-readable). The serving module's
web-server user reaches everything through membership in group `pagespeed`
(the module packages perform that join). Upgrades cold-start the daemon
cache by design: no installer chowns or migrates pre-existing root-owned
cache content, and the postinst prints a one-line notice when it detects
one. The daemon refuses to start — loudly, naming the cause and the
migration doc — when its cache directory is missing, unwritable, or holds
content owned by another uid. Also new: `--cache-dir` (default
`/var/cache/pagespeed-optimizer/v1`; `--cache-path` remains as the expert
override), a `cache_dir_generation` field published in the shared config
(C API 1.10: `ps_read_shared_config_generation`) so a
module/daemon generation skew fails loudly, and a secrets env file
`/etc/pagespeed-optimizer/daemon.env` (0640 `root:pagespeed`) so
`--api-token` no longer rides the process command line, where it was exposed
to other local accounts on the host. Update recommended.

Action required on upgrade: the daemon's default cache and socket paths move
with this change, but your web server keeps whatever path you configured.
After upgrading, point `pagespeed_cache_path` (nginx) or
`ModPagespeedDaemonVolumePath` / `ModPagespeedDaemonSocketPath` at
`/var/cache/pagespeed-optimizer/v1/cache` and
`/run/pagespeed-optimizer/notify.sock`, then restart the web server. Until you
do, in-place optimization stays off and the log reports the socket as absent
even though the daemon is running — the module is still looking at the old
location. The upgrade prints the same reminder.

Fixed: the JavaScript minifier now handles an object literal that spreads the
result of a call. In `{...f(), k: v}` -- a shape that bundler output and
analytics trackers produce routinely -- the spread argument was read as if it
were a property name, so the call's parenthesis was taken for a method
shorthand's parameter list and the comma that follows had nothing to close.
Minification of the file then failed and the script was served exactly as
received -- no optimized variant was written. Spread of a call is now
recognized as a value, so these files minify normally.
One deliberate change on invalid input: `{...a: 1}` -- a spread element cannot
take a property colon -- is now refused instead of tolerated, which leaves the
script byte-for-byte as received. Output for all other valid JavaScript is
unchanged. Affects all earlier releases. Update recommended if you serve
JavaScript through the optimizer daemon or the middleware.

Fixed: the JavaScript minifier no longer drops a required line break after a
`let` declaration whose binding is preceded by an HTML-style comment. `<!--`
opens a comment anywhere, and `-->` opens one at the start of a line; with
either between `let` and the name it binds, the declaration was misread as an
ordinary identifier and the line break that ends it was removed. Where that
line break was the only thing separating the declaration from the next
statement, the result no longer parsed -- and because a script is parsed as a
whole, the failure took the entire file down, not just the statement. Both
comment forms are now recognized, so the line break is preserved. (2.0.41
fixed the same misreading for `//` and `/* */` comments; the HTML comment forms
remained.) These forms are rare in modern JavaScript, and output for all other
valid JavaScript is unchanged. Affects all earlier releases. Update recommended
if you serve JavaScript through the optimizer daemon or the middleware.

Fixed: a serving module or middleware that opens the optimizer daemon's cache
volume as a member of the daemon's group — the default arrangement after the
privilege drop above — was refused with an I/O error and ran with in-place
optimization off for the life of every worker process, while still reporting
itself healthy, so a site could serve entirely unoptimized responses with no
failing check to show for it. Only the process that created the volume can set
its mode, so a peer was being refused for not changing a mode that was already
correct. The client now accepts a volume that already carries the expected
mode. It still refuses a volume whose mode is wrong, one reached through a
symlink, and — new — one that a second name is hard-linked to. Affects the
1.16.0-rc.7 package pair; update recommended.

Fixed: the ASP.NET Core host now provisions its own cache directory, so the
worker it starts comes up on a machine where the optimizer was never
installed from a package. The daemon validates its cache directory at
startup and refuses to start when the directory is absent — the right
behaviour for the packaged service, whose directory is created ahead of it,
but the NuGet package has no such install step. The host was relying on that
directory happening to be created first by the request-side cache, so
whichever of the two ran first decided whether the worker survived startup:
the worker came up on one run and exited on the next, and when it exited,
optimization stayed off and the worker's local API never answered. The
directory is now created before the worker is started, owner-only, at
whatever location `Cache.VolumePath` selects; a directory that already
exists is left exactly as it is. No configuration change is required.

Fixed: image optimization for a lossy WebP source on a desktop-class request
no longer decodes the source image twice when only the original-format
variant is served. The optimization pass now reuses the decode it already
performed, halving the decode work on that path; optimized output is
unchanged. (#1407)

Fixed: a URL whose optimized variants were evicted from the cache while the
optimizer still remembered processing them is no longer skipped forever.
Previously, every renotification for such a URL was dropped as "already
processed" without checking that anything servable remained, so the front
end kept falling through to the unoptimized original with no path back.
The optimizer now validates a dedup hit against the cache and, when
nothing servable remains for the URL, forgets the stale entry and
reoptimizes; these recoveries are visible in a new `dedup_healed`
counter on the stats endpoints. A family with nothing servable is not
always a lost one, though: several paths record a terminal decision
while writing nothing servable (a deterministic CSS or JS minify
failure, an integrity-pinned URL, an oversized image), and a decline
tombstone is written when ANY slot declines, so its presence alone does
not prove a family is empty by decision. The reoptimization is therefore
bounded to at most once per URL per ten seconds rather than forbidden
outright: a changed source is still picked up, while a terminal decision
cannot turn into a per-request re-optimization loop. Deferred heals are
visible in a new `dedup_heal_rate_limited` counter on the stats
endpoints.

Fixed: an origin-content refresh can no longer rebuild a URL's optimized
variants from superseded bytes. When the refreshed origin had not (yet)
been recorded at the default cache slot, the refresh handling could keep
the optimizer's own previous output there and reuse it as the source for
the rebuilt variants, republishing stale content as freshly optimized.
The refresh now keeps that slot only when it holds a genuine origin
recording, and otherwise defers the rebuild until the fresh content
arrives; these deferrals are visible in a new
`origin_refresh_rebuild_refused` counter on the stats endpoints.

Added: a Debian package for the standalone optimizer daemon
(pagespeed-optimizer). tools/packaging/build-optimizer-deb.sh wraps a built
worker binary into a versioned .deb carrying a hardened systemd unit, an
/etc/default/pagespeed-optimizer conffile for overrides, and maintainer
scripts that enable and start the service on install, stop it on removal,
and remove its state directory only on purge. The package also ships the
C API client library (libpagespeed.so) in the multiarch libdir with an
ldconfig refresh, so a serving module can bind the daemon by soname out of
the box, and the unit's cache path is an extensionless stem the serving
module can actually locate (the daemon derives the physical volume name by
inserting a generation/geometry suffix before any extension) — without
either, module integrations silently fell through to serving unoptimized.
The package depends only on glibc (floor derived as the max
across both shipped ELF objects); the C++ runtime is linked statically by
the build's release toolchain. An RPM sibling
(tools/packaging/build-optimizer-rpm.sh) packages the same payload for
EL hosts with the same derived floor, version tripwire, and self-test
discipline. (#1392, #1399)

Fixed: building the worker from source with a host GCC toolchain now works.
The switch-fallthrough annotation degraded to a no-op outside clang (GCC then
correctly refused the unannotated fallthrough); two locals crossing setjmp in
the PNG/JPEG conversion paths were not volatile, so their values after a
libpng/libjpeg error longjmp were formally indeterminate; and three
const-qualified scalar casts tripped -Werror=ignored-qualifiers. All are
fixed for both toolchains; release binaries were unaffected. (#1388)

Fixed: on resized viewports, the original-format image slot for a lossy
WebP source is now re-encoded at the viewport dimensions using the
request's own quality settings (Save-Data reductions included), instead
of a second full-size re-encode of the origin under the resized variant
id -- and that re-encode is bound to the same quality-verification
verdict as every other path: a candidate that fails verification is
declined and the original bytes are served. PNG keeps its full-size
stream optimization (re-encoding would un-palette palettized sources)
and GIF is carried verbatim; lossless WebP sources remain untouched.
(#1380)

Fixed: a failure inside the quality-verification metric itself is no longer
recorded as a catastrophic quality measurement. Metric errors are now reported
on their own channel, so an affected variant is declined as unmeasurable on
every format arm instead of shipping without a verdict, and a decline no
longer cites a score that was never produced. (#1382)

Fixed: the decoded-size limit for monochrome AVIF images now accounts for the
full-size color conversion performed during decoding, so an image sized just
under the limit no longer transiently allocates about three times that limit.
(#1382)

Fixed: an image variant refused by quality verification was also counted as
skipped-for-no-savings when its original-format slot stayed empty, overstating
both counters; the refusal is now counted only once. (#1382)

Added: variants refused by quality verification are now remembered per source
image, so repeated cache notifications for an image no longer re-run the full
optimization attempt ladder for a variant that was already refused and never
stored. The skip is visible in a new `ssimulacra2_decline_tombstone_hits`
counter, and a changed source image retries its variants immediately. (#1382)

Fixed: re-optimizing a lossy WebP image in its own format is now bound to
the same quality-verification verdict as the format conversions. This
path previously accepted its result on the size test alone, so a
re-encode that failed verification -- including catastrophically low
scores -- could replace the original; it is now declined and the original
bytes are served instead. Lossless WebP sources remain untouched, and
JPEG's accept-at-the-cap behavior is unchanged. Update recommended where
quality verification is enabled. (#1385)

Fixed: image decoding no longer terminates the process on input it
cannot decode. A truncated PNG — or any undecodable bytes offered to a
format's decoder — now fails cleanly and is rejected instead of crashing
the worker. Update recommended: any deployment that serves images from
untrusted sources was exposed to process termination on malformed input.
(#1371)

Fixed: a stored image variant is now bound to its quality-verification
verdict. When the optimizer's SSIMULACRA2 check measures a WebP or AVIF
candidate below the configured acceptance floor — including
catastrophically low scores, which are legitimate verdicts — the variant
is declined instead of stored, and serving falls back to the remaining
variants and the original. A candidate whose quality cannot be measured
at all (a failed decode-back, or a pixel layout not comparable with the
source) is declined on every format arm rather than shipped unverified —
previously this fired for every monochrome image. Among competing encode
attempts, the one closest to the acceptance band is the one stored, and
the shipped attempt's encoder setting is what gets recorded. Declines
are counted in a new `ssimulacra2_declines` counter — a decision, not an
error. (#1381)

Fixed: an image whose pixel decode fails — some GIFs, for example — is no
longer stored only under a format-specific variant it was never converted to.
Its bytes now go to the original-format variant, which every client can select,
so such an image is served from cache instead of falling through to the origin's
copy for every client that does not advertise that format.

A counter, `image_unconverted_fallthrough`, now records each such refusal —
"there was nothing to convert" was previously indistinguishable from "the
optimizer ran and could not win", which is why this went unnoticed. (#1374)

Fixed: lossy WebP images are optimized rather than passed through. A WebP
origin encoded above the quality this pipeline targets was previously served as
it arrived, on the assumption that a WebP is already optimal; it is not, and
such an image now re-encodes. The result is kept only when it is strictly
smaller than the origin — the same rule JPEG and PNG already get — so an image
the encoder cannot beat is still served untouched, and a *lossless* (VP8L)
origin keeps the old passthrough, because every WebP encoder here is lossy and
a size gate cannot see a fidelity change. Lossy images that merely carry an
alpha channel are optimized like any other.

Reachability, stated precisely: the optimized bytes are stored at the variant a
WebP-negotiating client selects, so that is who receives them. A client whose
`Accept` names AVIF negotiates to the AVIF variant, finds none for a WebP
origin, and is served the origin's own bytes rather than the smaller WebP
sibling it also accepts — the selection inversion tracked in #1376, which is
where the remaining gap lives. (#1375)

Fixed: a request that sends no `Accept` header is now classified as `*/*`,
per RFC 9110 §12.5.1. It was the only request shape that resolved differently
from the wildcard it stands for, and the effect was that such a client was
served the original of every resource whose optimized variants all sit at
converted formats, instead of an optimized one. (#1377)

Fixed: a stylesheet with an invalid value in a complete longhand family no
longer loses the whole family through minification. CSS drops one declaration
for an invalid longhand but every declaration the shorthand would have set for
an invalid shorthand, so collapsing `padding-top/right/bottom/left` around a
typo — `padding-right:red`, a mistyped unit, a malformed `calc()`, a colour
that is not one, an unknown `overflow` keyword — turned one ignored declaration
into the loss of all of them. The collapse now checks each member value against
what that longhand accepts and declines the collapse otherwise, so the typo
costs exactly what it costs a browser. The check is per longhand, so a negative
`padding` or `border-width` — invalid, and previously collapsed — is declined
too, while a negative `margin` still collapses.

This is not free on well-formed CSS: a family the check cannot positively
recognise is left as longhands, which costs a few bytes. The complete list of
value shapes that no longer collapse is: math and environment functions
(`calc()`, `min()`, `max()`, `clamp()`, `env()`); `url()` values; colour
functions other than `rgb()`/`rgba()`/`hsl()`/`hsla()`, such as `color-mix()`
and `oklch()`; the slash-alpha (`rgb(0 0 0 / 50%)`) and `<angle>`-hue
(`hsl(120deg …)`) colour forms; the modern space-separated colour form
(`rgb(255 0 0)`); and numbers in scientific notation. Plain
`rgb()`/`rgba()`/`hsl()`/`hsla()` colours — with a well-formed comma-separated
argument list — and container-query units (`cqw`, `cqi`, …) do collapse. (#1378)

Workbench: the operator surface now names and explains the three
notify-rejection counters added by PR #1314 — `rejected_version`
(version skew between components), `rejected_malformed` (unparseable
notifications), and `rejected_sentinel` (reserved sentinel name). The
metrics page shows human-readable descriptions for all six notification
counters.

Fixed: ASP.NET Core middleware read-back now honors the true origin-revalidation
signal via `ReadResult.OriginCcFlags` (PS_CC_ORIGIN_* bitfield). The middleware
write path persists origin Cache-Control state using `ps_parse_cache_control`, so
cache entries carry the correct revalidation requirement and aggressive-mode
`stale-if-error` is properly suppressed when the origin requires revalidation.
For a private cache (in-process middleware), only `no-cache` and `must-revalidate`
trigger revalidation; `proxy-revalidate` and `s-maxage` are ignored per RFC 9111
§5.2.2.9-10. (#772)

Fixed: `ps_cache_stats` now fills only the caller-declared struct prefix and
reports the bytes written in `out->struct_size`. An out struct whose
`struct_size` is unset or smaller than the size of the first field is rejected
with `PS_ERR_INVALID_ARG`. Previously the call cleared and filled the
library's own size of the struct regardless of the caller's — harmless while
caller and library share the same header, but an overrun for a caller built
against a smaller, older definition of the struct once the library's grows.
(#1304)

Changed: the nginx serve path now re-validates a cache HIT's selected image
variant against the request's negotiated capability mask immediately before
serving, as a second, independent layer behind variant selection. Selection
already refuses a format-incompatible variant (the WebP/AVIF fix above), so
this check changes nothing a correctly-functioning selector hands down: a
stored Original remains the universal fallback, stored SVG remains universal,
and an exact format match serves exactly as before. Should a selector
regression or a differently-scored peer ever hand down a WebP or AVIF variant
the request's mask does not name, the variant is now refused at the seam —
the request falls through to re-optimization exactly as on a cache miss, and
a WARN log names the URL with the stored and request masks. No counter was
added: the shared stats file's layout is version-pinned and its reserved
anomaly counters belong to a different writer, and this event should never
occur.

Added: the C API publishes `PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE`, a
stored-entry flag a front end sets while recording a response whose headers
include something no later serve can reproduce. Until now that knowledge was
lost at the moment it was learned: the question can only be answered with the
origin's complete header set in hand, and on a cache hit those headers are
gone. A serve path was therefore left with two options, and no way to tell
which one it was in — answer from the entry and drop headers the origin sent,
silently, or give up optimizing the whole class.

With the flag on the entry it has a third: serve optimized where the headers
are reproducible, which is the large majority of responses, and fall through to
the plain response where they are not. Falling through is visible and
measurable; dropping a header is neither. `ps_headers_sidecar_classify` already
answers the question a writer sets the flag from, so no new classification is
involved.

The flag is advisory. It records what the origin sent and gates nothing: a
marked response is stored, scored, optimized and expired exactly as an unmarked
one is, and a front end that does not set it behaves exactly as before. It sits
on the entry the writer stamped — the optimizer's own variants are written with
their own flags and do not carry it — so a serve path reads it back from that
entry by id.

One effect reaches the wire, for a front end that serves marked entries: the
weak `ETag` of a hit covers the flag, so a marked entry does not revalidate
against an unmarked one, which is correct because the two are not served the
same way. Nothing in the shipped engine serves marked entries today, so no
existing deployment sees it. API version 1.7 → 1.8; additions only, so existing
callers and binaries are unaffected.

Fixed: a request whose `Accept` header advertised no image format at all could
be answered with a WebP or AVIF variant it never said it could decode. Variant
selection hard-disqualified a transfer-encoding mismatch but treated an
image-format mismatch as merely a low score — and a family holding only WebP
and AVIF variants (exactly what a resource stored as a durable original
produces, since the stored original itself is never selectable) had no
format-compatible member to outscore them. Worse, the WebP and AVIF entries
tied at exactly the same score, so which undecodable format the client
received depended on cache listing order.

Format mismatches are now disqualified the same way encoding mismatches are: a
stored WebP or AVIF variant is served only to a request whose negotiated mask
names that format, with the original format remaining the universal fallback
and SVG — which the optimizer only ever stores for content it produced itself
— unchanged. When no variant in the family is compatible, selection returns
nothing and the request falls through to re-optimization, the same as a cache
miss. The disqualify is deliberately strict in one direction: a client that
sent `image/*` or `*/*` and could in fact decode a stored AVIF may now miss
the cache where it previously hit, because the negotiated mask cannot prove
that acceptance — a re-optimization is the safe cost, undecodable bytes were
not. Equal-scoring alternates also no longer resolve by listing order: ties
break toward the system's own format preference (AVIF over WebP over
Original, the order `Accept` negotiation itself uses), then toward the
numerically larger variant id, so the same family answers every listing order
the same way.

Fixed: a front end that stores a response as a durable original — the storage
class holding the bytes the origin sent — and then notifies the optimizer now
gets that resource optimized. Until now it got nothing, and got it silently: the
notification was accepted, the optimizer looked for something to work on, found
nothing it was allowed to use, and returned. Nothing errored and nothing was
logged that distinguished it from a resource with no cache entry at all, so what
an operator saw was a cache filling up and an optimizer that never produced
anything.

The cause was the interaction of two behaviours that are each correct. Stored
originals are never returned by ordinary cache selection — that is the guarantee
that keeps a stored copy of the origin's bytes from ever being served in place
of an optimized variant — and the optimizer was looking for its input through
that same selection. It now asks for the stored original deliberately, by name,
which is how the class was always meant to be read, and only after ordinary
selection has found nothing usable. Anything already being optimized is
optimized from the same input as before; nothing that was preferred as an input
is preferred any less.

The guarantee itself is unchanged and unchanged in scope: no request, of any
shape, is answered with a stored original by ordinary selection, before or after
the optimizer has used one as input. Nor does using one as input change how long
its bytes may be used: an optimized variant built from a stored original carries
that original's origin caching state verbatim, so it goes stale exactly when the
original would have. The existing refusal to optimize a resource whose origin
negotiates on `Accept` also still applies, and now reads that marker off a
stored original as well, so the refusal cannot be escaped by storing one.

What this covers is the resource the notification names, and the sub-resources
optimizing a page pulls in — a page's external stylesheets, `@import` targets,
and the images it measures — are NOT covered: those still resolve through
ordinary selection only. On a front end that stores nothing but durable
originals, a page's external stylesheet therefore reads as absent until that
stylesheet is itself recorded and notified, and until then the page is
re-optimized on its cooldown rather than settling. It does settle once each
sub-resource has been through the optimizer, because the optimizer's own output
is an ordinary cache entry. Widening the remaining reads is a separate change.

The statistics surface gains `source_reads_from_durable_original`, which counts
the inputs that came from the stored original rather than from ordinary
selection. It reads zero on a deployment whose front end stores ordinary cache
entries, and tracks the notification rate on one that stores durable originals —
which is the distinction the logs could not previously make.

Fixed: re-recording a sentinel entry — the content hash rewritten when a page's
origin bytes change, the `/llms.txt` pair refreshed on its TTL, the agent
markdown variant re-rendered on refresh — no longer leaves the superseded copy
linked under the same id. Every such refresh used to add one node to the URL's
alternate chain while reads kept finding the newest, so nothing looked wrong
until the chain reached the storage layer's traversal ceiling — at which point
the whole key stopped accepting alternate writes and the URL silently lost every
cache class, not just the one that grew. A re-record now unlinks the previous
entry before writing its replacement, so a single writer per URL keeps one node;
concurrent writers for the same URL and id can still accumulate, bounded by the
chain ceiling and made visible in the log, until a later storage version makes
replacement unconditional. One consequence of replacing rather than adding: the
previous entry is removed when the write begins and the new one exists when it
completes, so a write abandoned in between leaves the URL without that entry
until the next store — which reads as an ordinary miss, never as wrong bytes.

Added: the C API publishes `PS_FLAG_ORIGIN_VARIES_ACCEPT`, the stored-entry flag
that records "the origin negotiates on `Accept` itself", together with
`ps_vary_varies_accept` — the predicate that answers whether a response's `Vary`
calls for it. A writer storing a durable original MUST set the flag when that
predicate says so. The optimizer refuses to derive variants from an entry
carrying it, and refusing is the only correct outcome: the stored copy is
whichever representation the first requester's `Accept` elicited, so a variant
family derived from it would be served to clients that asked for something else.
The flag is the entire record of that decision, because on a cache hit the
origin's own `Vary` is gone.

This closes a gap rather than adding an option. The obligation existed and the
optimizer already honoured the flag, but there was no published constant to set
and no exported way to compute it, so a writer using only documented API could
not comply — and the omission is silent, which is why it is called out here
rather than left to the reference. `ps_vary_varies_accept` is the second half of
the same verdict `ps_vary_uncacheable` already answered; both classify one value
once, so they cannot disagree. API version 1.6 → 1.7; additions only, so
existing callers and binaries are unaffected.

Added: the optimizer now publishes the size it opened its cache volume with, so
that another process opening the same volume can inherit that number instead of
assuming one. The volume's on-disk filename is derived from its geometry, which
means a second process opening the same directory with a different size does not
collide with the optimizer and does not fail — it quietly creates and uses a
different file, shares nothing, and runs a permanently cold cache, with no error
on either side. The only way to be sure two processes are looking at the same
cache is for the second one to be told the number rather than guess it.

The size appears in the shared configuration file the optimizer already writes,
and the C API exposes it as `ps_read_shared_config_volume_size`. A caller that
gets 0 back is being told the size is not known — no configuration file, an
unreadable one, or one written by an older optimizer — and the documented
answer to 0 is to decline to open the volume, not to substitute a default,
because substituting a default is exactly what produces the second cold cache.
Nothing existing reads the new value: the bundled front end resolves the file a
different way and is unaffected.

The shared configuration's schema version deliberately did not move. Its version
gate is a reader-side "I cannot read this at all" — a reader seeing a higher
version discards the entire file and falls back to built-in defaults for every
setting in it — and unknown keys are ignored by design, so every already
installed reader keeps working unchanged. Raising the version to announce an
optional key would have degraded all of them in exchange for announcing
something none of them consult.

The C API also gains `ps_cache_config_init_sized`, which fills the cache
configuration structure while writing no byte past the size the caller states it
owns. The existing `ps_cache_config_init` is not size-aware — it writes the
structure as *this* library defines it, which overruns a caller compiled against
an older, shorter definition before that caller can check anything. Callers that
may meet a newer library should prefer the sized form; `ps_cache_config_init` is
unchanged and keeps working for callers whose definition matches.


Added: the notification protocol and the C API now carry the options a request
resolved to. A module in front of the optimizer resolves its own configuration
per request — globals, per-host, per-directory, per-request overrides — and
until now none of that reached here at all.

A notification may now carry that resolved configuration as a value, together
with a signature over it. The value is carried, never interpreted: nothing in
the optimizer merges configuration, inherits from it, or reads policy out of it.
The signature is checked byte for byte against the value it arrived with, on
every notification that carries one. That check is what it is for: a sender and
this optimizer that have drifted apart in how they render a configuration find
out on the first notification rather than never.

What the optimizer then does with an accepted context is worth stating plainly,
because it is less than the field's name suggests. The work is done and stored
under the default context, whatever context named it. That is not a shortcut. On
this surface optimized output is a function of the resource and of the request —
the client's capabilities, the content type, whether the request was an entitled
agent request — plus this optimizer's own configuration, which a notification
cannot reach. No value a module resolves per request reaches a rewriter here, so
two requests that resolved to two different configurations cannot produce two
different optimized artifacts, and one stored artifact is the correct answer for
both of them. Splitting the cache by a name that separates nothing would turn
one warm cache into several cold ones and buy nothing. Requests that supply no
options context, which is every deployment shipping today, are unaffected in
every respect and key byte for byte as they always have.

An options context that arrives from a format this release does not read, with a
malformed signature, or with a signature that does not match its own contents is
refused: the notification is dropped before anything is read or written, the
resource it named is served without optimization that time round, and the
refusal is counted in `rejected_option_context` and reported with a rate-limited
log line naming which of those it was. An oversized payload is refused a layer
earlier, as a frame this release cannot accept, so it is counted in
`rejected_malformed` rather than with the three above — and the sending side
refuses to put one on the wire in the first place.

The statistics and metrics endpoints carry the other half too:
`accepted_non_default_option_context` counts notifications accepted while naming
a configuration other than the default one, which is how a deployment sees that
its front end is resolving and sending contexts at all, and where that work is
landing.

The notification protocol version moved with the field. Version skew between a
module and this optimizer therefore behaves the way it always has: a
notification from a peer speaking another version is refused whole and counted,
the resource it named is served without optimization that time round, and
nothing is written. That is deliberately the outcome rather than reading the
part of the message that looks familiar, which would mean acting on fields whose
meaning is guesswork.

The C API gains `ps_notify_worker_ex`, which takes every parameter
`ps_notify_worker` takes plus the agent-request bit and the options context, and
`ps_option_context_signature` for computing a signature over a canonical
payload. `ps_notify_worker` is unchanged and keeps working. Exporting the
signature function is deliberate: every consumer that declares an options
context has to produce byte-identical signatures for byte-identical payloads
indefinitely, and a consumer that reimplements it and drifts by one byte has
every notification it sends refused.

Fixed: a resource whose origin negotiates on `Accept` is now cached and served
faithfully, and is excluded from optimization. An origin that answers a
request with `Vary: Accept` is telling every cache in front of it that IT
chooses the representation from what the client asked for. Until now that
declaration was accepted for storage and then ignored twice over.

It was ignored on the way in. The response was cached and also queued for
optimization, and the optimizer worked from whichever representation the
FIRST visitor's `Accept` happened to elicit from the origin. Everything
derived from it was therefore derived from one arbitrary visitor's
negotiation result, and later visitors — including ones whose `Accept` would
have asked the origin for something else — were served from that capture.
Such a response is now stored exactly as the origin sent it and is never
handed to the optimizer, the same treatment a response marked `no-transform`
already received. The entry records the reason, so a later request can tell a
resource that was deliberately left alone from one that has not been optimized
yet. Note the scope: this affects only resources the origin itself declares as
Accept-negotiated. Ordinary resources are optimized exactly as before, and
image format selection on our own side is unchanged.

It was ignored on the way out. A response served from cache is rebuilt from
the entry, at which point the origin's headers are long gone — so an origin
`Vary: Accept` on a stylesheet or a script was dropped from the response
entirely, while the same response passing straight through carried it. A
shared cache or CDN sitting between the install and its visitors then stored
that response keyed without `Accept`, and could hand one visitor's
representation to a visitor who asked for something else. Responses served
from cache now carry `Accept` in `Vary` whenever the entry records that the
origin negotiated on it. This is deliberately not applied across the board:
declaring `Accept` on every stylesheet and script would multiply the number of
copies every downstream cache keeps of resources that do not vary at all, so
it is declared only where the origin actually asked for it.

Fixed, separately and with wider reach than the above: an expired resource is
now re-cached after it is refreshed. The serving stage attached its own `Vary`
to the response before it was known whether the response would be served from
cache or fetched from the origin. On an expired resource it was fetched, and
the header went out with the request and came back attached to the origin's
reply, where the cache read it as though the origin had sent it. For images
that value names a field the cache deliberately refuses to store on, so the
refreshed image was judged uncacheable: it was fetched from the origin again
on the next request, and again on the one after, and was never re-optimized.

This reached images whose origin does not support conditional requests, or
whose content had changed — that is, any refresh answered with a full
response. Images from an origin that answers `304 Not Modified` were never
affected, because that exchange never consults the header in question. The
header is now attached only to responses actually served from cache.

Operators should expect a one-time revalidation for affected resources.
Entries recorded as Accept-negotiated carry a different entity tag from ones
that are not, because the two are served with different downstream cache
keying and are not interchangeable. A resource whose origin starts or stops
sending `Vary: Accept` therefore costs each visitor one full response, once,
after which conditional requests resume as normal.

A resource that was already cached and optimized before its origin began
sending `Vary: Accept` is handled too, and it is the case worth calling out
because it is the one every existing install is in. The copies built while it
was an ordinary resource are discarded rather than left in place — they would
otherwise keep being served in preference to the origin's own representation,
since a compressed copy is a better match for most visitors than an
uncompressed one. Recovery costs one origin fetch and happens the first time
the resource is refreshed after the origin changes.

The reverse direction works too: an origin that stops sending `Vary: Accept`
releases the resource back into optimization the next time it is refreshed.

Unchanged in both directions: a `Vary` this cache cannot key on — `Vary: *`,
or one naming a field such as `Cookie` or `Origin` — is still refused
outright and the response is served straight through without being stored, and
an `Accept` appearing beside such a field does not soften that refusal. Cache
entries written by earlier versions are read exactly as before; the record
this change writes uses space the entry format already reserved, so no cache
needs to be rebuilt.

Changed: version skew between the optimizer daemon and the components that
notify it is now reported instead of being silently absorbed. Notifications
have always carried a protocol version, and one carrying an unexpected version
has always been refused rather than half-read — which is the safe direction,
because acting on a message you cannot read is how a cache gets a wrong entry
written into it. What was missing was any way to tell that this was happening:
a refused notification looked exactly like a site that simply never warms up.
Refusals are now counted by cause, and a refusal caused by a version
disagreement is logged with both versions named, so the fix is readable off the
line. Three new counters appear on the statistics and Prometheus surfaces:
`rejected_version`, `rejected_malformed`, and `rejected_sentinel` (respectively:
a sender speaking another protocol version, a truncated or corrupt message from
a sender that agrees on the version, and a message naming a reserved cache
entry class that is not a valid notification trigger). The log line for a
version disagreement is rate-limited to one per minute per sending version, so
a misconfigured sender cannot flood the error log.

Added: a health check for the shared configuration file that the daemon writes
and the server module reads. If that file declares a schema version the running
build cannot read, the file is not parsed at all and the build falls back to
its compiled-in defaults — the install keeps serving and keeps optimizing, but
it does so on defaults rather than on the settings in the file. Until now that
happened silently outside the server module, which meant an install could run
for a long time in a configuration nobody had chosen. It is now reported the
moment it is observed, with both the file's version and the build's version
named and a statement of what is in effect instead, and `/v1/health` reports
`shared_config_version` as failing (overall status `degraded`) for as long as
the process has seen the mismatch. Operators running a matched set of
components will see no change.

Added: the cache gained a storage class for a response's request-independent
headers. A cache entry has always reproduced the headers the cache itself
needs — `Cache-Control`, `Content-Type`, `Content-Length`, `ETag`,
`Last-Modified`, `Expires` — and anything else the origin sent was not
reproducible from a stored entry, so serving such a response optimized would
have meant serving it with a header removed. Those responses were therefore
left alone. This class is what removes that obstacle: a per-URL block holding
the request-independent headers verbatim, exactly as the origin sent them, so
that a response can later be served optimized with its headers intact.

As with the durable-original class above, the storage and the API to use it
land first: nothing in the engine writes or serves these blocks yet, and no
serving behaviour changes in this release. What ships is the class, its
admission rules, and the C API an embedder uses to store one — so that a cache
written now is one a later version can serve from.

The headers kept are `Content-Security-Policy` (see below),
`Access-Control-Allow-Origin`, `X-Content-Type-Options`, `Referrer-Policy`,
`Cross-Origin-Opener-Policy`, `Cross-Origin-Embedder-Policy`,
`Cross-Origin-Resource-Policy`, `Permissions-Policy`, and the origin's own
`Vary` string. The list is closed and is not configurable, deliberately: a
header whose value depends on who is asking must never be replayed to the next
client, and that is not a per-deployment judgement. A response carrying
anything outside the list is still served plain — correctly, just not
optimized in place — and a count of those is kept in-process so the size of
that class can be observed.

A `Content-Security-Policy` carrying a nonce is never stored, and a resource
whose CSP carries one is never optimized in place. A nonce is per-response by
construction: replaying it to the next visitor would defeat the policy it is
part of. The detection is deliberately broad — a policy that merely mentions
`nonce-` anywhere is treated as carrying one — because the cost of being wrong
in that direction is one resource served as it always was, and the cost of
being wrong in the other is a security policy quietly disarmed.

Stored blocks are never served by ordinary cache selection: a block of headers
is not a representation of a resource and no request is ever answered with one.
A read always returns the most recent block. Where a single writer stores for a
URL, a later response replaces the block rather than adding to it; where
several writers store for the same URL concurrently, superseded blocks can
accumulate until a later version of the storage layer makes replacement
unconditional. The accumulation is bounded and logged rather than silent.

For embedders: `ps_cache_write_headers_sidecar` (PS_API 1.4) offers a
response's complete header set for storage, and `ps_headers_sidecar_classify`
answers the same admission question with no cache involved. You pass the whole
header set, not a curated one — the call is the gate, which is also why
`ps_cache_write_sentinel` refuses this entry's id. A refused response is not an
error: the call succeeds, reports what it decided, and stores nothing. Read the
header notes before building on it: the same concurrent-purge constraint that
applies to writing originals applies here, replacement is scoped to a single
writer per URL, and a missing block must be read as "this response is not
optimizable" rather than as "optimized, with no headers".

Added: the cache gained a storage class for the ORIGINAL of a resource — the
bytes the origin sent, before any optimization — held as an entry of its own
rather than in the slot an optimized variant may later take. What it buys is
rebuilding: with the original still stored, a changed optimization setting can
produce new variants without fetching the resource from the origin again. The
engine does not populate this class yet; the storage and the API to use it land
first, so that a cache written now is one a later version can read.

How long a stored original may be used is the origin's decision, not a retention
setting: it is governed by the `Cache-Control` and validators the origin sent,
evaluated the same way every other cache entry is. When the origin's response is
refreshed, the stored original is dropped in the same operation as the variants
derived from it, so a rebuild can never mix new variants with superseded bytes.

Stored originals are never served by ordinary cache selection: no request, of
any shape, is answered with the origin's own bytes merely because a copy of them
happens to be stored. They are read deliberately, by the code that wants them.

Re-recording an original replaces it, and a read always returns the most recent
one. That replacement holds for a single writer per URL; several writers storing
for the same URL at once can leave superseded copies behind, bounded by the
storage layer's chain limit but costing volume, since each copy is a whole
response body, and the copies retained are the oldest ones, reclaimed only by
normal volume eviction — so serialise them if you store originals from more
than one process. A later storage version makes replacement unconditional with no change
on your side. Note also that the replacement is not instantaneous: because the
write streams, the previous original is removed when the write starts and the
new one exists when it completes, so a stream that is abandoned, that the
storage layer refuses at close, or that exceeds the size limit leaves the URL
with no stored original. That reads as a miss — a
re-fetch, never wrong bytes.

Capacity planning, worth doing before this class is in use: storing originals
adds roughly one further copy of each recorded resource, and for image-heavy
pages the original is often the largest object in its family. Our sizing
guidance is to plan for about 2.5x your current cache volume for a typical live
set and about 3x for an image-dominated site, with 1.5x to 3.2x covering the
site shapes we have looked at; your own mix is the better guide once the class
is in use. Under-sizing costs no correctness — the volume evicts as it always
has — but it costs hit rate, so raise the volume first.

Responses larger than 16 MB are not kept as originals — above that size the
copy costs more than the rebuild it saves. The limit is a fixed property of the
build: no configuration file or directive changes it. It is enforced while the
response streams rather than after it has been buffered, so an oversized
response never costs the memory the limit exists to avoid. A response over the
limit is simply not stored as an original; nothing else about serving it
changes. There is no operator-facing metric for these skips yet: the C API
reports each one at the call that skipped it, with the limit named in the error
message, and an aggregate count exists in-process for a future stats surface.

For embedders: `ps_cache_write_original` (PS_API 1.3) stores an original through
the C API, taking the same write parameters as an ordinary write, and
`ps_cache_read_alternate` with `PS_SENTINEL_ORIGINAL` reads it back. It is a new
entry point, so nothing existing changes; `ps_cache_write_sentinel` refuses the
original's id, because the class carries entry metadata and a size limit that a
raw sentinel write would skip. Read its header notes before building on it: an
original you write is not fenced against a concurrent purge of the same URL,
which is a constraint a producer has to close rather than assume.

Changed: a cache entry now also records the origin's `Cache-Control` header
verbatim, next to the parsed form it already kept, so that anything which has to
reproduce the origin's own directives has the bytes rather than a
re-synthesis of them. Two further per-entry fields are added and left unused for
now, in preparation for upcoming work.

Entries written by earlier versions are still read, unchanged. Entries written by
this version are NOT readable by an earlier one: a downgraded build treats them
as a miss and re-records, so the cache re-warms rather than a downgraded build
misreading anything. Plan a downgrade as a cold cache, not as a rollback with no
effect. In practice 2.1 also moves the on-disk cache format, so a 2.0 build
reopens its own pre-upgrade volume and never reads an entry 2.1 wrote (see the
cache-format entry at the top of this section); the rule stated here is what
applies wherever an entry-format skew does meet inside one volume.

Note for embedders that set `max_metadata_size` explicitly: its default rises to
878 bytes, derived from the largest metadata blob the format can now produce. A
ceiling below that does not truncate anything — it refuses the write whole, so
affected entries are silently never stored, which in production looks like a
cache that never warms. Two ways to be affected, not one: every entry grows by
10 bytes regardless of what the origin sends, so a pinned ceiling can start
losing entries that were previously just inside it even with no long headers at
all; and separately, an entry can now carry up to 256 bytes of origin
Cache-Control.

Also for embedders: `ps_cache_write_sentinel` now refuses sentinel ids that are
reserved for entry classes which do not exist yet, returning the invalid-argument
error. Ids that name no class remain writable. No shipped id changes behaviour —
every id the API documents as usable stays usable.

Added: the embedding C API can now record and read back an origin's caching
state, and can answer the caching questions itself. A cache write accepts the
origin's `ETag`, `Last-Modified`, `max-age` / `s-maxage` and Cache-Control
directives, and new accessors read them back from a cache hit; three new
entry points evaluate whether an entry is still fresh, assemble the
`Cache-Control` value to send with it, and decide whether a response carrying
a given `Vary` may be cached at all. Previously an embedder had to implement
RFC 9111 a second time to answer those, and two caches disagreeing about
whether the same bytes are fresh is not a difference anyone notices until it
is serving stale content. Nothing existing changed: the write parameters grew
by appending, so code built against the previous version keeps working
unmodified, and an embedder that sets none of the new fields gets exactly the
previous behaviour.

Note for embedders: the freshness and Cache-Control entry points take a
cache-scope field whose default (zero) is SHARED, matching a proxy — the
deployment these are primarily for. An origin-private integration, such as
in-process middleware serving one application, must set it to private, or
`s-maxage` and `proxy-revalidate` will be applied where they should not be.

Note for embedders storing responses that arrive through another cache: the
insertion time is now yours to supply, and it should be the time the origin
generated the response, not the time you stored it. A response that reached
you carrying `Age: N` was already N seconds old, and recording your own clock
instead grants it N extra seconds of apparent freshness — behind a CDN, on
every response. `ps_age_adjusted_insert_time` does the arithmetic; leaving the
field at zero keeps the previous behaviour of stamping the local clock.

Added: the library's C API gains a documented stability contract
(`lib/pagespeed/ABI.md`) and the checks that enforce it. A published
structure's layout is now pinned at compile time, so moving a field fails the
build on every platform; and a separate check fails the change if an entry
point disappears, if the exported set drifts from what the header declares, or
if the version number does not move the way the change requires.

Fixed: four C API entry points — the agent-negotiation helpers, the
agent-aware cache read, and the content-binding accessor — were documented and
built but never actually exported, so any attempt to call them from a
separately compiled program failed to resolve them at load time. They are
exported now.

Added: the shared serve-statistics file gains a serve-class and worker-load
block. The optimizer now records how busy its own work queue is — a running sum,
a sample count and a high-water mark — so a monitoring tool can report average
and peak backlog over any interval instead of guessing from a single poll. The
file also reserves counters for how each response was served (optimized, or
original bytes because the optimized copy was absent, still being produced,
declined, or a version mismatch was detected), plus a count of records the
optimizer could not interpret, so a gap in those figures is visible rather than
silent; they stay at zero until a front-end that classifies serves ships. Nothing changes about how requests are
served or queued: this block is observation only. Two library entry points are
added for reading and writing it, and no existing entry point changed.

Note for operators: this bumps the serve-statistics file version, which resets
its counters once on upgrade, exactly as previous versions of that file have.
The optimizer recreates the file automatically and now logs a warning naming
both versions when it does, so a front end left on the older version — which
stops recording until it is upgraded — is visible in the log instead of only as
flat counters.

Fixed: re-optimizing a JPEG could encode it at a higher quality than the
original was saved at, producing a larger file with no visible improvement —
detail the source already discarded cannot be restored by spending more bytes
on it. A JPEG re-encode is now capped at the quality detected in the source
image, and the perceptual quality check no longer raises quality past that cap.
Sources saved at a high quality, and conversions to WebP or AVIF, are
unaffected. `--no-quality-cap` turns the cap off and now takes effect;
`--quality-cap-margin` is still accepted but no longer raises the cap.

Fixed: AVIF image candidates were shipped without the verify-and-re-encode
quality check that guards the other formats — the quality verifier could not
decode its own AVIF output, so a badly degraded AVIF encode would ship instead
of being caught and re-encoded at higher quality. AVIF candidates are now
decoded and verified like the other formats, and a verification that cannot
run now logs a loud warning instead of passing silently. Enabling the AV1
decode path also makes origin-served AVIF decodable and transcodable for the
first time (previously declined by construction); decode is capped at 50 MB of
pixels and runs in the isolated worker process, not the webserver.

For embedders: PS_API 1.9 adds size-aware entry points `ps_html_config_init_sized`
and `ps_critical_css_config_init_sized`, completing the set alongside
`ps_cache_config_init_sized`. The three legacy initializers
`ps_cache_config_init`, `ps_html_config_init` and `ps_critical_css_config_init`
now write only their PS_API 1.8 prefix (48, 128, and 88 bytes respectively),
pinned so future struct growth forces a developer decision. Callers compiled
against older headers must use the sized entry points or pass a buffer large
enough for the current library's layout.
