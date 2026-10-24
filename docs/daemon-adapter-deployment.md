# Deploying alongside the optimizer worker

This page covers the two Apache directives that point the module at the
optimizer worker, the layout they are meant to be used in, and what happens
when the worker is missing. The nginx module takes the same two directives
(`pagespeed DaemonSocketPath` and `pagespeed DaemonVolumePath` in a `server`
block); its set-up is in `docs/install-nginx.md`, "Using the optimizer worker".
IIS is covered in `docs/daemon-adapter-iis.md`.

Both halves are wired now. With the directives set, a server records origin
responses into the worker's cache, asks the worker to optimize them, and
answers later in-place-eligible requests from that cache when there is
something there to answer them with.

The Apache module packages install `pagespeed_daemon.conf`, which sets both
directives to the optimizer package's default paths. With them set and the
worker usable, in-place optimization runs over the worker's cache: the module
records eligible responses, the worker optimizes them, and the module serves
the optimized variant, or the stored original when no variant fits the
client. With both unset, the module's own (classic) in-place path is used,
exactly as before.

One behaviour is worth knowing before it is observed. A response the module
cannot faithfully reproduce from a cache entry — because the origin sent a
header the entry does not carry — is recorded and marked as such, and later
requests for it are served by the ordinary path rather than from the cache. The
substrate declines rather than serving a response with headers missing. What
counts as reproducible is listed under "Which responses this substrate will
serve" below.

## The directives

| Directive | Argument | Scope |
|---|---|---|
| `ModPagespeedDaemonSocketPath` | Path of the worker's notification socket | server / virtual host |
| `ModPagespeedDaemonVolumePath` | Path of the worker's shared cache volume | server / virtual host |
| `ModPagespeedDaemonServeStoredEncodings` | `on` or `off` (default `off`): serve the optimizer's stored gzip and brotli copies — see "Serving the stored compressed copies" | server / virtual host |

The two path directives are unset in the module's own defaults, which is the
classic configuration: in-place optimization behaves exactly as it always has
and nothing on this page applies. The Apache module packages set both in
`pagespeed_daemon.conf` (see above); the nginx module package sets neither.
Set them together — one without the other is a configuration mistake and the
module says so at startup.

There is no directive that switches between the classic and worker in-place
substrates at run time, and there will not be one. If the worker substrate
misbehaves on a host, the mitigation is to reinstall the previous module and
worker packages together. The module and the worker are a matched pair; they
are installed, upgraded and rolled back as one.

## Group membership and the versioned cache directory (2.1)

Since the worker's privilege drop (2.1), the optimizer worker runs as the
unprivileged `pagespeed` user and its artifacts are group-scoped: the cache
volume, the notify socket and the shared configuration are mode `0660`/`0640`
owned by `pagespeed:pagespeed`, under the versioned cold-start cache directory
(`/var/cache/pagespeed-optimizer/v3/`) and `/run/pagespeed-optimizer/`. The
module reaches all three as a **group member**, never as their owner — it
still never creates the volume, and it still refuses to start if an open
authors a stray second volume file.

The module packages perform the group join in their postinst (`www-data` on
deb; `apache` and/or `nginx` on rpm), guarded and idempotent, and a no-op when
the optimizer package — which creates the group — is not installed. A
web-server restart is required for the membership to take effect. An exotic
web user gets the one-liner version: `usermod -a -G pagespeed <user>`.

Two consequences are visible at startup:

* **Permission denied is told apart from absent.** A module that cannot read
  the worker's cache directory, shared configuration or socket *because of
  permissions* logs a line naming the likely cause — the web-server user not
  being in the `pagespeed` group — rather than the "start the worker" line
  the absent case gets. The same distinction is made in the request-serving
  process, where the volume is really opened.
* **The cache-directory generation is checked.** The worker publishes
  `cache_dir_generation` in its shared configuration; this module is built
  for generation 3. A worker publishing a different generation is a loud
  handshake failure — in-place optimization off, nothing opened, never a
  silent split-brain across two layouts that share nothing. A worker
  publishing no generation at all (a package from before the privilege drop)
  is the legacy layout: tolerated, announced once at startup, and used with
  the configured paths as-is.

