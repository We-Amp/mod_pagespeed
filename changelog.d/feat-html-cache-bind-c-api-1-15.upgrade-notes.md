- **The cached HTML mode and the credentialed policy need optimizer C API
  1.15.** With an older optimizer library, `DaemonHtmlCache
  serve-cached-html` runs as `serve-variants`, logs one warning naming the
  library's version, and counts each HTML response in
  `daemon_html_serve_cached_html_unavailable`. On such a library
  `serve-variants` keeps its earlier cookie rule (a page the origin marks
  `public` or gives an `s-maxage` is stored and served for a request with a
  cookie, an analytics cookie included), and `DaemonHtmlCacheCredentialed`,
  `DaemonHtmlCacheIgnoreCookies` and `DaemonHtmlCacheIgnoreCookie` have no
  effect; when one is set to anything but its default, the server logs one
  warning saying so. Upgrade the optimizer first, then the module.
