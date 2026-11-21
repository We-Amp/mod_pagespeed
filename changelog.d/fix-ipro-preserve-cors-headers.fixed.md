- Fixed: with the optimizer daemon serving in-place resources, a cross-origin
  CORS request (a web font, a `crossorigin` or module script, a `fetch()`) could
  be answered with a stored copy that had no `Access-Control-Allow-Origin`, so
  the browser blocked it when the origin sends that header only to requests
  carrying `Origin`. Such requests are now answered by the server itself, with
  its own CORS headers, and their responses are not stored as the copy other
  requests are served from. `InPlaceCorsGuard off` restores the old behaviour.
