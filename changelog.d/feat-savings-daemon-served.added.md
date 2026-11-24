- **Served-size statistics for responses answered from the optimizer cache.**
  When an optimized image, stylesheet or script is answered from the optimizer
  cache, its sizes are now counted each time it is sent, in six new variables on
  the statistics page: `ipro_daemon_served_image_original_bytes` and
  `ipro_daemon_served_image_optimized_bytes`, and the same pair for `css` and
  `js`. Before, such a response moved `ipro_daemon_served_image` (or `_css`,
  `_js`) and no size variable. They are a family of their own next to
  `image_served_*_bytes`, `css_served_*_bytes` and `javascript_served_*_bytes`:
  a request is answered either by the server's own rewriting or from the
  optimizer cache, never both, so no response is counted twice.
  Limits: HEAD, range and conditional (304) requests are excluded. A response
  sent uncompressed is measured against the uncompressed original. A stored
  compressed copy (`DaemonServeStoredEncodings`) is measured against the
  original in that same compression when the cache entry records that size, and
  is not counted when it does not. Applies to nginx, Apache and IIS.
