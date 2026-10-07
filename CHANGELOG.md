# Changelog

All notable changes to mod_pagespeed are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [2.2.0] - 2026-10-06

### Security

- **nginx: certain requests could make a worker process exit (denial of
  service).** An unauthenticated request could cause an nginx worker process
  to exit; nginx restarts the worker, but requests it was serving are lost.
  Affects the nginx module in releases up to and including 1.16.0. **Update
  recommended.**
- **Apache: certain requests could make a child process exit (denial of
  service).** A request could cause an Apache child process to exit; Apache
  replaces it, but requests it was serving are lost. Affects releases up to
  and including 1.16.0. **Update recommended.**
- **Information disclosure between sites on per-host admin consoles.** A
  per-host admin console's cache cooldown list could include entries that
  belong to other sites on the same server. A per-host console now lists
  only its own site's entries; the full list stays on the whole-server
  console. This matters where per-host consoles are given to different
  people. On IIS and Envoy a per-host console lists no cooldowns for now.
  Affects 1.16.0 with the optimizer in use. **Update recommended.**
- **HTML rewriter: some pages could make a server worker process exit (denial
  of service).** Some pages served through the rewriter could make the server
  worker process handling them exit, losing the requests it was serving.
  Affects releases 1.15.0+r20 through 1.16.0. **Update recommended.**
- **nginx: access restrictions on the admin, statistics, console and message
  pages.** This release addresses an access-restriction bypass affecting these
  pages in the nginx module and updates the access-restriction example in the
  packaged snippet and the documentation. Affects the nginx module in releases
  up to and including 1.16.0 where access to these paths is restricted in
  the nginx configuration. **Update recommended.** After updating, replace your
  access rules for these paths with the updated example, in every `server`
  block; rules already in your configuration are not changed by the update.

- **nginx: cache integrity in in-place resource optimization.** A request
  could affect what in-place optimization stores and serves to other visitors.
  Ordinary page and resource requests behave as before, and the admin pages,
  beacons, cache purge, `.pagespeed.` resources and HTML rewriting are not
  affected. Affects the nginx module in releases up to and including 1.16.0
  with in-place optimization enabled (the default). **Update recommended.**

- **nginx: information disclosure in in-place resource optimization.** In some
  nginx configurations, in-place optimization could make content available
  that the configuration does not expose directly. Affects the nginx module in
  releases up to and including 1.16.0 with in-place optimization enabled (the
  default). **Update recommended**, and flush the PageSpeed cache once after
  updating (touch `cache.flush` in the `FileCachePath` directory, or purge `*`
  when `EnableCachePurge` is on) so that entries recorded by earlier releases
  are dropped.

- **Admin console hardening.** Actions in the admin console can no longer be
  triggered from other sites, and admin responses are hardened against use
  from other sites. Affects releases up to and including 1.16.0 where the
  admin console is reachable beyond loopback. **Update recommended.** Scripted
  purges and some monitoring probes need a change: see the breaking-change
  notes under Changed and the documentation.

- **The bundled HTTPS fetch library is updated to curl 8.22.0.** This
  addresses nine curl vulnerabilities published on 2026-09-02
  (CVE-2026-19931, CVE-2026-18924, CVE-2026-82209, CVE-2026-80229,
  CVE-2026-80230, CVE-2026-80231, CVE-2026-80255, CVE-2026-82208 and
  CVE-2026-13608), every one of which curl's own vulnerability database
  records as fixed in 8.22.0. No exploitation is known; update recommended.

### Fixed

- Envoy: hardened the filter's request-header handling.
- With the optimizer in use, a stylesheet or script whose optimized copy had
  gone missing from the optimizer's cache was served unoptimized until the
  URL was purged. On Apache and nginx the module now notices when it serves
  the stored original of such a URL and asks the optimizer to optimize it
  again, about once per URL every 10 seconds per server process; the
  response itself is unchanged, and content the optimizer leaves
  unoptimized on purpose is not asked for. On IIS nothing is asked for yet.
  This takes effect with an optimizer that includes the matching fix, which
  also optimizes such a URL again on its own when the stored original
  expires; an older optimizer ignores the request. When the optimizer runs
  with gzip compression turned off, the module cannot recognise such a URL
  and does not ask; it is optimized again when its stored original expires.
  Images are not covered. Two statistics count the requests:
  `ipro_daemon_heal_notified` and `ipro_daemon_heal_notify_failed`.
- With the optimizer in use, purging the optimizer's whole cache left the
  web server on the deleted cache file: nothing was optimized any more, and
  copies stored before the purge could still be served, until the web
  server was restarted. The module now notices within a second that the
  optimizer replaced its cache, stops using the old file at once (requests
  are answered by the origin meanwhile) and opens the new one, at the cache
  size the optimizer uses now. In the ordinary case no restart is needed. On
  IIS a full purge still needs an application-pool recycle: Windows usually
  cannot delete a cache file that worker processes hold open. A worker
  process that starts
  after the optimizer was given another cache size now opens the cache at
  that size too, instead of the size the server read when it started. A
  cache file kept from an earlier cache format is left alone and does not
  get in the way. Whenever the module cannot be
  sure it is on the optimizer's current cache file it does not use the
  cache. Before every open it checks that the optimizer's cache file is
  there, and afterwards that the open did not create one; if it did, or a
  file changed during the open, that worker process logs one error naming
  the files, stops using the optimizer's cache until it is recycled, and
  leaves every file in place (the module deletes nothing). A worker process
  also stops using the cache until it is recycled, or the web server is
  restarted, in these cases: a second full purge lands in the few
  milliseconds in which it is opening the cache after the first; the
  optimizer stays unavailable after a purge for longer than the usual
  retries; two cache files of the current format are present after a purge;
  or it has followed four full purges (below). The old file stays open in
  each worker process until that process ends, because a response still
  being recorded may use it: every full purge therefore keeps one more
  cache file's worth of disk space in use on the host, and of address
  space in each worker process for each virtual host that uses the
  optimizer, until the worker processes are recycled. A virtual host in a
  worker process follows four full purges this way. At the fifth it stops
  using the optimizer's cache in that process (requests are answered by the
  origin) and logs one error; at that point up to five times the configured
  cache size of disk is held, and as much address space per virtual host
  in each worker process. Reloading or restarting the web server releases
  it. Sites that purge the whole cache routinely should reload the web
  server along with it.
- With the optimizer in use, an optimized copy the optimizer wrote at the
  same moment the module recorded the same URL's original could be dropped
  from the shared cache without any error, and the URL was then served
  unoptimized. The bundled cache library now notices that another process
  changed the entry between a write's two steps and redoes the write. The
  optimizer needs the matching version for its own writes: a process still
  on the older library can drop a copy the same way, so update the module
  and the optimizer together. The cache's on-disk format is unchanged and
  nothing needs to be configured. The same library update stops every open
  cache from starting a background monitor thread and an idle worker
  thread that never had work, closes a moment right after an entry was
  recorded again in which the in-memory tier could briefly serve the
  entry's previous version, and makes it safe for a process to exit while
  a cache is still open.
