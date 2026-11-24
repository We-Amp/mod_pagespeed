# Operations & Monitoring

The operations and monitoring guide now lives in the canonical product
documentation on modpagespeed.com:

- [Admin console](https://modpagespeed.com/docs/admin-console/) — statistics, cache inspection, message history, and configuration
- [Security](https://modpagespeed.com/docs/security/) — restricting admin endpoint access
- [Caching](https://modpagespeed.com/docs/cache-modes/) — cache configuration and purging

## Savings statistics

The statistics page (`/pagespeed_admin/statistics`, and the Statistics and
Savings pages of the admin console) reports byte savings for images, CSS and
JavaScript in two kinds of numbers. They answer different questions and are
never added together.

### Saved at rewrite time

| Variable | Counts |
|---|---|
| `image_rewrite_total_bytes_saved`, `image_rewrite_total_original_bytes` | images |
| `css_filter_total_bytes_saved`, `css_filter_total_original_bytes` | stylesheets |
| `javascript_total_bytes_saved`, `javascript_total_original_bytes` | scripts |

These grow once per resource, at the moment it is optimized. Serving the
optimized resource afterwards, a thousand times or from the cache after a
restart, adds nothing. On a site with a warm cache they therefore stand still
for hours, and after a restart with the cache kept they stay at zero until
something is optimized anew. That is expected: they measure optimization work,
not what visitors received.

### Delivered

These grow each time an optimized response is sent, from the cache as well.
Each type has a pair: the size of the originals behind the responses sent, and
the size sent. The saving is the first minus the second; the share saved is
that difference divided by the first.

Responses the server optimized itself:

| Variable | Counts |
|---|---|
| `image_served_original_bytes`, `image_served_optimized_bytes` | optimized images |
| `css_served_original_bytes`, `css_served_optimized_bytes` | optimized stylesheets |
| `javascript_served_original_bytes`, `javascript_served_optimized_bytes` | minified scripts |

Both sizes are before compression (`gzip`, `br`), whatever went over the wire.
This covers rewritten URLs (`.pagespeed.`) and resources optimized in place
under their own URL.

Responses answered from the optimizer cache:

| Variable | Counts |
|---|---|
| `ipro_daemon_served_image_original_bytes`, `ipro_daemon_served_image_optimized_bytes` | optimized images |
| `ipro_daemon_served_css_original_bytes`, `ipro_daemon_served_css_optimized_bytes` | optimized stylesheets |
| `ipro_daemon_served_js_original_bytes`, `ipro_daemon_served_js_optimized_bytes` | optimized scripts |

A request is answered either by the server's own rewriting or from the
optimizer cache, never both, so no response is in both families. They are kept
apart because they do not always measure the same thing:

- A response sent uncompressed is measured against the uncompressed original,
  like the first family.
- With `DaemonServeStoredEncodings` on, a response can be sent as a stored
  compressed copy. It is then measured against the original in that same
  compression, so both sizes are sizes after compression, and the original's
  compressed size is an estimate. Compression alone never shows up as a saving
  of the optimization.
- A stored compressed copy whose cache entry does not record the original's
  size in that compression is not counted.

Envoy does not answer from the optimizer cache, so the second family stays at
zero there.

### What is counted and what is not

Counted: a `GET` answered with status 200 and the whole optimized body.

Not counted, in either family:

- `HEAD` and other methods; conditional requests answered with 304; any
  response that is not a 200;
- requests with a `Range` header;
- the module's own fetches, when it loads a resource from its own server to
  optimize it;
- responses that fall back to the unoptimized resource, because the optimized
  version was not ready in time or the resource could not be made smaller;
- combined stylesheets and scripts made from files that were not optimized
  first: combining saves requests, not bytes. A combination does count when it
  is smaller than the files it started from, which is the case when its parts
  were minified first, and for combined scripts whenever JavaScript
  minification is on, because the combiner then minifies the scripts itself;
- inlined resources and resources whose URL was only rewritten for longer
  caching: these save requests or revalidations, not bytes. The existing
  counters `image_inline`, `num_css_inlined`, `num_js_inlined`,
  `css_file_count_reduction` and `js_file_count_reduction` show them.

An optimized response that is larger than its original is counted as it is. It
lowers the saving, and a type can show a negative saving.

The numbers are what this server sent. A response answered by a browser cache,
a CDN or another cache in front of the server never reaches these counters, so
they are a lower bound on what visitors saved.

### Restarts

Every statistics variable starts again from zero when the web server restarts
or reloads. Totals are not kept across a restart. The delivered counters leave
zero with the first optimized responses sent, also when everything comes from
the cache; the rewrite-time counters only when something is optimized anew.

To keep a history across restarts, turn on statistics logging
(`StatisticsLogging on`). The log is off by default, takes a sample every few
minutes, and starts over when its file reaches its size limit
(`StatisticsLoggingMaxFileSizeKb`), so it holds a limited history.

### In the admin console

The Savings page opens with **Delivered savings**: per type the original size,
the size sent, the saving and how much smaller that was, with the two families
side by side as "Optimized by this server" and "Answered from the optimizer
cache". The console never adds the two.

- Right after a restart the card says that nothing was sent yet. It does not
  show 0%. For the first 15 minutes it shows sizes without percentages, marked
  "warming up", and every figure states since when it counts.
- A type that was sent larger than its originals shows a negative saving and
  reads "larger".
- With statistics logging on, the card also shows the same figures over the
  history the log holds, restarts included, and states the span that history
  covers.
- The rewrite-time numbers are in their own card, **Saved at rewrite time**.
- **Optimizer cache serves** shows the optimizer's own statistics for the
  responses answered from its cache. They are measured in bytes sent and
  include compression, so they differ from the `ipro_daemon_served_*_bytes`
  variables above and restart with the optimizer, not with the web server.

### After upgrading

The version that introduces the delivered counters starts with an empty
optimization cache: entries written by earlier versions are not used. Every
resource is fetched and optimized again once, the first time it is needed, so
expect more origin fetches and more optimization work for a while after the
upgrade, and unoptimized responses until then. Nothing needs to be done. The
old entries age out within the configured cache size; to get the space back at
once, clear the cache directory while the server is stopped. Servers of the old
and the new version that share a cache do not read each other's entries.
