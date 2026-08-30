# Changelog

All notable changes to mod_pagespeed are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **The admin console can now show the optimizer daemon's status, cache
  statistics, and back-pressure state.** Three read-only endpoints under the
  admin path — `v1/daemon/health`, `v1/daemon/stats`, and
  `v1/daemon/cooldowns` — proxy the daemon's management API over its unix
  socket, configured with the new `DaemonApiSocketPath` directive
  (Apache/nginx/Envoy; default `/run/pagespeed-optimizer/api.sock`, empty
  disables). The proxy is read-only by construction: GET/HEAD only, the
  upstream path comes from a fixed allow-list, and no client query string,
  headers, or body reach the daemon. When the daemon is unreachable the
  endpoints answer 502 so the console panels can render that state.

- **The admin console now shows the optimizer daemon alongside the module.**
  Three read-only panels — daemon status (health, version, uptime, checks,
  browser-analysis state), daemon cache (entries, size, serve-savings
  counters), and daemon back-pressure (thread-pool occupancy, dropped
  notifications, cache cooldowns) — poll the daemon's management API through
  the module's `/v1/daemon/` endpoints. When the daemon is unreachable or not
  configured the panels say so plainly; the module's own pages are unaffected.

### Removed

- **The admin console's License tab is gone.** In its navigation slot is a
  single gentle, dismissible Support panel pointing at support subscriptions;
  the dismissal is remembered. The unlicensed and over-cap banners in the top
  bar are removed with it. The underlying `/v1/license/*` endpoints are
  untouched and still answer.

### Fixed

- **A cache volume path whose name ends in a dot now attaches to the
  optimizer daemon's volume instead of disabling in-place optimization.**
  For a trailing-dot name (for example `.../cache.`) the daemon writes its
  volume as `cache-<format>-<hash>.` — the trailing dot is a bare-dot file
  extension — while the module looked for names starting with the whole
  `cache.` stem, a shape the daemon never writes. The module concluded the
  volume did not exist and logged the misleading advice to "start the
  daemon first" while the daemon was running and healthy, silently keeping
  in-place optimization off. The module now mirrors the daemon's filename
  split for the trailing-dot case, so such a volume path attaches like any
  other. Extensionless and ordinary-extensioned paths were never affected.

- **A cache volume path with a file extension now attaches to the optimizer
  daemon's volume instead of disabling in-place optimization.** With the
  daemon directives pointing at an extensioned path (for example
  `.../cache.vol`), the module looked for the volume under a filename the
  daemon never uses, concluded the volume did not exist, and logged the
  misleading advice to "start the daemon first" while the daemon was running
  and healthy — in-place optimization stayed off with no real cause reported.
  The module now recognizes the filename the daemon actually writes, so an
  extensioned volume path attaches like any other. Installations using the
  packaged default path (extensionless) were never affected.

- **An image optimized in place at its original URL no longer permanently
  falls back to serving from the origin once its cache entry ages out.**
  When the optimizer daemon was in use, an optimized URL was served from
  cache only until its freshness lifetime elapsed; after that, every request
  went back to the origin and was never re-optimized, because the optimizer
  treated the URL as already processed. The server now tells the optimizer
  when it declines an aged-out variant for genuine expiry, so the optimizer
  discards the stale variants and rebuilds them from the freshly fetched
  original. Client reloads (Ctrl+F5) and origin `no-cache` responses do not
  trigger this, so a plain browser reload can never evict a URL's optimized
  variants. Affected only deployments running the optional optimizer daemon;
  the classic in-place cache was not affected.

- **Licensed installs no longer log a spurious per-process UNLICENSED
  warning.** An internal helper context used only for decoding
  `.pagespeed.` URLs ran the license check against a path that never
  holds a license file, so every worker process logged the "running
  UNLICENSED" warning at startup even when the real serving contexts
  had loaded a valid license. Served responses were never affected (no
  `X-PageSpeed-Warn` header was emitted); the warning was cosmetic but
  landed in the error log and the admin message page on every worker
  spawn. The helper context now skips license initialization. All
  server flavors were affected.

- **A cached WebP or AVIF image selected by one browser's Accept header can
  no longer be reused for a browser that did not advertise the format, when
  the origin spells its Vary header differently.** The cache-validity check
  that keeps format-negotiated images from crossing clients matched the
  entry's `Vary` value byte-for-byte, so two legal origin spellings slipped
  past it: a lowercase `Vary: accept` (header-field tokens are
  case-insensitive) and `Vary: *`. With either, a stored WebP/AVIF response
  could be served to a client whose own request would not have selected it —
  bytes it may not decode. The check now matches the `Accept` token
  case-insensitively and treats `Vary: *` as never reusable as-selected for
  a client that did not advertise the format. Deployments in front of
  origins that emit either spelling should update; nothing changes for
  clients that do advertise the format, and `.pagespeed.` resource URLs
  were never affected (their format is committed in the URL).

### Added

- **The Debian package can declare an exact-version dependency on
  `pagespeed-optimizer`.** `install/debian/build.sh -d <version>` adds
  `pagespeed-optimizer (= <version>)` to the module package's Depends: the
  module serves through the optimizer daemon, and the pair is only supported
  at matching versions, so installs, upgrades, and rollbacks move both
  packages together. Omitting `-d` packages without the dependency (with a
  warning), which keeps synthetic upgrade-test and pre-daemon builds working
  unchanged.

