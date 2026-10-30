// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_local_request.h"

namespace net_instaweb {

const char* const kIisForwardingHeaders[] = {
    "X-Forwarded-For",   "X-Forwarded-Host",   "X-Forwarded-Proto",
    "X-Forwarded-Server", "X-Forwarded-Port",  "Forwarded",
    "X-ARR-ClientIP",    "X-ARR-SSL",          "X-ARR-LOG-ID",
    "X-Real-IP",         "X-Client-IP",        "True-Client-IP",
    "CF-Connecting-IP",  "CF-Ray",             "CDN-Loop",
    "Via",               "X-Original-URL",     "X-Original-Host",
    "X-Rewrite-URL",     "X-Azure-ClientIP",   "X-Azure-SocketIP",
    "X-Azure-FDID",      "Fastly-Client-IP",   "X-Cluster-Client-IP",
    "X-Envoy-External-Address",
};
const int kIisForwardingHeaderCount =
    sizeof(kIisForwardingHeaders) / sizeof(kIisForwardingHeaders[0]);

bool IisHasForwardingHeaders(
    const std::function<bool(const char*)>& has_header) {
  for (int i = 0; i < kIisForwardingHeaderCount; ++i) {
    if (has_header(kIisForwardingHeaders[i])) {
      return true;
    }
  }
  return false;
}

bool IisIsLocalRequest(bool peer_is_loopback, StringPiece remote_ip,
                       StringPiece local_ip, bool has_forwarding_headers,
                       bool treat_proxied_as_local) {
  const bool peer_is_local =
      peer_is_loopback || (!remote_ip.empty() && remote_ip == local_ip);
  if (!peer_is_local) {
    return false;
  }
  return !has_forwarding_headers || treat_proxied_as_local;
}

bool IisAdminAccessAllowed(bool local_only, bool is_local,
                           bool allowlist_configured, bool host_on_allowlist) {
  if (allowlist_configured && host_on_allowlist) {
    return true;
  }
  if (!local_only) {
    return !allowlist_configured;
  }
  return is_local;
}

}  // namespace net_instaweb
