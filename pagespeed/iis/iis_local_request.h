// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The "is this request local" decision for the IIS port's admin, statistics,
// console, message and diagnostic pages, split out of iis_http_module.cpp so
// it can be unit-tested without IIS.
//
// A request is local when the client address is a loopback address or equals
// the server's own address.  A request that carries reverse-proxy forwarding
// headers was relayed by a proxy, so its client address is the proxy's, not
// the visitor's: it only counts as local when the operator opted in with
// TreatProxiedRequestsAsLocal.  Header values are never interpreted; their
// presence is enough.

#ifndef PAGESPEED_IIS_IIS_LOCAL_REQUEST_H_
#define PAGESPEED_IIS_IIS_LOCAL_REQUEST_H_

#include <functional>

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Request header names that identify a request relayed by a reverse proxy or
// tunnel.  Compared by presence only.
extern const char* const kIisForwardingHeaders[];
extern const int kIisForwardingHeaderCount;

// True when the request has at least one of kIisForwardingHeaders.
// `has_header` answers whether a request header (by name) is present.
bool IisHasForwardingHeaders(const std::function<bool(const char*)>& has_header);

// True when the request counts as local for the admin-style pages.
//   peer_is_loopback:   the client address is a loopback address.
//   remote_ip/local_ip: textual client and server addresses; equal means the
//                       client is the server itself.
//   has_forwarding_headers: see IisHasForwardingHeaders().
//   treat_proxied_as_local: the site option TreatProxiedRequestsAsLocal.
bool IisIsLocalRequest(bool peer_is_loopback, StringPiece remote_ip,
                       StringPiece local_ip, bool has_forwarding_headers,
                       bool treat_proxied_as_local);

// Access decision for an admin-style page.
//   local_only:           InfoUrlsLocalOnly (default on).
//   is_local:             IisIsLocalRequest().
//   allowlist_configured: a *Domains allowlist exists for this page.
//   host_on_allowlist:    the request host matches that allowlist.
// A host on a configured allowlist is always allowed.  Otherwise the page is
// served to local requests only, or, with InfoUrlsLocalOnly off, to everyone
// unless an allowlist is configured (in which case the host must match).
bool IisAdminAccessAllowed(bool local_only, bool is_local,
                           bool allowlist_configured, bool host_on_allowlist);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_LOCAL_REQUEST_H_