- **Apache now serves optimized responses from the optimizer daemon's cache.**
  With the two daemon directives set, an in-place-eligible request is answered
  from the daemon's shared cache when there is something there to answer it
  with, and the response is recorded only when there is not. Together with the
  recording change below this completes the in-place path over the daemon's
  cache: a response goes in on its way out, and later requests for it come back
  optimized.
  **It is still not the time to enable them**, for the reason under the
  recording entry: a request the cache cannot answer is still recorded, so a
  hot URL whose optimized form does not exist yet is recorded once per request
  per process and superseded copies of its body accumulate.
  What a deployment can observe from the outside:
  - The bytes served are the variant the daemon produced for THIS request's
    capabilities, chosen from the request headers alone. A variant in an image
    format the request did not advertise is never served: if the only variant
    that exists is one this client cannot decode, the request is answered as it
    would have been with no daemon configured at all.
  - When no variant fits, the ORIGINAL — the bytes the origin sent — is served
    from the cache. That saves a trip to the origin; it is not an optimized
    response and is not counted as one.
  - When neither exists, or the entry has aged out, or the client asked for a
    reload, the request is served by the ordinary path exactly as it would be
    on a server with no daemon.
  - `Vary: Accept` is emitted on a response whose bytes were chosen from the
    request's `Accept`, and on a response whose origin negotiates on `Accept`
    itself, and on nothing else. Both matter: the second is what keeps a cache
    in front of this server from handing one client's representation to
    everybody. It is emitted with whichever copy is sent — the origin's own
    bytes, or an optimized one where such a copy exists — because the fact
    that the origin negotiates is recorded with the resource rather than with
    a copy. In practice a resource whose origin negotiates on `Accept` is
    served as the origin's own bytes today: the optimizer deliberately derives
    no optimized copies for one, precisely because they would all descend from
    whichever representation the first requester happened to elicit.
  - Every response served from the cache carries an `Age` saying how old the
    stored copy is, so a cache in front of this server does not add the full
    lifetime again on top of time already spent.
  - Responses carry a **weak** `ETag` derived from the stored entry, and an
    `If-None-Match` carrying one gets a `304`. It is the same tag ModPageSpeed
    2.0's own front end emits for the same entry, so a client that revalidates
    against either gets a consistent answer. A conditional request carrying the
    ORIGIN's own `ETag` does not match it — that tag is for a different
    representation — and gets the full response once.
  - `Last-Modified` is emitted only when what is served is the origin's own
    bytes, and an `If-Modified-Since` is answered on exactly those responses.
    An optimized variant is not the representation the origin's validator
    describes, so it carries no `Last-Modified` and a conditional over one
    gets the full response.
  - `Cache-Control` is built from the origin state stored with the entry, by
    the daemon's own rules, so this module and the daemon cannot disagree about
    how long a response is good for. A `stale-while-revalidate` is never
    synthesized: authorizing a cache downstream to serve stale bytes is the
    origin's call, not this server's. An origin's `no-cache` is passed on
    rather than dropped.
  - Two statistics counters, `ipro_daemon_served` and
    `ipro_daemon_fallthrough`, report how many in-place-eligible requests this
    substrate answered and how many it passed through. They are separate from
    `ipro_served` / `ipro_not_in_cache` / `ipro_not_rewritable`, which count the
    classic in-place path and stay at zero on this one.
  - When the cache answers a request with a variant that is not the one this
    request's capabilities name — a **fallback hit**, for example a request
    answered with the desktop copy because no smaller-screen copy exists yet —
    the request is served, and the module now also asks the daemon, once and
    without waiting for an answer, to build the variant this client named.
    Previously nothing asked: a serve records nothing, so such a family only
    converged if a cache miss happened to come first. No ask is made for a
    resource whose origin negotiates on `Accept` itself, because the daemon
    deliberately builds no variants for one — asking would be permanent
    per-request traffic for work that never happens. Two statistics counters
    observe the mechanism: `ipro_daemon_fallback_notified` counts the asks,
    and `ipro_daemon_fallback_notify_failed` counts asks that could not be
    delivered — a failed send is fire-and-forget, so without the second
    counter a dead notification channel would be indistinguishable from a
    quiet converged family. Neither is part of the served/fell-through
    partition above, because every fallback hit is already a serve.
  - Which variant is chosen ignores the request's `Accept-Encoding`: the
    selection is made as though the client asked for uncompressed bytes, so
    what comes back is the daemon's uncompressed variant and compression on
    this path stays exactly where it already was. This matters for what you
    observe: a browser advertises `br`, and without this the daemon would
    offer its pre-compressed copy, which this module will not emit — every
    such request would have been answered with the unoptimized original
    instead, and nothing in the counters would have said so.
  What a response carries is composed from the entry — its content type, its
  caching state and the validators above. A header the origin sent that is
  outside that set cannot be reproduced, and the entry below is what the
  substrate now does about it.

