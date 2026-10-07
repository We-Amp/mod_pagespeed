// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_admin_path_match.h"

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

bool NgxAdminPathMatches(StringPiece path, StringPiece handler_path,
                         bool subtree) {
  if (handler_path.empty()) {
    return false;
  }
  // Byte-for-byte, like an nginx `location` block.
  if (path == handler_path) {
    return true;
  }
  if (!subtree || !path.starts_with(handler_path)) {
    return false;
  }
  // Below the handler path, and only at a `/` boundary.
  return handler_path[handler_path.size() - 1] == '/' ||
         path[handler_path.size()] == '/';
}

NgxAdminHandler NgxAdminHandlerForPath(StringPiece path,
                                       const NgxAdminHandlerPaths& paths) {
  if (NgxAdminPathMatches(path, paths.statistics, false)) {
    return NgxAdminHandler::kStatistics;
  }
  if (NgxAdminPathMatches(path, paths.global_statistics, false)) {
    return NgxAdminHandler::kGlobalStatistics;
  }
  if (NgxAdminPathMatches(path, paths.console, false)) {
    return NgxAdminHandler::kConsole;
  }
  if (NgxAdminPathMatches(path, paths.messages, false)) {
    return NgxAdminHandler::kMessages;
  }
  // The admin handlers get their path and everything below it (/path/*)
  // while all the other handlers only get exact matches (/path).
  if (NgxAdminPathMatches(path, paths.admin, true)) {
    return NgxAdminHandler::kAdmin;
  }
  if (NgxAdminPathMatches(path, paths.global_admin, true)) {
    return NgxAdminHandler::kGlobalAdmin;
  }
  return NgxAdminHandler::kNone;
}

NgxAdminRoute NgxDecideAdminRoute(StringPiece server_uri,
                                  StringPiece module_path,
                                  bool server_redirected,
                                  const NgxAdminHandlerPaths& paths) {
  NgxAdminRoute route;
  const NgxAdminHandler by_server = NgxAdminHandlerForPath(server_uri, paths);
  const NgxAdminHandler by_module = NgxAdminHandlerForPath(module_path, paths);
  if (by_server == NgxAdminHandler::kNone &&
      by_module == NgxAdminHandler::kNone) {
    return route;
  }
  if (server_redirected || by_server != by_module) {
    route.declined = true;
    return route;
  }
  route.handler = by_server;
  return route;
}

}  // namespace net_instaweb
