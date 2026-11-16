- **A `LoadFromFile` rule written for a `MapProxyDomain` origin now also
  applies when a proxied resource is rebuilt after cache loss.** Previously
  the rule applied while the HTML was rewritten but not on rebuild, so the
  rebuild fetched the origin over the network instead of reading the file.
