- **In-place optimization no longer serves a script, stylesheet, image or font
  without the CORS headers a cross-origin request needs.** A request that
  carries `Origin` (a `crossorigin` or module script, a web font, a `fetch()`)
  is given an in-place copy only when the copy's own
  `Access-Control-Allow-Origin` is `*` or names that origin (for a request
  sent with cookies or credentials, only when it names that origin and the
  copy allows credentials, as browsers require). Otherwise the
  server answers it directly, or, when proxying, the resource is fetched for
  that visitor and not stored for others. Responses whose
  `Access-Control-Allow-Origin` names one origin, or that carry `Vary: Origin`,
  are no longer optimized or recorded in place, so such a response can no
  longer replace the copy other visitors are served. Origins that send
  `Access-Control-Allow-Origin: *` keep full optimization. The new
  `InPlaceCorsGuard` option (on by default) turns this off, and the
  `in_place_cors_declined` statistic counts the requests it passed on.
