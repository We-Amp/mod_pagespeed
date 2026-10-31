// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Decides whether the module's reading of a request names what the server
// routed on, for the nginx port's in-place optimization.  Pulled out of
// ngx_pagespeed.cc so the decision can be unit-tested without nginx.
//
// A request has two readings: the server's, on which it selected the
// location, applied the access rules and chooses the response, and the
// module's, the URL it parsed from the request line and keys its in-place
// lookups and recordings on.  The two parsers normalize differently, so one
// key could be reached by spellings the server treats as different
// resources.  In-place optimization acts only when both readings name the
// same path on the same host; every other request is left to the server.

#ifndef PAGESPEED_NGINX_NGX_INPLACE_URL_AGREEMENT_H_
#define PAGESPEED_NGINX_NGX_INPLACE_URL_AGREEMENT_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// True when the server and the module read the request alike.  `server_uri`
// is the server's normalized request path (percent-decoded, dot segments
// resolved, slash runs merged when the server merges them, no query);
// `server_host` is the host the server matched the request to.
// `module_path` is the path of the URL the module parsed, in its canonical,
// percent-encoded form and without query; `module_host` is that URL's host.
// `merge_slashes` is the server's setting for the request: when set, runs of
// slashes in the module's decoded path are merged as the server merged them.
// The module's path is decoded (and merged) and compared byte for byte with
// the server's; the hosts are compared without regard to case.
bool NgxInPlaceUrlAgrees(StringPiece server_uri, StringPiece server_host,
                         StringPiece module_path, StringPiece module_host,
                         bool merge_slashes);

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_INPLACE_URL_AGREEMENT_H_