- **A response whose headers this substrate cannot reproduce is served
  unoptimized instead of served with those headers missing.** Previously such a
  response was recorded, optimized and served from the daemon's cache like any
  other, and the headers it carried that a cache entry cannot hold were dropped
  from every later response — silently, with nothing in a log or a counter to
  say so. A resource whose origin sent a CORS grant, a security policy or any
  custom header was served without it for as long as the entry lived.
  Now the module classifies the origin's headers at the moment the response is
  first seen — the only moment they are still visible — and marks the stored
  entry when it finds one it could not put back. Requests for a marked URL are
  answered by the ordinary path, exactly as on a server with the daemon
  directives unset: unoptimized, but complete.
  What is reproduced, and so still served from the cache: `Content-Type`,
  `Content-Length`, `ETag`, `Last-Modified`, `Expires`; `Cache-Control`, rebuilt
  from the origin's lifetime and cache directives, though not directives outside
  that set such as `immutable` or `stale-while-revalidate`; `Vary` on
  `Accept-Encoding` and on `Accept` — the latter re-emitted from the recorded
  fact that the origin negotiates, on optimized responses as well as on the
  origin's own bytes, though what is reproduced is that fact and not a second
  representation fetched from the origin; and the headers the server puts on
  every response on its way out, including `Date`, `Server`, `Age` and
  `Accept-Ranges`. Anything else — `Set-Cookie`, `Content-Encoding`, a `Vary` on
  another axis, a CORS or security header, a custom header — makes the response
  fall through.
  Two things to plan around. A resource that used to be optimized in place may
  now be served unoptimized; that is the intended direction, and the response is
  correct where before it was quietly incomplete. And because the classification
  happens after the server has assembled the response, a header your
  configuration adds to *every* response cannot be told apart from one the
  origin sent for a single URL — so a server that sets, say, a security header
  site-wide will get little or no in-place optimization from this substrate,
  even though that header would in fact have been reproduced.
  These directives also now require a **newer optimizer release** than before
  (`PS_API` 1.8), because the mark is recorded in a field that release is the
  first to define; an older one is refused at startup with a message saying so,
  as it always has been.

- **Apache now records origin responses into the optimizer daemon's cache and
  asks the daemon to optimize them.** With the two daemon directives set, an
  in-place-eligible response is kept in the daemon's shared cache as the
  original — the bytes the origin sent, before anything touched them — together
  with the origin's `Cache-Control` state and validators, and the daemon is
  told it is there.
  **Still not the time to enable them.** Reading back what the daemon produced
  now happens too — see the serving entry above — so a server with these
  directives set does optimize in place. What has not changed is the cost
  below.
  One cost is worth knowing before it is measured rather than after. A request
  the cache cannot answer is recorded, and that includes a request whose URL
  is cached but whose format is not yet built, so on a busy URL **eligible
  requests record again**. Re-recording replaces when one
  process writes a URL, but a server runs many, so superseded copies of a hot
  URL's body can accumulate until the cache's own limit on how many versions
  of one entry it will hold stops them. That costs disk as well as work, it
  does not shrink again by itself, and it is the main reason these directives
  are not ready for a production server.
  The rules a deployment can observe from the outside:
  - A response the origin marked `no-transform` is kept but never offered for
    optimization, and a response marked `no-store` is not kept at all.
  - A response whose `Vary` names anything the daemon's cache cannot tell
    variants apart by — anything outside `Accept-Encoding`, `User-Agent`,
    `Accept` and `Save-Data`, or `Vary: *` — is not stored. Two tests are
    applied and a response has to pass both: this module's own, exactly as it
    is applied on the classic path and honouring `ModPagespeedRespectVary` the
    same way, and then the daemon's, which is the stricter of the two on the
    axes where they differ. The cache is shared, so the stricter rule is the
    one that keeps a client from being handed the wrong representation.
    Nothing about how this module treats `Vary` in its own cache changes.
  - A response to a request that carried `Authorization`, or that Apache
    authenticated, is stored only if the origin marked it `public` — the same
    rule the classic path applies, and it matters more here because the
    daemon's cache is shared across every virtual host on the server.
  - Only `200` responses are recorded. A redirect is not the resource.
  - A response that arrived through an upstream cache carrying `Age` is
    recorded as having been generated when the ORIGIN generated it, not when
    this server stored it. Without that an entry behind a CDN would serve stale
    bytes as fresh for the whole of `Age`.
  - Responses larger than 16 MB are not kept as originals; the response itself
    is unaffected.
  - URLs this module's own rewriting produced are never recorded as originals.
  - Recording is limited to URLs with no query string and no percent-escape.
    The cache key is composed from a canonical form of the URL that this module
    cannot produce, so it records only the URLs that are already in that form
    rather than filing entries where nothing will look for them.
  - A response that arrives already compressed by an upstream is not recorded,
    because what would be stored is not the original.
  A server whose daemon is too old is now told so at startup and switches
  in-place optimization off, rather than starting in a state where nothing
  could ever be recorded.

- **An origin that negotiates on `Accept` is now recorded as doing so.** When a
  recorded response carries `Vary: Accept`, the origin has said that IT picks
  the representation from the request — so the copy that gets kept is whichever
  representation the first requester happened to elicit. The stored entry is now
  marked accordingly, which is what stops the daemon deriving a family of
  variants from that one copy and serving them to clients that asked for
  something else. Without the mark the outcome is wrong bytes with no error
  anywhere, and it cannot be corrected afterwards: on a cache hit the origin's
  `Vary` is gone, so the mark can only be applied when the response is stored.
  Whether an origin varies on `Accept` is answered by the daemon's own
  predicate rather than by a second reading of the header here, so the two
  cannot disagree.
  **This raises the daemon release these directives need.** A daemon that
  predates the marker cannot be used: in-place optimization is switched off for
  that server and one line at startup names the version installed and the
  version required. That is deliberate — without the daemon's predicate there
  is no safe way to answer the question, and answering it wrongly is the silent
  failure above. Install the matching daemon release. Servers with the daemon
  directives unset are unaffected.

