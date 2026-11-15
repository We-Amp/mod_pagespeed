---
title: 'API reference: IPC, sockets, and C API'
description: 'mod_pagespeed internal interfaces: the module-to-worker IPC wire format, the health and management sockets, the capability mask and the ps_ C API.'
order: 65
group: 'Reference'
lastUpdated: 2026-10-22
---

This page documents the internal protocols and interfaces of mod_pagespeed:
the IPC wire format between nginx and the worker, the health check endpoint,
the management socket protocol (STATS, PURGE, METRICS, BROWSER-STATUS), the
capability mask encoding, and the `ps_` C API for embedding the cache and HTML
processor into other applications.

## IPC wire format

The nginx module sends fire-and-forget notifications to the worker over
a Unix socket. There is no response — the worker reads the notification,
processes the content from the shared cache, and writes optimized variants back.

### Message structure

Each message consists of a 4-byte length header followed by the payload:

```text
[4 bytes: payload_length (big-endian uint32)]
[4 bytes: url_length (big-endian uint32)]
[url_length bytes: URL string]
[4 bytes: hostname_length (big-endian uint32)]
[hostname_length bytes: hostname string]
[1 byte: content_type]
[4 bytes: capability_mask (big-endian uint32)]
```

The `payload_length` field does not include itself — it is the total size of
everything after the first 4 bytes. The hostname is required for cache key
computation (`SHA-256(URL, hostname)`).

### Content type values

| Value | Type       |
| ----- | ---------- |
| `0`   | HTML       |
| `1`   | CSS        |
| `2`   | JavaScript |
| `3`   | Image      |
| `4`   | Other      |

### Example

For URL `/style.css` (10 bytes), hostname `localhost` (9 bytes), content type
CSS (`0x01`), capability mask `0x00000008` (Desktop/Identity):

```text
00 00 00 20          # payload_length = 32 bytes
00 00 00 0A          # url_length = 10
2F 73 74 79 6C 65 2E 63 73 73  # "/style.css"
00 00 00 09          # hostname_length = 9
6C 6F 63 61 6C 68 6F 73 74     # "localhost"
01                   # content_type = CSS
00 00 00 08          # capability_mask = 0x08
```

### Connection lifecycle

1. Nginx opens a new Unix socket connection for each notification.
2. Nginx writes the serialized message.
3. Nginx closes the connection (fire-and-forget).
4. On connection failure (ECONNREFUSED, ENOENT), nginx retries with
   exponential backoff: 2 retries, 10ms initial delay, 3x multiplier
   (10ms, 30ms).
5. If all retries fail, the notification is dropped. The cache serves the
   original content. Subsequent requests for the same URL re-notify.

## Health endpoint

The worker listens on a Unix socket at `{socket_path}.health`. On connection,
it immediately writes a status line and closes.

### Socket path

If the worker socket is `/var/lib/pagespeed/pagespeed.sock`, the health socket
is `/var/lib/pagespeed/pagespeed.sock.health`.

### Response format

```text
OK {active_connections}/{max_connections} notifs={N} variants={N} proactive={N} errors={N} cache_entries={N}\n
```

### Fields

| Field                | Description                                          |
| -------------------- | ---------------------------------------------------- |
| `active_connections` | Number of currently active client connections        |
| `max_connections`    | Configured maximum connections (`--max-connections`) |
| `notifs`             | Total notifications received from nginx              |
| `variants`           | Total optimized variants written to cache            |
| `proactive`          | Proactively generated sibling variants               |
| `errors`             | Total processing errors                              |
| `cache_entries`      | Current number of entries in the Cyclone cache       |

### Example

```text
OK 3/128 notifs=4821 variants=2106 proactive=1580 errors=7 cache_entries=3412
```

### Usage

```bash
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX)
s.connect('/var/lib/pagespeed/pagespeed.sock.health')
print(s.recv(256).decode())
s.close()
"
```

The health endpoint is designed for automated health checks (Docker
`HEALTHCHECK`, Kubernetes liveness probes, load balancer checks). It accepts
a connection, writes the response, and closes. No request data is needed.

## Management socket protocol

The worker listens on a Unix socket at `{socket_path}.mgmt` for
newline-terminated text commands. After processing a command, the worker writes
a response and closes the connection; the one exception is a successful `AUTH`,
which keeps the connection open for the `PURGE` that follows.

### Socket path

If the worker socket is `/var/lib/pagespeed/pagespeed.sock`, the management
socket is `/var/lib/pagespeed/pagespeed.sock.mgmt`.

### Commands

#### `STATS`

Returns a JSON object with worker statistics: connections, notifications, variants, errors, cache, and per-type and per-format breakdowns.

**Request:** `STATS\n`

**Response:**

```json
{
  "status": "ok",
  "connections": {
    "active": 2,
    "max": 128
  },
  "notifications": {
    "received": 4821,
    "skipped_dedup": 1200
  },
  "variants": {
    "written": 2106,
    "proactive": 1580
  },
  "errors": 7,
  "cache": {
    "entries": 3412,
    "size": 268435456
  },
  "by_type": {
    "html": { "n": 120, "us": 450000 },
    "css": { "n": 340, "us": 180000 },
    "js": { "n": 280, "us": 150000 },
    "images": { "n": 1366, "us": 28500000 }
  },
  "by_format": {
    "webp": 580,
    "avif": 490,
    "jpeg": 180,
    "png": 116
  },
  "timing_us": {
    "total": 29280000
  }
}
```

**Field descriptions:**

| Field                         | Description                                     |
| ----------------------------- | ----------------------------------------------- |
| `connections.active`          | Currently open client connections               |
| `connections.max`             | Configured `--max-connections`                  |
| `notifications.received`      | Total notifications from nginx                  |
| `notifications.skipped_dedup` | Skipped because variant already exists          |
| `variants.written`            | Total optimized variants written to cache       |
| `variants.proactive`          | Subset of `written` that were sibling variants  |
| `errors`                      | Total processing failures                       |
| `cache.entries`               | Current number of cache entries                 |
| `cache.size`                  | Current cache size in bytes                     |
| `by_type.{type}.n`            | Number of items processed for this content type |
| `by_type.{type}.us`           | Cumulative processing time in microseconds      |
| `by_format.{format}`          | Number of variants generated per image format   |
| `timing_us.total`             | Cumulative processing time across all types     |

