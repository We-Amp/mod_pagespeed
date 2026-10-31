- In-place optimization no longer stores a compressed origin response under
  the wrong encoding, which could leave stylesheets and scripts unreadable for
  every visitor. Affected: 2.6.0 and earlier, where the origin compresses with
  brotli or zstd. Update recommended for nginx and Apache sites that proxy to
  an origin.