- **Apache can now be pointed at the optimizer daemon, with
  `ModPagespeedDaemonSocketPath` and `ModPagespeedDaemonVolumePath`.** These
  name the daemon's notification socket and its shared cache volume. Both are
  unset by default and an existing configuration is unaffected: with neither
  set, in-place optimization behaves exactly as it did before.
  **Do not enable them yet.** The startup, recording and serving halves are all
  in place now — see the two entries above — and the reason to wait is the
  recording cost described there rather than a missing piece. The operational
  rules around these directives are the part worth reading first; see
  `docs/daemon-adapter-deployment.md`.
  Two of those rules matter enough to state here. Keep the daemon's volume on a
  **different** path from `ModPagespeedFileCachePath`, so one cache file's
  health is not a shared-fate dependency of both. And there is deliberately **no
  cache-size setting on the module**: the volume's filename is derived from its
  size, so a module and a daemon that disagreed would quietly use two different
  files and share nothing. The module inherits the size the daemon publishes,
  and there is nothing to keep in sync. If opening the daemon's volume creates a
  second volume file rather than attaching to the daemon's, **the server refuses
  to start** and the message names the file — that is the only condition that
  stops a start, because it is the only one that is otherwise silent. Leftover
  volume files from an earlier cache size are normal, are reported as a warning
  naming them, and never prevent a start.
  If the directives are set and the daemon cannot be used — not installed, not
  running, an older release that does not publish its cache volume's size, or
  not answering — in-place optimization is switched off for that server and
  says so once, at startup, in the error log. It does not fall back to recording
  into the module's own cache, and it never opens or creates the daemon's
  volume. Requests are served normally either way: a missing daemon costs
  optimization, never availability.

- **Options that a request resolves to can now be stated as a value, for the
  optimizer to key its work by.** A request's effective configuration is the
  result of merging the global settings with anything set per virtual host, per
  directory, and per request, and until now that result existed only inside the
  module. It can now also be rendered as a compact, canonical description of
  those options together with a signature over it.
  Nothing about how options are resolved changes: the merge is unchanged, the
  values are unchanged, and every existing directive behaves exactly as before.
  What is new is that the answer can be handed to something else, so that work
  done on behalf of one configuration is never reused for a request that
  resolved to a different one.
  Two properties are worth stating because deployments depend on them. The
  description is *canonical*: setting the same options in a different order, or
  through a different mechanism, produces byte-identical output and therefore
  the same signature. And it is *stable across upgrades*: it contains no
  version number, build identifier, or timestamp, so upgrading does not silently
  change the signature of a configuration that has not changed — which would
  otherwise discard every piece of optimized output associated with it.
  Values belonging to options that are not safe to display — signing keys and
  the like — are represented by a hash of themselves rather than reproduced, so
  a configuration description separates two different secrets without
  containing either.
  Note for anyone building from source: this adds BoringSSL as a build
  dependency of the rewriter library on every platform, Windows included. It is
  unconditional on purpose — the alternative hashing path compiles out under
  some supported build configurations and would produce a constant instead of a
  signature, which would silently merge every configuration into one.

### Fixed

- **A converted image is no longer served with the origin's media type.** When
  a request was answered from the optimizer daemon's cache with a variant in a
  different image format from the one the origin sent, the response carried the
  origin's `Content-Type`: AVIF bytes labelled `image/png`, WebP bytes labelled
  `image/png`. The negotiation itself was right — the format served was one
  the request had advertised — but a client decodes by the declared media
  type, so the response fails in the consumer: a shared cache or CDN stores the
  mislabelling and repeats it to everyone, and any client that trusts the
  declared type rather than sniffing the bytes cannot use the response. The
  media type now describes the bytes actually being sent. A response whose
  format did not change, and any format outside the four this path can
  produce, keeps the origin's own field exactly as before. **Updating is
  recommended for any deployment serving from the daemon's cache with image
  format conversion in play.**

- **A 304 now states the same `Vary` as the 200 it revalidates.** A response
  served from the optimizer daemon's cache and the `304 Not Modified` that
  revalidated it could disagree about which request headers the response varies
  on: the 200 listed `Accept-Encoding` and the 304 listed nothing, or listed
  `Accept` alone. A cache updates its stored headers from the 304, so it was
  left believing the response varies on a narrower set than the one it had
  keyed on — and could then reuse a stored copy for a request whose encoding
  capabilities differ from the one it was stored for. The two responses now
  state the same set. Nothing about a 200 changes.

- **The classic in-place path's `304` states the same `Vary` as its `200`
  too.** The same disagreement existed on the classic in-place serve path,
  from a different emitter: there the 200's `Vary: Accept-Encoding` token is
  stamped by the serving chain's compressor as the optimized resource is
  written out, and the compressor states nothing on a 304 — a bodyless
  response never reaches the line that names the axis. A cache that
  revalidated a stored in-place resource was told it no longer varies on
  encoding, the narrowing direction that can let one client's stored copy be
  reused for a request with different encoding capabilities. The `304` now
  states the encoding axis for the media types the module's own compressor
  predicate selects, and states nothing for the types it does not. Nothing
  about a `200` changes. **Updating is recommended for deployments with a
  shared cache or CDN in front of a server using in-place optimization.**

