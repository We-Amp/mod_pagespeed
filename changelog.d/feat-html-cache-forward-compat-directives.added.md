- **The cached HTML mode's settings are accepted on every server.**
  `DaemonHtmlCacheServeScope agents|all`, `DaemonHtmlCacheMaxAge` (0 to 86400
  seconds, default 3600; it only ever shortens the lifetime the origin gives
  a page), `DaemonHtmlCacheCredentialed anonymous-only|explicit-public`,
  `DaemonHtmlCacheIgnoreCookies on|off` and the repeatable
  `DaemonHtmlCacheIgnoreCookie <name|prefix*>` (one name or prefix per line;
  on Envoy, where a virtual host option is set once, a comma-separated list)
  are accepted on Apache, nginx, IIS and Envoy. `DaemonHtmlCacheServeScope`,
  `DaemonHtmlCacheMaxAge` and `DaemonHtmlCacheCredentialed` take effect in
  this release (see `DaemonHtmlCache serve-cached-html`); the two
  ignore-cookie settings are checked now and take effect in a later release.
  An invalid value is refused, as for any other directive: a configuration
  error on Apache and nginx, a logged and skipped line on IIS and Envoy.
