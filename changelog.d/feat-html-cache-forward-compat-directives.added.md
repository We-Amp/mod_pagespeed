- **Configurations written for the upcoming cached HTML mode keep loading.**
  `DaemonHtmlCacheServeScope agents|all`, `DaemonHtmlCacheMaxAge` (0 to 86400
  seconds, default 3600; it only ever shortens the lifetime the origin gives
  a page), `DaemonHtmlCacheCredentialed anonymous-only|explicit-public`,
  `DaemonHtmlCacheIgnoreCookies on|off` and the repeatable
  `DaemonHtmlCacheIgnoreCookie <name|prefix*>` are accepted on Apache, nginx,
  IIS and Envoy, with a warning each that they take effect with
  `DaemonHtmlCache serve-cached-html`; in this release they have no effect. An
  invalid value is still a configuration error. `DaemonHtmlCache
  serve-cached-html` now runs as `serve-variants`, with a warning, instead of
  being ignored: it serves the Markdown copies of stored pages and never serves
  stored HTML. A configuration written for the release that adds the mode
  therefore keeps the server starting, and Markdown serving, if the package is
  rolled back to this one.
