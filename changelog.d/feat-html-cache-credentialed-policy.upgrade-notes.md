- **What the cookie and credential policy changes for `serve-variants`.**
  With an optimizer library that publishes C API 1.15: a page the origin
  marks `public` is no longer stored for a request with a cookie outside the
  ignore list, and such a request no longer gets the Markdown copy, unless
  you set `DaemonHtmlCacheCredentialed explicit-public`. A request that
  carries only listed analytics cookies is now stored and answered without
  `public`. Every page stored before the upgrade goes to the origin once,
  for the page and its Markdown copy, and is then stored again. Behind a
  TLS-terminating proxy, set `RespectXForwardedProto on`: otherwise the
  forwarded scheme differs from the one the server keys on, nothing is
  stored or answered on that virtual host, and the server logs one warning
  saying so. A `DaemonHtmlCacheIgnoreCookie` addition that is `*` alone, has
  fewer than 3 characters before its `*`, or matches or covers a session
  cookie name fails the configuration on Apache and nginx (the
  configuration test included); on IIS it is logged as an error and the
  site's HTML cache stores and answers nothing. If you roll the module back
  past this release, purge each site's HTML cache after the rollback.