## Reference deployment: separate paths

**Keep `ModPagespeedDaemonVolumePath` on a different path from
`ModPagespeedFileCachePath`.** This is the recommended layout, and the one
support answers assume:

```apache
<IfModule pagespeed_module>
  ModPagespeed on

  # The module's own cache. Unchanged, and on its own path.
  ModPagespeedFileCachePath      /var/cache/mod_pagespeed/
  ModPagespeedFileCacheSizeKb    1048576

  # The worker. A DIFFERENT directory, and its own socket.
  ModPagespeedDaemonSocketPath   /var/run/pagespeed/daemon.sock
  ModPagespeedDaemonVolumePath   /var/cache/pagespeed-daemon/volume
</IfModule>
```

The two caches are different things with different owners. The module owns the
first; the worker owns the second and this module is one of its readers.
Pointing both at one directory makes a single volume file's health a
shared-fate dependency of both: a volume that will not open, or that a
crashed process left locked, takes out the classic cache at the same time,
and one cache's size accounting starts evicting the other's entries.

Co-locating them is not blocked, but it is a deviation, and the split-cache
warning below applies to it with extra force.

## Cache sizing is inherited, not configured, and a split cache stops the server

The worker derives the FILENAME of its cache volume from the volume's
geometry. That is deliberate: two peers with different layouts open different
files during an overlapping upgrade instead of corrupting one another's data.

The consequence is that a sizing disagreement is invisible. A process that
opens the worker's directory with a different size does not collide and does
not fail — it creates and uses a different file, shares nothing, and runs a
permanently cold cache. Every request works. Nothing is ever optimized. There
is no error anywhere.

So the module does not have a size setting, and does not carry one internally.
It **inherits** the size from the worker, which publishes the size it actually
opened its volume with. There is nothing for an operator to keep in sync.

What the module then checks is whether inheriting that size actually landed on
the worker's volume:

* **It never opens the volume while the size is unknown**, and it never
  creates one. If the worker is not running, or is an older release that does
  not publish its size, in-place optimization is simply off (see below).
* **If the directory already holds more than one volume file**, the server
  starts and logs a warning naming them. This is the normal state after you
  change the worker's cache size: the filename encodes the size, and the worker
  leaves the file it stopped using in place. The size the worker publishes
  names exactly one of them, and the module verifies it attached to that one,
  so the extras are wasted disk rather than a split cache. They can be deleted
  while the worker is stopped.
* **If opening at the worker's published size creates a second volume file**,
  the server **refuses to start**, names the file its open created, and says
  what became of it: removed, still there, not confirmed gone, or left in
  place because another user or process holds it. There are two ways to reach
  the refusal, and the message distinguishes them.

  The common one is **start order during an upgrade to a release that changes
  the on-disk cache format**. The worker's volume filename carries the format,
  so a worker on a new format opens a new file; until it has restarted and
  created it, the only volume in the directory is the previous format's, and a
  web server that starts in that window creates the new file itself instead of
  attaching to the worker's. **Upgrade and start or restart the optimizer
  worker, let it create its volume, then start the web server.** The refused
  start has usually removed the file it created; when the message says it
  could not, or could not confirm it is gone, remove that file by hand before
  the next start —
  unless the message says the file is owned by another user or held open by
  another process: that file is most likely the optimizer's own volume. Leave
  it in place; once the optimizer runs on it, the next start attaches to it.

  **At boot, whether you need to do anything depends on your web server.**
  The optimizer's service counts as started only once its notify socket
  exists, so a web server ordered after it is not released until the volume is
  there. It is ordered ahead of Apache (`apache2.service`, `httpd.service`),
  and, from a packaged 2.1 optimizer install onward, ahead of `nginx.service`
  as well. Two consequences worth being precise about: a host still running a
  2.0-era optimizer package has the Apache-only unit, so an **nginx**
  deployment that upgrades this module before the optimizer is still raced at
  every boot, not just once at upgrade; and a web server under any other unit
  name is not ordered at all, so add an ordering drop-in for it
  (`Before=<your unit>` on the optimizer service, or `After=` on yours). The
  ordering is ordering only — the worker is not required by the web server, so
  a web server still starts when the worker is absent, and the worker still
  starts when no web server is installed.

  The other is a genuine disagreement: the published size does not match the
  volume on disk. Start or restart the optimizer worker, let it create its
  volume, then start the web server; if it still refuses after that, install a
  module and worker package pair that agree.