- **Apache and nginx: a worker process could hang right after it started
  and keep its slot.** The server's parent process keeps the module's file
  cache open, and a worker process created at an unlucky moment could start
  with one of the cache's internal locks already taken. Such a worker
  stopped at its first cache lookup that needed that lock. On Apache that
  can be during the worker's own start-up, and then it never served a
  request. It was most visible after `apache2ctl graceful` (which log
  rotation runs): the stuck worker stayed behind, and later graceful
  restarts did not remove it. A full restart of the web server cleared it.
  A worker could also stop the same way while it was shutting down. The
  bundled cache library now finishes its background work before a worker
  process is created. Affects the Apache and nginx modules in 1.15.0 and
  1.16.0. Update recommended.
  The same library update makes it safe to stop the file cache while other
  threads are still reading from or writing to it, which in the module
  happens only while a worker process shuts down; a write that comes too
  late is dropped. It also fixes a crash at exit in builds that hash cache
  keys with OpenSSL; the module's builds use the hash of their bundled TLS
  library and were not affected. The cache's on-disk format is unchanged,
  the existing cache is kept, and nothing needs to be configured. IIS and
  Envoy are not affected.
- `prioritize_critical_css` no longer trusts what browsers reported when it
  does not describe the page any more. After a deploy that adds rules, the
  new rules are kept in the inline block until a browser has reported on
  them, and the page's stylesheets stay blocking until the first such
  report arrives (a few page views); a report that was requested before the
  change is ignored. A report that had to be cut short because the page has
  more matching selectors than one report can carry is no longer taken for
  a complete one: such a page keeps blocking stylesheets until a complete
  report arrives (see the `beacon_overflow_count` statistic). Previously
  rules could be missing from the first paint in both cases until the full
  stylesheet arrived. After a page's markup changes without its stylesheets
  changing, rules that newly apply can be late until a visitor's browser
  reports on the new markup; the module now asks for a report again after
  about a minute by default (twelve `BeaconReinstrumentTimeSec`, it was a
  hundred), and at once after a report that shows a rule newly applying;
  when the visitor who is asked does not report, the wait is longer.
  Reports collected by earlier releases are not reused, so after updating
  each page is optimized again once its visitors' browsers have reported.
  Browsers are no longer asked about the selectors of inline `<style>`
  blocks, which the filter leaves as they are: a page whose inline blocks
  differ from one response to the next (generated class names) is therefore
  optimized like any other, and a page with inline blocks only is not
  instrumented.

- **Apache: reports from phones and tablets are used.** The module keeps
  what browsers report for `prioritize_critical_css` and for the filters
  that use critical-image reports (such as `prioritize_critical_images`)
  per device class: phone, tablet and desktop. On Apache every page view was
  treated as a desktop one when that data was looked up, while a report was
  filed under the class of the browser that sent it. Reports from phones and
  tablets were therefore ignored, and those visitors were served
  critical-CSS and critical-image data computed from desktop browsers; a
  site visited mostly from phones could stay on blocking stylesheets for a
  long time, until a desktop browser happened to report. Page views are now
  looked up under the visitor's own device class, as on nginx, IIS and
  Envoy, which were not affected. After updating, an existing Apache site
  sees phones and tablets warm up once: their pages get ordinary stylesheets
  and ordinary image handling until the first report from a browser of that
  device class arrives, normally within a few page views. Desktop page views
  are not affected, and no page is shown without its styles at any point.

- **A refused optimizer cache now shows in the admin console's message
  history.** When the web server starts with in-place optimization off
  because it cannot use the optimizer daemon's cache (for example after an
  upgrade left the two cache layouts out of step), Apache and nginx logged
  the reason only to the web server's error log: the check runs before the
  message history exists. Each serving process now repeats the reason once,
  as a warning, where the admin console can see it.

- **CSS parser diagnostics for unsupported modern syntax no longer flood the
  web server's error log at the default log level.** The legacy parser emits
  one verbose note per declaration it cannot interpret (for example
  `color-mix()`, `calc(var(--x))` or `margin-block-*` values); those notes
  were reported as ordinary info-level messages, so a site using modern CSS
  could generate thousands of error-log lines per day even at `LogLevel
  warn`. Verbose diagnostics now appear at debug level only, and only where the server provides a log sink for them.

- **A resource fetch that times out, cannot connect or is answered 5xx is
  remembered for ten seconds, not five minutes.** When the module could not
  fetch the original of a `.pagespeed.` resource because the origin stalled,
  the failure was recorded like a missing file: a made-up 404 from the fetcher
  counted as a real one, so one five-second stall turned into a five-minute
  404 for that resource (and the in-place optimizer remembered a 5xx for as
  long). Such failures describe the origin's condition at that moment, not
  the resource, and are now retried after ten seconds. A 404 or 410 the
  origin actually sent is still remembered for `MetadataInputErrorsCacheTtlMs`
  (five minutes by default).

- **IIS: the user-mode cache time-to-live for optimized resources is now
  applied.** It was left unset while the kernel-mode value was overwritten.

- **Apache: option response headers such as `PageSpeed: off` no longer reach
  the client when they switch optimization off for a response.** They are a
  server-side control channel.

- **nginx: option response headers such as `PageSpeed: off` no longer reach
  the client.** They are a server-side control channel.

- **Remote configuration is applied only from a successful response.** A
  `RemoteConfigurationUrl` answered with any status other than 200 (or a 304
  revalidating the cached copy), for example 403 or 410, is no longer applied,
  even when its body is a valid configuration.

- **Remote configuration keeps its last good copy when a refetch fails.**
  Once the cached configuration expires, a refetch that fails (an error
  status or an unreachable server) no longer drops it: the last good copy
  keeps applying, for up to one day past its expiry, until a refetch
  succeeds. `ServeStaleIfFetchError off` turns this off.

- **The console's Graphs page no longer re-requests its data continuously.** It re-armed its own poll after every response and issued hundreds of requests per second for as long as the page was open; it now polls every 5 seconds. Update recommended where the console's Graphs page is used.
- **The nginx module's message-history endpoint returns the same JSON as the
  Apache and Envoy modules** instead of an HTML page.

- **IIS: the message-history endpoint is served, and IPv6 loopback counts as
  local for the admin pages.**

- **The admin console's daemon panels are reached only as
  `v1/daemon/<endpoint>` directly under the admin path, and a daemon endpoint
  can never reach the module's own admin pages.** The admin path may be
  mounted at any depth (for example `/alt/admin/path`).

- **The admin console's daemon panels now say why the optimizer is
  unavailable** — not configured, unreachable, or an optimizer version
  without the endpoint — instead of one generic message.

- **The global admin console's graphs and console pages now plot the
  aggregate, not the serving vhost.**

- **Apache: the module warns at startup when a virtual host cannot get its
  own statistics** (per-vhost statistics enabled, but the host carries no
  ModPagespeed directive) and says how to fix it.

- **IIS: the installer's permission grants on the cache and logs directories
  now take effect.** The installer is meant to give the IIS worker processes
  (`IIS_IUSRS`, and `NETWORK SERVICE` for older application pool setups)
  modify rights on `C:\ProgramData\We-Amp\PageSpeed\cache` and read and
  write rights on its `logs` directory. Those grants failed silently on every
  install, so both directories kept only the rights they inherit from
  `C:\ProgramData`. Most installs worked anyway, because an application pool
  identity can create files there through the inherited rights; an install
  where those inherited rights had been tightened got the
  `cache-path-not-writable` diagnostic page instead. Installing or repairing
  this release applies the grants.

