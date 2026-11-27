// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CDP Client over Pipe Transport
//
// Chrome DevTools Protocol JSON-RPC client using stdin/stdout pipe
// transport (FD 3/4). Runs on the main libuv event loop (H5).
//
// Key design decisions from expert review:
// - B2: SendCommand() includes session_id parameter
// - H5: Chrome pipes on main libuv event loop (not separate loop)
// - M2: Per-command uv_timer_t timeout
// - M3: CancelAll() on pipe EOF resolves all pending with error
// - M10: Large JSON parsed off event loop via uv_queue_work()
// - 10MB max message size guard
// See docs/implementation-plan-headless-browser.md, Expert Review appendix.

#ifndef PAGESPEED_SRC_BROWSER_CDP_CLIENT_H_
#define PAGESPEED_SRC_BROWSER_CDP_CLIENT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/browser/cdp_types.h"
#include "uv.h"

namespace pagespeed {

// CDP client that communicates with Chrome over pipe transport.
//
// Usage:
//   CdpClient client(loop);
//   client.SetEventCallback([](const CdpEvent& e) { ... });
//   client.AttachPipes(read_fd, write_fd);
//   client.SendCommand({"Page.enable"}, [](auto result) { ... });
class CdpClient {
 public:
  // Maximum CDP message size (10MB guard).
  static constexpr size_t kMaxMessageSize = 10 * 1024 * 1024;

  explicit CdpClient(uv_loop_t* loop);
  ~CdpClient();

  CdpClient(const CdpClient&) = delete;
  CdpClient& operator=(const CdpClient&) = delete;

  // Attach to Chrome's pipe FDs (typically FD 3 for read, FD 4 for write).
  // Returns error if pipe initialization fails.
  absl::Status AttachPipes(uv_pipe_t* read_pipe, uv_pipe_t* write_pipe);

  // Send a CDP command and receive the response via callback.
  // Returns the command ID assigned, or error if pipes not attached.
  absl::StatusOr<int> SendCommand(const CdpCommand& command,
                                  CdpResponseCallback callback);

  // Set callback for CDP events (method notifications without id).
  void SetEventCallback(CdpEventCallback callback);

  // Cancel all pending commands with an error status.
  // Called on pipe EOF or Chrome crash (M3).
  void CancelAll(absl::Status reason);

  // Returns true if the pipe transport is connected.
  bool connected() const { return connected_; }

  // Number of pending (in-flight) commands.
  size_t pending_count() const { return pending_commands_.size(); }

  // The event loop this client runs on.
  uv_loop_t* loop() const { return loop_; }

  // Where the renders report problems that do not end them
  // (a refused Fetch.failRequest). The worker sets this to its message
  // handler; unset, reports are dropped.
  void set_diagnostic_handler(std::function<void(std::string_view)> handler) {
    diagnostic_handler_ = std::move(handler);
  }
  void Diagnose(std::string_view message) const {
    if (diagnostic_handler_) diagnostic_handler_(message);
  }

 private:
  // Per-command tracking: callback + timeout timer.
  struct PendingCommand {
    CdpResponseCallback callback;
    uv_timer_t* timer = nullptr;  // Per-command timeout (M2)
    uint32_t id = 0;
  };

  // libuv callbacks for reading from Chrome's pipe.
  static void OnAlloc(uv_handle_t* handle, size_t suggested_size,
                      uv_buf_t* buf);
  static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
  static void OnWriteDone(uv_write_t* req, int status);
  static void OnCommandTimeout(uv_timer_t* timer);

  // Process a complete CDP message (null-byte delimited).
  void ProcessMessage(const std::string& message);

  // Process parsed JSON (may be called from thread pool for large messages).
  void HandleParsedMessage(nlohmann::json msg);

  uv_loop_t* loop_;
  uv_pipe_t* read_pipe_ = nullptr;
  uv_pipe_t* write_pipe_ = nullptr;
  bool connected_ = false;

  // Read buffer for null-byte delimited framing.
  std::string read_buffer_;

  // Reusable allocation buffer for uv_read (avoids malloc per read).
  std::vector<char> alloc_buffer_;

  // Command ID counter (monotonically increasing, unsigned to avoid UB).
  uint32_t next_id_ = 1;

  // Pending commands awaiting responses, keyed by command ID.
  std::unordered_map<uint32_t, PendingCommand> pending_commands_;

  // Event callback for CDP notifications.
  CdpEventCallback event_callback_;

  // See set_diagnostic_handler().
  std::function<void(std::string_view)> diagnostic_handler_;

  // Alive flag for safe access from deferred callbacks (M10, M4).
  std::shared_ptr<bool> alive_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_CDP_CLIENT_H_
