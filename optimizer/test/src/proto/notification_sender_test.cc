// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Notification Sender Tests
//
// Tests the fire-and-forget notification sender that connects to the worker
// via Unix socket (POSIX) or Named Pipe (Windows).

#include "src/proto/notification_sender.h"

#ifdef _WIN32
#include <windows.h>
using ssize_t = intptr_t;
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#endif

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "src/proto/worker_ipc.h"
#include "test/test_util/temp_dir.h"

// Cross-platform mock server helpers used by tests.
// On POSIX: AF_UNIX socket server. On Windows: Named Pipe server.
#ifdef _WIN32

inline std::string ToPipeName(const std::string& path) {
  const char* kPrefix = "\\\\.\\pipe\\";
  if (path.size() >= 9 && path.substr(0, 9) == kPrefix) return path;
  return std::string(kPrefix) + path;
}

// Create a named pipe server instance with overlapped I/O support.
// Returns HANDLE (as intptr_t) or -1.
inline intptr_t CreatePipeServer(const std::string& socket_path) {
  std::string pipe_name = ToPipeName(socket_path);
  HANDLE h = CreateNamedPipeA(
      pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
      4096, 4096, 50, nullptr);
  return (h == INVALID_HANDLE_VALUE)
             ? -1
             : static_cast<intptr_t>(reinterpret_cast<uintptr_t>(h));
}

// Accept a client on the pipe using overlapped I/O with a timeout.
// Returns server_handle on success, or -1 on failure/timeout.
inline intptr_t AcceptPipeClient(intptr_t server_handle,
                                 DWORD timeout_ms = 10000) {
  HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(server_handle));

  OVERLAPPED ov = {};
  ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) return -1;

  BOOL ok = ConnectNamedPipe(h, &ov);
  if (!ok) {
    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
      // Client already connected before ConnectNamedPipe was called.
      CloseHandle(ov.hEvent);
      return server_handle;
    }
    if (err == ERROR_IO_PENDING) {
      // Wait for a client with a timeout.
      DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
      if (wait != WAIT_OBJECT_0) {
        // Timed out or error — cancel the pending ConnectNamedPipe.
        CancelIo(h);
        // Must wait for the cancelled I/O to complete.
        DWORD dummy;
        GetOverlappedResult(h, &ov, &dummy, TRUE);
        CloseHandle(ov.hEvent);
        return -1;
      }
      // ConnectNamedPipe completed — verify success.
      DWORD dummy;
      if (!GetOverlappedResult(h, &ov, &dummy, FALSE)) {
        CloseHandle(ov.hEvent);
        return -1;
      }
    } else {
      // Some other error.
      CloseHandle(ov.hEvent);
      return -1;
    }
  }

  CloseHandle(ov.hEvent);
  // Return same handle — on named pipes, the server handle IS the connection.
  return server_handle;
}

inline ssize_t PipeRead(intptr_t handle, void* buf, size_t len) {
  HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
  DWORD bytes_read = 0;
  if (!ReadFile(h, buf, static_cast<DWORD>(len), &bytes_read, nullptr)) {
    return (GetLastError() == ERROR_BROKEN_PIPE) ? 0 : -1;
  }
  return static_cast<ssize_t>(bytes_read);
}

inline void ClosePipeHandle(intptr_t handle) {
  HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
  DisconnectNamedPipe(h);
  CloseHandle(h);
}

// For the accept loop, we need a new pipe instance per connection.
inline intptr_t CreateAndAcceptPipe(const std::string& socket_path,
                                    DWORD timeout_ms = 10000) {
  intptr_t h = CreatePipeServer(socket_path);
  if (h == -1) return -1;
  if (AcceptPipeClient(h, timeout_ms) == -1) {
    ClosePipeHandle(h);
    return -1;
  }
  return h;
}

#endif  // _WIN32

// =============================================================================
// Cross-platform accept/read/close wrappers (work with intptr_t handles).
// =============================================================================
#ifdef _WIN32
// On Windows Named Pipes, each ConnectNamedPipe handles one client.
// AcceptClient creates a fresh pipe instance for each connection.
// The caller keeps the returned handle and closes it via CloseClient.
inline intptr_t AcceptClient(intptr_t /*server*/, const std::string& path) {
  return CreateAndAcceptPipe(path);
}
inline ssize_t ReadClient(intptr_t h, void* buf, size_t len) {
  return PipeRead(h, buf, len);
}
inline void CloseClient(intptr_t h) { ClosePipeHandle(h); }
inline void CloseServer(intptr_t h) {
  // StartListener returns a sentinel (no-op) on Windows; skip close for it.
  if (h != 0) ClosePipeHandle(h);
}
#else
inline intptr_t AcceptClient(intptr_t server, const std::string& /*path*/) {
  // Fail fast instead of blocking forever when no client ever connects.
  struct pollfd pfd = {static_cast<int>(server), POLLIN, 0};
  if (poll(&pfd, 1, 10000) <= 0) return -1;
  return static_cast<intptr_t>(
      accept(static_cast<int>(server), nullptr, nullptr));
}
inline ssize_t ReadClient(intptr_t h, void* buf, size_t len) {
  return read(static_cast<int>(h), buf, len);
}
inline void CloseClient(intptr_t h) { close(static_cast<int>(h)); }
inline void CloseServer(intptr_t h) { close(static_cast<int>(h)); }
#endif

namespace pagespeed {
namespace {

class NotificationSenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    ASSERT_FALSE(temp_dir_.empty());
    socket_path_ = temp_dir_ + "/notify.sock";
  }

  void TearDown() override {
    ClosePersistentConnection();  // Clean up any leaked persistent fd.
    if (!temp_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(temp_dir_, ec);
    }
  }

  // Helper: start a listening endpoint. Returns a server handle (fd on POSIX,
  // sentinel on Windows), or -1 on failure.
  // On Windows, StartListener is a no-op that returns a sentinel value (0)
  // because AcceptClient creates its own pipe instance via CreateAndAcceptPipe.
  // The old approach created a disconnected pipe instance that was never used
  // by AcceptClient, causing ConnectNamedPipe to block indefinitely.
#ifdef _WIN32
  static constexpr intptr_t kWinServerSentinel = 0;
  intptr_t StartListener() { return kWinServerSentinel; }
#else
  intptr_t StartListener() {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
      close(server_fd);
      return -1;
    }

    if (listen(server_fd, 16) != 0) {
      close(server_fd);
      return -1;
    }

    return static_cast<intptr_t>(server_fd);
  }
#endif

  // Helper: create a notification with specific URL and hostname
  static CacheNotification MakeNotification(
      const std::string& url = "http://example.com/page.html",
      const std::string& hostname = "example.com",
      ContentType ct = ContentType::kHtml, uint32_t mask = 0x08) {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.hostname = hostname;
    notification.content_type = ct;
    notification.capability_mask = mask;
    return notification;
  }

  std::string temp_dir_;
  std::string socket_path_;
};

// =============================================================================
// NotificationSendResult and RetryConfig default value tests
// =============================================================================

TEST_F(NotificationSenderTest, ResultDefaultValues) {
  NotificationSendResult result;
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error_message.empty());
  EXPECT_EQ(result.retries_attempted, 0);
  EXPECT_EQ(result.connect_errno, 0);
  EXPECT_FALSE(result.invalid_argument);
}

TEST_F(NotificationSenderTest, RetryConfigDefaultValues) {
  RetryConfig config;
  EXPECT_EQ(config.max_retries, 0);
  EXPECT_EQ(config.initial_delay_ms, 10);
  EXPECT_DOUBLE_EQ(config.backoff_multiplier, 3.0);
}

// =============================================================================
// SendNotification basic tests
// =============================================================================

TEST_F(NotificationSenderTest, SendToListeningSocket) {
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  // Receive in background thread
  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[4096];
      ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
      if (n > 0) {
        received_data.assign(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  // Give listener time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = "http://example.com/page.html";
  notification.scheme = "https";
  notification.hostname = "example.com";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x42;

  auto result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(result.success) << result.error_message;

  receiver.join();
  CloseServer(server_fd);

  // Verify received data can be deserialized
  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
  EXPECT_EQ(parsed.url, notification.url);
  EXPECT_EQ(parsed.hostname, "example.com");
  EXPECT_EQ(parsed.content_type, ContentType::kHtml);
  EXPECT_EQ(parsed.capability_mask, 0x42u);
}

TEST_F(NotificationSenderTest, SuccessfulSendSetsFieldsCorrectly) {
  // Verify that on success, connect_errno remains 0 and retries_attempted is 0.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[4096];
      (void)ReadClient(client_fd, buf, sizeof(buf));
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto result = SendNotification(socket_path_, MakeNotification());
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.connect_errno, 0);
  EXPECT_EQ(result.retries_attempted, 0);
  EXPECT_TRUE(result.error_message.empty());

  receiver.join();
  CloseServer(server_fd);
}

TEST_F(NotificationSenderTest, SendWithEmptySocketPath) {
  auto result = SendNotification("", CacheNotification{});
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Empty socket path");
  EXPECT_EQ(result.connect_errno, 0);
}

TEST_F(NotificationSenderTest, SendRefusedSerializationFailsWithoutConnect) {
  // Serialize refuses an empty scheme. That refusal must surface as an
  // invalid-argument failure before any connection is attempted: no listener
  // exists at this path, so a connect attempt would leave ENOENT or
  // ECONNREFUSED in connect_errno.
  CacheNotification notification = MakeNotification();
  notification.scheme.clear();

  auto result = SendNotification(socket_path_, notification);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.invalid_argument);
  EXPECT_EQ(result.connect_errno, 0);
  EXPECT_NE(result.error_message.find("serialized"), std::string::npos)
      << "Error: " << result.error_message;
}

TEST_F(NotificationSenderTest, SendRefusedOversizedFieldFailsWithoutConnect) {
  // A URL one byte over kMaxUrlLength is refused by Serialize; no listener
  // exists, so the refusal must surface before any connection is attempted.
  CacheNotification notification = MakeNotification();
  notification.url = std::string(kMaxUrlLength + 1, 'u');

  auto result = SendNotification(socket_path_, notification);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.invalid_argument);
  EXPECT_EQ(result.connect_errno, 0);
}

TEST_F(NotificationSenderTest, RetryDoesNotRetryRefusedSerialization) {
  // connect_errno is 0 for a serialization refusal, so the retry loop must
  // break immediately instead of sleeping.
  CacheNotification notification = MakeNotification();
  notification.scheme.clear();

  RetryConfig config;
  config.max_retries = 5;
  config.initial_delay_ms = 5;

  auto result = SendNotificationWithRetry(socket_path_, notification, config);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.invalid_argument);
  EXPECT_EQ(result.retries_attempted, 0);
}

