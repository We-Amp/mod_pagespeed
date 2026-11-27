// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Notification Sender Implementation

#include "src/proto/notification_sender.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

namespace pagespeed {

// Maximum time (ms) to wait for connect + write to complete. This bounds
// each individual wait, not the whole send: the timer restarts whenever a
// write makes progress.
static constexpr int kSendTimeoutMs = 50;

// Create a non-blocking Unix stream socket and connect it to socket_path,
// waiting up to kSendTimeoutMs for a non-blocking connect to complete.
// Returns the connected fd on success, or -1 on failure (setting
// result.error_message and result.connect_errno).
static int CreateAndConnectSocket(std::string_view socket_path,
                                  NotificationSendResult& result);

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

  int fd = CreateAndConnectSocket(socket_path, result);
  if (fd < 0) {
    return result;
  }

  // Send (non-blocking with timeout)
  const char* ptr = data.data();
  size_t remaining = data.size();

  while (remaining > 0) {
    ssize_t written = send(fd, ptr, remaining, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        int poll_rc = poll(&pfd, 1, kSendTimeoutMs);
        if (poll_rc <= 0) {
          close(fd);
          result.error_message = "Write to worker timed out";
          return result;
        }
        continue;
      }
      close(fd);
      result.error_message = "Failed to write notification";
      return result;
    }
    if (written == 0) {
      close(fd);
      result.error_message =
          "Failed to write notification (zero bytes written)";
      return result;
    }
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }

  close(fd);
  result.success = true;
  return result;
}

// =========================================================================
// Persistent connection state (process-local)
// =========================================================================

// Cached fd for the persistent connection. -1 means not connected.
// Safe without mutex: nginx worker processes are single-threaded.
static int g_persistent_fd = -1;

// The socket path this fd is connected to. Used to detect path changes
// (e.g., worker restart with a different socket path).
static std::string g_persistent_path;

// Forward declaration — used by EnsureConnected and SendNotificationPersistent.
void ResetPersistentConnection();

static int CreateAndConnectSocket(std::string_view socket_path,
                                  NotificationSendResult& result) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    result.error_message = "Failed to create socket";
    return -1;
  }

  // Set non-blocking
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    result.error_message = "Failed to set non-blocking";
    return -1;
  }

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;

  size_t path_len = socket_path.size();
  if (path_len >= sizeof(addr.sun_path)) {
    close(fd);
    result.error_message = "Socket path too long";
    return -1;
  }
  std::memcpy(addr.sun_path, socket_path.data(), path_len);

  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    if (errno != EINPROGRESS && errno != EAGAIN) {
      result.connect_errno = errno;
      close(fd);
      result.error_message = "Failed to connect to worker socket";
      return -1;
    }
    // Wait for connect with timeout
    struct pollfd pfd = {fd, POLLOUT, 0};
    int poll_rc = poll(&pfd, 1, kSendTimeoutMs);
    if (poll_rc <= 0) {
      result.connect_errno = (poll_rc == 0) ? ETIMEDOUT : errno;
      close(fd);
      result.error_message = "Connect to worker timed out";
      return -1;
    }
    int sock_err = 0;
    socklen_t err_len = sizeof(sock_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len) < 0) {
      result.connect_errno = errno;
      close(fd);
      result.error_message = "Failed to check connect result";
      return -1;
    }
    if (sock_err != 0) {
      result.connect_errno = sock_err;
      close(fd);
      result.error_message = "Failed to connect to worker socket";
      return -1;
    }
  }

  return fd;
}

// Write all bytes to fd. Returns true on success.
// On EAGAIN, retries with poll (bounded by kSendTimeoutMs).
// On EPIPE/ECONNRESET/other error, returns false.
static bool WriteAll(int fd, const char* ptr, size_t remaining) {
  while (remaining > 0) {
    ssize_t written = send(fd, ptr, remaining, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        int poll_rc = poll(&pfd, 1, kSendTimeoutMs);
        if (poll_rc <= 0) {
          fprintf(stderr,
                  "[pagespeed] WriteAll: poll %s (fd=%d, %zu bytes left)\n",
                  poll_rc == 0 ? "timeout" : "error", fd, remaining);
          return false;
        }
        continue;
      }
      fprintf(stderr,
              "[pagespeed] WriteAll: send errno=%d (fd=%d, %zu bytes left)\n",
              errno, fd, remaining);
      return false;
    }
    if (written == 0) {
      fprintf(stderr,
              "[pagespeed] WriteAll: zero-byte send (fd=%d, %zu bytes left)\n",
              fd, remaining);
      return false;
    }
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

// Ensure g_persistent_fd is connected to socket_path.
// Returns the fd, or -1 on failure.
static int EnsureConnected(std::string_view socket_path,
                           NotificationSendResult& result) {
  // If connected to the right path, reuse.
  if (g_persistent_fd >= 0 && g_persistent_path.size() == socket_path.size() &&
      g_persistent_path == socket_path) {
    return g_persistent_fd;
  }

  // Close stale connection (wrong path or leftover).
  ResetPersistentConnection();

  int fd = CreateAndConnectSocket(socket_path, result);
  if (fd < 0) return -1;

  g_persistent_fd = fd;
  g_persistent_path.assign(socket_path.data(), socket_path.size());
  return fd;
}

NotificationSendResult SendNotificationPersistent(
    std::string_view socket_path, const CacheNotification& notification) {
  NotificationSendResult result;

  if (socket_path.empty()) {
    result.error_message = "Empty socket path";
    return result;
  }

  std::vector<char> data = notification.Serialize();

  // Attempt 1: write on existing (or newly created) connection.
  int fd = EnsureConnected(socket_path, result);
  if (fd >= 0 && WriteAll(fd, data.data(), data.size())) {
    result.success = true;
    return result;
  }

  // Write failed (EPIPE, ECONNRESET, timeout, etc.) or connect failed.
  // Close the stale fd and reconnect once.
  ResetPersistentConnection();

  result = {};  // Reset error state for the retry.
  fd = EnsureConnected(socket_path, result);
  if (fd < 0) return result;

  if (WriteAll(fd, data.data(), data.size())) {
    result.success = true;
    return result;
  }

  // Second write also failed. Close fd to avoid sending partial data
  // on the next call.
  ResetPersistentConnection();
  result.error_message = "Persistent send failed after reconnect";
  return result;
}

void ResetPersistentConnection() {
  if (g_persistent_fd >= 0) {
    close(g_persistent_fd);
    g_persistent_fd = -1;
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

  // Only retry on connection failures (worker not ready / socket missing).
  // Don't retry on timeouts (worker overloaded) or send failures.
  int delay_ms = config.initial_delay_ms;
  for (int attempt = 0; attempt < config.max_retries; ++attempt) {
    if (result.connect_errno != ECONNREFUSED &&
        result.connect_errno != ENOENT) {
      break;
    }
    usleep(static_cast<useconds_t>(delay_ms) * 1000);
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