That last one is the only condition that stops the server. It earns it because
it is the only one an operator cannot otherwise see: everything else announces
itself in the log and the server starts. In particular, a routine cache resize
must never prevent a start.

### The cache directory needs room for two volumes across a format change

A release that changes the on-disk cache format leaves the old volume file in
place and creates a new one beside it. Both count against the same filesystem,
and the new one grows to the worker's **full configured cache size** as it
fills — the worker extends it at startup, before any reclamation runs. So the
directory needs free space of at least the worker's cache size on top of what
the existing volume already occupies.

**If the directory is sized for exactly one volume, delete the old file before
starting the new worker, not after.** This is the opposite of the usual advice
to tidy up afterwards, and the reason is that on Linux the new volume is
allocated sparsely: too little space does not fail the start. It fails hours
later, mid-traffic, as the cache warms and the sparse file becomes real. On
Windows the space is claimed at creation, so the failure is immediate instead.
Neither the module nor the worker checks free space, so this is an operator
check.

The procedure, with the worker stopped:

1. List the directory. Volume files are named `<stem>-<format number>-<hash>`,
   where `<stem>` is the last path element of `ModPagespeedDaemonVolumePath`
   (`cache` for the packaged default, which has no file extension).
2. The file to remove is the one whose **format number is lower**. A file with
   a different hash and the same format number is a leftover from an earlier
   cache size, not from an earlier format; those are safe to remove too.
3. Remove it, then start the worker, then start the web server.

Removing the old file gives up a warm rollback — an earlier worker would have
reopened it untouched. Where there is room for both, keep it until you are
confident you will not roll back, and reclaim it then. A running worker holds
its volume open, so deleting a file while it runs does not return the space
until it restarts.

The module never enables a per-process memory tier over the worker's volume,
regardless of the worker's own defaults. Such a tier is keyed without any
check against the on-disk entry, so a process holding it would keep serving
bytes the worker had already replaced.

## What gets recorded, and what does not

With both directives set and the worker usable, an in-place-eligible response
is kept in the worker's cache as the ORIGINAL — the bytes the origin sent,
before anything touched them — with the origin's `Cache-Control` state and its
validators, and the worker is told the entry is there. The module writes that
one class of entry and no other: the optimized variants are the worker's to
write, and each class has a single writer.

What an operator can observe from the outside:

* Only `200` responses are recorded. A redirect is not the resource, and a
  `304`/`206` is not the whole of one.
* A response to a request that carried `Authorization`, or that Apache
  authenticated, is recorded only if the origin marked it `public` — the same
  rule the classic path applies. It matters more here: the worker's cache is
  shared across every virtual host on the server.
* `Cache-Control: no-transform` is kept but never offered for optimization.
  `Cache-Control: no-store` is not kept at all.
* **Two `Vary` tests are applied and a response has to pass both.** First this
  module's own, exactly as on the classic path, honouring
  `ModPagespeedRespectVary` the same way. Then the worker's, which admits only
  `Accept-Encoding`, `User-Agent`, `Accept` and `Save-Data`. They differ in
  both directions: the worker refuses an ordinary named axis such as
  `Vary: Accept-Language` that this module would admit on a resource, and this
  module refuses `Vary: *` and `Vary: Cookie` on resources before the worker
  is asked. The cache is shared, so whichever is stricter for a given response
  is the one that decides. How this module treats `Vary` in its own cache is
  unchanged.
* **An origin that sends `Vary: Accept` is recorded, and recorded as such.**
  That origin picks the representation from the request itself, so the copy
  that gets kept is whichever one the first requester happened to ask for.
  The entry is marked with the worker's own flag for it, which is what stops
  the worker deriving a set of variants from that one copy and serving them to
  everyone else. The mark can only be applied when the response is stored: on
  a later cache hit the origin's `Vary` is gone. The question of whether the
  origin varies on `Accept` is answered by the worker's own predicate, not by
  a second reading of the header here.
