# Deploying alongside the optimizer daemon

This page covers the two Apache directives that point the module at the
optimizer daemon, the layout they are meant to be used in, and what happens
when the daemon is missing.

Both halves are wired now. With the directives set, a server records origin
responses into the daemon's cache, asks the daemon to optimize them, and
answers later in-place-eligible requests from that cache when there is
something there to answer them with.

**Leave both directives unset on a production server for now.** The cost that
kept them off has not changed: a request the cache cannot answer is still
recorded, so a hot URL whose optimized form does not exist yet is recorded once
per request per process.

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
| `ModPagespeedDaemonSocketPath` | Path of the daemon's notification socket | server / virtual host |
| `ModPagespeedDaemonVolumePath` | Path of the daemon's shared cache volume | server / virtual host |

Both are unset by default, which is the classic configuration: in-place
optimization behaves exactly as it always has and nothing on this page
applies. Set them together — one without the other is a configuration mistake
and the module says so at startup.

There is no directive that switches between the classic and daemon in-place
substrates at run time, and there will not be one. If the daemon substrate
misbehaves on a host, the mitigation is to reinstall the previous module and
daemon packages together. The module and the daemon are a matched pair; they
are installed, upgraded and rolled back as one.

## Group membership and the versioned cache directory (2.1)

Since the daemon's privilege drop (2.1), the optimizer daemon runs as the
unprivileged `pagespeed` user and its artifacts are group-scoped: the cache
volume, the notify socket and the shared configuration are mode `0660`/`0640`
owned by `pagespeed:pagespeed`, under the versioned cold-start cache directory
(`/var/cache/pagespeed-optimizer/v1/`) and `/run/pagespeed-optimizer/`. The
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
  the daemon's cache directory, shared configuration or socket *because of
  permissions* logs a line naming the likely cause — the web-server user not
  being in the `pagespeed` group — rather than the "start the daemon" line
  the absent case gets. The same distinction is made in the request-serving
  process, where the volume is really opened.
* **The cache-directory generation is checked.** The daemon publishes
  `cache_dir_generation` in its shared configuration; this module is built
  for generation 1. A daemon publishing a different generation is a loud
  handshake failure — in-place optimization off, nothing opened, never a
  silent split-brain across two layouts that share nothing. A daemon
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

  # The daemon. A DIFFERENT directory, and its own socket.
  ModPagespeedDaemonSocketPath   /var/run/pagespeed/daemon.sock
  ModPagespeedDaemonVolumePath   /var/cache/pagespeed-daemon/volume
