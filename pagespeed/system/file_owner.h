// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Handing a file or directory the server created as root to the user its
// workers run as.

#ifndef PAGESPEED_SYSTEM_FILE_OWNER_H_
#define PAGESPEED_SYSTEM_FILE_OWNER_H_

#ifndef _WIN32

#include <sys/types.h>

#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

// Which existing ownership counts as already right.
enum class OwnerMatch {
  kUserAndGroup,  // change unless both the user and the group match
  kUserOnly,      // change only when the user differs
};

enum class SetOwnerResult {
  kDone,          // the owner is now uid:gid (or already matched)
  kSymbolicLink,  // the path is a symbolic link: left as it is
  kFailed,        // not done; the reason is in *error
};

// Gives `path` to uid:gid.  The path must be a plain file (one link) or a
// directory; a symbolic link is left as it is and reported as such, anything
// else is reported and left alone.  Trailing slashes are ignored.  Nothing
// changes when the ownership already matches per `match`.
SetOwnerResult SetOwnerThroughDescriptor(const GoogleString& path, uid_t uid,
                                         gid_t gid, OwnerMatch match,
                                         GoogleString* error);

}  // namespace net_instaweb

#endif  // !_WIN32

#endif  // PAGESPEED_SYSTEM_FILE_OWNER_H_
