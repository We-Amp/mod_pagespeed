---
title: 'Troubleshoot common issues'
description: 'Fix common mod_pagespeed issues: cache misses, images not converting to WebP or AVIF, the optimizer worker not processing, and socket diagnostics.'
order: 58
group: 'Operate'
lastUpdated: 2026-11-14
---

Most mod_pagespeed problems surface in one place: the `X-PageSpeed`
response header. `HIT` means the worker served a cached response (normally an
optimized variant); `MISS` means it served the original. Read that header first,
then jump to the symptom
below that matches what you see. Flags referenced throughout are documented in
the [configuration reference](/docs/configuration/).

## Cache miss on every request

**Symptom:** Every response has `X-PageSpeed: MISS`, even for URLs that have
been requested before.

**Possible causes:**

1. **Cache file not shared.** The nginx module and worker must use the
   same cache file path. Verify that `pagespeed_cache_path` in your nginx
   config matches the worker's cache (`--cache-dir` / `--cache-path`).

   **After upgrading to 2.1.0 or later this is the first thing to check.** The worker's
   default paths moved to `/var/cache/pagespeed-optimizer/v3/cache` and
   `/run/pagespeed-optimizer/notify.sock`, but your web-server configuration
   keeps whatever it was set to. A configuration still naming the old
   `/var/lib/pagespeed-optimizer/...` paths makes the module attach to the
   abandoned cache and report the socket as **absent** — "start the optimizer worker" —
   while the optimizer worker is running perfectly well somewhere else. Repoint the
   directives and restart the web server.

   ```bash
   # Check nginx config
   nginx -T 2>/dev/null | grep pagespeed_cache_path

   # Check worker process
   ps aux | grep factory_worker
   ```