* A `HEAD` is not recorded: there is no body to record.
* A response that arrived through an upstream cache carrying `Age` is recorded
  as having been generated when the origin generated it, not when this server
  stored it.
* Responses over 16 MB are not kept as originals. The response itself is
  unaffected.
* URLs this module's own rewriting produced are never recorded.
* Only URLs with no query string and no percent-escape are recorded. The
  cache key is composed from a canonical form of the URL that this module
  cannot produce, so it records only URLs already in that form rather than
  filing entries where nothing will look for them.
* A response an upstream already compressed is not recorded: what would be
  stored is not the original.

None of these decisions logs anything per request today, and a response that
is not recorded is served exactly as it would have been. Do not read that as a
guarantee: it is what the current code does, not a contract, and what an
operator is told about a request answered from the cache — or declined — still
needs its own answer.

**A request the cache does not answer is recorded again**, on Apache and
nginx alike, because recording is what happens when there is nothing there to
serve. That covers a URL with no entry, an entry that has aged out, a client
that asked for a reload, and a URL whose entry is marked because its origin
sent headers this module cannot put back (see "Which responses this substrate
will serve" below): such a URL is served by the ordinary path and recorded
again on every eligible request. When no optimized variant fits the client
but the stored original can be served, the original is served from the cache
and nothing is recorded.

Each re-record costs the work of storing the body again and one notification
to the worker. A re-record replaces the previous copy: the superseded copy is
unlinked as the replacement is written, and an entry that reaches the cache's
limit on versions of one entry resets and accepts writes again instead of
freezing. Several server processes can still record the same URL at the same
moment; there is no cross-process ordering available to this module that
would prevent that.

One further caveat inherited from the cache's own contract: a store that
commits while a purge of the same URL is in flight is not fenced against it,
so a recorded original can outlive the entries it belongs to. Nothing in this
module can close that; it is recorded here so it is not discovered later.

## Which responses this substrate will serve

Serving from a cache entry can only put back what the entry carries. So at the
moment a response is first recorded — the only moment the origin's headers are
still visible — the module classifies them, and marks the entry when the
response carries anything a later serve could not reproduce. A marked entry is
still stored and still optimized; what changes is that requests for that URL
are answered by the ordinary path instead of from the cache, exactly as on a
server with these directives unset.

Reproduced, and therefore served from the cache:

- `Content-Type`, `Content-Length`, `ETag`, `Last-Modified`, `Expires`
- `Cache-Control` — rebuilt from the origin's lifetime and its cache
  directives. Directives outside that set, such as `immutable`,
  `stale-while-revalidate` and `stale-if-error`, are not carried.
- `Vary: Accept, Save-Data, User-Agent, Sec-CH-DPR` for a negotiated image.
  When this module serves an optimized image it chose from the request's
  headers, it names each one the choice read: the format comes from
  `Accept`, the size from the device class in `User-Agent`, the pixel density
  from `Sec-CH-DPR` when the browser sends it (else from `User-Agent`), and
  the lighter copy from `Save-Data`. The format is AVIF when the
  header names `image/avif`; WebP when it names `image/webp` or contains
  `image/*` or `*/*`, or when there is no `Accept` header at all (RFC 9110
  reads a wildcard and an absent header as "any type"); the original format
  otherwise. When no copy in that format exists (not written yet, or declined
  by the optimizer's quality check), the original format is served; a client
  naming `image/avif` never gets WebP instead. So `curl` with its default
  `Accept: */*` receives WebP for a `.jpg` URL; a client that needs the
  original format sends `Accept: image/jpeg` (or whichever format it wants,
  with no wildcard). A response carrying the recorded original, rather than an
  optimized copy, is not negotiated and carries none of these for this
  reason. For a cache or CDN in front of this server, see "Behind a shared
  cache or CDN" below.
- `Vary: Accept-Encoding`, and `Vary: Accept` for an origin that negotiates on
  `Accept`. The second is reproduced from the fact recorded with the resource
  that the origin negotiates on `Accept`, and is emitted with whichever copy
  is sent — the origin's own bytes, or an optimized one where such a copy
  exists — so a cache in front of this server is still told not to hand one
  client's representation to another. What is reproduced is that fact; this
  server does not go back to the origin for a second representation. Worth
  knowing for capacity planning rather than correctness: such a resource is
  served as the origin's own bytes today, because the optimizer deliberately
  derives no optimized copies for one — they would all descend from whichever
  representation the first requester happened to elicit.
- `Content-Encoding` — never the origin's (a response whose origin sends one is
  marked, as below), but the module's own: with
  `ModPagespeedDaemonServeStoredEncodings on` it is emitted for a stored
  compressed copy it sends — see "Serving the stored compressed copies".
- Headers the server itself puts on every response — `Date`, `Server`, `Age`,
  `Accept-Ranges` and the hop-by-hop names. These are added again on the way
  out, so they are never lost.

Everything else marks the response. In practice that means a resource whose
origin sends a CORS grant, a security policy, a `Set-Cookie`, a
`Content-Encoding`, a `Vary` on any other axis, or any custom header is served
by the ordinary path and is not optimized in place.

**A coarse edge worth planning around.** The classification happens after the
server has finished assembling the response, so a header your configuration
adds to *every* response is indistinguishable there from one the origin sent
for that URL alone. Such a header is in fact reproduced — your configuration
adds it to these responses too — but the module marks the response anyway. A
server that sets, say, a security header site-wide will therefore get little or
no in-place optimization from this substrate.

## Serving the stored compressed copies

The optimizer stores a gzip and a brotli copy next to each optimized
stylesheet, script and SVG image. With `ModPagespeedDaemonServeStoredEncodings
off` — the default in this release — the module serves the uncompressed
optimized copy and the server compresses it on the way out, exactly as before.
With it on, for that server or virtual host:

* A client that lists `br` (or `gzip`) by name in `Accept-Encoding`, with a
  weight above zero, is sent the stored copy in that coding. Brotli is
  preferred when both are listed.
* The response carries `Content-Encoding` naming that coding,
  `Vary: Accept-Encoding` (on the 200 and on a 304 alike), and a
  `Content-Length` that is the stored copy's size. Apache's own compression is
  not applied to it: `mod_deflate` is not attached, and a compressor added by
  site configuration (`AddOutputFilterByType DEFLATE …`, `mod_brotli`) leaves a
  response that already names a coding alone.
* Each coding has its own validator. The coding is part of the weak ETag's
  first field (`W/"ps-…88…"` for the brotli copy, `…48…` for gzip, `…08…`
  for the uncompressed one), so a 304 only ever validates the copy the client
  holds. These tags never end in the `-gzip` suffix `mod_deflate` adds to
  tags of responses it compresses.
* A client that does not list the coding plainly — `br;q=0`, an unreadable
  weight (`br;q =0`, a bare `br;q`), only the wildcard `*` — gets the
  uncompressed copy, compressed by the server as before.
* When no stored copy in the client's coding exists yet, the uncompressed
  optimized copy is served exactly as with the directive off.
* HTML is not served from the store on Apache, nginx or IIS: the module's
  filter chain rewrites every HTML response, and the server compresses it as
  before.

A reverse proxy in front of the server (an nginx with `gzip on`, a CDN) does
not compress a response that already carries `Content-Encoding`, so the stored
copy reaches the client as stored.

**Your compression exclusions do not apply to these responses.** With the
directive on, a stored copy is sent to every client that lists its coding,
whatever `no-gzip` or `gzip-only-text/html` (set by `BrowserMatch` or
`SetEnvIf`) say for that request: such a client did list the coding, so it can
decode the copy, but if you rely on those exclusions, leave the directive off
for that host. Output filters that rewrite bodies, such as `mod_substitute`,
see the compressed bytes of these responses.

**Statistics.** With the directive on, the optimizer's serve statistics count
the bytes actually sent: for these serves `serve_savings.<type>.optimized_bytes`
is the compressed size, measured against the uncompressed original, and
`serve_savings.<type>.by_encoding` shows how many serves (and bytes) went out
as `identity`, `gzip` and `br`.

**Rollback.** Set the directive to `off` (or remove it) and reload: responses
go back to exactly what the module sent before.

**The default changes to `on` in a later release**, after it has run in
production.

**If no server serves the stored copies**, they are work the optimizer does
for nothing. Setting the optimizer's `gzip_level` and `brotli_level` to `0` (in
its configuration file, or `--gzip-level 0 --brotli-level 0` on its command
line) stops it writing them. Leave them at their defaults on any optimizer
that serves a server with this directive on, or there is nothing to serve.
Raising a level from `0` again does not add copies to resources that are
already optimized: they gain them only when they are next optimized.

