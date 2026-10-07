// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Handler selection for the nginx port's admin-style handlers (statistics,
// global statistics, console, messages, admin, global admin).  Pulled out of
// ngx_pagespeed.cc so the decision can be unit-tested without nginx.
//
// The selection follows nginx `location` semantics, applied to the URI nginx
// itself matched its `location` blocks against:
//   - a handler path matches a request path exactly (byte-for-byte, so case
//     matters), like `location = /path`;
//   - the two admin handlers, which own a whole subtree, also match every
//     path below theirs, at a `/` boundary, like `location ^~ /path/`.
// A handler is served only when the server and the module select the same
// one for a request; in every other case the request is left to the server.

#ifndef PAGESPEED_NGINX_NGX_ADMIN_PATH_MATCH_H_
#define PAGESPEED_NGINX_NGX_ADMIN_PATH_MATCH_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// True when `path` (the request path without query) selects the handler
// configured at `handler_path`.  `subtree` is true for the admin handlers,
// which take their own path and every path below it; false for the
// exact-match handlers.  An empty `handler_path` never matches (the handler
// is unconfigured).
bool NgxAdminPathMatches(StringPiece path, StringPiece handler_path,
                         bool subtree);

enum class NgxAdminHandler {
  kNone,
  kStatistics,
  kGlobalStatistics,
  kConsole,
  kMessages,
  kAdmin,
  kGlobalAdmin,
};

// The configured handler paths.  An empty path means that handler is off.
struct NgxAdminHandlerPaths {
  StringPiece statistics;
  StringPiece global_statistics;
  StringPiece console;
  StringPiece messages;
  StringPiece admin;
  StringPiece global_admin;
};

// The handler `path` selects, or kNone.  When more than one configured path
// matches, the first in the order of NgxAdminHandler wins.
NgxAdminHandler NgxAdminHandlerForPath(StringPiece path,
                                       const NgxAdminHandlerPaths& paths);

struct NgxAdminRoute {
  // The handler to serve the request with; kNone leaves it to the server.
  NgxAdminHandler handler = NgxAdminHandler::kNone;
  // True when a handler path matched but the request is left to the server
  // all the same (see NgxDecideAdminRoute).  Only for reporting.
  bool declined = false;
};

// Decides which admin-style handler, if any, serves a request.
//
// `server_uri` is nginx's own normalized request URI (the one its `location`
// blocks were matched against) and selects the handler.  `module_path` is
// the path of the URL the module parsed for the request; it has to select
// the same handler, or the request is left to the server.  A request the
// server redirected internally, and a subrequest, is left to the server too
// (`server_redirected`).
NgxAdminRoute NgxDecideAdminRoute(StringPiece server_uri,
                                  StringPiece module_path,
                                  bool server_redirected,
                                  const NgxAdminHandlerPaths& paths);

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_ADMIN_PATH_MATCH_H_
