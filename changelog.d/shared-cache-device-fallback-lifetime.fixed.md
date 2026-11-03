- **A fallback image no longer sticks in a shared cache.** While a client's own
  image variant was being written, the variant served in its place carried the
  image's full cache lifetime, so a cache such as nginx proxy_cache could keep
  handing a desktop client a phone-sized image until it expired. Such a response
  now has a lifetime of at most 60 seconds; the client's own variant keeps the
  full lifetime.