- **The by-id variant retry can no longer name a sentinel entry.** The serve
  arm's recovery path after a format refusal asks the shared cache for a
  variant by explicit id, derived from the request's capability mask. A mask
  whose viewport field carried the peer's reserved value would have folded
  that id onto the durable original's, serving origin bytes as an optimized
  variant — with the negotiated `Vary: Accept` and the wrong serve class. No
  classifier produces that mask today, which is exactly why the retry refuses
  sentinel ids by name rather than relying on reachability.

- **AVIF responses are no longer served to clients that did not ask for AVIF.**
  Two separate cache paths could hand an AVIF representation to a request that
  never advertised `image/avif`: a cached response an origin had selected by
  content negotiation, and an optimized AVIF resource reused from a warm cache
  for a later, differently-capable client. On both paths the affected client
  received an image it cannot decode — a broken image rather than a slower one.
  Because the representation is chosen once and then cached, one capable
  client's request could decide what later clients got for the cached lifetime
  of that resource, including through a shared cache or CDN. Both paths now
  check the request's own `Accept` header before reusing an AVIF entry, which
  is what the equivalent WebP paths already did (one of them since
  1.15.0+r22); AVIF now
  behaves identically. No other format changes behaviour, and no response gains
  a `Vary` header it did not already have. **Updating is recommended for any
  deployment that serves AVIF.**

## [1.15.0+r22] - 2026-08-08

### Changed

- **Optimized `.pagespeed.` resources now declare `Cache-Control: public,
  immutable`.** The URL of a rewritten resource embeds a hash of its content —
  when the content changes, the URL changes — so the response behind a given
  URL can never change. The one-year `max-age` these resources have always
  carried now comes with an explicit `public` and the standard `immutable`
  directive (RFC 8246): browsers that support it (Firefox and Safari) skip
  revalidating these resources even on a user-triggered reload — the case
  where browsers otherwise revalidate every resource on the page despite a
  valid freshness lifetime — and the explicit `public` makes the responses
  cacheable on CDNs that require it (such as Google Cloud CDN) without extra
  configuration. Other browsers and caches ignore the new directives, so
  behavior there is unchanged. The upgrade is applied only to responses that
  were already publicly cacheable: a resource derived from any `private`,
  `no-cache`, or `no-store` input keeps its restricted caching exactly as
  before, and responses served under a non-matching URL (for example after a
  stale link) keep their existing short, private lifetime.

- **In-place optimization no longer varies its output by request header.** When
  PageSpeed optimizes an image at its original URL (rather than at a rewritten
  `.pagespeed.` URL), it now optimizes that image identically for every client
  and serves the same bytes to all of them. It may still convert between
  formats where the target is universally supported — a photographic PNG still
  becomes a JPEG when `convert_png_to_jpeg` is enabled — but never based on who
  is asking; the request-dependent targets (WebP, AVIF) are no longer chosen in
  place. These responses no longer carry a `Vary: Accept` or `Vary: User-Agent`
  header, and are no longer downgraded to `Cache-Control: private` for Internet
  Explorer, so they are plainly cacheable by browsers, proxies and CDNs with no
  special configuration. Conversion to WebP and AVIF is unchanged on rewritten
  URLs, where the chosen format is part of the URL itself.

### Fixed

- **The JS minifier no longer emits unparseable output for valid JavaScript
  when a comment sits between `let` and its binding.** The tokenizer's `let`
  declaration lookahead skipped whitespace but not comments, so `let` in
  `let /*c*/ row …` (or `let//…` with the binding on the next line) was
  misread as an identifier. On that reading a linebreak after the binding
  looked droppable — but the output re-parses with `let` as a declaration
  keyword, where the linebreak can be required: `let /*c*/ row\n+4;` was
  minified to `let row+4;`, a syntax error that breaks the entire script.
  The lookahead now skips `//` and `/*…*/` comments too, so the declaration
  is recognized and the linebreak is preserved. Minifier output for the
  full differential corpus (including the third-party originals) is
  byte-identical to before — the change only repairs the misread shapes.

- **The JS minifier no longer fuses adjacent operator tokens into a different
  operator when it drops whitespace on unparseable input.** When the minifier
  removes whitespace or comments between two operator characters, the pair
  could re-lex as a single, different operator — `= =` became `==`,
  `& =` became `&=`, `. 0` became the number `.0`, and a dropped comment
  could glue tokens across the gap (`var d = /*c*/ > x` became `var d=>x`).
  Such pairs can only be adjacent in JavaScript that is already unparseable,
  so no valid page changes behavior: minifier output for valid JavaScript is
  byte-identical to before (verified against the full differential corpus,
  including the third-party originals). What changes is that minified output
  for broken input no longer silently turns two operators into one. The
  minifier's decline pass-through (used for constructs it cannot model) also
  no longer fuses the minified prefix with the unmodified remainder at the
  seam when both sides are word characters.

- **Optimized HTML no longer carries an `s-maxage` directive inviting shared
  caches to store it.** Optimized pages are served with
  `Cache-Control: max-age=0, no-cache`, but an `s-maxage` value could survive
  alongside it — inherited from the origin response, or from the short
  shared-cache window PageSpeed itself adds while a resource awaits
  optimization. Because `s-maxage` overrides `max-age` for shared caches, a
  CDN or proxy could briefly hold and re-serve one visitor's optimized page
  to other visitors. The page can embed image URLs whose format was chosen
  for the original requester's browser, so in setups with a shared cache in
  front this could intermittently serve images in a format the receiving
  browser cannot display. Deployments using the downstream-cache integration
  (`DownstreamCachePurgeLocationPrefix` and related directives) are
  unaffected: that feature intentionally preserves the origin's
  `Cache-Control` on optimized HTML, and continues to. The same applies with
  `ModifyCachingHeaders off`, which disables all of PageSpeed's caching-header
  rewriting including this fix. `s-maxage` handling on non-HTML resources is
  unchanged. If you operate a shared cache in front of mod_pagespeed, an
  update is recommended.

