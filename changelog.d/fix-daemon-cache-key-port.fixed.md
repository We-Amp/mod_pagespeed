- **Resources recorded from sites that share a host name but use different
  ports are no longer served to each other.** The host half of the
  optimizer cache key now carries the site's port, so a site on
  `http://example.com/` and a site on `http://example.com:8080/` no longer
  resolve to the same entry; before, either could be served the other's
  script, stylesheet or image out of the shared cache. Operators serving
  one host name on several ports should purge the optimizer cache once
  after upgrading, because existing entries may already be mixed.
