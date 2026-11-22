// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_TEST_UTIL_CDP_PIPE_H_
#define TEST_TEST_UTIL_CDP_PIPE_H_

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

#include <set>
using ssize_t = intptr_t;
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "uv.h"

namespace pagespeed {
namespace test {

#ifdef _WIN32
// Track which FDs have been marked non-blocking via SetNonBlocking().
// On POSIX, fcntl(O_NONBLOCK) handles this natively. On Windows,
// overlapped pipes don't have a non-blocking flag, so we track it
// ourselves and use zero-timeout waits in PipeRead().
inline std::set<int>& NonBlockingFds() {
  static std::set<int> fds;
  return fds;
}
#endif

// Cross-platform pipe pair for simulating CDP communication.
// Uses libuv's uv_pipe() with UV_NONBLOCK_PIPE to create overlapped
// Named Pipes on Windows — required for libuv's IOCP-based async I/O.
// On Windows, PipeWrite/PipeRead use native WriteFile/ReadFile (via
// _get_osfhandle) because CRT _write/_read don't work reliably on
// overlapped pipe handles.
struct PipePair {
  int read_fd = -1;
  int write_fd = -1;

  // Create a pipe pair compatible with uv_pipe_open().
  // UV_NONBLOCK_PIPE creates overlapped pipes on Windows for IOCP.
  bool Create() {
    uv_file fds[2];
    int flags = 0;
#ifdef _WIN32
    flags = UV_NONBLOCK_PIPE;
#endif
    if (uv_pipe(fds, flags, flags) != 0) return false;
    read_fd = fds[0];
    write_fd = fds[1];
    return true;
  }

  // Transfer ownership of read_fd to caller (e.g., for uv_pipe_open).
  // Returns the fd and sets internal member to -1 so CloseRead() is a no-op.
  int TakeReadFd() {
    int fd = read_fd;
    read_fd = -1;
    return fd;
  }

  int TakeWriteFd() {
    int fd = write_fd;
    write_fd = -1;
    return fd;
  }

  void CloseRead() {
    if (read_fd >= 0) {
#ifdef _WIN32
      NonBlockingFds().erase(read_fd);
      _close(read_fd);  // Closes HANDLE + frees CRT fd slot.
#else
      close(read_fd);
#endif
      read_fd = -1;
    }
  }

  void CloseWrite() {
    if (write_fd >= 0) {
#ifdef _WIN32
      NonBlockingFds().erase(write_fd);
      _close(write_fd);
#else
      close(write_fd);
#endif
      write_fd = -1;
    }
  }