- **Safari 16+ and Firefox 132+ now receive WebP on rewritten image URLs.**
  Image rewriting chooses an image's output format from the `Accept` header the
  browser sent with the page request. Safari has never listed image formats in
  that header, and Firefox stopped doing so in version 132, so neither browser
  was offered WebP and both were served the original format instead — a JPEG
  where Chrome received a WebP or an AVIF. Nothing reported an error; the only
  visible symptom was that pages weighed more in those browsers than they
  needed to. Both are now recognised as WebP-capable from the browser
  identification they send: Safari 16 and later, Firefox 132 and later. On one
  representative photograph, an affected browser previously transferred 554 KB
  where Chrome transferred 127 KB; it now receives the WebP at 292 KB. Real
  pages will vary, but the saving applies to every photographic image on the
  page. The floor is Safari 16 deliberately: whether Safari 14 and 15 can
  decode WebP depends on the version of macOS under them, which the browser
  identification cannot reveal, and serving WebP to the one combination that
  cannot decode it would render broken images — Safari 16 is the first version
  free of that ambiguity. There is nothing to configure and no cache to clear.
  Newly eligible browsers begin requesting a format that may not have been
  produced yet, so expect a short warm-up during which they are served the
  original image while the WebP is generated in the background — the same
  behaviour as any cold cache. Existing optimized images stay valid and are not
  regenerated. AVIF is unaffected and continues to be offered only to browsers
  that ask for it by name, so these browsers receive WebP rather than AVIF
 .

### Upgrade Notes

- **In-place optimization: retired directives and one output change.** The
  `in_place_optimize_for_browser` filter and the `AllowVaryOn` and
  `PrivateNotVaryForIE` directives are retired. They are still accepted so that
  an existing configuration keeps loading — you will see a warning in the error
  log — but they no longer have any effect and should be removed. If you enabled
  `in_place_optimize_for_browser` (directly, or through the
  `OptimizeForBandwidth` rewrite level), images served at their original URLs
  will now be recompressed in place rather than converted to WebP or AVIF, so
  those particular responses get larger. Sites whose HTML PageSpeed rewrites are
  unaffected: the smaller WebP and AVIF variants continue to be served from the
  rewritten URLs the HTML points at. If you had configured a reverse proxy, CDN
  or Varnish instance to handle `Vary: Accept` or `Vary: User-Agent` on
  PageSpeed image responses, that configuration is no longer needed. One
  behavioral note: configurations that used `AllowVaryOn "None"` or
  `AllowVaryOn "Accept"` to keep the `...QualityForSaveData` settings from
  being applied no longer get that suppression — the Save-Data qualities are
  now used on rewritten URLs whenever they are configured; unset them if you
  do not want them. Expect a one-time re-optimization pass after upgrading.

## [1.15.0+r21] - 2026-08-01

### Added

- Module scripts (`<script type="module">`) are hinted again: collected module
  dependencies are emitted as `rel=modulepreload` in the `Link` response
  header. Modules carrying `integrity` or `crossorigin="use-credentials"` are
  left unhinted. Mixed-version deployments sharing a cache degrade cleanly
 .
- nginx: new server-scope directive `WebBotAuthBotDetection` (default off).
  When enabled, a cryptographically verified Web Bot Auth signature (RFC 9421)
  classifies the request as an automated client regardless of its user-agent
  string. With it off — the default — Web Bot Auth verification remains
  observe-only, exactly as before.

### Removed

- The legacy JavaScript minifier. The tokenizer-based minifier (the default
  since 1.10.33.0) is now the only JavaScript minifier.
  `UseExperimentalJsMinifier` remains accepted on every port, is ignored, and
  logs a deprecation warning; configurations carrying it start normally.
  Sites that had explicitly set it re-optimize their JavaScript once in the
  background after upgrading; sites that ran the legacy minifier also gain
  module minification and source-map support.

### Changed

- The known automated-client list used for optimization decisions has been
  brought up to date (it predated the current generation of AI assistant
  fetchers, HTTP client libraries, and command-line tools), and token matching
  now recognizes parenthesized user-agent comment forms. `curl` and `wget`
  are now classified as automated clients.
- `defer_javascript` (and the filters sharing its gate: `disable_javascript`,
  `defer_iframe`, `fix_reflow`, `support_noscript`) no longer applies to
  automated clients, including search-engine crawlers: they receive the page's
  normal authored script markup instead of deferred markup that only
  PageSpeed's client-side runtime can execute.
- WebP support is now determined from the browser's `Accept` request header
  alone, instead of from hand-maintained lists of browser version strings. A
  browser that advertises WebP is taken to support every flavour of it, as AVIF
  already was; browsers that do not advertise WebP are unaffected. This also
  fixes browsers whose major version number reached three digits (current
  Chrome, Edge and Opera, and Chrome on iOS) being read as incapable of
  animated WebP, and Chrome on iOS losing lossless/alpha WebP. Sites using
  `convert_to_webp_animated`, `convert_to_webp_lossless` or
  `in_place_optimize_for_browser` should expect a one-time re-optimization pass
  after upgrading.
  No purge or manual invalidation of a downstream or CDN cache is required:
  where an image's optimized output changes, it is published under a new
  rewritten URL that the HTML is updated to point at, while previously
  rewritten URLs keep resolving and age out normally.