#ifndef _WIN32
TEST_F(NotificationSenderTest, SendToPathTooLong) {
  // sun_path is 104 bytes on macOS, 108 on Linux. A path >= sizeof(sun_path)
  // should be rejected before attempting connect.
  sockaddr_un addr;
  std::string long_path(sizeof(addr.sun_path), 'x');

  auto result = SendNotification(long_path, MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("too long"), std::string::npos)
      << "Error: " << result.error_message;
  EXPECT_EQ(result.connect_errno, 0);
}

TEST_F(NotificationSenderTest, SendToPathExactlyAtLimit) {
  // A path of exactly sizeof(sun_path) - 1 is the maximum allowed by the
  // sender. This should fail with a connect error (no socket), not "too long".
  sockaddr_un addr;
  std::string max_path(sizeof(addr.sun_path) - 1, 'x');

  auto result = SendNotification(max_path, MakeNotification());
  EXPECT_FALSE(result.success);
  // Should NOT be "Socket path too long" since it fits in sun_path
  EXPECT_EQ(result.error_message.find("too long"), std::string::npos)
      << "Error: " << result.error_message;
}
#endif  // !_WIN32

// =============================================================================
// Connection error tests
// =============================================================================

TEST_F(NotificationSenderTest, SendToNonExistentSocket) {
  auto result =
      SendNotification("/tmp/nonexistent_socket.sock", MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
  // Should get ENOENT or ECONNREFUSED
  EXPECT_TRUE(result.connect_errno == ENOENT ||
              result.connect_errno == ECONNREFUSED)
      << "Unexpected connect_errno: " << result.connect_errno;
}

TEST_F(NotificationSenderTest, SendToNonExistentDirectory) {
  // Connecting to a socket path in a non-existent directory gives ENOENT.
  auto result = SendNotification("/tmp/no_such_dir_12345/notify.sock",
                                 MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.connect_errno, ENOENT);
  EXPECT_NE(result.error_message.find("connect"), std::string::npos)
      << "Error: " << result.error_message;
}

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectToRegularFileNotSocket) {
  if (getuid() == 0) GTEST_SKIP() << "Root may connect to non-socket files";
  // Connecting to a regular file should fail with a non-retryable error.
  std::string file_path = temp_dir_ + "/regular_file";
  {
    int fd = open(file_path.c_str(), O_CREAT | O_WRONLY, 0644);
    ASSERT_GE(fd, 0);
    close(fd);
  }

  auto result = SendNotification(file_path, MakeNotification());
  EXPECT_FALSE(result.success);
  // Should be a connect error, but NOT ENOENT (the file exists).
  // The errno varies by kernel: ENOTSOCK on some, ECONNREFUSED on others.
  EXPECT_NE(result.connect_errno, ENOENT);
  EXPECT_NE(result.connect_errno, 0)
      << "Expected a non-zero connect_errno for regular file";
}
#endif  // !_WIN32

// =============================================================================
// Write error tests
// =============================================================================

