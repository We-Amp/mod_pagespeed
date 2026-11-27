- **`DaemonHtmlCache serve-cached-html` is a mode of its own, and its settings
  are read.** With an optimizer library that publishes C API 1.15 the mode
  serves the Markdown copies of stored pages exactly as `serve-variants` does,
  and the HTML cache now reads `DaemonHtmlCacheServeScope`,
  `DaemonHtmlCacheMaxAge`, `DaemonHtmlCacheCredentialed`,
  `DaemonHtmlCacheIgnoreCookies` and `DaemonHtmlCacheIgnoreCookie` without the
  earlier "takes effect with serve-cached-html" warning. The mode does not yet
  answer a request with the stored page itself: such a request goes to the
  origin, as with `serve-variants`. The same lines load as before, and a line
  that failed to load still fails.
