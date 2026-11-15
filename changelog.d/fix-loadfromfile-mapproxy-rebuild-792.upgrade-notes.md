- **With `MapProxyDomain`, a resource under a proxied path is now always
  fetched from the proxied origin, also when a `LoadFromFile` rule covers the
  same path.** This applies when a resource is created as a rewrite input, for
  example when a rewritten resource is rebuilt after cache loss. During HTML
  rewriting the input is the origin URL, so rewritten URLs do not change. If
  you mirrored proxied files under the document root to avoid origin fetches,
  those files are no longer read, and you will see more origin fetches in that
  setup.