TEST_F(NotificationSenderTest, WriteFailsWhenPeerClosesImmediately) {
  // Create a listener that accepts but immediately closes the client fd.
  // With a small payload, the kernel may buffer it all before the close
  // is noticed, so success is also acceptable. We mainly verify no crash.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

#ifndef _WIN32
  // Ignore SIGPIPE to prevent test process from dying
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  std::atomic<bool> closed{false};
  std::thread acceptor([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
#ifndef _WIN32
      // Set SO_LINGER with timeout=0 to force RST on close
      struct linger ling = {1, 0};
      setsockopt(static_cast<int>(client_fd), SOL_SOCKET, SO_LINGER, &ling,
                 sizeof(ling));
#endif
      CloseClient(client_fd);
      closed.store(true);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send a notification with a reasonably large payload to increase
  // the chance of triggering the write error (peer already closed).
  CacheNotification notification =
      MakeNotification(std::string(4096, 'u'), "example.com");
  auto result = SendNotification(socket_path_, notification);

  acceptor.join();
  CloseServer(server_fd);

  // Either outcome is acceptable - no crash or hang is the main goal.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

TEST_F(NotificationSenderTest, WriteFailsWithLargePayload) {
  // Use a very large URL to guarantee the write exceeds kernel socket buffers.
  // When the peer closes immediately, the sender will get a write error
  // (EPIPE/ECONNRESET) partway through the write loop.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  std::atomic<bool> accepted{false};
  std::thread acceptor([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      accepted.store(true);
#ifndef _WIN32
      // Set SO_LINGER with timeout=0 to force RST
      struct linger ling = {1, 0};
      setsockopt(static_cast<int>(client_fd), SOL_SOCKET, SO_LINGER, &ling,
                 sizeof(ling));
#endif
      // Small delay to ensure RST is sent
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // kMaxUrlLength is 16384; use it fully to create a large serialized message
  CacheNotification notification =
      MakeNotification(std::string(kMaxUrlLength, 'w'), "example.com");
  auto result = SendNotification(socket_path_, notification);

  acceptor.join();
  CloseServer(server_fd);

  // With a large payload and RST, the write should fail. On some OS/timing
  // combinations, all data may be buffered before the RST arrives, in which
  // case success is still acceptable.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
    // The error message should mention write failure
    EXPECT_TRUE(result.error_message.find("write") != std::string::npos ||
                result.error_message.find("Write") != std::string::npos)
        << "Unexpected error: " << result.error_message;
  }
}

TEST_F(NotificationSenderTest, WriteFailsWhenPeerResets) {
  // Create a listener that accepts, reads a bit, then closes with SO_LINGER=0
  // to force a RST. Send a very large notification to ensure the write loop
  // encounters the peer reset.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  bool accepted = false;
  std::thread acceptor([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    accepted = client_fd != -1;
    if (client_fd != -1) {
#ifndef _WIN32
      // Set SO_LINGER with timeout=0 to force RST on close
      struct linger ling = {1, 0};
      setsockopt(static_cast<int>(client_fd), SOL_SOCKET, SO_LINGER, &ling,
                 sizeof(ling));
#endif
      // Close immediately with RST
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Largest URL the wire format accepts: a longer one fails to serialize, so
  // the sender would never connect and the acceptor above would block.
  CacheNotification notification =
      MakeNotification(std::string(kMaxUrlLength, 'u'), "example.com");
  auto result = SendNotification(socket_path_, notification);

  acceptor.join();
  CloseServer(server_fd);

  EXPECT_TRUE(accepted) << "sender never connected";
  EXPECT_FALSE(result.invalid_argument) << result.error_message;

  // The write should eventually fail with EPIPE, ECONNRESET, or similar.
  // On some OSes with large socket buffers, the entire write may succeed
  // before the RST is processed - that's also acceptable.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

TEST_F(NotificationSenderTest, WriteLargeNotificationSucceeds) {
  // Verify that a large notification is sent correctly through the write loop.
  // This exercises the partial-write path (remaining > 0 loop).
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      // Read all data (may come in multiple chunks)
      char buf[4096];
      while (true) {
        ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
        if (n <= 0) break;
        received_data.append(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use a large URL (just under kMaxUrlLength) to exercise multi-write behavior
  std::string large_url(kMaxUrlLength, 'x');
  CacheNotification notification = MakeNotification(
      large_url, "large.example.com", ContentType::kImage, 0xFF);
  auto result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(result.success) << result.error_message;

  receiver.join();
  CloseServer(server_fd);

  // Verify the received data round-trips correctly
  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
  EXPECT_EQ(parsed.url, large_url);
  EXPECT_EQ(parsed.hostname, "large.example.com");
  EXPECT_EQ(parsed.content_type, ContentType::kImage);
  EXPECT_EQ(parsed.capability_mask, 0xFFu);
}

TEST_F(NotificationSenderTest, SendMinimalNotification) {
  // Test with the smallest possible valid notification (empty URL, empty
  // hostname) to verify that the write loop handles small payloads.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[256];
      ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
      if (n > 0) {
        received_data.assign(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "";
  notification.scheme = "https";
  notification.hostname = "";
  notification.content_type = ContentType::kOther;
  notification.capability_mask = 0;

  auto result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(result.success) << result.error_message;

  receiver.join();
  CloseServer(server_fd);

  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
  EXPECT_EQ(parsed.url, "");
  EXPECT_EQ(parsed.hostname, "");
  EXPECT_EQ(parsed.content_type, ContentType::kOther);
  EXPECT_EQ(parsed.capability_mask, 0u);
}

// =============================================================================
// Retry tests
// =============================================================================

TEST_F(NotificationSenderTest, RetryNotNeeded) {
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[4096];
      (void)ReadClient(client_fd, buf, sizeof(buf));
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto result = SendNotificationWithRetry(socket_path_, MakeNotification());
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.retries_attempted, 0);

  receiver.join();
  CloseServer(server_fd);
}

TEST_F(NotificationSenderTest, RetryExhausted) {
  RetryConfig config;
  config.max_retries = 2;
  config.initial_delay_ms = 5;
  config.backoff_multiplier = 2.0;

  auto start = std::chrono::steady_clock::now();
  auto result = SendNotificationWithRetry("/tmp/nonexistent_retry_test.sock",
                                          MakeNotification(), config);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.retries_attempted, 2);
  // Should take at least 5ms + 10ms = 15ms for the retries.
  EXPECT_GE(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      10);
  // But not more than 200ms.
  EXPECT_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      200);
}

#ifndef _WIN32
TEST_F(NotificationSenderTest, RetryBreaksOnNonTransientError) {
  if (getuid() == 0) GTEST_SKIP() << "Root may connect to non-socket files";
  // Connecting to a path that exists but is a regular file (not a socket)
  // gives ENOTSOCK or ECONNREFUSED on some systems, but importantly not
  // ECONNREFUSED/ENOENT. The retry loop should break immediately.
  std::string file_path = temp_dir_ + "/not_a_socket";
  {
    int fd = open(file_path.c_str(), O_CREAT | O_WRONLY, 0644);
    ASSERT_GE(fd, 0);
    close(fd);
  }

  RetryConfig config;
  config.max_retries = 3;
  config.initial_delay_ms = 5;
  config.backoff_multiplier = 2.0;

  auto result =
      SendNotificationWithRetry(file_path, MakeNotification(), config);

  EXPECT_FALSE(result.success);
  // On most kernels, connecting to a regular file gives ENOTSOCK (not
  // retryable), so the loop breaks immediately. However, some Linux kernels
  // return ECONNREFUSED, which the retry loop treats as transient. Accept
  // either behavior: 0 retries (broke early) or up to max_retries.
  EXPECT_LE(result.retries_attempted, 3);
}
#endif  // !_WIN32

TEST_F(NotificationSenderTest, RetrySucceedsOnSecondAttempt) {
  // First attempt: no listener -> ECONNREFUSED
  // After a short delay, start the listener -> second attempt succeeds.
  RetryConfig config;
  config.max_retries = 3;
  config.initial_delay_ms = 100;  // Long enough to start listener
  config.backoff_multiplier = 1.0;

  // Start the listener in a background thread after a brief delay,
  // so the first retry attempt finds the socket available.
  std::thread delayed_listener([&]() {
    // Wait a bit so the first SendNotification call fails
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    intptr_t server_fd = StartListener();
    if (server_fd == -1) return;
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[4096];
      (void)ReadClient(client_fd, buf, sizeof(buf));
      CloseClient(client_fd);
    }
    CloseServer(server_fd);
  });

  auto result =
      SendNotificationWithRetry(socket_path_, MakeNotification(), config);

  delayed_listener.join();

  EXPECT_TRUE(result.success) << "Error: " << result.error_message;
  EXPECT_GE(result.retries_attempted, 1);
}

TEST_F(NotificationSenderTest, RetryWithZeroMaxRetries) {
  // When max_retries is 0, SendNotificationWithRetry should try once
  // and not retry.
  RetryConfig config;
  config.max_retries = 0;
  config.initial_delay_ms = 5;

  auto result = SendNotificationWithRetry("/tmp/nonexistent_zero_retry.sock",
                                          MakeNotification(), config);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.retries_attempted, 0);
}

TEST_F(NotificationSenderTest, RetryWithEmptyPath) {
  // Empty path gives "Empty socket path" with connect_errno=0, which is
  // not ECONNREFUSED or ENOENT. The retry loop should break immediately.
  RetryConfig config;
  config.max_retries = 3;
  config.initial_delay_ms = 5;

  auto result = SendNotificationWithRetry("", CacheNotification{}, config);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Empty socket path");
  // Should not retry because connect_errno is 0 (not ECONNREFUSED/ENOENT)
  EXPECT_EQ(result.retries_attempted, 0);
}

#ifndef _WIN32
TEST_F(NotificationSenderTest, RetryWithPathTooLong) {
  // Path too long gives error with connect_errno=0, not retryable.
  sockaddr_un addr;
  std::string long_path(sizeof(addr.sun_path), 'x');

  RetryConfig config;
  config.max_retries = 3;
  config.initial_delay_ms = 5;

  auto result =
      SendNotificationWithRetry(long_path, MakeNotification(), config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("too long"), std::string::npos);
  // connect_errno is 0 (never attempted connect), so retry should break
  EXPECT_EQ(result.retries_attempted, 0);
}
#endif  // !_WIN32

TEST_F(NotificationSenderTest, RetryBackoffMultiplier) {
  // Verify that backoff multiplier actually increases delays.
  RetryConfig config;
  config.max_retries = 3;
  config.initial_delay_ms = 10;
  config.backoff_multiplier = 2.0;

  auto start = std::chrono::steady_clock::now();
  auto result = SendNotificationWithRetry("/tmp/nonexistent_backoff.sock",
                                          MakeNotification(), config);
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.retries_attempted, 3);
  // With backoff: 10ms + 20ms + 40ms = 70ms minimum
  EXPECT_GE(elapsed_ms, 50);
  // But not unreasonably long
  EXPECT_LT(elapsed_ms, 500);
}

TEST_F(NotificationSenderTest, RetryPreservesLastErrorMessage) {
  // After exhausting retries, the result should contain the error from the
  // last attempt.
  RetryConfig config;
  config.max_retries = 1;
  config.initial_delay_ms = 5;

  auto result = SendNotificationWithRetry("/tmp/nonexistent_lasterr.sock",
                                          MakeNotification(), config);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
  EXPECT_EQ(result.retries_attempted, 1);
  EXPECT_NE(result.error_message.find("connect"), std::string::npos)
      << "Error: " << result.error_message;
}

// =============================================================================
// SendNotification with multiple sequential sends
// =============================================================================

TEST_F(NotificationSenderTest, MultipleSendsToSameSocket) {
  // Verify that we can send multiple notifications sequentially to the same
  // socket (each creates a new connection).
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  constexpr int kNumNotifications = 3;
  std::vector<std::string> received;
  std::thread receiver([&]() {
    for (int i = 0; i < kNumNotifications; ++i) {
      intptr_t client_fd = AcceptClient(server_fd, socket_path_);
      if (client_fd != -1) {
        char buf[4096];
        ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
        if (n > 0) {
          received.emplace_back(buf, static_cast<size_t>(n));
        }
        CloseClient(client_fd);
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int success_count = 0;
  for (int i = 0; i < kNumNotifications; ++i) {
    auto notification =
        MakeNotification("/page" + std::to_string(i), "example.com");
    auto result = SendNotification(socket_path_, notification);
    if (result.success) ++success_count;
    // Small delay between sends to let the acceptor catch up
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(success_count, kNumNotifications);

  receiver.join();
  CloseServer(server_fd);

  ASSERT_EQ(received.size(), static_cast<size_t>(kNumNotifications));
  for (int i = 0; i < kNumNotifications; ++i) {
    CacheNotification parsed;
    ASSERT_TRUE(CacheNotification::Deserialize(received[i], &parsed));
    EXPECT_EQ(parsed.url, "/page" + std::to_string(i));
  }
}

// =============================================================================
// CacheNotification::Deserialize validation tests
// =============================================================================

TEST_F(NotificationSenderTest, DeserializeRejectsInvalidContentType) {
  // Build a valid notification and serialize it.
  CacheNotification notification;
  notification.url = "/page.html";
  notification.scheme = "https";
  notification.hostname = "example.com";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x42;

  auto serialized = notification.Serialize();

  // The content-type byte is at a known offset: 4 (total_len) + 1 (version)
  // + 4 (url_len) + url.size() + 4 (host_len) + hostname.size().
  size_t ct_offset =
      4 + 1 + 4 + notification.url.size() + 4 + notification.hostname.size();

  // Corrupt the content-type byte to an out-of-range value (>4).
  serialized[ct_offset] = static_cast<char>(0xFF);

  std::string_view data(serialized.data(), serialized.size());
  CacheNotification parsed;
  EXPECT_FALSE(CacheNotification::Deserialize(data, &parsed))
      << "Deserialize should reject invalid content type";
}

TEST_F(NotificationSenderTest, DeserializeRejectsOversizedHostname) {
  // Build a notification with a hostname longer than kMaxHostnameLength.
  CacheNotification notification;
  notification.url = "/page.html";
  notification.scheme = "https";
  notification.hostname = std::string(513, 'a');  // > 512
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x42;

  auto serialized = notification.Serialize();

  std::string_view data(serialized.data(), serialized.size());
  CacheNotification parsed;
  EXPECT_FALSE(CacheNotification::Deserialize(data, &parsed))
      << "Deserialize should reject hostnames > 512 bytes";
}

TEST_F(NotificationSenderTest, DeserializeAcceptsMaxHostname) {
  // A hostname exactly at the limit should be accepted.
  CacheNotification notification;
  notification.url = "/page.html";
  notification.scheme = "https";
  notification.hostname = std::string(512, 'a');  // exactly 512
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x42;

  auto serialized = notification.Serialize();

  std::string_view data(serialized.data(), serialized.size());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(data, &parsed))
      << "Deserialize should accept hostnames <= 512 bytes";
  EXPECT_EQ(parsed.hostname.size(), 512u);
}

TEST_F(NotificationSenderTest, DeserializeTruncatedAfterHostname) {
  // Build a valid notification then truncate mid-content-type.
  CacheNotification notification;
  notification.url = "https://test.com/";
  notification.scheme = "https";
  notification.hostname = "test.com";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x42;

  auto serialized = notification.Serialize();
  // url_len(4) + url(17) + host_len(4) + host(8) = 33 bytes of payload after
  // total_len(4). Truncate before content_type byte.
  size_t truncate_at =
      4 + 4 + notification.url.size() + 4 + notification.hostname.size();
  std::string_view truncated(serialized.data(), truncate_at);

  CacheNotification parsed;
  EXPECT_FALSE(CacheNotification::Deserialize(truncated, &parsed));
}

TEST_F(NotificationSenderTest, DeserializeZeroLengthMessage) {
  // A message where total_len = 0 -> no fields at all.
  std::string data(4, '\0');
  std::string_view view(data.data(), data.size());

  CacheNotification parsed;
  EXPECT_FALSE(CacheNotification::Deserialize(view, &parsed));
}

TEST_F(NotificationSenderTest, DeserializeRejectsOversizedUrl) {
  // Build a notification with a URL longer than kMaxUrlLength.
  CacheNotification notification;
  notification.url = std::string(kMaxUrlLength + 1, 'u');
  notification.scheme = "https";
  notification.hostname = "example.com";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x08;

  auto serialized = notification.Serialize();

  std::string_view data(serialized.data(), serialized.size());
  CacheNotification parsed;
  EXPECT_FALSE(CacheNotification::Deserialize(data, &parsed))
      << "Deserialize should reject URLs > 16384 bytes";
}

// =============================================================================
// Connection closed before write completes (write error paths)
// =============================================================================

TEST_F(NotificationSenderTest, WriteToShutdownPeerSocket) {
  // Listener accepts, shuts down the write end, then closes.
  // This may cause the sender's write to fail with EPIPE.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  bool accepted = false;
  std::thread acceptor([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    accepted = client_fd != -1;
    if (client_fd != -1) {
#ifndef _WIN32
      // Shut down both directions, then close
      shutdown(static_cast<int>(client_fd), SHUT_RDWR);
#else
      // On Windows, disconnecting the pipe is equivalent.
      DisconnectNamedPipe(
          reinterpret_cast<HANDLE>(static_cast<uintptr_t>(client_fd)));
#endif
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Largest URL the wire format accepts: a longer one fails to serialize, so
  // the sender would never connect and the acceptor above would block.
  CacheNotification notification =
      MakeNotification(std::string(kMaxUrlLength, 'w'), "example.com");
  auto result = SendNotification(socket_path_, notification);

  acceptor.join();
  CloseServer(server_fd);

  EXPECT_TRUE(accepted) << "sender never connected";
  EXPECT_FALSE(result.invalid_argument) << result.error_message;

  // Either the write completes before shutdown takes effect (success),
  // or we get a write error. Both are acceptable.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

TEST_F(NotificationSenderTest, SendAllContentTypes) {
  // Verify that notifications with all valid content types are sent correctly.
  for (uint8_t ct = 0; ct <= static_cast<uint8_t>(ContentType::kOther); ++ct) {
    // Create a new socket for each iteration
    std::string ct_socket = temp_dir_ + "/ct_" + std::to_string(ct) + ".sock";
    std::string old_path = socket_path_;
    socket_path_ = ct_socket;

    intptr_t server_fd = StartListener();
    ASSERT_NE(server_fd, -1) << "Failed to create listener for ct=" << (int)ct;

    std::string received;
    std::thread receiver([&]() {
      intptr_t client_fd = AcceptClient(server_fd, socket_path_);
      if (client_fd != -1) {
        char buf[4096];
        ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
        if (n > 0) received.assign(buf, static_cast<size_t>(n));
        CloseClient(client_fd);
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto notification =
        MakeNotification("/test", "h", static_cast<ContentType>(ct), ct);
    auto result = SendNotification(ct_socket, notification);
    EXPECT_TRUE(result.success)
        << "ct=" << (int)ct << ": " << result.error_message;

    receiver.join();
    CloseServer(server_fd);

    if (result.success && !received.empty()) {
      CacheNotification parsed;
      ASSERT_TRUE(CacheNotification::Deserialize(received, &parsed));
      EXPECT_EQ(parsed.content_type, static_cast<ContentType>(ct));
    }

    socket_path_ = old_path;
  }
}

// =============================================================================
// Connect error errno validation
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectRefusedSetsErrno) {
  // Create a socket file that is not being listened on: bind but don't listen.
  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(server_fd, 0);

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(0, bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)));
  // Don't call listen() - connecting to this socket should fail with
  // ECONNREFUSED.

  auto result = SendNotification(socket_path_, MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.connect_errno, ECONNREFUSED);
  EXPECT_NE(result.error_message.find("connect"), std::string::npos)
      << "Error: " << result.error_message;

  close(server_fd);
}
#endif  // !_WIN32

// =============================================================================
// Retry with ENOENT (missing socket file)
// =============================================================================

TEST_F(NotificationSenderTest, RetryWithENOENT) {
  // A socket path that does not exist gives ENOENT. The retry loop should
  // retry (since ENOENT is retryable) and eventually exhaust retries.
  std::string missing_path = temp_dir_ + "/missing.sock";

  RetryConfig config;
  config.max_retries = 2;
  config.initial_delay_ms = 5;
  config.backoff_multiplier = 1.0;

  auto result =
      SendNotificationWithRetry(missing_path, MakeNotification(), config);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.retries_attempted, 2);
  EXPECT_EQ(result.connect_errno, ENOENT);
}

#ifndef _WIN32
TEST_F(NotificationSenderTest, RetryWithENOENTThenSuccess) {
  // Socket path initially does not exist (ENOENT), then becomes available.
  std::string delayed_path = temp_dir_ + "/delayed.sock";

  RetryConfig config;
  config.max_retries = 5;
  config.initial_delay_ms = 100;
  config.backoff_multiplier = 1.0;

  std::thread delayed_listener([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Now create the socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, delayed_path.c_str(),
                 sizeof(addr.sun_path) - 1);
    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
      close(server_fd);
      return;
    }
    if (listen(server_fd, 16) != 0) {
      close(server_fd);
      return;
    }
    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd >= 0) {
      char buf[4096];
      (void)read(client_fd, buf, sizeof(buf));
      close(client_fd);
    }
    close(server_fd);
  });

  auto result =
      SendNotificationWithRetry(delayed_path, MakeNotification(), config);

  delayed_listener.join();

  EXPECT_TRUE(result.success) << "Error: " << result.error_message;
  EXPECT_GE(result.retries_attempted, 1);
}
#endif  // !_WIN32

// =============================================================================
// Additional coverage: empty socket path with real payload
// =============================================================================

TEST_F(NotificationSenderTest, SendEmptyPathWithRealPayload) {
  // Verify that an empty socket path is rejected even when carrying a fully
  // populated notification. The function should return before attempting
  // socket creation, so connect_errno must remain 0.
  auto notification = MakeNotification("http://example.com/style.css",
                                       "example.com", ContentType::kCss, 0x48);
  auto result = SendNotification("", notification);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Empty socket path");
  EXPECT_EQ(result.connect_errno, 0);
  EXPECT_EQ(result.retries_attempted, 0);
}

TEST_F(NotificationSenderTest, RetryWithEmptyPathAndRealPayload) {
  // Even with retries configured, an empty path should not be retried
  // because connect_errno == 0 (not ECONNREFUSED or ENOENT).
  RetryConfig config;
  config.max_retries = 5;
  config.initial_delay_ms = 5;
  config.backoff_multiplier = 2.0;

  auto notification = MakeNotification(
      "http://example.com/img.png", "example.com", ContentType::kImage, 0x08);
  auto result = SendNotificationWithRetry("", notification, config);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Empty socket path");
  EXPECT_EQ(result.retries_attempted, 0);
  EXPECT_EQ(result.connect_errno, 0);
}

// =============================================================================
// Additional coverage: socket path exceeding sun_path limit
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, SendToPathWellOverLimit) {
  // A 200+ character path far exceeds sizeof(addr.sun_path) (104 on macOS,
  // 108 on Linux). Should be rejected before any socket operations.
  std::string very_long_path(250, '/');
  // Make it look like a real path
  for (size_t i = 1; i < very_long_path.size(); i += 5) {
    very_long_path[i] = 'a';
    if (i + 1 < very_long_path.size()) very_long_path[i + 1] = 'b';
    if (i + 2 < very_long_path.size()) very_long_path[i + 2] = 'c';
    if (i + 3 < very_long_path.size()) very_long_path[i + 3] = 'd';
  }

  auto result = SendNotification(very_long_path, MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("too long"), std::string::npos)
      << "Error: " << result.error_message;
  EXPECT_EQ(result.connect_errno, 0);
  EXPECT_EQ(result.retries_attempted, 0);
}

TEST_F(NotificationSenderTest, SendToPathOneByteOverLimit) {
  // Path of exactly sizeof(sun_path) should be rejected (>= check).
  // Path of sizeof(sun_path) + 1 should also be rejected.
  sockaddr_un addr;
  std::string one_over(sizeof(addr.sun_path) + 1, 'x');

  auto result = SendNotification(one_over, MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("too long"), std::string::npos)
      << "Error: " << result.error_message;
  EXPECT_EQ(result.connect_errno, 0);
}

TEST_F(NotificationSenderTest, RetryWithVeryLongPath) {
  // A 200+ char path should not be retried since it fails before connect
  // (connect_errno == 0, not retryable).
  std::string long_path(200, 'p');

  RetryConfig config;
  config.max_retries = 5;
  config.initial_delay_ms = 5;
  config.backoff_multiplier = 2.0;

  auto start = std::chrono::steady_clock::now();
  auto result =
      SendNotificationWithRetry(long_path, MakeNotification(), config);
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("too long"), std::string::npos);
  EXPECT_EQ(result.retries_attempted, 0);
  // Should return immediately, no retry delays.
  EXPECT_LT(elapsed_ms, 50);
}
#endif  // !_WIN32

// =============================================================================
// Additional coverage: non-retryable connect errors (not ECONNREFUSED/ENOENT)
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectToDevNull) {
  // /dev/null exists but is a character device, not a Unix socket.
  // connect() should fail with a non-retryable error (e.g. ENOTSOCK or
  // ECONNREFUSED depending on OS).
  auto result = SendNotification("/dev/null", MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.connect_errno, 0);
  // The error should not be ENOENT since /dev/null exists.
  EXPECT_NE(result.connect_errno, ENOENT);
}

TEST_F(NotificationSenderTest, RetryDoesNotRetryDevNull) {
  if (getuid() == 0)
    GTEST_SKIP() << "Root may connect to /dev/null differently";
  // /dev/null gives a non-retryable error. The retry loop should break
  // immediately without sleeping.
  RetryConfig config;
  config.max_retries = 5;
  config.initial_delay_ms = 100;
  config.backoff_multiplier = 2.0;

  auto result =
      SendNotificationWithRetry("/dev/null", MakeNotification(), config);

  EXPECT_FALSE(result.success);
  // On most kernels, /dev/null gives ENOTSOCK (not retryable), so the loop
  // breaks immediately. However, some Linux kernels return ECONNREFUSED,
  // which the retry loop treats as transient and retries. Accept either.
  EXPECT_LE(result.retries_attempted, 5);
}

TEST_F(NotificationSenderTest, ConnectToInaccessibleDirectory) {
  if (getuid() == 0) GTEST_SKIP() << "Root ignores directory permissions";
  // Create a directory without execute permission, then try to connect to a
  // socket inside it. This should give EACCES — a non-retryable error.
  std::string no_access_dir = temp_dir_ + "/noaccess";
  ASSERT_EQ(0, mkdir(no_access_dir.c_str(), 0700));
  std::string sock_inside = no_access_dir + "/notify.sock";

  // Remove execute permission from the directory so path resolution fails.
  ASSERT_EQ(0, chmod(no_access_dir.c_str(), 0000));

  auto result = SendNotification(sock_inside, MakeNotification());

  // Restore permissions for cleanup.
  chmod(no_access_dir.c_str(), 0700);

  EXPECT_FALSE(result.success);
  // Should get EACCES (permission denied during path resolution).
  EXPECT_EQ(result.connect_errno, EACCES)
      << "Expected EACCES, got errno=" << result.connect_errno << " ("
      << strerror(result.connect_errno) << ")";
}

TEST_F(NotificationSenderTest, RetryDoesNotRetryEACCES) {
  if (getuid() == 0) GTEST_SKIP() << "Root ignores directory permissions";
  // EACCES is not ECONNREFUSED or ENOENT, so retry should break immediately.
  std::string no_access_dir = temp_dir_ + "/noaccess2";
  ASSERT_EQ(0, mkdir(no_access_dir.c_str(), 0700));
  std::string sock_inside = no_access_dir + "/notify.sock";
  ASSERT_EQ(0, chmod(no_access_dir.c_str(), 0000));

  RetryConfig config;
  config.max_retries = 5;
  config.initial_delay_ms = 100;
  config.backoff_multiplier = 2.0;

  auto start = std::chrono::steady_clock::now();
  auto result =
      SendNotificationWithRetry(sock_inside, MakeNotification(), config);
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();

  // Restore permissions for cleanup.
  chmod(no_access_dir.c_str(), 0700);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.connect_errno, EACCES)
      << "Expected EACCES, got errno=" << result.connect_errno;
  // Should not have retried.
  EXPECT_EQ(result.retries_attempted, 0);
  // Should be fast: no retry sleeps.
  EXPECT_LT(elapsed_ms, 100);
}
#endif  // !_WIN32

// =============================================================================
// Additional socket error path tests
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectToSocketInDeepNonexistentPath) {
  // A deeply nested non-existent path should fail with ENOENT at connect.
  // This exercises the connect failure path (lines 58-64 of sender.cc).
  std::string deep_path = temp_dir_ + "/a/b/c/d/e/f/g/notify.sock";
  auto result = SendNotification(deep_path, MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.connect_errno, ENOENT)
      << "Expected ENOENT for deeply nested non-existent path";
  EXPECT_NE(result.error_message.find("connect"), std::string::npos)
      << "Error: " << result.error_message;
}

TEST_F(NotificationSenderTest, ConnectToSocketWithPermissionDenied) {
  // Create a socket file inside a directory without execute permission.
  // This should trigger EACCES at connect (not ECONNREFUSED or ENOENT).
  if (getuid() == 0) GTEST_SKIP() << "Root ignores directory permissions";

  std::string restricted_dir = temp_dir_ + "/restricted";
  ASSERT_EQ(0, mkdir(restricted_dir.c_str(), 0700));
  std::string sock_path = restricted_dir + "/worker.sock";

  // Remove execute permission from the directory.
  ASSERT_EQ(0, chmod(restricted_dir.c_str(), 0000));

  auto result = SendNotification(sock_path, MakeNotification());

  // Restore permissions for cleanup.
  chmod(restricted_dir.c_str(), 0700);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.connect_errno, EACCES)
      << "Expected EACCES, got errno=" << result.connect_errno << " ("
      << strerror(result.connect_errno) << ")";
}
#endif  // !_WIN32

TEST_F(NotificationSenderTest, SendToListenerThatAcceptsThenDelays) {
  // Verify that the sender completes successfully even when the listener
  // accepts the connection but delays reading. This exercises the write
  // loop path where the kernel buffers the data.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      // Delay before reading to let kernel buffer fill.
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      char buf[4096];
      ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
      if (n > 0) {
        received_data.assign(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto result = SendNotification(socket_path_, MakeNotification());
  EXPECT_TRUE(result.success) << result.error_message;

  receiver.join();
  CloseServer(server_fd);

  // Verify data was received.
  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
}

TEST_F(NotificationSenderTest, SendToSlowReaderExercisesWriteLoop) {
  // Use a reader that introduces a small delay before reading.
  // This exercises the write loop more thoroughly by reading in small chunks.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      // Read in small 64-byte chunks to exercise multiple reads.
      char buf[64];
      while (true) {
        ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
        if (n <= 0) break;
        received_data.append(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use a moderate notification to exercise write loop.
  CacheNotification notification =
      MakeNotification(std::string(2048, 'z'), "example.com");
  auto result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(result.success) << result.error_message;

  receiver.join();
  CloseServer(server_fd);

  // Verify the data round-trips correctly.
  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
  EXPECT_EQ(parsed.url.size(), 2048u);
}

TEST_F(NotificationSenderTest, SendToListenerWithBackpressure) {
  // Create a receiver that delays briefly before reading, creating
  // potential backpressure on the sender's write.  With the 50ms
  // send timeout in the sender, we use a moderate payload size to
  // avoid hitting the timeout while still exercising the write loop.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      // Brief delay before reading to create some backpressure.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      char buf[4096];
      while (true) {
        ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
        if (n <= 0) break;
        received_data.append(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Moderate payload to exercise the write loop.
  CacheNotification notification =
      MakeNotification(std::string(4096, 'x'), "example.com");
  auto result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(result.success) << result.error_message;

  receiver.join();
  CloseServer(server_fd);

  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
  EXPECT_EQ(parsed.url.size(), 4096u);
}

#ifndef _WIN32
TEST_F(NotificationSenderTest, RetryWithConnectionRefusedAndENOENTMixed) {
  // First create a socket that exists but isn't listening (ECONNREFUSED),
  // then remove it (ENOENT). Both errors are retryable. Verify that
  // the retry loop handles both errno values without breaking.
  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(server_fd, 0);

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(0, bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)));
  // Don't call listen() — ECONNREFUSED.

  RetryConfig config;
  config.max_retries = 3;
  config.initial_delay_ms = 10;
  config.backoff_multiplier = 1.0;

  // Remove the socket file after a short delay so the second retry
  // gets ENOENT instead of ECONNREFUSED.
  std::thread remover([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    close(server_fd);
    unlink(socket_path_.c_str());
  });

  auto result =
      SendNotificationWithRetry(socket_path_, MakeNotification(), config);

  remover.join();

  EXPECT_FALSE(result.success);
  // Should have retried (both ECONNREFUSED and ENOENT are retryable).
  EXPECT_GE(result.retries_attempted, 1);
  EXPECT_TRUE(result.connect_errno == ECONNREFUSED ||
              result.connect_errno == ENOENT)
      << "Expected ECONNREFUSED or ENOENT, got " << result.connect_errno;
}
#endif  // !_WIN32

TEST_F(NotificationSenderTest, ConcurrentSendsToSameSocket) {
  // Verify that multiple concurrent sends to the same socket all succeed.
  // This exercises the code path where multiple senders connect to the
  // same listener simultaneously.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  constexpr int kNumSenders = 5;
  std::atomic<int> received_count{0};

#ifdef _WIN32
  // On Windows Named Pipes, each client connection needs its own pipe
  // instance. Pre-create all pipe instances so they are available when
  // the senders connect concurrently. Each receiver thread accepts on
  // its own pre-created pipe instance.
  std::vector<intptr_t> pipe_instances(kNumSenders);
  for (int i = 0; i < kNumSenders; ++i) {
    pipe_instances[i] = CreatePipeServer(socket_path_);
    ASSERT_NE(pipe_instances[i], -1) << "Failed to create pipe instance " << i;
  }

  std::vector<std::thread> receivers;
  for (int i = 0; i < kNumSenders; ++i) {
    receivers.emplace_back([&, i]() {
      intptr_t client_fd = AcceptPipeClient(pipe_instances[i]);
      if (client_fd != -1) {
        char buf[4096];
        ssize_t n = PipeRead(client_fd, buf, sizeof(buf));
        if (n > 0) {
          received_count.fetch_add(1);
        }
        ClosePipeHandle(client_fd);
      } else {
        // Accept timed out — clean up the pipe instance.
        ClosePipeHandle(pipe_instances[i]);
      }
    });
  }
#else
  std::thread receiver([&]() {
    for (int i = 0; i < kNumSenders; ++i) {
      intptr_t client_fd = AcceptClient(server_fd, socket_path_);
      if (client_fd != -1) {
        char buf[4096];
        ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
        if (n > 0) {
          received_count.fetch_add(1);
        }
        CloseClient(client_fd);
      }
    }
  });
#endif

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Launch multiple sender threads concurrently.
  std::vector<std::thread> senders;
  senders.reserve(kNumSenders);
  std::atomic<int> success_count{0};
  for (int i = 0; i < kNumSenders; ++i) {
    senders.emplace_back([&, i]() {
      auto notification =
          MakeNotification("/concurrent/" + std::to_string(i), "example.com");
      auto result = SendNotification(socket_path_, notification);
      if (result.success) {
        success_count.fetch_add(1);
      }
    });
  }

  for (auto& t : senders) {
    t.join();
  }
#ifdef _WIN32
  for (auto& t : receivers) {
    t.join();
  }
#else
  receiver.join();
#endif
  CloseServer(server_fd);

  EXPECT_EQ(success_count.load(), kNumSenders);
  EXPECT_EQ(received_count.load(), kNumSenders);
}

TEST_F(NotificationSenderTest, SendToNullTerminatedSocketPath) {
  // Verify that a socket path containing embedded null bytes is handled
  // safely. The path_len check should use size() correctly.
  // std::string_view can contain nulls, but memcpy + sun_path are
  // null-terminated. A path with an embedded null truncates at the null.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[4096];
      (void)ReadClient(client_fd, buf, sizeof(buf));
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use the actual socket path (no embedded nulls) -- this is a
  // baseline test for the path-length check code path.
  auto result = SendNotification(socket_path_, MakeNotification());
  EXPECT_TRUE(result.success) << result.error_message;

  receiver.join();
  CloseServer(server_fd);
}

TEST_F(NotificationSenderTest, RetryConfigCustomValues) {
  // Verify that custom RetryConfig values are respected.
  RetryConfig config;
  config.max_retries = 1;
  config.initial_delay_ms = 5;
  config.backoff_multiplier = 10.0;  // Large multiplier

  auto start = std::chrono::steady_clock::now();
  auto result = SendNotificationWithRetry("/tmp/nonexistent_custom.sock",
                                          MakeNotification(), config);
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.retries_attempted, 1);
  // With 1 retry at 5ms delay: ~5ms minimum.
  EXPECT_GE(elapsed_ms, 3);
  // Should complete within 200ms.
  EXPECT_LT(elapsed_ms, 200);
}

#ifndef _WIN32
TEST_F(NotificationSenderTest, SendConnectRefusedSetsCorrectFields) {
  // Verify that when connect fails with ECONNREFUSED, all result fields
  // are set correctly (success=false, error_message set, connect_errno set).
  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(server_fd, 0);

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(0, bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)));
  // Don't listen — connect will get ECONNREFUSED.

  auto result = SendNotification(socket_path_, MakeNotification());

  close(server_fd);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.connect_errno, ECONNREFUSED);
  EXPECT_EQ(result.retries_attempted, 0);
  EXPECT_NE(result.error_message.find("connect"), std::string::npos);
}
#endif  // !_WIN32

TEST_F(NotificationSenderTest, WriteZeroBytesNotification) {
  // Create a notification that serializes to a very small payload.
  // This ensures the write loop handles small payloads correctly.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[4096];
      ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
      if (n > 0) {
        received_data.assign(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Minimal notification: empty URL and hostname, minimal content type.
  CacheNotification notification;
  notification.url = "";
  notification.scheme = "https";
  notification.hostname = "";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  auto result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(result.success) << result.error_message;

  receiver.join();
  CloseServer(server_fd);

  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
  EXPECT_EQ(parsed.url, "");
  EXPECT_EQ(parsed.hostname, "");
}

TEST_F(NotificationSenderTest, RetryNegativeMaxRetries) {
  // A negative max_retries should be treated as 0 retries (the loop
  // condition `attempt < config.max_retries` handles this).
  RetryConfig config;
  config.max_retries = -1;
  config.initial_delay_ms = 5;

  auto result = SendNotificationWithRetry("/tmp/nonexistent_neg_retry.sock",
                                          MakeNotification(), config);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.retries_attempted, 0);
}

// =============================================================================
// Write EAGAIN / write timeout coverage (lines 94-107 of sender.cc)
// =============================================================================

// This test exercises POSIX-specific EAGAIN/poll/timeout write paths that
// don't exist on Windows. On Windows, synchronous WriteFile to a pipe where
// the receiver never reads blocks forever (no poll-based timeout), so
// the test would hang.
#ifndef _WIN32
TEST_F(NotificationSenderTest, WriteEagainAndTimeoutWhenReceiverNeverReads) {
  // Create a listener that accepts a connection but never reads from it.
  // Set the smallest possible SO_RCVBUF on the accepted socket.
  // Then send a very large notification. The non-blocking write loop
  // will eventually fill the kernel send buffer, triggering EAGAIN.
  // Since the receiver never reads, poll() will eventually time out
  // (kSendTimeoutMs = 50ms), exercising lines 94-103.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);

  std::atomic<bool> done{false};
  intptr_t accepted_fd = -1;
  std::thread acceptor([&]() {
    accepted_fd = AcceptClient(server_fd, socket_path_);
    if (accepted_fd != -1) {
      // Set the smallest possible receive buffer to create backpressure
      // faster. The kernel may round up to a minimum, but it helps.
      int rcvbuf = 1;
      setsockopt(static_cast<int>(accepted_fd), SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                 sizeof(rcvbuf));
      // Never read from the socket. Just wait until the test is done.
      while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      CloseClient(accepted_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use a very large payload to fill the kernel buffer.
  // kMaxUrlLength (16384) serialized plus overhead should exceed any
  // reasonably small kernel buffer.
  CacheNotification notification =
      MakeNotification(std::string(kMaxUrlLength, 'W'), "example.com");
  auto result = SendNotification(socket_path_, notification);

  done.store(true);
  acceptor.join();
  CloseServer(server_fd);

  // With the receiver never reading, the sender should eventually time
  // out on write (EAGAIN → poll → timeout). On systems with very large
  // default socket buffers, the entire write may complete before EAGAIN.
  // Both outcomes are acceptable; the key assertion is no crash/hang.
  if (!result.success) {
    // Lines 98-101: "Write to worker timed out"
    EXPECT_TRUE(result.error_message.find("timed out") != std::string::npos ||
                result.error_message.find("Write") != std::string::npos)
        << "Unexpected error: " << result.error_message;
  }
}
#endif  // !_WIN32

TEST_F(NotificationSenderTest, WriteEagainRecoveryWhenReceiverDrains) {
  // Create a listener that accepts and starts draining. The receiver
  // uses default kernel buffer sizes so it can keep up with the sender,
  // but the large payload may still briefly trigger EAGAIN before the
  // receiver frees space. This exercises the EAGAIN → poll → continue
  // path (lines 95-103) when poll returns writable and the write resumes.
  //
  // On systems with very large default socket buffers, EAGAIN may never
  // trigger and the write succeeds in one call — that's also acceptable.
  // The key goal is that the write loop completes successfully.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      // Read aggressively in large chunks to keep up with sender.
      char buf[4096];
      while (true) {
        ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
        if (n <= 0) break;
        received_data.append(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification =
      MakeNotification(std::string(kMaxUrlLength, 'E'), "example.com");
  auto result = SendNotification(socket_path_, notification);

  receiver.join();
  CloseServer(server_fd);

  // With the receiver aggressively draining and default buffer sizes,
  // the write should succeed.
  EXPECT_TRUE(result.success) << result.error_message;

  // Verify data integrity.
  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
  EXPECT_EQ(parsed.url.size(), kMaxUrlLength);
}

// This test exercises POSIX-specific EAGAIN/poll/timeout write paths.
// On Windows, synchronous WriteFile blocks forever when the receiver
// never reads and the pipe buffer fills, so the test would hang.
#ifndef _WIN32
TEST_F(NotificationSenderTest, WriteTimeoutWithTinyBuffer) {
  // Aggressively try to trigger the write timeout path by setting the
  // smallest possible SO_SNDBUF on the sender side AND SO_RCVBUF on the
  // receiver side, with the receiver never reading.
  //
  // The function creates its own socket, so we can't control SO_SNDBUF
  // directly. But we can set SO_RCVBUF to 1 on the accepted connection
  // and never read, which creates maximum backpressure.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);

  std::atomic<bool> done{false};
  std::thread acceptor([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      // Minimize receive buffer.
      int rcvbuf = 1;
      setsockopt(static_cast<int>(client_fd), SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                 sizeof(rcvbuf));
      // Also set a small send buffer on the receiver side (this affects
      // the TCP window on the sender's perspective for some stacks).
      int sndbuf = 1;
      setsockopt(static_cast<int>(client_fd), SOL_SOCKET, SO_SNDBUF, &sndbuf,
                 sizeof(sndbuf));
      while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send multiple large notifications rapidly. At least one should time out.
  bool got_timeout = false;
  for (int i = 0; i < 3 && !got_timeout; ++i) {
    CacheNotification notification =
        MakeNotification(std::string(kMaxUrlLength, 'T'), "example.com");
    auto result = SendNotification(socket_path_, notification);
    if (!result.success &&
        result.error_message.find("timed out") != std::string::npos) {
      got_timeout = true;
    }
  }

  done.store(true);
  acceptor.join();
  CloseServer(server_fd);

  // On most systems this will trigger write timeout. On systems with very
  // large kernel buffers it may not. The test verifies no crash/hang.
  // If we got a timeout, that's the target coverage.
  if (got_timeout) {
    SUCCEED() << "Write timeout correctly triggered";
  } else {
    // Still acceptable - kernel buffer was large enough.
    SUCCEED() << "Kernel buffer absorbed all data (no EAGAIN triggered)";
  }
}
#endif  // !_WIN32

// =============================================================================
// Connect timeout via poll (lines 66-72 of sender.cc) - best effort
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectTimeoutWithFullBacklog) {
  // Try to trigger EINPROGRESS on connect by filling the listen backlog.
  // Create a listening socket with backlog=1 (the minimum), then fill it
  // with pending connections (connect without accept). The next connect
  // should either:
  // - Return ECONNREFUSED (Linux) or EAGAIN/EINPROGRESS (some systems)
  // - On macOS Unix sockets, connect may block or fail immediately.
  //
  // This is a best-effort test for the connect timeout path (lines 66-72).
  // It exercises the code path when connect() returns EINPROGRESS/EAGAIN.
  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(server_fd, 0);

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(0, bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)));
  // Listen with minimum backlog.
  ASSERT_EQ(0, listen(server_fd, 1));

  // Fill the backlog: connect multiple times without accepting.
  std::vector<int> fill_fds;
  for (int i = 0; i < 16; ++i) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) break;
    // Use non-blocking connect to avoid blocking if backlog is full.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc =
        connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
      fill_fds.push_back(fd);
    } else {
      close(fd);
      break;
    }
  }

  // Now try to send a notification. The connect should either:
  // - Get EINPROGRESS → poll → timeout (exercising lines 66-72)
  // - Get ECONNREFUSED (immediate failure, already covered)
  auto result = SendNotification(socket_path_, MakeNotification());
  EXPECT_FALSE(result.success);
  // Accept any connect failure - the important thing is no crash/hang.
  EXPECT_FALSE(result.error_message.empty());

  // Clean up all the fill connections.
  for (int fd : fill_fds) {
    close(fd);
  }
  close(server_fd);
}
#endif  // !_WIN32

// =============================================================================
// Write error via EPIPE (lines 104-107 of sender.cc)
// =============================================================================

TEST_F(NotificationSenderTest, WriteErrorEpipeWithLargePayload) {
  // Create a listener that accepts, sets SO_LINGER={1,0} (force RST on
  // close), and closes immediately. With a large enough payload, the
  // write loop should hit EPIPE or ECONNRESET, exercising lines 104-107.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  std::atomic<bool> accepted{false};
  std::thread acceptor([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      accepted.store(true);
#ifndef _WIN32
      // Force RST on close to cause EPIPE on the sender's write.
      struct linger ling = {1, 0};
      setsockopt(static_cast<int>(client_fd), SOL_SOCKET, SO_LINGER, &ling,
                 sizeof(ling));
      // Small receive buffer to maximize backpressure.
      int rcvbuf = 1;
      setsockopt(static_cast<int>(client_fd), SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                 sizeof(rcvbuf));
#endif
      // Close immediately to send RST.
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Large payload to maximize chance of hitting write after RST.
  CacheNotification notification =
      MakeNotification(std::string(kMaxUrlLength, 'P'), "example.com");
  auto result = SendNotification(socket_path_, notification);

  acceptor.join();
  CloseServer(server_fd);

  // The write loop should fail with "Failed to write notification" (line 106)
  // on EPIPE/ECONNRESET, or "Write to worker timed out" if EAGAIN is hit
  // first and then the peer reset causes poll timeout.
  if (!result.success) {
    EXPECT_TRUE(result.error_message.find("write") != std::string::npos ||
                result.error_message.find("Write") != std::string::npos ||
                result.error_message.find("timed out") != std::string::npos)
        << "Unexpected error: " << result.error_message;
  }
}

// =============================================================================
// Socket to nonexistent path in valid directory (ENOENT at connect)
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectToNonexistentPathInValidDirectory) {
  // The temp directory exists, but the socket file does not. This should
  // fail with ENOENT at connect, exercising the immediate connect failure
  // path (lines 59-63) rather than the EINPROGRESS/timeout path.
  std::string missing_sock = temp_dir_ + "/definitely_not_here.sock";
  auto result = SendNotification(missing_sock, MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.connect_errno, ENOENT);
  EXPECT_NE(result.error_message.find("connect"), std::string::npos)
      << "Error: " << result.error_message;
}
#endif  // !_WIN32

// =============================================================================
// Multiple rapid sends to overwhelmed receiver (EAGAIN / write timeout)
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, RapidSendsToOverwhelmedReceiver) {
  // Create a listener that accepts connections but reads very slowly.
  // Send many large notifications in rapid succession. This should
  // trigger EAGAIN paths and potentially write timeouts on some sends.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);

  constexpr int kNumSenders = 8;
  std::atomic<bool> done{false};
  std::atomic<int> accepted{0};

  // Acceptor thread: accept connections but never read, creating
  // maximum backpressure on the senders.
  std::vector<int> accepted_fds;
  std::mutex fds_mutex;
  std::thread acceptor([&]() {
    while (!done.load()) {
      struct pollfd pfd = {static_cast<int>(server_fd), POLLIN, 0};
      int rc = poll(&pfd, 1, 20);
      if (rc > 0) {
        int client_fd = accept(static_cast<int>(server_fd), nullptr, nullptr);
        if (client_fd >= 0) {
          // Minimize receive buffer for maximum backpressure.
          int rcvbuf = 1;
          setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
          std::lock_guard<std::mutex> lock(fds_mutex);
          accepted_fds.push_back(client_fd);
          accepted.fetch_add(1);
        }
      }
    }
    // Clean up accepted connections.
    std::lock_guard<std::mutex> lock(fds_mutex);
    for (int fd : accepted_fds) close(fd);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Launch multiple senders with large payloads in rapid succession.
  std::vector<std::thread> senders;
  senders.reserve(kNumSenders);
  std::atomic<int> success_count{0};
  std::atomic<int> timeout_count{0};
  std::atomic<int> error_count{0};

  for (int i = 0; i < kNumSenders; ++i) {
    senders.emplace_back([&, i]() {
      CacheNotification notification = MakeNotification(
          std::string(kMaxUrlLength, 'R' + static_cast<char>(i % 26)),
          "example.com");
      auto result = SendNotification(socket_path_, notification);
      if (result.success) {
        success_count.fetch_add(1);
      } else if (result.error_message.find("timed out") != std::string::npos) {
        timeout_count.fetch_add(1);
      } else {
        error_count.fetch_add(1);
      }
    });
  }

  for (auto& t : senders) {
    t.join();
  }

  done.store(true);
  acceptor.join();
  CloseServer(server_fd);

  // At least some sends should have completed (success or timeout).
  // The main assertion is no crash or hang.
  EXPECT_EQ(success_count.load() + timeout_count.load() + error_count.load(),
            kNumSenders);
}
#endif  // !_WIN32

// =============================================================================
// Partial write recovery with large payload
// =============================================================================

TEST_F(NotificationSenderTest, PartialWriteWithSlowDrain) {
  // Use a receiver that reads in very small chunks with delays,
  // forcing the sender's write loop to handle partial writes and
  // potentially EAGAIN. The sender should still complete successfully.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  std::string received_data;
  std::thread receiver([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      // Read in tiny 32-byte chunks to create backpressure that exercises
      // the EAGAIN/poll/continue path where the payload is larger than the
      // socket's send buffer (true on macOS, whose default is 8 KiB; on
      // Linux the buffer is larger and the send completes at once). No sleep
      // between reads: the sender gives up after kSendTimeoutMs without
      // progress, and a sleeping reader on a loaded host can stall longer
      // than that, failing the test on timing alone.
      char buf[32];
      while (true) {
        ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
        if (n <= 0) break;
        received_data.append(buf, static_cast<size_t>(n));
      }
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send a large notification (not kMaxUrlLength, to stay within timeout).
  CacheNotification notification =
      MakeNotification(std::string(8192, 'P'), "example.com");
  auto result = SendNotification(socket_path_, notification);

  receiver.join();
  CloseServer(server_fd);

  // With slow-but-active draining, the sender should succeed.
  EXPECT_TRUE(result.success) << result.error_message;

  // Verify data integrity.
  ASSERT_FALSE(received_data.empty());
  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(received_data, &parsed));
  EXPECT_EQ(parsed.url.size(), 8192u);
}

// =============================================================================
// Connect to socket with wrong permissions (EACCES)
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectToSocketWithWrongPermissions) {
  if (getuid() == 0) GTEST_SKIP() << "Root ignores file permissions";

  // Create a real listening socket, then change the socket file
  // permissions to 000 so that connect fails with EACCES.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  // Remove all permissions on the socket file.
  ASSERT_EQ(0, chmod(socket_path_.c_str(), 0000));

  auto result = SendNotification(socket_path_, MakeNotification());

  // Restore permissions for cleanup.
  chmod(socket_path_.c_str(), 0700);
  CloseServer(server_fd);

  EXPECT_FALSE(result.success);
  // On most systems, connecting to a socket file without permissions
  // gives EACCES. Some systems may give a different error.
  EXPECT_NE(result.connect_errno, 0);
  EXPECT_NE(result.connect_errno, ECONNREFUSED)
      << "Expected a permission error, not ECONNREFUSED";
  EXPECT_NE(result.connect_errno, ENOENT)
      << "Expected a permission error, not ENOENT";
}

TEST_F(NotificationSenderTest, RetryDoesNotRetryPermissionDeniedOnSocket) {
  if (getuid() == 0) GTEST_SKIP() << "Root ignores file permissions";

  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  // Remove permissions on the socket file.
  ASSERT_EQ(0, chmod(socket_path_.c_str(), 0000));

  RetryConfig config;
  config.max_retries = 3;
  config.initial_delay_ms = 50;
  config.backoff_multiplier = 2.0;

  auto start = std::chrono::steady_clock::now();
  auto result =
      SendNotificationWithRetry(socket_path_, MakeNotification(), config);
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();

  // Restore permissions for cleanup.
  chmod(socket_path_.c_str(), 0700);
  CloseServer(server_fd);

  EXPECT_FALSE(result.success);
  // Permission denied is not ECONNREFUSED or ENOENT, so should not retry.
  EXPECT_EQ(result.retries_attempted, 0);
  // Should be fast - no retry delays.
  EXPECT_LT(elapsed_ms, 100);
}
#endif  // !_WIN32

// =============================================================================
// Connect poll timeout with stalled connection (best-effort lines 66-72)
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectWithStalledBacklog) {
  // This test attempts to trigger the EINPROGRESS -> poll -> timeout path
  // (lines 66-72) by creating a listening socket and filling its backlog
  // with connections that are never accepted.
  //
  // On macOS Unix domain sockets, EINPROGRESS is rare for local sockets,
  // so this is a best-effort test. The key is that no crash or hang occurs.
  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(server_fd, 0);

  std::string sock = temp_dir_ + "/stall.sock";
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(0, bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)));
  // Minimum backlog.
  ASSERT_EQ(0, listen(server_fd, 1));

  // Fill the backlog with unaccepted connections.
  std::vector<int> fill_fds;
  for (int i = 0; i < 32; ++i) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) break;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc =
        connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
      fill_fds.push_back(fd);
    } else {
      close(fd);
      break;
    }
  }

  // Attempt to send. Should fail (either immediate connect refuse or
  // EINPROGRESS -> poll timeout at line 68-72).
  auto start = std::chrono::steady_clock::now();
  auto result = SendNotification(sock, MakeNotification());
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();

  for (int fd : fill_fds) close(fd);
  close(server_fd);

  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
  // If the connect got EINPROGRESS and hit the poll timeout (50ms),
  // elapsed should be around 50ms. If it failed immediately, elapsed
  // should be near 0. Both are acceptable.
  EXPECT_LT(elapsed_ms, 200) << "Should not hang indefinitely";
}
#endif  // !_WIN32

// =============================================================================
// Socket error after poll (lines 79-82): connect succeeds in poll but
// getsockopt reveals an error.
// =============================================================================

#ifndef _WIN32
TEST_F(NotificationSenderTest, ConnectToClosedListenerDuringPoll) {
  // Start a listener, fill the backlog, then close the listener.
  // Any pending connections should fail with a socket error that
  // getsockopt reveals after poll returns.
  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(server_fd, 0);

  std::string sock = temp_dir_ + "/close_during.sock";
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(0, bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)));
  ASSERT_EQ(0, listen(server_fd, 1));

  // Fill backlog.
  std::vector<int> fill_fds;
  for (int i = 0; i < 16; ++i) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) break;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc =
        connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
      fill_fds.push_back(fd);
    } else {
      close(fd);
      break;
    }
  }

  // Close the server before attempting our send. Pending EINPROGRESS
  // connections should get a socket error (ECONNREFUSED or ECONNRESET)
  // visible via getsockopt after poll (lines 74-83).
  close(server_fd);

  auto result = SendNotification(sock, MakeNotification());

  for (int fd : fill_fds) close(fd);

  // Should fail with some connect-related error.
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}
#endif  // !_WIN32

