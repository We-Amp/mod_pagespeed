- **With `MapProxyDomain`, a `LoadFromFile` rule for the proxied origin now
  also takes effect when a rewritten resource is rebuilt.** If you have such a
  rule, rebuilt resources are read from the mapped files instead of being
  fetched from the origin, which matches how they are read during HTML
  rewriting. A `LoadFromFile` rule that covers only the proxy path itself is
  still ignored for proxied resources.
