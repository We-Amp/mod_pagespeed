- **Served-size statistics also count resources optimized in place.** The
  `image_served_*_bytes`, `css_served_*_bytes` and `javascript_served_*_bytes`
  variables now grow as well when an image, stylesheet or script is sent
  optimized under its own URL (in-place optimization), and when a rewritten URL
  that names an outdated hash is answered with the current optimized content.
  A response that carries the original bytes, because the optimized version
  was not ready in time or the resource could not be improved, is not counted.
  For a stylesheet whose URLs are corrected on the way out, the optimized size
  is that of the body sent.
