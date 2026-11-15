# Upgrading to mod_pagespeed

This document covers upgrading to mod_pagespeed:

- **From 2.8.x or earlier 2.x:** the cache starts empty, and responses
  fetched for visitors with cookies are shared only when the origin marks
  them shareable — see the first section below.
- **From 2.4.0 or earlier 2.x:** the cache directory moves from `v2` to `v3` and
  the cache starts empty — see the first section below.
- **From 2.1.0:** the cache directory moves from `v1` to `v2` and the cache
  starts empty — see the first section below.
- **From 1.x (1.15):** an in-place package upgrade — see the second section.
- **From 2.0:** the cache cold-start, start-order and configuration notes in
  the third section.
- The long appendix at the end describes the 1.x → 2.0 architecture change
  for readers of the nginx line. It is background, not a migration guide:
  a 1.x install upgraded to 2.1.0 or later through the packages does not go through it.

## Upgrading from 2.8.x

- **The cache starts empty.** Entries stored by earlier versions are never
  used: they stay in the volume until they are evicted, and the cache refills
  as traffic arrives. The cache directory and file do not change. During a
  rolling restart, old and new processes do not share entries, so the hit rate
  stays low until every process runs the new version.
- **Fewer cache hits for traffic with cookies.** A page fetched for a request
  that carries a `Cookie` header is cached and shared only when the origin
  sends `Cache-Control: public` or an `s-maxage` above zero, and a cached page
  is served to such a request only with the same mark. Sites that set a cookie
  on every request (analytics, consent) will see fewer cache hits and more
  requests reaching the origin unless they mark the pages that are the same
  for every visitor, for example `Cache-Control: public, max-age=600`. Do not
  mark pages that differ per visitor. "Page" here means every response that is
  not a stylesheet, script or image.
- **Static assets without Cache-Control.** On those same sites, stylesheets,
  scripts and images that the origin serves without `Cache-Control` (or
  `Expires`) are no longer cached from a request that carries a cookie, so
  they are cached less until the origin sends an explicit `max-age`. Most real
  assets already have one.
- **Server blocks keep their own entries (nginx module).** Server blocks
  that share a cache path no longer share cached entries or optimized
  variants, also when they list the same host names; each block stores its
  own copy of a URL. A block's entries belong to its `server_name` list and
  its `listen` addresses, so changing either starts that block with an empty
  cache.
- **Authorization** handling is unchanged.

## Upgrading from 2.4.0

The cache storage library moved to a new on-disk format (major 9: the document
checksum covers the fixed document header, so a damaged header field reads as
a miss instead of being served). The optimizer's versioned cache directory
moves with it: the default is now `/var/cache/pagespeed-optimizer/v3` (2.4.0
used `.../v2`), and the `cache_dir_generation` the optimizer publishes in
`pagespeed-shared.conf` is now `3`. This applies to the optimizer worker, the
nginx module and the `WeAmp.PageSpeed.NativeAssets.*` NuGet packages.

- **The cache starts empty.** The new optimizer opens a new volume in `v3/`
  and never reads `v2/`. The first requests after the upgrade are misses and
  the cache refills as traffic arrives -- pick a quiet hour if your origin is
  sensitive to that.
- **The old cache is left on disk.** Nothing deletes `v2/` for you. A
  rollback to 2.4.0 reopens it still warm; once you will not roll back,
  delete it (`rm -rf /var/cache/pagespeed-optimizer/v2`). The disk needs
  room for both volumes until you do. The package prints a notice whenever
  a loaded web-server configuration still names `v2`.
- **Rolling restarts use separate files.** Old and new versions never share
  a cache file, so disk use temporarily doubles and the hit rate stays low
  until every process runs the new version.
- **Repoint web-server configuration that names `v2`.** A default package
  install needs nothing. If your configuration names the cache path
  explicitly (`pagespeed_cache_path` in nginx, `ModPagespeedDaemonVolumePath`
  in Apache), change `.../v2/cache` to `.../v3/cache` and restart the web
  server. The nginx module compares the generation published beside the
  configured path with its own and, on a mismatch, keeps serving with its
  cache turned off; see the section below for the log line and variables.

## Upgrading from 2.1.0

The cache storage library moved to a new on-disk format (major 8: CRC-32C
document checksums). A cache directory holds one format, so the optimizer's
versioned cache directory moves with it: the default is now
`/var/cache/pagespeed-optimizer/v2` (2.1.0 used `.../v1`), and the
`cache_dir_generation` the optimizer publishes in `pagespeed-shared.conf`
is now `2`.

