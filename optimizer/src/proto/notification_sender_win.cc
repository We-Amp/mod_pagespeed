// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Notification Sender Implementation (Windows Named Pipes)

#include <windows.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "src/proto/notification_sender.h"

// ECONNREFUSED is POSIX (not in MSVC's <cerrno>). Define it for the
// connect_errno field so retry logic works cross-platform.
#ifndef ECONNREFUSED
#define ECONNREFUSED 10061  // WSAECONNREFUSED value
#endif

namespace pagespeed {

static_assert(kNotificationPipeDesiredAccess ==
                  (FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE),
              "read, write and synchronize only");

// Maximum time (ms) to wait for the named pipe to become available
// (WaitNamedPipe). This bounds the worst-case stall of the calling thread.
// 500ms is generous enough to avoid spurious failures on loaded systems
// while still bounding the stall for the IIS/nginx worker thread.
static constexpr DWORD kSendTimeoutMs = 500;

// Map Windows error codes to POSIX errno equivalents for connect_errno.
static int MapWindowsError(DWORD err) {
  switch (err) {
    case ERROR_FILE_NOT_FOUND:
      return ENOENT;
    case ERROR_PIPE_BUSY:
      return ECONNREFUSED;
    case ERROR_BROKEN_PIPE:
      return EPIPE;
    case ERROR_NO_DATA:
      return EPIPE;
    default:
      return EIO;
  }
}

// Convert a socket path to a Windows named pipe name, matching libuv's
// uv_pipe_bind convention: prepend \\.\pipe\ to the raw path bytes.
// No slash stripping or translation — libuv preserves the path as-is.
static std::string ToPipeName(std::string_view socket_path) {
  constexpr std::string_view kPrefix = "\\\\.\\pipe\\";
  if (socket_path.size() >= kPrefix.size() &&
      socket_path.substr(0, kPrefix.size()) == kPrefix) {
    return std::string(socket_path);
  }
  std::string name;
  name.reserve(kPrefix.size() + socket_path.size());
  name += kPrefix;
  name += socket_path;
  return name;
}

// Open a named pipe handle for writing. Returns INVALID_HANDLE_VALUE on
// failure and populates result with error details.
static HANDLE ConnectPipe(const std::string& pipe_name,
                          NotificationSendResult& result) {
  // Wait for the pipe to become available (bounded by timeout).
  if (!WaitNamedPipeA(pipe_name.c_str(), kSendTimeoutMs)) {
    DWORD err = GetLastError();
    // ERROR_FILE_NOT_FOUND means the pipe doesn't exist yet (server not
    // running). Skip WaitNamedPipe failure in that case and let CreateFile
    // produce the definitive error below. For any other WaitNamedPipe
    // failure, also fall through to CreateFile so we get a single error path.
    if (err != ERROR_FILE_NOT_FOUND) {
      // Pipe exists but is busy and didn't become available in time.
      result.connect_errno = MapWindowsError(err);
      result.error_message = "WaitNamedPipe timed out";
      return INVALID_HANDLE_VALUE;
    }
  }

  // Least privilege: connect at identification level. The worker never acts
  // as its clients.
  HANDLE h =
      CreateFileA(pipe_name.c_str(),
                  // libuv pipes are duplex. Not GENERIC_WRITE: it includes
                  // FILE_APPEND_DATA, which on a named pipe is
                  // FILE_CREATE_PIPE_INSTANCE, withheld by the worker's DACL.
                  kNotificationPipeDesiredAccess,
                  0,              // no sharing
                  nullptr,        // default security
                  OPEN_EXISTING,  // must already exist
                  SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                  nullptr);  // no template

  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    result.connect_errno = MapWindowsError(err);
    result.error_message = "Failed to connect to worker pipe";
    return INVALID_HANDLE_VALUE;
  }

  // Switch to message mode is not needed; we write raw bytes.
  // Set a write timeout via pipe mode isn't available on client side,
  // but our messages are small enough that WriteFile won't block
  // meaningfully on a local pipe.

  return h;
}

