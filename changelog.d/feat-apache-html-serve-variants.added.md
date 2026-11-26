- **Apache: `DaemonHtmlCache serve-variants` stores pages and serves their
  Markdown copy.** With `ModPagespeedDaemonHtmlCache serve-variants`,
  `ModPagespeedAgentOptimize on` and an optimizer worker in a Markdown HTML
  mode with agent-optimize on, Apache now keeps a copy of each HTML page it
  serves that its headers allow a shared cache to keep, and answers a
  request that prefers `text/markdown` with the Markdown copy the worker
  derives from it. What an operator can rely on:
  - Only pages requested under one of the virtual host's own configured
    names (`ServerName` or an exact `ServerAlias`) are stored or served;
    wildcard aliases and requests that reach the default host are not.
  - Headers your configuration adds after the page leaves the origin
    (`Header set Cache-Control no-store`, `private`, `Vary: Cookie`, a
    shorter `max-age`) are honoured: the page is refused, or kept for the
    shorter lifetime. Those headers never become part of the stored copy.
  - Access, authentication and authorization run before any Markdown
    answer; a request with cookies, credentials, `Range`, `If-Range` or
    `Origin` gets the HTML page.
  - The Markdown answer carries exactly `Content-Type`, `Content-Length`,
    `Cache-Control: private`, one `Vary` line, `X-Robots-Tag: noindex`,
    `Age` and a weak `ETag`, and is never compressed or rewritten by another
    output filter. Headers your configuration adds to every response
    (`Header always set`, including `Strict-Transport-Security`) are not
    sent with it. A matching `If-None-Match` gets a `304`, also for a page
    served through `DirectoryIndex`.
  - A URL purge (the purge method or `?purge=`), a global purge and a touch
    of `cache.flush` remove or fence the stored copy; a URL purge that cannot
    remove it fails.
  - Markdown is served with in-place optimization off, and never under a
    location where PageSpeed is off.
  - A site whose server configuration adds `CDN-Cache-Control` or
    `Surrogate-Control` to every HTML response gets no HTML cache: its pages
    are not stored.
  Without a worker in a Markdown mode, or with an optimizer library that
  does not publish the HTML serve set, the mode stays report-only and logs
  why once.