</IfModule>
```

The two caches are different things with different owners. The module owns the
first; the daemon owns the second and this module is one of its readers.
Pointing both at one directory makes a single volume file's health a
shared-fate dependency of both: a volume that will not open, or that a
crashed process left locked, takes out the classic cache at the same time,
and one cache's size accounting starts evicting the other's entries.

Co-locating them is not blocked, but it is a deviation, and the split-cache
warning below applies to it with extra force.

## Cache sizing is inherited, not configured, and a split cache stops the server

The daemon derives the FILENAME of its cache volume from the volume's
geometry. That is deliberate: two peers with different layouts open different
files during an overlapping upgrade instead of corrupting one another's data.

The consequence is that a sizing disagreement is invisible. A process that
opens the daemon's directory with a different size does not collide and does
not fail — it creates and uses a different file, shares nothing, and runs a
permanently cold cache. Every request works. Nothing is ever optimized. There
is no error anywhere.

So the module does not have a size setting, and does not carry one internally.
It **inherits** the size from the daemon, which publishes the size it actually
opened its volume with. There is nothing for an operator to keep in sync.

What the module then checks is whether inheriting that size actually landed on
the daemon's volume:

* **It never opens the volume while the size is unknown**, and it never
  creates one. If the daemon is not running, or is an older release that does
  not publish its size, in-place optimization is simply off (see below).
* **If the directory already holds more than one volume file**, the server
  starts and logs a warning naming them. This is the normal state after you
  change the daemon's cache size: the filename encodes the size, and the daemon
  leaves the file it stopped using in place. The size the daemon publishes
  names exactly one of them, and the module verifies it attached to that one,
  so the extras are wasted disk rather than a split cache. They can be deleted
  while the daemon is stopped.
* **If opening at the daemon's published size creates a second volume file**,
  the server **refuses to start** and names the file it just created. There
  are two ways to reach it, and the message distinguishes them.

  The common one is **start order during an upgrade to a release that changes
  the on-disk cache format**. The daemon's volume filename carries the format,
  so a daemon on a new format opens a new file; until it has restarted and
  created it, the only volume in the directory is the previous format's, and a
  web server that starts in that window creates the new file itself instead of
  attaching to the daemon's. **Upgrade and start the optimizer daemon first,
  let it create its volume, then start the web server.** The file the refused
  start created can be removed once the daemon is up.

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
  ordering is ordering only — the daemon is not required by the web server, so
  a web server still starts when the daemon is absent, and the daemon still
  starts when no web server is installed.

  The other is a genuine disagreement: the published size does not match the
  volume on disk. Remove the file the start created, then install a module and
  daemon package pair that agree.

That last one is the only condition that stops the server. It earns it because
it is the only one an operator cannot otherwise see: everything else announces
itself in the log and the server starts. In particular, a routine cache resize
must never prevent a start.

### The cache directory needs room for two volumes across a format change

A release that changes the on-disk cache format leaves the old volume file in
place and creates a new one beside it. Both count against the same filesystem,
and the new one grows to the daemon's **full configured cache size** as it
fills — the daemon extends it at startup, before any reclamation runs. So the
directory needs free space of at least the daemon's cache size on top of what
the existing volume already occupies.

**If the directory is sized for exactly one volume, delete the old file before
starting the new daemon, not after.** This is the opposite of the usual advice
to tidy up afterwards, and the reason is that on Linux the new volume is
allocated sparsely: too little space does not fail the start. It fails hours
later, mid-traffic, as the cache warms and the sparse file becomes real. On
Windows the space is claimed at creation, so the failure is immediate instead.
Neither the module nor the daemon checks free space, so this is an operator
check.

The procedure, with the daemon stopped:

1. List the directory. Volume files are named `<stem>-<format number>-<hash>`,
   where `<stem>` is the last path element of `ModPagespeedDaemonVolumePath`
   (`cache` for the packaged default, which has no file extension).
2. The file to remove is the one whose **format number is lower**. A file with
   a different hash and the same format number is a leftover from an earlier
   cache size, not from an earlier format; those are safe to remove too.
3. Remove it, then start the daemon, then start the web server.

Removing the old file gives up a warm rollback — an earlier daemon would have
reopened it untouched. Where there is room for both, keep it until you are
confident you will not roll back, and reclaim it then. A running daemon holds
its volume open, so deleting a file while it runs does not return the space
until it restarts.

The module never enables a per-process memory tier over the daemon's volume,
regardless of the daemon's own defaults. Such a tier is keyed without any
check against the on-disk entry, so a process holding it would keep serving
bytes the daemon had already replaced.

## What gets recorded, and what does not

With both directives set and the daemon usable, an in-place-eligible response
is kept in the daemon's cache as the ORIGINAL — the bytes the origin sent,
before anything touched them — with the origin's `Cache-Control` state and its
validators, and the daemon is told the entry is there. The module writes that
one class of entry and no other: the optimized variants are the daemon's to
write, and each class has a single writer.

What an operator can observe from the outside:

* Only `200` responses are recorded. A redirect is not the resource, and a
  `304`/`206` is not the whole of one.
* A response to a request that carried `Authorization`, or that Apache
  authenticated, is recorded only if the origin marked it `public` — the same
  rule the classic path applies. It matters more here: the daemon's cache is
  shared across every virtual host on the server.
* `Cache-Control: no-transform` is kept but never offered for optimization.
  `Cache-Control: no-store` is not kept at all.
* **Two `Vary` tests are applied and a response has to pass both.** First this
  module's own, exactly as on the classic path, honouring
  `ModPagespeedRespectVary` the same way. Then the daemon's, which admits only
  `Accept-Encoding`, `User-Agent`, `Accept` and `Save-Data`. They differ in
  both directions: the daemon refuses an ordinary named axis such as
  `Vary: Accept-Language` that this module would admit on a resource, and this
  module refuses `Vary: *` and `Vary: Cookie` on resources before the daemon
  is asked. The cache is shared, so whichever is stricter for a given response
  is the one that decides. How this module treats `Vary` in its own cache is
  unchanged.
* **An origin that sends `Vary: Accept` is recorded, and recorded as such.**
  That origin picks the representation from the request itself, so the copy
  that gets kept is whichever one the first requester happened to ask for.
  The entry is marked with the daemon's own flag for it, which is what stops
  the daemon deriving a set of variants from that one copy and serving them to
  everyone else. The mark can only be applied when the response is stored: on
  a later cache hit the origin's `Vary` is gone. The question of whether the
  origin varies on `Accept` is answered by the daemon's own predicate, not by
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

**A request the cache cannot answer re-records**, because recording is what
happens when there is nothing there to serve. Re-recording REPLACES when a
single process writes a URL — but a server runs many processes and each is an
independent writer for the same URL, so superseded copies of a hot URL's body
can accumulate under that URL until the cache's own limit on how many versions
of one entry it will hold refuses further writes. Each copy is a whole
response body, so this costs **disk as well as work**, and it does not shrink
again on its own. There is no cross-process ordering available to this module
that would prevent it. That is an accepted intermediate state, not a design
target, and it is the main reason these directives are not ready for a
production server.

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
- `Vary: Accept-Encoding` and `Vary: Accept`. The second is reproduced from the
  fact recorded with the resource that the origin negotiates on `Accept`, and is
  emitted with whichever copy is sent — the origin's own bytes, or an optimized
  one where such a copy exists — so a cache in front of this server is still
  told not to hand one client's representation to another. What is reproduced
  is that fact; this server does not go back to the origin for a second
  representation. Worth knowing for capacity planning rather than correctness:
  such a resource is served as the origin's own bytes today, because the
  optimizer deliberately derives no optimized copies for one — they would all
  descend from whichever representation the first requester happened to
  elicit.
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

## When the daemon is missing

If the directives are set and the daemon cannot be used — its client library
is not installed, it is **an older release than this module can work with**, it
is an older release that does not publish its cache volume's size, it
publishes a cache-directory generation this build is not written for, it has
not created that volume yet, its volume will not open, nothing answers on the
socket, or any of those is unreachable **because the web-server user is not in
the `pagespeed` group** — then, for that server:

* in-place optimization is **off** — nothing is recorded and nothing is
  served from the daemon's cache;
* **one** message is logged, at startup, at error level, saying which of those
  it was — once per condition per server process, however many virtual hosts
  and however many times the configuration is read;
* the classic in-place recorder does **not** come back;
* the daemon's cache volume is **not** opened and never created.

A version too old to use is a **refusal**, not a partial start, and that is
deliberate. The published interface grows, and this module binds what it needs;
a release missing part of it is missing rules, not merely features. The
`Vary: Accept` marking above is the clearest case — without the daemon's own
predicate there is no safe way to decide whether an origin negotiates on
`Accept`, and guessing wrong is wrong bytes with nothing to notice it. A
refusal at startup, named in the log with both version numbers, is the loud
form of that answer. Install the matching release.

That last point is the one worth stating explicitly. Falling back would look
like the kinder behaviour and is not: it would fill a cache that nothing on
the daemon substrate reads, and it would leave a server that appears healthy
while an operator's daemon is simply absent. Off and loud beats busy and
silent.

Requests continue to be served normally throughout. The daemon's absence
costs optimization, never availability.

## Checking a deployment

* Start the server and read the error log once. A healthy pair logs nothing
  about the daemon; each unhealthy condition logs exactly one line.
* If the server refuses to start, read the message: it names the volume file
  the start created and what to do about it.
* A warning about extra volume files after changing the cache size is
  expected; it names the leftovers so you can delete them.
* An older daemon release is a normal, loud, non-fatal state: in-place
  optimization is off until the packages match. A release too old to use at all
  says so in one line naming the version it has and the version this build
  needs.
* A **permission-denied** line names the likely missing group membership
  (`usermod -a -G pagespeed <web user>`); an **absent** socket or volume line
  tells you to start the daemon. The two are not the same fix, and the log
  says which one you have.
* A daemon publishing no cache-directory generation logs one line about the
  legacy layout and keeps working; one publishing a *different* generation
  logs a handshake failure and in-place optimization stays off until the
  module and daemon packages agree.
* An in-place request on a server where the daemon is unavailable is served
  as-is, with no per-request log entries.
