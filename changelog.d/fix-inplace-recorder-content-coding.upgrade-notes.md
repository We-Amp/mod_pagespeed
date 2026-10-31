- **In-place optimization behind an origin that compresses only with
  brotli.** Such an origin's responses are now served unoptimized. To keep
  optimizing them, let the origin answer the server's own fetches with gzip or
  without compression.
