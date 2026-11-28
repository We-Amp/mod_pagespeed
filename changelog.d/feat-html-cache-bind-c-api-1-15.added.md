- **`DaemonHtmlCache serve-cached-html` is a mode of its own.** With an
  optimizer library that publishes C API 1.15 the mode serves the Markdown
  copies of stored pages exactly as `serve-variants` does. It does not yet
  answer a request with the stored page itself: such a request goes to the
  origin, as with `serve-variants`. `DaemonHtmlCacheCredentialed` takes effect
  with a 1.15 library, in `serve-variants` and in `serve-cached-html`.
  `DaemonHtmlCacheServeScope` and `DaemonHtmlCacheMaxAge` are accepted and
  passed to the library, and apply to the stored-page answer, which a later
  release adds; until then they change nothing. `DaemonHtmlCacheIgnoreCookies`
  and `DaemonHtmlCacheIgnoreCookie` take effect with a 1.15 library too (see
  the cookie and credential policy entry). The same lines load as before, and
  a line that failed to load still fails.