- **The cache starts empty.** The new optimizer opens a new volume in `v2/`
  and never reads `v1/`. The first requests after the upgrade are misses and
  the cache refills as traffic arrives -- pick a quiet hour if your origin is
  sensitive to that.
- **The old cache is left on disk.** Nothing deletes `v1/` for you. A
  rollback to 2.1.0 reopens it still warm; once you will not roll back,
  delete it (`rm -rf /var/cache/pagespeed-optimizer/v1`). The disk needs
  room for both volumes until you do. The package prints a notice
  whenever it finds `v1/` during an install or upgrade.
- **Repoint web-server configuration that names `v1`.** A default package
  install needs nothing. If your web-server configuration names the cache
  path explicitly (`pagespeed_cache_path` in nginx,
  `ModPagespeedDaemonVolumePath` in Apache), change `.../v1/cache` to
  `.../v2/cache` and restart the web server. A serving module that checks
  the generation refuses to attach when the generation published beside the
  configured path differs from its own, and says so in the error log; it
  does not go looking for another directory.
- **The nginx module checks the generation for you.** At start, on every
  reload, and whenever the optimizer rewrites `pagespeed-shared.conf`, it
  compares the optimizer's `cache_dir_generation` with its own. On a
  mismatch it keeps serving, with its cache turned off (requests go straight
  to the origin; nothing is read, stored or sent to the optimizer), and logs
  one error:

  ```
  [error] ... pagespeed: cache-directory generation mismatch: this module is
  generation 2, the optimizer (/var/cache/pagespeed-optimizer/v1/pagespeed-shared.conf)
  is generation 1 -- the optimizer is older than this module. ...
  ```

  Upgrade both packages to the same release, or run both containers from the
  same image tag. The cache comes back on by itself, without a reload, once
  the optimizer publishes the matching generation (the error log then says
  "now matches"). The variable `$pagespeed_cache_generation` reads `match`,
  `mismatch` or `unknown` (an optimizer that publishes no generation; the
  module then warns once and uses the cache as before), and
  `$pagespeed_cache_generation_module` / `$pagespeed_cache_generation_optimizer`
  hold the two numbers. Put them in a `log_format`, or in a status location
  restricted to your monitoring hosts. A module older than this release does
  not check, so "upgrade together" (the last point in this list) still
  applies.
- **Containers: the old cache file stays in the volume.** The container
  images do not use a `v<N>` directory: the worker always opens
  `--cache-path /data/cache.vol`, and the cache library names the real file
  after its format, so the new build creates `/data/cache-8-<hex>.vol` next
  to 2.1.0's `/data/cache-7-<hex>.vol` in the same volume. Nothing removes
  the old one (it is `CACHE_SIZE` bytes, 1 GiB by default), and on a named
  volume (the compose files) it stays until you delete it. Once the new
  build is serving and no 2.1.0 container still mounts the volume, delete it:
  `docker compose exec worker sh -c 'rm -f /data/cache-7-*.vol /data/cache.vol-7-*.small'`.
  Do not delete it while an old build still runs, and never delete a
  `cache-8-*` file. The Helm chart's cache is a per-pod `emptyDir`, which an
  upgrade replaces, so nothing is left behind there.
- **Compose: the worker and nginx now share one PID namespace.** This
  release's cache library is safe across PID namespaces, but older builds
  recover the cross-process write lock from a holder whose PID looks gone,
  and a live holder in another container does. While an old and a new build
  share a named volume during the upgrade, two writes could then overlap,
  so the compose files put every container that mounts the volume in the PID
  namespace of a `pidns` service. It runs the digest-pinned Kubernetes pause
  image, so an image upgrade never recreates it. Restarting the worker or
  nginx leaves the other running; stopping or recreating `pidns` stops both
  (`docker compose up -d` starts all three again). If you run your own
  compose file, do the same, with an anchor image that does not change with
  the release: `pid: "service:<anchor>"` on every container that mounts the
  volume. The Helm chart (0.3.5) sets `shareProcessNamespace: true`; there
  the cache is per pod, so builds never mix, and the setting keeps the same
  layout and makes the pause container PID 1. Keep the cache on a local
  filesystem (NFS and SMB are untested).
