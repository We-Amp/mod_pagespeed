- **Conditional requests to resources served from the optimizer cache
  follow HTTP more closely.** If a request carries both `If-None-Match`
  and `If-Modified-Since`, only the entity tag is used, as HTTP requires.
  Before, a resource that had changed but kept an older modification date
  could be answered `304 Not Modified`, and the client kept the outdated
  copy. Entity tags that contain a comma are now recognized, so such a
  resource is revalidated with a `304` instead of being sent in full, and
  `If-None-Match: *` is answered with `304 Not Modified` when the resource
  is served from the cache. Applies to Apache, nginx and IIS with the
  optimizer worker. Update recommended.
- **A stored original that is re-recorded while it is being served goes
  out with the headers of the copy actually sent.** In a narrow timing
  window the response could miss `Vary: Accept`, or go out when the new
  copy should have been passed to the origin. Applies to Apache, nginx
  and IIS with the optimizer worker.