- **Apache: a start refused because the module's cache volume did not match
  the optimizer daemon's no longer leaves its own file behind.** When opening
  the daemon's cache volume at the size the daemon publishes would create a
  second volume file, the server still refuses to start — and now removes the
  file that attempt created, or says what became of it: still there, not
  confirmed gone, or left in place because another user or process holds it
  (most likely the daemon's own new volume, which must not be removed).
  Before, the file always stayed, and the next start attached to it without
  refusing and ran on a separate cache the daemon never reads. If a start
  with 1.16.0 refused with "created a SECOND volume file (…)", stop the
  optimizer daemon, remove the file that message named, then start the daemon
  and then the server.
- **Apache with the optimizer daemon: stylesheets and scripts served from the
  daemon's variants no longer re-notify the daemon on every request.** Every
  such response was classed a fallback and asked the daemon again for a
  variant it already had: one notification per request on the request path,
  answered and logged by the daemon each time, and an
  `ipro_daemon_fallback_notified` counter that climbed with traffic instead of
  signalling real fallbacks. Responses were correct and do not change; images
  were not affected. Affects the Apache module with `DaemonSocketPath` and
  `DaemonVolumePath` set.
- **IIS: `UseEventLog on` writes to the Windows event log again.** In
  previous releases the directive parsed and did nothing: the module wrote
  no entry under the `PageSpeed` source at all. It now writes the module's
  warnings and errors there, one entry per distinct message per worker
  process and at most a thousand of them, with informational messages left
  out (they are per-request). A fatal is written whatever the directive
  says. With the directive off or absent, the first suppressed warning and
  the first suppressed error each write one entry saying so and where the
  messages can be read instead. Messages from before the directive's value
  is known in a worker process are not written; where the directive sits
  inside a host- or path-matched block, that covers the module's start-up
  messages, which the message history at `/pagespeed_message` carries as
  before. A real fault still reaches the event log: an unreachable
  `FileCachePath`, for instance, writes its error under the `PageSpeed`
  source, which is how an operator tells a quiet log apart from a silent
  one. The event source is not registered by the installer yet, so Event
  Viewer prefaces each entry with a note that it cannot find the
  description; the message itself is intact.

- **nginx: the module's filters run at their intended place in nginx's filter
  chain.** In previous releases the dynamically loaded module ran ahead of all
  of nginx's own output filters instead of immediately before compression.
  What that fixes:
  - An `expires` or `add_header Cache-Control` directive no longer overrides
    the caching headers of rewritten HTML (`Cache-Control: max-age=0,
    no-cache`) or of `.pagespeed.` resources (one year, immutable). Before, a
    location with `expires 1h` made rewritten HTML cacheable for an hour and
    cut optimized resources down to an hour.
  - Responses the module serves carry exactly one `Vary: Accept-Encoding`
    line (uncompressed responses carried two), and `add_header` values are no
    longer added to them a second time — a doubled
    `Access-Control-Allow-Origin` is rejected by browsers. A response whose
    own `Vary` lists `Accept-Encoding` among other tokens states that token
    once as well.
  - Content brought in by SSI includes, `sub_filter` and `addition` is
    optimized with the rest of the page; the module used to see the page
    before nginx expanded it.
  - The module's body filters no longer read static files synchronously on
    the event loop; nginx's copy filter delivers them in memory, as it does
    for its own filters.
  - Responses that in-place optimization is still working on carry
    `s-maxage` (`InPlaceSMaxAgeSec`, default 10) so that shared caches fetch
    them again once optimized; the old position kept this from taking effect.
  `If-None-Match` on `.pagespeed.` resources still answers `304`, and the
  `charset` directive still applies to rewritten HTML. The position applies
  to the dynamically loaded module, which is what the packages ship; a build
  that links the module into nginx statically keeps its previous position.

- **A server start or configuration test can no longer hang on the optimizer
  daemon's socket.** When the daemon directives are set, the module probes
  the daemon's notification socket while the server starts. The probe used a
  connect with no time limit, so a daemon that existed but was not accepting
  connections (stopped, or overloaded) could hold the start or a config test
  indefinitely. The probe now gives up after two seconds, reports that the
  daemon did not accept a connection, and the server starts with in-place
  optimization through the daemon off, as it already does for a daemon that
  is not running. A missing socket, a permission problem and a refused
  connection are reported at once, with the same messages as before. A
  daemon that does not accept costs those two seconds once per start, not
  once per virtual host: the timed-out answer is remembered per socket path
  for ten seconds.

- **Admin console text is readable in the light theme.** Secondary text
  (timestamps, hints, empty states, table headers) and warning and success
  text now meet the WCAG AA contrast ratio in both the light and the dark
  theme; before, several were well below it on white.

- **The Caches page cannot hang on an unusual cache description,** and it
  describes the cache layers correctly (write-through pairs, the shared-memory
  block size, disk-cache tiers, L1/L2 roles). Its views are proper tabs that
  work with the arrow keys, and on a server where cache purging is off the
  purge views are hidden and the page says how to turn purging on.

- **The Graphs page says why it is empty.** It used to report a missing
  graphs endpoint whenever it had nothing to draw. It now tells apart a server
  without graphs, a statistics log with no samples yet, and a log with no
  samples in the chosen range; on the global console it explains that with
  per-virtual-host statistics enabled the whole-server log stops receiving
  samples and links to this host's own console. With the per-interval view on,
  graph titles name the interval (for example "per 1 min interval").

- Graphs JSON reports a counter missing from a statistics-log segment as null
  instead of an invented 0; charts show a gap rather than a dip.

- With per-virtual-host statistics enabled, the whole-server statistics log
  keeps receiving samples, so the whole-server console's Graphs page has
  history again.

- **HTTPS resources that mod_pagespeed fetches from its own server no
  longer fail certificate verification.** For a resource on an origin that
  no domain directive (such as `ModPagespeedDomain`) names, mod_pagespeed
  connects to its own server's IP address and sends the site's name in the
  `Host` header. The serf-based fetcher of earlier releases sent that name
  as SNI and checked the certificate against it. The curl fetcher took both
  from the URL, that is, the IP address: the server presented its default
  certificate, verification failed with curl error 60, and the resource
  stayed unoptimized. When the URL names the server by IP address, the
  fetcher now takes the TLS server name from the `Host` header, as serf did,
  and still connects to that address. This also applies to an HTTPS origin
  that `MapOriginDomain` maps to an IP address. Such an origin must present
  a certificate for the site's name, the name sent in the `Host` header; a
  certificate that names only the IP address no longer passes. Fetches to a
  host name, and fetches through a proxy, are unchanged.

- **HTTPS fetches through a proxy work.** With `ModPagespeedFetchProxy`, or
  a proxy from the environment, an HTTPS fetch goes through a CONNECT
  tunnel. The fetcher took the proxy's answer to CONNECT for the origin's
  response headers and ignored the real ones, so every such fetch ended with
  status 0. The proxy's answer now stays with libcurl.

- **The source tree builds with Bazel when `VERSION` has no `PRERELEASE=`
  line.** A final release's `VERSION` file can leave that line out. The step
  that writes `version.h` then stopped with `PRERELEASE: unbound variable`,
  and the module did not build from source. A missing line now means a final
  release.

- **HTTPS fetches work with a certificate directory alone.** With
  `ModPagespeedSslCertDirectory` set and no `ModPagespeedSslCertFile`, as in
  the configuration the Debian and Ubuntu packages install, the fetcher
  passed an empty CA file name to libcurl, and every HTTPS fetch failed with
  curl error 77 (`CURLE_SSL_CACERT_BADFILE`). The fetcher now uses the
  directory on its own.

- **A server process that dies while holding a shared-memory lock no longer
  hangs every other process.** The mutexes the Apache and nginx modules keep
  in shared memory (metadata cache sectors, statistics, the message buffer,
  named locks) were not robust. When a server child process terminated while
  one of its threads held such a mutex, for example when it was killed by a
  signal or crashed, the mutex stayed locked for good: every thread of every
  process that needed it blocked, and the sites hung until the server was
  restarted. The next process to lock such a mutex now takes it over and logs
  a warning. This needs robust mutexes, which Linux and FreeBSD have; on
  macOS such a mutex still stays locked. A metadata cache sector taken over
  this way is disabled and acts as empty until the next restart, as its
  contents may have been left half-updated. A server that hangs this way at
  the time of the upgrade needs a full stop and start: a graceful restart
  gives the new child processes robust mutexes, but the old, stuck ones keep
  waiting on the old mutexes.

- **A process that dies while reading from the shared-memory cache no longer
  hangs the next writer.** A process or thread that terminated while it
  copied a value out of the cache left that entry marked as being read for
  good, and the next write or delete of the same key waited for it forever:
  the request hung, and an Apache child stuck this way did not exit on a
  graceful restart. A writer now gives up after one second, logs a warning
  and drops the write. That key is no longer kept in the shared-memory cache
  until the server is restarted; it is still served from the file cache or
  the external cache.

- Thanks to [@trcyberoptic](https://github.com/trcyberoptic), who contributed
  the HTTPS fetch, shared-memory and source-build fixes in this release.

### Added

- `CriticalCssAboveTheFoldOnly` (default off) makes
  `prioritize_critical_css` inline only the rules for the first screen, as
  releases since 1.15.0+r18 did. Choose it when the smallest inline block
  matters more than content further down being styled from the first paint.
  Apache: `ModPagespeedCriticalCssAboveTheFoldOnly on`; nginx:
  `pagespeed CriticalCssAboveTheFoldOnly on;`; IIS:
  `pagespeed CriticalCssAboveTheFoldOnly on`. `prioritize_critical_css`
  depends on the beacon and therefore does nothing on Envoy, where this
  setting has no effect either.

- The whole-server admin console has a host lens: a Host selector in the
  top bar (in the menu on a phone) narrows the URL index to one site and the
  Logs timeline to the entries that name it. The lens follows the optimizer's
  own rule for host names: every site the optimizer lists can be chosen —
  names with an underscore and sites configured by address, such as
  `[2001:db8::1]`, included — and a `lens=` link is read the way the
  optimizer names a host (case, a port and a trailing dot do not matter); a
  value the optimizer would never list is ignored. The choice is part of the
  page address, so a link can be shared, and the browser remembers it: while
  the lens is on the console's address always carries it — also after typing
  an address or following a link without it — updated in place, so Back and
  Forward are unaffected and a cleared lens stays cleared. Pages without
  per-site figures say so while the lens is on. Per-host consoles are
  unchanged.
- A per-host console's Savings and Graphs pages show the optimizer's row
  for the site the console belongs to — also when that is the site's
  primary name rather than the name the console was opened under —
  labelled with that name; when the site has no name of its own, or the
  optimizer has no row for it yet, the card stays muted, and the Graphs
  page says its optimizer savings chart covers the whole server. With an
  older module the console matches its own name, as before.
- With an optimizer that reports serve savings per host, the admin
  console's Savings page lists them per site on the whole-server console,
  leads with the selected site's figures when the host lens is on, and —
  on a per-host console — shows that site's own optimizer savings instead
  of only the server-wide total. An older optimizer is told apart and
  nothing else changes.
- With an optimizer package that records serve savings per host, the module
  now names the host each response from the optimizer's cache was served
  for, so the optimizer's statistics — and the admin console — can show
  those savings per site. With an older optimizer package the serve is
  recorded as before, without the host. Apache, nginx and IIS.

- The admin console's Support page has a "Copy diagnostics" action: it copies
  the module and console builds, the optimizer's version, a SHA-256
  fingerprint of the configuration and the last 50 warnings and errors as
  plain text, for a support request. Nothing is sent anywhere; a browser that
  refuses the clipboard shows the text to copy by hand.

- The admin message history can answer grouped by kind of message
  (`message_history?grouped=1`): one row per message template — URLs,
  hexadecimal identifiers and numbers folded, so repeats of one message share
  a row — with its level, how often it was logged and when last, and with
  `window_s=<seconds>` how often within that recent window. The ungrouped
  answer is unchanged.
- Every headline number on the Overview and Savings pages now states the window it counts over — since the module's restart or since the optimizer's — and a freshly restarted optimizer shows a "warming up" marker instead of percentages, with the raw counts kept. Each card carries a scope chip saying whose traffic it covers, the optimizer's server-wide cards are visibly muted on a per-host console with a link to the whole-server view, and the Overview's cache-serve figure is a link into the Savings page carrying its own percentage.

- The Savings page explains each content type with a split of where its traffic went — already optimal (with the resources and original sizes behind that verdict), optimized and served, served compressed, and not yet optimized — instead of showing a bare percentage that reads as zero work; the byte figures say whether they are optimized bytes or transfer bytes of compressed copies. The cache hit rate moved to the optimizer card, is computed over optimizable types only, names how many requests it excludes (not optimizable or not recorded), and keeps the previous all-requests rate with its own label when the module is older than the split counters. A real but tiny saving reads "<1%" instead of "0%".

- The statistics JSON the admin console reads now includes a start-time gauge: when the module's shared statistics were initialised, so every counter the console shows can state the window it counts over. It reads as a gauge and is set once, in the root process.

- The daemon-serve statistics now split by content class: cache serves count per CSS, JavaScript, image and other, and fall-throughs per CSS, JavaScript and image (learned from the origin response, so a fall-through whose type is never known stays only in the total). The existing totals are unchanged, and a cache-serve hit rate over optimizable types can now be computed from them.

- **In-place serving can send the optimizer's stored gzip and brotli copies
  (Apache and nginx, off by default).** With
  `ModPagespeedDaemonServeStoredEncodings on` (nginx:
  `pagespeed DaemonServeStoredEncodings on;`), a stylesheet, script or SVG
  image the optimizer has stored compressed is sent to a client that lists
  `br` or `gzip` in `Accept-Encoding` as that stored copy — with
  `Content-Encoding`, `Vary: Accept-Encoding` and a validator of its own —
  instead of being compressed again on the way out. A client that does not
  list the coding by name, HTML, and resources without a stored copy get
  exactly the response they get today, and so does every request with the
  directive off. With it on, the optimizer's serve statistics count the
  bytes actually sent: `serve_savings.<type>.optimized_bytes` becomes the
  compressed size for these serves, measured against the uncompressed
  original, and `by_encoding` shows how many went out under each coding. The
  default changes to on in a later release. Not available on IIS in this
  release.

- The whole-server admin console can read the optimizer's recent log (GET v1/daemon/logs with validated since/limit parameters); the per-virtual-host console answers it 403, an optimizer without the log endpoint answers 501 endpoint_unsupported_by_daemon, and an answer larger than the proxy accepts is 502 response_too_large.

- The whole-server admin console can page the optimizer's cached-URL index (GET v1/daemon/cache/urls with validated offset/limit/hostname parameters); the per-virtual-host console answers it 403.

- The whole-server admin console can read which cached variants the optimizer holds for a URL (GET v1/daemon/cache/alternates); a cached entry is named by its path, host and scheme (url, hostname, scheme — all required and validated), and a URL the optimizer holds nothing for answers 404 "not_in_index".

- The whole-server admin console can read a cached image variant's bytes (GET v1/daemon/cache/content?url=…&hostname=…&scheme=…&alternate_id=…); only image/png, image/jpeg, image/gif, image/webp and image/avif are served, every response carries content-sniffing, script-execution, framing and cross-origin hardening headers, at most two reads (32 MiB) run at once per server worker process (whatever the number of virtual hosts), and a variant over the 16 MiB cap is an error, never a truncated image.

- **The admin console opens on an Overview page.** One screen says whether the
  module is working and what it has saved, whether the optimizer daemon is
  running, not configured, unreachable or too old for this console, what the
  optimizer saved on the responses it served, and which scope (this virtual
  host or the whole server) the figures cover. Statistics moves to the second
  navigation item; links lead to the detail pages.

- **The Overview raises alerts.** Rules for the optimizer (failing health
  checks, failed cache writes, new errors, compressed origin responses, busy
  threads, saturated connections, a stopped browser, unreachable, or too old
  for this console) and for the module (new resource fetch failures) show as
  a ranked list, errors first, with a link to the page that has the detail.
  Alerts based on counters fire only on an increase between two refreshes and
  stay visible for a minute; a dismissed alert returns only after its
  condition has cleared and come back. No optimizer configured raises nothing.

- The console has a Savings page answering "how much am I saving?": module rewrite savings and optimizer cache-serve savings, each labelled, with a live chart of the savings rate per second. It is reachable at #/savings, in the navigation after Overview, and via g then v.

- The Graphs page draws real time series with axes and time labels: the statistics log's history for the chosen range, with the live poll merged into the same lines. Counters show rates per second once the module names its gauges (gauges always draw raw), a counter reset shows as a gap instead of a spike, an idle or missing log no longer blanks the page while live counters exist, and long counter lists page through a "Show more" button.

- The daemon cache panel's serve savings end in a total row, and the raw counters are one click away behind a disclosure.

- The Statistics page shows when its counters were captured, on builds that report it.

- The whole-server admin console gains a URLs page that pages the optimizer's cached-URL index, with client-side filter and sort of the current page (labelled as such), cooldown badges, and clear explanations when the module or optimizer cannot serve the index or the console is per-virtual-host.

- The whole-server admin console gains a URL detail view: the optimizer's cached variants for one URL with format, quality and content-class badges, freshness, an in-cooldown banner, and image previews loaded two at a time (a busy server is retried); the URLs page rows and the Cache page's lookup result link to it.

- The URL detail view can compare a cached variant with its original in an accessible before/after diff: a keyboard-operable slider (arrow keys, Home/End) and a manual blink toggle.

- **IIS: the optimizer, installed with the module and off until you turn it
  on.** The IIS installer now also installs the PageSpeed optimizer as the
  Windows service `WeAmpPageSpeedOptimizer`, disabled, under its own virtual
  account, with its cache volume directory and log. Turned on and pointed at
  with the `DaemonSocketPath` and `DaemonVolumePath` directives, it takes over
  in-place optimization: the module records each eligible resource, the
  optimizer builds optimized variants of it, and the module serves the variant
  that fits each client from the optimizer's volume. The optimizer needs no
  Visual C++ runtime. An upgrade or repair keeps an optimizer you turned on
  running, and keeps the cache size you chose (`OPTIMIZERCACHESIZE`, 1 GiB by
  default). Existing installs that do not turn it on behave exactly as before.
  See `docs/daemon-adapter-iis.md`.

- **nginx: the module can now work with the optimizer daemon.** Point a
  server block at the daemon with `pagespeed DaemonSocketPath` and
  `pagespeed DaemonVolumePath` — the daemon's notification socket and its
  shared cache volume — and the module records each eligible in-place
  response into the optimizer's cache and answers in-place requests from
  the optimizer's variants, as the Apache module does: the optimizer
  builds optimized variants of the recorded resources, and the module
  serves the variant that fits each client from the optimizer's volume.
  The daemon relationship is resolved in the master process and the cache
  volume is opened once per worker process; nothing on this path blocks
  nginx's event loop. With both directives unset the classic in-place path
  is unchanged. See `docs/install-nginx.md`.

- **The admin console says when it cannot reach the server.** A banner above
  the page names the problem, says when the figures on screen were last
  refreshed, and offers "Retry now"; the page keeps its last data and retries
  on its own, less often the longer the outage lasts. The top bar shows
  "connected" or "reconnecting" instead of a fixed "running". An unreachable
  optimizer daemon stays a panel state and does not raise the banner, while a
  reverse proxy or load balancer answering with its own 502/503/504 counts as
  an outage, so the banner holds steady behind a gateway too. Requests
  that get no answer within 15 seconds count as unanswered.

- **Messages can be opened filtered by severity.** A link such as
  `#/messages?level=error` opens the Messages page showing errors and anything
  more severe; the Overview's fetch-failure figure and alert open it at
  warnings and worse. Each message now shows its time once (in the browser's
  time zone, with the server's UTC time on hover) instead of twice.

- **Keyboard shortcuts in the admin console.** `?` lists them (also from the
  new button in the top bar), `r` refreshes the page's data, `/` jumps to the
  page's search box, and `g` followed by a letter opens a page (for example
  `g s` for Statistics, `g d` for Daemon Status). They never fire while typing
  in a field or with Ctrl, Alt or Cmd held.

- The statistics JSON now reports the console scope, the instance's host identity, a capture timestamp, and which counters are gauges; counter names are escaped, and a reply with no plain variables is no longer invalid JSON.

- The statistics JSON includes the timed counters (such as num_rewrites_executed) as totals since start.

- The per-vhost cache JSON reports backend statistics for the vhost's own cache path; the global view is unchanged.

- Optimizer Logs page in the whole-server admin console (#/logs, "Optimizer daemon" group, shortcut g l): the optimizer's recent log, refreshed every 3 seconds and catching up when the optimizer logs faster than that, with level and text filters, follow mode with jump-to-newest, and markers where entries were dropped or the optimizer restarted. Needs an optimizer and a module that provide the log; otherwise, and on a per-host console, the page explains why it is empty. A control character embedded in a logged message (for example a line break) renders as a visible symbol instead of blank space, and each entry has a visible divider, so one entry's text can never be mistaken for another log line.

- The overview's "Optimizer errors" alert links to the Optimizer Logs page filtered to errors, and the Messages and Optimizer Logs pages cross-link so an operator can tell the module's own log from the optimizer daemon's.

### Changed

- The Apache and nginx modules no longer leave any symbol of their bundled
  TLS library for the web server to resolve, and no longer export internal
  linker-section symbols. This is now verified for every release.

- `prioritize_critical_css` fetches each full stylesheet from the place its
  `<link>` had in the page instead of discovering it only after the page has
  been parsed: the stylesheet is preloaded there and takes effect there as
  soon as it has arrived, so the full styles land sooner and the order in
  which a page's rules apply is exactly the page's own. A `<noscript>` copy
  of the link follows each preload. Inline `<style>` blocks are now left in
  place and complete (one that contains an `@import` therefore blocks as it
  does without the filter), and alternate, print-only and `<noscript>`
  stylesheets are no longer moved. `@keyframes` now stay in the inline
  rules, so animations start with the first paint. A stylesheet keeps its
  ordinary blocking `<link>` when it still has an `@import` the server could
  not merge into it, when nearly all of it would be inline anyway, or when
  its `<link>` carries an event-handler attribute, a `title` (a named set
  of stylesheets) or `disabled`. The
  `<noscript class="psa_add_styles">` blocks at the end of the body are
  gone; a page script that looked for them finds the deferred stylesheets
  as `link[data-pagespeed-deferred-css]`. The small inline script that
  turns the stylesheets on now sits ahead of the first of them instead of
  at the end of the body, and turns on whatever is in the page whenever it
  runs; if something in front of the server delays inline scripts, the
  full styles arrive when it does. Content a script adds while the page is
  loading takes its rules from the full stylesheet.
  If the stylesheet request fails, the link becomes an ordinary stylesheet
  link at once, as on a page without the filter; a browser without preload
  support gets the stylesheet when the document has been parsed.

- `prioritize_critical_css` now inlines the rules for everything in the
  page, not only for the first screen. Since 1.15.0+r18 only rules whose
  selectors matched in the first screen were inlined, so on a slow
  connection content further down could be shown partly styled, and then
  move, while the full stylesheet was on its way. The inline block is
  larger for it (on a long
  page with a 110 KB stylesheet: about 58 KB instead of 46 KB, about 1.6 KB
  more compressed, estimated). A selector a visitor's browser cannot parse
  is now treated as needed. Pages pick the new rule up as visitors' browsers
  report again; see `CriticalCssAboveTheFoldOnly` to keep the smaller block.
  What the inline rules cover is the page as visitors' browsers last saw
  it: content a script adds while the page is loading takes its rules from
  the full stylesheet, and for pages whose markup differs from visitor to
  visitor under one URL the filter is best left off. The browser reports
  which rules the page uses once it has loaded, so content that a script
  removes, hides or gives other class names before then (a loading overlay,
  a `loading` class on the body) can be painted without the rules that
  applied only to its earlier state, until the full stylesheet arrives.

- Admin console layout: the sidebar always runs the full height of the
  window, the top bar and every row of buttons, filters and toggles use one
  even spacing and one control height, and "whole server" / "this host"
  chips and the top bar's console label appear only where they tell the
  two scopes apart.
- Admin console pages now use the full width of the window and line up
  with the top bar: cards, charts, statistics groups and the optimizer
  status sections share the width in columns, tables use it in full, and
  text keeps a readable line length. Rows of controls are grouped, the
  sidebar highlights the whole row of the current page, and buttons inside
  list rows use one compact size.
- Serve savings per host now name only a host name the server's
  configuration lists for the site that served the response: the
  request's host when it is one of the site's exact names (Apache
  `ServerName` or `ServerAlias`, nginx `server_name`), otherwise the site's
  own primary name. A request that reached a site through a wildcard alias,
  a regular expression, or because the site is the default one, counts under
  that site's name, and a host name a visitor sends is never listed on its
  own. A site reached under several exact names has one row per name used.
  On Apache a site is listed under its `ServerName` only when its
  configuration states one; a virtual host (or a main server) without a
  `ServerName` of its own, on any address, records its serves under
  "other", whatever name httpd derives for it (a request for one of its
  exact `ServerAlias` names still counts under that name). The same holds
  for any site without a usable name of its own (nginx `server_name _;`,
  or only wildcard and regular-expression names): give each virtual host
  its own `ServerName` to have it listed. Apache and nginx; on IIS every
  serve is recorded under "other" for now, as the module does not yet read
  IIS site bindings.
- A per-host admin console now receives only its own site's row of the
  optimizer's serve savings per host (v1/daemon/stats); every other site's
  serves are added to "other", so the figures still add up, and the
  whole-server console's answer is unchanged. When the optimizer's answer
  cannot be read, a per-host console is told the optimizer is unreachable
  instead of being sent the answer as it was. A per-host console opened
  under one of a site's exact names sees the row of that name. On IIS and
  Envoy a per-host console shows no per-site row.
- The admin console's three optimizer pages (status, back-pressure, cache)
  are one "Optimizer status" page with Health, Load and Cache sections; old
  links and bookmarks land on the matching section.
- The admin console's sidebar is grouped as Overview, Savings, URLs; Module
  (Statistics, Histograms, Caches, Configuration); Optimizer (Status, Logs);
  Help. Each entry shows its keyboard shortcut. Graphs is a view of
  Statistics, and the old Console page's address opens Statistics with its
  "Δ since open" column on.
- The admin console's Messages and Optimizer Logs pages are one "Logs"
  page: the module's messages and the optimizer's log on one timeline,
  newest first, with a source filter, one level filter, repeats grouped per
  source (one tick shows every entry) and dates on every time. The
  optimizer's log on its own with grouping off is the live stream it was
  before: oldest to newest, following the newest entry, with "Jump to
  newest" and its restart and gap markers. Links to the old Messages page
  keep their level and open the module's messages.
- The admin console's Graphs view opens on six charts — in-place
  requests, module and optimizer savings per second, the optimized-copy hit
  rate, resource fetch failures and the optimizer's jobs in progress — with
  history from the statistics log where it has one. "Add a counter" puts
  any other counter next to them (the address keeps the choice), charts
  without data yet are listed instead of drawn empty, and every counter is
  one link away.
- The admin console's Overview lists findings instead of a single health
  line: each says what is wrong, how to fix it and where to read more, worst
  first — among them the admin pages being reachable from another address,
  the optimizer's cache not being in use, a recently started or briefly
  unreachable optimizer, a development build, and the warnings logged in the
  last 15 minutes, grouped by kind. A finding can be acknowledged; it then
  waits in a collapsed group, and returns if its condition clears and comes
  back. The summary line counts the findings, those that need action and
  the warnings and errors logged in the last 15 minutes. With nothing to
  report, the Overview says "No findings" and since when its counters run;
  when the message log cannot be read it says the findings are unavailable
  instead.
- The admin console's Messages page shows warnings and errors by default
  (Info is one tick away), folds repeats of one message into a single row
  with a count and the time of the last one — the individual entries are a
  click away —, no longer repeats the severity inside the text, and turns
  http and https addresses in messages into links.
- The admin console's Optimizer Logs page shows the date with each time.
- The console now shows one consistent set of versions: the About page and the keyboard-shortcuts dialog list the module build (with a visible warning when it was built with uncommitted changes), the optimizer's version and commit, and the console build; the top bar shows the friendly tag form of the module build with the full build stamp in its tooltip, instead of the long stamp itself. Overview, Savings and About now use the same page header, content width and number formatting as the rest of the console.

- The Savings chart's title now names the window and sample interval its data actually covers ("Last 6 min · 10 s samples") instead of a fixed "last hour", the y-axis carries a bytes-per-second unit, a pause in sampling breaks the line instead of bridging it with a straight segment, and the legend moved inside the card showing each series' latest value rather than placeholders under the card border. Charts also expose their summary as text for assistive technology and copy/paste.

- **Breaking: purging through `pagespeed_admin/cache?purge=` with a GET no
  longer works** (it answers 405). Send a POST with the
  `X-Requested-With: XMLHttpRequest` header instead; purge the whole cache
  (`purge=*`) from the global admin page (`pagespeed_global_admin`). Scripts
  can keep using the `PURGE` request method (`PurgeMethod`), which is
  unchanged. A per-host admin page now purges only its own host's URLs: a
  script that purges another host's URL through it must use that host's
  admin page or the whole-server admin page.

- **Breaking for monitoring probes:** the admin console's optimizer status
  paths (`v1/daemon/health`, `stats`, `cooldowns`) answer a request that
  carries a query string with a 400 and a reason code instead of ignoring
  the query string. A probe that appends a cache-buster to
  `v1/daemon/health` must drop it.

- nginx: in-place optimization no longer applies to requests that nginx
  redirects internally. Resources referenced from rewritten HTML are
  optimized as before.

- **nginx: `add_header` and optimized resources.** Responses the module
  generates (`.pagespeed.` resources and in-place optimized responses) do not
  pass nginx's headers filter. They carry the headers of the origin response
  they were made from — including what your `add_header` directives put on
  it — exactly once. A resource read with `LoadFromFile` has no origin
  response to inherit from: use `pagespeed AddResourceHeader` for a header
  that must be on every optimized resource. In-place optimization records the
  response as served, so caching headers set by `expires` or `add_header` are
  what it sees.

- **Admin console chrome now reflects the 2.1 product identity.** The console's
  checked-in copy of the product facts (names, canonical URLs) was behind the
  upstream single source; it is synced and the console bundle rebuilt.

- **The admin console's Configuration page says which scope it shows** (this
  virtual host or server-wide) and can show the options in effect for the
  request, not only the server configuration. The Statistics page names the
  host as well, and a console at a renamed global admin path says it shows
  the whole server.

- **The admin console's Messages page fetches only new messages on each
  refresh** and says that the buffer is process-wide (all virtual hosts).

- **The admin console's histograms are served as data, not as an HTML table
  wrapped in JSON**; the page is lighter and no longer scrapes markup. Tools
  reading the `histograms` endpoint get an array of objects.

- **The admin console refreshes more gently.** Each page makes one request at
  a time, waits longer after each failed refresh (up to a minute), makes no
  requests while its browser tab is hidden and refreshes as soon as it is
  shown again; the Overview does the same when the server does not answer.
  Pages open at the same time share a read that is already under
  way, and a busy answer from the optimizer daemon (another tab reading the
  same panel) no longer shows as a failed refresh. The Caches page keeps
  showing its last data when a refresh fails, like every other page.

- **The admin console is easier to use with a keyboard or a screen reader.**
  The navigation is grouped into Module, Optimizer daemon and Help pages, its
  items are links that mark the current page, and a "Skip to content" button
  leads the tab order. Moving to a page puts focus on its heading and names it
  in the browser tab; every control shows a focus ring; an error that leaves a
  page empty is announced; headings no longer skip levels. If reading the
  configuration fails when the console first opens, it is retried once the
  connection to the server is next seen restored.

- **Daemon Cache shows serve savings per content type, as the module records
  them.** The optimizer serves no page responses itself; the web server's
  module serves optimized responses from the optimizer's cache and records
  each one. The panel now says so, shows responses, original and served bytes
  and the saving for each content type served, and names the types with
  nothing recorded instead of listing rows of zeros. Daemon Status shows each
  health check as "Pass" or "Fail" (with the reason when the optimizer gives
  one) instead of raw JSON, and Daemon Back-pressure explains what the
  "skipped" notification counts mean. A negative byte count from the optimizer
  can no longer make the Overview's saving exceed the original size.

- **Admin console tables work from the keyboard.** Sortable column headers
  on Statistics and Console are buttons and announce the current order, and a
  histogram can be chosen with the keyboard. Statistics shows the counter
  descriptions by default (the choice to hide them is remembered), and on a
  phone counter names wrap between words instead of mid-word.

- **About shows the optimizer daemon's version and a documentation link;**
  it no longer repeats the console's build version. The Support page's text no
  longer ends in a double full stop.

- **Plan for this before you upgrade: the disk cache starts empty again, and
  the optimizer's cache directory moves to `v2`.** The cache library moves
  to on-disk format 8 (CRC-32C checksums). The module's own cache
  (`ModPagespeedFileCachePath`) opens a new file beside the old one, which is
  left on disk for a warm rollback; delete it once you will not roll back.
  The optimizer daemon moves its cache to
  `/var/cache/pagespeed-optimizer/v2` and publishes cache-directory
  generation 2; this module is built for generation 2 and the packaged
  `pagespeed_daemon.conf` points `ModPagespeedDaemonVolumePath` at
  `/var/cache/pagespeed-optimizer/v2/cache`. If you set that directive
  yourself, change `v1` to `v2`: a module and daemon on different
  generations refuse to attach, loudly, and in-place optimization stays off
  until they agree. Install the module and the daemon as the matching pair
  the packages already require.

- **A cache write or delete that loses a cross-process lock wait now fails
  instead of overlapping.** Under heavy write traffic one server process
  could take the disk cache's lock from another process that still held it.
  A process now waits for a live holder; a write or delete that has waited
  250 ms in total gives up. The write is not stored (the entry is fetched
  and written again on a later miss), and the delete is no longer counted
  in `cyclone_cache_deletes` as if it had happened. The same applies to the
  steps a write takes before storing: when the module cannot confirm the
  target slot is empty, or cannot remove the entry already in it, the write
  is dropped instead of stored over the live entry.

- **After the disk cache wraps, entries from the previous lap stay readable
  until overwritten** (wrap retention, now the default), so hit rates no
  longer dip just past a wrap.
- The admin console gets a shared visual foundation: one monospace stack
  (system fonts, no more Courier fallback), tabular digits, one page width,
  and a top bar that stays dark and legible in dark mode.
- The console's pages share one header, width, number format and phone
  layout: statistics group into collapsible families with an optional
  "Δ since open" column (the old Console page now points there), histograms
  gain a unit column and readable digits, the caches and daemon panels drop
  icons and inverted labels for words, message and optimizer-log levels
  share one colour palette, and phones get a closing drawer with card
  tables and page scrolling.

## [2.1.0] - 2026-09-17

### Added

- **Every Linux module package now also carries `THIRD-PARTY-NOTICES`.** The
  Apache module deb and rpm and the nginx module deb and rpm install it next
  to `LICENSE` and `NOTICE` under `/usr/share/doc/<package>/`: the full
  attribution for every statically linked third-party component — license,
  copyright holders, and for the BSD/MIT/Zlib-class licenses the license
  text itself, quoted from the pinned upstream, since those licenses'
  terms require the text to accompany a binary redistribution — including
  the required Independent JPEG Group statement for the statically linked
  libjpeg-turbo. The file is curated from and continuously cross-checked
  against the drift-gated SBOM (`sbom/pagespeed-1.1.spdx.json`) by a new CI
  gate, `tools/sbom/check-third-party-notices.py`, which fails the build
  when the notices and the SBOM disagree in either direction. The root
  `NOTICE` was corrected in the same pass: stanzas for code no longer in
  the tree (serf, the chromium family, closure_library, base64, modp_b64,
  apr_memcache2) were removed, the remaining stanza paths now match the
  live tree layout, and stanzas for the vendored css_parser (+ its nested
  Lucent `utf` library) and redis-crc were added. The googleurl (gurl)
  dependency moved from `WORKSPACE` into `bazel/repositories.bzl` so this
  chain covers it; it had shipped statically linked with no attribution the
  gate could see.

- **Every module package now carries the license text and the attribution
  notices.** The Apache module deb and rpm (including the cPanel EasyApache 4
  build), the nginx module deb and rpm and the IIS installer include the
  Apache License 2.0 text (`LICENSE`) and the attribution notices (`NOTICE`):
  under `/usr/share/doc/<package>/` on Linux, next to the module DLL on
  Windows. Previously only the NuGet package shipped them.

- **The module deb and rpm packages now install the daemon configuration.**
  `pagespeed_daemon.conf` — `/etc/apache2/conf-available/` on Debian/Ubuntu
  (enabled by the package with `a2enconf`, a dpkg conffile) and
  `/etc/httpd/conf.d/` on the Red Hat family (`%config(noreplace)`) — sets
  `ModPagespeedDaemonSocketPath` and `ModPagespeedDaemonVolumePath` to the
  optimizer package's defaults inside `<IfModule pagespeed_module>`, so an
  upgrade from 1.15 reaches the daemon without hand-editing. The name sorts
  after `pagespeed.conf`, which on the Red Hat family is the file that loads
  the module. Existing hand-written directives keep working; where both set
  the same directive the packaged file, read later, is the effective value.

- **The module now records the serve-time bandwidth savings it produces into
  the optimizer daemon's statistics.** In the topology where the daemon only
  writes optimized variants and the module serves them in place, the daemon
  never answers a request itself, so its serve-savings counters (original vs
  optimized bytes and hit counts, per content type) previously stayed at zero
  forever. The module now records each optimized in-place serve through the
  daemon's published client interface, so those counters reflect real traffic
  and the daemon cache console panel has live data in this topology.
  Only serves of worker-produced optimized variants with a recorded origin
  size are counted, matching the gate the daemon's own front ends apply.

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

- **The admin console's navigation now carries a dismissible Support panel**
  pointing at support subscriptions; the dismissal is remembered.

### Changed

- **The admin console's product-facts data no longer carries the retired
  license pricing.** The console now syncs its facts copy verbatim from
  `shared/product-facts.mjs` at the pagespeed-optimizer repo root — the
  identity-only, pricing-free module (names, canonical URLs, the one
  product statement, the support-terms URL) that replaced the website's
  much larger facts file as the sync source. The per-site license ladder,
  its dollar figures and the launch-promo metadata are gone from the
  shipped copy. The console UI is unchanged: it renders only product names
  and URLs from this file, and the rebuilt `admin_console.html` differs
  only in its version stamp.

- **Plan for this before you upgrade: the disk cache starts empty.** This
  release moves to a new on-disk cache format. The new binaries open a new
  cache file instead of converting the old one, so every deployment begins
  with a cold cache that refills as traffic arrives. Nothing is lost that
  cannot be rebuilt and no configuration change is needed, but expect a
  temporary drop in cache hit rate and a matching rise in origin fetches and
  optimization work until traffic has warmed the cache again — so upgrade at
  a quiet hour if your origin is sensitive to that.

  **Check free space first: the cache directory holds two cache files at
  once.** The previous file is not removed, and the new one grows to the full
  configured cache size as it fills, so the directory needs room for both —
  free space of at least the configured cache size (`ModPagespeedFileCacheSizeKb`,
  or the optimizer daemon's cache size where the daemon owns the volume) on
  top of what the existing cache already occupies. **If the directory is sized
  for exactly one cache file, delete the old file before starting the new
  binary, not after.** On Linux the new file is allocated sparsely, so a
  directory that is too small does not fail at startup — it fills up hours
  later, mid-traffic, as the cache warms. On Windows the space is taken at
  once. Nothing in the product checks free space for you.

  The cache directory holds files named `<cache name>-<format number>-<hash>`;
  the one to remove is the one with the **lower** format number, and it should
  be removed with the server (or, on the daemon topology, the daemon) stopped,
  since a running process holds its file open and the space is not returned
  until it exits. Removing it gives up the warm rollback described next, so on
  a directory with room for both, keep it until you are confident you will not
  roll back.

  Downgrading is safe and warm as long as the old file is still there: an
  earlier binary reopens its own previous cache file untouched.

  **Where the optimizer daemon is in use, upgrade and start the daemon
  first.** The daemon owns the cache volume and the Apache module attaches to
  it; Apache restarted after the daemon package is upgraded but before the
  daemon has restarted and created its new-format volume will refuse to
  start, and say so, rather than quietly running a cache of its own. Starting
  the daemon and restarting Apache clears it.

  **At boot there is nothing to do for Apache, and nothing to do for nginx
  from a packaged 2.1 optimizer onward.** The optimizer service counts as
  started only once its notify socket exists, and is ordered ahead of
  apache2/httpd and — new in 2.1 — nginx, so those web servers are not
  released until the cache volume is there. A host still carrying a 2.0-era
  optimizer package has the Apache-only ordering, so an **nginx** deployment
  that upgrades this module before the optimizer is raced at every boot rather
  than once at upgrade. A web server under any other unit name is not ordered
  against the daemon; add an ordering drop-in. The ordering never makes one
  service require the other — each still starts without the other.

- **The cache now refuses a single object larger than 64 MiB (67,108,864
  bytes).** The cache library enforces a per-object ceiling it previously
  declared but never applied. There is no directive to raise or disable it in
  this release.

  **Most deployments cannot reach it.** `ModPagespeedMaxCacheableContentLength`
  defaults to 16777216 (16 MiB) and is applied to the HTTP cache, so a larger
  response is already excluded long before the new ceiling. You are exposed
  only if you have raised that directive above 67108864, or set it to `-1`
  (unlimited).

  **What you would see.** An affected response is simply never cached. It is
  fetched from the origin on every request — permanently, not as part of the
  post-upgrade warm-up — while everything else caches normally. Each refusal
  increments `cyclone_cache_failures`, and a warning naming the size limit is
  logged (sampled, one in every 1024 write failures).

  **Telling it apart from a genuinely full cache.** A full or unwritable cache
  fails *every* write: `cyclone_cache_failures` climbs with all traffic, the
  hit rate collapses across the board, and nothing new is stored. The size
  ceiling fails only the over-size objects: ordinary pages, scripts, styles
  and images keep being cached and the hit rate for them is unchanged. The log
  message distinguishes the two — an over-size write now says so, where it
  previously reported "cache may be full" and pointed at the one remedy that
  cannot help (enlarging the cache does not raise a per-object bound).

  **If you were relying on caching larger objects,** set
  `ModPagespeedMaxCacheableContentLength` to 67108864 or less. That excludes
  those responses before they reach the cache, so the work of trying is not
  spent on every request; they are then served straight from the origin, as
  they will be in any case.

- License — Apache License 2.0 (was BUSL-1.1). Every feature is available to
  everyone. Every file now carries an SPDX `Apache-2.0` header or is a
  documented exception, verified with Apache RAT (Release Audit Tool).

- The bundled Cyclone cache library, statically linked into every shipped
  module, is licensed under the Apache License 2.0, matching the rest of the
  distribution. `NOTICE` and the SBOM record it.

### Fixed

- **The admin console no longer reports a version stamp that exists in no
  commit.** The console bundle shipped with the product-facts sync carried a
  `-dirty` build stamp because it had been built with uncommitted changes in
  its worktree; the About page and the top bar displayed that stamp. The
  bundle is rebuilt from a clean checkout and now reports the release
  version. Bundle content is otherwise unchanged.

- **A URL whose optimized variants were re-recorded many times could
  permanently stop accepting new ones.** Every re-record used to leave the
  superseded copy linked behind the new one, so the stored chain for that URL
  grew on every refresh while the number of distinct variants stayed put,
  until it reached its limit and every further write to that URL was refused.
  The URL then froze: reads kept succeeding, so nothing looked wrong, but the
  browser was served an increasingly stale version that was never replaced.
  Superseded copies are now unlinked as the replacement is written, so the
  chain no longer grows with refreshes, and a chain that does reach the limit
  resets and accepts writes again instead of staying frozen.

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
  that ask for it by name, so these browsers receive WebP rather than AVIF.

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
  left unhinted. Mixed-version deployments sharing a cache degrade cleanly.
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

- **Version line renumbered 1.1 -> 1.15.** mod_pagespeed 1.15 is the
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
