- **Response headers no longer include the product version by default.** The
  `X-Mod-Pagespeed` (Apache) and `X-Page-Speed` (nginx, IIS, Envoy) headers now
  report `on`; detection by the presence of the header is unchanged. Set the
  new server-wide option `ExposeVersionInHeaders on` to restore the version
  (`ModPagespeedExposeVersionInHeaders on` in Apache, `pagespeed
  ExposeVersionInHeaders on;` in nginx). An explicit `XHeaderValue` still wins.