2. **Permissions.** The cache directory and every file in it must be owned
   by the worker's user (`pagespeed` in the packaged install), mode 0660
   owner+group — and the nginx worker user must be in group `pagespeed`.
   Check:

   ```bash
   ls -la /var/cache/pagespeed-optimizer/v3/
   id www-data   # (or apache / nginx) — must list "pagespeed"
   ```

   Fix with (the module package's postinst normally does the join):

   ```bash
   sudo usermod -a -G pagespeed www-data
   sudo systemctl restart nginx   # group membership applies on restart
   ```

3. **Memory-mapped directory not enabled.** Both processes must open the cache
   with mmap directory sharing. This is automatic in the worker and nginx
   module, but if you see persistent misses, the processes may have separate
   in-memory directories. Restart both to re-sync:

   ```bash
   sudo systemctl restart pagespeed-optimizer
   sudo systemctl reload nginx
   ```

4. **Worker and module from different releases.** If the error log has a
   line starting `pagespeed: cache-directory generation mismatch`, the worker
   and the nginx module use different cache formats (typically one was rolled
   back, or the packages were upgraded separately). The module then serves
   with its cache off, so every response is a `MISS`. The line names both
   generations and which side is older. Install the same release of both, or
   run both containers from the same image tag; the module turns its cache
   back on by itself once they match. Check the current state:

   ```bash
   grep 'cache-directory generation' /var/log/nginx/error.log | tail -1
   grep cache_dir_generation /var/cache/pagespeed-optimizer/v3/pagespeed-shared.conf
   ```

   Or expose `$pagespeed_cache_generation` (`match`, `mismatch`, `unknown`)
   in a log format or status location, see the
   [configuration reference](/docs/worker-configuration/#cache-directory-generation-check).

5. **Cache size too small.** If the cache is full, LRU eviction removes older
   entries before they can be served. Check cache utilization via the management
   socket:

   ```bash
   echo "STATS" | socat - UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.mgmt
   ```

   Look at `cache.size` relative to your `--cache-size` setting. If they are
   close, increase the cache size.

## X-PageSpeed: MISS on every reload in Chrome

**Symptom:** In Chrome you keep seeing `X-PageSpeed: MISS` and the original
image on every reload, and you are not sure optimization is working.

**Cause:** Almost certainly nothing is wrong. Chrome DevTools open with
**Disable cache** ticked, or a hard reload (`Cmd/Ctrl+Shift+R`), both make the
browser send `Cache-Control: no-cache` on every request. mod_pagespeed honors
that by revalidating and serving the unmodified origin response
(`X-PageSpeed: MISS`) instead of a cached optimized variant. Real visitors do
not send `no-cache`, so they always get the optimized response.

**Fix:** Untick **Disable cache** (or do a normal reload) and the same image URL
returns the optimized variant with `X-PageSpeed: HIT` and a smaller
`image/webp` or `image/avif` body. From the command line, request without the
no-cache header to see the variant directly:

```bash
curl -sI http://localhost:5050/hero.jpg -H 'Accept: image/webp'
```

## Worker not processing content

**Symptom:** The cache has original content (`X-PageSpeed: HIT`) but images
are not transcoded, CSS/JS are not minified, and no optimized variants appear.

**Possible causes:**

1. **Worker not running.** Verify the worker process is active:

   ```bash
   # systemd
   sudo systemctl status pagespeed-optimizer

   # Docker
   docker compose ps worker
   ```

2. **Socket path mismatch.** The worker writes its socket path to
   `pagespeed-shared.conf` (next to the cache file), which nginx reads
   automatically. Verify the shared config file exists and contains the
   correct socket path:

   ```bash
   # Check the shared config and socket file
   cat /var/cache/pagespeed-optimizer/v3/pagespeed-shared.conf
   ls -la /run/pagespeed-optimizer/notify.sock
   ```

3. **Socket permissions.** If the socket exists but nginx cannot connect,
   the nginx worker user is almost certainly not in group `pagespeed` (the
   socket is 0660 `pagespeed:pagespeed` by design — never world-writable).
   The worker's log distinguishes "permission denied" from "socket absent";
   the fix is the group join plus a web-server restart.

4. **Content type disabled.** The worker may have processing disabled for
   specific content types. Check for `--disable-html`, `--disable-css`,
   `--disable-js`, or `--disable-image` flags in the worker's startup command.

5. **Content too large.** The worker silently skips content exceeding size
   limits. Check worker logs for "too large" warnings:

   ```bash
   sudo journalctl -u pagespeed-optimizer | grep "too large"
   ```

   Increase limits with `--max-html-size`, `--max-css-size`, `--max-js-size`,
   or `--max-image-size` as needed.

## ASP.NET Core dashboard shows zeros

**Symptom:** The Dashboard shows zeros and nothing moves in DevTools when using
the ASP.NET Core middleware.

**Cause:** Almost always worker coordination. On ASP.NET Core releases before
2.0.14, `Worker.SocketPath` could default to a value that left the middleware
and worker unconnected, so no optimized variants were built and the Dashboard
stayed at zero. From 2.0.14 the default is an auto-resolved per-process socket
with coordination on, so the minimal `AddPageSpeed()` + `UsePageSpeed()` setup
works with no `Worker` config.

**Fix:** Upgrade to 2.0.14+ and confirm you have not set `Worker.SocketPath` to
`null` or `""` (either disables coordination and logs a startup warning) or
`Worker.AutoStart` to `false`.

One thing that is _not_ a fault: unlike
[the 1.x `.pagespeed.` URL scheme](/pagespeed-markers/#pagespeed-url), 2.0 does not
rewrite URLs into those links — the HTML stays clean and the original URL serves
optimized bytes through content negotiation, so you will not see new asset URLs in the
page source. To verify it is working, request a content route and check for
`X-PageSpeed: HIT`:

```bash
curl -i http://localhost:5050/
```

Then confirm transcoding on the same image URL — the following should return a
much smaller `Content-Type: image/webp` response with `Vary: Accept`:

```bash
curl -sI http://localhost:5050/hero.jpg -H 'Accept: image/webp'
```

A full walkthrough is in the
[getting-started guide](/docs/aspnet-getting-started/#verify-the-install).

## Admin console savings total reads lower after updating

**Symptom:** After updating, the [admin console](/docs/admin-console/)'s
savings total reads lower than before, or the console shows only a total with
no split.

**Cause:** Expected, not a regression. The console now shows how much of the
saving on responses served from the optimizer's cache came from compression
applied by mod_pagespeed and how much from its optimization; your web server's
own compression on other responses is not counted. The two parts, plus a share
not yet attributed, add up to the total saving. The total now counts HTML that
the optimizer makes larger, for example by inlining above-the-fold CSS or
adding preload hints; earlier versions counted that as zero, so the total can
read lower than before without anything optimizing less.

**Fix:** Nothing to fix. After the update the caches start cold, so the split
fills in as pages are re-optimized; until then the console shows the total
only.

## Images not converting to WebP/AVIF

**Symptom:** Image requests with `Accept: image/webp` still return the original
JPEG or PNG.

**Possible causes:**

1. **Worker has not processed yet.** After the first request (cache miss), the
   worker optimizes asynchronously. The first few requests may serve the
   original. Wait a moment and request again.

2. **Image too large.** Images larger than `--max-image-size` (default 10 MB)
   are skipped. Check worker logs:

   ```bash
   sudo journalctl -u pagespeed-optimizer | grep "Image too large"
   ```

3. **Image transcoding disabled.** Verify `--disable-image` is not set.

4. **Transcoding failed.** Some images (corrupted, unusual color profiles,
   very large dimensions) may fail to transcode. Check for error messages:

   ```bash
   sudo journalctl -u pagespeed-optimizer | grep -i "transcode\|failed\|error"
   ```

5. **Decoded pixel buffer too large.** The worker enforces a 50 MB limit on
   decoded pixel buffers to prevent out-of-memory conditions. A 10000x10000
   RGBA image decodes to ~400 MB and will be rejected. There is no
   configuration override for this limit.

6. **Proactive variants not enabled.** Without `--proactive-image-variants`,
   the worker only produces the single format matching the notification mask.
   WebP variants are only created when a WebP-capable client triggers the
   first notification.

## Debug logging

Enable debug-level logging to see detailed processing information:

```bash
# systemd: set LOG_LEVEL=debug in /etc/default/pagespeed-optimizer, then restart
sudoedit /etc/default/pagespeed-optimizer
sudo systemctl restart pagespeed-optimizer

# Docker: set environment variable or modify command
docker compose exec worker factory_worker --log-level debug ...
```

Debug logging shows:

- Every notification received (URL, content type, capability mask)
- Deduplication decisions (which notifications are skipped)
- Cache reads and writes (key, size, success/failure)
- Image decode/encode details (format, dimensions, output size)
- CSS/JS minification results (original size vs. minified size)
- HTML scanning and critical CSS extraction details

For JSON log format (recommended for parsing):

```bash
factory_worker --log-format json --log-level debug
```

Parse JSON logs with `jq`:

```bash
sudo journalctl -u pagespeed-optimizer -o cat | jq 'select(.level == "ERROR")'
```

## Management socket diagnostics

The management socket provides real-time insight into the worker's state.

### Check overall health

```bash
echo "STATS" | socat - UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.mgmt
```

In the container images the management socket is `/data/pagespeed.sock.mgmt`. On the packages, run these commands as root or as a member of group `pagespeed`.

Look for:

- **High `errors` count** — Processing failures. Check logs for details.
- **`cache.entries` is 0** — Cache may not be opening correctly.
- **`notifications.received` is 0** — The worker is not receiving notifications
  from nginx. Check socket connectivity.
- **`variants.written` is 0 but `notifications.received` is high** — The worker
  receives notifications but fails to write variants. Check for permission
  issues or content processing errors.

### Purge and re-test a specific URL

To force the worker to re-process a URL:

```bash
# PURGE needs AUTH first, on the same connection. The worker's
# PAGESPEED_PURGE_TOKEN is not in your shell by itself; read it in first.
# Packages: the env file is readable by root and group `pagespeed`, one KEY=value per line
PAGESPEED_PURGE_TOKEN=$(sudo sed -n 's/^PAGESPEED_PURGE_TOKEN=//p' /etc/pagespeed-optimizer/daemon.env)
# Container images and the Helm chart: the worker prints the token once at
# start when you did not set it -- `docker logs <worker> 2>&1 | grep PAGESPEED_PURGE_TOKEN=`
# or `kubectl logs <pod> -c worker | grep PAGESPEED_PURGE_TOKEN=` -- or set it
# yourself in the compose file's `environment:` and use that value.

# Purge all variants
printf 'AUTH %s\nPURGE localhost /images/photo.jpg\n' "$PAGESPEED_PURGE_TOKEN" | socat - UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.mgmt

# Request the URL again (triggers a cache miss and new notification)
curl -H "Accept: image/webp,*/*" http://localhost/images/photo.jpg -o /dev/null -w "%{size_download}\n"

# Wait a moment for the worker to process, then request again
sleep 2
curl -H "Accept: image/webp,*/*" http://localhost/images/photo.jpg -o /dev/null -w "%{size_download}\n"
```

If the second request returns a smaller size, the worker is processing
correctly for that URL.

A refused purge says why: `ERR authentication required` means this connection
sent no valid `AUTH` (an `AUTH` on another connection does not count);
`ERR invalid token` means the token was wrong, which is also what an empty
`$PAGESPEED_PURGE_TOKEN` produces; `ERR no purge token configured` means the
worker started without a token, so no purge is possible until it has one. The
responses are listed under [`AUTH`](/docs/api-reference/#auth-token).

### Cannot connect to management socket

If `socat` or Python fails to connect to the management socket:

```bash
# Verify the socket file exists
ls -la /run/pagespeed-optimizer/notify.sock.mgmt

# Verify the worker is running
sudo systemctl status pagespeed-optimizer

# Check socket permissions
stat /run/pagespeed-optimizer/notify.sock.mgmt
```

The management socket is created when the worker starts and removed when it
shuts down. If the socket file does not exist, the worker is not running or
failed during initialization.

## Common error messages

### "Failed to open cache at ..." {#failed-to-open-cache-at}

The worker cannot open or create the cache file, or refused to start. The
optimizer worker refuses to start (loudly, naming the cause) when its cache directory
is missing, unwritable, or holds content owned by another uid — it never
chowns or migrates content itself. Read the refusal line in the journal; it
distinguishes "does not exist" from "permission denied" from
"foreign-owned". For the packaged layout, recreate the directory the
declarative way:

```bash
sudo systemd-tmpfiles --create pagespeed-optimizer.conf
```

### "Failed to bind to ..." {#failed-to-bind-to}

The Unix socket path is already in use by another process, or a stale socket
file exists from a previous crash. The worker removes stale socket files on
startup, but if another instance is running, you will see this error. Stop the
other instance first.

### "Max connections reached"

The worker is at its connection limit. This can happen under heavy load when
nginx sends many notifications simultaneously. Increase the limit:

```bash
factory_worker --max-connections 256
```

### "Client buffer exceeded max size"

A single notification is larger than `--max-buffer-size` (default 1 MB). This
typically indicates a malformed message. If you have legitimate very long URLs,
increase the buffer size.

### "URL too long"

The notification URL exceeds `--max-url-length` (default 8192 bytes). Increase
the limit if your site uses very long URLs, or consider whether the long URL
is intentional.

## Browser analysis issues

Browser analysis requires headless Chrome and is disabled by default. Enable it
with `--enable-browser-analysis`. These issues only apply when browser analysis
is active.

### Chrome not found

**Symptom:** Worker logs `Failed to start Chrome` or `chrome binary not found`
on startup with browser analysis enabled.

**Fix:** The worker looks for Chrome at the path specified by `--chrome-binary`
(default: `/usr/bin/chrome-headless-shell`). Verify the binary exists and is
executable:

```bash
ls -la /usr/bin/chrome-headless-shell
# or wherever your Chrome is installed

# If Chrome is elsewhere, specify the path:
factory_worker --enable-browser-analysis --chrome-binary /usr/bin/chromium
```

In Docker containers, install `chrome-headless-shell` or `chromium`. The
workbench-demo Docker image includes it by default.

### CDP connection failures

**Symptom:** Worker logs `CDP pipe read error` or `Chrome pipe EOF` during
analysis. Browser profiles are not generated.

**Possible causes:**

1. **Chrome crashed.** The worker automatically restarts Chrome after a 2-second
   delay. Check logs for the crash reason:

   ```bash
   sudo journalctl -u pagespeed-optimizer | grep -i "chrome\|crash\|exit"
   ```

2. **Memory limit exceeded.** If Chrome's RSS exceeds `--chrome-max-memory`
   (default 512 MB), the worker kills and restarts it. Increase the limit for
   sites with large pages:

   ```bash
   factory_worker --enable-browser-analysis --chrome-max-memory 1024
   ```

3. **Startup timeout.** Chrome may take longer to start in resource-constrained
   environments. Increase `--chrome-startup-timeout` (default 10000 ms):

   ```bash
   factory_worker --enable-browser-analysis --chrome-startup-timeout 20000
   ```

### Analysis timeouts

**Symptom:** Worker logs `session timeout` for browser analysis. Some pages
never get browser-validated profiles.

**Fix:** The per-page timeout is controlled by `--chrome-page-timeout` (default
60000 ms). Complex pages with many stylesheets may need more time. However,
if timeouts are frequent, the root cause is often Chrome struggling with
inlined CSS volume. Check the page's stylesheet count and total CSS size.

```bash
# Check browser analysis status via management socket
echo "BROWSER-STATUS" | socat - UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.mgmt
```

The response includes `analysis_errors`, `chrome_crashes`, and `queue_depth`
counters.

### Browser analysis not improving results

**Symptom:** Browser analysis is enabled and running, but HTML output is
identical to heuristic-only mode.

**Possible causes:**

1. **Profile TTL too short.** If `--browser-profile-ttl` is very short, profiles
   expire before they are used. The default (86400 seconds / 24 hours) works for
   most sites.

2. **Template mismatch.** Browser profiles are keyed by DOM structure hash. If
   every page has a unique structure (e.g., inline content changes the DOM tree),
   each page gets its own profile and re-analysis runs constantly. This is normal
   for highly dynamic sites but reduces the benefit.

3. **Individual features disabled.** Check whether `--no-browser-critical-css`,
   `--no-browser-lazy-loading`, `--no-browser-lcp-preload`, or
   `--no-browser-image-sizing` flags are set. Each disables a specific browser
   analysis output.

## SVG vectorization issues

SVG auto-vectorization converts suitable raster images (logos, icons, flat
illustrations) to SVG format. It runs in `detect` mode by default, which only
evaluates candidacy without producing SVG output. Set `--svg-mode auto` for
production serving.

### No SVG variants produced

**Symptom:** The worker processes images but no SVG variants appear in the cache.

**Possible causes:**

1. **SVG mode is `detect` (the default).** In detect mode, the worker evaluates
   images for SVG candidacy and logs scores, but does not vectorize. Set
   `--svg-mode auto` or `--svg-mode preview` to produce SVG output:

   ```bash
   factory_worker --cache-path /data/cache.vol --svg-mode auto
   ```

2. **Candidacy threshold too high.** The `--svg-candidacy-threshold` (default 50)
   filters out images with low vectorization suitability. Photos and complex
   textures score low and are rejected. This is by design -- SVG is only
   beneficial for simple graphics. Lower the threshold to see more candidates:

   ```bash
   factory_worker --svg-candidacy-threshold 30
   ```

3. **Images too large.** The `--svg-max-pixels` flag (default 65536, about 256x256)
   limits which images are evaluated. Large photos are excluded because
   vectorization produces enormous SVGs. Increase for larger icons:

   ```bash
   factory_worker --svg-max-pixels 262144  # ~512x512
   ```

4. **Image processing disabled.** If `--disable-image` is set, all image
   processing is skipped, including SVG vectorization.

### SVG variants larger than raster

**Symptom:** Debug logs show `svg_size_rejected` counter increasing. SVGs are
produced but discarded.

This is the size gate working correctly. If the vectorized SVG is larger than
the raster original (which is common for photos and complex images), the SVG
variant is discarded. The `svg_bytes_saved` stat shows cumulative savings for
SVGs that did pass the gate.

### SVG path count exceeded

**Symptom:** Debug logs show `svg_path_count_rejected` counter increasing.

Complex images produce SVGs with many `<path>` elements, which can slow down
browser rendering. The `--svg-max-paths` flag (default 500) limits the maximum
path count. If you want to allow more complex SVGs:

```bash
factory_worker --svg-max-paths 1000
```

Be cautious: SVGs with thousands of paths can cause rendering jank on mobile
devices.

### LCP images not vectorized

**Symptom:** The LCP hero image qualifies for SVG but no SVG variant is produced.

By default, `--svg-exclude-lcp true` skips vectorization for images identified
as the Largest Contentful Paint candidate. SVG path tessellation in the browser
can be slower than decoding a raster image, potentially regressing LCP.

If your LCP image is a simple logo or icon that renders quickly as SVG:

```bash
factory_worker --svg-exclude-lcp false
```

## Vary header and cache poisoning

**Symptom:** All responses are cache misses. The cache never populates even
though nginx and the worker are running correctly.

**Cause:** The interceptor adds a `Vary: User-Agent` token to mark device-aware
responses. In builds before **2.0.16**, that token could be evaluated as part of
the response's own cacheability check, causing every response to be treated as
uncacheable.

**Fix:** Upgrade to **2.0.16 or newer**, where this is resolved — confirm with
the `X-PageSpeed` response header. If the symptom persists after upgrading,
[contact support](/contact/) with your worker version and a sample response's
headers.

## Native module issues

The issues below are specific to the in-process module (Apache, nginx, IIS)
rather than the Docker / nginx reverse-proxy worker.

### No X-Mod-Pagespeed or X-Page-Speed header

**Cause:** mod_pagespeed is not running or not intercepting the response.

**Fix:**

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

- Verify the module is loaded: check `nginx -V` for the module.
- Verify `pagespeed on;` is set in your server block.
- Check that the response Content-Type is `text/html`. mod_pagespeed only rewrites HTML responses.

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

- Verify the module is loaded: check `apachectl -M` for `pagespeed_module`.
- Verify `ModPagespeed on` is set.
- Verify `AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html` is present (usually set automatically).
- Check that the response Content-Type is `text/html`. mod_pagespeed only rewrites HTML responses.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

- Open IIS Manager and verify the module appears under **Modules** for your site.
- Verify `pagespeed on` is set in your `pagespeed.config`.
- Check that the response Content-Type is `text/html`. mod_pagespeed only rewrites HTML responses.
- Check the Windows Event Viewer (**Windows Logs > Application**) for startup errors from the mod_pagespeed source.
- Verify that the IIS worker process (app pool identity) can read the `pagespeed.config` file.

</div>

### Pages are never served optimized / cache never warms

**Cause:** Resources are being optimized but the cache is not warming.

**Fix:**

- Optimization is progressive: the first request to a page triggers background optimization. Wait for the rewrite to complete and retry.
- If repeated requests still serve the original resources, check cache directory permissions and disk space.
- Verify the cache path exists and is writable by the web server process.

### Stylesheets and scripts respond slowly while images are being optimized {#slow-ce-while-optimizing-images}

**Symptom:** On the first visit to an image-heavy page, for example after a deploy or a cache clear, the page stays blank for several seconds. Once image optimization has finished, the same page loads normally. In one lab test on a 2-CPU server, Largest Contentful Paint rose from about 0.35 s to 9.4 s on such a first view.

**How to confirm:** Request a stylesheet or script that the module serves with an extended cache lifetime (its URL contains `.pagespeed.ce.`) while images on the page are still being optimized, and look at the response time:

```bash
curl -s -o /dev/null -w '%{time_total}s\n' 'https://example.com/css/site.css.pagespeed.ce.HASH.css'
```

If those responses take seconds during that window and are fast afterwards, and other optimized resources are not affected, this is the cause. The URL to test is the `.pagespeed.ce.` one in the page's HTML.

**Workaround:** The heavy image work and these responses share the expensive rewrite pool, whose size [`NumExpensiveRewriteThreads`](/docs/configuration/#numexpensiverewritethreads) controls. With `auto` it is sized from the CPUs the process may use, half of them divided by the number of server processes, never fewer than one. On nginx the divisor is `worker_processes`: with `worker_processes auto` (one worker per core), or with workers pinned by `worker_cpu_affinity`, `auto` resolves to one expensive thread per worker. Setting a larger explicit value, at the `http` level, relieves the stall; in the lab test above, 4 removed it entirely.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
http {
    pagespeed NumExpensiveRewriteThreads 4;
}
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedNumExpensiveRewriteThreads 4
```

Put it in the server configuration, outside any virtual host.

</div>

Restart the web server to apply it. The directive is global and cannot be set per virtual host or location. The cost is more image work at the same time, so higher CPU use and more memory at peak while a page's images are being optimized; raise it in steps and watch request latency, as described in [Optimization threads](/docs/configuration/#optimization-threads). Reducing the number of images optimized per page also helps.

This is tracked as [mod_pagespeed#781](https://github.com/We-Amp/mod_pagespeed/issues/781) and is being fixed in an upcoming release.

### Why does a `.pagespeed.` URL return 404? {#optimized-resources-return-404}

A rewritten URL such as `/css/a.css+b.css.pagespeed.cc.HASH.css` carries what the module needs to rebuild the file: the original URL (or URLs, for a combined file), the filter id and a content hash. A flushed cache or a page cached by a CDN does not by itself break it: when the module gets a request for a `.pagespeed.` URL it does not have, it fetches the original and rewrites it again. A request whose hash is stale (the original changed after the page was cached) is still answered, with a shortened cache lifetime, rather than with a 404; the exception is a JavaScript source map, which is refused when its hash does not match. A 404 therefore means one of the causes below. The log line each one leaves tells them apart (the ones marked "info" need the log level raised to info; the admin console's [message history](/docs/admin-console/#message-history) shows the same messages):

| Log text | Cause |
| --- | --- |
| nothing from the module; the web server answers 404 itself | The request never reached the module ([routing](#pagespeed-404-routing)) |
| `Decoding of resource name … failed because … no filter with id` or `… cannot decode the URL` (info) | The URL names a filter id this module does not know, or the filter cannot decode the name |
| `Decoding of resource name … failed because … filter_id … is forbidden` (info) | The filter is disabled for the request by [`ForbidFilters`](/docs/configuration/#forbidfilters) (or `ForbidAllDisabledFilters`) |
| `Invalid resource signature for … provided. Received …` (info) | [URL signing](#pagespeed-404-signing) is on and the signature does not match |
| `Resource based on … but cannot access the original` (warning) | [The original cannot be fetched](#pagespeed-404-original) |

<a id="pagespeed-404-original"></a>

**The original cannot be fetched.** The module rebuilds a file from its original, so a 404 from the origin (the file was removed or renamed), a fetch that fails (the origin refuses the module's request, a TLS or DNS problem, a timeout) or a domain the module is not [authorized](/docs/domain-configuration/#domain-authorization) to fetch from all end as a 404 on the `.pagespeed.` URL. For a file with a single original (an image, a stylesheet, a script), a rewrite that fails while the original is fetchable is answered with the original instead. A _combined_ file has several originals and no such fallback: if one of them cannot be fetched at that moment, the whole combined URL answers 404. A fetch that timed out, could not connect or got a 5xx answer is remembered for ten seconds, then retried; a 404 or 410 that the origin itself sent is remembered for five minutes by default. To see which input failed, look in the log for the `Resource based on …` warning above and for `Fetch failed for resource url`, then request that original URL yourself, from the machine that runs the module. Pages in a CDN or a browser cache that still name a removed original stay broken until the page is refreshed; purge those pages rather than the `.pagespeed.` URLs.

<a id="pagespeed-404-signing"></a>

**URL signing.** With `UrlSigningKey` set, every `.pagespeed.` URL carries a signature, and the module treats a URL with a wrong or missing one as not its own, so the web server answers 404 for the file it does not have. Changing the key invalidates every signed URL in HTML that is already cached downstream. `pagespeed AcceptInvalidSignatures on;` (nginx) or `ModPagespeedAcceptInvalidSignatures on` (Apache) accepts them while you rotate the key. On nginx the signature also makes the hash segment longer (ten more characters by default), so the `[^.]{10}` in the `.pagespeed.` location block of the sample configuration no longer matches; use `[^.]{20}`.

<a id="pagespeed-404-routing"></a>

**The request never reaches the module.** Fix this per web server:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

Keep the three `location` blocks of the [sample configuration](/docs/configuration/#minimal-working-configuration) in the server that serves the page: the `.pagespeed.` pattern, `^/pagespeed_static/` and `^/ngx_pagespeed_beacon$`. A regular-expression `location` earlier in the file that matches `.js`, `.css` or image extensions (a static-file or cache rule) takes the request first, and the module never sees it.

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

If using `mod_rewrite`, add `RewriteCond %{REQUEST_URI} !\.pagespeed\.` to prevent rewrite rules from intercepting pagespeed URLs.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

Check that URL Rewrite rules (if installed) do not intercept `.pagespeed.` URLs. Add a stop-processing rule before other rewrite rules:

```xml
<rule name="PageSpeed" stopProcessing="true">
    <match url="\.pagespeed\." />
    <action type="None" />
</rule>
```

Also verify that the IIS app pool has not been recycled mid-optimization. The module reloads its cache on app pool restart, but in-flight rewrites are lost.

</div>

Verify that the configuration options match between the HTML-serving VHost and the resource-serving VHost. A `.pagespeed.` URL is decoded with the options of the host and path it is requested on, so a filter enabled for the page but forbidden, or not configured, where the resource is served cannot be decoded there.

**A CDN or load balancer in front.** The CDN must send `.pagespeed.` requests to the server that runs the module, not to a separate storage origin, and it must forward the request path unchanged. If the CDN caches error responses, one early failure (a request that arrived while the original was unreachable) is replayed until its cache lifetime ends; give 404 responses a short TTL at the CDN. See [Behind a TLS terminator, load balancer or CDN](/docs/cdn-integration/#behind-a-tls-terminator-load-balancer-or-cdn).

**The beacon returns 404.** The beacon endpoint (`/mod_pagespeed_beacon` on Apache, `/ngx_pagespeed_beacon` on nginx, or the path set by `BeaconUrl`) is answered by the module, so the request has to reach it: on nginx that means the `^/ngx_pagespeed_beacon$` location block above, and on Apache the module answers the path itself, so a 404 there usually comes from a rule that sends the path elsewhere first, such as a `ProxyPass` or a redirect. The module matches the beacon path without its query string; check that the `BeaconUrl` in the page source is the path your server actually routes to the module. A beacon 404 costs the beacon-driven filters their data but does not affect the page. Without those filters, set `pagespeed CriticalImagesBeaconEnabled false;` (nginx) or `ModPagespeedCriticalImagesBeaconEnabled false` (Apache).

### mod_pagespeed broke my page layout or JavaScript

**Cause:** A filter is incompatible with your site's CSS or JavaScript.

**Fix:**

1. Identify which filter causes the issue by adding [`?PageSpeed=off`](/pagespeed-markers/#pagespeed-off) to the URL to disable all optimization.
2. If the page works with `?PageSpeed=off`, narrow down the filter by disabling them one at a time.
3. Common culprits:
   - [`defer_javascript`](/docs/javascript-filters/#defer_javascript) — breaks scripts using `document.write` or expecting to run during parse.
   - [`combine_javascript`](/docs/javascript-filters/#combine_javascript) — can break order-dependent scripts.
   - [`prioritize_critical_css`](/docs/css-filters/#prioritize_critical_css) — can cause flash of unstyled content if critical CSS detection is incomplete.
4. Disable the offending filter with `DisableFilters`.

### mod_pagespeed is not picking up file changes

**Cause:** Resources are cached with their original TTL or mod_pagespeed's implicit cache TTL.

**Fix:**

- With `EnableCachePurge on`, purge everything from the global admin page (a `POST` with the `X-Requested-With` header; a plain `GET` answers `405`, and the per-vhost `/pagespeed_admin` page refuses `*`): `curl -X POST -H 'X-Requested-With: XMLHttpRequest' 'http://yoursite.com/pagespeed_global_admin/cache?purge=*'`. Send it from a client the [admin access rules](/docs/admin-console/#setup) allow.
- With `EnableCachePurge` off (the default), touch `cache.flush` in the `FileCachePath` directory: `sudo touch /var/cache/mod_pagespeed/cache.flush` on Apache, `sudo touch /var/cache/ngx_pagespeed/cache.flush` on nginx (the path in the Apache package's `pagespeed.conf` and in the nginx package's sample configuration; use the path your configuration names). See [Cache flushing and purging](/docs/cache-modes/#cache-flushing-and-purging).
- If resources are loaded from disk via `LoadFromFile`, changes are picked up after `LoadFromFileCacheTtlMs` expires (default: same as ImplicitCacheTtlMs, 5 minutes).
- For immediate pickup, set a shorter `ImplicitCacheTtlMs` or purge the specific URL.

### High memory usage

**Cause:** mod_pagespeed caches metadata and optimized resources in memory.

**Fix:**

- Check `CycloneRamCacheKb`. From v1.15.0+r18 the RAM tier is off by default (`0`); a positive value or `-1` (the pre-r18 default, which sizes it from `LRUCacheKbPerProcess`) adds per-process RAM on top of the shared memory-mapped cache.
- Reduce `DefaultSharedMemoryCacheKB` (default: 51200).
- If using IPRO on a site with many unique uncacheable URLs, the metadata cache can grow. Consider disabling IPRO for those URL patterns.

### SELinux blocks mod_pagespeed

**Cause:** SELinux policy prevents the web server from writing to the cache directory. On Enterprise Linux both Apache and nginx run in the `httpd_t` domain, which can write files labelled `httpd_cache_t` but not the `httpd_sys_content_t` (read-only content) or generic `var_t` labels.

**Check:** `ls -Zd` on your `FileCachePath` shows its label, and `sudo ausearch -m avc -ts recent` lists the denials.

**Fix:**

- **Apache:** the stock targeted policy already labels `/var/cache/mod_pagespeed`, the package's cache directory, `httpd_cache_t`. If the label is wrong (a directory copied in from elsewhere), restore it with `sudo restorecon -Rv /var/cache/mod_pagespeed`.
- **nginx:** the stock policy has no rule for `/var/cache/ngx_pagespeed`, so it is labelled `var_t` and nginx cannot write to it. Add a persistent rule and apply it (`semanage` is in the `policycoreutils-python-utils` package):

  ```bash
  sudo semanage fcontext -a -t httpd_cache_t '/var/cache/ngx_pagespeed(/.*)?'
  sudo restorecon -Rv /var/cache/ngx_pagespeed
  ```

- **A cache directory you chose yourself:** the same two commands with your path. Do not use `chcon`: its label is lost at the next `restorecon` or file-system relabel.
- **Fetching:** the module fetches subresources over HTTP, including from the server itself over loopback, and the stock policy denies the web server outbound connections. Without the boolean below, resources are served unoptimized: `sudo setsebool -P httpd_can_network_connect 1`.
- **The optimizer worker:** the Apache module RPM installs the `pagespeed-optimizer` SELinux policy module and relabels `/var/cache/pagespeed-optimizer` and `/run/pagespeed-optimizer` itself; `sudo semodule -l | grep pagespeed-optimizer` confirms it is loaded. If the install could not load it, the package printed the retry command: `sudo semodule -i /usr/share/selinux/targeted/pagespeed-optimizer.pp`. The nginx module RPM does not ship this policy module, so on a host with only the nginx module nothing installs it: with SELinux enforcing, nginx cannot reach the worker's cache volume and notify socket, and in-place optimization through the worker stays off.

### Beacons causing unexpected POST requests

**Cause:** Filters like [`lazyload_images`](/docs/image-filters/#lazyload_images), `defer_javascript`, and `prioritize_critical_css` use a [JavaScript beacon](/pagespeed-markers/#pagespeed-beacon) to report client-side data. The beacon sends POST requests to the beacon URL.

**Fix:**

- This is expected behavior, not an error.
- If you do not use beacon-dependent filters, you can disable the beacon: `pagespeed CriticalImagesBeaconEnabled false;`
- To suppress the `<noscript>` redirect tag inserted by some filters: `pagespeed SupportNoScriptEnabled false;`

### How to get more diagnostic information

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

- Check the admin page at `/pagespeed_admin/` for statistics, active filters, and cache status.
- Enable message history: set `MessageBufferSize` to a non-zero value, then view messages at `/pagespeed_admin/message_history`.
- Add the `debug` filter temporarily to see detailed rewriting information in HTML comments: `pagespeed EnableFilters debug;`
- Check nginx error logs for mod_pagespeed messages.

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

- Check the admin page at `/pagespeed_admin/` for statistics, active filters, and cache status.
- Enable message history: set `MessageBufferSize` to a non-zero value, then view messages at `/pagespeed_admin/message_history`.
- Add the `debug` filter temporarily: `ModPagespeedEnableFilters debug`
- Check Apache error logs for mod_pagespeed messages.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

- **Windows Event Viewer:** Open Event Viewer and check **Windows Logs > Application** for warnings and errors from the mod_pagespeed source. The module logs startup issues, configuration errors, and runtime warnings here automatically.
- **Admin pages:** Navigate to `/pagespeed_admin/` for statistics, active filters, and cache status.
- **Message history:** Set `pagespeed MessageBufferSize 100000` in your config, then view messages at `/pagespeed_global_admin/message_history`. Messages are collected globally from all IIS worker processes.
- **DebugView:** Use the [Sysinternals DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) tool for real-time debug output. Enable debug logging by adding `pagespeed diagnose` to your server-level `pagespeed.config`. In DebugView, enable **Capture > Capture Global Win32** to see the output.
- **Debug filter:** Add `?PageSpeedFilters=+debug` to any URL to see detailed optimization decisions, timing data, and filter descriptions in HTML comments.
- **App pool permissions:** Verify that the IIS app pool identity has read access to `pagespeed.config` and write access to the cache directory. Permission errors show up in Event Viewer.

</div>

## Next steps

- [Configuration Reference](/docs/configuration/) — All worker flags and tuning
  options
- [API reference](/docs/api-reference/) — Protocol details for management socket
  and IPC
- [HTTP API Reference](/docs/http-api/) — REST and WebSocket endpoints for
  programmatic access
- [Web Console](/docs/workbench/) — Visual cache inspector, debug console, and
  real-time metrics
- [Deployment Guide](/docs/deployment/) — Production setup and monitoring
- [Hydrated pages](/docs/hydrated-pages/) — What is left unchanged on hydrated
  React, Next.js, Vue, Nuxt and Svelte pages, and what to do when one is missed
