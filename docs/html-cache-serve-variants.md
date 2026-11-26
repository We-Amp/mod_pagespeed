# HTML cache: serving Markdown copies (`DaemonHtmlCache serve-variants`)

`DaemonHtmlCache serve-variants` lets Apache and nginx keep a copy of each HTML
page they serve and answer a request that prefers `text/markdown` with a
Markdown copy of that page, at the same URL. Browsers, search engines and every
request that does not ask for Markdown keep getting the HTML page.

This is an opt-in feature. It does nothing unless every step in "Turn it on"
is done, and `off` turns it off again at any time.

IIS does not serve Markdown yet. There, `serve-variants` runs as `report-only`
and no client sees a change. Envoy counts HTML responses as
`daemon_html_unavailable` in either mode.

## What you need

The module alone does not produce the Markdown copy. A worker does, and the
module and the worker have to agree:

- The optimizer library installed with the module must publish C API 1.14 or
  later. With an older one the module logs why once and stays in `report-only`.
- The optimizer worker must run with `--html-mode markdown-static` (or
  `markdown-rendered`) and `--agent-optimize`. In the Docker images the second
  one is `PAGESPEED_AGENT_OPTIMIZE=true`. The worker derives the copy from the
  stored page alone: no browser runs and no request leaves the worker.
- The module needs `AgentOptimize on` and `DaemonHtmlCache serve-variants`.
- A `LogDir` is needed for the decision log (below).
- nginx must be 1.13.4 or later.

## Turn it on

Do the steps in this order.

1. Install the optimizer and restart it before you reload the web server. A
   worker from before this release refuses the module's notifications.
2. Start the worker with `--html-mode markdown-static` and `--agent-optimize`.
3. Set `report-only` first, with `AgentOptimize on`, and let traffic run:

   Apache (in the `VirtualHost`; the directive does not work inside
   `<Location>` or `<Directory>` for serving):

   ```apache
   ModPagespeedAgentOptimize on
   ModPagespeedDaemonHtmlCache report-only
   ```

   nginx (in the `server` block):

   ```nginx
   pagespeed AgentOptimize on;
   pagespeed DaemonHtmlCache report-only;
   ```

   In `report-only` nothing a client receives changes. Each HTML response is
   judged and counted: `daemon_html_would_store` against
   `daemon_html_refused_<reason>`, and the decision log in `<LogDir>/daemon-html`
   gives the reason for each page. Read them before going on. If most of your
   pages are refused for `no_lifetime`, `vary`, `headers` or `private`, they
   would gain nothing from `serve-variants`.
4. Change the value to `serve-variants` and reload. The log then says that the
   optimizer library and worker allow serving. If they do not, the module logs
   why (for example, that the worker's `html_mode` is not a Markdown mode) and
   keeps running as `report-only`; `daemon_html_serve_unavailable` counts the
   HTML responses seen in that state.
5. Check with a request that prefers Markdown, twice:

   ```sh
   curl -sI -H 'Accept: text/markdown' https://example.com/page
   ```

   The first request for a page gets HTML: the copy is derived after the
   module stores the page. Later requests get `Content-Type: text/markdown`.

`DaemonHtmlMaxBytes` (default 5 MiB) is the largest HTML body that is stored.
To switch back, set `DaemonHtmlCache off` (the default) and reload; that is the
kill switch.

## What is stored and what is served

A page is stored only when all of these hold:

- It was served for one of the virtual host's own names (Apache `ServerName` or
  an exact `ServerAlias`; nginx an exact `server_name`). Wildcard aliases and
  requests that reach the default host are not stored.
- The origin gave it an explicit lifetime (`max-age` or `s-maxage`). A page
  without one, and a `no-cache` page, gains nothing yet.
- Its headers allow a shared cache to keep it: no `Set-Cookie`,
  `Cache-Control: private` or `no-store`, and no `Vary` that makes it differ
  per visitor (for example `Vary: Cookie`).
- The response as your server configuration sends it allows it too. Headers
  added by `Header set`, `mod_expires`, `add_header` and `expires` count, and
  the shorter of the two lifetimes applies. Those headers do not become part of
  the stored copy.
- The request was answered on its first pass under the URL exactly as sent.
  A request the server and the module read differently (merged slashes,
  percent-escaped paths, dot segments, a request line naming another host than
  `Host`) is left to the server. A page reached through an internal redirect is
  neither stored nor served.

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
like). A matching `If-None-Match` gets a `304` with `ETag`, `Cache-Control` and
`Vary`.

