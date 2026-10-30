- **Resource and admin responses carry each header once.** An
  `AddResourceHeader` for a header the origin already sent (for example
  `Access-Control-Allow-Origin`) now replaces it instead of sending two values,
  and, on Apache, `X-Content-Type-Options: nosniff` is no longer repeated on the
  admin and daemon JSON responses or on in-place optimized responses. Reported upstream in
  https://github.com/apache/incubator-pagespeed-mod/issues/1760 and
  https://github.com/apache/incubator-pagespeed-mod/issues/1988.
