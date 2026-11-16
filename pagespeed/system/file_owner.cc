// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/file_owner.h"

#ifndef _WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace net_instaweb {

SetOwnerResult SetOwnerThroughDescriptor(const GoogleString& path, uid_t uid,
                                         gid_t gid, OwnerMatch match,
                                         GoogleString* error) {
  // The path must be a plain file or directory owned by the configured
  // user: O_NOFOLLOW, and no wait on a path that is neither (O_NONBLOCK,
  // O_NOCTTY).  Trailing slashes are dropped first: the configured path may
  // carry them, and with one O_NOFOLLOW would not apply to the last name.
  GoogleString name = path;
  while (name.size() > 1 && name.back() == '/') {
    name.pop_back();
  }
  int fd = -1;
  do {
    fd = open(name.c_str(),
              O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    if (errno == ELOOP) {
      *error = "not a plain file or directory";
      return SetOwnerResult::kSymbolicLink;
    }
    *error = strerror(errno);
    return SetOwnerResult::kFailed;
  }
  bool ok = false;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    *error = strerror(errno);
  } else if (!S_ISDIR(st.st_mode) &&
             !(S_ISREG(st.st_mode) && st.st_nlink == 1)) {
    *error = "not a plain file or directory";
  } else {
    const bool already_right =
        st.st_uid == uid &&
        (match == OwnerMatch::kUserOnly || st.st_gid == gid);
    // fchown only when the owner is not already right.
    if (already_right || fchown(fd, uid, gid) == 0) {
      ok = true;
    } else {
      *error = strerror(errno);
    }
  }
  close(fd);
  return ok ? SetOwnerResult::kDone : SetOwnerResult::kFailed;
}

}  // namespace net_instaweb

#endif  // !_WIN32
