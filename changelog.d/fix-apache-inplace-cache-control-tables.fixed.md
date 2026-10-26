- **Apache: in-place `s-maxage` no longer produces two `Cache-Control`
  headers when the origin sets it outside the normal header table.** A
  `Cache-Control` written by php-fpm, CGI or `Header always set` was not
  seen when the module added `s-maxage` to a static resource, so the response
  carried both the origin's header and the module's. The origin's directives
  are now merged with `s-maxage` into a single `Cache-Control` header.
