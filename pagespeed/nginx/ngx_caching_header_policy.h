// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Which caching headers the nginx port keeps from the server's own response
// instead of the ones the module computes.  Pulled out of ngx_pagespeed.cc so
// the decision can be unit-tested without nginx.
//
// ModifyCachingHeaders off is about HTML: the operator's caching headers on a
// page stay as they are.  A .pagespeed. resource is the module's own response
// and always carries the module's caching headers (the long-lived ones when
// the hash matches, the short private ones on the fallback path), as on the
// other servers.

#ifndef PAGESPEED_NGINX_NGX_CACHING_HEADER_POLICY_H_
#define PAGESPEED_NGINX_NGX_CACHING_HEADER_POLICY_H_

namespace net_instaweb {

enum PreserveCachingHeaders {
  kPreserveAllCachingHeaders,  // Cache-Control, ETag, Last-Modified, etc
  kPreserveOnlyCacheControl,   // Only Cache-Control.
  kDontPreserveHeaders,
};

struct NgxCachingHeaderRequest {
  // The ModifyCachingHeaders option for the request.
  bool modify_caching_headers = true;
  // Downstream cache integration is configured.
  bool downstream_cache_integration = false;
  // The request names a .pagespeed. resource.
  bool pagespeed_resource = false;
  // The request is for one of the module's admin-style handlers.
  bool admin_handler = false;
  // The request carries the configured downstream cache rebeaconing key.
  bool rebeaconing_key_matches = false;
};

// The caching headers of the server's response that the module keeps for a
// request it handles.
PreserveCachingHeaders NgxPreserveCachingHeadersFor(
    const NgxCachingHeaderRequest& request);

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_CACHING_HEADER_POLICY_H_
