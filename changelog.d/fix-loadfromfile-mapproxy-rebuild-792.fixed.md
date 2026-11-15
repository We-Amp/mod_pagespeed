- **Proxied resources no longer answer 404 after cache loss when a
  `LoadFromFile` prefix overlaps a `MapProxyDomain` path.** A resource under
  a proxied path is now rebuilt from the proxied origin instead of being read
  from a local file that does not exist.
