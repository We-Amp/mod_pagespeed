- **Hardening: the diagnostics bundle redacts the cache invalidation,
  rejected request and caching wildcard sections.** With the configuration
  text added, the URL patterns in those sections are replaced; a rejected
  request entry keeps only its header name.
