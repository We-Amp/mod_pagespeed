- **On IIS, rewritten HTML keeps the origin's no-store and related caching
  directives, and ModifyCachingHeaders off is honoured for HTML.** Rewritten
  HTML went out with a bare `Cache-Control: no-cache`, dropping the
  `no-store` (and `no-transform`) the origin had sent, so pages marked
  never-store could land in browser disk and back/forward caches; IIS now
  sends the same disabled-caching `Cache-Control` as the Apache and nginx
  ports, which keeps those directives. `ModifyCachingHeaders off` was
  ignored for HTML requests that did not go through the classic in-place
  lookup (optimizer-service sites, in-place optimization off, POST
  requests): those still received `no-cache` and lost `Expires` and
  `Last-Modified`. The option now comes from the options resolved for the
  response, as on the other ports.