// Write all bytes to the pipe handle. Returns true on success.
static bool WriteAll(HANDLE h, const char* ptr, size_t remaining) {
  while (remaining > 0) {
    DWORD written = 0;
    if (!WriteFile(h, ptr, static_cast<DWORD>(remaining), &written, nullptr)) {
      return false;  // ERROR_BROKEN_PIPE, ERROR_NO_DATA, etc.
    }
    if (written == 0) return false;
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

NotificationSendResult SendNotification(std::string_view socket_path,
                                        const CacheNotification& notification) {
  NotificationSendResult result;

  if (socket_path.empty()) {
    result.error_message = "Empty socket path";
    return result;
  }

  // Serialize before connecting: an empty serialization means Serialize
  // refused the notification (bad scheme, oversized fields, half an option
  // context), and no connection could deliver it. Connecting first would open
  // a connection only to write nothing and report success.
  std::vector<char> data = notification.Serialize();
  if (data.empty()) {
    result.invalid_argument = true;
    result.error_message =
        "Notification could not be serialized (invalid scheme or field "
        "lengths)";
    return result;
  }

  std::string pipe_name = ToPipeName(socket_path);

  HANDLE h = ConnectPipe(pipe_name, result);
  if (h == INVALID_HANDLE_VALUE) {
    return result;
  }

  if (!WriteAll(h, data.data(), data.size())) {
    CloseHandle(h);
    result.error_message = "Failed to write notification";
    return result;
  }

  CloseHandle(h);
  result.success = true;
  return result;
}

// =========================================================================
// Persistent connection state (process-local)
// =========================================================================

// Cached handle for the persistent connection. INVALID_HANDLE_VALUE means
// not connected. Safe without mutex: IIS/nginx worker processes are
// single-threaded per request pipeline.
static HANDLE g_persistent_handle = INVALID_HANDLE_VALUE;

// The pipe path this handle is connected to. Used to detect path changes
// (e.g., worker restart with a different socket path).
static std::string g_persistent_path;

// Ensure g_persistent_handle is connected to socket_path.
// Returns the handle, or INVALID_HANDLE_VALUE on failure.
static HANDLE EnsureConnected(std::string_view socket_path,
                              const std::string& pipe_name,
                              NotificationSendResult& result) {
  // If connected to the right path, reuse.
  if (g_persistent_handle != INVALID_HANDLE_VALUE &&
      g_persistent_path.size() == socket_path.size() &&
      g_persistent_path == socket_path) {
    return g_persistent_handle;
  }

  // Close stale connection (wrong path or leftover).
  if (g_persistent_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(g_persistent_handle);
    g_persistent_handle = INVALID_HANDLE_VALUE;
    g_persistent_path.clear();
  }

  HANDLE h = ConnectPipe(pipe_name, result);
  if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

  g_persistent_handle = h;
  g_persistent_path.assign(socket_path.data(), socket_path.size());
  return h;
}

NotificationSendResult SendNotificationPersistent(
    std::string_view socket_path, const CacheNotification& notification) {
  NotificationSendResult result;

  if (socket_path.empty()) {
    result.error_message = "Empty socket path";
    return result;
  }

  std::string pipe_name = ToPipeName(socket_path);
  std::vector<char> data = notification.Serialize();

  // Attempt 1: write on existing (or newly created) connection.
  HANDLE h = EnsureConnected(socket_path, pipe_name, result);
  if (h != INVALID_HANDLE_VALUE && WriteAll(h, data.data(), data.size())) {
    result.success = true;
    return result;
  }

  // Write failed (broken pipe, etc.) or connect failed.
  // Close the stale handle and reconnect once.
  if (g_persistent_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(g_persistent_handle);
    g_persistent_handle = INVALID_HANDLE_VALUE;
    g_persistent_path.clear();
  }

  result = {};  // Reset error state for the retry.
  h = EnsureConnected(socket_path, pipe_name, result);
  if (h == INVALID_HANDLE_VALUE) return result;

  if (WriteAll(h, data.data(), data.size())) {
    result.success = true;
    return result;
  }

  // Second write also failed. Close handle to avoid sending partial data
  // on the next call.
  CloseHandle(g_persistent_handle);
  g_persistent_handle = INVALID_HANDLE_VALUE;
  g_persistent_path.clear();
  result.error_message = "Persistent send failed after reconnect";
  return result;
}

void ResetPersistentConnection() {
  if (g_persistent_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(g_persistent_handle);
    g_persistent_handle = INVALID_HANDLE_VALUE;
  }
  g_persistent_path.clear();
}

void ClosePersistentConnection() { ResetPersistentConnection(); }

NotificationSendResult SendNotificationWithRetry(
    std::string_view socket_path, const CacheNotification& notification,
    const RetryConfig& config) {
  NotificationSendResult result = SendNotification(socket_path, notification);
  if (result.success) {
    return result;
  }

  // Only retry on connection failures (worker not ready / pipe missing).
  // Don't retry on timeouts (worker overloaded) or send failures.
  int delay_ms = config.initial_delay_ms;
  for (int attempt = 0; attempt < config.max_retries; ++attempt) {
    if (result.connect_errno != ECONNREFUSED &&
        result.connect_errno != ENOENT) {
      break;
    }
    Sleep(static_cast<DWORD>(delay_ms));
    result = SendNotification(socket_path, notification);
    result.retries_attempted = attempt + 1;
    if (result.success) {
      return result;
    }
    delay_ms = static_cast<int>(delay_ms * config.backoff_multiplier);
  }

  return result;
}

}  // namespace pagespeed
