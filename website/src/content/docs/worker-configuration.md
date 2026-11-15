---
title: 'Worker and reverse-proxy configuration'
description: 'Every optimizer worker flag and thin nginx module directive of mod_pagespeed (the Docker and reverse-proxy shape), generated from the source.'
order: 22
group: 'Configure'
lastUpdated: 2026-10-22
faq:
  - q: 'What is the difference between safe and aggressive cache modes?'
    a: 'Safe mode adds `must-revalidate` to non-HTML responses, suppresses stale-while-revalidate synthesis, and strips `immutable`. Aggressive mode adds `public` and `stale-if-error=86400` and allows SWR synthesis. HTML always gets `no-cache` in both modes.'
  - q: 'How do I disable PageSpeed for specific URLs?'
    a: 'Use `pagespeed off` inside a `location` block to disable the module for that path, or `pagespeed_disallow` to skip individual URL patterns (prefix, suffix, or substring) while keeping the module active elsewhere.'
  - q: 'How do I share the cache file between nginx and the worker?'
    a: 'Point `pagespeed_cache_path` (nginx) and the worker cache at the same file (the packaged optimizer worker defaults to `--cache-dir /var/cache/pagespeed-optimizer/v3`). The optimizer worker owns the directory and every file in it (0660 `pagespeed:pagespeed`); the nginx worker user reaches them through membership in group `pagespeed`. Memory-mapped directory sharing is enabled automatically.'
  - q: 'What does the capability mask do?'
    a: 'The 32-bit mask encodes the client image format, viewport, pixel density, Save-Data, and transfer encoding into the cache key, so different optimized variants are served to different clients. Nginx derives the mask from request headers and client hints.'
  - q: 'How large should the Cyclone cache be?'
    a: 'Small blog or portfolio sites need 256 MB–1 GB (the `--cache-size` default is 1 GB); medium sites (~1,000 pages) need 1–2 GB; large sites (10,000+ pages) need 2–4 GB; and image-heavy sites need 4–8 GB, because each source image can produce up to 37 variants when all proactive flags are enabled. See the sizing table for details.'
---

:::tip[Running the native module?]
This page is for the **Docker / reverse-proxy shape**: the thin nginx module in
front of your origin plus the optimizer worker beside it, as the
`pagespeed-nginx` and `pagespeed-worker` images and the Helm chart run them. If
you installed the in-process module into Apache or nginx from the package
repository, its directives (`ModPagespeed…` / `pagespeed …`) are on the
[configuration reference](/docs/configuration/).
:::

This shape has a small configuration surface: two required nginx directives
(`pagespeed on;` and `pagespeed_cache_path`), a shared configuration file the
worker writes, and the worker's command-line flags. The reference below is
generated from the product's source, so it lists exactly what the shipped
binaries accept: the 16 directives of the thin module from its command table,
and every flag of the worker from its own `--help` text, each as a fixed
Syntax / Default block (the directives also list their context). Everything
past the two required directives is optional tuning.

To work out which flags your own pages need, run them through a
[PageSpeed Insights test](/analyze/): every failing audit is mapped to the
transform that fixes it, so the report doubles as a configuration checklist.

## Which nginx version

Three things on this site bundle or target an nginx, and they are not the same
version. This table is the one place that says which applies to which shape; the
Docker and sidecar rows come from the version files in the source tree, the
package rows from the signed repository's distribution matrix.

<!-- generated:begin nginx-compat -->