## Behind a shared cache or CDN

The same rule holds on Apache, nginx and IIS: every request header the variant
selection reads is named in `Vary`, on the 200 and on the 304, and no other.

| Response served from the optimizer's cache | Request headers that choose the bytes | `Vary` |
|---|---|---|
| An optimized image | `Accept` (format), `User-Agent` (device class: phone, tablet or desktop, and pixel density), `Sec-CH-DPR` (pixel density, when sent), `Save-Data` | `Accept, Save-Data, User-Agent, Sec-CH-DPR` |
| A stored compressed copy (any type) | `Accept-Encoding` in addition | the above plus `Accept-Encoding` |
| The recorded original of an image, a stylesheet, a script | nothing (one copy for every client) | none, or `Accept-Encoding` where the server compresses it |
| Any of these, when the origin itself sends `Vary: Accept` | the origin's `Accept` negotiation | `Accept` is added |

This list is the optimizer's own, so a cache keys a response the same way
whether the module or the optimizer's front end served it. The one client
hint read is `Sec-CH-DPR`: a value of 1.5 or more selects the high-density
copy, any other number the standard one. Browsers send it only to a site that
asks for it with `Accept-CH: Sec-CH-DPR`. The module does not add that header
(nor `Critical-CH`, which would cost a retried request): without the hint,
density comes from `User-Agent`, as before, and a site that wants the hint
can send `Accept-CH` itself. Other hints (`Sec-CH-UA-Mobile`,
`Viewport-Width`, `DPR`) are not read, so they are not named. They could not
replace `User-Agent` either: browsers that send no client hints still need a
device class, and in the traffic sample below they made up about 44% of
requests. HTML is never served from the optimizer's cache, so its
`Vary` is unchanged. A response whose origin sends a `Vary` on any other
header is not served from the optimizer's cache at all (see above), so the
module never replaces a `Vary` the origin chose; the server merges its own
`Accept-Encoding` into the same field. `Cache-Control` is unaffected.

