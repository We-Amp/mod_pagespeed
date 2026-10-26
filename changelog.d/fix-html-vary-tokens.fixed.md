- **Rewritten HTML now names every request header that changes it in `Vary`.**
  With image optimization on, the markup chosen for a page can differ by the
  browser's image-format support (`Accept`), its data-saver preference
  (`Save-Data`) and its device (`User-Agent`). Pages rewritten with these
  optimizations now carry `Vary` naming exactly those headers. The value
  depends only on your configuration, not on the request or on cache state.
  With the default settings HTML is still served as not cacheable by shared
  caches, so most sites see no change. If you let a CDN or shared cache store
  rewritten HTML (for example with `ModifyCachingHeaders off` and a public
  `Cache-Control`), this stops one visitor's page from being served to
  another. Expect fewer HTML hits on caches that key on the full
  `User-Agent`. To keep HTML hit rates high, use `ImagePreserveURLs on` with
  the optimizer worker installed so images are selected at their own URLs, or
  normalize these headers at the cache as described in the deployment guide.
  Update recommended for sites that cache HTML at a CDN or proxy.
