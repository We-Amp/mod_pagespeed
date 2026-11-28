# HTML cache: serving Markdown copies (`DaemonHtmlCache serve-variants`)

`DaemonHtmlCache serve-variants` lets Apache, nginx and IIS keep a copy of each
HTML page they serve and answer a request that prefers `text/markdown` with a
Markdown copy of that page, at the same URL. Browsers, search engines and every
request that does not ask for Markdown keep getting the HTML page.

This is an opt-in feature. It does nothing unless every step in "Turn it on"
is done, and `off` turns it off again at any time.

On IIS the notes in the IIS section below apply. Envoy counts HTML responses
as `daemon_html_unavailable` in either mode.

## What you need

The module alone does not produce the Markdown copy. A worker does, and the
module and the worker have to agree:

- The optimizer library installed with the module must publish C API 1.14 or
  later. With an older one the module logs why when the state changes and
  stays in `report-only`.
- The optimizer worker must run with `--html-mode markdown-static` (or
  `markdown-rendered`) and `--agent-optimize`. `html_mode` is a setting of the
  whole worker, not of one site. The worker derives the copy from the stored
  page alone: no browser runs and no request leaves the worker.
  The Docker images set `--agent-optimize` with `PAGESPEED_AGENT_OPTIMIZE=true`
  but have no variable for the HTML mode yet, and they do not pass arguments
  through. Set the mode with the `html_mode` key in `pagespeed.json`, which the
  worker reads at start from the directory that holds its cache volume (the
  data directory, `/data` by default, in the images), or with
  `PATCH /v1/config` on the worker's management API (enabled with
  `PAGESPEED_API_PORT` or `PAGESPEED_API_SOCKET` in the images).
- The module needs `AgentOptimize on` and `DaemonHtmlCache serve-variants`.
- A `LogDir` is needed for the decision log (below).
- nginx must be 1.13.4 or later.

## Turn it on

Do the steps in this order.

1. Install the optimizer and restart it before you reload the web server. A
   worker from before this release refuses the module's notifications.
2. Start the worker with `--html-mode markdown-static` and `--agent-optimize`
   (or set `html_mode` as described above).
3. Set `report-only` first, with `AgentOptimize on`, and let traffic run:

   Apache. Set both directives in the `VirtualHost` (or at server level, which
   virtual hosts inherit). Inside `<Location>`, `<Directory>` or `.htaccess`,
   `AgentOptimize` does not turn serving on, and `AgentOptimize off` there does
   not stop it. To keep a path out, turn PageSpeed off there
   (`ModPagespeed off`) or use `ModPagespeedDisallow`:

   ```apache
   ModPagespeedAgentOptimize on
   ModPagespeedDaemonHtmlCache report-only
   ```

   nginx, in the `server` block (or `http`):

   ```nginx
   pagespeed AgentOptimize on;
   pagespeed DaemonHtmlCache report-only;
   ```

   In `report-only` nothing a client receives changes. Each HTML response is
   judged and counted: `daemon_html_would_store` against
   `daemon_html_refused_<reason>`, and the decision log in
   `<LogDir>/daemon-html`
   gives the reason for each page. Read them before going on. If most of your
   pages are refused for `no_lifetime`, `vary`, `headers` or `private`, they
   would gain nothing from `serve-variants`.
4. Change the value to `serve-variants` and reload. The log then says that the
   optimizer library and worker allow serving. If they do not, the module logs
   why (for example, that the worker's `html_mode` is not a Markdown mode) each
   time that state changes, and keeps running as `report-only`;
   `daemon_html_serve_unavailable` counts the HTML responses seen in that
   state.
5. Check with a `GET` that prefers Markdown:

   ```sh
   curl -s -o /dev/null -D - -H 'Accept: text/markdown' https://example.com/page
   ```

   The first `GET` for a page gets HTML: it stores the page, and the worker then
   derives the copy. Repeat the request after a few seconds; it then gets
   `Content-Type: text/markdown`. A `HEAD` request never stores a page, so do
   not use `curl -I` for this check.

`DaemonHtmlMaxBytes` (default 5 MiB) is the largest HTML body that is stored.
To switch back, set `DaemonHtmlCache off` (the default) and reload; that is the
kill switch.

## What is stored and what is served

A page is stored only when all of these hold:

- It was served for one of the virtual host's own names (Apache `ServerName` or
  an exact `ServerAlias`; nginx an exact `server_name`). Wildcard aliases and
  requests that reach the default host are not stored.
- The origin gave it an explicit lifetime (`max-age`, `s-maxage` or
  `Expires`). A page without one, and a `no-cache` page, gains nothing yet.
  On nginx, an `add_header Cache-Control` beside the origin's own line leaves
  the page with no lifetime (see "nginx" below).
- Its headers allow a shared cache to keep it: no `Set-Cookie`,
  `Cache-Control: private` or `no-store`, and no `Vary` naming anything other
  than `Accept-Encoding`.
- The response as your server configuration sends it allows it too. Headers
  added by `Header set` and `mod_expires` (Apache) or `add_header` and `expires`
  (nginx) count: a header that refuses caching refuses the store, and where the
  configuration replaces the lifetime the shorter of the two applies. Those
  headers do not become part of the stored copy.
