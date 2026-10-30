// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Path matching for the IIS port's admin handlers (admin and global admin),
// split out of iis_http_module.cpp so it can be unit-tested without IIS.
//
// A handler path selects a request path when the request path is the handler
// path itself or lies below it: the character after the handler path must be
// '/' or '?', or the path must end there.  Matching is case-insensitive, like
// the other IIS handler paths.

#ifndef PAGESPEED_IIS_IIS_ADMIN_PATH_MATCH_H_
#define PAGESPEED_IIS_IIS_ADMIN_PATH_MATCH_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// True when `path` selects the admin-style handler configured at
// `handler_path`.  An empty `handler_path` never matches (the handler is
// unconfigured).
bool IisAdminPathMatches(StringPiece path, StringPiece handler_path);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_ADMIN_PATH_MATCH_H_
