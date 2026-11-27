// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_TEST_UTIL_PIPE_CLIENT_H_
#define TEST_TEST_UTIL_PIPE_CLIENT_H_

// Cross-platform IPC pipe client for tests.
// On POSIX: connects via Unix domain sockets (AF_UNIX).
// On Windows: connects via Windows Named Pipes, matching libuv's naming.
//
// All functions use intptr_t for handles (safe for both POSIX int fd and
// Windows HANDLE without truncation on 64-bit systems).
//
// Usage pattern:
//   intptr_t fd = pagespeed::test::ConnectPipe(socket_path);
//   pagespeed::test::PipeWrite(fd, data.data(), data.size());
//   pagespeed::test::ClosePipe(fd);

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
using ssize_t = intptr_t;
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace pagespeed::test {

#ifdef _WIN32

inline std::string ToPipeName(const std::string& path) {
  const char* kPrefix = "\\\\.\\pipe\\";
  if (path.size() >= 9 && path.substr(0, 9) == kPrefix) {
    return path;
  }
  return std::string(kPrefix) + path;
}

// Connect to a named pipe. Returns opaque handle, or -1 on failure.
inline intptr_t ConnectPipe(const std::string& path, int timeout_ms = 5000) {
  std::string pipe_name = ToPipeName(path);

  if (!WaitNamedPipeA(pipe_name.c_str(), static_cast<DWORD>(timeout_ms))) {
    if (GetLastError() != ERROR_FILE_NOT_FOUND) return -1;
  }

  HANDLE h = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1;

  return reinterpret_cast<intptr_t>(h);
}

inline ssize_t PipeWrite(intptr_t fd, const void* data, size_t len) {
  HANDLE h = reinterpret_cast<HANDLE>(fd);
  DWORD written = 0;
  if (!WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr)) {
    return -1;
  }
  return static_cast<ssize_t>(written);
}

inline ssize_t PipeRead(intptr_t fd, void* buf, size_t len) {
  HANDLE h = reinterpret_cast<HANDLE>(fd);
  DWORD bytes_read = 0;
  if (!ReadFile(h, buf, static_cast<DWORD>(len), &bytes_read, nullptr)) {
    return (GetLastError() == ERROR_BROKEN_PIPE) ? 0 : -1;
  }
  return static_cast<ssize_t>(bytes_read);
}

inline void ClosePipe(intptr_t fd) {
  HANDLE h = reinterpret_cast<HANDLE>(fd);
  // Flush ensures the server's overlapped read completes before the pipe
  // handle is destroyed.  Without this, CloseHandle can discard unread
  // data when many clients connect in rapid succession.
  FlushFileBuffers(h);
  CloseHandle(h);
}

// Check if a named pipe endpoint exists (server is listening).
inline bool PipeEndpointExists(const std::string& path) {
  std::string pipe_name = ToPipeName(path);
  if (WaitNamedPipeA(pipe_name.c_str(), 0)) return true;
  return GetLastError() == ERROR_SEM_TIMEOUT;
}

#else  // POSIX

inline intptr_t ConnectPipe(const std::string& path,
                            int /*timeout_ms*/ = 5000) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    close(fd);
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());

  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    close(fd);
    return -1;
  }
  return static_cast<intptr_t>(fd);
}

inline ssize_t PipeWrite(intptr_t fd, const void* data, size_t len) {
  return write(static_cast<int>(fd), data, len);
}

inline ssize_t PipeRead(intptr_t fd, void* buf, size_t len) {
  return read(static_cast<int>(fd), buf, len);
}

inline void ClosePipe(intptr_t fd) { close(static_cast<int>(fd)); }

inline bool PipeEndpointExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

#endif  // _WIN32

}  // namespace pagespeed::test

#endif  // TEST_TEST_UTIL_PIPE_CLIENT_H_
