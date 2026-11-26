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
    `X-Content-Type-Options`) are not on it, and it is never compressed.
  - The answer is sent by the `PageSpeedFinalView` module entry, which the
    installer lists first. Where that entry does not run, the first Markdown
    request gets an error and every later one the page's HTML (logged once);
    where another module replaces the response first (a custom error page),
    the client gets that response. Never a Markdown answer with the site's
    own headers on it.
  - Every HTML response of such a site is kept out of IIS's kernel and
    output caches, so an agent is never answered with cached HTML.
