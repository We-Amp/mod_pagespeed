# Installing PageSpeed for Nginx

## Requirements

- A supported Linux distribution running its **stock** nginx package
  (Debian 12/13, Ubuntu 22.04/24.04/26.04, AlmaLinux/RHEL/Rocky 9/10; Debian 11
  stays on the repository's final 1.15.0 packages).
- `x86_64` or `arm64` (AlmaLinux/RHEL/Rocky 10: `x86_64` only).

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
also listed on the [releases page](https://github.com/We-Amp/mod_pagespeed/releases).

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh

sudo apt install nginx-module-pagespeed     # Debian / Ubuntu
sudo dnf install nginx-module-pagespeed     # RHEL / AlmaLinux / Rocky 9 and 10

sudo nginx -t && sudo systemctl reload nginx
```

On stock nginx the package's `load_module` directive is auto-enabled, so no
manual `nginx.conf` edit is needed.

## nginx.org's nginx and the nginx Docker image

The `nginx-module-pagespeed-nginxorg` package is the same module, built for
nginx.org's stable nginx on Debian 13, Ubuntu 24.04 and Enterprise Linux 9
(AlmaLinux, Rocky Linux, RHEL), amd64 (x86_64) only. Each build is pinned to
one nginx version, which the package version names
(`<version>+ngx1.30.6~trixie`, `<version>-0.ngx1.30.6.el9`), so it must match
the nginx you run, and it cannot be installed alongside
`nginx-module-pagespeed`. There is no build for Enterprise Linux 10, and none
for nginx.org's mainline.

On Debian and Ubuntu, nginx.org's `nginx.conf` does not read
`modules-enabled`, so add that include.
The package ships its `load_module` line there and takes it away when removed,
so removing the package leaves nginx able to start.

**On a host** that runs nginx.org's repository, install the package as on a
distribution: `apt-get install nginx-module-pagespeed-nginxorg` takes the
newest build and, when nginx.org carries it, the nginx that build is for.

**On AlmaLinux, Rocky Linux or RHEL 9** with nginx.org's repository, install
the package as on a distribution: `sudo dnf install nginx-module-pagespeed-nginxorg`,
and add `include /usr/share/nginx/modules/*.conf;` at the top of
`/etc/nginx/nginx.conf`. The package installs a dnf plugin that keeps a plain
`dnf update` working while nginx.org is ahead of the matching build: it holds
nginx back until that build is available, and `dnf check-update` still lists
the pending nginx. To take an nginx security fix first:
`sudo dnf upgrade nginx --allowerasing` (removes the module package; reinstall
it when its build is available). The package's README has the details. Not
tested so far:

- Updates through PackageKit or Cockpit are likely to bypass dnf plugins, so a
  plain update there may still fail while nginx.org is ahead of the matching
  build; use `dnf update` on the command line.
- dnf5 (Enterprise Linux 10) is untested; the package is for Enterprise Linux 9.
- Only AlmaLinux 9 has been exercised; Rocky Linux 9 and RHEL 9 are expected to
  behave the same but are untested.

**In the nginx Docker image**, install the build for the image's nginx by its
version: `apt-cache madison` lists the builds, the one whose version carries
`+ngx<nginx version>~` is the match, and the image build fails at `test -n`
rather than moving nginx when no package exists for that version yet.
`NGINX_VERSION` is set by the image. Pin the tag to the exact stable version:
`nginx:latest` follows mainline and the `alpine` variants use another package
format, so neither works with this package.

```dockerfile
FROM nginx:1.30.6
RUN apt-get update && apt-get install -y curl ca-certificates gnupg \
 && curl -fsSL https://packages.modpagespeed.com/install.sh | sh \
 && apt-get update \
 && v="$(apt-cache madison nginx-module-pagespeed-nginxorg \
        | awk -v n="+ngx${NGINX_VERSION}~" 'index($3, n) { print $3; exit }')" \
 && test -n "$v" \
 && apt-get install -y "nginx-module-pagespeed-nginxorg=$v" \
 && sed -i '1i include /etc/nginx/modules-enabled/*.conf;' /etc/nginx/nginx.conf \
 && mkdir -p /var/cache/ngx_pagespeed && chown nginx /var/cache/ngx_pagespeed
```

Then enable PageSpeed in your server configuration as shown below.

**When nginx moves.** A new nginx version needs a matching module package,
which follows nginx.org's stable releases. Until it is available, `apt upgrade`
holds nginx back, and `apt full-upgrade` removes the module (on Enterprise
Linux 9, see the dnf notes above). If nginx ships a
security fix first, take the fix: remove the module, upgrade nginx, and
reinstall the module when its package is available. In Docker, move the image
tag only once the package for that version exists.

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

### Settings in `location` blocks

`pagespeed` directives can be scoped to a `location` block, for example to
turn PageSpeed off for part of a site:

```nginx
location /shop/ {
    pagespeed off;
}
```

Settings put on a location also apply inside its nested locations and its
`if` and `limit_except` blocks when those carry no `pagespeed` directives of
their own; a nested block that sets its own directives keeps them and
inherits the rest of the enclosing location's settings. To run a nested block
with different settings, put the directives in the nested block itself.

### HTML caching

Rewritten HTML is served as not cacheable by shared caches by default, and it
carries `Vary: Accept, Save-Data, User-Agent` (the headers its image URLs and
inlining depend on; fewer when fewer image filters are on). If you let a CDN or
proxy store the HTML (for example with `pagespeed ModifyCachingHeaders off`, or
with `pagespeed DownstreamCachePurgeLocationPrefix` set), see "Behind a shared
cache or CDN" in [daemon-adapter-deployment.md](daemon-adapter-deployment.md),
including how to make the HTML the same for every client with
`ImagePreserveURLs on`.

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

Expected: 173 pass, 40 skip, 0 fail.

## Using the optimizer worker

The nginx module can hand in-place optimization to the optimizer worker (the
`pagespeed-optimizer` package), as the Apache module does. With two directives
set in a `server {}` block, the module records each eligible resource into the
worker's cache, the worker builds optimized variants of it, and the module
serves the variant that fits each client from that cache. With both directives
unset, the classic in-place path is unchanged.

### Before you start

- **Install the worker of the same release.** Install `pagespeed-optimizer`
  from the same package repository, at the same release as
  `nginx-module-pagespeed`, and start it. The module and the worker are a
  matched pair: install, upgrade and roll them back together.
- **Let nginx reach the worker's files.** The worker runs as the unprivileged
  `pagespeed` user; its cache directory, notification socket and shared
  configuration are open to members of the `pagespeed` group only. When
  `nginx-module-pagespeed` is installed or upgraded and that group exists, the
  package adds the nginx user to it (`www-data` on Debian/Ubuntu, `nginx` on
  the RHEL family). If the module was installed before the worker, or nginx
  runs as another user, add the user yourself
  (`sudo usermod -a -G pagespeed www-data`). Restart nginx afterwards: the
  membership takes effect only in a restarted nginx.
- **Find the two paths.** The packaged worker's defaults are
  `/run/pagespeed-optimizer/notify.sock` (notification socket) and
  `/var/cache/pagespeed-optimizer/v3/cache` (cache volume). If `SOCKET_PATH`
  or `CACHE_DIR` was changed in `/etc/default/pagespeed-optimizer`, use those
  values: the volume path is the cache directory followed by `/cache`.

### Configuration

```nginx
server {
    listen 80;
    server_name example.com;

    pagespeed on;
    pagespeed FileCachePath /var/cache/ngx_pagespeed;

    # The optimizer worker: its notification socket and its cache volume.
    pagespeed DaemonSocketPath /run/pagespeed-optimizer/notify.sock;
    pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v3/cache;
}
```

- Set the two together. With only one of them set, in-place optimization is
  off and the error log says that both are needed.
- Keep `DaemonVolumePath` on a different path from `FileCachePath`.
- nginx resolves the worker when it starts. After changing either directive,
  restart nginx.

### Start order and upgrades

The packaged worker service is ordered ahead of `nginx.service` and waits up
to 30 seconds for its notification socket before it counts as started, so at
boot nginx starts after the worker. This is ordering only: nginx still starts
when the worker is absent. An nginx under another unit name is not ordered
against the worker; give it an ordering drop-in
(`After=pagespeed-optimizer.service`).

When upgrading, upgrade and start the worker first, let it create its cache
volume, then restart nginx. If nginx starts in between, in-place optimization
is off, with the reason in the error log, and nginx picks the worker up by
itself once it answers: with requests arriving, at the latest about 30
seconds after the worker starts, one info line per worker process says so.
After an update that changes the cache format, a fresh start of nginx in
that window can fail instead, again with the reason in the error log.
Starting the worker and then restarting nginx clears both. See
`docs/daemon-adapter-deployment.md` for the cache-volume details.

### When the worker is not available

If the directives are set but the worker cannot be used (not running, a
release this module cannot work with, or nginx is not allowed to reach its
files), in-place optimization is off for that server and nginx serves every
request normally. One error line at startup names the cause and tells a
missing `pagespeed` group membership apart from a worker that is not running;
the same reason appears once in the admin console's message history. The
classic in-place path does not take over. A worker that was only not
running needs no restart: with requests arriving, each worker process
checks for it again at most once every 30 seconds and turns in-place
optimization back on once the worker answers. Every other cause -- a
release this module cannot work with, nginx not being allowed to reach
its files, or a split cache volume -- still needs the cause fixed and a
restart.

### Checking it works

1. `sudo nginx -t`, restart nginx, and read the error log. A working pair logs
   no error about the worker; each problem logs one line.
2. Request a stylesheet or script of the site a few times.
3. Read the module's statistics (`StatisticsPath`, or the admin console's
   Statistics page). `ipro_daemon_served` counts in-place requests answered
   with an optimized variant from the worker's cache;
   `ipro_daemon_fallthrough` counts the ones passed to the ordinary path.
   `ipro_daemon_served_original` counts the ones answered with the stored
   original's own bytes: the worker held no variant for the URL yet, which
   is every such answer while the optimizer service is down. Once the worker
   has built a variant, `ipro_daemon_served` rises. If only
   `ipro_daemon_fallthrough` rises, see "Limits" below; if
   `ipro_daemon_served_original` rises instead, the optimizer service is
   answering no variants -- check that it is running.

The admin console's optimizer panels read the worker's management API through
the separate `DaemonApiSocketPath` directive.

### Limits

- A response carrying headers this path cannot reproduce from the cache (a
  `Set-Cookie`, a CORS or security header, a `Vary` on another axis) is served
  by the ordinary path: complete, but not optimized in place. This includes
  headers the nginx configuration adds to every response with `add_header`
  (an HSTS header, for example): the module cannot tell them apart from
  headers the origin sent. On a server that sets such a header site-wide,
  little or nothing is optimized in place: `ipro_daemon_served` does not
  rise, and those requests land in `ipro_daemon_fallthrough`.
- An optimized image is chosen by `Accept`, the device class in `User-Agent`,
  `Sec-CH-DPR` when sent, and `Save-Data`, and says so in `Vary: Accept, Save-Data, User-Agent, Sec-CH-DPR`. For
  a cache or CDN in front of nginx, see "Behind a shared cache or CDN" in
  `docs/daemon-adapter-deployment.md`.
- A request that nginx redirects internally (`rewrite`, a `try_files`
  fallback, `index`, `error_page`) is not optimized in place.
- The module serves the uncompressed optimized copy by default; see the next
  section to send the stored compressed copies.

## Serving the optimizer's compressed copies

When the module is paired with the optimizer worker (`pagespeed
DaemonSocketPath` and `pagespeed DaemonVolumePath`), the optimizer stores a
gzip and a brotli copy next to each optimized stylesheet, script and SVG
image. By default the module serves the uncompressed optimized copy and nginx's
`gzip` (or a brotli module) compresses it as configured. To send the stored
copies instead:

```nginx
server {
    # ...
    pagespeed DaemonServeStoredEncodings on;   # default: off
    pagespeed DaemonServeHonorRequestFreshness on;   # default: off; a reload bypasses the cache
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

- Waiting for a `RemoteConfigurationUrl` refresh can hold up the nginx
  worker for up to `RemoteConfigurationTimeoutMs` (one second by default).
  With `UseNativeFetcher on` and the default file cache the module does
  not wait: requests during a refresh run with the local configuration,
  and later requests use the refreshed copy. With an external cache
  (memcached or redis) the module still waits, and nginx logs a warning
  about the combination at startup.
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
