# Notes for the thin nginx module directives

Operator prose appended to the generated blocks on `/docs/worker-configuration/`.
One `## <directive>` section per directive; the name must exist in
`thin-module-directives.json` or rendering fails. Everything factual that the
source states (default, context) is generated and must not be repeated here.

## pagespeed

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

## pagespeed_cache_path

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

## pagespeed_disallow

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

## pagespeed_hot_threshold

Number of fallback hits before a URL counts as "hot" and triggers a warmup
notification to the worker. Only relevant when the worker runs with
[`--enable-warmup`](#--enable-warmup).

## pagespeed_max_age

Cap in seconds on the `max-age` taken from the origin's `Cache-Control`. See the
[cache-control guide](/docs/cache-control/).

## pagespeed_immutable_max_age

Cap in seconds for responses the origin marks `Cache-Control: immutable`.

## pagespeed_synthesize_swr

Synthesizes `stale-while-revalidate` on cache-served (`HIT`) responses. Safe
mode suppresses the synthesis regardless of this flag; see
[`pagespeed_cache_mode`](#pagespeed_cache_mode).

## pagespeed_conditional_revalidation

Answers `If-None-Match` and `If-Modified-Since` requests with `304 Not Modified`
when the cached body still matches.

## pagespeed_stale_if_error_max_age

Window in seconds during which a stale cached response may be served when the
origin fails; `0` disables it. The default matches the `stale-if-error=86400`
that aggressive mode advertises to downstream caches.

## pagespeed_html_max_age

Default `max-age` in seconds for HTML the origin serves without a
`Cache-Control` header. `0` keeps such HTML uncached; set it to opt an origin
into [HTML caching](#html-caching).

## pagespeed_css_max_age

Default `max-age` in seconds for CSS and JavaScript the origin serves without a
`Cache-Control` header. The default follows the cache mode.

## pagespeed_image_max_age

Default `max-age` in seconds for images the origin serves without a
`Cache-Control` header. The default follows the cache mode.

## pagespeed_force_refresh_html

Revalidates HTML at the origin when the browser force-refreshes (Ctrl+F5 /
`Cache-Control: no-cache` on the request).

## pagespeed_force_refresh

Revalidates every non-HTML type at the origin on a browser force-refresh.

## pagespeed_trust_x_forwarded_proto

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

## pagespeed_cache_mode

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

## pagespeed_agent_content_signal

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
