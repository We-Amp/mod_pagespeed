// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Names the static asset a request asks for in the nginx port.  Pulled out of
// ngx_pagespeed.cc so the decision can be unit-tested without nginx.
//
// A request has two readings of its path: the one the server works with and
// the one the module derives from the request line.  The static asset
// handler answers only when both readings are the same path and that path is
// a file directly under the static asset prefix; every other request is left
// to the server.

#ifndef PAGESPEED_NGINX_NGX_STATIC_ASSET_PATH_H_
#define PAGESPEED_NGINX_NGX_STATIC_ASSET_PATH_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Returns the asset file name -- the part of the path after `prefix` -- when
// `server_uri` and `module_path` (both without query) are identical and name
// a file directly under `prefix`.  Returns an empty StringPiece otherwise:
// the two readings differ, the path is not under `prefix`, nothing follows
// the prefix, what follows contains a '/', or `prefix` is empty or does not
// end in '/'.  The result points into `server_uri`.
StringPiece NgxStaticAssetName(StringPiece server_uri, StringPiece module_path,
                               StringPiece prefix);

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_STATIC_ASSET_PATH_H_
