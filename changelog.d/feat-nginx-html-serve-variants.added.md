- **nginx: `DaemonHtmlCache serve-variants` stores pages and serves their
  Markdown copies.** With `AgentOptimize on`, an optimizer library that
  publishes C API 1.14 and an optimizer worker running with
  `--html-mode markdown-static` (or `markdown-rendered`) and
  `--agent-optimize`, nginx now stores an HTML page it served under one of
  the server block's exact `server_name` entries, and answers a request that
  prefers `text/markdown` with the page's Markdown copy once the worker has
  derived it. A page is stored only when both the response as the origin sent
  it and the response after the server's own header configuration
  (`add_header`, `expires`) allow it. An `add_header Cache-Control` adds a
  second `Cache-Control` line beside the origin's, and two different `max-age`
  values leave the page with no lifetime, so it gets no Markdown copy: set the
  lifetime at the origin. `proxy_hide_header` does not hide a `Set-Cookie`, `Cache-Control`, `Vary` or
  `Pragma` from that decision. The Markdown answer carries exactly
  `Content-Type`, `Content-Length` (when no body filter changed the body),
  `Cache-Control: private`, one `Vary` line, `X-Robots-Tag: noindex`, `Age` and
  an `ETag`, whatever `add_header ... always`, `expires` or another filter
  adds, and is never compressed. Access rules and authentication run before
  it. A URL purge removes the stored page and its copy; a global purge, by
  the admin page or `cache.flush`, fences everything stored before it, on
  every worker. A static file nginx serves without an upstream
  `Cache-Control` has no lifetime of its own (`expires` is applied after the
  store decision) and is not stored. Only a request nginx answers on a first
  pass, under the URL exactly as sent, is stored or served: a page reached
  through an internal redirect (`index`, `try_files` to another file or a
  named location, `error_page`, `X-Accel-Redirect`), a percent-escaped path,
  a path with a dot segment or merged slashes, and a request line naming
  another host than `Host` are neither. The Markdown serve needs nginx 1.13.4
  or later. A 304 nginx answers for a page carries no `Vary: Accept`; a
  cache keeps the `Vary` of the 200 it revalidates.
- **nginx: the HTML cache decision log names what the server configuration
  added.** With C API 1.14, nginx's report-only lines now carry `vhost_added`
  as Apache's do: the names of the response headers `add_header` or
  `expires` added or changed.
