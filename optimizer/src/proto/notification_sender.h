// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Notification Sender
//
// Fire-and-forget notification sender for the worker IPC protocol.
// Two modes:
//   1. Per-notification: socket → connect → write → close (legacy).
//   2. Persistent: one cached connection per process, reused across
//      notifications. ~1 syscall (write) per notification instead of 6-8.
//      Automatically reconnects on error or socket path change.

#ifndef PAGESPEED_SRC_PROTO_NOTIFICATION_SENDER_H_
#define PAGESPEED_SRC_PROTO_NOTIFICATION_SENDER_H_

#include <string>
#include <string_view>

#include "src/proto/worker_ipc.h"

namespace pagespeed {

#ifdef _WIN32
// The access the sender requests when it opens the worker's pipe:
// FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE. Not GENERIC_WRITE, which
// includes FILE_APPEND_DATA, the same bit as FILE_CREATE_PIPE_INSTANCE that
// the worker's access rules withhold from other accounts.
inline constexpr unsigned long kNotificationPipeDesiredAccess = 0x00100003;
#endif

// Result of a send operation.
struct NotificationSendResult {
  bool success = false;
  std::string error_message;
  int retries_attempted = 0;
  int connect_errno = 0;  // errno from connect() if it failed
  // True when the notification itself was refused: Serialize produced no
  // bytes, so no connection and no retry could deliver it. Everything else
  // is a delivery (IO) failure, so callers map this field to their
  // invalid-argument error.
  bool invalid_argument = false;
};

// Send a CacheNotification to the worker via Unix socket.
// This is fire-and-forget: no response is expected.
// The function connects, sends, and closes in one call.
// A notification that cannot be serialized fails without connecting
// (invalid_argument set on the result).
[[nodiscard]] NotificationSendResult SendNotification(
    std::string_view socket_path, const CacheNotification& notification);

// Send via a persistent connection cached in process-local state.
// On first call, connects and caches the fd. Subsequent calls reuse it.
// Reconnects automatically on write error (EPIPE, ECONNRESET) or when
// socket_path changes (e.g., worker restart with new path).
//
// This is safe in nginx's multi-process model: each worker process has
// its own address space and thus its own cached fd. No mutex is needed
// because nginx worker processes are single-threaded.
//
// Call ResetPersistentConnection() in process init hooks (e.g., after
// fork) and ClosePersistentConnection() on process exit.
[[nodiscard]] NotificationSendResult SendNotificationPersistent(
    std::string_view socket_path, const CacheNotification& notification);

// Reset the persistent connection state. Call after fork() to avoid
// sharing the parent's fd across processes. Safe to call when no
// connection is open (no-op).
void ResetPersistentConnection();

// Close the persistent connection and release resources. Call on
// process exit for clean shutdown.
void ClosePersistentConnection();

// Retry configuration for SendNotificationWithRetry.
struct RetryConfig {
  int max_retries = 0;              // Default: no retries (non-blocking).
  int initial_delay_ms = 10;        // Only used when max_retries > 0.
  double backoff_multiplier = 3.0;  // 10ms, 30ms
};

// Send with retry on connection failure (ECONNREFUSED/ENOENT).
// Default: no retries (fire-and-forget). Natural retry occurs when
// the next request re-notifies on fallback-hit.
// WARNING: usleep() blocks the calling thread between retries.
[[nodiscard]] NotificationSendResult SendNotificationWithRetry(
    std::string_view socket_path, const CacheNotification& notification,
    const RetryConfig& config = {});

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_PROTO_NOTIFICATION_SENDER_H_