- **The optimizer's systemd unit admits one more system call, `mincore`.**
  The cache library now checks whether data is already in memory before it
  asks the kernel to read ahead, and `mincore` is outside
  `@system-service`. The unit's allow-list is now
  `SystemCallFilter=@system-service mincore`, and `SystemCallLog=` no longer
  logs it. If you replaced the unit's filter with your own, add `mincore`, or
  the optimizer is killed with `SIGSYS` when it reopens its cache.
- **Upgrade the optimizer and the serving module together.** They must link
  the same cache library. An older module next to a newer optimizer (or the
  reverse) opens a different volume file instead of sharing one. The
  packages depend on each other at the exact same version, which prevents
  this for package installs; container deployments get it from the single
  image tag the Helm chart and the compose files use for both images.

## Upgrading from 1.x (1.15)

Upgrading a mod_pagespeed 1.x (1.15) install to 2.1.0 is an in-place package
upgrade, boring by design and proven by in-place upgrade rehearsals:

- **Upgrade the package.** `apt upgrade mod-pagespeed` (or the yum/dnf
  equivalent). The `mod-pagespeed` package depends on the
  `pagespeed-optimizer` package at the exact same version, so the daemon is
  pulled in automatically. 2.1.0 runs the 1.x module lineage on top of a
  separate optimization daemon; your existing module configuration carries
  over.
- **Start order is handled.** At boot, the packaged systemd unit orders the
  daemon ahead of the web server and counts it as started only once its
  notify socket exists. During the upgrade itself, when you start things by
  hand rather than booting, start (or restart) `pagespeed-optimizer` first,
  then the web server.
- **The cache is not purged — it is side-stepped.** 1.15 already stores its
  cache in a Cyclone volume (`cyclone.dat` under the file-cache path), and
  the 2.1.0 daemon opens its own new volume — format major 8, with the format
  carried in the filename, under its own cache directory
  (`/var/cache/pagespeed-optimizer/v2`) — and never touches the old file.
  There is no purge storm: the old file stays where it is, and a rollback to
  1.x reopens it still warm unless you deliberately deleted it. Nothing
  removes the old file for you — delete it yourself once you are confident
  you will not roll back.
- **First requests are cold.** The new volume starts empty: the first
  requests after the upgrade are cache misses and the cache refills as
  traffic arrives. Nothing is lost that cannot be rebuilt, but hit rate dips
  and origin fetches rise until the cache is warm — pick a quiet hour if your
  origin is sensitive to that.

The free-space check and the manual start order written for 2.0 installs in
the next section apply to the 1.x upgrade exactly the same way, with one
difference: the 2.0→2.1.0 files share one cache directory, while the 1.15
volume and the 2.1.0 volume live in different directories (the 1.x file-cache
path versus `/var/cache/pagespeed-optimizer/v2`). Both files exist during
the upgrade either way, so the disk needs room for the second volume.

## Upgrading from 2.0

mod_pagespeed is licensed under the Apache License 2.0. Coming from a 2.0 install:

- **The cache starts empty.** 2.1.0 uses a new on-disk cache format. The 2.1.0
  binary opens a new cache file instead of converting the 2.0 one, so the
  first requests after the upgrade are misses and the cache refills as
  traffic arrives. Nothing needs configuring and nothing is lost that cannot
  be rebuilt, but hit rate dips and origin fetches and optimization work rise
  until it is warm -- pick a quiet hour if your origin is sensitive to that.

  **Check free space first.** Both files exist during the upgrade, so the
  cache directory needs free space at least equal to your configured cache
  size on top of what the 2.0 volume already occupies. With that room, leave
  the 2.0 file in place -- it is what makes a rollback warm -- and delete it
  once you are confident you will not roll back. Without it, **stop the
  daemon, delete the 2.0 cache file, then start 2.1.0**: the new volume is
  extended to its full configured size during startup, ahead of any cleanup,
  so starting first on a directory sized for one volume can exhaust the disk.
  Allocation is sparse, so this would not fail at the upgrade -- it would fail
  hours later, mid-traffic, as the cache warms. A rollback to 2.0 reopens the
  2.0 file, still warm, unless you deleted it: 2.1.0 leaves format-stamped
  volumes of other formats alone, so neither a cache purge nor the module's
  automatic open-failure recovery takes the rollback away. Nothing removes the 2.0 file
  for you -- deleting it is your step, once you are confident you will not
  roll back.

