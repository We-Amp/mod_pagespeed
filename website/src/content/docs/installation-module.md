---
title: 'Install the module on Apache and nginx'
seoTitle: 'Install mod_pagespeed on Apache and nginx: signed packages'
description: 'Install the native mod_pagespeed module for Apache and nginx from the signed package repository: distributions, nginx compatibility, worker pairing.'
order: 3
group: 'Install'
lastUpdated: 2026-11-02
faq:
  - q: 'Is there a native nginx module for mod_pagespeed?'
    a: 'Yes. The native Apache and nginx module ships from the signed packages.modpagespeed.com repository — the same channel mod_pagespeed 1.15 uses.'
  - q: 'How do I run mod_pagespeed on nginx today?'
    a: 'Two options. Install the native nginx module — a dynamic module from the signed packages.modpagespeed.com repository, built for Debian and Ubuntu on amd64 and arm64 and pinned to each distribution stock nginx (1.18 to 1.28). Or put the Docker / nginx reverse proxy in front of your origin. Install the optimizer worker (pagespeed-optimizer) first, then the module, and set pagespeed DaemonSocketPath and pagespeed DaemonVolumePath in the server block so the module hands in-place optimization to the worker; the module also runs without the worker, on its own filters only.'
---

mod_pagespeed ships a native module for Apache and nginx. You have three
working options:

- **Native Apache module** — install from the signed
  [packages.modpagespeed.com repository](/download/apt-yum/), or the
  [cPanel / EasyApache 4 guide](/docs/cpanel/) on cPanel hosts.
- **Native nginx module** — install from the signed
  [packages.modpagespeed.com repository](/download/apt-yum/).
- **Docker / nginx reverse proxy** — run the
  [Docker / nginx reverse proxy](/docs/installation-docker/) in front of your
  origin.

