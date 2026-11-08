// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_caching_header_policy.h"

namespace net_instaweb {

// See build_context_for_request() in mod_instaweb.cc.
PreserveCachingHeaders NgxPreserveCachingHeadersFor(
    const NgxCachingHeaderRequest& request) {
  if (request.pagespeed_resource) {
    // The module's own response: its caching headers are the ones that
    // describe it, whatever ModifyCachingHeaders says about pages.  Keeping
    // the server's instead would send it out with none at all.
    return kDontPreserveHeaders;
  }
  if (!request.modify_caching_headers) {
    return kPreserveAllCachingHeaders;
  }
  if (!request.downstream_cache_integration) {
    // Downstream cache integration is not enabled. Disable original
    // Cache-Control headers.
    return kDontPreserveHeaders;
  }
  if (request.admin_handler) {
    return kDontPreserveHeaders;
  }
  // Downstream cache integration is enabled. If a rebeaconing key has been
  // configured and there is a ShouldBeacon header with the correct key,
  // disable original Cache-Control headers so that the instrumented page is
  // served out with no-cache.
  if (request.rebeaconing_key_matches) {
    return kDontPreserveHeaders;
  }
  return kPreserveOnlyCacheControl;
}

}  // namespace net_instaweb