- **Start the optimizer before the web server.** The volume filename carries
  the cache format, so the 2.1.0 daemon opens a new file. Until it has started
  and created that file, the only volume in the directory is the 2.0 one, and
  a web server that starts in that window creates the new file itself instead
  of attaching to the daemon's. The serving module will not paper that over:
  it **refuses to start**, so the web server does not come up at all rather
  than quietly serving unoptimized. The order is: upgrade and start the
  optimizer, let it create its volume, then start the web server. A file left
  behind by a refused start can be removed once the optimizer is up.

  **At boot the packaged unit handles this for you.** The optimizer is ordered
  ahead of `apache2.service`, `httpd.service` and `nginx.service`, and counts
  as started only once its notify socket exists, so the web server is not
  released until the volume is there. The nginx half of that ordering is new
  in 2.1.0, so it applies from a packaged 2.1.0 install onward. The manual order
  above is what applies during the upgrade itself, when you start things by
  hand rather than booting. If your web server runs under some other unit
  name, order it yourself with a `Before=` drop-in on
  `pagespeed-optimizer.service`.
- **Nothing you must do.** An unchanged 2.0 configuration starts. A retired
  2.0 setting is accepted and ignored, with a startup warning that names it
  so it can be removed.
- **`pagespeed-shared.conf`** keeps schema version 1. Keys that 2.1.0 no longer
  writes read as missing to a 2.0-era reader, which keeps its defaults, so a
  mixed-version window during a package upgrade is harmless.
  `agent_optimize_entitled` keeps its historical name and mirrors the
  `--agent-optimize` flag.
- **HTTP API.** `/v1/license/*` no longer exists (404), and `/v1/health` has
  no `license` object and no `checks.license` entry. Monitoring that keyed on
  `checks.license.pass` should key on `ready` / `status` instead. The
  `x-pagespeed-warn` response header is never emitted.
- **agent_optimize, `/llms.txt`, browser analysis** are enabled by their
  operator flags alone.
- **Outbound traffic.** The daemon makes no request of its own accord. The
  only egress is what you configure: origin fetches and, if enabled, the Web
  Bot Auth / RSL-CAP key directories.

## 2.1.0 host packages: the system-call filter is enforced by default

This section applies to the `pagespeed-optimizer` deb/rpm host packages only
(containers have no systemd; the container runtime's seccomp profile is the
filter there, unchanged). It covers an upgrade from a 2.0 host package, from
an earlier 2.1.0 release candidate, or from 1.x -- for which the filter is
simply new.

**What changes.** The daemon's systemd unit now carries an enforcing
system-call allow-list: `SystemCallFilter=@system-service` minus
`@privileged @resources`, plus `RestrictNamespaces=yes`. On the first call
outside that set the kernel terminates the daemon with `SIGSYS` (exit status
31). Earlier 2.1.0 release candidates shipped the same unit log-only and offered
enforcement as an opt-in drop-in (`10-syscall-filter-enforce.conf.example`);
that file is retired, and if you installed it into
`/etc/systemd/system/pagespeed-optimizer.service.d/` it is now redundant --
remove it (`systemctl daemon-reload && systemctl restart pagespeed-optimizer`)
so `systemctl cat` shows one copy of the profile. 2.0 host packages shipped
no syscall filter at all. The package restarts the service on install, so an
upgraded host enforces from that restart; the unit keeps
`SystemCallLog=~@system-service`, so every call outside the group is still
written to the audit log as before.

**Why it is safe to ship enforcing.** The allow-list was decided from a
syscall census of the daemon across Debian 12, Ubuntu 24.04 and AlmaLinux 9
(every measured posture -- cold start, cache create and peer open, the sockets,
the management API with real requests, purge, reload, shutdown, and the daemon
under its own unit as the unprivileged `pagespeed` user -- is inside the set),
and the package's own verification runs the real daemon and a real headless
Chrome under the enforcing profile on every build and asserts no kill.

**Browser analysis.** Headless Chrome's own sandbox makes a handful of calls
the daemon's list does not admit. The package ships that re-admission list
(`20-browser-analysis.conf`) **installed and active** as a vendor drop-in next
to the unit -- `/usr/lib/systemd/system/pagespeed-optimizer.service.d/`
(`/lib/systemd/system/...` on Debian-family hosts) -- so browser analysis
works under the enforcing unit with no extra step. If you had installed the
earlier example copy of that file under `/etc/systemd/system/`, it now
shadows the vendor copy with identical content; remove it so the packaged
version is the one in force on future upgrades. The file only adds to the
unit's allow-list, so the earlier warning never to install it without its
companion no longer applies.