Machine-readable recipes for coding agents: [modpagespeed.com/recipes/nginx.md](https://modpagespeed.com/recipes/nginx.md) and [modpagespeed.com/recipes/apache.md](https://modpagespeed.com/recipes/apache.md) (see [Install with a coding agent](/docs/agent-install/)).

## Native Apache module

The signed repository at `packages.modpagespeed.com` ships the Apache module
(`mod-pagespeed`) — the same channel mod_pagespeed 1.15 uses — for Apache
2.4+:

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install mod-pagespeed   # Debian/Ubuntu
sudo dnf install mod-pagespeed       # AlmaLinux/RHEL/Rocky
sudo systemctl restart apache2       # Debian/Ubuntu
sudo systemctl restart httpd         # RHEL family
```

The module keeps the same directive surface and filter names, so an existing
mod_pagespeed configuration carries over unchanged — see the
[configuration reference](/docs/configuration/) for the full directive
set. The package also drops a default `pagespeed.conf` (in
`/etc/apache2/mods-available/` on Debian/Ubuntu, `/etc/httpd/conf.d/` on
RHEL) — edit that file to turn filters on. The admin console
(`/pagespeed_admin/` and `/pagespeed_global_admin/`) is on after install and
limited to localhost; see the [admin console](/docs/admin-console/). The Apache packages also install the
configuration that points the module at the `pagespeed-optimizer` worker, so the
module reaches the worker without further configuration.

Running Apache under cPanel/WHM? Use the signed EasyApache 4 RPM instead —
see the [cPanel / EasyApache 4 guide](/docs/cpanel/). See
[Install from packages.modpagespeed.com](/download/apt-yum/) for the full
distribution matrix (including the yum packages) and the nginx module.

## Native nginx module

The signed repository at `packages.modpagespeed.com` ships the native nginx
module (`nginx-module-pagespeed`) — the same channel mod_pagespeed 1.15 uses —
for Debian and Ubuntu on amd64 and arm64, and for AlmaLinux, RHEL and Rocky 9
and 10. It is the ngx_pagespeed port of mod_pagespeed, built from the
mod_pagespeed source, and it loads into the nginx your distribution ships.
Each build is pinned to that distribution's stock nginx (1.18 through 1.28),
so there is no version to match by hand.

:::note[Two nginx modules, one file name]
Two different nginx modules install a file named `ngx_pagespeed_module.so`.
The **native nginx module** on this page comes from the `nginx-module-pagespeed`
package, runs inside your own nginx, and takes the `pagespeed <Directive>`
surface from the [configuration reference](/docs/configuration/)
(`pagespeed FileCachePath`, `pagespeed EnableFilters`, …); it answers with an
`X-Page-Speed` header. The **thin nginx module** ships only inside the
`pagespeed-nginx` and `pagespeed-combined` container images of the
[Docker / nginx reverse proxy](#docker--nginx-reverse-proxy), runs in the nginx
bundled there, and takes the `pagespeed_…` directives from the
[worker and reverse-proxy configuration](/docs/worker-configuration/)
(`pagespeed_cache_path`, …); it answers with `X-PageSpeed: MISS`, then `HIT`.
Apart from `pagespeed on;`, the directives of one are not valid in the other.
:::

The steps below install the module the way the Apache packages install the
Apache module: with the `pagespeed-optimizer` worker beside it. With the worker
paired, the module records each eligible CSS, JavaScript and image response
into the worker's cache, the worker builds the optimized variants off the
request path, and the module serves the variant that fits each client. The
module's own filters also work without the worker: leave out step 2 and the
`DaemonSocketPath` and `DaemonVolumePath` lines in step 5 to run it alone.

1. **Add the repository.**

   ```bash
   curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
   ```

2. **Install the optimizer worker, before the module.**

   ```bash
   sudo apt-get install pagespeed-optimizer   # Debian/Ubuntu
   sudo dnf install pagespeed-optimizer       # AlmaLinux/RHEL/Rocky
   ```

   The package creates the `pagespeed` user and group, and enables and starts
   `pagespeed-optimizer.service`. These steps are for a host that runs
   systemd; in a container, use the [Docker images](/docs/installation-docker/).

   Install the worker on its own, before the module, never both in one
   command: the module package adds the nginx user to the `pagespeed` group
   only if that group already exists when its install script runs, and in a
   single transaction dnf runs the module's script first. On a host where the
   module went in first or alongside, add the user yourself and restart nginx:
   `sudo usermod -a -G pagespeed www-data` on Debian and Ubuntu,
   `sudo usermod -a -G pagespeed nginx` on Enterprise Linux.

3. **Install the module.**

   ```bash
   sudo apt-get install nginx-module-pagespeed   # Debian/Ubuntu
   sudo dnf install nginx-module-pagespeed       # AlmaLinux/RHEL/Rocky
   ```

   The package loads the module itself (through `/etc/nginx/modules-enabled/`
   on Debian and Ubuntu, `/usr/share/nginx/modules/` on Enterprise Linux), so
   add no `load_module` line.

4. **Set the shared settings.** Create `/etc/nginx/conf.d/pagespeed.conf`;
   the stock `nginx.conf` of every supported distribution includes
   `conf.d/*.conf` inside its `http` block:

   ```nginx
   # Off in every server block that does not turn it on.
   pagespeed standby;
   pagespeed FileCachePath /var/cache/ngx_pagespeed;
   # The admin console; step 5 restricts it.
   pagespeed GlobalAdminPath /pagespeed_global_admin;
   ```

   `GlobalAdminPath` can only be set in the `http` block, and a
   `FileCachePath` there covers every server block. Leave out
   `pagespeed standby;` and the module is on in every server block, including
   the default server.

5. **Turn it on for your site.** Create the restriction for the admin pages,
   `/etc/nginx/snippets/pagespeed_admin_restrict.conf` (localhost only; add,
   for example, `allow 10.0.0.0/8;` before `deny all;` for your operations
   network). Enterprise Linux has no `snippets` directory, so create it first:

   ```bash
   sudo mkdir -p /etc/nginx/snippets
   sudo tee /etc/nginx/snippets/pagespeed_admin_restrict.conf >/dev/null <<'EOF'
   # PageSpeed admin pages, localhost only.
   location =  /pagespeed_admin         { allow 127.0.0.1; allow ::1; deny all; }
   location ^~ /pagespeed_admin/        { allow 127.0.0.1; allow ::1; deny all; }
   location =  /pagespeed_global_admin  { allow 127.0.0.1; allow ::1; deny all; }
   location ^~ /pagespeed_global_admin/ { allow 127.0.0.1; allow ::1; deny all; }
   EOF
   ```

   From 2.5.2, the `nginx-module-pagespeed` package installs the same four
   `location` lines as `/etc/nginx/snippets/pagespeed_admin_localhost.conf`,
   so you can include that file instead of creating the one above. The file
   is not included anywhere on its own: add
   `include /etc/nginx/snippets/pagespeed_admin_localhost.conf;` to each
   `server {}` block that serves the admin pages. It is a conffile on Debian
   and Ubuntu and `%config(noreplace)` on Enterprise Linux, so your edits
   survive upgrades. A copy you made earlier at
   `/etc/nginx/snippets/pagespeed_admin_restrict.conf` is left alone. Include
   one of the two files, not both: nginx rejects the duplicate `location`
   blocks. Remove the `include` lines before purging (deb) or removing (rpm)
   the package, or `nginx -t` fails.

   Then add the `pagespeed` and `include` lines below to each existing
   `server` block that should be optimized (on Debian and Ubuntu,
   `/etc/nginx/sites-available/default` or your site's own file), and keep
   that block's `root` and `location` lines. A `server` block without a `root`
   serves nginx's built-in welcome page, not your site:

   ```nginx
   server {
       listen 80;
       server_name <your-domain>;

       pagespeed on;
       # The optimizer worker: its notification socket and its cache volume.
       pagespeed DaemonSocketPath /run/pagespeed-optimizer/notify.sock;
       pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v3/cache;
       # Every server block with `pagespeed on` serves the admin console.
       include /etc/nginx/snippets/pagespeed_admin_restrict.conf;
   }
   ```

   On Enterprise Linux the stock server in `/etc/nginx/nginx.conf` includes
   `/etc/nginx/default.d/*.conf`; to turn the module on there, put
   `pagespeed on;`, the `DaemonSocketPath` and `DaemonVolumePath` lines and the
   `include` line in a file in that directory. That stock server has no
   `default_server`, and `conf.d/*.conf` is included before it, so a server
   you add in `conf.d` that listens on the same port becomes the default
   server.

6. **Restart nginx.**

   ```bash
   sudo nginx -t && sudo systemctl enable nginx && sudo systemctl restart nginx
   ```

   `enable` matters on Enterprise Linux, where the nginx that `dnf` pulled in
   does not start at boot otherwise. Restarting also applies the nginx user's
   new `pagespeed` group membership.

7. **Check it.**

   ```bash
   curl -s -o /dev/null -D - -H 'Host: <your-domain>' http://127.0.0.1/ | grep -i '^x-page-speed'
   curl -s -o /dev/null -w '%{http_code}\n' -H 'Host: <your-domain>' http://127.0.0.1/pagespeed_global_admin/
   ```

   Run them on the server. The first prints `X-Page-Speed: on` (the generic
   default; the version appears only with the server-wide
   [`ExposeVersionInHeaders`](/docs/configuration/#expose-version-in-headers)
   option), the second `200`. The same admin URL requested from another machine answers
   `403`. To open the console from your workstation, tunnel to the server
   (`ssh -L 8080:127.0.0.1:80 <you>@<server>`), point `<your-domain>` at
   `127.0.0.1` in the workstation's hosts file for the session, and browse to
   `http://<your-domain>:8080/pagespeed_global_admin/`: the request reaches
   nginx from `127.0.0.1` and selects your server block by its name. For a site
   that redirects port 80 to HTTPS, tunnel port 443 instead
   (`ssh -L 8443:127.0.0.1:443 <you>@<server>`) and browse to
   `https://<your-domain>:8443/pagespeed_global_admin/`. Its
   Statistics page shows whether the worker is in use: see
   [Check that it works](#check-that-it-works).

The [admin console guide](/docs/admin-console/#setup) covers the other handler
paths (`AdminPath`, `StatisticsPath`, …) and their restrictions, and
[Using the optimizer worker with nginx](#using-the-optimizer-daemon-with-nginx)
the details of the pairing: other paths, start order, upgrades, and what happens
when the worker is not available.

See [Install from packages.modpagespeed.com](/download/apt-yum/) for the full
distribution matrix (including the yum packages) and the Apache module. The
signed packages also sidestep the
[ngx_pagespeed build failures on modern nginx](/blog/ngx-pagespeed-wont-build-modern-nginx/)
that come from compiling the old module from source.

### nginx.org's nginx and the nginx Docker image

The `nginx-module-pagespeed-nginxorg` package is the same native module, built
for nginx.org's stable nginx on Debian 13 and Ubuntu 24.04 and on Enterprise
Linux 9, amd64 (x86_64) only. Each build is pinned to one nginx version, which
the package version names (`<version>+ngx1.30.6~trixie`; on Enterprise Linux
the package requires `nginx = 2:1.30.6`), so it must match the nginx you run,
and it cannot be installed alongside `nginx-module-pagespeed`. There is no
build for nginx.org's mainline, and none for Enterprise Linux 10.

nginx.org's `nginx.conf` does not read the module directory, so add its
include at the top of `/etc/nginx/nginx.conf`:
`include /etc/nginx/modules-enabled/*.conf;` on Debian and Ubuntu (Enterprise
Linux below).
The package ships its `load_module` line there and takes it away when removed,
so removing the package leaves nginx able to start.

**On a Debian or Ubuntu host** that runs nginx.org's repository, install the
package as in the steps above, with the other package name:
`sudo apt-get install nginx-module-pagespeed-nginxorg`. It takes the newest
build and, when nginx.org carries it, the nginx that build is for.

**On AlmaLinux, Rocky Linux or RHEL 9** with nginx.org's repository, install
the package with dnf:
`sudo dnf install nginx-module-pagespeed-nginxorg`, and add
`include /usr/share/nginx/modules/*.conf;` at the top of
`/etc/nginx/nginx.conf`. The package installs a dnf plugin that keeps a plain
`dnf update` working while nginx.org is ahead of the matching build: it holds
nginx back until that build is available, and `dnf check-update` still lists
the pending nginx. To take an nginx security fix first, run
`sudo dnf upgrade nginx --allowerasing`; that removes the module package, so
reinstall it when its build is available.

Not yet tested on Enterprise Linux:

- PackageKit and Cockpit updates likely bypass dnf plugins, so a plain update
  through them may still fail while nginx.org is ahead of the matching build.
  Use `dnf update` on the command line.
- dnf5 (Enterprise Linux 10) is untested; the package is for Enterprise
  Linux 9.
- Only AlmaLinux 9 has been exercised. Rocky Linux 9 and RHEL 9 are expected to
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

Then set the shared settings as in step 4 above.

**When nginx moves.** A new nginx version needs a matching module package,
which follows nginx.org's stable releases. Until it is available, `apt upgrade`
holds nginx back, and `apt full-upgrade` removes the module. If nginx ships a
security fix first, take the fix: remove the module, upgrade nginx, and
reinstall the module when its package is available. On Enterprise Linux the
dnf plugin holds nginx back the same way (see above). In Docker, move the image
tag only once the package for that version exists.

## nginx compatibility

Two shapes, two nginx stories. The native module runs inside the nginx your
distribution ships and is built for exactly that version; the Docker reverse
proxy brings its own nginx inside the image.

| Shape                                                                          | nginx version                   | Where it comes from                                |
| ------------------------------------------------------------------------------ | ------------------------------- | -------------------------------------------------- |
| Native module (`nginx-module-pagespeed`) on Debian 12 bookworm                 | 1.22.1                          | the distribution's stock nginx (amd64 + arm64)     |
| Native module on Debian 13 trixie                                              | 1.26.3                          | the distribution's stock nginx (amd64 + arm64)     |
| Native module on Ubuntu 22.04 jammy                                            | 1.18.0                          | the distribution's stock nginx (amd64 + arm64)     |
| Native module on Ubuntu 24.04 noble                                            | 1.24.0                          | the distribution's stock nginx (amd64 + arm64)     |
| Native module on Ubuntu 26.04 resolute                                         | 1.28.3                          | the distribution's stock nginx (amd64 + arm64)     |
| Native module on AlmaLinux / RHEL / Rocky 9                                    | 1.20.1                          | the distribution's stock nginx (x86_64 + aarch64)  |
| Native module on AlmaLinux / RHEL / Rocky 10                                   | the distribution's stock nginx  | the distribution's stock nginx (x86_64)            |
| Native module for nginx.org's stable nginx (`nginx-module-pagespeed-nginxorg`) on Debian 13, Ubuntu 24.04 and AlmaLinux / RHEL / Rocky 9 | 1.30.6 (nginx.org's stable; one nginx version per build) | nginx.org's repository (amd64 / x86_64) or the nginx Docker image (amd64) |
| Docker / nginx reverse proxy (`pagespeed-nginx` and `pagespeed-combined` images) | 1.30.6                        | bundled in the image                               |

nginx refuses to load a module built for a different version, and
`--with-compat` does not relax that check, so the native module follows your
distribution's nginx package, or nginx.org's stable with the
[separate package](#nginxorgs-nginx-and-the-nginx-docker-image). Running an
nginx we do not package? [Contact us](/contact/) for a matching pinned build. Debian 11
(bullseye) stays on the repository's final 1.15.0 module packages.

## Using the optimizer worker with nginx {#using-the-optimizer-daemon-with-nginx}

The native nginx module can hand in-place optimization to the optimizer
worker (the `pagespeed-optimizer` package), as the Apache module does. With two directives set in a `server` block, the
module records each eligible resource into the optimizer's cache, the
optimizer builds optimized variants of it, and the module serves the variant
that fits each client from that cache. With both directives unset, in-place
optimization works exactly as before.

### Before you start

- **Install the optimizer.** Install the `pagespeed-optimizer` package from
  the same repository (`sudo apt-get install pagespeed-optimizer` or
  `sudo dnf install pagespeed-optimizer`) and start it. The module and the
  optimizer are versioned separately, so their version numbers can differ:
  install the optimizer the repository currently offers, and upgrade or roll
  back the two together.
- **Let nginx reach the optimizer's files.** The optimizer runs as the
  unprivileged `pagespeed` user. Its cache directory, notification socket and
  shared configuration are open to members of the `pagespeed` group only. When
  `nginx-module-pagespeed` is installed or upgraded and that group exists, the
  package adds the nginx user to it (`www-data` on Debian and Ubuntu, `nginx`
  on the RHEL family). If you installed the module before the optimizer or in
  the same command, or nginx runs as another user, add the user yourself, for
  example `sudo usermod -a -G pagespeed www-data`, then restart nginx; the
  restart also applies the new group membership.
- **Find the two paths.** The packaged optimizer's defaults are
  `/run/pagespeed-optimizer/notify.sock` for the notification socket and
  `/var/cache/pagespeed-optimizer/v3/cache` for the cache volume. If you
  changed `SOCKET_PATH` or `CACHE_DIR` in `/etc/default/pagespeed-optimizer`,
  use those values instead: the volume path is the cache directory followed by
  `/cache`.

### Configuration

Add the `pagespeed` lines below to each existing `server` block that should
use the optimizer (on Debian and Ubuntu, `/etc/nginx/sites-available/default`
or your site's own file), and keep that block's `root` and `location` lines.
A `server` block without a `root` serves nginx's built-in welcome page, not
your site:

```nginx
server {
    listen 80;
    server_name <your-domain>;

    pagespeed on;
    pagespeed FileCachePath /var/cache/ngx_pagespeed;

    # The optimizer worker: its notification socket and its cache volume.
    pagespeed DaemonSocketPath /run/pagespeed-optimizer/notify.sock;
    pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v3/cache;
}
```

- `FileCachePath` works in either place: in the `server` block as above, or
  once in the `http` block as in [step 4](#native-nginx-module), which then
  covers every server block.
- Set the two together. With only one of them set, in-place optimization is
  off and the error log says that both are needed.
- Keep `DaemonVolumePath` on a different path from `FileCachePath`. The
  module's own cache and the optimizer's cache are separate.
- nginx connects to the optimizer when it starts. After you change either
  directive, restart nginx.

### Start order and upgrades

The packaged optimizer service is ordered ahead of `nginx.service` and waits
up to 30 seconds for its notification socket before it counts as started, so
at boot nginx starts after the optimizer. The ordering does not make nginx depend on the
optimizer: nginx still starts when the optimizer is absent. An nginx that
runs under another unit name is not ordered against the optimizer; give it an
ordering drop-in (`After=pagespeed-optimizer.service`).

When you upgrade, upgrade and start the optimizer first, let it create its
cache volume, then restart nginx. If nginx starts in between, in-place
optimization is off until you restart nginx, with the reason in the error log.
After an update that changes the cache format, a fresh start of nginx in that
window can fail instead, again with the reason in the error log. Starting the
optimizer and then restarting nginx clears both.

### When the optimizer is not available

If the directives are set but the optimizer cannot be used — it is not
running, it is a release this module cannot work with, or nginx is not allowed
to reach its files — then for that server:

- in-place optimization is off, and nginx serves every request normally;
- nginx logs the reason. When nginx is not allowed to reach the optimizer's
  files, the log does not name the `pagespeed` group: nginx repeats a warning
  that the optimizer's cache volume "cannot be used yet" because the worker
  "does not publish the size of its cache volume", once per attempt, and
  nothing is optimized; after about six minutes it stops retrying and logs one
  error. If the optimizer is running, add the nginx user to the `pagespeed`
  group (for example `sudo usermod -a -G pagespeed www-data`) and restart
  nginx;
- when nginx refuses the optimizer at startup, the same reason appears once in
  the admin console's [message history](/docs/admin-console/#message-history).

The module's own in-place path does not take over in that case. Fix the cause
and restart nginx.

### Check that it works

1. Run `sudo nginx -t`, restart nginx, and read the nginx error log. A working
   pair logs no repeated warning about the optimizer; the one error line you
   may see is a note about left-over cache volume files after you change the
   optimizer's cache size.
2. Request a stylesheet or script of your site a few times.
3. Open the module's statistics (the admin console's Statistics page, or your
   `StatisticsPath`). `ipro_daemon_served` counts in-place requests answered
   from the optimizer's cache, and `ipro_daemon_fallthrough` counts the ones
   passed to the ordinary path. Once the optimizer has built a variant,
   `ipro_daemon_served` rises. If only `ipro_daemon_fallthrough` rises, see
   [Limits](#limits).

The admin console's [optimizer worker panels](/docs/admin-console/#optimizer-daemon-panels)
read the optimizer's management API through the separate
`DaemonApiSocketPath` directive.

### Limits

- A response that carries headers this path cannot reproduce from the cache —
  a `Set-Cookie`, a CORS or security header, a `Vary` on another axis — is
  served by the ordinary path: complete, but not optimized in place. This
  includes headers your own nginx configuration adds to every response with
  `add_header` (an HSTS header, for example): the module cannot tell them
  apart from headers the origin sent. On a server that sets such a header
  site-wide, little or nothing is optimized in place and `ipro_daemon_served`
  does not rise.
- A request that nginx redirects internally (`rewrite`, a `try_files`
  fallback, `index`, `error_page`) is not optimized in place.
- By default the module serves the uncompressed optimized copy and nginx
  compresses it as configured. `pagespeed DaemonServeStoredEncodings on;`
  sends the optimizer's stored gzip and brotli copies instead; it is off by
  default.

## Docker / nginx reverse proxy

The [Docker / nginx reverse proxy](/docs/installation-docker/) runs the same
optimization core in front of any HTTP origin. It uses the thin nginx module,
bundled in its own nginx inside the `pagespeed-nginx` and `pagespeed-combined`
images and paired with the optimizer worker. The thin module's file is also
named `ngx_pagespeed_module.so`, but it is a different module from the native
one above (see [Two nginx modules, one file name](#native-nginx-module)). The
pair provides
[image transcoding](/blog/nginx-image-optimization-module/),
CSS/JS minification, critical CSS, and zero-copy serving from the Cyclone
shared-memory cache.

Run your site through a [PageSpeed Insights test](/analyze/) to see which
failing audits mod_pagespeed will fix, and read the
[Core Web Vitals](/core-web-vitals/) guide for the LCP, CLS, and INP plan.
