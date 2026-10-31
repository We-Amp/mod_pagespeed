- Responses to requests that carry an `Authorization` header no longer get a
  cache lifetime that lets a shared cache, such as a CDN or a caching proxy,
  store them and serve them to other clients. Affected: 2.6.0 and earlier.
  Update recommended for nginx and Apache sites behind a shared cache.
