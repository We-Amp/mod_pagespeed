- **`DaemonHtmlCache serve-variants` works on IIS.** With `AgentOptimize on`,
  the IIS module stores the HTML pages it serves and answers an agent that
  asks for `text/markdown` with the page's Markdown copy, as Apache and nginx
  do. Notes for IIS:
  - A page is stored and served only for a host name one of the site's
    bindings names. A binding with an empty host name (`*:80:` or an address
    only) vouches for no name.
  - Only the plain spelling of a URL path is stored or served: lowercase
    ASCII, without percent-escapes, merged or doubled slashes,
    dot segments, 8.3 short names, a trailing dot, `;` parameters, `::$DATA`
    or path info after the file. IIS reads all of these as the same file, so
    a purge of one spelling could otherwise leave another serving. A site
    whose URLs carry capitals or non-ASCII characters gets no Markdown copy
    for them.
  - The Markdown answer carries only its own header set: the site's
    `customHeaders` (for example `Strict-Transport-Security` and
    `X-Content-Type-Options`) are not on it. IIS never compresses it: a
    brotli or gzip copy is sent as the optimizer stored it.
  - The answer is sent by the `PageSpeedFinalView` module entry, which the
    installer lists first. Each worker process serves Markdown for a site
    only after it has seen that entry run for one of the site's responses;
    a site without it gets HTML. Where another module replaces the answer
    first (a custom error page with `existingResponse="Replace"`) or a
    compressor encodes it, that request gets the error and every later one
    the page's HTML (logged once). Never a Markdown answer with the site's
    own headers on it.
  - Every HTML response of such a site is kept out of IIS's kernel and
    output caches, so an agent is never answered with cached HTML.
  - Each worker process reads the site's bindings once: a binding added or
    removed takes effect when the application pool recycles. An HTML page
    IIS cached before the option was turned on is answered from that cache
    until it expires; recycle the pool after turning it on.
  - On a site with inbound URL Rewrite rules, a `PURGE` request may be
    answered by the site itself and purge nothing; use the admin purge
    (`/pagespeed_admin/cache?purge=<url>`), which works there.
  - A `Range` or `If-Range` request is answered by IIS, never from a copy;
    one that asks for Markdown counts `daemon_markdown_fallthrough_range`.
  - `Vary: Accept` is added to the HTML pages the module rewrites; a page it
    passes through unchanged (a HEAD, an AJAX request, a disallowed URL)
    does not get it.