- The request was answered on its first pass under the URL exactly as sent.
  A request the server and the module read differently (merged slashes,
  percent-escaped paths, dot segments, a request line naming another host than
  `Host`) is left to the server. A page reached through an internal redirect is
  neither stored nor served (on Apache, `DirectoryIndex` is the exception; see
  below).

A page whose response carries a `CDN-Cache-Control` or `Surrogate-Control`
header added by your server configuration is refused, so a site that adds one
to every HTML response gets no HTML cache.

The Markdown copy is served only for a `GET` or `HEAD` request that prefers it
(`text/markdown` listed with a q-value at least that of `text/html`). A request
with cookies, `Authorization`, `Range`, `If-Range` or `Origin` gets the HTML
page. Access, authentication and authorization run before any Markdown answer.
A location where PageSpeed is off never serves Markdown, and Markdown is served
with in-place optimization off.

### The Markdown response

The response carries exactly: `Content-Type: text/markdown; charset=utf-8`,
`Content-Length`, `Cache-Control: private`, one `Vary` line (`Accept`, plus any
`Vary` tokens the origin sent), `X-Robots-Tag: noindex`, `Age` and a weak
`ETag`, besides the server's own transport lines (`Date`, `Server` and the
like), and `Content-Encoding` when a compressed copy is sent (below). A
matching `If-None-Match` gets a `304` with `ETag`, `Cache-Control` and `Vary`.

Consequences to plan for:

- Headers your configuration adds to every response, including
  `Strict-Transport-Security` and `X-Content-Type-Options`, are not applied to
  the Markdown copy. Set them at a stage in front of the server.
- With an optimizer library that publishes C API 1.16 the optimizer stores a
  brotli and a gzip copy of each Markdown copy beside the plain one, and a
  request is answered with the one its `Accept-Encoding` prefers (q-values
  honoured; a coding the request ranks below the plain copy is not used). The
  answer then carries `Content-Encoding: br` or `gzip`, the coded length and an
  entity tag of that coding, so a `304` is given only for the tag of the coding
  asked for. The copies are compressed once by the optimizer, never per
  request, and the server's own compression never compresses them again. A
  request that accepts no coding the copy is stored in (`identity;q=0` with no
  coded copy ready) goes to the origin. A page stored before the optimizer
  wrote coded copies gets them after its first Markdown request (asked once
  per page per server process). With an older library the copy is never
  compressed.
- Every Markdown answer's `Vary` line lists `Accept-Encoding` after `Accept`
  when the library publishes C API 1.16, the plain copy's too.
- With `AgentOptimize on` and `serve-variants`, every rewritten HTML response
  carries `Vary: Accept`, on Apache and on nginx. An HTML `304` does not, and on
  Apache neither does an HTML `HEAD` answer; a cache keeps the `Vary` of the
  `200`.
- `Cache-Control: private` keeps the copy out of shared caches, including CDNs
  that ignore `Vary`. Behind a CDN, `Age` can cost extra cache misses.
- A script's request is never stored and never answered with the Markdown
  copy: one whose `X-Requested-With` contains `XMLHttpRequest` anywhere
  (any case, also in a value joined from repeated lines), or that carries a
  non-empty `X-MicrosoftAjax` or `X-Prototype-Version`. It counts
  `daemon_html_refused_request_header` once at the store and, for a Markdown
  request, once more at the lookup, together with
  `daemon_markdown_fallthrough_request_header`. In
  `report-only` such a request is not evaluated either: it counts only in
  `daemon_html_refused_request_header`.

### Apache

- A page reached through an internal redirect (a `.htaccess` front controller
  such as WordPress, Drupal or Laravel with a rewrite to `index.php`) is not
  stored or served as Markdown, and counts `daemon_html_refused_url_disagree`.
  `DirectoryIndex` works: `/` served as `index.html` is stored and served.
- A decoded path containing `?`, `#`, a space, a control byte or DEL is neither
  stored nor served.
- A compressed copy is written as stored and never encoded again. A stage
  outside the content filters that encodes it a second time is kept (both
  codings are named, so the body still decodes) and counted in
  `daemon_markdown_transport_conflict`, unless it applies the same coding the
  copy is in, which is not detected.
- Content filters (every output filter ahead of the protocol stage) do not run
  on a Markdown answer, so
  `mod_security` response-body scanning and `mod_substitute` do not see it.
  Set security headers at a front stage.

### nginx

- Pages reached through an internal redirect (`index`, `try_files` to another
  file or a named location, `error_page`, `X-Accel-Redirect`) are not stored, so
  a directory URL such as `/` gets no Markdown on nginx yet.
- A static file nginx serves without an upstream `Cache-Control` has no
  lifetime of its own, because `expires` is applied after the store decision.
  Such pages are never served as Markdown on nginx.
- `add_header Cache-Control ...` adds a second `Cache-Control` line beside the
  origin's. Two lines with different `max-age` values conflict, so the page
  keeps no lifetime and is never served as Markdown. Set the lifetime at the
  origin and do not add a `Cache-Control` line with `add_header`.
