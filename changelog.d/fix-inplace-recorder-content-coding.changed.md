- **In-place optimization no longer records origin responses with a content
  coding the server cannot decode**, such as brotli, zstd or several codings
  stacked. Such URLs are served as the origin sends them until the origin
  negotiates gzip or no compression. See Upgrade notes.