A host that does not run browser analysis can take the narrower daemon-only
profile back by masking the vendor file with an empty file of the same name
under `/etc` (systemd lets a same-named drop-in in `/etc` replace the vendor
one, and an empty drop-in contributes nothing):

```bash
sudo install -D -m 0644 /dev/null \
  /etc/systemd/system/pagespeed-optimizer.service.d/20-browser-analysis.conf
sudo systemctl daemon-reload && sudo systemctl restart pagespeed-optimizer
```

With it masked the daemon starts and serves; browser analysis refuses to
initialize, reports `browser_sandbox: "unavailable"` and names the restriction
in the journal. It never falls back to an unsandboxed browser. Masking is a
tightening, not the opt-out.

**How to opt out.** The documented way back to the log-only posture is a
drop-in of your own, copied from the shipped example:

```bash
sudo install -D -m 0644 \
  /usr/share/doc/pagespeed-optimizer/90-syscall-filter-off.conf.example \
  /etc/systemd/system/pagespeed-optimizer.service.d/90-syscall-filter-off.conf
sudo systemctl daemon-reload && sudo systemctl restart pagespeed-optimizer
```

It resets `SystemCallFilter=` and `RestrictNamespaces=` (an empty assignment
clears the directive), and it survives `apt upgrade` and `dnf upgrade`: the
package writes its vendor files under `/usr/lib/systemd` (`/lib/systemd` on
Debian-family hosts) and never under `/etc/systemd`, so no upgrade can touch,
prompt on, or re-create anything there. The `90-` prefix must stay -- systemd
applies drop-ins in lexical order of their names regardless of directory, and
the reset has to sort after every vendor drop-in to win. Do not edit the unit
under `/usr/lib` or `/lib` instead; an upgrade overwrites it. Remove the file
and repeat the two commands to go back to enforcing.

**How to confirm which posture you are in.** Ask systemd, not the daemon:

```bash
systemctl show -p SystemCallFilter pagespeed-optimizer
# enforcing -> a long allow-list (no leading '~')
# opted out -> SystemCallFilter=~   (an empty deny-list: nothing is filtered)
systemd-analyze security pagespeed-optimizer
```

The `syscall_filter` field on `GET /v1/health` reports `"filtered"` in both
postures, because `SystemCallLog=` is itself a seccomp filter; it tells you a
filter is attached, not which one.

**How to diagnose a SIGSYS.** Three surfaces, and which one you see depends on
who was killed and whether `auditd` is running:

1. `systemctl status pagespeed-optimizer` -- `code=dumped, status=31/SYS` means
   the daemon itself was killed (no dump is written; `LimitCORE=0`).
2. `journalctl -u pagespeed-optimizer` -- `Chrome exited (status=0, signal=31)`
   means the browser was killed; the daemon keeps running and relaunches it
   after a back-off.
3. The kernel's audit record, type `1326`, names the call by **number**
   (`syscall=N`):

   ```bash
   systemctl is-active auditd
   # active   -> ausearch -m SECCOMP -ts recent -i     (-i prints the name)
   # inactive -> journalctl -k | grep 'type=1326'       (the kernel ring; audit records
   #             carry no unit field, so `journalctl -u` cannot select them)
   # a KILL reads sig=31 (code=0x8...); lines with sig=0 code=0x7ffc0000 are the
   # log filter's records, not kills
   ausyscall <N>          # number -> name, from the audit package
   ```

   When `auditd` runs (the default on RHEL/AlmaLinux/Rocky) it owns the audit
   socket and these records never reach the journal -- an empty journal there
   means nothing was read, not that nothing was killed.

Report the syscall name (with the Chrome version if the browser was the
victim). A name missing from the browser profile is a defect in the shipped
file and is fixed there; opting out is the bridge, not the destination.

## Appendix: the 1.x → 2.0 architecture change (nginx line)

mod_pagespeed 2.0 was the nginx-centric rewrite: it replaced the 1.x
in-process filter pipeline with the three-component architecture (serving
module + optimization daemon + shared cache) that mod_pagespeed 2 converges on. If
you upgrade a 1.x install to 2.1.0 through the packages, the change described
below is absorbed by the packages themselves -- you do not migrate anything
by hand. This appendix remains for readers who ran the 2.0 line, who run
ngx_pagespeed and are moving to the mod_pagespeed 2 nginx module, or who want the detail
of what changed under the hood.

