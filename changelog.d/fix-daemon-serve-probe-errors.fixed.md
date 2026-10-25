- **A failed read of a stored original no longer lets an optimized variant
  through without the origin's headers.** Before serving from the optimizer
  cache, the module reads the stored original to learn whether the origin
  sent headers a cached response cannot reproduce (for example
  `Access-Control-Allow-Origin`) or `Vary: Accept`. When that read failed
  with busy, an I/O error or a corrupted entry, the failure was treated as
  "the origin sent no such headers" and the optimized variant was served
  without them, for its full cache lifetime. Any such read failure now
  falls through to the origin for that request, like a cache miss. Applies
  to Apache, nginx and IIS with the optimizer worker.
- **Resources whose origin states its lifetime only with `Expires` are
  served with that lifetime, not a default.** When recording a response,
  the module stored the origin's lifetime from `Cache-Control` only; a
  response that stated it only with `Expires` was stored as if it had none,
  and the optimizer's freshness evaluation then applied its type defaults
  (300 seconds for CSS and JavaScript, 3600 seconds for images), so a
  change at the origin could stay hidden behind a much longer emitted
  `Cache-Control` lifetime. The recorded entry now carries the lifetime
  derived from `Expires` relative to the response `Date` (an `Expires`
  already in the past is recorded as not freshly cacheable), so the served
  `Cache-Control` matches what the origin granted. Applies to Apache,
  nginx and IIS with the optimizer worker.
