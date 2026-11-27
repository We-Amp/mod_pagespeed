// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/base/atomic_file_writer.h"

#include <cerrno>
#include <string>
#include <string_view>

#include "lib/base/posix_compat.h"

namespace pagespeed {

bool AtomicWriteFile(const std::string& path, std::string_view content,
                     int mode) {
  if (path.empty()) {
    return false;
  }

  std::string tmp_path = path + ".tmp";

  // Remove stale tmp (could be a leftover or a symlink).
  ps_unlink(tmp_path.c_str());

  // Exclusive create + refuse to follow symlinks (defense-in-depth).
  int fd = ps_open_exclusive_nofollow(tmp_path.c_str(), mode);
  if (fd < 0) {
    return false;
  }

#ifndef _WIN32
  // The open() mode bits are masked by the process umask; make the
  // requested mode exact so no caller's correctness depends on umask
  // (every security-relevant mode is set explicitly).
  if (::fchmod(fd, static_cast<mode_t>(mode)) != 0) {
    int saved_errno = errno;
    ps_unlink(tmp_path.c_str());
    ps_close(fd);
    errno = saved_errno;
    return false;
  }
#endif

  // Write the full content, looping over short writes. A bare write() can
  // return a positive short count — ENOSPC with partial room, or an
  // RLIMIT_FSIZE ceiling — WITHOUT setting errno, so testing
  // `written == size` alone would report a stale reason (e.g. an ENOENT
  // left over from an unrelated unlink, i.e. "No such file or directory"
  // for a full disk) or even "Success".
  size_t written = 0;
  bool write_ok = true;
  while (written < content.size()) {
    ssize_t n =
        ps_write(fd, content.data() + written, content.size() - written);
    if (n < 0) {
#ifdef EINTR
      if (errno == EINTR) continue;
#endif
      write_ok = false;  // errno holds the real reason.
      break;
    }
    if (n == 0) {
      // No progress and no errno: fabricate a truthful reason rather than
      // reporting a stale one.
      errno = ENOSPC;
      write_ok = false;
      break;
    }
    written += static_cast<size_t>(n);
  }

  // Short-circuit: skip fsync if write already failed.
  if (!write_ok || ::fsync(fd) != 0) {
    // Preserve the failure reason across cleanup so callers can report it.
    int saved_errno = errno;
    // Write or fsync failed — close fd and remove temp.
    // On Windows: close first (open files can't be unlinked).
    // On POSIX: unlink first (removes dir entry while fd still valid).
#ifdef _WIN32
    ps_close(fd);
    ps_unlink(tmp_path.c_str());
#else
    ps_unlink(tmp_path.c_str());
    ps_close(fd);
#endif
    errno = saved_errno;
    return false;
  }

  // Close the fd and check for errors (close can fail on NFS, etc.).
  if (ps_close(fd) != 0) {
    int saved_errno = errno;
    // fd state is unspecified after failed close — do not retry close.
    ps_unlink(tmp_path.c_str());
    errno = saved_errno;
    return false;
  }

  // Atomic rename: replaces destination if it exists.
  if (ps_rename(tmp_path.c_str(), path.c_str()) != 0) {
    int saved_errno = errno;
    ps_unlink(tmp_path.c_str());
    errno = saved_errno;
    return false;
  }

  return true;
}

}  // namespace pagespeed