### Architecture differences

mod_pagespeed 1.x processed responses synchronously inside the web server. A
RewriteDriver instance managed a pipeline of 60+ filters that ran during the request,
each modifying the HTML response in sequence. Filters had carefully managed
inter-dependencies, and the nginx port (`ngx_pagespeed`) adapted Apache's output
filter chain to nginx's event-driven model.

ModPageSpeed 2.0 replaces this with three cooperating components:

1. **Nginx interceptor** -- a thin C++ nginx module that classifies requests, serves
   cached variants via zero-copy mmap reads, and proxies cache misses to the origin.
2. **Factory worker** -- a standalone C++ process (libuv event loop) that receives
   notifications from nginx, reads original content from cache, optimizes it, and writes
   variant alternates back to cache.
3. **Cyclone cache** -- a memory-mapped disk cache shared between nginx and worker.
   Each URL can have multiple alternates keyed by a 32-bit capability bitmask encoding
   image format, viewport class, pixel density, Save-Data, and transfer encoding.

The key behavioral difference: in 1.x, every response was transformed in-flight, adding
latency to every request. In 2.0, the first request gets the original response
(`X-PageSpeed: MISS`). The worker optimizes it asynchronously, and subsequent requests
get the optimized variant (`X-PageSpeed: HIT`) with zero processing overhead.

### What was removed

**RewriteDriver and the filter pipeline.** The entire 2000+ LOC orchestration layer is
gone. There is no filter ordering, no filter dependencies, no `RewriteContext`. The
worker dispatches on content type (HTML, CSS, JS, Image) and each path runs
independently.

**HTTP/1.1 workaround filters.** These filters solved problems that HTTP/2 makes
irrelevant:

| Removed filter | Reason |
|----------------|--------|
| `combine_css` | HTTP/2 multiplexing eliminates round-trip cost of multiple files |
| `combine_javascript` | Same as above |
| `sprite_images` | HTTP/2 and modern image formats make spriting counterproductive |
| `inline_css` / `inline_javascript` | Replaced by targeted critical CSS injection |
| `lazyload_images` (JS-based) | Replaced by native `loading="lazy"` attribute injection |
| `defer_javascript` | Standard `defer` and `async` attributes are the correct solution |

**Apache httpd integration.** ModPageSpeed 2.0 does not run inside Apache. If you use
Apache as your application server, deploy the nginx interceptor + worker in front of it
as a reverse proxy. (This restriction was specific to the 2.0 line: since 2.1.0 the Apache
module is first-class again -- see "What about Apache?" in the FAQ below.)

**Per-filter configuration.** `ModPagespeedEnableFilters` and `ModPagespeedDisableFilters`
directives do not exist. Optimizations are enabled by default and controlled at the
content-type level.

### What is new

**Variant-aware caching.** A single URL can have multiple cached variants: WebP for
Chrome, AVIF for supporting browsers, optimized JPEG as fallback, each at mobile/tablet/
desktop viewport sizes, at 1x and 2x pixel densities, with and without Save-Data. The
cache stores all variants as alternates and selects the best match per request.

**Image format negotiation.** The nginx interceptor reads the `Accept` header and serves
the best available format. No URL rewriting, no `.webp` extensions, no JavaScript
detection. The original URL serves different bytes based on client capabilities.

**SVG auto-vectorization.** Simple images (illustrations, screenshots, icons) are
automatically converted to resolution-independent SVG using VTracer. A single SVG
variant replaces the entire viewport/density matrix for qualifying images.

**Learned quality prediction.** Machine learning models predict the optimal encoder
quality setting for each image to hit a target SSIMULACRA2 perceptual quality score.
This eliminates the guesswork of fixed quality settings.

**Browser analysis.** A headless Chrome instance renders pages to extract critical CSS
and identify the LCP element. This produces more accurate results than static HTML
analysis alone.

**Web console.** A built-in SvelteKit dashboard at `/console/` provides cache
inspection, real-time stats, image comparison, and configuration editing.

**ASP.NET Core middleware.** `WeAmp.PageSpeed.AspNetCore` brings HTML optimization to
.NET applications without nginx, using the same `libpagespeed` library via P/Invoke.

### Configuration migration

#### Worker flags (replace filter directives)

