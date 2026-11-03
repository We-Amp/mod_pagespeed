- A conditional request for an optimized resource now gets a 304 that carries
  the same caching headers as the full response, so browsers that cached it
  earlier learn its long lifetime after one revalidation, and those that
  honour `immutable` (Firefox, Safari) stop revalidating it. Resources whose
  input does not allow long caching, and entries cached before the upgrade,
  still get a plain 304 until they are optimized again.