  void CloseAll() {
    CloseRead();
    CloseWrite();
  }
};

// Set a file descriptor to non-blocking mode.
inline bool SetNonBlocking(int fd) {
#ifdef _WIN32
  // Record the FD so PipeRead() can use a zero-timeout wait instead
  // of blocking forever when no data is available.
  NonBlockingFds().insert(fd);
  return true;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Write data to a pipe fd (cross-platform).
// On Windows, uses WriteFile with OVERLAPPED for overlapped pipes.
// If the pipe buffer is full and SetNonBlocking was called, returns 0
// (like EAGAIN) so callers can drain the pipe and retry.
inline ssize_t PipeWrite(int fd, const void* data, size_t len) {
#ifdef _WIN32
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) return -1;
  // Write in chunks up to the pipe buffer size to avoid blocking on
  // a single large overlapped write that can't complete until the
  // reader drains the pipe. The caller (WriteLargeFromChrome) runs
  // uv_run(UV_RUN_ONCE) between calls to drain the read side.
  constexpr DWORD kChunkSize = 4096;
  DWORD to_write = static_cast<DWORD>(len);
  if (to_write > kChunkSize) to_write = kChunkSize;
  DWORD written = 0;
  OVERLAPPED ov = {};
  ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!WriteFile(h, data, to_write, &written, &ov)) {
    if (GetLastError() == ERROR_IO_PENDING) {
      if (!GetOverlappedResult(h, &ov, &written, TRUE)) {
        CloseHandle(ov.hEvent);
        return -1;
      }
    } else {
      CloseHandle(ov.hEvent);
      return -1;
    }
  }
  CloseHandle(ov.hEvent);
  return static_cast<ssize_t>(written);
#else
  return write(fd, data, len);
#endif
}

// Read data from a pipe fd (cross-platform).
// On Windows, uses ReadFile with OVERLAPPED for overlapped pipes.
// When the FD has been marked non-blocking via SetNonBlocking(),
// returns -1 immediately (simulating EAGAIN) if no data is available.
inline ssize_t PipeRead(int fd, void* buf, size_t len) {
#ifdef _WIN32
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) return -1;
  bool non_blocking = NonBlockingFds().count(fd) > 0;
  DWORD bytes_read = 0;
  OVERLAPPED ov = {};
  ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) return -1;
  if (!ReadFile(h, buf, static_cast<DWORD>(len), &bytes_read, &ov)) {
    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      if (non_blocking) {
        // Non-blocking: poll with zero timeout instead of waiting.
        DWORD wait = WaitForSingleObject(ov.hEvent, 0);
        if (wait == WAIT_TIMEOUT) {
          CancelIoEx(h, &ov);
          // Drain the cancelled I/O so the kernel cleans up.
          GetOverlappedResult(h, &ov, &bytes_read, TRUE);
          CloseHandle(ov.hEvent);
          return -1;  // No data available (like EAGAIN).
        }
        // Data arrived between ReadFile and WaitForSingleObject.
        if (!GetOverlappedResult(h, &ov, &bytes_read, FALSE)) {
          DWORD err2 = GetLastError();
          CloseHandle(ov.hEvent);
          return (err2 == ERROR_BROKEN_PIPE) ? 0 : -1;
        }
      } else {
        // Blocking: wait indefinitely for data.
        if (!GetOverlappedResult(h, &ov, &bytes_read, TRUE)) {
          DWORD err2 = GetLastError();
          CloseHandle(ov.hEvent);
          return (err2 == ERROR_BROKEN_PIPE) ? 0 : -1;
        }
      }
    } else if (err == ERROR_BROKEN_PIPE) {
      CloseHandle(ov.hEvent);
      return 0;  // EOF
    } else {
      CloseHandle(ov.hEvent);
      return -1;
    }
  }
  CloseHandle(ov.hEvent);
  return static_cast<ssize_t>(bytes_read);
#else
  return read(fd, buf, len);
#endif
}

// Cross-platform microsecond sleep.
inline void SleepUs(int us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

// Run `step` (answer the fake browser's commands, run the loop) until `done`
// holds. Wait on the step a test expects, never on a fixed window of wall
// time: on a busy host the setup round trips can take longer than any window.
// `bound` only stops a real hang; a slow or stalled host just takes longer.
inline void PumpUntil(
    const std::function<void()>& step, const std::function<bool()>& done,
    std::chrono::milliseconds bound = std::chrono::seconds(10)) {
  const auto end = std::chrono::steady_clock::now() + bound;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= end) return;
    step();
    SleepUs(2000);
  }
}

// Let a libuv timer of `delay` run out that an event already sent to the
// client arms: run the loop to read that event (which starts the timer),
// sleep past `delay` in wall time, then run the loop so the now-due timer
// fires. A host stall anywhere in here only lengthens the wait; the timer
// never misses it.
inline void RunPastTimer(const std::function<void()>& run_loop,
                         std::chrono::milliseconds delay) {
  run_loop();
  run_loop();
  std::this_thread::sleep_for(delay + std::chrono::milliseconds(20));
  run_loop();
  run_loop();
}

}  // namespace test
}  // namespace pagespeed

#endif  // TEST_TEST_UTIL_CDP_PIPE_H_