Consequences to plan for:

- Headers your configuration adds to every response, including
  `Strict-Transport-Security` and `X-Content-Type-Options`, are not applied to
  the Markdown copy. Set them at a stage in front of the server.
- The Markdown copy is never compressed in this release.
- With `AgentOptimize on` and `serve-variants`, every rewritten HTML response
  carries `Vary: Accept`. An HTML `304` and an HTML `HEAD` answer do not; a
  cache keeps the `Vary` of the `200`.
- `Cache-Control: private` keeps the copy out of shared caches, including CDNs
  that ignore `Vary`. Behind a CDN, `Age` can cost extra cache misses.

### Apache

- A page reached through an internal redirect (a `.htaccess` front controller
  such as WordPress, Drupal or Laravel with a rewrite to `index.php`) is not
  stored or served as Markdown, and counts `daemon_html_refused_url_disagree`.
  `DirectoryIndex` works: `/` served as `index.html` is stored and served.
- A decoded path containing `?`, `#`, a space, a control byte or DEL is neither
  stored nor served.
- Output filters other than the core ones do not run on a Markdown answer, so
  `mod_security` response-body scanning and `mod_substitute` do not see it.
  Set security headers at a front stage.

### nginx

- Pages reached through an internal redirect (`index`, `try_files` to another
  file or a named location, `error_page`, `X-Accel-Redirect`) are not stored, so
  a directory URL such as `/` gets no Markdown on nginx yet.
- A static file nginx serves without an upstream `Cache-Control` has no
  lifetime of its own, because `expires` is applied after the store decision.
  Such pages are not stored on nginx.
- A `proxy_hide_header` does not hide a `Set-Cookie`, `Cache-Control`, `Vary`
  or `Pragma` from the store decision.

## Purging

A URL purge (the purge method or `?purge=`) removes the stored page and its
copy; one that cannot remove it reports a failure. The URL is matched with its
host in lowercase and a default port removed, but a purge for a different
scheme does not match. A global purge, from the admin page or a touch of
`cache.flush`, fences everything stored before it.

- A purge takes effect in other server processes within one poll interval
  (`CacheFlushPollIntervalSec`, default 5 seconds).
- A global purge is kept in a per-site file beside the cache volume. If that
  file cannot be read, the whole site is fenced until it can.
- A `cache.flush` touch followed by a wipe of the file cache, before any
  process has seen the touch, is not recorded.

## Limits

- The Markdown copy is extracted from the stored HTML without running a browser
  (`markdown-static` style). Text hidden only by a CSS class or an external
  stylesheet stays in the copy.
- A page whose Markdown body is too small to be useful gets no copy, and agents
  get the page itself.
- On an origin whose refusal depends on request headers, a client can trigger
  the removal of a stored entry, at the rate pages are stored. This fails
  closed: the next request gets HTML.
- One entry is stored per site path, whatever port it is requested on.
- The decision log is a file under `<LogDir>/daemon-html`; the admin console's
  HTML cache panel shows the mode, the counters and the reasons serving is not
  active, but does not show the log.

## Counters

All are on the statistics page and in the admin console's HTML cache panel.

| Counter | Meaning |
|---|---|
| `daemon_html_evaluated`, `daemon_html_would_store`, `daemon_html_refused`, `daemon_html_refused_<reason>` | The judgement of each HTML response, as in `report-only`. |
| `daemon_html_stored`, `daemon_html_removed`, `daemon_html_removed_on_refusal` | Entries written and removed. |
| `daemon_html_serve_unavailable` | HTML responses seen while the library or worker does not allow serving. |
| `daemon_html_notify_sent`, `daemon_html_notify_limited` | Requests to the worker to derive a copy, and ones skipped by the rate limit. |
| `daemon_markdown_served`, `daemon_markdown_not_modified` | Markdown `200` and `304` answers. |
| `daemon_markdown_fallthrough_<reason>` | Requests that preferred Markdown and got HTML, by reason (for example `absent`, `stale`, `withheld`, `url_disagree`). |
| `daemon_html_sidecar_write_failed`, `daemon_html_epoch_write_failed`, `daemon_html_purge_remove_failed` | Failures to investigate. |
| `daemon_markdown_served_without_preference`, `daemon_markdown_transport_conflict` | Should stay 0. |

## IIS

IIS does not serve Markdown copies yet. `DaemonHtmlCache serve-variants` is
accepted there and runs as `report-only`; the page evaluation and decision log
work as described in the changelog for `report-only`.
