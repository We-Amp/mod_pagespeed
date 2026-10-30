- **A failed `nginx -s reload` no longer leaks memory or disturbs the running
  configuration.** Each configuration cycle now owns its own optimizer state, so
  a reload that fails to start leaves the live cycle's state tracked for a clean
  shutdown and releases the state it had built, including when the new
  configuration has no pagespeed server.
