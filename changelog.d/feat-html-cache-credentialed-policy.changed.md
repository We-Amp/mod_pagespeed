- **The HTML cache stores and answers anonymous requests only, for the page
  and its Markdown copy alike.** With an optimizer library that publishes C
  API 1.15, in `DaemonHtmlCache serve-variants` and `serve-cached-html` on
  Apache, nginx and IIS:
  a request with `Authorization`, `Proxy-Authorization` or an authenticated
  user is never stored and never looked up. A request with a cookie is stored
  and looked up only when every cookie is on the ignore list in force: the
  built-in list of analytics cookies (`DaemonHtmlCacheIgnoreCookies on`, the
  default: `_ga`, `_ga_*`, `_gid`, `_gat`, `_gat_*`, `_gcl_*`, `__utma`,
  `__utmb`, `__utmc`, `__utmt`, `__utmt_*`, `__utmv`, `__utmz`, `_fbp`,
  `_fbc`, `_hjid`, `_hjSession_*`, `_hjSessionUser_*`, `_hjFirstSeen`,
  `_hjAbsoluteSessionInProgress`, `_hjIncludedInPageviewSample`,
  `_hjIncludedInSessionSample`, `_hjTLDTest`, `_hjLocalStorageTest`,
  `_hjSessionStorageTest`, `_hjCookieTest`, `_hjViewportId`,
  `_hjRecordingEnabled`, `_hjRecordingLastActivity`, `_hjUserAttributesHash`,
  `_hjClosedSurveyInvites`, `_hjDonePolls`, `_clck`, `_clsk`, `_pk_id.*`,
  `_pk_ses.*`, `_pk_ref.*`, `_uetsid`, `_uetvid`, `_uetmsclkid`) and the
  `DaemonHtmlCacheIgnoreCookie` additions. Any other cookie, a session
  cookie whatever the additions say, a cookie line that does not parse, or
  more listed cookies than the cache can check sends the request to the
  origin. `DaemonHtmlCacheCredentialed explicit-public` lets a request with
  other cookies be stored and answered when the origin marks the page
  `public` or gives it an `s-maxage`, as before. A page whose body or stored
  headers carry the value of a listed cookie (8 bytes or longer, also
  percent-encoded) is never stored. A request that carries a header the
  cache key does not contain (`X-Forwarded-Host`, `X-Host`,
  `X-Original-Host`, `X-Original-URL`, `X-Rewrite-URL`, `X-Forwarded-Port`,
  `X-Forwarded-Scheme`, `X-Forwarded-Prefix`, `X-HTTP-Method-Override`,
  `X-Method-Override`, `Forwarded`, `X-Forwarded-Ssl`, `Front-End-Https`,
  `X-HTTP-Method`, `X-Forwarded-Protocol`, `X-Url-Scheme`,
  `X-Forwarded-Server`, `X-Original-URI`, `X-Forwarded-Uri`, in either
  spelling with `-` or `_`), an AJAX marker, or an `X-Forwarded-Proto` other
  than the scheme the server keys on, or whose path has a `;` parameter, an
  encoded slash or backslash, a dot segment or a static-asset extension, is
  neither stored nor answered. Each stored page records whether it came from
  an anonymous request and under which of these settings; a page stored
  under other settings, or by an earlier release, is never answered and is
  stored again on its next miss. The refusals are counted in
  `daemon_html_refused_auth`, `daemon_html_refused_cookie_request`,
  `daemon_html_refused_cookie_echo`, `daemon_html_refused_request_header`
  and `daemon_html_refused_path` (each lookup and each store), the decision
  log names the cookie or header (never its value), and
  `daemon_html_upgrade_restores` counts pages stored again after an
  upgrade. With an older optimizer library every cookie counts as a cookie,
  as before.
