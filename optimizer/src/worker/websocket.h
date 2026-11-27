// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - WebSocket Protocol Support (RFC 6455)
//
// Implements WebSocket handshake, frame parsing/construction, and
// connection lifecycle for the management API's real-time endpoints.
// Only text frames are used (no binary).

#ifndef PAGESPEED_SRC_WORKER_WEBSOCKET_H_
#define PAGESPEED_SRC_WORKER_WEBSOCKET_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {

// --- WebSocket Frame Types (RFC 6455 §5.2) ---

enum class WsOpcode : uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

// A parsed WebSocket frame.
struct WsFrame {
  bool fin = true;
  WsOpcode opcode = WsOpcode::kText;
  uint16_t close_code = 0;  // Only for close frames.
  std::string payload;
};

// Result of feeding bytes to the frame parser.
enum class WsParseResult : std::uint8_t {
  kNeedMore,  // Incomplete frame, need more data.
  kFrame,     // Complete frame parsed.
  kError,     // Protocol error.
};

// Incremental WebSocket frame parser.
// Feed raw bytes via Feed(); when a complete frame is ready,
// returns kFrame and populates the output frame.
class WsFrameParser {
 public:
  // Max payload size for a single frame (protection against OOM).
  static constexpr size_t kMaxPayloadSize = 1 * 1024 * 1024;  // 1MB

  // Feed data to the parser.  May consume part of `data`.
  // Returns kFrame when a complete frame is available via frame().
  // Call repeatedly with remaining data after each kFrame.
  WsParseResult Feed(std::string_view data, size_t& consumed);

  // The last parsed frame (valid after Feed returns kFrame).
  const WsFrame& frame() const { return frame_; }

  // Reset parser state for next frame.
  void Reset();

 private:
  enum class State : std::uint8_t {
    kHeader,    // Reading first 2 bytes.
    kExtLen16,  // Reading 2-byte extended length.
    kExtLen64,  // Reading 8-byte extended length.
    kMaskKey,   // Reading 4-byte mask key.
    kPayload,   // Reading payload bytes.
    kDone,      // Frame complete.
  };

  State state_ = State::kHeader;
  WsFrame frame_;
  uint8_t header_[2] = {};
  size_t header_read_ = 0;
  bool masked_ = false;
  uint8_t mask_key_[4] = {};
  size_t mask_read_ = 0;
  uint64_t payload_len_ = 0;
  uint8_t ext_len_buf_[8] = {};
  size_t ext_len_read_ = 0;
  size_t ext_len_needed_ = 0;
};

// --- Frame Construction ---

// Build a WebSocket text frame (server→client, unmasked).
std::string WsBuildTextFrame(std::string_view payload);

// Build a WebSocket close frame with status code.
std::string WsBuildCloseFrame(uint16_t code);

// Build a WebSocket ping frame.
std::string WsBuildPingFrame(std::string_view payload = "");

// Build a WebSocket pong frame.
std::string WsBuildPongFrame(std::string_view payload = "");

// --- Handshake ---

// Compute the Sec-WebSocket-Accept value from a Sec-WebSocket-Key.
// Returns the base64-encoded SHA-1 hash per RFC 6455 §4.2.2.
std::string WsComputeAcceptKey(std::string_view client_key);

// Build the HTTP 101 Switching Protocols response for a WebSocket
// upgrade request.
std::string WsBuildHandshakeResponse(std::string_view client_key);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_WEBSOCKET_H_