- Body filters still run on a Markdown answer. When one would rewrite it
  (`sub_filter`, `ssi`, `addition` or another filter that clears the length
  for `text/markdown`), a request that prefers a compressed copy gets the plain
  copy instead, with the plain copy's entity tag, so the filter sees text; it
  counts `daemon_markdown_coded_fallback`. That happens also for a request
  that refused the uncompressed form (`identity;q=0`), and such a client gets
  a full `200` each time, as its `If-None-Match` names the compressed copy's
  tag. `charset` recoding never applies to
  a compressed copy (nginx skips an encoded response). With `gunzip on` the
  gzip copy reaches a client that accepts gzip unchanged.
- A `proxy_hide_header` does not hide a `Set-Cookie`, `Cache-Control`, `Vary`
  or `Pragma` from the store decision.

## Purging

A URL purge (the purge method or `?purge=`) removes the stored page and its
copy; one that cannot remove it reports a failure. The URL is matched with its
host in lowercase and a default port removed, but a purge for a different
scheme does not match. A global purge, from the admin page or a touch of
`cache.flush` (read while `EnableCachePurge` is off), fences everything stored
before it.

- A purge takes effect in other server processes within one poll interval
  (`CacheFlushPollIntervalSec`, default 5 seconds).
- A global purge is kept in a per-site file beside the cache volume. If that
  file cannot be read, the whole site is fenced until it can.
- After touching `cache.flush`, wait one poll interval
  (`CacheFlushPollIntervalSec`) before wiping the file cache. A wipe before any
  server process has seen the touch loses that purge.

## Limits

- The Markdown copy is extracted from the stored HTML without running a browser
  (`markdown-static` style). Text hidden only by a CSS class or an external
  stylesheet stays in the copy.
- A page whose Markdown body is too small to be useful gets no copy, and agents
  get the page itself.
- If your origin refuses caching for some requests based on a request header
  (for example `Cache-Control: private` for one `User-Agent`), such a request
  removes the stored copy, and agents get HTML until the page is stored again.
  Mark such pages `private` for every request, or let them vary only on
  `Accept-Encoding`.
- One entry is stored per site path, whatever port it is requested on.
- The decision log is a file under `<LogDir>/daemon-html`; the admin console's
  HTML cache panel shows the mode, the counters and the reasons serving is not
  active, but does not show the log.

## Counters

All are on the statistics page and in the admin console's HTML cache panel.

| Counter | Meaning |
|---|---|
| `daemon_html_evaluated`, `daemon_html_would_store`, `daemon_html_refused`, `daemon_html_refused_<reason>` | The judgement of each HTML response, as in `report-only` (the engine's own refusals below excepted). |
| `daemon_html_stored`, `daemon_html_removed`, `daemon_html_removed_on_refusal` | Entries written and removed. |
| `daemon_html_refused_url_disagree`, `daemon_html_refused_request_header`, `daemon_html_refused_budget` | Requests the engine itself refused to record (a URL the server and the module read differently, a script's request, the body budget). Not part of `daemon_html_evaluated` or `daemon_html_refused`. `daemon_html_refused_request_header` (and, with optimizer C API 1.15, `daemon_html_refused_auth`, `daemon_html_refused_cookie_request` and `daemon_html_refused_path`) counts once at the store and, for a Markdown request, once at the lookup. |
| `daemon_html_serve_unavailable` | HTML responses seen while the library or worker does not allow serving. |
| `daemon_html_notify_sent`, `daemon_html_notify_limited` | Requests to the worker to derive a copy, and ones skipped by the rate limit. |
| `daemon_markdown_served`, `daemon_markdown_not_modified` | Markdown `200` and `304` answers. |
| `daemon_markdown_served_br`, `daemon_markdown_served_gzip`, `daemon_markdown_served_identity` | Markdown `200` answers by the coding sent (C API 1.16 libraries). |
| `daemon_markdown_encoded_copy_missing`, `daemon_markdown_encoded_copy_unbound`, `daemon_markdown_compress_demand_sent` | Compressed copies a request preferred that were not stored yet or were made from an earlier copy, and the requests to the optimizer to write them. |
| `daemon_markdown_coded_fallback` | Compressed answers sent as the plain copy because a body filter would have rewritten them (nginx). |
| `daemon_markdown_fallthrough_<reason>` | Requests that preferred Markdown and got HTML, by reason (for example `absent`, `stale`, `withheld`, `url_disagree`, `request_header`, `no_acceptable_encoding`). |
| `daemon_html_sidecar_write_failed`, `daemon_html_epoch_write_failed`, `daemon_html_purge_remove_failed` | Failures to investigate. |
| `daemon_markdown_served_without_preference`, `daemon_markdown_transport_conflict` | Should stay 0. |

## IIS

`DaemonHtmlCache serve-variants` serves Markdown copies on IIS as on Apache and
nginx, with the IIS notes in the changelog (host names from the site's
bindings, plain URL spellings only, the `PageSpeedFinalView` module entry).
The compressed copies are pending verification on IIS: there the plain
Markdown copy is served as before.
