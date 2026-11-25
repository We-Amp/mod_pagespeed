# Notes for the optimizer worker flags

Operator prose appended to the generated blocks on `/docs/worker-configuration/`.
One `## <flag>` section per flag; the flag must exist in `worker-flags.json`
(the worker's own usage text) or rendering fails. Syntax, default and the usage
description are generated and must not be repeated here.

## --socket

```bash
factory_worker --socket /run/pagespeed-optimizer/notify.sock
```

The socket path reaches nginx automatically through the
[shared configuration file](#shared-configuration-file); no nginx directive is
needed. The worker creates the socket at startup with an explicit 0660
owner+group mode, never umask-derived, so the web side reaches it through group
`pagespeed` membership rather than world permissions. The directory must exist
and be writable by the worker's user (the packaged unit uses a systemd
`RuntimeDirectory`, 0750 `pagespeed:pagespeed`).

## --cache-dir

```bash
factory_worker --cache-dir /var/cache/pagespeed-optimizer/v3
```

The `v3` in the default is the compiled-in cache-directory generation. The
directory must exist, be writable by the worker, and contain only files owned by
the worker's user; otherwise the worker refuses to start and names the cause.
The packaged tmpfiles.d drop-in creates the default as 3770
`pagespeed:pagespeed` (setgid so the group is inherited, sticky so one group
member cannot remove another's files).

## --cache-path

```bash
factory_worker --cache-path /var/cache/pagespeed-optimizer/v3/cache
```

Must match the nginx [`pagespeed_cache_path`](#pagespeed_cache_path) directive.
The worker reads original content from this file and writes optimized variants
back to it.

## --cache-size

```bash
factory_worker --cache-size 536870912  # 512 MB
```

Total size of the Cyclone cache volume. The cache evicts least recently used
entries when full. See [Sizing the cache](#sizing-the-cache).

## --ram-cache-size

The RAM tier uses write-around semantics: writes go to the volume, reads
populate the RAM cache on a miss, so nginx and the worker never see stale copies
of each other's writes. See [Threading and memory](#threading).

## --read-lease-duration

```bash
factory_worker --read-lease-duration 0  # disable read leases
```

While a lease is live the cache defers overwriting that region, so in-flight
zero-copy responses are never served corrupted bytes. `0` disables leases
(writes are never deferred). On a full cache under sustained read traffic,
leases can delay new writes to a hot region; lowering or disabling them favors
write throughput over zero-copy read protection.

## --lease-wrap-ceiling

```bash
factory_worker --lease-wrap-ceiling 30000
```

Only relevant when read leases are enabled. Lower values favor write admission
at a full cache; responses that hold cache content longer than this ceiling fall
back to copying.

## --num-threads

`auto` sizes the pool from the CPUs the worker may use, following the CPU limit of its container or cgroup. See
[Threading and memory](#threading).

## --max-connections

```bash
factory_worker --max-connections 256
```

When the limit is reached, new connections are accepted and immediately closed.
Monitor the [health check](#health-check) to track connection usage.

## --max-buffer-size

```bash
factory_worker --max-buffer-size 2097152  # 2 MB
```

Connections exceeding this buffer size are disconnected, so a malformed or
oversized message cannot exhaust memory.

## --connection-timeout

```bash
factory_worker --connection-timeout 60000  # 60 seconds
```

Connections that send no data within the timeout are closed.

## --shutdown-timeout

```bash
factory_worker --shutdown-timeout 10000  # 10 seconds
```

On SIGTERM or SIGINT the worker stops accepting new connections and waits up to
this long for active connections to drain before force-closing them.

## --html-mode

`off` ignores HTML notifications, like `--disable-html`. `full` is today's
behaviour: optimized HTML variants, plus the Markdown copy for agents when
`--agent-optimize` is on. `markdown-static` and `markdown-rendered` write no
optimized HTML variant; the Markdown copy for agents is the only output derived
from a page. On pages the nginx front stores, the two `markdown-*` modes
currently build the agent copy the same way. HTML variants stored under `full`
keep being served after a switch until the page changes or is refreshed; purge
after switching to drop them at once. When both `--html-mode` and `--disable-html` are given,
`--html-mode` wins. The same setting is `html_mode` in `pagespeed.json` and
`PATCH /v1/config`, where the legacy `disable_html` (`true` = `off`, `false` =
`full`) is still accepted and still reported.

## --disable-html

HTML notifications from nginx are ignored: no critical-CSS inlining, no HTML
transforms, no HTML caching.

## --disable-css

CSS notifications are ignored.

## --disable-js

JavaScript notifications are ignored.

## --disable-image

Image notifications are ignored.

## --proactive-image-variants

Without this flag the worker produces only the single format the notification
asked for (for example only WebP). With it, one decode yields WebP, AVIF and an
optimized original. See [Proactive variant generation](#proactive-variant-generation).

## --no-proactive-viewport-variants

By default variants for all three viewport classes (mobile, tablet, desktop) are
generated from one decode, resized to each viewport's target width. With this
flag only the viewport in the notification is produced.

## --no-proactive-savedata-variants

By default both normal-quality and Save-Data (lower-quality) variants are
generated from each decode. With this flag only the notification's Save-Data
preference is produced. The Save-Data quality levels are the `--savedata-*-quality`
flags.

## --no-proactive-density-variants

By default variants for both 1x and 2x+ pixel density are generated from each
decode; for 2x+ the viewport target width is doubled before resizing (a
mobile/2x variant targets 960px instead of 480px). With this flag only the
density in the notification is produced.

## --enable-warmup

When nginx detects a URL exceeding [`pagespeed_hot_threshold`](#pagespeed_hot_threshold)
it sends a warmup sentinel to the worker. With warmup enabled, the worker
pre-generates all missing variants for that URL; without it, warmup
notifications are ignored.

## --no-lazy-load-images

See [Lazy load images](#lazy-load-images) for what is skipped (the LCP
candidate, the first body images, the first iframe, invisible images).

## --no-image-dimensions

See [Image dimensions](#image-dimensions).

## --no-lcp-preload

See [LCP preload](#lcp-preload).

## --no-preconnect-injection

See [Preconnect injection](#preconnect-injection).

## --no-async-css

Enabled does not mean applied: see [Async CSS](#async-css) for the per-page
conditions deferral requires.

## --async-css-min-coverage

A stylesheet is deferred only when the critical rules cover at least this share
of it; `0` disables the gate. See [Async CSS](#async-css).

## --async-css-min-deferred-bytes

Stylesheets below this size defer regardless of coverage. See
[Async CSS](#async-css).

## --no-css-import-flattening

See [CSS import flattening](#css-import-flattening).

## --mobile-width

Images wider than the target are downscaled, preserving aspect ratio; images
already smaller are not resized. `0` disables resizing for the viewport. See
[Viewport resize widths](#viewport-resize-widths).

## --tablet-width

See [Viewport resize widths](#viewport-resize-widths).

## --desktop-width

The default `0` means desktop images are not resized. See
[Viewport resize widths](#viewport-resize-widths).

## --jpeg-quality

Range 1-100. See [Image quality](#image-quality-flags).

## --webp-quality

Range 0-100. See [Image quality](#image-quality-flags).

## --avif-quality

Range 0-100. See [Image quality](#image-quality-flags).

## --savedata-jpeg-quality

Used when the request carries `Save-Data: on`. Range 1-100.

## --savedata-webp-quality

Used when the request carries `Save-Data: on`. Range 0-100.

## --savedata-avif-quality

Used when the request carries `Save-Data: on`. Range 0-100.

## --no-learned-quality

Falls back to the heuristic quality calculation for every format. See
[Learned quality prediction](#learned-quality-prediction).

## --no-learned-quality-jpeg

See [Learned quality prediction](#learned-quality-prediction).

## --no-learned-quality-webp

See [Learned quality prediction](#learned-quality-prediction).

## --no-learned-quality-avif

See [Learned quality prediction](#learned-quality-prediction).

## --savedata-score-reduction

SSIMULACRA2 points subtracted from the target for Save-Data variants.

## --target-ssimulacra2

Range 0-100. See [SSIMULACRA2 quality tuning](#ssimulacra2-quality-tuning).

## --ssimulacra2-tolerance

The band is asymmetric: `[target - 0.6 * tolerance, target + 1.6 * tolerance]`.
See [SSIMULACRA2 quality tuning](#ssimulacra2-quality-tuning).

## --no-quality-verify

Skips the SSIMULACRA2 measurement pass entirely, saving CPU at the risk of
inconsistent visual quality.

## --no-content-analysis

Falls back to the `Unknown` content class for every image. See
[Content analysis](#content-analysis-and-denoising).

## --denoise-threshold

Removed: noisy images are no longer smoothed before encoding. The flag is
still accepted and ignored.

## --denoise-sigma-spatial

Removed. Still accepted and ignored.

## --gzip-level

Range 1-9; `0` disables gzip alternates. See
[Pre-compressed text variants](#pre-compressed-text-variants).

## --brotli-level

Range 1-11; `0` disables brotli alternates. See
[Pre-compressed text variants](#pre-compressed-text-variants).

## --max-url-length

Notifications with longer URLs are rejected.

## --max-html-size

Larger HTML documents are skipped.

## --log-level

```bash
factory_worker --log-level warning
```

## --log-format

```bash
factory_worker --log-format json
```

`text` is human-readable (`[INFO] message text`); `json` is one object per line
(`{"timestamp":"...","level":"INFO","message":"..."}`) and is the better choice
with log aggregation.

## --api-socket

The recommended local transport: filesystem permission (group `pagespeed`) is
the credential, the worker binds no TCP port, and no bearer token is asked for
on the socket. See [HTTP management API](#http-management-api).

## --api-port

`0` keeps the TCP API off. `--api-socket` and `--api-port` are mutually
exclusive: the worker serves exactly one transport. See
[HTTP management API](#http-management-api).

## --api-bind

An IPv4 literal; `localhost`, `::1` and other IPv6 forms are rejected by name.
A non-loopback bind also needs `--api-allow-remote` and a token.

## --api-allow-remote

Confirms a deliberate non-loopback bind. Remote is never unauthenticated: a
token is still required. See
[One invariant, enforced at startup](#one-invariant-enforced-at-startup).

## --api-no-auth

Confirms a deliberate tokenless API on loopback or the unix socket; the worker
prints a warning banner at startup.

## --api-token

Prefer the `PAGESPEED_API_TOKEN` environment variable: anything on the command
line is readable by any local user through `/proc/<pid>/cmdline`. Package
installs generate a token into `/etc/pagespeed-optimizer/daemon.env`.

## --api-read-open

Opens an explicit allow-list of GET endpoints and WebSocket streams (the
cached-URL inventory, origin hosts, the cooldown table, the live stats and
events streams) without the token; the log endpoint and stream stay token-only.
Use it only behind a reverse proxy that does its own authentication.

## --no-security-headers

Omit the security headers the worker otherwise adds to console responses, for
deployments where a reverse proxy sets them.

## --console-dir

Path of the web console's static files. See [the admin console](/docs/admin-console/).

## --enable-browser-analysis

Chrome (or `chrome-headless-shell`) must be available at `--chrome-binary`. See
[Browser analysis](#browser-analysis) and the
[browser analysis guide](/docs/browser-analysis/).

## --browser-sandbox

There is no `auto`. With `require` the worker probes at startup and refuses
browser analysis, rather than running an unsandboxed browser, when the sandbox
is unavailable; the worker keeps serving without it. `off` is a deliberate
opt-out and logs a warning at startup and on every browser start. See
[Headless browser sandbox](#headless-browser-sandbox).

## --browser-user-data-dir

Chrome profile directory; defaults to a `chrome` directory under the worker's
runtime directory.

## --chrome-recycle-interval

Chrome is restarted after this many pages to bound memory growth. See
[Chrome memory management](#chrome-memory-management).

## --chrome-max-memory

On Linux the worker watches Chrome's RSS through `/proc` and kills the process
above this threshold; elsewhere only page-count recycling applies.

## --no-browser-critical-css

Keeps the heuristic critical-CSS extraction even when browser analysis runs.

## --browser-profile-ttl

Optimization profiles expire after this many seconds and are re-analyzed.

## --allow-private-urls

Off by default for security. Enable only in development, or when the worker
has to analyze sites on private networks.

## --strip-query-extensions

```bash
factory_worker --strip-query-extensions css,js,png,jpg,webp
```

Consolidates cache entries for static assets that differ only by a cache-busting
query string. The whole query string is dropped for every listed extension, on
every site the worker serves, so list an extension only when no site's server
returns different content for it depending on the query string. See
[URL normalization](#url-normalization).

## --strip-query-groups

```bash
factory_worker --strip-query-groups static
```

A group stands for a fixed list of extensions (`images`, `static`) and works
exactly like listing them with
[`--strip-query-extensions`](#--strip-query-extensions), on every site the
worker serves. See [URL normalization](#url-normalization).

## --strip-query-params

```bash
factory_worker --strip-query-params utm_source,utm_medium,fbclid
```

Removes only the named parameters and keeps the rest of the query string, on
every site the worker serves. This is the right setting for tracking parameters
such as `utm_source` or `fbclid`: list only parameters that no site's server
uses to choose what it returns. See [URL normalization](#url-normalization).

## --host-alias

```bash
factory_worker --host-alias www.example.com=example.com
```

Requests for the source host are treated as the destination host in cache
lookups. Repeatable.

## --svg-mode

Start with `detect` to see which images qualify, move to `preview` to inspect
the stored SVGs, then `auto` to serve them. See
[SVG auto-vectorization](#svg-auto-vectorization).

## --svg-exclude-lcp

Enabled by default because SVG render cost (path tessellation in the browser)
can regress LCP for hero images. Set it to `false` when your LCP images are
simple graphics that benefit from vectorization.

## --svg-preset

Reserved for a later release; accepted but not yet applied.

## --svg-fidelity-threshold

Reserved for a later release; accepted but not yet applied.

## --svg-timeout-ms

Reserved for a later release; accepted but not yet applied.

## --agent-optimize

Experimental; behavior may change between releases. See
[Agent optimize](/docs/agent-optimize/).

## --web-bot-auth

Observe-only: labels and counts signed agent requests and never changes request
handling. See [Web Bot Auth](/docs/web-bot-auth/).

## --service

Runs the worker under the Windows Service Control Manager; used by the IIS
installer's service registration.

## --log-file

With `--service`: appends the log to the given file instead of standard error.

## --rsl-cap-enforcement

Experimental. In the container image, `PAGESPEED_RSL_CAP_ENFORCEMENT=true` sets
this flag. See [RSL-CAP enforcement](/docs/rsl-cap/).