| Shape                                                                               | nginx version                                                    | Configuration surface |
| ----------------------------------------------------------------------------------- | ---------------------------------------------------------------- | --------------------- |
| Docker / reverse proxy (`ghcr.io/we-amp/pagespeed-nginx`, the combined image, Helm) | nginx 1.30.6, bundled in the image                               | Thin module + worker  |
| Native module package, Debian 12 bookworm                                           | nginx 1.22.1 (the distribution's stock nginx; exact-version pin) | Native module         |
| Native module package, Debian 13 trixie                                             | nginx 1.26.3 (the distribution's stock nginx; exact-version pin) | Native module         |
| Native module package, Ubuntu 22.04 jammy                                           | nginx 1.18.0 (the distribution's stock nginx; exact-version pin) | Native module         |
| Native module package, Ubuntu 24.04 noble                                           | nginx 1.24.0 (the distribution's stock nginx; exact-version pin) | Native module         |
| Native module package, Ubuntu 26.04 resolute                                        | nginx 1.28.3 (the distribution's stock nginx; exact-version pin) | Native module         |
| Native module package, AlmaLinux 9 (yum)                                            | nginx 1.20.1 (stock nginx; exact-version pin)                    | Native module         |
| NuGet sidecar `WeAmp.PageSpeed.Sidecar`                                             | nginx 1.30.6, bundled in the package                             | Thin module + worker  |

<!-- generated:end nginx-compat -->

The native module is built per distribution and pinned to that distribution's
exact nginx version (nginx's `--with-compat` does not relax the check), so an
nginx version that is not in the table needs a matching pinned build. The
Docker image and the NuGet sidecar ship their own nginx, so the host's nginx
version does not matter for them.

## Thin module directives {#nginx-directives}

The thin module's directives are available in the `http`, `server` and
`location` contexts; a value set at a higher level is inherited by nested
blocks. The module reads everything else (the worker socket, the HTML toggle,
URL normalization, the cache mode when no directive sets it) from the
[shared configuration file](#shared-configuration-file).

<!-- generated:begin thin-module-directives -->

[`pagespeed`](#pagespeed), [`pagespeed_cache_path`](#pagespeed_cache_path), [`pagespeed_disallow`](#pagespeed_disallow), [`pagespeed_hot_threshold`](#pagespeed_hot_threshold), [`pagespeed_max_age`](#pagespeed_max_age), [`pagespeed_immutable_max_age`](#pagespeed_immutable_max_age), [`pagespeed_synthesize_swr`](#pagespeed_synthesize_swr), [`pagespeed_conditional_revalidation`](#pagespeed_conditional_revalidation), [`pagespeed_stale_if_error_max_age`](#pagespeed_stale_if_error_max_age), [`pagespeed_html_max_age`](#pagespeed_html_max_age), [`pagespeed_css_max_age`](#pagespeed_css_max_age), [`pagespeed_image_max_age`](#pagespeed_image_max_age), [`pagespeed_force_refresh_html`](#pagespeed_force_refresh_html), [`pagespeed_force_refresh`](#pagespeed_force_refresh), [`pagespeed_trust_x_forwarded_proto`](#pagespeed_trust_x_forwarded_proto), [`pagespeed_cache_mode`](#pagespeed_cache_mode), [`pagespeed_agent_content_signal`](#pagespeed_agent_content_signal)

#### pagespeed {#pagespeed}

- **Syntax:** `pagespeed on | off;`
- **Default:** `off`
- **Context:** http, server, location

When on, the module intercepts responses, serves optimized variants from the
cache, notifies the worker on a miss and adds the `X-PageSpeed` response header
(`HIT` or `MISS`). `pagespeed off;` inside a `location` block switches the module
off for that path; see [Disabling per location](#disabling-pagespeed-per-location).
The thin module's switch is a plain flag; the native module's `on`, `standby` and
`unplugged` states are on the
[configuration reference](/docs/configuration/#modpagespeed).

```nginx
pagespeed on;
```

#### pagespeed_cache_path {#pagespeed_cache_path}

- **Syntax:** `pagespeed_cache_path path;`
- **Default:** `(empty)`
- **Context:** http, server, location

Path of the Cyclone cache volume file.

```nginx
pagespeed_cache_path /var/lib/pagespeed/cache.vol;
```

Caching is disabled while this is unset. The path must be the same file the
worker opens (`--cache-path`, or the volume stem inside `--cache-dir`). Both
processes open the file with memory-mapped directory sharing, so writes from
either side are immediately visible to the other.

The worker creates the file if it does not exist. For sharing to work the file
must be readable and writable by both the nginx worker processes and the
optimizer worker. The packaged worker takes care of this: it runs as the
unprivileged `pagespeed` user and sets every shared file to 0660 owner+group,
so the only setup the web side needs is membership in group `pagespeed` (the
module package's postinst adds it).

The module also checks that the worker beside this path uses the same cache
format, by comparing its own cache-directory generation with the
`cache_dir_generation` the worker publishes in the
[shared configuration file](#shared-configuration-file). See
[Cache-directory generation check](#cache-directory-generation-check).

#### pagespeed_disallow {#pagespeed_disallow}

- **Syntax:** `pagespeed_disallow pattern;`
- **Default:** —
- **Context:** http, server, location

Excludes URLs from caching and optimization while the module stays active for
everything else. Repeatable; the first matching pattern wins.

```nginx
pagespeed_disallow /api/;       # prefix match
pagespeed_disallow *.woff2;     # suffix match
pagespeed_disallow admin;       # substring match
```

A pattern starting with `/` matches a URL prefix, a pattern starting with `*`
matches a suffix, any other pattern matches as a substring. See
[URL pattern exclusions](#url-pattern-exclusions).

#### pagespeed_hot_threshold {#pagespeed_hot_threshold}

- **Syntax:** `pagespeed_hot_threshold count;`
- **Default:** `5`
- **Context:** http, server, location

Number of fallback hits before a URL counts as "hot" and triggers a warmup
notification to the worker. Only relevant when the worker runs with
[`--enable-warmup`](#--enable-warmup).

#### pagespeed_max_age {#pagespeed_max_age}

- **Syntax:** `pagespeed_max_age seconds;`
- **Default:** `86400`
- **Context:** http, server, location

Cap in seconds on the `max-age` taken from the origin's `Cache-Control`. See the
[cache-control guide](/docs/cache-control/).

#### pagespeed_immutable_max_age {#pagespeed_immutable_max_age}

- **Syntax:** `pagespeed_immutable_max_age seconds;`
- **Default:** `604800`
- **Context:** http, server, location

Cap in seconds for responses the origin marks `Cache-Control: immutable`.

#### pagespeed_synthesize_swr {#pagespeed_synthesize_swr}

- **Syntax:** `pagespeed_synthesize_swr on | off;`
- **Default:** `on`
- **Context:** http, server, location

Synthesizes `stale-while-revalidate` on cache-served (`HIT`) responses. Safe
mode suppresses the synthesis regardless of this flag; see
[`pagespeed_cache_mode`](#pagespeed_cache_mode).

#### pagespeed_conditional_revalidation {#pagespeed_conditional_revalidation}

- **Syntax:** `pagespeed_conditional_revalidation on | off;`
- **Default:** `on`
- **Context:** http, server, location

Answers `If-None-Match` and `If-Modified-Since` requests with `304 Not Modified`
when the cached body still matches.

#### pagespeed_stale_if_error_max_age {#pagespeed_stale_if_error_max_age}

- **Syntax:** `pagespeed_stale_if_error_max_age seconds;`
- **Default:** `86400`
- **Context:** http, server, location

Window in seconds during which a stale cached response may be served when the
origin fails; `0` disables it. The default matches the `stale-if-error=86400`
that aggressive mode advertises to downstream caches.

#### pagespeed_html_max_age {#pagespeed_html_max_age}

- **Syntax:** `pagespeed_html_max_age seconds;`
- **Default:** `0`
- **Context:** http, server, location

Default `max-age` in seconds for HTML the origin serves without a
`Cache-Control` header. `0` keeps such HTML uncached; set it to opt an origin
into [HTML caching](#html-caching).

#### pagespeed_css_max_age {#pagespeed_css_max_age}

- **Syntax:** `pagespeed_css_max_age seconds;`
- **Default:** `300 (safe) / 86400 (aggressive)` (depends on the resolved cache mode)
- **Context:** http, server, location

Default `max-age` in seconds for CSS and JavaScript the origin serves without a
`Cache-Control` header. The default follows the cache mode.

#### pagespeed_image_max_age {#pagespeed_image_max_age}

- **Syntax:** `pagespeed_image_max_age seconds;`
- **Default:** `1800 (safe) / 86400 (aggressive)` (depends on the resolved cache mode)
- **Context:** http, server, location

Default `max-age` in seconds for images the origin serves without a
`Cache-Control` header. The default follows the cache mode.

#### pagespeed_force_refresh_html {#pagespeed_force_refresh_html}

- **Syntax:** `pagespeed_force_refresh_html on | off;`
- **Default:** `on`
- **Context:** http, server, location

Revalidates HTML at the origin when the browser force-refreshes (Ctrl+F5 /
`Cache-Control: no-cache` on the request).

#### pagespeed_force_refresh {#pagespeed_force_refresh}

- **Syntax:** `pagespeed_force_refresh on | off;`
- **Default:** `off`
- **Context:** http, server, location

Revalidates every non-HTML type at the origin on a browser force-refresh.

#### pagespeed_trust_x_forwarded_proto {#pagespeed_trust_x_forwarded_proto}

- **Syntax:** `pagespeed_trust_x_forwarded_proto on | off;`
- **Default:** `off`
- **Context:** http, server, location

Reads the `X-Forwarded-Proto` request header to decide the request scheme
(`http` or `https`) instead of connection-level TLS detection. Use it when
nginx sits behind a TLS-terminating reverse proxy, load balancer or CDN that
sets the header. Only lowercase `http` and `https` values are accepted; any
other value falls back to connection-level detection.

:::caution
Only enable this when the upstream proxy strips and re-sets the
`X-Forwarded-Proto` header. If clients can set the header directly, they can
manipulate scheme detection.
:::

```nginx
# Upstream nginx (TLS termination)
server {
    listen 443 ssl;
    location / {
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_pass http://backend;
    }
}

# Backend nginx (PageSpeed)
server {
    listen 80;
    pagespeed on;
    pagespeed_trust_x_forwarded_proto on;
    pagespeed_cache_path /var/lib/pagespeed/cache.vol;
}
```

#### pagespeed_cache_mode {#pagespeed_cache_mode}

- **Syntax:** `pagespeed_cache_mode safe | aggressive;`
- **Default:** `safe` (unless the worker shared configuration sets aggressive)
- **Context:** http, server, location

Controls how `Cache-Control` headers are assembled on optimized responses.

```nginx
pagespeed_cache_mode safe;        # default: short TTLs, must-revalidate
pagespeed_cache_mode aggressive;  # long TTLs, public, stale-if-error
```

In safe mode, `must-revalidate` is added to all non-HTML responses, SWR
synthesis is suppressed, and `immutable` is stripped from transformed content.
In aggressive mode, `public` and `stale-if-error=86400` are added, and SWR
synthesis is allowed. HTML always gets `no-cache` in both modes. The per-type
defaults of [`pagespeed_css_max_age`](#pagespeed_css_max_age) and
[`pagespeed_image_max_age`](#pagespeed_image_max_age) follow the mode. See
[Cache modes](/docs/cache-modes/) for the full reference.

#### pagespeed_agent_content_signal {#pagespeed_agent_content_signal}

- **Syntax:** `pagespeed_agent_content_signal value | off;`
- **Default:** `(empty)`
- **Context:** http, server, location

The `content-signal` header sent with a Markdown response for agents (the
page's Markdown copy and `/llms.txt`), in the
[Content Signals](https://contentsignals.org/) format. Unset, no header is
sent; `off` stops an inherited value.

```nginx
pagespeed_agent_content_signal "search=yes, ai-input=yes, ai-train=no";
```

The value must be printable ASCII on one line, up to 512 bytes; nginx refuses
to start with any other value. See
[Serve Markdown to AI agents](/docs/agent-optimize/).

<!-- generated:end thin-module-directives -->

### Cache-Control directives {#cache-control-directives}

`pagespeed_max_age`, `pagespeed_immutable_max_age`, the per-type
`pagespeed_*_max_age` directives, `pagespeed_synthesize_swr`,
`pagespeed_conditional_revalidation` and the two `pagespeed_force_refresh*`
directives together decide the `Cache-Control` and `Age` headers on
cache-served responses. The [cache-control guide](/docs/cache-control/) walks
through the resulting headers per content type, and
[Cache modes](/docs/cache-modes/) through `pagespeed_cache_mode`.

### Cache-directory generation check

A cache directory holds one cache format, and the format is part of the
volume file's name. A module and a worker built for different formats would
open two different files in the same directory and never share a cache. So
the module compares its compiled-in generation with the worker's
`cache_dir_generation`: at start, on every reload, and whenever the worker
rewrites its shared config (the ~1s poll).

| Result     | What the module does                                                                                                                                                                       |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `match`    | Uses the cache as normal.                                                                                                                                                                  |
| `mismatch` | Keeps serving with its cache **off**: requests go to the origin, nothing is read or stored, and the worker is not notified. Logs one `[error]` naming both generations and the older side. |
| `unknown`  | The worker publishes no generation (older than 2.1.0), or there is no shared config yet. Uses the cache as before and logs one `[warn]`.                                                   |

A mismatch clears itself: once the worker publishes the matching generation,
the module turns its cache back on without a reload and logs "now matches".
The fix is to run the same release of both, from packages or from one image
tag.

Three variables expose the result, as this nginx worker last saw it:

| Variable                                | Value                                             |
| --------------------------------------- | ------------------------------------------------- |
| `$pagespeed_cache_generation`           | `match`, `mismatch` or `unknown`                  |
| `$pagespeed_cache_generation_module`    | The module's generation                           |
| `$pagespeed_cache_generation_optimizer` | The worker's published generation (empty if none) |

```nginx
log_format pagespeed_gen '$remote_addr "$request" $status '
                         'cache_generation=$pagespeed_cache_generation';

location = /pagespeed-generation {
    allow 127.0.0.1;
    deny all;
    return 200 "$pagespeed_cache_generation module=$pagespeed_cache_generation_module optimizer=$pagespeed_cache_generation_optimizer\n";
}
```

## Shared configuration file

The worker writes a `pagespeed-shared.conf` file next to the cache file
(in the parent directory of `--cache-path`). Nginx reads this file automatically
using `pagespeed_cache_path` to locate it. The file is polled every ~1 second
for changes.

**Format:** `key=value` (one per line)

**Fields:**

| Key                    | Description                                                                                                                                 | Worker flag      |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- | ---------------- |
| `socket_path`          | Unix socket path for nginx-to-worker notifications                                                                                          | `--socket PATH`  |
| `disable_html`         | Whether HTML optimization is disabled (`true`/`false`)                                                                                      | `--disable-html` |
| `cache_dir_generation` | Cache-directory generation N (skew = loud handshake failure; nginx turns its cache off, see [the check](#cache-directory-generation-check)) | (compiled in)    |

The file is written atomically at mode 0640 `pagespeed:pagespeed`: readable
by the worker and its group-`pagespeed` peers, not by the world.

The shared config file is written on worker startup and whenever settings change
via `PATCH /v1/config`. This eliminates the need to duplicate configuration
between the nginx config and worker flags.

On a cache miss, nginx reads the `socket_path` from the shared config and sends
a fire-and-forget notification to the worker. If the socket is not available
(worker not running, socket not yet created), the notification is silently
dropped. The original content continues to be served from the cache.

## Worker flags

The `factory_worker` binary accepts the flags below. The blocks are generated
from the worker's own usage text, so a flag listed here is a flag the shipped
binary accepts, and `--help` on your installed version shows the same list.
Boolean `--no-…` flags switch off a transform that is on by default; the
`(default: …)` in a description is the worker's own wording.

<!-- generated:begin worker-flags -->

Sections: [General options](#flags-general-options) (88) · [Windows service](#flags-windows-service) (2) · [Cache key normalization](#flags-cache-key-normalization) (4) · [SVG auto-vectorization](#flags-svg-auto-vectorization) (11) · [Agent optimize (experimental, off by default)](#flags-agent-optimize-experimental-off-by-default) (8) · [Web Bot Auth (observe-only, off by default)](#flags-web-bot-auth-observe-only-off-by-default) (4) · [RSL-CAP enforcement (experimental, off by default)](#flags-rsl-cap-enforcement-experimental-off-by-default) (5).

### General options {#flags-general-options}

[`--socket`](#--socket), [`--cache-dir`](#--cache-dir), [`--cache-path`](#--cache-path), [`--cache-size`](#--cache-size), [`--ram-cache-size`](#--ram-cache-size), [`--read-lease-duration`](#--read-lease-duration), [`--lease-wrap-ceiling`](#--lease-wrap-ceiling), [`--num-threads`](#--num-threads), [`--max-connections`](#--max-connections), [`--max-buffer-size`](#--max-buffer-size), [`--connection-timeout`](#--connection-timeout), [`--shutdown-timeout`](#--shutdown-timeout), [`--disable-html`](#--disable-html), [`--disable-css`](#--disable-css), [`--disable-js`](#--disable-js), [`--disable-image`](#--disable-image), [`--proactive-image-variants`](#--proactive-image-variants), [`--no-proactive-viewport-variants`](#--no-proactive-viewport-variants), [`--no-proactive-savedata-variants`](#--no-proactive-savedata-variants), [`--no-proactive-density-variants`](#--no-proactive-density-variants), [`--enable-warmup`](#--enable-warmup), [`--no-lazy-load-images`](#--no-lazy-load-images), [`--no-image-dimensions`](#--no-image-dimensions), [`--no-lcp-preload`](#--no-lcp-preload), [`--no-preconnect-injection`](#--no-preconnect-injection), [`--no-async-css`](#--no-async-css), [`--async-css-min-coverage`](#--async-css-min-coverage), [`--async-css-min-deferred-bytes`](#--async-css-min-deferred-bytes), [`--enable-speculation-rules`](#--enable-speculation-rules), [`--no-content-analysis`](#--no-content-analysis), [`--no-preserve-c2pa`](#--no-preserve-c2pa), [`--c2pa-carry`](#--c2pa-carry), [`--denoise-threshold`](#--denoise-threshold), [`--denoise-sigma-spatial`](#--denoise-sigma-spatial), [`--denoise-sigma-range`](#--denoise-sigma-range), [`--no-quality-verify`](#--no-quality-verify), [`--target-ssimulacra2`](#--target-ssimulacra2), [`--ssimulacra2-tolerance`](#--ssimulacra2-tolerance), [`--no-learned-quality`](#--no-learned-quality), [`--no-learned-quality-jpeg`](#--no-learned-quality-jpeg), [`--no-learned-quality-webp`](#--no-learned-quality-webp), [`--no-learned-quality-avif`](#--no-learned-quality-avif), [`--savedata-score-reduction`](#--savedata-score-reduction), [`--gzip-level`](#--gzip-level), [`--brotli-level`](#--brotli-level), [`--no-css-import-flattening`](#--no-css-import-flattening), [`--mobile-width`](#--mobile-width), [`--tablet-width`](#--tablet-width), [`--desktop-width`](#--desktop-width), [`--jpeg-quality`](#--jpeg-quality), [`--webp-quality`](#--webp-quality), [`--avif-quality`](#--avif-quality), [`--savedata-jpeg-quality`](#--savedata-jpeg-quality), [`--savedata-webp-quality`](#--savedata-webp-quality), [`--savedata-avif-quality`](#--savedata-avif-quality), [`--max-url-length`](#--max-url-length), [`--max-html-size`](#--max-html-size), [`--max-css-size`](#--max-css-size), [`--max-js-size`](#--max-js-size), [`--max-image-size`](#--max-image-size), [`--log-level`](#--log-level), [`--log-format`](#--log-format), [`--api-socket`](#--api-socket), [`--api-port`](#--api-port), [`--api-bind`](#--api-bind), [`--api-allow-remote`](#--api-allow-remote), [`--api-no-auth`](#--api-no-auth), [`--api-token`](#--api-token), [`--api-read-open`](#--api-read-open), [`--no-security-headers`](#--no-security-headers), [`--console-dir`](#--console-dir), [`--enable-browser-analysis`](#--enable-browser-analysis), [`--browser-sandbox`](#--browser-sandbox), [`--browser-user-data-dir`](#--browser-user-data-dir), [`--chrome-binary`](#--chrome-binary), [`--chrome-recycle-interval`](#--chrome-recycle-interval), [`--chrome-page-timeout`](#--chrome-page-timeout), [`--chrome-max-memory`](#--chrome-max-memory), [`--chrome-startup-timeout`](#--chrome-startup-timeout), [`--no-browser-critical-css`](#--no-browser-critical-css), [`--no-browser-lazy-loading`](#--no-browser-lazy-loading), [`--no-browser-lcp-preload`](#--no-browser-lcp-preload), [`--no-browser-image-sizing`](#--no-browser-image-sizing), [`--browser-queue-size`](#--browser-queue-size), [`--browser-profile-ttl`](#--browser-profile-ttl), [`--allow-private-urls`](#--allow-private-urls), [`--version`](#--version), [`--help`](#--help)

#### --socket {#--socket}

- **Syntax:** `--socket PATH`
- **Default:** `/run/pagespeed-optimizer/notify.sock`

Unix socket path (default: /run/pagespeed-optimizer/notify.sock)

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

#### --cache-dir {#--cache-dir}

- **Syntax:** `--cache-dir DIR`
- **Default:** `/var/cache/pagespeed-optimizer/v3`

Cache directory; the volume, shared config and serve-stats live inside it (default: /var/cache/pagespeed-optimizer/v3)

```bash
factory_worker --cache-dir /var/cache/pagespeed-optimizer/v3
```

The `v3` in the default is the compiled-in cache-directory generation. The
directory must exist, be writable by the worker, and contain only files owned by
the worker's user; otherwise the worker refuses to start and names the cause.
The packaged tmpfiles.d drop-in creates the default as 3770
`pagespeed:pagespeed` (setgid so the group is inherited, sticky so one group
member cannot remove another's files).

#### --cache-path {#--cache-path}

- **Syntax:** `--cache-path PATH`
- **Default:** —

Expert override: full cache volume file STEM (deliberately extensionless); wins over --cache-dir

```bash
factory_worker --cache-path /var/cache/pagespeed-optimizer/v3/cache
```

Must match the nginx [`pagespeed_cache_path`](#pagespeed_cache_path) directive.
The worker reads original content from this file and writes optimized variants
back to it.

#### --cache-size {#--cache-size}

- **Syntax:** `--cache-size BYTES`
- **Default:** `1073741824`

Cache size in bytes (default: 1073741824)

```bash
factory_worker --cache-size 536870912  # 512 MB
```

Total size of the Cyclone cache volume. The cache evicts least recently used
entries when full. See [Sizing the cache](#sizing-the-cache).

#### --ram-cache-size {#--ram-cache-size}

- **Syntax:** `--ram-cache-size BYTES`
- **Default:** `67108864`

RAM cache size in bytes, 0 to disable (default: 67108864)

The RAM tier uses write-around semantics: writes go to the volume, reads
populate the RAM cache on a miss, so nginx and the worker never see stale copies
of each other's writes. See [Threading and memory](#threading).

#### --read-lease-duration {#--read-lease-duration}

- **Syntax:** `--read-lease-duration MS`
- **Default:** `5000`

Cache read-lease duration for wrap deferral, 0 to disable leases (default: 5000)

```bash
factory_worker --read-lease-duration 0  # disable read leases
```

While a lease is live the cache defers overwriting that region, so in-flight
zero-copy responses are never served corrupted bytes. `0` disables leases
(writes are never deferred). On a full cache under sustained read traffic,
leases can delay new writes to a hot region; lowering or disabling them favors
write throughput over zero-copy read protection.

#### --lease-wrap-ceiling {#--lease-wrap-ceiling}

- **Syntax:** `--lease-wrap-ceiling MS`
- **Default:** `60000`

Max continuous wrap deferral before a wrap is forced (default: 60000)

```bash
factory_worker --lease-wrap-ceiling 30000
```

Only relevant when read leases are enabled. Lower values favor write admission
at a full cache; responses that hold cache content longer than this ceiling fall
back to copying.

#### --num-threads {#--num-threads}

- **Syntax:** `--num-threads N`
- **Default:** `auto`

Thread pool size for notification processing (default: auto)

`auto` sizes the pool from the CPUs the worker may use, following the CPU limit of its container or cgroup. See
[Threading and memory](#threading).

#### --max-connections {#--max-connections}

- **Syntax:** `--max-connections N`
- **Default:** `128`

Max simultaneous connections (default: 128)

```bash
factory_worker --max-connections 256
```

When the limit is reached, new connections are accepted and immediately closed.
Monitor the [health check](#health-check) to track connection usage.

#### --max-buffer-size {#--max-buffer-size}

- **Syntax:** `--max-buffer-size BYTES`
- **Default:** `1048576`

Max per-client buffer size (default: 1048576)

```bash
factory_worker --max-buffer-size 2097152  # 2 MB
```

Connections exceeding this buffer size are disconnected, so a malformed or
oversized message cannot exhaust memory.

#### --connection-timeout {#--connection-timeout}

- **Syntax:** `--connection-timeout MS`
- **Default:** `30000`

Idle connection timeout in ms (default: 30000)

```bash
factory_worker --connection-timeout 60000  # 60 seconds
```

Connections that send no data within the timeout are closed.

#### --shutdown-timeout {#--shutdown-timeout}

- **Syntax:** `--shutdown-timeout MS`
- **Default:** `5000`

Graceful shutdown timeout in ms (default: 5000)

```bash
factory_worker --shutdown-timeout 10000  # 10 seconds
```

On SIGTERM or SIGINT the worker stops accepting new connections and waits up to
this long for active connections to drain before force-closing them.

#### --disable-html {#--disable-html}

- **Syntax:** `--disable-html`
- **Default:** —

Disable HTML optimization

HTML notifications from nginx are ignored: no critical-CSS inlining, no HTML
transforms, no HTML caching.

#### --disable-css {#--disable-css}

- **Syntax:** `--disable-css`
- **Default:** —

Disable CSS minification

CSS notifications are ignored.

#### --disable-js {#--disable-js}

- **Syntax:** `--disable-js`
- **Default:** —

Disable JS minification

JavaScript notifications are ignored.

#### --disable-image {#--disable-image}

- **Syntax:** `--disable-image`
- **Default:** —

Disable image transcoding

Image notifications are ignored.

#### --proactive-image-variants {#--proactive-image-variants}

- **Syntax:** `--proactive-image-variants`
- **Default:** `off`

Enable proactive multi-format image generation (default: off)

Without this flag the worker produces only the single format the notification
asked for (for example only WebP). With it, one decode yields WebP, AVIF and an
optimized original. See [Proactive variant generation](#proactive-variant-generation).

#### --no-proactive-viewport-variants {#--no-proactive-viewport-variants}

- **Syntax:** `--no-proactive-viewport-variants`
- **Default:** —

Disable proactive viewport sibling generation

By default variants for all three viewport classes (mobile, tablet, desktop) are
generated from one decode, resized to each viewport's target width. With this
flag only the viewport in the notification is produced.

#### --no-proactive-savedata-variants {#--no-proactive-savedata-variants}

- **Syntax:** `--no-proactive-savedata-variants`
- **Default:** —

Disable proactive Save-Data sibling generation

By default both normal-quality and Save-Data (lower-quality) variants are
generated from each decode. With this flag only the notification's Save-Data
preference is produced. The Save-Data quality levels are the `--savedata-*-quality`
flags.

#### --no-proactive-density-variants {#--no-proactive-density-variants}

- **Syntax:** `--no-proactive-density-variants`
- **Default:** —

Disable proactive pixel density sibling generation

By default variants for both 1x and 2x+ pixel density are generated from each
decode; for 2x+ the viewport target width is doubled before resizing (a
mobile/2x variant targets 960px instead of 480px). With this flag only the
density in the notification is produced.

#### --enable-warmup {#--enable-warmup}

- **Syntax:** `--enable-warmup`
- **Default:** `off`

Enable hot URL variant warmup (default: off)

When nginx detects a URL exceeding [`pagespeed_hot_threshold`](#pagespeed_hot_threshold)
it sends a warmup sentinel to the worker. With warmup enabled, the worker
pre-generates all missing variants for that URL; without it, warmup
notifications are ignored.

#### --no-lazy-load-images {#--no-lazy-load-images}

- **Syntax:** `--no-lazy-load-images`
- **Default:** —

Disable loading="lazy" injection

See [Lazy load images](#lazy-load-images) for what is skipped (the LCP
candidate, the first body images, the first iframe, invisible images).

#### --no-image-dimensions {#--no-image-dimensions}

- **Syntax:** `--no-image-dimensions`
- **Default:** —

Disable image width/height injection

See [Image dimensions](#image-dimensions).

#### --no-lcp-preload {#--no-lcp-preload}

- **Syntax:** `--no-lcp-preload`
- **Default:** —

Disable LCP image preload injection

See [LCP preload](#lcp-preload).

#### --no-preconnect-injection {#--no-preconnect-injection}

- **Syntax:** `--no-preconnect-injection`
- **Default:** —

Disable preconnect hint injection

See [Preconnect injection](#preconnect-injection).

#### --no-async-css {#--no-async-css}

- **Syntax:** `--no-async-css`
- **Default:** —

Disable async CSS loading (keep stylesheets render-blocking and inline no above-the-fold CSS)

Enabled does not mean applied: see [Async CSS](#async-css) for the per-page
conditions deferral requires.

#### --async-css-min-coverage {#--async-css-min-coverage}

- **Syntax:** `--async-css-min-coverage F`
- **Default:** `0.10`

Min critical/total CSS coverage to defer a stylesheet, 0-1 (default: 0.10; 0 disables the gate)

A stylesheet is deferred only when the critical rules cover at least this share
of it; `0` disables the gate. See [Async CSS](#async-css).

#### --async-css-min-deferred-bytes {#--async-css-min-deferred-bytes}

- **Syntax:** `--async-css-min-deferred-bytes N`
- **Default:** `15000`

Sheets below this size always defer regardless of coverage (default: 15000)

Stylesheets below this size defer regardless of coverage. See
[Async CSS](#async-css).

#### --enable-speculation-rules {#--enable-speculation-rules}

- **Syntax:** `--enable-speculation-rules`
- **Default:** `off`

Enable speculation rules injection (default: off)

#### --no-content-analysis {#--no-content-analysis}

- **Syntax:** `--no-content-analysis`
- **Default:** —

Disable content-aware quality presets

Falls back to the `Unknown` content class for every image. See
[Content analysis](#content-analysis-and-denoising).

#### --no-preserve-c2pa {#--no-preserve-c2pa}

- **Syntax:** `--no-preserve-c2pa`
- **Default:** —

Disable C2PA/Content Credentials provenance preservation (on by default)

#### --c2pa-carry {#--c2pa-carry}

- **Syntax:** `--c2pa-carry`
- **Default:** —

Recompress manifest-bearing PNGs and re-splice C2PA chunks (PNG only; default: off, serve original)

#### --denoise-threshold {#--denoise-threshold}

- **Syntax:** `--denoise-threshold LEVEL`
- **Default:** —

Deprecated and ignored (accepted so existing command lines keep working)

Removed: noisy images are no longer smoothed before encoding. The flag is
still accepted and ignored.

#### --denoise-sigma-spatial {#--denoise-sigma-spatial}

- **Syntax:** `--denoise-sigma-spatial F`
- **Default:** —

Deprecated and ignored

Removed. Still accepted and ignored.

#### --denoise-sigma-range {#--denoise-sigma-range}

- **Syntax:** `--denoise-sigma-range F`
- **Default:** —

Deprecated and ignored

#### --no-quality-verify {#--no-quality-verify}

- **Syntax:** `--no-quality-verify`
- **Default:** —

Disable SSIMULACRA2 quality verification

Skips the SSIMULACRA2 measurement pass entirely, saving CPU at the risk of
inconsistent visual quality.

#### --target-ssimulacra2 {#--target-ssimulacra2}

- **Syntax:** `--target-ssimulacra2 SCORE`
- **Default:** `70`

Target SSIMULACRA2 score 0-100 (default: 70)

Range 0-100. See [SSIMULACRA2 quality tuning](#ssimulacra2-quality-tuning).

#### --ssimulacra2-tolerance {#--ssimulacra2-tolerance}

- **Syntax:** `--ssimulacra2-tolerance F`
- **Default:** `5.0`

Acceptable score deviation (default: 5.0)

The band is asymmetric: `[target - 0.6 * tolerance, target + 1.6 * tolerance]`.
See [SSIMULACRA2 quality tuning](#ssimulacra2-quality-tuning).

#### --no-learned-quality {#--no-learned-quality}

- **Syntax:** `--no-learned-quality`
- **Default:** —

Disable ML quality prediction

Falls back to the heuristic quality calculation for every format. See
[Learned quality prediction](#learned-quality-prediction).

#### --no-learned-quality-jpeg {#--no-learned-quality-jpeg}

- **Syntax:** `--no-learned-quality-jpeg`
- **Default:** —

Disable ML prediction for JPEG

See [Learned quality prediction](#learned-quality-prediction).

#### --no-learned-quality-webp {#--no-learned-quality-webp}

- **Syntax:** `--no-learned-quality-webp`
- **Default:** —

Disable ML prediction for WebP

See [Learned quality prediction](#learned-quality-prediction).

#### --no-learned-quality-avif {#--no-learned-quality-avif}

- **Syntax:** `--no-learned-quality-avif`
- **Default:** —

Disable ML prediction for AVIF

See [Learned quality prediction](#learned-quality-prediction).

#### --savedata-score-reduction {#--savedata-score-reduction}

- **Syntax:** `--savedata-score-reduction F`
- **Default:** `15.0`

Target SSIMULACRA2 reduction for Save-Data (default: 15.0)

SSIMULACRA2 points subtracted from the target for Save-Data variants.

#### --gzip-level {#--gzip-level}

- **Syntax:** `--gzip-level N`
- **Default:** `6`

Gzip compression level 1-9 (default: 6, 0=disable)

Range 1-9; `0` disables gzip alternates. See
[Pre-compressed text variants](#pre-compressed-text-variants).

#### --brotli-level {#--brotli-level}

- **Syntax:** `--brotli-level N`
- **Default:** `6`

Brotli compression quality 1-11 (default: 6, 0=disable)

Range 1-11; `0` disables brotli alternates. See
[Pre-compressed text variants](#pre-compressed-text-variants).

#### --no-css-import-flattening {#--no-css-import-flattening}

- **Syntax:** `--no-css-import-flattening`
- **Default:** —

Disable CSS @import flattening

See [CSS import flattening](#css-import-flattening).

#### --mobile-width {#--mobile-width}

- **Syntax:** `--mobile-width PIXELS`
- **Default:** `480`

Mobile viewport resize width (default: 480, 0=disable)

Images wider than the target are downscaled, preserving aspect ratio; images
already smaller are not resized. `0` disables resizing for the viewport. See
[Viewport resize widths](#viewport-resize-widths).

#### --tablet-width {#--tablet-width}

- **Syntax:** `--tablet-width PIXELS`
- **Default:** `768`

Tablet viewport resize width (default: 768, 0=disable)

See [Viewport resize widths](#viewport-resize-widths).

#### --desktop-width {#--desktop-width}

- **Syntax:** `--desktop-width PIXELS`
- **Default:** `0 (no resize)`

Desktop viewport resize width (default: 0=no resize)

The default `0` means desktop images are not resized. See
[Viewport resize widths](#viewport-resize-widths).

#### --jpeg-quality {#--jpeg-quality}

- **Syntax:** `--jpeg-quality N`
- **Default:** `85`

JPEG output quality 1-100 (default: 85)

Range 1-100. See [Image quality](#image-quality-flags).

#### --webp-quality {#--webp-quality}

- **Syntax:** `--webp-quality N`
- **Default:** `75`

WebP output quality 0-100 (default: 75)

Range 0-100. See [Image quality](#image-quality-flags).

#### --avif-quality {#--avif-quality}

- **Syntax:** `--avif-quality N`
- **Default:** `60`

AVIF output quality 0-100 (default: 60)

Range 0-100. See [Image quality](#image-quality-flags).

#### --savedata-jpeg-quality {#--savedata-jpeg-quality}

- **Syntax:** `--savedata-jpeg-quality N`
- **Default:** `60`

Save-Data JPEG quality (default: 60)

Used when the request carries `Save-Data: on`. Range 1-100.

#### --savedata-webp-quality {#--savedata-webp-quality}

- **Syntax:** `--savedata-webp-quality N`
- **Default:** `50`

Save-Data WebP quality (default: 50)

Used when the request carries `Save-Data: on`. Range 0-100.

#### --savedata-avif-quality {#--savedata-avif-quality}

- **Syntax:** `--savedata-avif-quality N`
- **Default:** `45`

Save-Data AVIF quality (default: 45)

Used when the request carries `Save-Data: on`. Range 0-100.

#### --max-url-length {#--max-url-length}

- **Syntax:** `--max-url-length BYTES`
- **Default:** `8192`

Max URL length (default: 8192)

Notifications with longer URLs are rejected.

#### --max-html-size {#--max-html-size}

- **Syntax:** `--max-html-size BYTES`
- **Default:** `5242880`

Max HTML size (default: 5242880)

Larger HTML documents are skipped.

#### --max-css-size {#--max-css-size}

- **Syntax:** `--max-css-size BYTES`
- **Default:** `2097152`

Max CSS size (default: 2097152)

#### --max-js-size {#--max-js-size}

- **Syntax:** `--max-js-size BYTES`
- **Default:** `2097152`

Max JS size (default: 2097152)

#### --max-image-size {#--max-image-size}

- **Syntax:** `--max-image-size BYTES`
- **Default:** `10485760`

Max image size (default: 10485760)

#### --log-level {#--log-level}

- **Syntax:** `--log-level LEVEL`
- **Default:** `info`

Log level: debug|info|warning|error (default: info)

```bash
factory_worker --log-level warning
```

#### --log-format {#--log-format}

- **Syntax:** `--log-format FORMAT`
- **Default:** `text`

Log format: text|json (default: text)

```bash
factory_worker --log-format json
```

`text` is human-readable (`[INFO] message text`); `json` is one object per line
(`{"timestamp":"...","level":"INFO","message":"..."}`) and is the better choice
with log aggregation.

#### --api-socket {#--api-socket}

- **Syntax:** `--api-socket [PATH]`
- **Default:** `/run/pagespeed-optimizer/api.sock`

Serve the management API over a unix socket, mode 0660 (default path: /run/pagespeed-optimizer/api.sock)

The recommended local transport: filesystem permission (group `pagespeed`) is
the credential, the worker binds no TCP port, and no bearer token is asked for
on the socket. See [HTTP management API](#http-management-api).

#### --api-port {#--api-port}

- **Syntax:** `--api-port PORT`
- **Default:** `0 (disabled)`

HTTP management API TCP port (default: 0=disabled)

`0` keeps the TCP API off. `--api-socket` and `--api-port` are mutually
exclusive: the worker serves exactly one transport. See
[HTTP management API](#http-management-api).

#### --api-bind {#--api-bind}

- **Syntax:** `--api-bind ADDRESS`
- **Default:** `127.0.0.1`

API bind address (default: 127.0.0.1)

An IPv4 literal; `localhost`, `::1` and other IPv6 forms are rejected by name.
A non-loopback bind also needs `--api-allow-remote` and a token.

#### --api-allow-remote {#--api-allow-remote}

- **Syntax:** `--api-allow-remote`
- **Default:** —

Permit a non-loopback --api-bind (a token is still required)

Confirms a deliberate non-loopback bind. Remote is never unauthenticated: a
token is still required. See
[One invariant, enforced at startup](#one-invariant-enforced-at-startup).

#### --api-no-auth {#--api-no-auth}

- **Syntax:** `--api-no-auth`
- **Default:** —

Permit a tokenless API on loopback or the unix socket

Confirms a deliberate tokenless API on loopback or the unix socket; the worker
prints a warning banner at startup.

#### --api-token {#--api-token}

- **Syntax:** `--api-token TOKEN`
- **Default:** —

API auth token (or set PAGESPEED_API_TOKEN env var)

Prefer the `PAGESPEED_API_TOKEN` environment variable: anything on the command
line is readable by any local user through `/proc/<pid>/cmdline`. Package
installs generate a token into `/etc/pagespeed-optimizer/daemon.env`.

#### --api-read-open {#--api-read-open}

- **Syntax:** `--api-read-open`
- **Default:** —

Allow unauthenticated access to the allow-listed GET routes and the stats/events streams (public console reads; the log stays token-only)

Opens an explicit allow-list of GET endpoints and WebSocket streams (the
cached-URL inventory, origin hosts, the cooldown table, the live stats and
events streams) without the token; the log endpoint and stream stay token-only.
Use it only behind a reverse proxy that does its own authentication.

#### --no-security-headers {#--no-security-headers}

- **Syntax:** `--no-security-headers`
- **Default:** —

Omit security headers on console responses (reverse proxy handles them)

Omit the security headers the worker otherwise adds to console responses, for
deployments where a reverse proxy sets them.

#### --console-dir {#--console-dir}

- **Syntax:** `--console-dir PATH`
- **Default:** —

Web console SPA directory (optional)

Path of the web console's static files. See [the admin console](/docs/admin-console/).

#### --enable-browser-analysis {#--enable-browser-analysis}

- **Syntax:** `--enable-browser-analysis`
- **Default:** `off`

Enable browser-based analysis (default: off)

Chrome (or `chrome-headless-shell`) must be available at `--chrome-binary`. See
[Browser analysis](#browser-analysis) and the
[browser analysis guide](/docs/browser-analysis/).

#### --browser-sandbox {#--browser-sandbox}

- **Syntax:** `--browser-sandbox MODE`
- **Default:** `require`

Headless Chrome sandbox: require|off (default: require; `require` refuses browser analysis rather than running an unsandboxed browser -- the worker keeps serving)

There is no `auto`. With `require` the worker probes at startup and refuses
browser analysis, rather than running an unsandboxed browser, when the sandbox
is unavailable; the worker keeps serving without it. `off` is a deliberate
opt-out and logs a warning at startup and on every browser start. See
[Headless browser sandbox](#headless-browser-sandbox).

#### --browser-user-data-dir {#--browser-user-data-dir}

- **Syntax:** `--browser-user-data-dir PATH`
- **Default:** `<runtime dir>/chrome`

Chrome profile directory (default: &lt;runtime dir&gt;/chrome)

Chrome profile directory; defaults to a `chrome` directory under the worker's
runtime directory.

#### --chrome-binary {#--chrome-binary}

- **Syntax:** `--chrome-binary PATH`
- **Default:** `/usr/bin/chrome-headless-shell`

Chrome executable path (default: /usr/bin/chrome-headless-shell)

#### --chrome-recycle-interval {#--chrome-recycle-interval}

- **Syntax:** `--chrome-recycle-interval N`
- **Default:** `100`

Pages before Chrome restart (default: 100)

Chrome is restarted after this many pages to bound memory growth. See
[Chrome memory management](#chrome-memory-management).

#### --chrome-page-timeout {#--chrome-page-timeout}

- **Syntax:** `--chrome-page-timeout MS`
- **Default:** `60000`

Per-page CDP timeout (default: 60000)

#### --chrome-max-memory {#--chrome-max-memory}

- **Syntax:** `--chrome-max-memory MB`
- **Default:** `512`

Chrome RSS kill threshold (default: 512)

On Linux the worker watches Chrome's RSS through `/proc` and kills the process
above this threshold; elsewhere only page-count recycling applies.

#### --chrome-startup-timeout {#--chrome-startup-timeout}

- **Syntax:** `--chrome-startup-timeout MS`
- **Default:** `10000`

Chrome startup timeout (default: 10000)

#### --no-browser-critical-css {#--no-browser-critical-css}

- **Syntax:** `--no-browser-critical-css`
- **Default:** —

Disable browser CSS (keep heuristic)

Keeps the heuristic critical-CSS extraction even when browser analysis runs.

#### --no-browser-lazy-loading {#--no-browser-lazy-loading}

- **Syntax:** `--no-browser-lazy-loading`
- **Default:** —

Disable browser fold detection

#### --no-browser-lcp-preload {#--no-browser-lcp-preload}

- **Syntax:** `--no-browser-lcp-preload`
- **Default:** —

Disable browser LCP detection

#### --no-browser-image-sizing {#--no-browser-image-sizing}

- **Syntax:** `--no-browser-image-sizing`
- **Default:** —

Disable browser image dimensions

#### --browser-queue-size {#--browser-queue-size}

- **Syntax:** `--browser-queue-size N`
- **Default:** `1000`

Max pending analysis items (default: 1000)

#### --browser-profile-ttl {#--browser-profile-ttl}

- **Syntax:** `--browser-profile-ttl SEC`
- **Default:** `86400`

Profile expiry in seconds (default: 86400)

Optimization profiles expire after this many seconds and are re-analyzed.

#### --allow-private-urls {#--allow-private-urls}

- **Syntax:** `--allow-private-urls`
- **Default:** —

Allow capture endpoints to target private/loopback URLs

Off by default for security. Enable only in development, or when the worker
has to analyze sites on private networks.

#### --version {#--version}

- **Syntax:** `--version`
- **Default:** —

Show version and exit

#### --help {#--help}

- **Syntax:** `--help`
- **Default:** —

Show this help

### Windows service {#flags-windows-service}

[`--service`](#--service), [`--log-file`](#--log-file)

#### --service {#--service}

- **Syntax:** `--service`
- **Default:** —
- **Platform:** Windows service only

Run under the Service Control Manager

Runs the worker under the Windows Service Control Manager; used by the IIS
installer's service registration.

#### --log-file {#--log-file}

- **Syntax:** `--log-file PATH`
- **Default:** —
- **Platform:** Windows service only

With --service: append the log to PATH

With `--service`: appends the log to the given file instead of standard error.

### Cache key normalization {#flags-cache-key-normalization}

[`--strip-query-extensions`](#--strip-query-extensions), [`--strip-query-groups`](#--strip-query-groups), [`--strip-query-params`](#--strip-query-params), [`--host-alias`](#--host-alias)

#### --strip-query-extensions {#--strip-query-extensions}

- **Syntax:** `--strip-query-extensions EXT`
- **Default:** —

Comma-separated extensions (e.g., .jpg,.png,.css)

```bash
factory_worker --strip-query-extensions css,js,png,jpg,webp
```

Consolidates cache entries for static assets that differ only by a cache-busting
query string. The whole query string is dropped for every listed extension, on
every site the worker serves, so list an extension only when no site's server
returns different content for it depending on the query string. See
[URL normalization](#url-normalization).

#### --strip-query-groups {#--strip-query-groups}

- **Syntax:** `--strip-query-groups GROUPS`
- **Default:** —

Comma-separated group names (e.g., images,static)

```bash
factory_worker --strip-query-groups static
```

A group stands for a fixed list of extensions (`images`, `static`) and works
exactly like listing them with
[`--strip-query-extensions`](#--strip-query-extensions), on every site the
worker serves. See [URL normalization](#url-normalization).

#### --strip-query-params {#--strip-query-params}

- **Syntax:** `--strip-query-params PARAMS`
- **Default:** —

Comma-separated param names (e.g., utm_source,fbclid)

```bash
factory_worker --strip-query-params utm_source,utm_medium,fbclid
```

Removes only the named parameters and keeps the rest of the query string, on
every site the worker serves. This is the right setting for tracking parameters
such as `utm_source` or `fbclid`: list only parameters that no site's server
uses to choose what it returns. See [URL normalization](#url-normalization).

#### --host-alias {#--host-alias}

- **Syntax:** `--host-alias SRC=DST`
- **Default:** —

Host alias mapping (repeatable)

```bash
factory_worker --host-alias www.example.com=example.com
```

Requests for the source host are treated as the destination host in cache
lookups. Repeatable.

### SVG auto-vectorization {#flags-svg-auto-vectorization}

[`--svg-mode`](#--svg-mode), [`--svg-candidacy-threshold`](#--svg-candidacy-threshold), [`--svg-max-pixels`](#--svg-max-pixels), [`--svg-max-paths`](#--svg-max-paths), [`--svg-max-svg-bytes`](#--svg-max-svg-bytes), [`--svg-fidelity-threshold`](#--svg-fidelity-threshold), [`--svg-exclude-lcp`](#--svg-exclude-lcp), [`--svg-timeout-ms`](#--svg-timeout-ms), [`--svg-preset`](#--svg-preset), [`--svg-color-precision`](#--svg-color-precision), [`--svg-filter-speckle`](#--svg-filter-speckle)

#### --svg-mode {#--svg-mode}

- **Syntax:** `--svg-mode MODE`
- **Default:** `detect`

detect, preview, or auto (default: detect)

Start with `detect` to see which images qualify, move to `preview` to inspect
the stored SVGs, then `auto` to serve them. See
[SVG auto-vectorization](#svg-auto-vectorization).

#### --svg-candidacy-threshold {#--svg-candidacy-threshold}

- **Syntax:** `--svg-candidacy-threshold N`
- **Default:** `50`

Score threshold 0-100 (default: 50)

#### --svg-max-pixels {#--svg-max-pixels}

- **Syntax:** `--svg-max-pixels N`
- **Default:** `65536`

Max decoded pixels (default: 65536)

#### --svg-max-paths {#--svg-max-paths}

- **Syntax:** `--svg-max-paths N`
- **Default:** `500`

Max &lt;path&gt; elements (default: 500)

#### --svg-max-svg-bytes {#--svg-max-svg-bytes}

- **Syntax:** `--svg-max-svg-bytes N`
- **Default:** `262144`

Max uncompressed SVG bytes (default: 262144)

#### --svg-fidelity-threshold {#--svg-fidelity-threshold}

- **Syntax:** `--svg-fidelity-threshold F`
- **Default:** `55.0`

SSIMULACRA2 minimum (default: 55.0) [reserved]

Reserved for a later release; accepted but not yet applied.

#### --svg-exclude-lcp {#--svg-exclude-lcp}

- **Syntax:** `--svg-exclude-lcp BOOL`
- **Default:** `true`

Skip LCP images (default: true)

Enabled by default because SVG render cost (path tessellation in the browser)
can regress LCP for hero images. Set it to `false` when your LCP images are
simple graphics that benefit from vectorization.

#### --svg-timeout-ms {#--svg-timeout-ms}

- **Syntax:** `--svg-timeout-ms N`
- **Default:** `500`

Vectorization timeout (default: 500) [reserved]

Reserved for a later release; accepted but not yet applied.

#### --svg-preset {#--svg-preset}

- **Syntax:** `--svg-preset N`
- **Default:** `1`

0=bw, 1=poster, 2=photo (default: 1) [reserved]

Reserved for a later release; accepted but not yet applied.

#### --svg-color-precision {#--svg-color-precision}

- **Syntax:** `--svg-color-precision N`
- **Default:** `0`

0=adaptive, 1-8=fixed (default: 0)

#### --svg-filter-speckle {#--svg-filter-speckle}

- **Syntax:** `--svg-filter-speckle N`
- **Default:** `4`

Min cluster area (default: 4)

### Agent optimize (experimental, off by default) {#flags-agent-optimize-experimental-off-by-default}

[`--agent-optimize`](#--agent-optimize), [`--agent-optimize-paths`](#--agent-optimize-paths), [`--agent-optimize-llms-txt`](#--agent-optimize-llms-txt), [`--agent-optimize-sitemap-url`](#--agent-optimize-sitemap-url), [`--[no-]agent-optimize-respect-ai-directives`](#--no-agent-optimize-respect-ai-directives), [`--agent-optimize-llms-summary-fetch-cap`](#--agent-optimize-llms-summary-fetch-cap), [`--agent-optimize-cache-ttl`](#--agent-optimize-cache-ttl), [`--agent-render-allow-hosts`](#--agent-render-allow-hosts)

#### --agent-optimize {#--agent-optimize}

- **Syntax:** `--agent-optimize`
- **Default:** —

Serve a rendered-markdown variant to AI agents (Accept: text/markdown). Behavior may change.

Experimental; behavior may change between releases. See
[Agent optimize](/docs/agent-optimize/).

#### --agent-optimize-paths {#--agent-optimize-paths}

- **Syntax:** `--agent-optimize-paths LIST`
- **Default:** —

Comma-separated path prefixes to enable

#### --agent-optimize-llms-txt {#--agent-optimize-llms-txt}

- **Syntax:** `--agent-optimize-llms-txt`
- **Default:** —

Publish a synthesized /llms.txt site index (requires --agent-optimize)

#### --agent-optimize-sitemap-url {#--agent-optimize-sitemap-url}

- **Syntax:** `--agent-optimize-sitemap-url URL`
- **Default:** —

Sitemap to build /llms.txt from

#### --[no-]agent-optimize-respect-ai-directives {#--no-agent-optimize-respect-ai-directives}

- **Syntax:** `--[no-]agent-optimize-respect-ai-directives`
- **Default:** `on`

Honor robots AI rules (default: on)

#### --agent-optimize-llms-summary-fetch-cap {#--agent-optimize-llms-summary-fetch-cap}

- **Syntax:** `--agent-optimize-llms-summary-fetch-cap N`
- **Default:** `200`

Max summary fetches (default: 200)

#### --agent-optimize-cache-ttl {#--agent-optimize-cache-ttl}

- **Syntax:** `--agent-optimize-cache-ttl SEC`
- **Default:** `86400`

/llms.txt cache TTL (default: 86400)

#### --agent-render-allow-hosts {#--agent-render-allow-hosts}

- **Syntax:** `--agent-render-allow-hosts HOSTS`
- **Default:** `none`

Third-party hosts the agent render may fetch: comma-separated, exact host names (no wildcards), still SSRF-guarded (default: none)

### Web Bot Auth (observe-only, off by default) {#flags-web-bot-auth-observe-only-off-by-default}

[`--web-bot-auth`](#--web-bot-auth), [`--web-bot-auth-key-directory`](#--web-bot-auth-key-directory), [`--web-bot-auth-verified-bots`](#--web-bot-auth-verified-bots), [`--web-bot-auth-public-counter`](#--web-bot-auth-public-counter)

#### --web-bot-auth {#--web-bot-auth}

- **Syntax:** `--web-bot-auth`
- **Default:** —

Classify RFC 9421-signed agent requests (label + counter only; never changes request handling)

Observe-only: labels and counts signed agent requests and never changes request
handling. See [Web Bot Auth](/docs/web-bot-auth/).

#### --web-bot-auth-key-directory {#--web-bot-auth-key-directory}

- **Syntax:** `--web-bot-auth-key-directory URL`
- **Default:** —

https JWKS key-directory URL to warm-fetch periodically (repeatable)

#### --web-bot-auth-verified-bots {#--web-bot-auth-verified-bots}

- **Syntax:** `--web-bot-auth-verified-bots LIST`
- **Default:** —

keyid=name pairs (comma-separated) promoting a verified signature to a named bot

#### --web-bot-auth-public-counter {#--web-bot-auth-public-counter}

- **Syntax:** `--web-bot-auth-public-counter MODE`
- **Default:** —

opt-in verified-crawl counter (experimental): off (default) | private | public. Enabling a non-off mode publishes a discoverable marker at /.well-known/webbotauth-counter. The exact doc is gated by the PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN env bearer token.

### RSL-CAP enforcement (experimental, off by default) {#flags-rsl-cap-enforcement-experimental-off-by-default}

[`--rsl-cap-enforcement`](#--rsl-cap-enforcement), [`--rsl-cap-key-directory`](#--rsl-cap-key-directory), [`--rsl-cap-requested-license`](#--rsl-cap-requested-license), [`--rsl-cap-requested-scope`](#--rsl-cap-requested-scope), [`--rsl-cap-issuer`](#--rsl-cap-issuer)

#### --rsl-cap-enforcement {#--rsl-cap-enforcement}

- **Syntax:** `--rsl-cap-enforcement`
- **Default:** —

Validate Authorization: License capability tokens and answer 401, 402 or pass (status codes only; never settles or meters payment)

Experimental. In the container image, `PAGESPEED_RSL_CAP_ENFORCEMENT=true` sets
this flag. See [RSL-CAP enforcement](/docs/rsl-cap/).

#### --rsl-cap-key-directory {#--rsl-cap-key-directory}

- **Syntax:** `--rsl-cap-key-directory URL`
- **Default:** —

https JWKS key-directory URL of a token issuer to warm-fetch periodically (repeatable)

#### --rsl-cap-requested-license {#--rsl-cap-requested-license}

- **Syntax:** `--rsl-cap-requested-license ID`
- **Default:** —

License id a token must grant (empty: every valid token is refused with 402)

#### --rsl-cap-requested-scope {#--rsl-cap-requested-scope}

- **Syntax:** `--rsl-cap-requested-scope SCOPE`
- **Default:** —

Scope a token must grant (empty: every valid token is refused with 402)

#### --rsl-cap-issuer {#--rsl-cap-issuer}

- **Syntax:** `--rsl-cap-issuer ISSUER`
- **Default:** —

Accept tokens from this issuer only; others are rejected as an unknown issuer (401). Empty: any issuer in the key directories

<!-- generated:end worker-flags -->

## Proactive variant generation

When the worker receives an image notification, it can generate multiple
variants from a single decode pass. This avoids decoding the same image
repeatedly for different client types. Each dimension can be independently
enabled or disabled:
[`--proactive-image-variants`](#--proactive-image-variants),
[`--no-proactive-viewport-variants`](#--no-proactive-viewport-variants),
[`--no-proactive-savedata-variants`](#--no-proactive-savedata-variants) and
[`--no-proactive-density-variants`](#--no-proactive-density-variants).

Save-Data variants use lower compression quality to reduce bandwidth:

| Format | Normal quality | Save-Data quality |
| ------ | -------------- | ----------------- |
| JPEG   | 85             | 60                |
| WebP   | 75             | 50                |
| AVIF   | 60             | 45                |

These quality levels can be overridden at runtime with the
[image quality flags](#image-quality-flags).

### Variant matrix

With all proactive flags enabled, a single image notification can produce up to:

- 3 formats (WebP, AVIF, optimized original)
- 3 viewports (Mobile, Tablet, Desktop)
- 2 Save-Data states (off, on)
- 2 pixel densities (1x, 2x+)

That is up to **37 variants** (36 raster plus 1 SVG for eligible images) from a
single decode pass. In practice, many of these already exist in the cache and
are skipped, so the actual number of new writes is typically much smaller.

### Viewport resize widths

[`--mobile-width`](#--mobile-width), [`--tablet-width`](#--tablet-width) and
[`--desktop-width`](#--desktop-width) set the target width for viewport-based
image resizing. Images wider than the target are downscaled (preserving aspect
ratio). Images already smaller than the target are not resized.

```bash
factory_worker --mobile-width 480 --tablet-width 768 --desktop-width 0
```

## URL normalization

[`--strip-query-extensions`](#--strip-query-extensions),
[`--strip-query-groups`](#--strip-query-groups),
[`--strip-query-params`](#--strip-query-params) and
[`--host-alias`](#--host-alias) normalize the cache key: query strings that only
bust caches and host aliases that serve the same content collapse onto one
cache entry.

The query-string settings apply to every site the worker serves, not to one
site, and requests whose URLs differ only in what a setting removes share one
cache entry. Choose them with that in mind:

- Prefer [`--strip-query-params`](#--strip-query-params) for tracking
  parameters (`utm_source`, `utm_medium`, `fbclid`): it removes only the
  parameters you name.
- Use [`--strip-query-extensions`](#--strip-query-extensions) or
  [`--strip-query-groups`](#--strip-query-groups) only for file types whose
  query strings are cache-busters on every site the worker serves, such as a
  version number after a stylesheet or image URL.
- Leave out any parameter or file type for which a site's server returns
  different content depending on the query string, such as a theme, language or
  size parameter.

All three settings are empty by default: nothing is removed until you list it.

## Image quality {#image-quality-flags}

[`--jpeg-quality`](#--jpeg-quality), [`--webp-quality`](#--webp-quality) and
[`--avif-quality`](#--avif-quality) override the default encoding quality at
runtime for all image transcoding the worker performs;
[`--savedata-jpeg-quality`](#--savedata-jpeg-quality),
[`--savedata-webp-quality`](#--savedata-webp-quality) and
[`--savedata-avif-quality`](#--savedata-avif-quality) are used when the request
includes `Save-Data: on`.

```bash
factory_worker --cache-path /data/cache.vol \
  --jpeg-quality 80 --webp-quality 70 --avif-quality 55 \
  --savedata-jpeg-quality 55 --savedata-webp-quality 40 --savedata-avif-quality 40
```

## Learned quality prediction

Per-format ML models predict the optimal encoder quality parameter for a target
SSIMULACRA2 score. The models are trained LightGBM decision trees compiled to C
at build time — zero runtime dependencies, ~5 microsecond inference per format.

When enabled, the worker extracts 16 image features (edge density, noise level,
color entropy, spatial frequency, luminance, etc.) from the decoded pixels and
predicts the encoder quality that hits the configured `target_ssimulacra2` score.
SSIMULACRA2 verification still runs as a safety net: if the predicted quality
produces a score outside the tolerance band, the worker re-encodes.

When disabled ([`--no-learned-quality`](#--no-learned-quality), or per format
with [`--no-learned-quality-jpeg`](#--no-learned-quality-jpeg),
[`--no-learned-quality-webp`](#--no-learned-quality-webp) and
[`--no-learned-quality-avif`](#--no-learned-quality-avif)) or when a model
returns an invalid prediction (e.g., the AVIF model is not yet trained), the
worker falls back to the existing heuristic quality calculation based on content
class and base quality. [`--savedata-score-reduction`](#--savedata-score-reduction)
sets how many SSIMULACRA2 points Save-Data variants give up.

```bash
# Disable learned quality for AVIF (stub model), keep JPEG and WebP
factory_worker --cache-path /data/cache.vol --no-learned-quality-avif

# Disable all learned quality, fall back to heuristic
factory_worker --cache-path /data/cache.vol --no-learned-quality
```

## SSIMULACRA2 quality tuning

The worker uses SSIMULACRA2 perceptual quality scoring to verify that encoded
images meet a target visual quality. [`--target-ssimulacra2`](#--target-ssimulacra2)
sets the target score and [`--ssimulacra2-tolerance`](#--ssimulacra2-tolerance)
the band around it.

When verification is enabled (the default), the worker encodes at the predicted
or configured quality level, then measures the actual SSIMULACRA2 score. If the
score falls outside the asymmetric tolerance band
`[target - 0.6*tolerance, target + 1.6*tolerance]`, the worker re-encodes with
adjusted quality.

Disabling verification ([`--no-quality-verify`](#--no-quality-verify)) skips the
SSIMULACRA2 measurement pass entirely, reducing CPU cost at the risk of
inconsistent visual quality.

## Content analysis {#content-analysis-and-denoising}

The worker classifies image content (photo, screenshot, illustration, noisy) and
applies content-aware encoding presets. Images are not smoothed before
encoding. The flag is [`--no-content-analysis`](#--no-content-analysis); the
`--denoise-*` flags are deprecated and ignored.

Content analysis is fast (runs on the already-decoded pixel buffer) and feeds
into the learned quality model. Disabling it
falls back to the `Unknown` content class for all images.

## HTML transforms {#html-optimization-flags}

The HTML transforms are on by default and switched off individually with
[`--no-lazy-load-images`](#--no-lazy-load-images),
[`--no-image-dimensions`](#--no-image-dimensions),
[`--no-lcp-preload`](#--no-lcp-preload),
[`--no-preconnect-injection`](#--no-preconnect-injection),
[`--no-async-css`](#--no-async-css) and
[`--no-css-import-flattening`](#--no-css-import-flattening);
[`--enable-speculation-rules`](#--enable-speculation-rules) switches on the one
transform that is off by default. Script deferral has no flag of its own: it
runs as part of HTML optimization and is governed by browser script analysis.

Lazy loading skips the LCP candidate image (which gets `fetchpriority="high"`
instead), the first 3 body images when an LCP candidate is identified, and the
first iframe in the document body (typically an above-the-fold video or media
embed). Invisible images — 1x1 tracking pixels, `hidden` or `display:none`
elements — are left untouched entirely: never promoted, never lazy-loaded.

LCP preload injects a `<link rel="preload" as="image" fetchpriority="high">`
tag in the `<head>` and writes the URL to the Early Hints cache sentinel, so
nginx can emit `103 Early Hints` or `Link` response headers on subsequent
requests.

Preconnect injection detects third-party origins from external resources in the
HTML and writes `preconnect:` hints to the Early Hints sentinel. The hint
carries `crossorigin` exactly when the resource that motivated it is fetched in
CORS mode (fonts, `crossorigin`-marked resources, ES modules), so the warmed
connection is the one the browser actually reuses.

Async CSS loading makes render-blocking `<link rel="stylesheet">` tags
non-blocking. Each link is switched to `rel="preload" as="style"` so it
downloads at normal priority without blocking first paint; a small same-origin
loader script turns it back into a stylesheet, with its original media, once
the sheet has loaded. Because it is a standard preload, the deferred stylesheet
is also announced in early hints. The loader is served at a content-hashed path under
`/pagespeed_static/` (for example `/pagespeed_static/async_css.<hash>.js`; the
hash changes whenever the loader script changes, so the front-end serves it with
an immutable, year-long cache and a loader update lands as a fresh URL). It is
referenced with an external `<script defer src>`, not an inline `onload`
handler, so it works under a strict `script-src 'self'` Content-Security-Policy.
A `<noscript>` copy
preserves the stylesheet for clients without JavaScript. The inlined critical CSS
renders the page above the fold while the full sheet loads.

Because a stylesheet deferred behind an above-the-fold block that does not
actually cover the fold would flash unstyled content, this transform activates
only for a page whose above-the-fold appearance has been confirmed unchanged
with the stylesheet deferred, and only while that page still serves the exact
stylesheet the confirmation was made against — publish new styles and the check
runs again before deferral resumes. Everywhere the confirmation does not apply,
including a page whose browser analysis has not completed, the stylesheet stays
render-blocking and no above-the-fold CSS is inlined next to it. Disable the
transform entirely with `--no-async-css`.

Under a nonce-only or `strict-dynamic` CSP, host allowlists including `'self'`
are ignored, so the loader is blocked and deferred sheets stay non-applied for
JavaScript-enabled clients — turn async CSS off (`--no-async-css`) on such sites.

Script deferral adds the `defer` attribute to `<script src="...">` tags that
browser script analysis has identified as safe to defer. Scripts already marked
with `async`, `defer`, or `type="module"` are left unchanged. The worker uses
path-boundary suffix matching to map analysis results to script URLs across
pages.

## Transforms

Per-transform reference. Each entry names the optimizer worker's control
surface and the matching module filter(s), so operators coming from a
filter-based configuration can find the new knob and `/analyze` filter chips
can deep-link to a specific transform.

Toggleable transforms expose a `factory_worker --no-<name>` flag and are
enabled by default. Always-on transforms run from the master switch
(`pagespeed on;`) and cannot be turned off individually; the only way to
suppress them is the coarse `--disable-html` / `--disable-css` /
`--disable-js` / `--disable-image` family or per-location `pagespeed off;`.

### Image pipeline {#image-pipeline}

Decodes source images, applies content-aware quality, and
re-encodes to modern formats (WebP, AVIF). Emits viewport, density, and
Save-Data variants. Metadata (EXIF, ICC profiles other than sRGB) is
stripped. Always-on baseline under `pagespeed on;`; there is no
per-transform off switch. Use `--disable-image` to skip the image pipeline
entirely, or [`pagespeed_disallow`](#pagespeed_disallow) to exclude
specific URL patterns. The proactive variant family is tuned via the
[Proactive Variant Generation](#proactive-variant-generation) flags.
(module equivalent: `rewrite_images`, `convert_jpeg_to_webp`,
`convert_to_webp_lossless`, `recompress_jpeg`, `recompress_png`,
`recompress_webp`, `resize_images`, `resize_rendered_image_dimensions`,
`responsive_images`, `jpeg_subsampling`, `strip_image_meta_data`.)

### Image dimensions {#image-dimensions}

Adds `width` and `height` attributes to `<img>` tags that are missing
them, reserving layout space before the image loads and improving CLS.
Dimensions are read from the worker's cached image headers. Toggleable
via [`--no-image-dimensions`](#--no-image-dimensions). When
[browser analysis](#browser-analysis) is enabled, rendered dimensions
from headless Chrome take precedence over decoded pixel dimensions.
(module equivalent: `insert_image_dimensions`.)

### Lazy load images {#lazy-load-images}

Adds `loading="lazy"` to images and iframes that are below the
predicted fold. The LCP candidate is excluded (it gets
`fetchpriority="high"` instead), the first three body images are
skipped when an LCP candidate is identified, and the first iframe in
the document body is skipped, to avoid demoting above-the-fold
content. Invisible images (1x1 tracking pixels, hidden elements) are
never promoted and never lazy-loaded. Toggleable via
[`--no-lazy-load-images`](#--no-lazy-load-images). (module equivalent:
`lazyload_images`.)

### LCP preload {#lcp-preload}

Injects `<link rel="preload" as="image" fetchpriority="high">` in
`<head>` for the predicted LCP image and writes the URL to the Early
Hints cache sentinel, so nginx can emit `103 Early Hints` or `Link`
response headers on subsequent requests. Toggleable via
[`--no-lcp-preload`](#--no-lcp-preload). (module equivalent:
`hint_preload_subresources`.)

### Preconnect injection {#preconnect-injection}

Detects third-party origins referenced from external resources in the
HTML and writes `preconnect:` hints to the Early Hints sentinel, so
browsers can open TCP and TLS connections in parallel with the HTML
download. The hint carries `crossorigin` when the motivating resource
fetches in CORS mode (fonts, `crossorigin`-marked resources, ES
modules), matching the connection pool the browser will reuse.
Toggleable via
[`--no-preconnect-injection`](#--no-preconnect-injection). (module
equivalent: `insert_dns_prefetch`.)

### Critical CSS {#critical-css}

Extracts the CSS rules used by above-the-fold content and inlines them
in a `<style>` tag in `<head>`, so the browser can render the visible
viewport without waiting for external stylesheets. Always-on under
`pagespeed on;` (it is part of the HTML optimization pipeline disabled
only by `--disable-html`). When [browser analysis](#browser-analysis)
is enabled the critical set is derived from the CSS Coverage API at
three viewport sizes; otherwise a heuristic computes it from cached
stylesheet structure. Used together with [async CSS](#async-css) so the
remaining stylesheets stop blocking render. (module equivalent:
`prioritize_critical_css`.)

:::caution[Critical-CSS inlining can break dark mode]
Critical-CSS extraction analyses the page in its rendered state at extraction
time. That render happens without a `.dark` class on `<html>` (or whatever
toggles your dark theme), so the extractor never sees the `.dark` / `dark:`
selectors and never inlines them as critical CSS. Any dark-mode style on an
above-the-fold element then flashes the light value (or a missing element)
until the full stylesheet finishes loading. The fix is to add an inline
dark-mode override in the page `<head>` — a small `<style>` block that
hard-sets the dark values for critical-render-path elements, for example:

```html
<style>
  html.dark {
    background: #0c0a09;
  }
</style>
```

Add one override per dark-mode utility that lands on a critical element
(background, header/nav colors, logo `dark:hidden`/`dark:block` swaps, card
backgrounds). This is required whenever you ship both critical-CSS inlining and
a class-based dark theme.
:::

### Async CSS {#async-css}

Makes render-blocking `<link rel="stylesheet">` tags non-blocking by switching
each to `rel="preload" as="style"` and turning it back into a stylesheet, with
its real media, once it loads — driven by a
CSP-safe external loader served at a content-hashed path under
`/pagespeed_static/` (for example `/pagespeed_static/async_css.<hash>.js`, no
inline `onload` handler, so it works under `script-src 'self'`). A `<noscript>`
fallback preserves the stylesheet for clients without JavaScript. The inlined
critical CSS paints above the fold while the full sheet loads. Activates only
for a page whose above-the-fold appearance has been confirmed unchanged with the
stylesheet deferred, against the stylesheet it is currently serving; otherwise
the stylesheet stays render-blocking and the above-the-fold CSS is still
inlined. Toggleable via
[`--no-async-css`](#--no-async-css); the coverage gate is tuned with
[`--async-css-min-coverage`](#--async-css-min-coverage) and
[`--async-css-min-deferred-bytes`](#--async-css-min-deferred-bytes). (module
equivalent: `move_css_to_head`, `move_css_above_scripts`.)

### CSS import flattening {#css-import-flattening}

Inlines `@import` chains so the browser does not have to discover and
fetch each stylesheet sequentially. Flattening happens during CSS
processing and the resulting stylesheet is cached and served as a
single resource. Toggleable via
[`--no-css-import-flattening`](#--no-css-import-flattening). (module
equivalent: `flatten_css_imports`.)

### Script deferral {#script-deferral}

Adds the `defer` attribute to `<script src="...">` tags that script
coverage analysis has identified as safe to defer, so they execute
after HTML parsing instead of blocking it. Scripts already marked
`async`, `defer`, or `type="module"` are left unchanged. Runs as part of
HTML optimization when [browser analysis](#browser-analysis) has produced a
script profile; it has no flag of its own. (module equivalent:
`defer_javascript`.)

### CSS minification {#css-minification}

Parses CSS and emits a minified form (whitespace and comment removal,
shorthand collapsing where safe). The output is content-hashed and
served via [cache extension](#cache-extension). Always-on under
`pagespeed on;`; use `--disable-css` to skip CSS rewriting entirely.
(module equivalent: `rewrite_css`.)

### JS minification {#js-minification}

Parses JavaScript and emits a minified form (whitespace, identifier
shortening within safe scopes, dead-code removal where statically
provable). The output is content-hashed and served via
[cache extension](#cache-extension). Always-on under `pagespeed on;`;
use `--disable-js` to skip JS rewriting entirely. (module equivalent:
`rewrite_javascript`.)

### Cache extension {#cache-extension}

Rewrites references to static assets (CSS, JS, images, fonts) so they
point at content-hashed URLs the module serves with a long
`Cache-Control: max-age`. Because the URL changes when the byte content
changes, the long TTL is safe: a new deploy invalidates the old URL by
producing a new hash, and there is no need to purge intermediate
caches. Always-on under `pagespeed on;`. The TTL cap is controlled by
[`pagespeed_max_age`](#pagespeed_max_age) and the per-type
`pagespeed_*_max_age` directives. (module equivalent: `extend_cache`,
`extend_cache_css`, `extend_cache_images`, `extend_cache_scripts`,
`extend_cache_pdfs`.)

### HTML caching {#html-caching}

Caches HTML responses in the Cyclone shared-memory cache and serves
them zero-copy from the memory-mapped file on cache HIT. This masks
slow origin TTFB on repeat visits. The first request to a URL still
hits origin, and the origin's `Cache-Control` headers decide whether
HTML is eligible. `no-store` is always respected (the response
is not cached at all). For origins that send no `Cache-Control` on
HTML, set [`pagespeed_html_max_age N;`](#pagespeed_html_max_age) to
opt that origin in; for origins that do send `Cache-Control: public,
max-age=N`, HTML caching honours the origin's max-age (capped by
[`pagespeed_max_age`](#pagespeed_max_age)).

The `Cache-Control` header on the optimized HTML response is always
`no-cache` regardless of cache mode, so downstream browsers and CDNs
revalidate on every navigation. Conditional revalidation
([`pagespeed_conditional_revalidation on`](#pagespeed_conditional_revalidation),
the default) keeps that cheap by answering with `304 Not Modified` when
the cached body still matches. HTML caching runs in the optimizer worker; the
module always passes HTML through to the origin and rewrites in flight.

## Browser analysis

Browser analysis uses headless Chrome to generate per-template optimization
profiles. It replaces heuristic-based critical CSS extraction with
browser-validated results from the CSS Coverage API, and produces accurate
above-the-fold detection, LCP identification, and image dimension data.

Browser analysis is disabled by default. Enable it with
[`--enable-browser-analysis`](#--enable-browser-analysis). Chrome (or
`chrome-headless-shell`) must be available at the configured
[`--chrome-binary`](#--chrome-binary) path. The `--chrome-*`, `--browser-*` and
`--no-browser-*` flags above are the config-reference summary; the
[Browser Analysis guide](/docs/browser-analysis/) covers the CDP pipeline,
template profiling, and tuning in depth.

### How it works

When browser analysis is enabled, the worker spawns headless Chrome and
communicates over CDP (Chrome DevTools Protocol). For each unique HTML template
(identified by DOM structure hash), the worker:

1. Inlines cached stylesheets into the HTML (so Chrome can compute real CSS coverage)
2. Runs CSS Coverage API at three viewport sizes (Mobile 375x667, Tablet 768x1024, Desktop 1440x900)
3. Extracts above-the-fold detection, LCP candidates, and rendered image dimensions
4. Stores the result as an optimization profile in the cache

Subsequent HTML processing for pages matching the same template structure uses
the cached profile instead of heuristics. Profiles expire after
[`--browser-profile-ttl`](#--browser-profile-ttl) seconds.

### Chrome memory management

Chrome is recycled after [`--chrome-recycle-interval`](#--chrome-recycle-interval)
pages to prevent memory growth. On Linux, the worker monitors Chrome's RSS via
`/proc/pid/status` and kills the process if it exceeds
[`--chrome-max-memory`](#--chrome-max-memory) MB. On other platforms, RSS
monitoring is not available and only page-count recycling applies.

### Fallback behavior

All browser analysis failures fall back to the heuristic path. If Chrome fails
to start, crashes, or times out, the worker logs the error and continues with
heuristic-based optimization. Browser analysis is strictly additive -- it never
blocks or degrades the base optimization pipeline.

```bash
# Enable browser analysis with a custom Chrome path
factory_worker --cache-path /data/cache.vol \
  --enable-browser-analysis \
  --chrome-binary /usr/bin/chromium \
  --chrome-max-memory 768 \
  --chrome-page-timeout 30000
```

## HTTP management API

The worker embeds an HTTP/1.1 server for programmatic access and the web console.
It is **off** until you enable a transport: [`--api-socket`](#--api-socket) (the
recommended local transport) or [`--api-port`](#--api-port) with
[`--api-bind`](#--api-bind). [`--api-token`](#--api-token),
[`--api-allow-remote`](#--api-allow-remote), [`--api-no-auth`](#--api-no-auth)
and [`--api-read-open`](#--api-read-open) set the authentication posture;
[`--console-dir`](#--console-dir) serves the web console from the same server.

### One invariant, enforced at startup

**Remote is never unauthenticated, and unauthenticated is never remote.** The
worker checks this while parsing its configuration and refuses to start on any
combination that breaks it — with a message naming the flag that would make the
requested posture legal.

| Transport        | Token   | Extra flag           | Result                             |
| ---------------- | ------- | -------------------- | ---------------------------------- |
| unix socket      | either  | —                    | normal — no token is asked for     |
| TCP loopback     | present | —                    | normal                             |
| TCP loopback     | absent  | `--api-no-auth`      | allowed, WARNING banner at startup |
| TCP loopback     | absent  | —                    | **refuses to start**               |
| TCP non-loopback | present | `--api-allow-remote` | allowed, WARNING banner            |
| TCP non-loopback | present | —                    | **refuses to start**               |
| TCP non-loopback | absent  | any                  | **refuses to start**               |

Two more refusals, both at startup: `--api-socket` and `--api-port` together
(the worker serves exactly one transport), and an `--api-bind` that is not an
IPv4 literal — the API binds IPv4 only, so `localhost`, `::1` and other IPv6
forms are rejected by name rather than left to fail the bind later.

With a token configured, every endpoint except `/v1/health` requires an
`Authorization: Bearer <token>` header. `--api-read-open` opens an explicit
allow-list of GET requests and WebSocket streams while keeping mutating
operations behind the token — useful for a public demo dashboard. It is an
explicit choice: an absent token no longer opens reads for you, and nor does
a GET route added in a later release unless it is deliberately added to the
allow-list. Concretely, read-open makes the cached-URL inventory, the origin
hosts behind it, the cooldown table, and the live stats/events streams
readable without the token. The one exception is the optimizer log: `GET
/v1/logs` and the `/v1/ws/logs` stream still require the token — the stream
takes it as its first message, since a WebSocket handshake carries no header
a browser could set. Because read-open still exposes real operational data,
recommend it only behind a reverse proxy that does its own authentication,
not directly on the open internet.

### The unix socket is the recommended local transport

`--api-socket` serves the same HTTP API over
`/run/pagespeed-optimizer/api.sock`, mode 0660 `pagespeed:pagespeed`. Filesystem
permission is the credential for local peers, so nothing has to be provisioned
to your web server and nothing has to be rotated — and the worker binds no TCP
port at all, so "listens on localhost only" is something `ss -ltn` can prove.

**A connection accepted on that socket is an authenticated peer.** Reaching it
means already being in group `pagespeed` — the same boundary that governs the
cache volume — so the worker asks for no bearer token on top, including for
cache purge. A token configured alongside it (the packaged installer always
provisions one, for the TCP case) changes nothing here. That removal is the
entire reason to prefer the socket over `127.0.0.1` plus a token.

```bash
# Local: no port, no credential to distribute.
factory_worker --cache-dir /var/cache/pagespeed-optimizer/v3 \
  --api-socket \
  --console-dir /opt/pagespeed/console

# Remote, deliberately: both flags AND a token, or it will not start.
PAGESPEED_API_TOKEN="$(cat /run/secrets/api-token)" \
factory_worker --cache-dir /var/cache/pagespeed-optimizer/v3 \
  --api-port 9880 --api-bind 0.0.0.0 --api-allow-remote \
  --console-dir /opt/pagespeed/console
```

Prefer `PAGESPEED_API_TOKEN` over `--api-token`: anything on the command line is
readable by any local user through `/proc/<pid>/cmdline`. Package installs
generate a token into `/etc/pagespeed-optimizer/daemon.env` (mode 0640
`root:pagespeed`) for you, along with `PAGESPEED_PURGE_TOKEN` for cache
invalidation over the management socket.

## Headless browser sandbox

Browser analysis runs headless Chrome over untrusted page content, so Chrome's
own sandbox must be on. Chrome's namespace sandbox needs an unprivileged user
namespace and a non-root user; [`--browser-sandbox`](#--browser-sandbox) decides
what happens when it cannot have one.

| Value               | Behaviour                                                                                                                                                                                                                                                                        |
| ------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `require` (default) | The worker probes at startup. If the sandbox is available, Chrome runs sandboxed. If it is not, **browser analysis refuses to start**, logs one message naming the cause and the remedy, and the worker keeps serving without it. It never falls back to an unsandboxed browser. |
| `off`               | Deliberate opt-out. Chrome runs unsandboxed, a warning is logged at startup and on every browser start.                                                                                                                                                                          |

There is no `auto`. `GET /v1/health` reports the resolved state as
`browser_sandbox`: `"on"`, `"unavailable"`, `"off"` or `"disabled"` — so a
monitoring check can see an unsandboxed browser without reading logs. The
setting is also readable from the environment as `PAGESPEED_BROWSER_SANDBOX`.

The container images start as root for one setup step and then drop the
worker to the unprivileged `pagespeed` user (UID 918) before it starts, so the
non-root requirement is met inside them. What remains is the runtime's seccomp
profile: the default Docker and Kubernetes profiles filter out the namespace
flags Chrome's sandbox needs, so the probe reports `unavailable` until the
container gets a profile that allows them (the Helm chart's
`podSecurityContext` comment has the recipe). Set
`PAGESPEED_BROWSER_SANDBOX=off` only to accept an unsandboxed browser
deliberately.

If the sandbox probe is refused by a kernel syscall filter rather than by the
kernel's user-namespace policy, the message says so, so you can tell "this host
denies user namespaces" from "the syscall filter this service runs under is too
tight for Chrome".

### Syscall filter visibility

`GET /v1/health` and `GET /v1/stats` also report `syscall_filter`, read from
the kernel's view of the running process:

| Value        | Meaning                                                         |
| ------------ | --------------------------------------------------------------- |
| `"filtered"` | A kernel syscall filter is attached to the worker process.      |
| `"none"`     | No filter is attached.                                          |
| `"unknown"`  | The kernel does not report a filter state this worker can read. |

**`"filtered"` means _a_ kernel syscall filter is attached — not that any
particular hardening profile is in force.** A container runtime's default
seccomp profile counts, so a container started with stock settings reports
`"filtered"` without you configuring anything; a package install on a host
reports `"none"` today. Read the field as "is this process confined at all",
not as "is my intended profile applied".

This is read-only reporting, not a setting: the worker does not install the
filter. On a host the service manager applies whatever the unit specifies,
before the worker starts; in a container the container runtime's profile
applies; started by hand, the worker is unfiltered and says `"none"`. There is
no flag, because a flag naming a control the process does not own would be a
flag that lies.

## Threading and memory {#threading}

[`--num-threads`](#--num-threads) sizes the notification thread pool (the
default follows the CPU limit of the container or cgroup) and
[`--ram-cache-size`](#--ram-cache-size) the RAM cache tier (the default is 6%
of `--cache-size`, between 16 and 256 MB). The RAM cache uses write-around
semantics: writes go directly to disk, reads populate the RAM cache on miss.
This prevents cross-process staling between nginx and the worker.

When `--num-threads` is not set, the pool size is the number of CPUs the worker
may use, between 2 and 128. That is the smallest of the CPUs the process is
allowed to run on and the CPU limit of its container (for example
`docker run --cpus=12`), so a container limited to 12 CPUs on a larger host
starts 12 threads. The worker logs the chosen size and where it came from at
startup. Setting `--num-threads` explicitly overrides this.

## Pre-compressed text variants

The worker produces gzip and brotli pre-compressed alternates for all text
resources (HTML, CSS, JS) after optimization. This eliminates dynamic
compression CPU cost on cache HITs -- nginx serves the pre-compressed
alternate directly with the correct `Content-Encoding` header.
[`--gzip-level`](#--gzip-level) and [`--brotli-level`](#--brotli-level) set the
compression levels; `0` disables an encoding.

Bits 6-7 of the capability mask encode the transfer encoding:

| Value | Encoding | Description                |
| ----- | -------- | -------------------------- |
| `00`  | Identity | Uncompressed (fallback)    |
| `01`  | Gzip     | Pre-compressed with gzip   |
| `10`  | Brotli   | Pre-compressed with brotli |
| `11`  | Reserved | Reserved for future use    |

The default capability mask is `0x08` (Desktop, Identity encoding).

Images are always stored with identity encoding (compressed image formats
do not benefit from additional gzip/brotli compression).

```bash
factory_worker --cache-path /data/cache.vol \
  --gzip-level 6 --brotli-level 6
```

## SVG auto-vectorization

Format slot `11` (in the capability mask) is used for SVG auto-vectorization.

When the worker processes an image notification, it evaluates the raster source
for SVG candidacy before running the standard raster transcode loop. Suitable
images (simple graphics, logos, icons) are vectorized using VTracer, a Rust-based
raster-to-SVG engine integrated via FFI. A size gate ensures the resulting SVG
is smaller than the raster original; if it is not, the SVG variant is discarded.

SVG variants are resolution-independent -- a single variant serves all viewport
classes and pixel densities. The worker stores the SVG at a fixed capability mask
(`kSvg`, Desktop, 1x, Save-Data off, Identity encoding) and the cache selector
gives SVG a universal format bonus during alternate selection.

If a browser sends `image/jxl` in the `Accept` header, it maps to Original
format (no special handling). The JXL format slot in the capability mask is
fully repurposed for SVG and is never set from client headers.

### SVG operational modes

The [`--svg-mode`](#--svg-mode) flag controls how far the SVG pipeline runs:

| Mode      | Behavior                                                           |
| --------- | ------------------------------------------------------------------ |
| `detect`  | Evaluate candidacy and log scores. No vectorization. **(default)** |
| `preview` | Evaluate, vectorize, and store SVG. Not served to clients.         |
| `auto`    | Full pipeline: evaluate, vectorize, store, and serve.              |

Start with `detect` to see which images qualify, then move to `preview` for
inspection, and finally `auto` for production serving. The candidacy, size and
output gates are the `--svg-*` flags in the
[SVG auto-vectorization](#flags-svg-auto-vectorization) group of the reference;
all SVG settings are hot-reloadable via `PATCH /v1/config` on the HTTP API.

```bash
# Enable full SVG pipeline with stricter candidacy
factory_worker --cache-path /data/cache.vol \
  --svg-mode auto --svg-candidacy-threshold 65 --svg-max-paths 300
```

## Management socket

The worker exposes a management socket at `{socket_path}.mgmt` (e.g.,
`/var/lib/pagespeed/pagespeed.sock.mgmt`). This socket accepts newline-terminated
text commands and returns a response before closing the connection; the one
exception is a successful `AUTH`, which keeps the connection open for the
`PURGE` that follows.

### Connecting to the management socket

```bash
# Using socat
echo "STATS" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt

# Using Python
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX)
s.connect('/var/lib/pagespeed/pagespeed.sock.mgmt')
s.sendall(b'STATS\n')
print(s.recv(4096).decode())
s.close()
"
```

### `STATS` command

Returns a JSON object with worker statistics:

```json
{
  "status": "ok",
  "connections": { "active": 2, "max": 128 },
  "notifications": { "received": 1542, "skipped_dedup": 380 },
  "variants": { "written": 986, "proactive": 724 },
  "errors": 3,
  "cache": { "entries": 2048, "size": 134217728 },
  "by_type": {
    "html": { "n": 45, "us": 120000 },
    "css": { "n": 112, "us": 85000 },
    "js": { "n": 98, "us": 72000 },
    "images": { "n": 731, "us": 9500000 }
  },
  "by_format": { "webp": 320, "avif": 280, "jpeg": 85, "png": 46 },
  "timing_us": { "total": 9777000 }
}
```

The `us` fields are cumulative processing time in microseconds. Divide by
the count (`n`) to get the average processing time per item.

### `PURGE` command

Invalidates all cached variants for a URL. `PURGE` is refused unless the
connection was authenticated first: send `AUTH <token>` and then `PURGE`
followed by the hostname and URL, on the same connection. The token is the
worker's `PAGESPEED_PURGE_TOKEN` (the packages generate it into
`/etc/pagespeed-optimizer/daemon.env`; the container images generate one per
start and print it once in the container log unless you set it). It is not in
your shell by itself; [Purge and re-test a specific
URL](/docs/troubleshooting/#purge-and-re-test-a-specific-url) shows how to
read it per deployment before running:

```bash
printf 'AUTH %s\nPURGE example.com /images/photo.jpg\n' "$PAGESPEED_PURGE_TOKEN" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt
```

**Response:** `OK\n` to the `AUTH`, then `OK N entries deleted\n` where N is
the number of cache entries removed (`OK 0 entries deleted (URL not found — may
have been evicted or never cached)\n` when nothing matched). A `PURGE` without `AUTH` answers
`ERR authentication required\n` (`ERR no purge token configured\n` when the
worker has no token), and a wrong token answers `ERR invalid token\n`. The
PURGE command iterates all possible capability mask combinations
for the URL and deletes every matching entry, including sentinel entries used
for Early Hints and warmup.

After a PURGE, the next request for that URL will be a cache miss. Nginx will
proxy to the origin, re-cache the response, and send a new notification to the
worker.

### Cache invalidation

The PURGE command is the primary mechanism for cache invalidation. Common
use cases:

- **Content update:** When you deploy new CSS, JS, or images, purge the
  affected URLs so the worker re-optimizes from the updated originals.
- **Debugging:** Purge a URL to force a fresh optimization pass and observe
  the worker's processing logs.
- **Selective cache clearing:** Unlike restarting the worker or deleting the
  cache file (which loses everything), PURGE targets individual URLs.

The HTTP API's [`POST /v1/cache/purge`](/docs/http-api/#post-v1cachepurge)
does the same without the management socket; its credential is group
membership over the API's unix socket, or the bearer token over TCP where one
is configured.

For bulk invalidation, send multiple PURGE commands. Each iteration opens a
new connection to the management socket, and authentication is per
connection, so each one sends `AUTH` again:

```bash
for url in /style.css /app.js /hero.jpg; do
  printf 'AUTH %s\nPURGE example.com %s\n' "$PAGESPEED_PURGE_TOKEN" "$url" | socat - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt
done
```

## Capability mask

The module classifies each request into a 32-bit capability mask based on
the client's capabilities. This mask is used as part of the cache key, allowing
different optimized variants to be served to different clients.

### Bitmask layout

| Bits | Field          | Values                                               |
| ---- | -------------- | ---------------------------------------------------- |
| 0-1  | Image Format   | `00` Original, `01` WebP, `10` AVIF, `11` SVG        |
| 2-3  | Viewport Class | `00` Mobile, `01` Tablet, `10` Desktop               |
| 4    | Pixel Density  | `0` 1x, `1` 2x+ (Retina)                             |
| 5    | Save-Data      | `0` off, `1` on                                      |
| 6-7  | Transfer Enc.  | `00` Identity, `01` Gzip, `10` Brotli, `11` Reserved |

### How classification works

The nginx module determines these values from request headers:

- **Image Format:** Parsed from the `Accept` header. `image/avif` selects
  AVIF; otherwise `image/webp`, `image/*`, `*/*` or no `Accept` header at all
  selects WebP; any other value selects the original format. See
  [Image format and the Accept header](/docs/cdn-integration/#image-format-negotiation).
- **Viewport Class:** Derived from the `Sec-CH-Viewport-Width` client hint or
  `User-Agent` heuristics (mobile vs. desktop).
- **Pixel Density:** From `Sec-CH-DPR` client hint or device heuristics.
- **Save-Data:** From the `Save-Data: on` request header.
- **Transfer Encoding:** From the `Accept-Encoding` header (br > gzip > identity).

### Cache key format

Variants are stored as alternates within a `SHA-256(URL, hostname)` cache key.
The capability mask determines which alternate a client receives. For example,
a WebP-capable desktop client with brotli requesting `/style.css` gets a different
variant than a mobile client with gzip requesting the same URL.

### Variant fallback

When an exact match isn't found in the cache, nginx tries progressively
degraded fallback masks before falling back to mask `0x08` (the default for
original content stored at Desktop/Identity). This means a client always
gets a response — either the optimized variant, a close variant, or the
original.

## Content types

The worker processes these content types when notified by nginx:

| Type           | Optimizations Applied                                                                             |
| -------------- | ------------------------------------------------------------------------------------------------- |
| **Images**     | Format transcoding (JPEG/PNG/GIF to WebP/AVIF), lossless PNG reduction, quality-aware compression |
| **CSS**        | Minification (whitespace, comments, redundant syntax removal)                                     |
| **JavaScript** | Minification (whitespace, comments)                                                               |
| **HTML**       | Critical CSS extraction and inline injection                                                      |

The worker only writes an optimized variant if it is smaller than the original.
If minification doesn't reduce the size, the original is kept and no variant
is written.

## Cross-process cache sharing

Both nginx and the worker access the same Cyclone cache file. For this
to work correctly:

1. **Same file path** — `pagespeed_cache_path` and `--cache-path` must point to
   the same file.
2. **Permissions** — The worker owns the cache directory and sets every
   shared file explicitly to 0660 owner+group; nginx workers must be in group
   `pagespeed` (the module package's postinst adds them). Nothing is
   world-accessible anymore, and no correct setup depends on umask.
3. **Memory-mapped directory** — Both processes open the cache with mmap
   directory sharing enabled (this is automatic). Without it, each process would
   have its own in-memory directory and writes would be invisible to the other.

## Example: complete configuration

### nginx.conf

```nginx
worker_processes auto;
error_log /var/log/nginx/error.log warn;
pid /var/run/nginx.pid;

# The thin nginx module, as the pagespeed-nginx image bundles it. The native
# module from the nginx-module-pagespeed package has the same file name but is
# a different module, with different directives.
load_module /usr/lib/nginx/modules/ngx_pagespeed_module.so;

events {
    worker_connections 1024;
}

http {
    include       /etc/nginx/mime.types;
    default_type  application/octet-stream;

    server {
        listen 80;
        server_name <your-domain>;

        pagespeed on;
        pagespeed_cache_path /var/lib/pagespeed/cache.vol;
        # Worker socket and HTML toggle are read
        # automatically from pagespeed-shared.conf

        location / {
            proxy_pass http://127.0.0.1:8081;
            proxy_set_header Host $host;
            proxy_set_header X-Real-IP $remote_addr;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        }
    }
}
```

### Worker systemd unit

```ini
[Unit]
Description=PageSpeed Optimizer Worker
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/factory_worker \
  --socket /run/pagespeed-optimizer/notify.sock \
  --cache-dir /var/cache/pagespeed-optimizer/v3 \
  --cache-size 536870912 \
  --log-format json \
  --log-level info
User=pagespeed
Group=pagespeed
UMask=0007
RuntimeDirectory=pagespeed-optimizer
RuntimeDirectoryMode=0750
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

(The packaged unit — `deploy/pagespeed-optimizer.service` — adds the full
hardening set: empty `CapabilityBoundingSet`, `ProtectSystem=strict`,
`ProtectHome=yes`, `PrivateTmp=yes`, `NoNewPrivileges=yes`, and a secrets
`EnvironmentFile`. Use it as the reference.)

## Response headers

mod_pagespeed adds the following response header:

| Header        | Value  | Meaning                                                  |
| ------------- | ------ | -------------------------------------------------------- |
| `X-PageSpeed` | `HIT`  | Content served from cache (may be original or optimized) |
| `X-PageSpeed` | `MISS` | Cache miss — proxied to origin, response cached          |

A `HIT` response may contain original content if the worker
hasn't processed it yet. The worker runs asynchronously, so there's a brief
window after the first request where the cache contains the original. Subsequent
requests will get the optimized version once the worker has written it.

## Sizing the cache

The cache size depends on your site's content:

| Site Type                  | Recommended Cache Size |
| -------------------------- | ---------------------- |
| Small blog / portfolio     | 256 MB - 1 GB          |
| Medium site (1,000 pages)  | 1 - 2 GB               |
| Large site (10,000+ pages) | 2 - 4 GB               |
| Image-heavy site           | 4 - 8 GB               |

The `--cache-size` default is 1 GB, which suits most small and medium sites.

The cache stores both original and optimized variants. Image-heavy sites need
more space because each image may have multiple variants (WebP, AVIF, different
viewport sizes, Save-Data quality levels, and pixel density variants).

With all proactive variant generation enabled, a single image can produce up to
37 distinct optimized variants (36 raster plus 1 SVG for eligible images). The
HTTP API's PURGE command iterates a larger capability-mask space (up to 192
combinations, since several masks — including the transfer-encoding axis — map
to the same stored entry); the 37 figure counts the distinct cached outputs.
Size the cache generously for image-heavy sites.

Monitor cache effectiveness by checking the ratio of `HIT` to `MISS` responses
in your nginx access logs, or use the management socket `STATS` command to
inspect cache entry counts and size.

## Disabling PageSpeed per-location

You can enable PageSpeed at the server level and disable it for specific paths:

```nginx
server {
    pagespeed on;
    pagespeed_cache_path /var/lib/pagespeed/cache.vol;

    location / {
        proxy_pass http://127.0.0.1:8081;
    }

    # Don't optimize API responses
    location /api/ {
        pagespeed off;
        proxy_pass http://127.0.0.1:8081;
    }

    # Don't optimize admin panel
    location /admin/ {
        pagespeed off;
        proxy_pass http://127.0.0.1:8081;
    }
}
```

## URL pattern exclusions

The `pagespeed_disallow` directive excludes URL patterns from optimization.
Unlike `pagespeed off` (which disables the entire module for a location block),
`pagespeed_disallow` selectively skips individual URL patterns while keeping
the module active.

```nginx
server {
    pagespeed on;
    pagespeed_cache_path /var/lib/pagespeed/cache.vol;

    # Skip font files
    pagespeed_disallow *.woff2;
    pagespeed_disallow *.woff;

    # Skip API endpoints
    pagespeed_disallow /api/;

    # Skip admin panel
    pagespeed_disallow /admin/;

    location / {
        proxy_pass http://127.0.0.1:8081;
    }
}
```

**Pattern matching:**

- Patterns starting with `/` match URL prefixes: `/api/` matches `/api/v1/users`
- Patterns starting with `*` match URL suffixes: `*.woff2` matches `/fonts/main.woff2`
- Other patterns match as substrings: `admin` matches `/site/admin/panel`

Multiple `pagespeed_disallow` directives can be specified. They are checked in
order; the first match causes the request to bypass PageSpeed.

## Notification deduplication

The worker automatically skips processing when the target cache variant
already exists. This prevents redundant work when multiple requests arrive for
the same URL before the worker has finished optimizing.

For example, if 10 requests for `/photo.jpg` with WebP capability arrive in
quick succession, only the first notification triggers image transcoding. The
remaining 9 are skipped because the WebP variant is already in the cache.

## Health check

The worker exposes a health check socket at `{socket_path}.health`.
Connect to it to get the current status:

```bash
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX)
s.connect('/var/lib/pagespeed/pagespeed.sock.health')
print(s.recv(256).decode())
s.close()
"
```

**Response format:**
`OK {active}/{max} notifs=N variants=N proactive=N errors=N cache_entries=N\n`

Example: `OK 5/128 notifs=1542 variants=986 proactive=724 errors=3 cache_entries=2048`

## See also

- [Configuration reference: Apache and nginx directives](/docs/configuration/) — the native module
- [Install with Docker](/docs/installation-docker/) and [Kubernetes with Helm](/docs/helm-deployment/)
- [Cache modes](/docs/cache-modes/) and the [cache-control guide](/docs/cache-control/)
- [HTTP API](/docs/http-api/) and the [admin console](/docs/admin-console/)