### Fixed

- JavaScript minification now makes the correct regex-versus-division decision
  after `await` and after the `of` of a `for...of` loop: a regular-expression
  literal in that position keeps its interior spacing instead of being
  rewritten into a different pattern (e.g. `return await /a +b/.test(s)`),
  matching the existing `yield` behavior. Line breaks after these words are
  preserved whenever removing one could change how the script re-parses.
- JavaScript minification can no longer assemble a comment delimiter that was
  not in the input: deleting whitespace no longer welds a division or
  regex-closing slash onto a following `*` (forming `/*` and silently
  commenting out the rest of the script), and the same guard now covers
  retained IE conditional-compilation comments next to a slash. Scripts in
  which `await` or `yield` may really be plain variable names and a safe
  rewrite cannot be guaranteed are now declined — served byte-for-byte
  unchanged — instead of minified wrongly.
- JavaScript minification could corrupt a script in which a line break
  separates a postfix `++`/`--` from a next statement that begins with an
  opening parenthesis, or with a leading-dot number such as `.5`. That line
  break is what keeps the two statements apart — without it the code re-parses
  as a call or member access on the value just incremented, which the browser
  rejects as a syntax error — but the minifier removed it and reported
  success, so the script was served broken with nothing logged. Such line
  breaks are now preserved (including when carried inside a comment). Line
  breaks that a following binary operator genuinely continues are still
  removed, and already-correct minified output is byte-for-byte unchanged.
- A stray `;` after a rule inside an `@media` block no longer makes the whole
  stylesheet fall back to its original bytes: such sheets now minify, combine,
  and participate in `prioritize_critical_css` like any other stylesheet.
  Sheets that still fail to parse are served byte-for-byte unchanged, as
  before.
- JavaScript minification no longer merges a division operator into a retained
  IE conditional-compilation comment (`/*@ ... @*/`). The `/` and the comment's
  opening `/*` could fuse into `//`, turning the rest of the line into a
  comment and silently changing what the script computes. A separating space
  is now kept whenever the two would otherwise join, and the equivalent hazard
  after such a comment is guarded the same way.
- JavaScript minification no longer removes the space between a bare `0`
  literal and a following property access (`0 .toString()`). Removing it made
  the period parse as a decimal point, turning valid code into a script that
  fails to parse. Other numeric literals are unaffected.

## [1.15.0+r20] - 2026-07-23

### Changed

- Pages that set a strict Content-Security-Policy `base-uri` policy blocking all
  `<base>` elements (e.g. `base-uri 'none'`) now retain more optimization. When
  the policy guarantees the browser will ignore a `<base>` tag, the optimizer no
  longer conservatively disables rewriting on that page; other `base-uri`
  policies remain handled conservatively.

### Fixed

- License management in the admin console is now offered based on the server's
  authoritative global-admin determination. Operators who serve the global
  admin console at a custom (renamed) path can now purchase, apply, and activate
  licenses instead of finding those controls unexpectedly hidden.
- Histogram percentile stats (median/90/95/99) for very-small sample counts are
  now omitted instead of shown as a -5000 no-data placeholder in the admin
  console and `/histograms` output.
- **IIS: unified `pagespeed.config` resolution to a single documented source of
  truth.** The IIS module previously resolved its configuration through more
  than one independent code path, so editing one of the installed config files
  could appear to have no effect. Resolution now follows one documented
  precedence chain, shared by config load, change-detection, and the
  engage/serve gate; the module logs which file is in effect and warns when
  more than one config file exists with differing content. Behavior for a
  standard single site is unchanged.

### Upgrade Notes

