- **What the cookie and credential policy changes for `serve-variants`.**
  With an optimizer library that publishes C API 1.15: a page the origin
  marks `public` is no longer stored for a request with a cookie outside the
  ignore list, and such a request no longer gets the Markdown copy, unless
  you set `DaemonHtmlCacheCredentialed explicit-public`. A request that
  carries only listed analytics cookies is now stored and answered without
  `public`. A page stored before the upgrade is not answered as Markdown
  until its next miss stores it again. Behind a TLS-terminating proxy, set
  `RespectXForwardedProto on` (Apache, nginx and IIS key the page on the
  forwarded scheme then), but only where every request reaches the server
  through that proxy and the proxy sets the header itself; otherwise the
  forwarded scheme differs from the one the server keys on, nothing is
  stored or answered on that virtual host or site, and the server logs one
  warning saying so. `report-only` applies the same request policy, so its
  numbers change with a 1.15 library too: a request that carries only
  listed analytics cookies counts as one that would be stored, and a request
  the policy refuses for a request header, its path or a mismatched
  `X-Forwarded-Proto` is no longer evaluated (it counts in
  `daemon_html_refused_request_header` or `daemon_html_refused_path`
  instead of `daemon_html_evaluated`). On IIS, URL Rewrite adds
  `X-Original-URL` to a rewritten request, holding the request target as
  the client sent it; such a request is stored and answered as before. A
  request whose `X-Original-URL` differs from its target (another path or
  query, another case or percent-encoding, a second value that differs) is
  neither stored nor answered, and the site logs one warning saying so. A `DaemonHtmlCacheIgnoreCookie` addition that is `*` alone, has
  fewer than 3 characters before its `*`, or matches or covers a session
  cookie name fails the configuration on Apache and nginx (the
  configuration test included); on IIS it is written to the event log as
  an error, whatever `UseEventLog` says, and the site's HTML cache stores
  and answers nothing. A configuration test on a server without the
  optimizer library does not check the additions; the server then checks
  them when it starts with the library. If you roll the module back
  past this release, purge each site's HTML cache after the rollback.
