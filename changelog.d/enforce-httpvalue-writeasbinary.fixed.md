- **A failed response-header serialization no longer stores a silent,
  headerless value.** When response headers could not be serialized into
  the shared HTTP value, the empty result could read back as a valid
  response with no headers; the value is now dropped, so the lookup
  fails and the content is fetched again.
