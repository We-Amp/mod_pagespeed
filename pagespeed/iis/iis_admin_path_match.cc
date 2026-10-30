// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_admin_path_match.h"

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

bool IisAdminPathMatches(StringPiece path, StringPiece handler_path) {
  if (handler_path.empty()) {
    return false;
  }
  if (!StringCaseStartsWith(path, handler_path)) {
    return false;
  }
  if (path.size() == handler_path.size()) {
    return true;
  }
  // Below the handler path, and only at a path boundary.
  if (handler_path[handler_path.size() - 1] == '/') {
    return true;
  }
  const char next = path[handler_path.size()];
  return next == '/' || next == '?';
}

}  // namespace net_instaweb