**The recorded original carries no `Vary`.** Before the optimizer has written
a variant for a URL, and whenever no variant fits the client, the module
serves the origin's own bytes. Those bytes are the same for every client, so
one stored copy is correct for all of them, and a cache that keys it on the
URL alone cannot hand anyone the wrong variant. The cost is freshness of the
optimization, not correctness: a cache keeps serving that original until its
lifetime (the origin's `Cache-Control`) runs out, and only the response it
fetches after that carries a variant and its `Vary`. Every response the module
emits is the correct one for the request that produced it.

**Hit rate.** A cache that honours `Vary` keeps one copy per combination of
`Accept`, `Save-Data`, `User-Agent` and `Sec-CH-DPR` values. `User-Agent` is
the expensive one: on a weighted sample of real browser traffic, 100,000
requests carried 764 distinct `User-Agent` strings but only 3 device classes,
so a cache keyed on the raw header holds up to about 250 times more copies of
each image than one keyed on the device class. Expect lower hit rates on such
a cache after this change, and use device-class keying (below) to win them
back. `Accept` varies between browser families too; normalising it to the
format it selects (AVIF, WebP or the original) is a further saving where the
cache can do it.

**Device-class keying.** The class rules, as the optimizer's classifier at the
pinned release (`install/debian/optimizer-pin.json`, 2.5.0) applies them;
check them again when that pin moves: a `User-Agent` containing `iPad` or
`Tablet`, or `Android` without `Mobile`, is a tablet; one containing `Mobile`,
`Android`, `iPhone` or `iPod` is a phone; anything else is a desktop. A
`Sec-CH-DPR` number decides density (1.5 or more is high); without one, phones
count as high density, as does any `User-Agent` containing `2x` or `Retina`.
All matches ignore case except `2x` and `Retina`. A cache that keys on a
coarser or different rule can still hand one client another's variant where
the two rules disagree, so the configurations below send the origin one
representative `User-Agent` per class: the origin then selects for exactly the
class the key names.

**What each cache does by default, and what it needs.**

| Cache | Default with these responses | Configuration needed |
|---|---|---|
| Cloudflare | Ignores `Vary` apart from `Accept-Encoding` (and `Accept` for images with Vary for Images enabled), and caches images: one device's variant goes to everyone. | A Cache Rule for image paths whose cache key includes the device type and the `Accept`, `Save-Data` and `Sec-CH-DPR` request headers. Request headers in the cache key, and possibly the device type as well, may need the Enterprise plan: check what your plan offers, and without it set the rule to bypass the cache for these images. Cloudflare's device type comes from its own `User-Agent` rules, which can differ from the ones above at the edges. |
| Amazon CloudFront | Caches on the cache policy's key only and ignores the origin's `Vary`; it does not forward `User-Agent` or `Accept` unless told to, so one copy goes to everyone and the origin selects for CloudFront's own `User-Agent`. | Add `CloudFront-Is-Mobile-Viewer`, `CloudFront-Is-Tablet-Viewer`, `Accept`, `Save-Data` and `Sec-CH-DPR` to the cache policy's headers, and forward `User-Agent` in the origin request policy. For an exact match with the rules above, set a class header with a CloudFront Function and key on that instead of the CloudFront-Is-* pair. |
| Akamai | Treats a `Vary` other than `Accept-Encoding` as uncacheable: correct, but no hits for these images. | Leave it, or cache with a cache key that includes the device class, `Accept`, `Save-Data` and `Sec-CH-DPR`. Removing the `Vary` header without that key brings the cross-serving back. |
| Fastly, Varnish | Honour `Vary`: correct, one copy per raw `User-Agent`. | Normalise `User-Agent` to the class in `vcl_recv` (below). |
| nginx `proxy_cache` | Honours `Vary`: correct, one copy per raw `User-Agent`. | Put the class in `proxy_cache_key` (below). |

Varnish or Fastly, in `vcl_recv` (as tested by
`test/system/shared_cache/varnish-device-class.vcl`): the `User-Agent` of an
image request becomes one string per class, so the response's
`Vary: User-Agent` splits the cache into a handful of copies.

```vcl
if (req.url ~ "(?i)\.(jpe?g|png|gif|webp|avif)(\?|$)") {
  if (req.http.User-Agent ~ "(?i)ipad|tablet" ||
      (req.http.User-Agent ~ "(?i)android" && req.http.User-Agent !~ "(?i)mobile")) {
    if (req.http.User-Agent ~ "2x|Retina") {
      set req.http.User-Agent = "device-class tablet Retina";
    } else {
      set req.http.User-Agent = "device-class tablet";
    }
  } elsif (req.http.User-Agent ~ "(?i)mobile|android|iphone|ipod") {
    set req.http.User-Agent = "device-class Mobile";
  } elsif (req.http.User-Agent ~ "2x|Retina") {
    set req.http.User-Agent = "device-class Retina";
  } else {
    set req.http.User-Agent = "device-class desktop";
  }
}
```

nginx `proxy_cache` (as tested by
`test/system/shared_cache/nginx-device-class.conf`): the class goes into the
key, the raw-`User-Agent` `Vary` is not used for the lookup, and the origin is
sent the class's representative `User-Agent`.

```nginx
map $http_user_agent $device_view {
    "~*ipad|tablet"                tablet;
    "~*^(?!.*mobile).*android"     tablet;
    "~*mobile|android|iphone|ipod" mobile;
    default                        desktop;
}
map $http_user_agent $device_density {
    "~2x|Retina" " Retina";
    default      "";
}
map "$device_view$device_density" $device_class_ua {
    "mobile"         "device-class Mobile";
    "mobile Retina"  "device-class Mobile";
    "tablet"         "device-class tablet";
    "tablet Retina"  "device-class tablet Retina";
    "desktop Retina" "device-class Retina";
    default          "device-class desktop";
}

location ~* \.(jpe?g|png|gif|webp|avif)$ {
    proxy_pass http://origin;
    proxy_cache images;
    proxy_cache_key "$scheme$host$request_uri|$http_accept|$http_save_data|$http_sec_ch_dpr|$device_class_ua";
    proxy_ignore_headers Vary;
    proxy_set_header User-Agent $device_class_ua;
}
```

## When the worker is missing

If the directives are set and the worker cannot be used — its client library
is not installed, it is **an older release than this module can work with**, it
is an older release that does not publish its cache volume's size, it
publishes a cache-directory generation this build is not written for, it has
not created that volume yet, its volume will not open, nothing answers on the
socket, or any of those is unreachable **because the web-server user is not in
the `pagespeed` group** — then, for that server:

* in-place optimization is **off** — nothing is recorded and nothing is
  served from the worker's cache;
* **one** message is logged, at startup, at error level, saying which of those
  it was — once per condition per server process, however many virtual hosts
  and however many times the configuration is read;
* the classic in-place recorder does **not** come back;
* the worker's cache volume is **not** opened and never created.

Three of those conditions mean only "the worker was not running yet": no
published size, no volume on disk, nothing answering on the socket. Apache
looks for those three again while it serves, at most once every 30 seconds
per server process, and turns in-place optimization back on by itself when
the worker answers; one info line in the error log says so. nginx does the
same, per worker process: a request that finds the worker unusable asks for
one recheck at most every 30 seconds, run off the request path, and the
request after the recheck that finds the worker answering uses it again;
one info line per worker process in the error log says so (see
"Start order and upgrades" in [install-nginx.md](install-nginx.md)).

A version too old to use is a **refusal**, not a partial start, and that is
deliberate. The published interface grows, and this module binds what it needs;
a release missing part of it is missing rules, not merely features. The
`Vary: Accept` marking above is the clearest case — without the worker's own
predicate there is no safe way to decide whether an origin negotiates on
`Accept`, and guessing wrong is wrong bytes with nothing to notice it. A
refusal at startup, named in the log with both version numbers, is the loud
form of that answer. Install the matching release.

That last point is the one worth stating explicitly. Falling back would look
like the kinder behaviour and is not: it would fill a cache that nothing on
the worker substrate reads, and it would leave a server that appears healthy
while an operator's worker is simply absent. Off and loud beats busy and
silent.

Requests continue to be served normally throughout. The worker's absence
costs optimization, never availability.

## Checking a deployment

* Start the server and read the error log once. A healthy pair logs nothing
  about the worker; each unhealthy condition logs exactly one line.
* If the server refuses to start, read the message: it says whether the
  start removed the volume file its open created, and names the file to
  remove by hand before the next start when it could not.
* A warning about extra volume files after changing the cache size is
  expected; it names the leftovers so you can delete them.
* An older worker release is a normal, loud, non-fatal state: in-place
  optimization is off until the packages match. A release too old to use at all
  says so in one line naming the version it has and the version this build
  needs.
* A **permission-denied** line names the likely missing group membership
  (`usermod -a -G pagespeed <web user>`); an **absent** socket or volume line
  tells you to start the worker. The two are not the same fix, and the log
  says which one you have.
* A worker that was not running when the server started is picked up again
  within about 30 seconds of it answering: one info line says in-place
  optimization is on again. Only the three not-running conditions recover
  this way; every other line above stays until the server restarts.
* A worker publishing no cache-directory generation logs one line about the
  legacy layout and keeps working; one publishing a *different* generation
  logs a handshake failure and in-place optimization stays off until the
  module and worker packages agree.
* An in-place request on a server where the worker is unavailable is served
  as-is, with no per-request log entries.