| 1.x Directive | 2.0 Worker Flag | Notes |
|---------------|-----------------|-------|
| `ModPagespeedEnableFilters` | (automatic) | All optimizations enabled by default |
| `ModPagespeedDisableFilters rewrite_images` | `--disable-image` | Disables all image optimization |
| `ModPagespeedDisableFilters rewrite_css` | `--disable-css` | Disables CSS minification |
| `ModPagespeedDisableFilters rewrite_javascript` | `--disable-js` | Disables JS minification |
| `ModPagespeedDisableFilters prioritize_critical_css` | `--disable-html` | Disables HTML processing |
| `ModPagespeedCacheSizeMb 200` | `--cache-size 209715200` | Size in bytes |
| `ModPagespeedFileCachePath /var/cache` | `--cache-path /shared/cache.vol` | Single file, not directory |

#### Nginx directives

| 1.x Directive | 2.0 Directive |
|---------------|---------------|
| `pagespeed on;` | `pagespeed on;` |
| `pagespeed FileCachePath ...;` | `pagespeed_cache_path /shared/cache.vol;` |
| `pagespeed Disallow /pattern;` | `pagespeed_disallow /pattern;` |

Settings like the worker socket path and HTML processing toggle are
now configured on the worker side (via CLI flags or `PATCH /v1/config`) and shared
with nginx automatically via `pagespeed-shared.conf`, which the worker writes next
to the cache file.

#### Image quality

| 1.x Directive | 2.0 Equivalent |
|---------------|----------------|
| `ModPagespeedJpegRecompressQuality 85` | Learned quality prediction (automatic) |
| `ModPagespeedWebpRecompressQuality 80` | Learned quality prediction (automatic) |
| `ModPagespeedImageMaxRewritesAtOnce 8` | Worker handles concurrency internally |
| `ModPagespeedImageInlineMaxBytes 3072` | Not applicable (no inline images) |

To disable learned quality and use fixed quality settings, pass `--no-learned-quality`
to the worker. Per-format overrides: `--no-learned-quality-jpeg`,
`--no-learned-quality-webp`, `--no-learned-quality-avif`.

### Cache format changes

ModPageSpeed 2.0 uses Cyclone, a purpose-built variant-aware disk cache. It is
incompatible with the 1.x file-based cache. There is no migration path -- the cache
starts cold and warms as traffic flows through. (This describes the classic 1.x
directory cache as the 2.0 line found it. Later 1.15 releases already store
the cache in a Cyclone volume -- `cyclone.dat` under the file-cache path. The
2.1.0 daemon opens its own new volume -- format major 8, with the format
carried in the filename -- and never touches the old file, which is what
makes the side-stepping 1.x → 2.1.0 cache upgrade at the top of this document
work.)

Key differences:

- **Single file vs. directory tree.** Cyclone stores everything in one memory-mapped
  volume file instead of a directory hierarchy.
- **Multiple alternates per URL.** Each URL can have many cached variants (different
  formats, viewports, encodings). In 1.x, each rewritten resource was a separate cache
  entry.
- **Cross-process sharing.** Both nginx and worker mmap the same cache file. Writes from
  either process are immediately visible to the other (with `enable_mmap_directory = true`).
- **Metadata per alternate.** Each variant carries its own metadata: capability mask,
  content type, origin cache-control fields, SSIMULACRA2 score, content class, ETag,
  and Last-Modified.

#### Cached HTML written before an upgrade

Optimized HTML is cached as-is and is re-served without being reprocessed, so an
upgrade does not retroactively change pages already in the cache. Cache entries
carry no build identity, and nothing re-enters the optimizer on a cache hit — a
page keeps whatever the worker decided when it was written, until it is evicted,
purged, or its origin content changes.

This matters when an upgrade tightens what a page is allowed to do. In
particular, HTML written by a worker that predates the confirmed-above-the-fold
condition on stylesheet deferral (see the CHANGELOG entry) continues to serve
the deferral it was written with. The new condition applies to pages optimized
after the upgrade, not to pages already stored.

If you want the new behaviour to apply immediately across a site rather than as
the cache turns over, purge the cached HTML (or reset the cache volume) after
upgrading. Otherwise no action is needed; entries converge as they expire.

### Deployment topology changes

#### mod_pagespeed 1.x

```
Client --> Apache/Nginx (with mod_pagespeed built-in)
               |
               v
           Origin app
```

Single process. The web server handled both proxying and optimization.

#### ModPageSpeed 2.0

