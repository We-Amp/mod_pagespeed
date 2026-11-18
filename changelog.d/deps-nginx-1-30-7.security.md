- **The nginx we bundle is updated to 1.30.7, nginx's security release of
  2026-11-18, which fixes nine security issues in nginx: memory-safety issues
  (among them in HTTP/2, HTTP/3, the resolver and the mail proxy), two
  TLS-verification and access-restriction bypasses in the stream module, and a
  denial-of-service loop (CVE-2026-125283, CVE-2026-123007, CVE-2026-123633,
  CVE-2026-121810, CVE-2026-123634, CVE-2026-113425, CVE-2026-120967,
  CVE-2026-123640, CVE-2026-118899).** This covers the sidecar native pair;
  the portable `ngx_pagespeed` module tarballs are built against nginx 1.30.7.
  **Update recommended.** The per-distribution `nginx-module-pagespeed`
  packages build against the distribution's own nginx, which the
  distribution's security updates cover.