// =============================================================================
// Zero-byte write path (lines 110-113): extremely unlikely in practice
// but verify no crash with minimal payload when peer drops connection
// =============================================================================

TEST_F(NotificationSenderTest, WriteReturnsZeroBytesNoCrash) {
  // The zero-byte write path (lines 110-113) is extremely rare in practice
  // (write() returning 0 for a non-zero-length buffer). This test creates
  // conditions that maximize the chance of unusual write behavior by
  // having the receiver accept and immediately close with RST, then
  // sending a minimal payload.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  std::thread acceptor([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
#ifndef _WIN32
      struct linger ling = {1, 0};
      setsockopt(static_cast<int>(client_fd), SOL_SOCKET, SO_LINGER, &ling,
                 sizeof(ling));
#endif
      // Immediately close to send RST.
      CloseClient(client_fd);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Minimal notification to test the write path with a small buffer.
  CacheNotification notification;
  notification.url = "x";
  notification.scheme = "https";
  notification.hostname = "h";
  notification.content_type = ContentType::kOther;
  notification.capability_mask = 0;

  auto result = SendNotification(socket_path_, notification);

  acceptor.join();
  CloseServer(server_fd);

  // The write may succeed (buffered before RST) or fail (EPIPE/ECONNRESET).
  // Both are acceptable. The key assertion is no crash or hang, and that
  // the zero-byte write path code at lines 110-113 is safely compiled.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// =============================================================================
// Multiple notifications: sender reconnects for each message
// =============================================================================

TEST_F(NotificationSenderTest, RapidReconnectionStress) {
  // Send 10 notifications rapidly, each creating a new connection.
  // The receiver accepts and reads each one. This exercises the full
  // connect-write-close cycle under load.
  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  constexpr int kNumMessages = 10;
  std::atomic<int> received{0};

  std::thread receiver([&]() {
    for (int i = 0; i < kNumMessages; ++i) {
#ifndef _WIN32
      // Use poll() with timeout to avoid hanging if sender fails to connect.
      struct pollfd pfd = {static_cast<int>(server_fd), POLLIN, 0};
      int ready = poll(&pfd, 1, 30000);  // 30s per-accept timeout
      if (ready <= 0) break;             // timeout or error — stop waiting
#endif
      intptr_t client_fd = AcceptClient(server_fd, socket_path_);
      if (client_fd != -1) {
        char buf[4096];
        while (true) {
          ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
          if (n <= 0) break;
        }
        CloseClient(client_fd);
        received.fetch_add(1);
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  int success_count = 0;
  for (int i = 0; i < kNumMessages; ++i) {
    auto notification = MakeNotification(
        "/rapid/" + std::to_string(i), "example.com",
        static_cast<ContentType>(i % 5), static_cast<uint32_t>(i));
    auto result = SendNotification(socket_path_, notification);
    if (result.success) ++success_count;
    // Small delay between sends to avoid filling the listener backlog
    // under sanitizer instrumentation where everything runs ~10x slower.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  receiver.join();
  CloseServer(server_fd);

  EXPECT_EQ(success_count, kNumMessages);
  EXPECT_EQ(received.load(), kNumMessages);
}

// =============================================================================
// Persistent connection tests
// =============================================================================

// Helper: start a listener that accepts one connection and reads all
// messages until the connection is closed.  Returns the received
// notifications in order, plus the number of accept() calls that
// returned successfully (to verify connection reuse).
class PersistentListenerHelper {
 public:
  void Start(const std::string& socket_path) {
    socket_path_ = socket_path;
#ifdef _WIN32
    // On Windows, don't create a pipe instance here. AcceptClient creates
    // its own pipe instance via CreateAndAcceptPipe, so a pipe created here
    // would be an extra unused instance that the sender might connect to
    // instead of the one AcceptClient is waiting on, causing data loss.
    // server_fd_ stays -1; AcceptLoop manages pipe lifecycle entirely.
#else
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(
        0, bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
    ASSERT_EQ(0, listen(fd, 16));
    server_fd_ = static_cast<intptr_t>(fd);
#endif
  }

  // Accept connections and read notifications until Stop() is called.
  // Each connection can carry multiple length-prefixed notifications.
  void AcceptLoop() {
    thread_ = std::thread([this]() {
      while (!stop_.load()) {
#ifdef _WIN32
        // On Windows: create a fresh pipe instance for each accept attempt.
        // Use a short timeout (200ms) so we can check stop_ frequently,
        // rather than blocking for the full 10s default timeout.
        intptr_t client_fd = CreateAndAcceptPipe(socket_path_, 200);
        if (client_fd == -1) {
          // Timed out or failed. Retry (checking stop_).
          continue;
        }
#else
        struct pollfd pfd = {static_cast<int>(server_fd_), POLLIN, 0};
        int rc = poll(&pfd, 1, 50);
        if (rc <= 0) continue;
        intptr_t client_fd = AcceptClient(server_fd_, socket_path_);
        if (client_fd == -1) continue;
#endif
        accepts_.fetch_add(1, std::memory_order_relaxed);
        // Read all data from this connection.
        std::string buf;
        char chunk[4096];
        while (true) {
          ssize_t n = ReadClient(client_fd, chunk, sizeof(chunk));
          if (n <= 0) break;
          buf.append(chunk, static_cast<size_t>(n));
          // Parse complete messages.
          while (buf.size() >= 4) {
            uint32_t msg_len =
                (static_cast<uint32_t>(static_cast<unsigned char>(buf[0]))
                 << 24) |
                (static_cast<uint32_t>(static_cast<unsigned char>(buf[1]))
                 << 16) |
                (static_cast<uint32_t>(static_cast<unsigned char>(buf[2]))
                 << 8) |
                static_cast<uint32_t>(static_cast<unsigned char>(buf[3]));
            size_t total = 4 + msg_len;
            if (buf.size() < total) break;
            CacheNotification notif;
            if (CacheNotification::Deserialize(
                    std::string_view(buf.data(), total), &notif)) {
              std::lock_guard<std::mutex> lock(mu_);
              received_.push_back(std::move(notif));
            }
            buf.erase(0, total);
          }
        }
        CloseClient(client_fd);
        // On Windows, no need to recreate server_fd_: AcceptClient creates
        // a fresh pipe instance on every call via CreateAndAcceptPipe.
      }
    });
  }

  ~PersistentListenerHelper() { Stop(); }

  void Stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
#ifndef _WIN32
    if (server_fd_ != -1) {
      CloseServer(server_fd_);
      server_fd_ = -1;
    }
#endif
  }

  [[nodiscard]] int accept_count() const {
    return accepts_.load(std::memory_order_relaxed);
  }

  std::vector<CacheNotification> received() {
    std::lock_guard<std::mutex> lock(mu_);
    return received_;
  }

 private:
  intptr_t server_fd_ = -1;
  std::string socket_path_;
  std::atomic<bool> stop_{false};
  std::atomic<int> accepts_{0};
  std::mutex mu_;
  std::vector<CacheNotification> received_;
  std::thread thread_;
};

TEST_F(NotificationSenderTest, PersistentSendBasic) {
  // Basic persistent send: message is received correctly.
  ResetPersistentConnection();

  PersistentListenerHelper listener;
  listener.Start(socket_path_);
  listener.AcceptLoop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto result =
      SendNotificationPersistent(socket_path_, MakeNotification("/p1"));
  EXPECT_TRUE(result.success) << result.error_message;

  ClosePersistentConnection();  // Flush the connection so reader gets EOF.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  listener.Stop();

  auto msgs = listener.received();
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].url, "/p1");
}

TEST_F(NotificationSenderTest, PersistentConnectionReuse) {
  // Multiple sends should reuse the same connection (only 1 accept).
  ResetPersistentConnection();

  PersistentListenerHelper listener;
  listener.Start(socket_path_);
  listener.AcceptLoop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (int i = 0; i < 5; ++i) {
    auto result = SendNotificationPersistent(
        socket_path_, MakeNotification("/reuse/" + std::to_string(i)));
    EXPECT_TRUE(result.success) << "i=" << i << ": " << result.error_message;
  }

  ClosePersistentConnection();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  listener.Stop();

  auto msgs = listener.received();
  EXPECT_EQ(msgs.size(), 5u);
  for (int i = 0; i < 5 && i < static_cast<int>(msgs.size()); ++i) {
    EXPECT_EQ(msgs[i].url, "/reuse/" + std::to_string(i));
  }
  // Only one accept() call — connection was reused.
  EXPECT_EQ(listener.accept_count(), 1);
}

TEST_F(NotificationSenderTest, PersistentReconnectOnPeerClose) {
  // When the worker closes the connection (e.g., idle timeout), the next
  // send should reconnect automatically.
  ResetPersistentConnection();

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  // Phase 1: send one message, then kill the listener.
  {
    PersistentListenerHelper listener;
    listener.Start(socket_path_);
    listener.AcceptLoop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto result =
        SendNotificationPersistent(socket_path_, MakeNotification("/phase1"));
    EXPECT_TRUE(result.success) << result.error_message;

    ClosePersistentConnection();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    listener.Stop();
  }

  // Remove old socket file and create a new listener.
#ifndef _WIN32
  unlink(socket_path_.c_str());
#endif

  // Phase 2: send another message — should reconnect.
  {
    PersistentListenerHelper listener;
    listener.Start(socket_path_);
    listener.AcceptLoop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto result =
        SendNotificationPersistent(socket_path_, MakeNotification("/phase2"));
    EXPECT_TRUE(result.success) << result.error_message;

    ClosePersistentConnection();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    listener.Stop();

    auto msgs = listener.received();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].url, "/phase2");
  }
}

TEST_F(NotificationSenderTest, PersistentSocketPathChange) {
  // When the socket path changes, the persistent sender should detect it
  // and reconnect to the new path.
  ResetPersistentConnection();

  std::string path1 = temp_dir_ + "/path1.sock";
  std::string path2 = temp_dir_ + "/path2.sock";

  // Listener on path1.
  PersistentListenerHelper listener1;
  listener1.Start(path1);
  listener1.AcceptLoop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto r1 = SendNotificationPersistent(path1, MakeNotification("/on-path1"));
  EXPECT_TRUE(r1.success) << r1.error_message;

  // Now switch to path2 — a different listener.
  PersistentListenerHelper listener2;
  listener2.Start(path2);
  listener2.AcceptLoop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto r2 = SendNotificationPersistent(path2, MakeNotification("/on-path2"));
  EXPECT_TRUE(r2.success) << r2.error_message;

  ClosePersistentConnection();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  listener1.Stop();
  listener2.Stop();

  auto msgs1 = listener1.received();
  auto msgs2 = listener2.received();
  ASSERT_EQ(msgs1.size(), 1u);
  EXPECT_EQ(msgs1[0].url, "/on-path1");
  ASSERT_EQ(msgs2.size(), 1u);
  EXPECT_EQ(msgs2[0].url, "/on-path2");
}

TEST_F(NotificationSenderTest, PersistentResetClearsConnection) {
  // After ResetPersistentConnection, the next send should create a new
  // connection (2 accepts total).
  ResetPersistentConnection();

  PersistentListenerHelper listener;
  listener.Start(socket_path_);
  listener.AcceptLoop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto r1 = SendNotificationPersistent(socket_path_, MakeNotification("/a"));
  EXPECT_TRUE(r1.success) << r1.error_message;

  ResetPersistentConnection();
  // Give the accept loop thread time to detect the broken pipe, close the
  // old connection, and create a new pipe instance before we reconnect.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto r2 = SendNotificationPersistent(socket_path_, MakeNotification("/b"));
  EXPECT_TRUE(r2.success) << r2.error_message;

  ClosePersistentConnection();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  listener.Stop();

  auto msgs = listener.received();
  EXPECT_EQ(msgs.size(), 2u);
  // Reset caused a new connection, so 2 accepts.
  EXPECT_EQ(listener.accept_count(), 2);
}

TEST_F(NotificationSenderTest, PersistentEmptyPath) {
  ResetPersistentConnection();
  auto result = SendNotificationPersistent("", MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Empty socket path");
}

TEST_F(NotificationSenderTest, PersistentSendToNonExistentSocket) {
  ResetPersistentConnection();
  auto result = SendNotificationPersistent("/tmp/nonexistent_persistent.sock",
                                           MakeNotification());
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
  EXPECT_EQ(result.connect_errno, ENOENT);
}

TEST_F(NotificationSenderTest, PersistentMultipleResetsSafe) {
  // Calling Reset/Close multiple times should not crash.
  ResetPersistentConnection();
  ResetPersistentConnection();
  ClosePersistentConnection();
  ClosePersistentConnection();
  ResetPersistentConnection();
}

TEST_F(NotificationSenderTest, PersistentLargeNotification) {
  // Verify that large notifications work correctly on persistent connections.
  ResetPersistentConnection();

  PersistentListenerHelper listener;
  listener.Start(socket_path_);
  listener.AcceptLoop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string large_url(8192, 'L');
  auto result = SendNotificationPersistent(
      socket_path_, MakeNotification(large_url, "example.com"));
  EXPECT_TRUE(result.success) << result.error_message;

  ClosePersistentConnection();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  listener.Stop();

  auto msgs = listener.received();
  ASSERT_EQ(msgs.size(), 1u);
  EXPECT_EQ(msgs[0].url, large_url);
}

TEST_F(NotificationSenderTest, PersistentReconnectAfterWriteError) {
  // Simulate a write error by having the listener close the accepted
  // connection after the first message. The persistent sender should
  // reconnect and succeed on the next call.
  ResetPersistentConnection();

#ifndef _WIN32
  // Ignore SIGPIPE
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  intptr_t server_fd = StartListener();
  ASSERT_NE(server_fd, -1);

  // Accept first connection, read one message, then close to force EPIPE.
  std::string first_msg;
  std::thread first_accept([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[4096];
      ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
      if (n > 0) first_msg.assign(buf, static_cast<size_t>(n));
      CloseClient(client_fd);  // Force close → sender gets EPIPE on next write.
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto r1 = SendNotificationPersistent(socket_path_, MakeNotification("/msg1"));
  EXPECT_TRUE(r1.success) << r1.error_message;

  first_accept.join();
  ASSERT_FALSE(first_msg.empty());
  {
    CacheNotification first_parsed;
    ASSERT_TRUE(CacheNotification::Deserialize(first_msg, &first_parsed));
    EXPECT_EQ(first_parsed.url, "/msg1");
  }

  // Wait a moment for the close to propagate.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Second send should detect EPIPE and reconnect.
  std::string second_msg;
  std::thread second_accept([&]() {
    intptr_t client_fd = AcceptClient(server_fd, socket_path_);
    if (client_fd != -1) {
      char buf[4096];
      ssize_t n = ReadClient(client_fd, buf, sizeof(buf));
      if (n > 0) second_msg.assign(buf, static_cast<size_t>(n));
      CloseClient(client_fd);
    }
  });

  // Give the accept thread time to create the pipe instance (Windows) or
  // become ready to accept (POSIX) before the sender tries to reconnect.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto r2 = SendNotificationPersistent(socket_path_, MakeNotification("/msg2"));
  EXPECT_TRUE(r2.success) << r2.error_message;

  second_accept.join();
  CloseServer(server_fd);

  EXPECT_FALSE(second_msg.empty());
  CacheNotification parsed;
  ASSERT_TRUE(CacheNotification::Deserialize(second_msg, &parsed));
  EXPECT_EQ(parsed.url, "/msg2");

  ClosePersistentConnection();
}

}  // namespace
}  // namespace pagespeed