```
Client --> Nginx (interceptor module)
               |
               +-- cache HIT --> zero-copy mmap response
               |
               +-- cache MISS --> proxy to origin
                       |
                       +-- notify worker (async)
                               |
                               v
                        Factory worker --> writes variant to cache
```

Three components, typically two containers in Docker:

1. **nginx** container: runs nginx with the interceptor module
2. **worker** container: runs `factory_worker` with optional Chromium for browser analysis
3. **Shared volume**: both containers mount the same volume for cache file and Unix socket

Docker Compose example:

```yaml
services:
  worker:
    image: ghcr.io/we-amp/pagespeed-worker:2.0.0-beta.1
    command: >
      /usr/bin/factory_worker
      --socket /shared/pagespeed.sock
      --cache-path /shared/cache.vol
      --cache-size 104857600
    volumes:
      - shared:/shared

  nginx:
    image: ghcr.io/we-amp/pagespeed-nginx:2.0.0-beta.1
    volumes:
      - shared:/shared
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
    ports:
      - "80:80"

volumes:
  shared:
```

For Kubernetes deployments, install the official Helm chart from
`https://modpagespeed.com/charts` (`helm repo add weamp
https://modpagespeed.com/charts`). See the
[Helm deployment guide](https://modpagespeed.com/docs/helm-deployment/).

### Step-by-step migration

#### 1. Save your current configuration

```bash
# Apache
cp /etc/apache2/mods-enabled/pagespeed.conf ~/pagespeed-1x-backup.conf

# Nginx (ngx_pagespeed)
cp /etc/nginx/nginx.conf ~/nginx-1x-backup.conf
```

#### 2. Deploy ModPageSpeed 2.0

Start with Docker Compose for testing. Once validated, move to your production
deployment method (Docker, Kubernetes with Helm, or bare metal with systemd).

#### 3. Configure nginx

```nginx
load_module /usr/lib/nginx/modules/ngx_pagespeed_module.so;

server {
    listen 80;
    server_name example.com;

    pagespeed on;
    pagespeed_cache_path /shared/cache.vol;
    # Worker socket path and HTML toggle are read
    # automatically from pagespeed-shared.conf (written by the worker)

    # Migrate your Disallow patterns from 1.x
    pagespeed_disallow /api/;
    pagespeed_disallow /admin/;

    location / {
        proxy_pass http://your-backend:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

#### 4. Validate

```bash
# First request: should return X-PageSpeed: MISS
curl -I http://localhost/

# Wait for worker to process, then:
curl -I http://localhost/
# Should return X-PageSpeed: HIT

# Verify image format negotiation
curl -H "Accept: image/avif,image/webp,*/*" -o /dev/null -w "%{content_type}" \
  http://localhost/image.jpg
# Expected: image/avif

# Check worker stats
curl http://localhost:9880/v1/stats
```

#### 5. Gradual rollout

Route a fraction of traffic through 2.0 first. Compare Core Web Vitals in your RUM
data against the 1.x baseline. The web console at `/console/` provides real-time
visibility into cache hit rates and optimization progress.

## Frequently asked questions

**Can I run 1.x and mod_pagespeed 2 side by side?**
Yes. They share no state -- mod_pagespeed 2 even keeps its cache in a separate,
format-fingerprinted volume file. Run them on different servers or, on nginx,
different server blocks.

**Will my 1.x cache carry over?**
No -- and it is not destroyed either. 1.15 stores its cache in a Cyclone
volume (`cyclone.dat` under the file-cache path); the 2.1.0 daemon opens its
own new volume -- format major 8, with the format carried in the filename --
under its own cache directory and never touches the old file, which stays
warm for a rollback unless you delete it. The new cache starts cold and
rewarms as traffic flows through.

**Do I need to change my application code?**
No. All optimization is transparent at the reverse-proxy level.

**What about Apache?**
First-class since 2.1.0: the `mod-pagespeed` package is the 1.x module lineage,
served by the same pagespeed-optimizer daemon that backs the nginx module.
(The 2.0 line was nginx-only; that restriction is gone.)

**What if I need a filter that was removed?**
The removed filters (combining, spriting, JS-based lazy loading) were dropped
by the 2.0 nginx line because they addressed HTTP/1.1 limitations. On Apache,
2.1.0 is the 1.x module lineage, so its configuration surface carries over. On
the nginx line, if you still need them, keep running 1.x alongside during the
transition. For most deployments on HTTP/2, the mod_pagespeed optimizations produce
better results.
