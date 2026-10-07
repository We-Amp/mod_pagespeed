# Installing PageSpeed for Nginx

## Requirements

- A supported Linux distribution running its **stock** nginx package
  (Debian 11/12/13, Ubuntu 22.04/24.04, AlmaLinux/RHEL/Rocky 9).
- `x86_64` or `arm64`.

The prebuilt module is a dynamic nginx module, and nginx pins dynamic modules to
the **exact** nginx version they were built against (down to the patch;
`--with-compat` does not relax this). The published packages are therefore built
against each distribution's **stock** nginx. If you run a *custom* nginx —
including the **nginx.org** repository's `nginx` (e.g. the 1.30.x stable line) —
no published package matches it; build the module from the public source tree
against your nginx instead (see `DEVELOPER.md`). The simplest path is your
distribution's stock nginx.

## Install

Add the We-Amp package repository (once), then install. All release artifacts are
also listed on the [downloads page](https://modpagespeed.com/1.1/docs/downloads/).

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh

sudo apt install nginx-module-pagespeed     # Debian / Ubuntu
sudo dnf install nginx-module-pagespeed     # RHEL / AlmaLinux / Rocky 9

sudo nginx -t && sudo systemctl reload nginx
```

On stock nginx the package's `load_module` directive is auto-enabled, so no
manual `nginx.conf` edit is needed.

## Configuration

```nginx
load_module modules/ngx_pagespeed_module.so;

http {
    # ...

    server {
        listen 80;
        server_name example.com;

        # Enable PageSpeed. This activates the CoreFilters rewrite level by
        # default (29 optimization filters, including combine_css,
        # combine_javascript, and rewrite_images).
        pagespeed on;
        pagespeed FileCachePath /var/cache/ngx_pagespeed;

        # Admin console (restrict access in production)
        pagespeed AdminPath /pagespeed_admin;
        pagespeed StatisticsPath /pagespeed_statistics;

        # Restrict the admin console and statistics to localhost. `=` covers
        # the page itself and `^~` every page below it, ahead of any
        # regular-expression location. The admin path and the pages below it
        # are one unit for access control: give both blocks the same rule
        # (use StatisticsPath to expose statistics on their own). Set the
        # paths without a trailing slash. A path set in the http {} block
        # instead is served by every server {} block and needs these blocks
        # in each of them.
        location =  /pagespeed_admin      { allow 127.0.0.1; allow ::1; deny all; }
        location ^~ /pagespeed_admin/     { allow 127.0.0.1; allow ::1; deny all; }
        location =  /pagespeed_statistics { allow 127.0.0.1; allow ::1; deny all; }

        # Enable additional filters outside the default CoreFilters set.
        # collapse_whitespace, insert_image_dimensions, and defer_javascript
        # are NOT in CoreFilters and must be enabled explicitly.
        # See filter-reference.md for filter membership.
        pagespeed EnableFilters collapse_whitespace,insert_image_dimensions;

        # Ensure PageSpeed resource requests are handled
        location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
            add_header "" "";
        }
        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon$" { }

        location / {
            proxy_pass http://backend;
        }
    }
}
```

## Cache Directory Setup

```bash
sudo mkdir -p /var/cache/ngx_pagespeed
sudo chown www-data:www-data /var/cache/ngx_pagespeed
```

## Verification

```bash
sudo nginx -t
sudo systemctl restart nginx
curl -I http://localhost/
# Should show an X-PageSpeed: <version> response header
```

## System Tests

```bash
./test/system/run_nginx_tests.sh
```

Expected: 169 pass, 40 skip, 0 fail.

## Using the optimizer daemon

The nginx module can hand in-place optimization to the optimizer daemon (the
`pagespeed-optimizer` package), as the Apache module does. With two directives
set in a `server {}` block, the module records each eligible resource into the
daemon's cache, the daemon builds optimized variants of it, and the module
serves the variant that fits each client from that cache. With both directives
unset, the classic in-place path is unchanged.

### Before you start

- **Install the daemon of the same release.** Install `pagespeed-optimizer`
  from the same package repository, at the same release as
  `nginx-module-pagespeed`, and start it. The module and the daemon are a
  matched pair: install, upgrade and roll them back together.
- **Let nginx reach the daemon's files.** The daemon runs as the unprivileged
  `pagespeed` user; its cache directory, notification socket and shared
  configuration are open to members of the `pagespeed` group only. When
  `nginx-module-pagespeed` is installed or upgraded and that group exists, the
  package adds the nginx user to it (`www-data` on Debian/Ubuntu, `nginx` on
  the RHEL family). If the module was installed before the daemon, or nginx
  runs as another user, add the user yourself
  (`sudo usermod -a -G pagespeed www-data`). Restart nginx afterwards: the
  membership takes effect only in a restarted nginx.
- **Find the two paths.** The packaged daemon's defaults are
  `/run/pagespeed-optimizer/notify.sock` (notification socket) and
  `/var/cache/pagespeed-optimizer/v2/cache` (cache volume). If `SOCKET_PATH`
  or `CACHE_DIR` was changed in `/etc/default/pagespeed-optimizer`, use those
  values: the volume path is the cache directory followed by `/cache`.

### Configuration

```nginx
server {
    listen 80;
    server_name example.com;

    pagespeed on;
    pagespeed FileCachePath /var/cache/ngx_pagespeed;

    # The optimizer daemon: its notification socket and its cache volume.
    pagespeed DaemonSocketPath /run/pagespeed-optimizer/notify.sock;
    pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v2/cache;
}
```

- Set the two together. With only one of them set, in-place optimization is
  off and the error log says that both are needed.
- Keep `DaemonVolumePath` on a different path from `FileCachePath`.
- nginx resolves the daemon when it starts. After changing either directive,
  restart nginx.

### Start order and upgrades

The packaged daemon service is ordered ahead of `nginx.service` and waits up
to 30 seconds for its notification socket before it counts as started, so at
boot nginx starts after the daemon. This is ordering only: nginx still starts
when the daemon is absent. An nginx under another unit name is not ordered
against the daemon; give it an ordering drop-in
(`After=pagespeed-optimizer.service`).

When upgrading, upgrade and start the daemon first, let it create its cache
volume, then restart nginx. If nginx starts in between, in-place optimization
is off until nginx is restarted, with the reason in the error log. After an
update that changes the cache format, a fresh start of nginx in that window can
fail instead, again with the reason in the error log. Starting the daemon and
then restarting nginx clears both. See
`docs/daemon-adapter-deployment.md` for the cache-volume details.

### When the daemon is not available

If the directives are set but the daemon cannot be used (not running, a
release this module cannot work with, or nginx is not allowed to reach its
files), in-place optimization is off for that server and nginx serves every
request normally. One error line at startup names the cause and tells a
missing `pagespeed` group membership apart from a daemon that is not running;
the same reason appears once in the admin console's message history. The
classic in-place path does not take over. Fix the cause and restart nginx.

### Checking it works

1. `sudo nginx -t`, restart nginx, and read the error log. A working pair logs
   no error about the daemon; each problem logs one line.
2. Request a stylesheet or script of the site a few times.
3. Read the module's statistics (`StatisticsPath`, or the admin console's
   Statistics page). `ipro_daemon_served` counts in-place requests answered
   from the daemon's cache; `ipro_daemon_fallthrough` counts the ones passed
   to the ordinary path. Once the daemon has built a variant,
   `ipro_daemon_served` rises. If only `ipro_daemon_fallthrough` rises, see
   "Limits" below.

The admin console's optimizer panels read the daemon's management API through
the separate `DaemonApiSocketPath` directive.

### Limits

- A response carrying headers this path cannot reproduce from the cache (a
  `Set-Cookie`, a CORS or security header, a `Vary` on another axis) is served
  by the ordinary path: complete, but not optimized in place. This includes
  headers the nginx configuration adds to every response with `add_header`
  (an HSTS header, for example): the module cannot tell them apart from
  headers the origin sent. On a server that sets such a header site-wide,
  little or nothing is optimized in place and `ipro_daemon_served` does not
  rise.
- A request that nginx redirects internally (`rewrite`, a `try_files`
  fallback, `index`, `error_page`) is not optimized in place.
- The module serves the uncompressed optimized copy by default; see the next
  section to send the stored compressed copies.

## Serving the optimizer's compressed copies

When the module is paired with the optimizer daemon (`pagespeed
DaemonSocketPath` and `pagespeed DaemonVolumePath`), the optimizer stores a
gzip and a brotli copy next to each optimized stylesheet, script and SVG
image. By default the module serves the uncompressed optimized copy and nginx's
`gzip` (or a brotli module) compresses it as configured. To send the stored
copies instead:

```nginx
server {
    # ...
    pagespeed DaemonServeStoredEncodings on;   # default: off
}
```

With it on, a client that lists `br` or `gzip` by name in `Accept-Encoding`
(weight above zero) is sent the stored copy in that coding, with
`Content-Encoding`, `Vary: Accept-Encoding` on the 200 and the 304, a
validator of its own and the stored size as `Content-Length`; nginx's `gzip`
and brotli filters leave a response that already names a coding alone. Other
clients, HTML, and resources without a stored copy in the client's coding get
the uncompressed copy exactly as before. The optimizer's serve statistics then
count the compressed size as the bytes served (`serve_savings.<type>.by_encoding`
splits them by coding). Setting it back to `off` restores the previous
responses exactly. The default changes to `on` in a later release.

Your compression exclusions do not apply to these responses: a stored copy is
sent to every client that lists its coding, whatever `gzip_disable` or
`gzip_proxied` say for that request. Such a client did list the coding, so it
can decode the copy, but if you rely on those settings, leave the directive
off for that server block. With `gunzip on`, nginx may decompress a stored
gzip copy for a client that fails its own gzip checks; the body is then
correct, and keeps the gzip copy's `ETag`. Body filters such as `sub_filter`
see the compressed bytes of these responses.

If no server serves the stored copies, setting the optimizer's `gzip_level`
and `brotli_level` to `0` stops it writing them. Raising a level from `0`
again does not add copies to resources that are already optimized: they gain
them only when they are next optimized.

## Known Limitations

- Lazyload images filter may time out
- Inline preview images filter may time out
- Canonicalize JS libraries filter may time out
- ETag not added to `.pagespeed.` resources
- Chunked encoding differences affect Content-Length tests

See [test-catalog.md](test-catalog.md) for full details.

### Static builds (`--add-module`)

The module can also be compiled into the nginx binary with
`./configure --add-module=.../pagespeed/nginx`. No published package is built
that way and the build is not tested. The dynamic module keeps its own TLS
library (BoringSSL) apart from nginx's: a linker version script hides the
library's symbols, and every release build is checked to leave no
cryptographic symbol for nginx to resolve. A static build has no such
separation. It links that library and nginx's own OpenSSL into one executable
with a single symbol namespace, and nothing in the build decides which of the
two a given call is bound to. When building from source, prefer the dynamic
module (`--add-dynamic-module`).

## Next steps: configuration & operations

By default (`pagespeed on;`) the `CoreFilters` rewrite level is active, which enables 29 optimization filters automatically; a bare `pagespeed on;` is not a no-op. To enable additional filters not in CoreFilters (for example `collapse_whitespace` or `defer_javascript`), use `pagespeed EnableFilters`. To enable all non-dangerous filters, use `pagespeed RewriteLevel AllFilters`. For a smaller footprint, use `pagespeed RewriteLevel OptimizeForBandwidth`, or `pagespeed RewriteLevel PassThrough` to disable all default optimization.

Full configuration and operations documentation lives on modpagespeed.com:

- [Filter selection](https://modpagespeed.com/1.1/docs/filter-selection/) — rewrite levels and per-filter enable/disable
- [Filter reference](https://modpagespeed.com/1.1/docs/filter-reference/) — what each filter does and which level enables it
- [Admin console](https://modpagespeed.com/1.1/docs/admin-console/) — statistics, cache inspection, and cache purging
