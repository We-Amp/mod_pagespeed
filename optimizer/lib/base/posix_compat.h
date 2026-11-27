// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// POSIX compatibility layer for Windows (MSVC).
// Include this instead of POSIX headers in cross-platform code.
//
// This header lives in lib/base/ so both lib/ and src/ can use it.
// src/worker/posix_compat.h is a thin wrapper that includes this file.
//
// IMPORTANT: Do NOT use macros to remap POSIX names — they conflict with
// MSVC STL internals (e.g., std::filebuf::open). Instead, use the ps_*
// inline wrappers provided here.

#ifndef LIB_BASE_POSIX_COMPAT_H_
#define LIB_BASE_POSIX_COMPAT_H_

#ifdef _WIN32

#include <BaseTsd.h>  // SSIZE_T
#include <direct.h>   // _mkdir
#include <fcntl.h>
#include <io.h>       // _open, _close, _write, _read, _unlink, _get_osfhandle
#include <process.h>  // _getpid
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>

#include <cerrno>
#include <cstring>
#include <string>

typedef SSIZE_T ssize_t;
typedef int pid_t;

// O_NOFOLLOW is a no-op on Windows; callers must use
// ps_open_exclusive_nofollow() for symlink safety.
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

// Use FlushFileBuffers() instead of _commit() for reliable durability on
// all Windows file systems (network shares, USB, etc.).
inline int fsync(int fd) {
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  if (!FlushFileBuffers(h)) {
    errno = EIO;
    return -1;
  }
  return 0;
}

namespace ps_compat_detail {

// Mirrors FILE_RENAME_INFO_EX and its flags (Windows 10 1607 and later) so the
// code builds against older SDK targets; the enum value is FileRenameInfoEx.
struct RenameInfoEx {
  DWORD flags;
  HANDLE root_directory;
  DWORD file_name_length;  // bytes, without the terminator
  WCHAR file_name[1];
};
constexpr DWORD kRenameReplaceIfExists = 0x1;
constexpr DWORD kRenamePosixSemantics = 0x2;
constexpr int kFileRenameInfoEx = 22;

inline bool ToWidePath(const char* in, std::wstring* out) {
  const int n = MultiByteToWideChar(CP_ACP, 0, in, -1, nullptr, 0);
  if (n <= 0) return false;
  std::wstring w(static_cast<size_t>(n), L'\0');
  if (MultiByteToWideChar(CP_ACP, 0, in, -1, &w[0], n) <= 0) return false;
  w.resize(static_cast<size_t>(n) - 1);
  const DWORD full = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (full == 0) return false;
  out->assign(full, L'\0');
  const DWORD got = GetFullPathNameW(w.c_str(), full, &(*out)[0], nullptr);
  if (got == 0 || got >= full) return false;
  out->resize(got);
  return true;
}

// Renames src over dst with rename(2) semantics: an existing dst that other
// processes hold open is superseded instead of failing with access denied.
// Returns false when the call is unavailable or fails, so the caller can fall
// back; the caller must not assume anything about the files in that case.
inline bool PosixSemanticsRename(const char* src, const char* dst) {
  std::wstring wdst;
  if (!ToWidePath(dst, &wdst)) return false;
  std::wstring wsrc;
  if (!ToWidePath(src, &wsrc)) return false;
  // Open the link itself, not its target, as MoveFileExA does.
  HANDLE h = CreateFileW(wsrc.c_str(), DELETE | SYNCHRONIZE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const size_t name_bytes = wdst.size() * sizeof(WCHAR);
  std::string buf(sizeof(RenameInfoEx) + name_bytes, '\0');
  auto* info = reinterpret_cast<RenameInfoEx*>(&buf[0]);
  info->flags = kRenameReplaceIfExists | kRenamePosixSemantics;
  info->root_directory = nullptr;
  info->file_name_length = static_cast<DWORD>(name_bytes);
  std::memcpy(info->file_name, wdst.data(), name_bytes);
  const BOOL ok = SetFileInformationByHandle(
      h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoEx), info,
      static_cast<DWORD>(buf.size()));
  CloseHandle(h);
  return ok != FALSE;
}

}  // namespace ps_compat_detail

// rename() on Windows fails if destination exists. This wrapper replaces the
// destination the way POSIX rename() does: first through a rename with POSIX
// semantics (Windows 10 1607 / Server 2016 and later), which succeeds even
// while another process holds the destination open, then, where that is not
// supported by the system or the file system, through
// MoveFileExA(MOVEFILE_REPLACE_EXISTING).
inline int ps_rename(const char* src, const char* dst) {
  if (ps_compat_detail::PosixSemanticsRename(src, dst)) return 0;
  if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING)) return 0;
  // Report the real cause through errno, as the POSIX rename() does.
  const DWORD err = GetLastError();
  if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
    errno = ENOSPC;
  } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
    errno = ENOENT;
  } else if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
    errno = EACCES;
  } else {
    errno = EIO;
  }
  return -1;
}

// Opens a new file for writing, refusing to follow symlinks.
// Combines O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW semantics.
// Windows: CreateFileA with CREATE_NEW + FILE_FLAG_OPEN_REPARSE_POINT.
// Returns file descriptor on success, -1 on failure (errno set).
inline int ps_open_exclusive_nofollow(const char* path, int mode) {
  (void)mode;  // Windows ignores POSIX permission bits here.
  HANDLE h = CreateFileA(path, GENERIC_WRITE,
                         0,        // exclusive — no sharing
                         nullptr,  // default security
                         CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
      errno = EEXIST;
    } else if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
      errno = ENOSPC;
    } else {
      errno = EACCES;
    }
    return -1;
  }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), O_WRONLY);
  if (fd < 0) {
    CloseHandle(h);
    return -1;
  }
  return fd;
}

inline ssize_t ps_write(int fd, const void* buf, size_t count) {
  return _write(fd, buf, static_cast<unsigned int>(count));
}

inline int ps_close(int fd) { return _close(fd); }

inline int ps_unlink(const char* path) { return _unlink(path); }

#else  // !_WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

inline int ps_rename(const char* src, const char* dst) {
  return std::rename(src, dst);
}

inline int ps_open_exclusive_nofollow(const char* path, int mode) {
  return ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
}

inline ssize_t ps_write(int fd, const void* buf, size_t count) {
  return ::write(fd, buf, count);
}

inline int ps_close(int fd) { return ::close(fd); }

inline int ps_unlink(const char* path) { return ::unlink(path); }

#endif  // _WIN32

#endif  // LIB_BASE_POSIX_COMPAT_H_