- **IIS configuration resolution is now unified and documented — no files on
  your system were changed, deleted, moved, or overwritten by this upgrade.**
  IIS installs ship a `pagespeed.config` in two places: a machine-global
  default under `%ProgramData%\We-Amp\PageSpeed\`, and a per-site copy in each
  site's web root. Both are preserved across upgrades (they always have been).
  What changed is that the resolution order is now one documented rule:

  1. `%ProgramData%\We-Amp\PageSpeed\pagespeed.config` — machine-global base default
  2. `%ProgramData%\We-Amp\IISWebSpeed\pagespeed.config` — legacy fallback (upgrade-from-IISpeed installs only)
  3. `<site physical path>\pagespeed.config` — per-site override, **authoritative when present**

  The per-site file in the site's web root takes precedence; the `%ProgramData%`
  locations act as the base layer underneath it. If you previously edited one
  copy and did not see the expected effect, re-check which file you edited — for
  a standard single site, edit the copy in that site's web root. On startup the
  module now logs the file it resolved as effective and warns when a second
  config with different content is present, so any earlier ambiguity about which
  file applies is now visible in the log.

## [1.15.0] - 2026-06-01

### Changed

- **Version line renumbered 1.1 -> 1.15**. mod_pagespeed 1.15 is the
  maintained continuation of the Apache-lineage codebase and the direct successor
  to Google's final mod_pagespeed release (1.14.36.1); the prior "1.1" numbering
  read as older than Google's line on package/version surfaces. This is a
  version-identity change with no functional change from the 1.1.0 line. Package
  names (`mod-pagespeed`, `nginx-module-pagespeed`, `ea-apache24-mod_pagespeed`)
  and the runtime behavior are unchanged; the `X-Mod-Pagespeed` header now reports
  `1.15.0.0`.

## [1.1.0-beta.1] - 2026-02-25

First release under We-Amp stewardship. This is a complete modernization of
the mod_pagespeed codebase, expanding from Apache-only to four deployment
platforms while preserving the full set of 40+ optimization filters.

### Added

#### New Platforms
- **Envoy filter** (experimental): PageSpeed as an Envoy HTTP filter with IPRO
  and HTML rewriting support. Uses `CurlUrlAsyncFetcher` (linked against
  BoringSSL) for independent outbound fetching. Includes standalone binary
  (~212MB) and shared library (~113MB).
- **nginx module** (stable): Dynamic module (`ngx_pagespeed_module.so`) for
  nginx 1.26.x stable and 1.27.x mainline. Full IPRO and HTML rewriting.
- **IIS module** (experimental): Native C++ IIS module (`pagespeed_iis.dll`)
  for Windows Server. Supports IPRO, HTML rewriting, streaming responses with
  chunked transfer encoding, and the full admin UI.

#### Build System
- **Bazel 7.x build system** replacing the legacy GYP/gyp_chromium toolchain.
  WORKSPACE-based dependencies with bzlmod disabled.
- **Docker development environment** with `docker-compose.yml` providing
  dev container, Redis, and Memcached services.
- **Clang + GCC 13 libstdc++** build configuration (`--config=clang-libstdcxx13`)
  for C++20/C++23 support.
- **Sanitizer configs**: `--config=clang-asan` (AddressSanitizer) and
  `--config=clang-tsan` (ThreadSanitizer).
- **Windows cross-compilation** via `--config=windows --config=clang-cl`
  using clang-cl toolchain.
- **GitHub Actions CI/CD** replacing Travis CI, with workflows for unit tests,
  Apache/Nginx/Envoy system tests, and automated release packaging.

#### Core Improvements
- **C++20 standard** (C++23 for Cyclone cache files via `per_file_copt`).
- **Cyclone cache** integration for high-performance disk caching.
- **CurlUrlAsyncFetcher** replacing Serf for HTTP fetching, linked against
  BoringSSL for HTTPS support.
- **APR decoupling**: Core libraries no longer depend on Apache Portable Runtime
  for non-Apache deployments.
- **libcurl linked against BoringSSL** on Linux (same instance used by Envoy,
  no symbol conflicts).

#### Testing
- **Python test framework** (`test/system/pagespeed_test_framework/`) shared
  across all four platforms with `fetch_until` polling, statistics delta
  checking, and WebP negotiation helpers.
- **Comprehensive system test suites**:
  - Apache: 195 pass, 14 skip
  - Nginx: 169 pass, 40 skip
  - Envoy: 189 pass, 20 skip
  - IIS: 350 pass (Python) + 19 pass (C++ unit)
- **pytest markers** for test categorization: `@pytest.mark.ipro`,
  `@pytest.mark.html_rewrite`, `@pytest.mark.slow`, etc.

#### Envoy-Specific
- Admin authentication with token, IP allowlist, and rate limiting.
- Circuit breaker for resource fetching.
- Redis cache backend support.
- Prometheus metrics endpoint at `/stats/prometheus`.
- Health endpoint at `/pagespeed/health`.

#### IIS-Specific
- `web.config` XML configuration parsing.
- Windows shared memory (`WindowsSharedMem`) for cross-process state.
- BCrypt API for cryptographic operations.
- Performance counter integration with Windows `perfmon.exe`.
- IIS Express support for development and testing.

### Changed

- **Version scheme**: Moved from Google's `1.15.0.0` four-part versioning to
  semantic versioning (`1.1.0-beta.1`).
- **Branding**: Updated from Google Inc. to We-Amp.
- **Apache 2.2 support dropped**: Only Apache 2.4+ is supported (single `.so`).
- **Build default paths**: Packaging scripts updated from GYP `out/Release/`
  to Bazel `bazel-bin/pagespeed/` paths.
- **`version.h` generation**: Now includes git short hash as `LASTCHANGE` and
  supports `PRERELEASE` field for pre-release versions.

### Removed

- GYP/gyp_chromium build system.
- Apache 2.2 module (`mod_pagespeed_ap24.so` split).
- Google-hosted update repository integration (cron jobs retained but
  repository config cleared).
- Travis CI configuration (replaced by GitHub Actions).

### Known Issues

#### Envoy (Experimental)
- `combine_css` may timeout due to async worker pool coordination.
- IPRO cache lifetime returns upstream `max-age` minus cache time instead of
  `implicit_cache_ttl_ms`.
- `X-PSA-Blocking-Rewrite` header is not supported (Envoy is non-blocking).
- Version header shows placeholders when built without workspace status.

#### IIS (Experimental)
- IPRO async cache stubs return misses (Redis/disk caches not functional).
- Beacon data silently discarded.
- Requires `allowDoubleEscaping="true"` in IIS for combined resource URLs.

## Previous Releases (Google Era)

For releases prior to We-Amp stewardship, see the
[Google mod_pagespeed release notes](https://www.modpagespeed.com/doc/release_notes).

The last official Google release was **1.14.36.1** (August 2020).
Version 1.15.0.0 was assigned in December 2018 but never released.

[1.1.0-beta.1]: https://github.com/we-amp/mod_pagespeed/releases/tag/v1.1.0-beta.1