#### `PURGE <hostname> <url>`

Deletes all cached variants for the specified URL and hostname. The command is
refused unless the connection was authenticated with `AUTH` (below) first;
send both commands on the same connection.

**Request:** `AUTH <your-token>\n` followed by `PURGE example.com /images/photo.jpg\n`

**Response:** `OK\n` to the `AUTH`, then `OK N entries deleted\n` to the
`PURGE`; when nothing matched, `OK 0 entries deleted (URL not found — may have
been evicted or never cached)\n`. A `PURGE` without a preceding `AUTH` answers
`ERR authentication required\n`, or `ERR no purge token configured\n` when the
worker has no purge token.

The PURGE command iterates all possible capability mask combinations (image
formats, viewports, pixel densities, Save-Data states, transfer encodings) and
deletes every cache entry for the URL. It also deletes sentinel entries used
for Early Hints (`0x1C`) and warmup (`0x2C`).

The maximum number of entries deleted per URL is 192 (4 formats x 3 viewports
x 2 densities x 2 Save-Data x 4 encodings) plus 2 sentinel entries = 194.

The HTTP API offers the same operation as
[`POST /v1/cache/purge`](/docs/http-api/#post-v1cachepurge); there the
credential is group membership over the API's unix socket, or the bearer token
over TCP where one is configured.

#### `AUTH <token>`

Authenticates the connection for `PURGE` commands. Every `PURGE` requires it:
the worker refuses a `PURGE` on a connection that has not sent a valid `AUTH`,
and there is no unauthenticated mode. The token is the worker's
`PAGESPEED_PURGE_TOKEN` environment variable. The packages generate one into
`/etc/pagespeed-optimizer/daemon.env`; the container images generate one per
container start and print it once in the container log when you did not set
it.

**Request:** `AUTH <your-token>\n`

**Response:** `OK\n` on success, `ERR invalid token\n` on a wrong token, or
`ERR no token configured\n` when the worker has no purge token.

A successful `AUTH` keeps the connection open; every other command closes it
after its response. Authentication is per connection, so send `AUTH` as the
first command on each connection and the `PURGE` on that same connection: a
`PURGE` on a new connection is refused even right after a successful `AUTH` on
another, which is why a one-shot `echo "PURGE …" | socat` cannot purge. The
token is compared using constant-time comparison to prevent timing attacks.
STATS, METRICS and BROWSER-STATUS work without authentication.

When `PAGESPEED_PURGE_TOKEN` is not set, `PURGE` is unavailable: the worker
answers `ERR no purge token configured\n`, and `AUTH` answers
`ERR no token configured\n`.

#### `METRICS`

Returns worker statistics in Prometheus text exposition format. This enables
direct Prometheus scraping via a simple exporter script.

**Request:** `METRICS\n`

**Response:**

```text
# HELP pagespeed_notifications_total Total notifications received.
# TYPE pagespeed_notifications_total counter
pagespeed_notifications_total 4821
# HELP pagespeed_notifications_skipped_total Notifications skipped (dedup).
# TYPE pagespeed_notifications_skipped_total counter
pagespeed_notifications_skipped_total 1200
# HELP pagespeed_variants_written_total Total variants written to cache.
# TYPE pagespeed_variants_written_total counter
pagespeed_variants_written_total 2106
# HELP pagespeed_proactive_variants_total Proactively generated variants.
# TYPE pagespeed_proactive_variants_total counter
pagespeed_proactive_variants_total 1580
# HELP pagespeed_errors_total Total processing errors.
# TYPE pagespeed_errors_total counter
pagespeed_errors_total 7
# HELP pagespeed_processing_seconds_total Cumulative processing time in seconds.
# TYPE pagespeed_processing_seconds_total counter
pagespeed_processing_seconds_total{type="html"} 0.450000
pagespeed_processing_seconds_total{type="css"} 0.180000
pagespeed_processing_seconds_total{type="js"} 0.150000
pagespeed_processing_seconds_total{type="image"} 28.500000
# HELP pagespeed_format_generated_total Variants generated by image format.
# TYPE pagespeed_format_generated_total counter
pagespeed_format_generated_total{format="webp"} 580
pagespeed_format_generated_total{format="avif"} 490
pagespeed_format_generated_total{format="jpeg"} 180
pagespeed_format_generated_total{format="png"} 116
# HELP pagespeed_cache_entries Current number of cache entries.
# TYPE pagespeed_cache_entries gauge
pagespeed_cache_entries 3412
# HELP pagespeed_cache_bytes Current cache size in bytes.
# TYPE pagespeed_cache_bytes gauge
pagespeed_cache_bytes 268435456
# HELP pagespeed_connections_active Currently active connections.
# TYPE pagespeed_connections_active gauge
pagespeed_connections_active 3
# HELP pagespeed_connections_max Maximum allowed connections.
# TYPE pagespeed_connections_max gauge
pagespeed_connections_max 128
```

**Prometheus scraping example:**

```bash
# One-liner to scrape metrics via socat
echo "METRICS" | socat -t 5 - UNIX-CONNECT:/var/lib/pagespeed/pagespeed.sock.mgmt
```

For a Prometheus exporter, use a simple script that connects to the management
socket and exposes the output on an HTTP endpoint.

#### `BROWSER-STATUS`

Returns a JSON object with browser analysis status. If browser analysis is not
enabled, returns `{"enabled":false}`.

**Request:** `BROWSER-STATUS\n`

**Response:**

```json
{
  "enabled": true,
  "chrome_running": true,
  "chrome_pid": 12345,
  "chrome_rss_mb": 180,
  "queue_depth": 2,
  "analysis_in_progress": true,
  "templates_tracked": 14,
  "profiles_generated": 87,
  "profiles_used": 62,
  "analysis_errors": 3,
  "chrome_restarts": 1,
  "chrome_crashes": 0,
  "queue_enqueued": 120,
  "queue_dropped": 5,
  "queue_processed": 113,
  "css_inlining_attempted": 87,
  "css_inlining_stylesheets_found": 340,
  "css_inlining_stylesheets_cached": 298,
  "css_inlining_bytes_inlined": 145200,
  "reanalyses_scheduled": 12,
  "scripts_analyzed": 156,
  "scripts_deferrable": 42
}
```

| Field                                                  | Description                                            |
| ------------------------------------------------------ | ------------------------------------------------------ |
| `chrome_running`                                       | Whether the managed Chrome instance is alive           |
| `chrome_pid`                                           | OS process ID of Chrome (0 if not running)             |
| `chrome_rss_mb`                                        | Chrome resident memory in megabytes                    |
| `queue_depth`                                          | URLs currently waiting for analysis                    |
| `analysis_in_progress`                                 | Whether an analysis is actively running                |
| `templates_tracked`                                    | Number of page templates detected                      |
| `profiles_generated`                                   | Total CSS coverage profiles completed                  |
| `profiles_used`                                        | Profiles that resulted in critical CSS injection       |
| `analysis_errors`                                      | Total analysis failures                                |
| `chrome_restarts`                                      | Times Chrome was restarted (planned)                   |
| `chrome_crashes`                                       | Times Chrome crashed unexpectedly                      |
| `queue_enqueued` / `queue_dropped` / `queue_processed` | Queue lifecycle counters                               |
| `css_inlining_*`                                       | Critical CSS inlining statistics                       |
| `reanalyses_scheduled`                                 | Pages queued for re-analysis (e.g., after CSS changes) |
| `scripts_analyzed` / `scripts_deferrable`              | Script deferral analysis counters                      |

#### Unknown commands

Any command other than `AUTH`, `STATS`, `PURGE`, `METRICS`, or `BROWSER-STATUS` returns:

```text
ERR unknown command\n
```

### Connection safety

The management socket disconnects clients whose buffer exceeds 16 KB without
containing a newline. This prevents denial-of-service from malformed input.

## Capability mask encoding

The 32-bit capability mask encodes client characteristics as a bitmask. Only
the lowest 8 bits are used; bits 8-31 are reserved and must be zero (except
for sentinel values).

### Bit layout

```text
Bit:    7    6    5    4    3    2    1    0
      +----+----+----+----+----+----+----+----+
      |  Transfer  |Save| Px |  Viewport | Format |
      |  Encoding  |Data|Den |   Class   |        |
      +----+----+----+----+----+----+----+----+
```

### Field definitions

| Bits | Field             | Width | Values                                               |
| ---- | ----------------- | ----- | ---------------------------------------------------- |
| 0-1  | Image Format      | 2     | `00` Original, `01` WebP, `10` AVIF, `11` SVG        |
| 2-3  | Viewport Class    | 2     | `00` Mobile, `01` Tablet, `10` Desktop               |
| 4    | Pixel Density     | 1     | `0` 1x, `1` 2x+ (Retina)                             |
| 5    | Save-Data         | 1     | `0` off, `1` on                                      |
| 6-7  | Transfer Encoding | 2     | `00` Identity, `01` Gzip, `10` Brotli, `11` Reserved |

### Common mask values

| Mask   | Hex    | Description                                                   |
| ------ | ------ | ------------------------------------------------------------- |
| `0x08` | `0x08` | Default: Desktop, Identity, Original format, 1x, no Save-Data |
| `0x09` | `0x09` | Desktop, Identity, WebP                                       |
| `0x0A` | `0x0A` | Desktop, Identity, AVIF                                       |
| `0x19` | `0x19` | Desktop, Identity, WebP, 2x                                   |
| `0x29` | `0x29` | Desktop, Identity, WebP, Save-Data                            |
| `0x49` | `0x49` | Desktop, Gzip, WebP                                           |
| `0x01` | `0x01` | Mobile, Identity, WebP                                        |
| `0x05` | `0x05` | Tablet, Identity, WebP                                        |

### Sentinel values

These alternate IDs are reserved for special purposes and are never produced by
header classification:

| Alternate ID | Constant                      | Purpose                                  |
| ------------ | ----------------------------- | ---------------------------------------- |
| `0x0C`       | `PS_SENTINEL_ORIGINAL`        | Durable original content, as the origin sent it |
| `0x1C`       | `PS_SENTINEL_EARLY_HINTS`     | Stores preload hints for 103 Early Hints |
| `0x2C`       | `PS_SENTINEL_WARMUP`          | Triggers full variant matrix generation  |
| `0x3C`       | `PS_SENTINEL_CONTENT_HASH`    | Content hash for change detection        |
| `0x4C`       | `PS_SENTINEL_SUBRESOURCE`     | Subresource metadata                     |
| `0x5C`       | `PS_SENTINEL_BROWSER_PROFILE` | Browser analysis profile data            |
| `0x6C`       | `PS_SENTINEL_HEADERS_SIDECAR` | Verbatim request-independent response headers |

`PS_SENTINEL_ORIGINAL` and `PS_SENTINEL_HEADERS_SIDECAR` are written with
`ps_cache_write_original()` and `ps_cache_write_headers_sidecar()`, not with
`ps_cache_write_sentinel()`, and neither is ever returned by selection — read
them by id when you want them.

### Cache key format

Cache entries are keyed by `SHA-256(URL, hostname)` using Cyclone's native key
system. Multiple variants of the same URL are stored as alternates within this
key, each identified by an `AlternateId` (the low 8 bits of the capability
mask). Per-alternate metadata carries the full 32-bit mask and content type.

Lookups use `PageSpeedSelector` for best-fit alternate selection in a single
pass over all stored alternates (O(alternates), not O(fallback_masks)).

### Variant fallback

When no exact match exists among the stored alternates, the `PageSpeedSelector`
scores alternates by mask similarity and returns the closest match. The original
content (stored at the default mask `0x08`) always serves as a fallback, so a
client always receives a response — either the optimal variant, a close variant,
or the original.

## C API

mod_pagespeed exposes a C API (`lib/pagespeed/pagespeed.h`) for embedding
the cache, HTML processor, CSS minifier, and capability mask logic into custom
applications. All functions use the `ps_` prefix. The API uses opaque handle
types and C linkage, so it can be called from any language with a C FFI.

### Handle types

| Type                       | Description                              |
| -------------------------- | ---------------------------------------- |
| `ps_cache_t`               | Cache volume handle (opaque)             |
| `ps_read_result_t`         | Result from a cache read (opaque)        |
| `ps_write_handle_t`        | Streaming write handle (opaque)          |
| `ps_html_result_t`         | Result from `ps_html_process()` (opaque) |
| `ps_scan_result_t`         | Result from `ps_html_scan()` (opaque)    |
| `ps_critical_css_result_t` | Result from critical CSS extraction      |
| `ps_html_transform_t`      | HTML transform handle (opaque)           |

### Error handling

All fallible functions return `ps_error_t` (marked `PS_NODISCARD`):

| Constant                     | Value | Description                       |
| ---------------------------- | ----- | --------------------------------- |
| `PS_OK`                      | `0`   | Success                           |
| `PS_ERR_NOT_FOUND`           | `1`   | Cache key or alternate not found  |
| `PS_ERR_IO`                  | `2`   | I/O error                         |
| `PS_ERR_CORRUPTED`           | `3`   | Data corruption detected          |
| `PS_ERR_NO_SPACE`            | `4`   | Cache volume full                 |
| `PS_ERR_INVALID_ARG`         | `5`   | Invalid argument                  |
| `PS_ERR_BUSY`                | `6`   | Resource busy                     |
| `PS_ERR_CLOSED`              | `7`   | Handle already closed             |
| `PS_ERR_TOO_MANY_ALTERNATES` | `8`   | Alternate limit exceeded          |
| `PS_ERR_EXISTS`              | `9`   | Entry already exists              |
| `PS_ERR_NOT_OWNED`           | `10`  | Caller does not own the resource  |
| `PS_ERR_VERSION_MISMATCH`    | `11`  | Incompatible cache format version |
| `PS_ERR_INTERNAL`            | `99`  | Internal error                    |

```c
ps_error_t err = ps_cache_open(&config, &cache);
if (err != PS_OK) {
    fprintf(stderr, "%s: %s\n", ps_error_name(err), ps_strerror(err));
    // ps_last_error_message() returns additional detail (thread-local)
}
```

**Error utility functions:**

| Function                  | Signature                                   | Description                                                   |
| ------------------------- | ------------------------------------------- | ------------------------------------------------------------- |
| `ps_error_name()`         | `const char* ps_error_name(ps_error_t err)` | Returns the error constant name (e.g., `"PS_ERR_IO"`)         |
| `ps_strerror()`           | `const char* ps_strerror(ps_error_t err)`   | Returns a human-readable error description                    |
| `ps_last_error_message()` | `const char* ps_last_error_message(void)`   | Returns thread-local detail message from the last failed call |

### Content type constants

| Constant           | Value | MIME type                |
| ------------------ | ----- | ------------------------ |
| `PS_CONTENT_HTML`  | `0`   | `text/html`              |
| `PS_CONTENT_CSS`   | `1`   | `text/css`               |
| `PS_CONTENT_JS`    | `2`   | `application/javascript` |
| `PS_CONTENT_IMAGE` | `3`   | `image/*`                |
| `PS_CONTENT_OTHER` | `4`   | (other)                  |

Use `ps_classify_content_type(header)` to map a `Content-Type` header string
to a `ps_content_type_t`, and `ps_content_type_mime(type)` for the reverse.

### Sentinel constants

| Constant                      | Value  | Purpose                           |
| ----------------------------- | ------ | --------------------------------- |
| `PS_SENTINEL_ORIGINAL`        | `0x0C` | Durable original content          |
| `PS_SENTINEL_EARLY_HINTS`     | `0x1C` | Preload hints for 103 Early Hints |
| `PS_SENTINEL_WARMUP`          | `0x2C` | Full variant matrix generation    |
| `PS_SENTINEL_CONTENT_HASH`    | `0x3C` | Content hash for change detection |
| `PS_SENTINEL_SUBRESOURCE`     | `0x4C` | Subresource metadata              |
| `PS_SENTINEL_BROWSER_PROFILE` | `0x5C` | Browser analysis profile data     |
| `PS_SENTINEL_HEADERS_SIDECAR` | `0x6C` | Verbatim request-independent headers |

### Cache functions

**Opening and closing:**

```c
ps_cache_config_t config;
ps_cache_config_init(&config);            // zero-fills and sets struct_size
config.volume_path = "/data/cache.vol";
config.volume_size = 1ULL << 30;          // 1 GB
config.enable_checksum = 1;
config.ram_cache_size = 64 * 1024 * 1024; // 64 MB RAM cache

ps_cache_t* cache = NULL;
ps_error_t err = ps_cache_open(&config, &cache);
// ...
ps_cache_close(cache);
```

**Reading (zero-copy via mmap):**

```c
ps_read_result_t* result = NULL;
ps_error_t err = ps_cache_read_best(cache, url, hostname, mask, &result);
if (err == PS_OK) {
    const uint8_t* data;
    size_t len;
    err = ps_read_content(result, &data, &len);  // pointer into mmap'd region
    uint32_t stored_mask = ps_read_mask(result);
    ps_content_type_t ct = ps_read_content_type(result);
    uint8_t flags = ps_read_flags(result);
    int ram_hit = ps_read_is_ram_hit(result);
    ps_read_free(result);
}
```

Other read functions:

| Function                        | Description                                       |
| ------------------------------- | ------------------------------------------------- |
| `ps_cache_read_best()`          | Best-fit alternate for a capability mask          |
| `ps_cache_read_alternate()`     | Read a specific alternate by ID                   |
| `ps_cache_read_early_hints()`   | Read the Early Hints sentinel                     |
| `ps_read_content()`             | Get pointer + length from a read result           |
| `ps_read_copy()`                | Copy content into a caller-owned buffer           |
| `ps_read_mask()`                | Get the stored capability mask                    |
| `ps_read_content_type()`        | Get the content type                              |
| `ps_read_origin_content_type()` | Get the original Content-Type string              |
| `ps_read_flags()`               | Get the flags byte (the `PS_FLAG_*` bits)         |
| `ps_read_is_ram_hit()`          | Whether the result came from RAM cache            |
| `ps_read_free()`                | Free a read result                                |

Stored origin cache state. `ps_read_cache_inserted_at()` (when the entry became
fresh, in Unix seconds) has always been available; the rest were added at
PS_API 1.2. Together they are what the RFC 9111 helpers below take as input:

| Function                        | Description                                         |
| ------------------------------- | --------------------------------------------------- |
| `ps_read_origin_max_age()`      | Origin `max-age`, in seconds                        |
| `ps_read_origin_s_maxage()`     | Origin `s-maxage`, in seconds                       |
| `ps_read_origin_cc_flags()`     | Origin Cache-Control directives (`PS_CC_ORIGIN_*`)  |
| `ps_read_origin_last_modified()`| Origin `Last-Modified` as a Unix timestamp          |
| `ps_read_origin_etag()`         | Origin `ETag`, verbatim (quotes and `W/` included)  |

All return `0` / `NULL` when the field was never recorded.

**Writing (streaming):**

```c
ps_write_params_t params;
/* The _auto macro takes the size from your own type, so it stays correct when
 * the struct grows. Do not use ps_write_params_init: it initializes only the
 * PS_API 1.1 prefix, and the origin-state fields below are then silently
 * dropped at the write. */
ps_write_params_init_auto(&params);
params.alternate_id = mask & 0xFF;
params.content_length = total_len;
params.full_mask = mask;
params.content_type = PS_CONTENT_CSS;
params.origin_ct = "text/css; charset=utf-8";

/* Origin state (optional, PS_API 1.2). cache_inserted_at must be the time the
 * ORIGIN generated the response: if it reached you through another cache
 * carrying `Age: N`, it was already N seconds old, and recording your own
 * clock instead grants it N extra seconds of apparent freshness. */
params.cache_inserted_at = ps_age_adjusted_insert_time(time(NULL), inbound_age);
params.origin_max_age = 600;
params.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PUBLIC;
params.origin_etag = "\"abc123\"";

ps_write_handle_t* wh = NULL;
ps_error_t err = ps_cache_write_begin(cache, url, hostname, scheme, &params, &wh);
if (err == PS_OK) {
    err = ps_write_data(wh, chunk1, chunk1_len);
    err = ps_write_data(wh, chunk2, chunk2_len);
    err = ps_write_close(wh);  // commits to cache
}
// On error: ps_write_abort(wh) discards the write
```

Write a sentinel entry (e.g., Early Hints):

```c
ps_write_handle_t* wh = NULL;
ps_cache_write_sentinel(cache, url, hostname,
                        PS_SENTINEL_EARLY_HINTS, hints_len, &wh);
ps_write_data(wh, hints_data, hints_len);
ps_write_close(wh);
```

Re-recording a sentinel **replaces** it: with one writer per URL the previous
entry under that id is removed before the new one is written, and a read always
returns the most recent entry. Concurrent writers for the same URL and id can
leave superseded entries behind, bounded by the storage layer's chain ceiling —
a later storage version makes replacement unconditional with no change on your
side. Because the write is streaming, the removal happens when you call
`ps_cache_write_sentinel` and the replacement exists only when you close the
handle: an abandoned write, or one the storage layer refuses at close, leaves
the URL with no entry under that id until the next store. That reads as a miss
— never wrong bytes.

Store the durable original of a resource (PS_API 1.3) — the bytes the origin
sent, kept so optimized variants can be rebuilt without re-fetching. Same
`ps_write_params_t`, and the origin state you supply is what decides how long
the entry may be used:

```c
ps_write_params_t params;
ps_write_params_init_auto(&params);
params.content_length = body_len;
params.content_type = PS_CONTENT_IMAGE;
params.origin_ct = "image/jpeg";
params.cache_inserted_at = ps_age_adjusted_insert_time(time(NULL), inbound_age);
params.origin_max_age = 600;
params.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PUBLIC;

ps_write_handle_t* wh = NULL;
if (ps_cache_write_original(cache, url, hostname, scheme, &params, &wh) == PS_OK) {
    ps_write_data(wh, body, body_len);
    ps_write_close(wh);
}
/* Read it back deliberately, by id — never through selection: */
ps_read_result_t* r = NULL;
ps_cache_read_alternate(cache, url, hostname, scheme, PS_SENTINEL_ORIGINAL, &r);
```

Responses above 16 MB are not kept as durable originals — above that size the
copy costs more than the rebuild it saves, and the limit is fixed in the build
rather than configurable. Declaring more returns `PS_ERR_NO_SPACE`; writing more
than you declared fails the `ps_write_data` call that crosses the limit and
abandons the entry, after which `ps_write_close` returns `PS_ERR_CLOSED` because
there is nothing left to commit. Nothing partial is stored, and nothing else
about the response is affected.

Check `ps_last_error_message()` on `PS_ERR_NO_SPACE`: the same code means "the
cache volume is full", which is transient and worth retrying, and "this response
is over the original-size limit", which is permanent for that response. The
message names the limit in the second case.

Re-recording an original **replaces** it: the previous one is removed before
the new one is written. That holds for a single writer per URL — a read always
returns the most recent original, and concurrent writers for the same URL can
leave superseded copies behind (bounded by the storage layer's chain ceiling,
but each one holds a whole response body, so serialise them if you write from
several processes). A later storage version makes replacement unconditional
with no change on your side.

Because the write is streaming, the removal happens when you call
`ps_cache_write_original` and the replacement exists only when you close the
handle: for the duration of your stream the URL has no stored original, and an
abandoned stream, one whose close the storage layer refuses, or an over-size
stream leaves none. That reads as a miss — a re-fetch,
never wrong bytes — but if keeping a stale original matters more than the gap,
read for one before you start.

One constraint to design around before writing originals in production: an
original you store is **not** fenced against a concurrent purge of the same URL.
The engine's purge fencing is internal to the optimizer process and cannot see
your write, so a store that commits after a purge has enumerated the URL
survives with its family gone. See the header notes on `ps_cache_write_original`.

**Response-header sidecar:**

A cache entry reproduces the headers the cache itself needs — `Cache-Control`,
`Content-Type`, `Content-Length`, `ETag`, `Last-Modified`, `Expires`. The
sidecar is where the rest of a response's *request-independent* headers are
kept, verbatim, so a response carrying (say) a `Content-Security-Policy` can be
served from cache with its headers intact instead of being left alone.

```c
const char* names[]  = { "Cache-Control", "Content-Security-Policy", "Vary" };
const char* values[] = { "max-age=600",   "default-src 'self'",      "Accept-Encoding" };

int verdict = 0, stored = 0;
ps_cache_write_headers_sidecar(cache, url, hostname, scheme,
                               names, values, 3, &verdict, &stored);
/* Read it back by id; the blob starts with a payload format version byte. */
ps_read_result_t* r = NULL;
ps_cache_read_alternate(cache, url, hostname, scheme,
                        PS_SENTINEL_HEADERS_SIDECAR, &r);
```

Pass the response's **complete** header set, not a curated one: this call is the
admission gate. The headers it keeps are the security policy headers
(`Content-Security-Policy`, `Strict-Transport-Security`,
`X-Content-Type-Options`, `Referrer-Policy`, `Permissions-Policy`, the frame
and XSS protection headers and the `Cross-Origin-*` family), the reporting
headers, `Link`, the static CORS headers (`Access-Control-Allow-Origin` among
them), the resource description headers (such as `Content-Language`,
`Content-Disposition` and `Allow`) and the origin's own `Vary` string. The list
is closed and not configurable — a header whose value depends on the request
must never be replayed to the next client.

A refusal is **not** an error. The call returns `PS_OK` with `*out_stored` 0 and
one of three verdicts: `PS_SIDECAR_SWAP_ELIGIBLE` (nothing outside the reproducible
set — it may have stored a block, or had nothing to store),
`PS_SIDECAR_FALL_THROUGH` (something cannot be reproduced; serve the response
plain), or `PS_SIDECAR_NEVER_OPTIMIZED` (a `Content-Security-Policy` or `Link`
carrying a nonce, which is never stored because replaying a nonce defeats the
policy).
`ps_headers_sidecar_classify()` answers the same question with no cache
involved.

Record the answer on the entry as well as acting on it: on a
`PS_SIDECAR_FALL_THROUGH` or `PS_SIDECAR_NEVER_OPTIMIZED` verdict, set
`PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE` (PS_API 1.8) in `params.flags` on the
write. (Those two verdicts specifically — the classifier also returns `-1` for
a malformed argument list, which says nothing about the response.) The origin's
headers are gone by serve time, so an entry with no marker is indistinguishable
from one whose headers were all reproducible: recording it at write time is the
only way a serve path can later tell the two apart and fall through to the
plain response instead of answering with headers it knows are incomplete.

**Read it back off the entry you stamped, by id.** The bit lives on that one
entry and nowhere else:

- A durable original is never returned by `ps_cache_read_best()` — that class
  is excluded from selection by design. Ask for it by id instead:
  `ps_cache_read_alternate(cache, url, hostname, scheme, PS_SENTINEL_ORIGINAL, &r)`,
  then `ps_read_flags(r)`. Looking for the marker on a best-fit result gets you
  an unmarked variant and a false all-clear.
- Variants the optimizer produces do **not** carry it. Every optimizer write
  path composes the flags byte of what it stores, and none of them forwards
  this bit — so a derived variant always reads back with this bit clear.
- A marker stamped on an ordinary entry through `ps_cache_write_begin()` lives
  at that alternate id, including the default-mask id the optimizer's own
  output uses — so it is visible on hits until optimization lands and gone
  afterwards, when the optimizer replaces that entry with one whose flags it
  composed itself.

The flag is advisory and gates nothing: a marked entry is stored, scored,
optimized and expired exactly as an unmarked one. Wherever a marked entry is
actually served it does participate in the weak `ETag` of a hit, along with the
rest of the flags byte, so a marked entry does not revalidate against an
unmarked one — the two are not served the same way, so they are not the same
representation.

A read always returns the most recent block. With a single writer per URL a
later store replaces the previous block; with concurrent writers for the same
URL, superseded blocks can accumulate (bounded by the storage layer's chain
ceiling) — serialise the writers if you store this class from several
processes. Between the removal and the commit there is a window in which a
reader finds no block, so treat a missing block as "this response is not
optimizable", never as "optimized, with no headers". The same concurrent-purge
constraint as `ps_cache_write_original` applies.

**Cache policy (RFC 9111):**

Pure functions — no cache handle, no I/O — so a front end decides reuse,
storability and the emitted `Cache-Control` with the engine's own answers
instead of a second implementation of RFC 9111 that disagrees in the cases
nobody tests.

| Function                    | Description                                                                 |
| --------------------------- | --------------------------------------------------------------------------- |
| `ps_evaluate_freshness()`   | Fresh / serve-no-cache / revalidate, plus age and remaining TTL              |
| `ps_freshness_config_init_sized()` | Default caps and per-content-type lifetimes (use `ps_freshness_config_init_auto`) |
| `ps_build_cache_control()`  | Assemble the `Cache-Control` value to send with a hit                       |
| `ps_vary_uncacheable()`     | Whether a response's `Vary` forbids storing it at all                       |
| `ps_vary_varies_accept()`   | Whether the origin negotiates on `Accept` itself, so the stored entry must carry `PS_FLAG_ORIGIN_VARIES_ACCEPT` |
| `ps_parse_cache_control()`  | Derive the `PS_CC_ORIGIN_*` flags from a `Cache-Control` header line         |

:::next-release{after="2.8.2"}
| Function                    | Description                                                                 |
| --------------------------- | --------------------------------------------------------------------------- |
| `ps_vary_uncacheable()`     | Whether a response's `Vary` forbids storing it at all: anything but `Accept-Encoding` does |
| `ps_vary_varies_accept()`   | Whether the origin negotiates on `Accept` itself, so the stored entry must carry `PS_FLAG_ORIGIN_VARIES_ACCEPT`; such a response is now refused, so this returns 0 |
:::

`ps_parse_cache_control()` accumulates: call it once per `Cache-Control` header
line, in order, against the same output — a response may carry several, and
RFC 9111 treats them as one list.

**Management:**

| Function                      | Description                                               |
| ----------------------------- | --------------------------------------------------------- |
| `ps_cache_alternate_exists()` | Check if a specific alternate exists                      |
| `ps_cache_remove()`           | Delete all alternates for a URL                           |
| `ps_cache_list_alternates()`  | List all alternates (returns `ps_alternate_info_t` array) |
| `ps_alternates_free()`        | Free the alternate list                                   |
| `ps_cache_stats()`            | Get cache statistics (`ps_cache_stats_t`); caller sets `struct_size` first.                 |

### Version

| Function             | Signature                    | Description          |
| -------------------- | ---------------------------- | -------------------- |
| `ps_version_major()` | `int ps_version_major(void)` | Major version number |
| `ps_version_minor()` | `int ps_version_minor(void)` | Minor version number |
| `ps_version_patch()` | `int ps_version_patch(void)` | Patch version number |

### Capability mask helpers

```c
// Classify request headers into a 32-bit capability mask
uint32_t mask = ps_classify(accept_header, user_agent, save_data, accept_encoding);

// Adjust viewport from a pixel width
mask = ps_mask_set_viewport_from_width(mask, 375);  // → Mobile

// Score how well a stored alternate matches a client mask
int score = ps_score_alternate(client_mask, stored_mask);

// Normalize a hostname for cache key computation
char normalized[256];
int len = ps_normalize_hostname("WWW.Example.COM:443", normalized, sizeof(normalized));
// normalized = "example.com", len = 11
```

| Function                  | Signature                                                                         | Description                                                                                                       |
| ------------------------- | --------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `ps_normalize_hostname()` | `int ps_normalize_hostname(const char* hostname, char* out_buf, size_t buf_size)` | Lowercases hostname, strips `www.` prefix and port suffix. Returns length of normalized string, or `-1` on error. |

### HTML processing

Single-call HTML processing with cache-backed resource lookup:

```c
ps_html_config_t config;
ps_html_config_init(&config);                    // PS_API 1.8 legacy: writes 128-byte prefix
// Or, for callers that may meet a newer library:
ps_html_config_init_sized(&config, sizeof(config));  // PS_API 1.9
config.enable_critical_css = 1;
config.enable_lazy_load = 1;
config.enable_lcp_preload = 1;
config.viewport = PS_VIEWPORT_DESKTOP;

ps_html_result_t* result = NULL;
ps_error_t err = ps_html_process(html, html_len, url, hostname,
                                  &config, cache, &result);
if (err == PS_OK) {
    size_t out_len;
    const char* output = ps_html_result_output(result, &out_len);
    int modified = ps_html_result_modified(result);
    int needs_reval = ps_html_result_needs_revalidation(result);

    size_t hints_len;
    const char* hints = ps_html_result_early_hints(result, &hints_len);

    ps_html_result_free(result);
}
```

For lower-level control, use the scan + transform pipeline:

```c
// Step 1: Scan HTML to extract structure
ps_scan_result_t* scan = NULL;
ps_html_scan(html, html_len, url, &scan);

// Step 2: Extract critical CSS using scan results
ps_critical_css_config_t css_config;
ps_critical_css_config_init(&css_config);                    // PS_API 1.8 legacy: writes 88-byte prefix
// Or, for callers that may meet a newer library:
ps_critical_css_config_init_sized(&css_config, sizeof(css_config));  // PS_API 1.9
ps_critical_css_result_t* crit = NULL;
ps_css_extract_critical(scan, css, css_len, &css_config, &crit);
size_t crit_len;
const char* critical_css = ps_critical_css_output(crit, &crit_len);

// Step 3: Transform HTML with critical CSS injected
ps_html_transform_t* xform = NULL;
ps_html_transform_create(scan, &html_config, critical_css, crit_len,
                          cache, hostname, NULL, 0, &xform);
char* out_html = NULL;
size_t out_len;
ps_html_transform_run(xform, html, html_len, url, &out_html, &out_len);

ps_free(out_html);
ps_html_transform_free(xform);
ps_critical_css_result_free(crit);
ps_scan_result_free(scan);
```

**Scanner inspection:**

| Function                     | Signature                                                                                                                                                     | Description                                                                   |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| `ps_scan_element_count()`    | `size_t ps_scan_element_count(const ps_scan_result_t* result)`                                                                                                | Number of above-the-fold elements found                                       |
| `ps_scan_element()`          | `ps_error_t ps_scan_element(const ps_scan_result_t* result, size_t index, const char** out_tag, const char** out_id, int* out_depth, int* out_element_index)` | Get tag, id, depth, and DOM index of element at `index`                       |
| `ps_scan_element_classes()`  | `size_t ps_scan_element_classes(const ps_scan_result_t* result, size_t index, const char*** out_classes)`                                                     | Get CSS classes for element at `index`; returns class count                   |
| `ps_scan_stylesheet_count()` | `size_t ps_scan_stylesheet_count(const ps_scan_result_t* result)`                                                                                             | Number of external stylesheets found                                          |
| `ps_scan_stylesheet()`       | `ps_error_t ps_scan_stylesheet(const ps_scan_result_t* result, size_t index, const char** out_href, const char** out_media)`                                  | Get href and media attribute of stylesheet at `index`                         |
| `ps_scan_inline_css()`       | `const char* ps_scan_inline_css(const ps_scan_result_t* result, size_t* out_len)`                                                                             | Concatenated inline `<style>` content                                         |
| `ps_scan_lcp_candidate()`    | `const char* ps_scan_lcp_candidate(const ps_scan_result_t* result, const char** out_srcset, const char** out_sizes, int* out_element_index)`                  | LCP image candidate (returns `src`, outputs `srcset`, `sizes`, element index) |
| `ps_scan_origin_count()`     | `size_t ps_scan_origin_count(const ps_scan_result_t* result)`                                                                                                 | Number of third-party origins detected                                        |
| `ps_scan_origin()`           | `const char* ps_scan_origin(const ps_scan_result_t* result, size_t index)`                                                                                    | Third-party origin URL at `index`                                             |
| `ps_scan_result_free()`      | `void ps_scan_result_free(ps_scan_result_t* result)`                                                                                                          | Free a scan result                                                            |

**HTML result inspection:**

| Function                            | Signature                                                              | Description                                      |
| ----------------------------------- | ---------------------------------------------------------------------- | ------------------------------------------------ |
| `ps_html_result_has_critical_css()` | `int ps_html_result_has_critical_css(const ps_html_result_t* result)`  | Whether the result contains inlined critical CSS |
| `ps_html_transform_modified()`      | `int ps_html_transform_modified(const ps_html_transform_t* transform)` | Whether the transform changed the HTML           |

**Critical CSS inspection:**

| Function                        | Signature                                                                                                           | Description                    |
| ------------------------------- | ------------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| `ps_critical_css_output()`      | `const char* ps_critical_css_output(const ps_critical_css_result_t* result, size_t* out_len)`                       | Extracted critical CSS string  |
| `ps_critical_css_stats()`       | `void ps_critical_css_stats(const ps_critical_css_result_t* result, int* out_total_rules, int* out_critical_rules)` | Total and critical rule counts |
| `ps_critical_css_result_free()` | `void ps_critical_css_result_free(ps_critical_css_result_t* result)`                                                | Free a critical CSS result     |

### CSS processing

```c
// Minify CSS (single call, no minifier object)
char* out_css = NULL;
size_t out_len;
ps_error_t err = ps_css_minify(css, css_len, &out_css, &out_len);
if (err == PS_OK) {
    // use out_css / out_len
    ps_free(out_css);
}

// Validate CSS syntax
ps_error_t err = ps_css_validate(css, css_len);

// Flatten @import directives (requires a lookup callback)
char* flat_css = NULL;
size_t flat_len;
int resolved, unresolved;
ps_css_flatten_imports(css, css_len, css_url, my_lookup_fn, user_data,
                       /*max_depth=*/3, &flat_css, &flat_len,
                       &resolved, &unresolved);
ps_free(flat_css);
```

### Configuration structs

**`ps_cache_stats_t`** — returned by `ps_cache_stats()`:

| Field                   | Type       | Description                          |
| ----------------------- | ---------- | ------------------------------------ |
| `struct_size`           | `size_t`   | Size of this struct (for ABI compat). Caller sets to `sizeof` as its build sees it before the call. Smaller than `sizeof(size_t)`, the call fails with `PS_ERR_INVALID_ARG`; otherwise the library fills the prefix and reports bytes written. |
| `ram_cache_hits`        | `uint64_t` | RAM cache hit count                  |
| `ram_cache_misses`      | `uint64_t` | RAM cache miss count                 |
| `disk_cache_hits`       | `uint64_t` | Disk cache hit count                 |
| `disk_cache_misses`     | `uint64_t` | Disk cache miss count                |
| `bytes_read`            | `uint64_t` | Total bytes read from cache          |
| `bytes_written`         | `uint64_t` | Total bytes written to cache         |
| `evictions`             | `uint64_t` | Total evictions                      |
| `current_entries`       | `uint64_t` | Current number of cache entries      |
| `current_size_bytes`    | `uint64_t` | Current cache size in bytes          |
| `volume_capacity_bytes` | `uint64_t` | Total volume capacity in bytes       |
| `ram_cache_bytes`       | `uint64_t` | Current RAM cache usage in bytes     |
| `total_hits`            | `uint64_t` | Total hits (RAM + disk)              |
| `total_misses`          | `uint64_t` | Total misses (RAM + disk)            |

**`ps_css_lookup_fn`** — callback for `ps_css_flatten_imports()`:

```c
typedef const char* (*ps_css_lookup_fn)(const char* url, size_t* out_len, void* user_data);
```

Called for each `@import` URL encountered during flattening. Return the CSS
content and set `*out_len` to its length. Return `NULL` if the URL cannot be
resolved (counted as `unresolved`). The `user_data` pointer is passed through
from `ps_css_flatten_imports()`.

### Worker notification

```c
// Send a fire-and-forget notification to the worker over a Unix socket
ps_error_t err = ps_notify_worker("/var/lib/pagespeed/pagespeed.sock",
                                   url, hostname, "https",
                                   PS_CONTENT_IMAGE, mask);
```

#### With a per-request options context

`ps_notify_worker_ex` (PS_API 1.5) takes everything above plus the
agent-request bit and the **options context**: the configuration a request
resolved to, as a canonical payload, together with a signature over it.

The signature is checked byte for byte against the payload it arrives with, on
every notification that carries one — that check is what catches two
implementations of the format drifting apart. A context that passes is accepted
whatever it names, and the work is processed and stored under the **default
context**: optimized output is a function of the resource, of the request
(client capabilities and content type, plus whether it was an agent request:
the request declares its own intent, and it counts only when the operator has
turned on `agent_optimize`, which the optimizer advertises to the module), and of
the optimizer's own configuration, so two
requests that resolved to two different configurations cannot produce two
different optimized artifacts. Keep supplying the context you resolve — it is
what the drift check runs on.

```c
ps_notify_params_t params;
ps_notify_params_init_auto(&params);          /* not ps_notify_params_init_sized(..., 0) */
params.url           = url;
params.hostname      = hostname;
params.scheme        = "https";
params.content_type  = PS_CONTENT_IMAGE;
params.mask          = mask;

/* Optional. Supply BOTH or NEITHER. */
char signature[PS_OPTION_CONTEXT_SIGNATURE_CHARS + 1];
ps_error_t err = ps_option_context_signature(payload, payload_len,
                                             signature, sizeof(signature));
if (err == PS_OK) {
  params.option_context        = payload;
  params.option_context_length = payload_len;
  params.option_signature      = signature;
}

err = ps_notify_worker_ex("/var/lib/pagespeed/pagespeed.sock", &params);
```

Rules the library enforces, each returning `PS_ERR_INVALID_ARG`:

| rule | why |
|---|---|
| payload and signature are supplied together, or neither | a signature can only be checked against the payload it came with, and that check is the point of carrying it |
| the signature must be the signature **of that payload** — it is checked, not trusted | a signature computed over something else means the two sides no longer agree on how a configuration is rendered; the receiver refuses such a notification too |
| the payload must not exceed `PS_MAX_OPTION_CONTEXT_BYTES` | nothing truncates a payload: half a payload signs as a context that does not exist |
| the payload must open with a format version this library knows | a payload read by the wrong parser produces a signature that means something else |

Omitting the options context entirely is the pre-existing behaviour in every
respect, and `ps_notify_worker` remains available and unchanged.

Use `ps_option_context_signature` rather than reimplementing it. Every consumer
that declares an options context has to produce byte-identical signatures for
byte-identical payloads indefinitely; a reimplementation that drifts by one byte
has every notification it sends refused, because the receiver re-derives the
signature from the payload rather than trusting the one on the wire.

### Memory management

Buffers allocated by the API (`ps_css_minify`, `ps_html_transform_run`) must
be freed with `ps_free()`. Opaque result types have dedicated free functions
(`ps_read_free`, `ps_html_result_free`, etc.).

### Thread safety

Cache operations are thread-safe (the underlying Cyclone cache uses lock-free
reads and per-bucket write locks). `ps_html_result_t`, `ps_scan_result_t`,
`ps_html_transform_t`, and write handles are not thread-safe — use one per
thread.

## Next steps

- [Configuration Reference](/docs/configuration/) — Worker flags including
  management socket and proactive variant options
- [HTTP API Reference](/docs/http-api/) — REST and WebSocket endpoints for
  the worker's HTTP API
- [Troubleshooting](/docs/troubleshooting/) — Using the management socket for
  diagnostics
- [Deployment Guide](/docs/deployment/) — Monitoring setup with STATS
