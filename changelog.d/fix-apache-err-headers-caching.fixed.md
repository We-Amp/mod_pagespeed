- **Apache: rewritten HTML no longer keeps an origin Cache-Control or Expires
  set by php-fpm, CGI or "Header always set".** Such values sit in
  `err_headers_out`, which httpd merges into the response at send time, so a
  rewritten page could carry the origin's `Cache-Control: public, max-age=600`
  (and an `s-maxage`) next to the module's `max-age=0, no-cache`, plus the
  origin's `Expires`: a shared cache taking the first `max-age` it saw could
  store the page for the origin's lifetime. The module now computes its
  no-cache value from both tables, unsets Cache-Control, Expires, ETag and
  Last-Modified in both, and skips HTML already rewritten upstream when the
  `X-Mod-Pagespeed` header arrives in `err_headers_out`. mod_php, which
  writes `headers_out` directly, was never affected.
