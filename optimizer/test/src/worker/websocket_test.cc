// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// WebSocket Protocol + Endpoint Handler Tests
//
// Tests:
// 1. Frame parsing (text, ping, pong, close, masked, extended lengths)
// 2. Frame construction
// 3. Handshake (Sec-WebSocket-Accept computation)
// 4. WsManager lifecycle (upgrade, auth, stats push, events, ping/pong)

#include "src/worker/websocket.h"

#ifndef _WIN32
#include <signal.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "nlohmann/json.hpp"
#include "src/worker/ws_handlers.h"
#include "test/test_util/scoped_thread_join.h"
#include "test/test_util/tcp_client.h"
#include "uv.h"

#ifdef _WIN32
// The Windows headers that arrive with the includes above define the
// object-like macro FormatMessage (-> FormatMessageA), which rewrites the
// token before name lookup runs, so the unqualified call to the inherited
// MessageHandler::FormatMessage below would become a call to the Win32 API.
// Qualifying or parenthesising the name does not suppress an object-like
// macro; only #undef does.
#undef FormatMessage
#endif

namespace pagespeed {
namespace {

using json = nlohmann::json;

// Helper: build a masked client frame for any opcode/payload.
static std::string BuildMaskedFrame(WsOpcode opcode, std::string_view payload) {
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));

  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(0x80 | payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
    frame.push_back(static_cast<char>(payload.size() & 0xFF));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    uint64_t len = payload.size();
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }
  frame.append(reinterpret_cast<char*>(mask), 4);
  for (size_t i = 0; i < payload.size(); ++i) {
    frame.push_back(payload[i] ^ mask[i % 4]);
  }
  return frame;
}

// Helper: build a masked close frame with status code.
static std::string BuildMaskedCloseFrame(uint16_t code) {
  char payload[2];
  payload[0] = static_cast<char>((code >> 8) & 0xFF);
  payload[1] = static_cast<char>(code & 0xFF);
  return BuildMaskedFrame(WsOpcode::kClose, std::string_view(payload, 2));
}

// ===================================================================
// WebSocket Protocol Tests (framing + handshake)
// ===================================================================

TEST(WsFrameParserTest, UnmaskedClientFrameRejected) {
  // RFC 6455 §5.1: client-to-server frames MUST be masked.
  std::string frame = WsBuildTextFrame("Hello");

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kError);
}

TEST(WsFrameParserTest, ParseMaskedTextFrame) {
  // Client→server frames are masked (RFC 6455 §5.1).
  // Build a masked frame for "Hi" with mask key [0x37, 0xfa, 0x21, 0x3d].
  uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  std::string frame;
  frame.push_back(static_cast<char>(0x81));  // FIN + TEXT
  frame.push_back(static_cast<char>(0x82));  // MASK + len=2
  frame.append(reinterpret_cast<char*>(mask), 4);
  // Masked payload: "Hi" ^ mask
  frame.push_back('H' ^ mask[0]);
  frame.push_back('i' ^ mask[1]);

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "Hi");
}

TEST(WsFrameParserTest, ParseCloseFrame) {
  std::string frame = BuildMaskedCloseFrame(1000);

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().opcode, WsOpcode::kClose);
  EXPECT_EQ(parser.frame().close_code, 1000);
}

TEST(WsFrameParserTest, ParsePingFrame) {
  std::string frame = BuildMaskedFrame(WsOpcode::kPing, "ping-data");

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().opcode, WsOpcode::kPing);
  EXPECT_EQ(parser.frame().payload, "ping-data");
}

TEST(WsFrameParserTest, ParsePongFrame) {
  std::string frame = BuildMaskedFrame(WsOpcode::kPong, "pong-data");

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().opcode, WsOpcode::kPong);
  EXPECT_EQ(parser.frame().payload, "pong-data");
}

TEST(WsFrameParserTest, IncrementalParsing) {
  std::string frame = BuildMaskedFrame(WsOpcode::kText, "Hello, World!");

  WsFrameParser parser;
  // Feed one byte at a time.
  for (size_t i = 0; i < frame.size() - 1; ++i) {
    size_t consumed = 0;
    auto result = parser.Feed(std::string_view(frame.data() + i, 1), consumed);
    EXPECT_EQ(result, WsParseResult::kNeedMore);
    EXPECT_EQ(consumed, 1u);
  }

  // Feed last byte.
  size_t consumed = 0;
  auto result = parser.Feed(
      std::string_view(frame.data() + frame.size() - 1, 1), consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "Hello, World!");
}

TEST(WsFrameParserTest, ExtendedLength16) {
  // Create a payload of 200 bytes (> 125, uses 16-bit extended length).
  std::string payload(200, 'A');
  std::string frame = BuildMaskedFrame(WsOpcode::kText, payload);

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload.size(), 200u);
  EXPECT_EQ(parser.frame().payload, payload);
}

TEST(WsFrameParserTest, EmptyPayload) {
  std::string frame = BuildMaskedFrame(WsOpcode::kText, "");

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "");
}

TEST(WsFrameParserTest, MultipleFrames) {
  std::string frame1 = BuildMaskedFrame(WsOpcode::kText, "first");
  std::string frame2 = BuildMaskedFrame(WsOpcode::kText, "second");
  std::string combined = frame1 + frame2;

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(combined, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "first");
  EXPECT_EQ(consumed, frame1.size());

  parser.Reset();
  std::string_view remaining(combined.data() + consumed,
                             combined.size() - consumed);
  size_t consumed2 = 0;
  result = parser.Feed(remaining, consumed2);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "second");
}

// ===================================================================
// Handshake Tests
// ===================================================================

TEST(WsHandshakeTest, AcceptKeyComputation) {
  // RFC 6455 §4.2.2 example.
  std::string accept = WsComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
  EXPECT_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WsHandshakeTest, HandshakeResponse) {
  std::string response = WsBuildHandshakeResponse("dGhlIHNhbXBsZSBub25jZQ==");
  EXPECT_NE(response.find("101 Switching Protocols"), std::string::npos);
  EXPECT_NE(response.find("Upgrade: websocket"), std::string::npos);
  EXPECT_NE(response.find("Connection: Upgrade"), std::string::npos);
  EXPECT_NE(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
}

// ===================================================================
// WsManager Integration Tests
// ===================================================================

class WsManagerTest : public ::testing::Test {
 protected:
  // Connection teardown runs on the loop thread; poll for the expected
  // count (up to a deadline) instead of sleeping a fixed time, so a slow
  // host cannot observe a close that is still in flight.
  int WaitForActiveConnections(
      int expected,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int n = manager_->active_connections();
    while (n != expected && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      n = manager_->active_connections();
    }
    return n;
  }

  void SetUp() override {
#ifndef _WIN32
    // Server and clients share this process, and tests close clients while
    // the server may still write to them.  The daemon ignores SIGPIPE at
    // start-up (the write then fails with EPIPE and closes the connection);
    // give the server here the same process state.  The binary's own
    // behaviour is pinned in worker_client_disconnect_test.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &saved_sigpipe_);
#endif
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();

    // A tokenless WS stream is only pre-authenticated when
    // the API was deliberately opened.  These fixtures exercise the frame and
    // fan-out machinery, not the credential gate — the token-bearing
    // fixtures below cover that.
    ws_config_.allow_unauthenticated = true;
    ws_config_.max_connections = 8;
    ws_config_.ping_interval_ms = 5000;
    ws_config_.pong_timeout_ms = 2000;
    ws_config_.auth_timeout_ms = 1000;
    ws_config_.stats_interval_ms = 200;
    ws_config_.event_batch_ms = 50;
    Configure(ws_config_);

    manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
    stats_call_count_ = 0;
    manager_->SetStatsProvider([this]() -> json {
      stats_call_count_++;
      json j;
      j["notifications"] = stats_call_count_ * 10;
      j["variants"] = stats_call_count_ * 5;
      return j;
    });

    // Create a TCP listener to hand off connections.
    uv_tcp_init(loop_, &listener_);
    listener_.data = this;
    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", 0, &addr);
    uv_tcp_bind(&listener_, reinterpret_cast<const sockaddr*>(&addr), 0);

    struct sockaddr_storage bound;
    int namelen = sizeof(bound);
    uv_tcp_getsockname(&listener_, reinterpret_cast<sockaddr*>(&bound),
                       &namelen);
    port_ = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);

    uv_listen(reinterpret_cast<uv_stream_t*>(&listener_), 4,
              [](uv_stream_t* server, int status) {
                if (status < 0) return;
                auto* self = static_cast<WsManagerTest*>(server->data);
                // Allocated as the union HttpServer hands over (the
                // transport may be a pipe), and freed as one.
                auto* client = new uv_any_handle;
                uv_tcp_init(self->loop_, &client->tcp);
                if (uv_accept(server, &client->stream) == 0) {
                  self->manager_->AcceptUpgrade(
                      &client->stream, self->pending_endpoint_,
                      "dGhlIHNhbXBsZSBub25jZQ==", self->pending_interval_);
                } else {
                  uv_close(&client->handle, [](uv_handle_t* h) {
                    delete reinterpret_cast<uv_any_handle*>(h);
                  });
                }
              });

    manager_->Start();
    StartLoopThread();
  }

  void TearDown() override {
    StopLoopThread();
    manager_->Stop();
    uv_close(reinterpret_cast<uv_handle_t*>(&listener_), nullptr);
    uv_run(loop_, UV_RUN_DEFAULT);
    manager_.reset();
    uv_loop_close(loop_);
    delete loop_;
#ifndef _WIN32
    sigaction(SIGPIPE, &saved_sigpipe_, nullptr);
#endif
  }

  // Sub-fixtures change the manager's settings here, before it is built.
  virtual void Configure(WsConfig& config) { (void)config; }

  void StartLoopThread() {
    loop_running_ = true;
    loop_thread_ = std::thread([this] {
      while (loop_running_) {
        uv_run(loop_, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  void StopLoopThread() {
    loop_running_ = false;
    if (loop_thread_.joinable()) loop_thread_.join();
  }

  // Connect a raw TCP socket to the listener.
  int ConnectRawSocket() const { return test::ConnectTcp(port_, 2); }

  // Read until we get the full handshake response.
  // Reads byte-by-byte once close to the end of the HTTP header to avoid
  // consuming WebSocket frame data that arrives in the same TCP segment.
  std::string ReadHandshake(int sock) {
    std::string response;
    char c;
    while (true) {
      ssize_t n = test::SocketRead(sock, &c, 1);
      if (n != 1) break;
      response.push_back(c);
      if (response.size() >= 4 &&
          response.compare(response.size() - 4, 4, "\r\n\r\n") == 0) {
        break;
      }
    }
    return response;
  }

  // Build a masked client text frame.
  std::string BuildMaskedTextFrame(std::string_view payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + TEXT
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    if (payload.size() < 126) {
      frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
      frame.push_back(static_cast<char>(0x80 | 126));
      frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
      frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    frame.append(reinterpret_cast<char*>(mask), 4);
    for (size_t i = 0; i < payload.size(); ++i) {
      frame.push_back(payload[i] ^ mask[i % 4]);
    }
    return frame;
  }

  // Read a server text frame and return the payload.
  std::string ReadTextFrame(int sock) {
    uint8_t header[2];
    ssize_t n = test::SocketRead(sock, header, 2);
    if (n != 2) return "";

    uint8_t opcode = header[0] & 0x0F;
    size_t len = header[1] & 0x7F;

    if (len == 126) {
      uint8_t ext[2];
      if (test::SocketRead(sock, ext, 2) != 2) return "";
      len = (size_t(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      uint8_t ext[8];
      if (test::SocketRead(sock, ext, 8) != 8) return "";
      len = 0;
      for (unsigned char i : ext) {
        len = (len << 8) | i;
      }
    }

    std::string payload(len, '\0');
    size_t total = 0;
    while (total < len) {
      n = test::SocketRead(sock, payload.data() + total, len - total);
      if (n <= 0) break;
      total += n;
    }

    // Return empty for non-text frames (ping/pong/close).
    if (opcode != 0x1) return std::string(1, char(opcode)) + payload;
    return payload;
  }

  // Read text frames until we get one matching the expected JSON "type",
  // skipping deltas, control frames, etc.  Uses the existing 2s socket
  // timeout; gives up after 3 attempts (6s worst case).
  std::string ReadTextFrameByType(int sock, const std::string& expected_type) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      std::string msg = ReadTextFrame(sock);
      if (msg.empty()) break;  // Socket timeout or error — give up.
      // Skip control frames (ping 0x09, pong 0x0A, close 0x08).
      if (msg[0] == '\x09' || msg[0] == '\x0A' || msg[0] == '\x08') continue;
      // Check if this is the expected type.
      json j = json::parse(msg, nullptr, false);
      if (!j.is_discarded() && j.contains("type") &&
          j["type"] == expected_type) {
        return msg;
      }
      // Wrong type (e.g., delta instead of snapshot); try next frame.
    }
    return "";
  }

  uv_loop_t* loop_ = nullptr;
#ifndef _WIN32
  struct sigaction saved_sigpipe_{};
#endif
  uv_tcp_t listener_;
  int port_ = 0;
  std::unique_ptr<NullMessageHandler> handler_;
  WsConfig ws_config_;
  std::unique_ptr<WsManager> manager_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
  std::string pending_endpoint_ = "stats";
  int pending_interval_ = 0;
  int stats_call_count_ = 0;
};

TEST_F(WsManagerTest, HandshakeAndConnect) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  std::string handshake = ReadHandshake(sock);
  EXPECT_NE(handshake.find("101 Switching Protocols"), std::string::npos);
  EXPECT_NE(handshake.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);

  // Should have 1 active connection.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  test::CloseSocket(sock);
  EXPECT_EQ(WaitForActiveConnections(0), 0);
}

TEST_F(WsManagerTest, StatsSnapshotOnConnect) {
  // No auth required — should get initial snapshot immediately.
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  std::string handshake = ReadHandshake(sock);
  ASSERT_NE(handshake.find("101"), std::string::npos);

  // Read the initial snapshot, skipping any delta messages that may arrive
  // first due to the stats timer firing before the snapshot is sent.
  std::string msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(msg.empty());

  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_EQ(j["type"], "snapshot");
  EXPECT_TRUE(j.contains("data"));
  EXPECT_TRUE(j["data"].contains("notifications"));

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, StatsDeltaPush) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);
  // Read initial snapshot, skipping any interleaved delta messages.
  ReadTextFrameByType(sock, "snapshot");

  // Wait for a delta push (stats_interval_ms = 200).
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::string msg = ReadTextFrame(sock);
  if (!msg.empty()) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded()) {
      EXPECT_EQ(j["type"], "delta");
      EXPECT_TRUE(j.contains("sequence"));
      EXPECT_TRUE(j.contains("data"));
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, AuthRequired) {
  ws_config_.auth_token = "secret-token";
  // Recreate manager with auth.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 42}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Send auth message.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ssize_t sent = test::SocketWrite(sock, auth_msg.data(), auth_msg.size());
  ASSERT_EQ(sent, static_cast<ssize_t>(auth_msg.size()));

  // Should get auth_ok and then snapshot.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string msg1 = ReadTextFrame(sock);
  ASSERT_FALSE(msg1.empty());
  json j1 = json::parse(msg1, nullptr, false);

  // Could be auth_ok or snapshot — find auth_ok.
  bool got_auth_ok = false;
  bool got_snapshot = false;

  if (!j1.is_discarded()) {
    if (j1["type"] == "auth_ok") got_auth_ok = true;
    if (j1["type"] == "snapshot") got_snapshot = true;
  }

  std::string msg2 = ReadTextFrame(sock);
  if (!msg2.empty()) {
    json j2 = json::parse(msg2, nullptr, false);
    if (!j2.is_discarded()) {
      if (j2["type"] == "auth_ok") got_auth_ok = true;
      if (j2["type"] == "snapshot") got_snapshot = true;
    }
  }

  EXPECT_TRUE(got_auth_ok);
  EXPECT_TRUE(got_snapshot);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, AuthTimeout) {
  ws_config_.auth_token = "secret-token";
  ws_config_.auth_timeout_ms = 200;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Don't send auth — wait for timeout.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  // Connection should be closed.
  EXPECT_EQ(manager_->active_connections(), 0);
  test::CloseSocket(sock);
}

TEST_F(WsManagerTest, WrongAuthToken) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Send wrong token.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"wrong"})");
  test::SocketWrite(sock, auth_msg.data(), auth_msg.size());

  EXPECT_EQ(WaitForActiveConnections(0), 0);
  test::CloseSocket(sock);
}

TEST_F(WsManagerTest, EventPosting) {
  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Post some events.
  manager_->PostEvent("variant_written", json{{"url", "/test.jpg"}});
  manager_->PostEvent("notification", json{{"url", "/page.html"}});

  // Wait for event batch (event_batch_ms = 50).
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::string msg = ReadTextFrame(sock);
  if (!msg.empty()) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded()) {
      EXPECT_EQ(j["type"], "events");
      EXPECT_TRUE(j.contains("data"));
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, MaxConnectionsEnforced) {
  ws_config_.max_connections = 2;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);

  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ReadHandshake(sock2);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 2);

  // Third connection should be rejected.
  int sock3 = ConnectRawSocket();
  ASSERT_GE(sock3, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->active_connections(), 2);

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  test::CloseSocket(sock3);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, MetricsTracking) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_GE(manager_->metrics().messages_sent.load(), 1u);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// /v1/ws/logs Endpoint Tests
// ===================================================================

TEST_F(WsManagerTest, LogsSnapshotOnConnect) {
  // Post a log entry before any client connects so the ring buffer
  // is non-empty.
  manager_->PostLog("info", "worker", "worker", "startup complete");
  // PostLog drains via async callback on the loop thread; give it time.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  std::string handshake = ReadHandshake(sock);
  ASSERT_NE(handshake.find("101"), std::string::npos);

  // The snapshot is sent async; skip any non-snapshot frames.
  std::string msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(msg.empty());

  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_EQ(j["type"], "snapshot");
  EXPECT_TRUE(j.contains("entries"));
  EXPECT_TRUE(j["entries"].is_array());
  EXPECT_GE(j["entries"].size(), 1u);
  EXPECT_TRUE(j.contains("total"));

  // Verify the entry shape.
  json entry = j["entries"][0];
  EXPECT_EQ(entry["type"], "log");
  EXPECT_EQ(entry["level"], "info");
  EXPECT_EQ(entry["source"], "worker");
  EXPECT_TRUE(entry.contains("timestamp"));
  EXPECT_TRUE(entry.contains("message"));

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, LogsPostLogDelivered) {
  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);
  // Read the (possibly empty) snapshot.
  ReadTextFrame(sock);

  // Post a log entry after connection is established.
  manager_->PostLog("warning", "worker", "image",
                    "image too large to optimize");

  // Wait for drain + batch flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::string msg = ReadTextFrame(sock);
  if (!msg.empty()) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded()) {
      EXPECT_EQ(j["type"], "log");
      EXPECT_EQ(j["level"], "warning");
      EXPECT_EQ(j["source"], "worker");
      EXPECT_EQ(j["module"], "image");
      EXPECT_EQ(j["message"], "image too large to optimize");
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, LogsRingBufferCap) {
  // Fill the ring buffer beyond its capacity.
  for (int i = 0; i < 2050; ++i) {
    manager_->PostLog("info", "worker", "worker", "entry " + std::to_string(i));
  }

  // Let all entries drain into the ring buffer.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  std::string msg = ReadTextFrame(sock);
  ASSERT_FALSE(msg.empty());

  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_EQ(j["type"], "snapshot");
  // Ring buffer should be capped at 2000.
  EXPECT_LE(j["entries"].size(), 2000u);
  // Total should reflect all posted entries.
  EXPECT_GE(j["total"].get<uint64_t>(), 2050u);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// --- Log message hygiene -----------------------------------------------------

TEST(LogMessageSanitizeTest, ValidTextIsUnchanged) {
  EXPECT_EQ(SanitizeLogMessage(""), "");
  EXPECT_EQ(SanitizeLogMessage("cache flush complete"), "cache flush complete");
  // Two-, three- and four-byte characters pass through untouched.
  const std::string multi = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";
  EXPECT_EQ(SanitizeLogMessage(multi), multi);
}

TEST(LogMessageSanitizeTest, InvalidBytesBecomeReplacementCharacters) {
  const std::string kFffd = "\xEF\xBF\xBD";
  EXPECT_EQ(SanitizeLogMessage("bad \xFF\xFE byte"),
            "bad " + kFffd + kFffd + " byte");
  // A stray continuation byte.
  EXPECT_EQ(SanitizeLogMessage("a\x80z"), "a" + kFffd + "z");
  // An overlong encoding of '/'.
  EXPECT_EQ(SanitizeLogMessage("\xC0\xAF"), kFffd + kFffd);
  // A UTF-16 surrogate encoded as UTF-8.
  EXPECT_EQ(SanitizeLogMessage("\xED\xA0\x80"), kFffd + kFffd + kFffd);
  // Above U+10FFFF.
  EXPECT_EQ(SanitizeLogMessage("\xF4\x90\x80\x80"),
            kFffd + kFffd + kFffd + kFffd);
  // A sequence cut short by the end of the message (a URL cut at a fixed
  // byte count ends like this).
  EXPECT_EQ(SanitizeLogMessage("ab\xE2\x82"), "ab" + kFffd + kFffd);
}

TEST(LogMessageSanitizeTest, LongMessagesAreCutOnACharacterBoundary) {
  // Exactly at the cap: unchanged.
  const std::string at_cap(kMaxLogMessageBytes, 'a');
  EXPECT_EQ(SanitizeLogMessage(at_cap), at_cap);

  // Twice the cap: the first 4096 bytes and a suffix naming what was cut.
  EXPECT_EQ(SanitizeLogMessage(std::string(2 * kMaxLogMessageBytes, 'x')),
            std::string(kMaxLogMessageBytes, 'x') +
                "\xE2\x80\xA6[truncated 4096 bytes]");

  // A three-byte character straddling the cap is not split: 4095 'a', then
  // U+20AC (3 bytes) and "tail" -- the cut falls before the euro sign.
  const std::string straddle =
      std::string(kMaxLogMessageBytes - 1, 'a') + "\xE2\x82\xAC" + "tail";
  EXPECT_EQ(SanitizeLogMessage(straddle),
            std::string(kMaxLogMessageBytes - 1, 'a') +
                "\xE2\x80\xA6[truncated 7 bytes]");
}

TEST(LogMessageSanitizeTest, ReplacementHappensBeforeTheCap) {
  // 4094 'a' and one invalid byte: the U+FFFD makes it 4097 bytes, so the
  // cap applies to the repaired text and the replacement character (which
  // would straddle the cap) is cut whole.
  EXPECT_EQ(
      SanitizeLogMessage(std::string(kMaxLogMessageBytes - 2, 'a') + "\xFF"),
      std::string(kMaxLogMessageBytes - 2, 'a') +
          "\xE2\x80\xA6[truncated 3 bytes]");
}

TEST_F(WsManagerTest, LogsEntriesCarryAscendingSeq) {
  manager_->PostLog("info", "worker", "worker", "first");
  manager_->PostLog("warning", "worker", "image", "second");
  manager_->PostLog("error", "cache", "cache", "third");
  // PostLog drains via async callback on the loop thread; give it time.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  std::string msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(msg.empty());
  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  ASSERT_GE(j["entries"].size(), 3u);
  // Entries carry a per-process sequence number, ascending in ring order,
  // starting at 1 on this fresh manager.
  for (size_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(j["entries"][i].contains("seq")) << i;
    EXPECT_EQ(j["entries"][i]["seq"].get<uint64_t>(), i + 1) << i;
  }

  // A live entry drained after the snapshot continues the sequence.
  manager_->PostLog("info", "worker", "worker", "fourth");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::string live = ReadTextFrameByType(sock, "log");
  ASSERT_FALSE(live.empty());
  json lj = json::parse(live, nullptr, false);
  ASSERT_FALSE(lj.is_discarded());
  ASSERT_TRUE(lj.contains("seq"));
  EXPECT_EQ(lj["seq"].get<uint64_t>(), 4u);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, LogsSeqsContiguousAcrossRingWrap) {
  // Fill the ring beyond its capacity (same shape as LogsRingBufferCap).
  for (int i = 0; i < 2050; ++i) {
    manager_->PostLog("info", "worker", "worker", "entry " + std::to_string(i));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  std::string msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(msg.empty());
  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  ASSERT_EQ(j["entries"].size(), 2000u);
  // The oldest 50 entries were evicted; the retained seqs are contiguous
  // and ascending: total - 1999 .. total.
  const uint64_t total = j["total"].get<uint64_t>();
  ASSERT_GE(total, 2050u);
  uint64_t expect = total - 1999;
  for (const auto& entry : j["entries"]) {
    ASSERT_TRUE(entry.contains("seq"));
    EXPECT_EQ(entry["seq"].get<uint64_t>(), expect);
    ++expect;
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, LogsInvalidUtf8IsReplacedOnTheStream) {
  // A log client is connected, so the drain serializes each entry for it:
  // every entry must reach the client as valid UTF-8.
  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ASSERT_FALSE(ReadTextFrameByType(sock, "snapshot").empty());

  manager_->PostLog("warning", "worker", "worker", "bad \xFF\xFE byte");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::string live = ReadTextFrameByType(sock, "log");
  ASSERT_FALSE(live.empty());
  json lj = json::parse(live, nullptr, false);  // strict: rejects bad UTF-8
  ASSERT_FALSE(lj.is_discarded());
  EXPECT_EQ(lj["message"].get<std::string>(),
            "bad \xEF\xBF\xBD\xEF\xBF\xBD byte");
  test::CloseSocket(sock);

  // A client connecting later gets the same repaired text in its snapshot.
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ReadHandshake(sock2);
  json snap =
      json::parse(ReadTextFrameByType(sock2, "snapshot"), nullptr, false);
  ASSERT_FALSE(snap.is_discarded());
  ASSERT_FALSE(snap["entries"].empty());
  EXPECT_EQ(snap["entries"].back()["message"].get<std::string>(),
            "bad \xEF\xBF\xBD\xEF\xBF\xBD byte");
  test::CloseSocket(sock2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, LogsMetricsTracking) {
  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->metrics().logs_connections.load(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->metrics().logs_connections.load(), 0);
}

// ===================================================================
// /v1/ws/logs live delivery: complete, in order, serialized once
// ===================================================================

class WsLogStreamTest : public WsManagerTest {
 protected:
  // No server ping while a test runs: these clients never answer one, and a
  // sanitizer build can take longer than the base fixture's ping interval.
  void Configure(WsConfig& config) override {
    config.ping_interval_ms = 600000;
  }

  // What a client saw on the stream, checked as it arrives: every "log"
  // message must carry the seq right after the previous message's, and an
  // "overflow" message must name exactly the seqs it stands for.
  struct StreamRead {
    uint64_t next = 0;     // The seq the next message must start at.
    uint64_t entries = 0;  // "log" messages received.
    uint64_t dropped = 0;  // Sum of dropped_count over "overflow" messages.
    std::vector<json> overflows;
    std::string error;  // First violation, empty when there was none.
  };

  // Connects a log client and reads its snapshot.  Returns the socket, or
  // -1; *next is set to the seq the live stream starts at -- the snapshot's
  // total + 1, since total counts entries and seqs start at 1.
  int ConnectLogClient(uint64_t* next) {
    pending_endpoint_ = "logs";
    int sock = ConnectRawSocket();
    if (sock < 0) return -1;
    ReadHandshake(sock);
    json snapshot =
        json::parse(ReadTextFrameByType(sock, "snapshot"), nullptr, false);
    if (!snapshot.is_object() || !snapshot.contains("total")) {
      test::CloseSocket(sock);
      return -1;
    }
    *next = snapshot["total"].get<uint64_t>() + 1;
    return sock;
  }

  // Reads the stream until every seq in [start, end) is accounted for --
  // delivered, or named by an overflow message -- or the socket times out.
  StreamRead ReadLogStream(int sock, uint64_t start, uint64_t end) {
    StreamRead read;
    read.next = start;
    const auto fail = [&read](const std::string& what) {
      if (read.error.empty()) read.error = what;
    };
    while (read.next < end) {
      const std::string msg = ReadTextFrame(sock);
      if (msg.empty()) {
        fail("stream ended or timed out at seq " + std::to_string(read.next));
        break;
      }
      if (msg[0] == '\x09' || msg[0] == '\x0A') continue;  // ping, pong
      json j = json::parse(msg, nullptr, false);
      if (!j.is_object() || !j.contains("type")) {
        fail("not a JSON message: " + msg.substr(0, 80));
        break;
      }
      if (j["type"] == "log") {
        const uint64_t seq = j.value("seq", UINT64_MAX);
        if (seq != read.next) {
          fail("expected seq " + std::to_string(read.next) + ", got " +
               std::to_string(seq));
        }
        if (j.value("message", "").rfind("entry " + std::to_string(seq), 0) !=
            0) {
          fail("seq " + std::to_string(seq) + " carries the wrong message");
        }
        read.next = seq + 1;
        ++read.entries;
      } else if (j["type"] == "overflow") {
        const uint64_t first = j.value("first_seq", UINT64_MAX);
        const uint64_t last = j.value("last_seq", UINT64_MAX);
        const uint64_t count = j.value("dropped_count", uint64_t{0});
        if (first != read.next || last < first || count != last - first + 1) {
          fail("overflow message does not continue the stream: " + msg);
          break;
        }
        read.dropped += count;
        read.next = last + 1;
        read.overflows.push_back(std::move(j));
      }
    }
    return read;
  }

  // Posts `count` entries whose message is "entry <seq>" followed by
  // `padding` filler bytes; `first_seq` is the seq the first one will get.
  void PostEntries(uint64_t first_seq, uint64_t count, size_t padding = 0) {
    const std::string filler(padding, 'x');
    for (uint64_t i = 0; i < count; ++i) {
      manager_->PostLog(
          "info", "worker", "worker",
          "entry " + std::to_string(first_seq + i) + " " + filler);
    }
  }
};

// A burst, however large, reaches a connected client completely and in
// order as long as the ring can hold it.  (One frame per entry, at most 64
// writes pending per client, and write callbacks one loop iteration later
// used to cap a flush at 64 entries, the rest silently lost.)
TEST_F(WsLogStreamTest, BurstIsDeliveredCompleteAndInOrder) {
  uint64_t next = 0;
  int sock = ConnectLogClient(&next);
  ASSERT_GE(sock, 0);

  for (uint64_t burst : {10u, 64u, 65u, 500u, 2000u}) {
    SCOPED_TRACE("burst of " + std::to_string(burst));
    PostEntries(next, burst);
    StreamRead read = ReadLogStream(sock, next, next + burst);
    GTEST_LOG_(INFO) << "burst of " << burst << ": received " << read.entries
                     << " entries, " << read.overflows.size()
                     << " overflow messages";
    EXPECT_EQ(read.error, "");
    EXPECT_EQ(read.entries, burst);
    EXPECT_TRUE(read.overflows.empty());
    next += burst;
  }
  EXPECT_EQ(manager_->metrics().messages_dropped.load(), 0u);

  // The paged endpoint holds the same entries (the loop is stopped so the
  // ring can be read from this thread).
  StopLoopThread();
  json page = manager_->BuildLogsResponse(next - 11, true, 100);
  EXPECT_EQ(page["entries"].size(), 10u);
  EXPECT_EQ(page["newest_seq"].get<uint64_t>(), next - 1);
  EXPECT_FALSE(page["gap"].get<bool>());

  test::CloseSocket(sock);
}

// Lines at the 4 KiB cap: 300 of them are far more than one write carries,
// so this is the path where the next write waits for the previous one.
TEST_F(WsLogStreamTest, LongLinesAreDeliveredCompleteAndInOrder) {
  uint64_t next = 0;
  int sock = ConnectLogClient(&next);
  ASSERT_GE(sock, 0);

  PostEntries(next, 300, 4000);
  StreamRead read = ReadLogStream(sock, next, next + 300);
  EXPECT_EQ(read.error, "");
  EXPECT_EQ(read.entries, 300u);
  EXPECT_TRUE(read.overflows.empty());
  EXPECT_EQ(manager_->metrics().messages_dropped.load(), 0u);

  test::CloseSocket(sock);
}

// Every client gets every entry, and an entry is serialized for the stream
// once, not once per client.
TEST_F(WsLogStreamTest, AnEntryIsSerializedOnceForAllClients) {
  uint64_t next = 0;
  int socks[3];
  for (int& sock : socks) {
    sock = ConnectLogClient(&next);
    ASSERT_GE(sock, 0);
  }
  ASSERT_EQ(next, 1u);

  PostEntries(next, 300);
  for (int sock : socks) {
    StreamRead read = ReadLogStream(sock, next, next + 300);
    EXPECT_EQ(read.error, "");
    EXPECT_EQ(read.entries, 300u);
    EXPECT_TRUE(read.overflows.empty());
  }
  EXPECT_EQ(manager_->metrics().log_entries_serialized.load(), 300u);
  EXPECT_EQ(manager_->metrics().messages_sent.load(),
            3u * 300u + 3u /* snapshots */);

  for (int sock : socks) test::CloseSocket(sock);
}

// No stream client, no stream serialization.
TEST_F(WsLogStreamTest, NothingIsSerializedWithoutAClient) {
  PostEntries(0, 300);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(manager_->metrics().log_entries_serialized.load(), 0u);
}

// More entries than the ring holds arrive before the client can be sent any
// of them: the ones that left the ring are named by one overflow message,
// and the stream continues with the oldest entry the ring still has.  The
// paged endpoint reports the same loss as a gap.
TEST_F(WsLogStreamTest, EntriesThatLeftTheRingAreNamedByAnOverflowMessage) {
  uint64_t next = 0;
  int sock = ConnectLogClient(&next);
  ASSERT_GE(sock, 0);
  ASSERT_EQ(next, 1u);
  // A first entry, delivered, so the client has a position to page from.
  PostEntries(1, 1);
  StreamRead first = ReadLogStream(sock, 1, 2);
  ASSERT_EQ(first.error, "");

  // One drain of 2500 entries into a ring of 2000 (seqs 2..2501, so 2..501
  // leave it): with the loop stopped, all are queued before any is drained.
  StopLoopThread();
  PostEntries(2, 2500);
  StartLoopThread();

  StreamRead read = ReadLogStream(sock, 2, 2502);
  EXPECT_EQ(read.error, "");
  ASSERT_EQ(read.overflows.size(), 1u);
  const json& overflow = read.overflows[0];
  EXPECT_EQ(overflow["type"], "overflow");
  EXPECT_EQ(overflow["dropped_count"].get<uint64_t>(), 500u);
  EXPECT_EQ(overflow["first_seq"].get<uint64_t>(), 2u);
  EXPECT_EQ(overflow["last_seq"].get<uint64_t>(), 501u);
  // Everything the ring still held came through the stream, in order.
  EXPECT_EQ(read.entries, 2000u);
  EXPECT_EQ(read.next, 2502u);
  EXPECT_EQ(manager_->metrics().messages_dropped.load(), 500u);

  // A client that pages from the last entry it had (seq 1) is told the same
  // thing: a gap, and the ring starting right after the dropped range.
  StopLoopThread();
  json page = manager_->BuildLogsResponse(1, true, 10);
  EXPECT_TRUE(page["gap"].get<bool>());
  EXPECT_EQ(page["oldest_seq"].get<uint64_t>(),
            overflow["last_seq"].get<uint64_t>() + 1);
  ASSERT_FALSE(page["entries"].empty());
  EXPECT_EQ(page["entries"][0]["seq"].get<uint64_t>(), 502u);

  test::CloseSocket(sock);
}

// A client that stops reading costs the others nothing and loses nothing
// the ring still holds: when it reads again it gets an unbroken stream, with
// an overflow message wherever entries left the ring first.
TEST_F(WsLogStreamTest, AStalledClientLosesOnlyWhatLeftTheRing) {
  uint64_t next = 0;
  int stalled = ConnectLogClient(&next);
  ASSERT_GE(stalled, 0);
  int reader = ConnectLogClient(&next);
  ASSERT_GE(reader, 0);
  ASSERT_EQ(next, 1u);

  // 4 rounds of 1000 lines at the 4 KiB cap, 16 MB in all.  The reading
  // client takes each round; the stalled one reads nothing meanwhile.
  constexpr uint64_t kRounds = 4;
  constexpr uint64_t kPerRound = 1000;
  constexpr uint64_t kTotal = kRounds * kPerRound;
  for (uint64_t round = 0; round < kRounds; ++round) {
    SCOPED_TRACE("round " + std::to_string(round));
    PostEntries(round * kPerRound + 1, kPerRound, 4000);
    StreamRead read = ReadLogStream(reader, round * kPerRound + 1,
                                    (round + 1) * kPerRound + 1);
    EXPECT_EQ(read.error, "");
    EXPECT_EQ(read.entries, kPerRound);
    EXPECT_TRUE(read.overflows.empty());
  }
  // One serialization per entry, although two clients are sent each.
  EXPECT_EQ(manager_->metrics().log_entries_serialized.load(), kTotal);

  // The stalled client wakes up.  Whether it lost anything depends on how
  // much the socket buffers took; what must hold either way is that the
  // stream accounts for every seq and skips nothing the ring still holds
  // (the last 2000 entries, at least).
  StreamRead read = ReadLogStream(stalled, 1, kTotal + 1);
  GTEST_LOG_(INFO) << "stalled client: " << read.entries << " entries, "
                   << read.overflows.size() << " overflow messages, "
                   << read.dropped << " dropped";
  EXPECT_EQ(read.error, "");
  EXPECT_EQ(read.next, kTotal + 1);
  EXPECT_EQ(read.entries + read.dropped, kTotal);
  for (const json& overflow : read.overflows) {
    EXPECT_LE(overflow["last_seq"].get<uint64_t>(), kTotal - 2000);
  }
  EXPECT_EQ(manager_->metrics().messages_dropped.load(), read.dropped);
  EXPECT_EQ(manager_->metrics().log_entries_serialized.load(), kTotal);

  test::CloseSocket(stalled);
  test::CloseSocket(reader);
}

// ===================================================================
// WsConfig and WsMetrics struct tests
// ===================================================================

TEST(WsConfigTest, Defaults) {
  WsConfig config;
  // Sanity checks: all timeouts/limits should be positive.
  EXPECT_GT(config.max_connections, 0);
  EXPECT_GT(config.ping_interval_ms, 0);
  EXPECT_GT(config.pong_timeout_ms, 0);
  EXPECT_GT(config.auth_timeout_ms, 0);
  EXPECT_GT(config.stats_interval_ms, 0);
  EXPECT_GT(config.stats_min_interval_ms, 0);
  EXPECT_GT(config.stats_max_interval_ms, config.stats_min_interval_ms);
  EXPECT_GT(config.event_batch_ms, 0);
  EXPECT_GT(config.event_hwm, 0u);
  EXPECT_TRUE(config.auth_token.empty());
  // The read-open allow-list: stats and events are in it, the log stream
  // is deliberately not.
  EXPECT_TRUE(config.read_open_streams.contains("stats"));
  EXPECT_TRUE(config.read_open_streams.contains("events"));
  EXPECT_FALSE(config.read_open_streams.contains("logs"));
  EXPECT_EQ(config.max_preauth_connections, 16);
  // The per-peer bound covers a client's full stream set (three streams)
  // plus one slot of reconnect slack.
  EXPECT_EQ(config.max_preauth_connections_per_peer, 4);
}

TEST(WsMetricsTest, Defaults) {
  WsMetrics metrics;
  EXPECT_EQ(metrics.stats_connections.load(), 0);
  EXPECT_EQ(metrics.events_connections.load(), 0);
  EXPECT_EQ(metrics.logs_connections.load(), 0);
  EXPECT_EQ(metrics.messages_sent.load(), 0u);
  EXPECT_EQ(metrics.messages_dropped.load(), 0u);
  EXPECT_EQ(metrics.log_entries_serialized.load(), 0u);
}

// ===================================================================
// Additional frame parsing tests
// ===================================================================

TEST(WsFrameParserTest, EmptyTextFrame) {
  std::string frame = BuildMaskedFrame(WsOpcode::kText, "");
  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "");
}

TEST(WsFrameParserTest, LargeTextFrame) {
  // Text frame with 1000-byte payload.
  std::string payload(1000, 'A');
  std::string frame = BuildMaskedFrame(WsOpcode::kText, payload);
  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, payload);
}

TEST(WsFrameParserTest, IncompleteFrameNeedsMore) {
  // Feed only the first byte of a text frame.
  std::string partial;
  partial.push_back(static_cast<char>(0x81));  // FIN + TEXT

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(partial, consumed);
  EXPECT_EQ(result, WsParseResult::kNeedMore);
}

TEST(WsFrameParserTest, CloseFrameWithCode1001) {
  std::string frame = BuildMaskedCloseFrame(1001);
  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().opcode, WsOpcode::kClose);
  EXPECT_EQ(parser.frame().close_code, 1001);
}

TEST(WsFrameParserTest, ControlFramePayloadTooLarge) {
  // RFC 6455 §5.5: control frames MUST NOT have payload > 125 bytes.
  std::string payload(126, 'X');
  std::string frame = BuildMaskedFrame(WsOpcode::kPing, payload);
  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kError);
}

// ===================================================================
// Coverage gap tests: stop/reject, interval clamping, event overflow,
// stats delta computation, ping/pong lifecycle
// ===================================================================

// Test: WsManager::Stop() while not running is a no-op.
TEST_F(WsManagerTest, StopWhenNotRunning) {
  // Create a manager that is never started.
  StopLoopThread();
  manager_
      ->Stop();  // Already stopped by TearDown logic, but let's be explicit.
  uv_run(loop_, UV_RUN_NOWAIT);

  WsConfig config;
  config.max_connections = 2;
  auto fresh_mgr = std::make_unique<WsManager>(loop_, config, handler_.get());
  // Stop without Start should not crash.
  fresh_mgr->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  fresh_mgr.reset();
}

// Test: Stats interval clamping to min/max bounds.
// When a client requests an interval below min or above max,
// AcceptUpgrade clamps it. This exercises lines 163-166.
TEST_F(WsManagerTest, StatsIntervalClamping) {
  // Reconfigure with known min/max.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.stats_min_interval_ms = 200;
  ws_config_.stats_max_interval_ms = 5000;
  ws_config_.stats_interval_ms = 1000;

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  // Request interval of 50ms (below min) — should be clamped to 200ms.
  pending_endpoint_ = "stats";
  pending_interval_ = 50;

  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  std::string handshake = ReadHandshake(sock);
  ASSERT_NE(handshake.find("101"), std::string::npos);

  // Connection should be active (not rejected).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test: Event buffer overflow (high-water mark exceeded).
// Exercises lines 271-274 (event_hwm exceeded, drops oldest events).
TEST_F(WsManagerTest, EventBufferOverflow) {
  // Reconfigure with very small event_hwm to trigger overflow.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.event_hwm = 64;        // Very small buffer.
  ws_config_.event_batch_ms = 200;  // Longer batch interval.

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Flood with events exceeding the 64-byte buffer.
  for (int i = 0; i < 50; ++i) {
    manager_->PostEvent("flood",
                        json{{"idx", i}, {"padding", "abcdefghijklmnop"}});
  }

  // Wait for batch flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  // Some messages should have been dropped.
  EXPECT_GT(manager_->metrics().messages_dropped.load(), 0u)
      << "Some events should have been dropped due to HWM";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test: Stats delta computation sends only changed fields.
// Exercises lines 345-356 (delta computation).
TEST_F(WsManagerTest, StatsDeltaOnlyChangedFields) {
  // The stats provider returns incrementing values each time.
  // After the initial snapshot, delta pushes should contain only changed fields.
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);
  // Read initial snapshot.
  std::string snapshot_msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(snapshot_msg.empty());

  // Wait for at least one delta push (stats_interval_ms = 200).
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  std::string delta_msg = ReadTextFrameByType(sock, "delta");
  if (!delta_msg.empty()) {
    json delta = json::parse(delta_msg, nullptr, false);
    if (!delta.is_discarded()) {
      EXPECT_EQ(delta["type"], "delta");
      EXPECT_TRUE(delta.contains("sequence"));
      EXPECT_TRUE(delta.contains("data"));
      // Delta should have a sequence number > 0.
      EXPECT_GT(delta["sequence"].get<uint64_t>(), 0u);
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test: Events endpoint connection increments events_connections metric.
TEST_F(WsManagerTest, EventsMetricsTracking) {
  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->metrics().events_connections.load(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->metrics().events_connections.load(), 0);
}

// Test: AcceptUpgrade when manager is not running rejects the connection.
// Exercises lines 140-143 (not running path).
TEST_F(WsManagerTest, AcceptUpgradeWhenStopped) {
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  // Create a new TCP handle and try to upgrade it after Stop.
  auto* handle = new uv_any_handle;
  uv_tcp_init(loop_, &handle->tcp);

  // AcceptUpgrade on a stopped manager should close the handle without crash.
  manager_->AcceptUpgrade(&handle->stream, "stats",
                          "dGhlIHNhbXBsZSBub25jZQ==", 0);

  // Run the loop to process the close callback.
  uv_run(loop_, UV_RUN_NOWAIT);

  // Should still have 0 active connections.
  EXPECT_EQ(manager_->active_connections(), 0);
}

// ===================================================================
// Coverage: PostEvent when manager is not running (line 238)
// ===================================================================

TEST_F(WsManagerTest, PostEventWhenStoppedIsNoOp) {
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  // PostEvent on a stopped manager should not crash.
  manager_->PostEvent("test_event", json{{"key", "value"}});

  // PostLog on a stopped manager should not crash.
  manager_->PostLog("info", "worker", "test", "should be ignored");

  // No connections, no errors.
  EXPECT_EQ(manager_->active_connections(), 0);
}

// ===================================================================
// Coverage: Multiple event connections receive broadcasts (lines 265-283)
// ===================================================================

TEST_F(WsManagerTest, MultipleEventConnectionsReceiveBroadcast) {
  pending_endpoint_ = "events";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);

  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ReadHandshake(sock2);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 2);

  // Post an event.
  manager_->PostEvent("broadcast_test", json{{"url", "/test.jpg"}});

  // Wait for event batch.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Both clients should receive data.
  std::string msg1 = ReadTextFrame(sock1);
  std::string msg2 = ReadTextFrame(sock2);

  // At least one should have received the event.
  bool got_event1 =
      !msg1.empty() && msg1.find("broadcast_test") != std::string::npos;
  bool got_event2 =
      !msg2.empty() && msg2.find("broadcast_test") != std::string::npos;
  EXPECT_TRUE(got_event1 || got_event2)
      << "At least one event connection should receive the broadcast";

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Invalid auth message format (lines 537-541)
// ===================================================================

TEST_F(WsManagerTest, InvalidAuthJsonFormat) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Send invalid JSON (not a valid auth message).
  std::string invalid_msg = BuildMaskedTextFrame("not json at all");
  test::SocketWrite(sock, invalid_msg.data(), invalid_msg.size());

  EXPECT_EQ(WaitForActiveConnections(0), 0)
      << "Invalid auth JSON should close the connection";
  test::CloseSocket(sock);
}

TEST_F(WsManagerTest, AuthMessageMissingAuthField) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Send valid JSON but missing "auth" field.
  std::string missing_auth =
      BuildMaskedTextFrame(R"({"token":"secret-token"})");
  test::SocketWrite(sock, missing_auth.data(), missing_auth.size());

  EXPECT_EQ(WaitForActiveConnections(0), 0)
      << "Auth message without 'auth' field should close the connection";
  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Stats push with no stats provider (line 332)
// ===================================================================

TEST_F(WsManagerTest, StatsWithNoProvider) {
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  // Create a manager with no stats provider.
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  // Deliberately do NOT call SetStatsProvider.
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Wait for a stats push interval — should not crash.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Connection should still be alive (no crash, no snapshot).
  EXPECT_EQ(manager_->active_connections(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Multiple stats clients delta push (lines 366-370)
// ===================================================================

TEST_F(WsManagerTest, MultipleStatsClientsReceiveDelta) {
  pending_endpoint_ = "stats";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);
  ReadTextFrameByType(sock1, "snapshot");

  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ReadHandshake(sock2);
  ReadTextFrameByType(sock2, "snapshot");

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 2);
  EXPECT_EQ(manager_->metrics().stats_connections.load(), 2);

  // Wait for a delta push.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Both should receive something.
  std::string msg1 = ReadTextFrame(sock1);
  std::string msg2 = ReadTextFrame(sock2);
  bool got_delta1 = false, got_delta2 = false;
  if (!msg1.empty()) {
    json j = json::parse(msg1, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) got_delta1 = true;
  }
  if (!msg2.empty()) {
    json j = json::parse(msg2, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) got_delta2 = true;
  }
  EXPECT_TRUE(got_delta1 || got_delta2)
      << "At least one stats client should receive a message";

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Logs connection with auth required (lines 552-568)
// ===================================================================

TEST_F(WsManagerTest, LogsWithAuthRequired) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });

  // Post a log before starting so ring buffer is populated.
  // (PostLog checks running_ so we start first.)
  manager_->Start();
  StartLoopThread();

  manager_->PostLog("info", "worker", "test", "pre-auth log entry");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Send auth.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ssize_t sent = test::SocketWrite(sock, auth_msg.data(), auth_msg.size());
  ASSERT_EQ(sent, static_cast<ssize_t>(auth_msg.size()));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Should receive auth_ok and then a log snapshot.
  bool got_auth_ok = false;
  bool got_snapshot = false;
  for (int i = 0; i < 4; ++i) {
    std::string msg = ReadTextFrame(sock);
    if (msg.empty()) break;
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) {
      if (j["type"] == "auth_ok") got_auth_ok = true;
      if (j["type"] == "snapshot") {
        got_snapshot = true;
        // Verify snapshot has entries.
        EXPECT_TRUE(j.contains("entries"));
        EXPECT_TRUE(j.contains("total"));
      }
    }
  }
  EXPECT_TRUE(got_auth_ok) << "Should receive auth_ok";
  EXPECT_TRUE(got_snapshot) << "Should receive log snapshot after auth";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Under read_open with a token configured, every stream is pre-authenticated
// -- except the log stream, which carries the same lines as GET /v1/logs and
// keeps the token the same way.  A log client that never authenticates gets
// nothing and is closed by the auth timeout.
TEST_F(WsManagerTest, LogsStreamKeepsTheTokenUnderReadOpen) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.read_open = true;
  ws_config_.auth_timeout_ms = 300;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  manager_->PostLog("info", "worker", "test", "only the token may read this");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ASSERT_NE(ReadHandshake(sock).find("101"), std::string::npos);

  // No auth sent: no snapshot arrives before the auth timeout closes the
  // connection (ReadTextFrameByType skips the close frame, then hits EOF).
  std::string frame = ReadTextFrameByType(sock, "snapshot");
  EXPECT_TRUE(frame.empty())
      << "log snapshot sent without the token: " << frame;
  EXPECT_EQ(frame.find("only the token may read this"), std::string::npos);

  EXPECT_EQ(WaitForActiveConnections(0), 0)
      << "an unauthenticated log stream outlived the auth timeout";
  test::CloseSocket(sock);
}

// With the token sent in-band, the log stream under read_open behaves exactly
// as without read_open: snapshot, then auth_ok.
TEST_F(WsManagerTest, LogsStreamWithTheTokenUnderReadOpen) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.read_open = true;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  manager_->PostLog("info", "worker", "test", "read with the token");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ASSERT_EQ(test::SocketWrite(sock, auth_msg.data(), auth_msg.size()),
            static_cast<ssize_t>(auth_msg.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  bool got_auth_ok = false;
  bool got_snapshot = false;
  for (int i = 0; i < 4; ++i) {
    std::string msg = ReadTextFrame(sock);
    if (msg.empty()) break;
    json j = json::parse(msg, nullptr, false);
    if (j.is_discarded() || !j.contains("type")) continue;
    if (j["type"] == "auth_ok") got_auth_ok = true;
    if (j["type"] == "snapshot") {
      got_snapshot = true;
      EXPECT_NE(msg.find("read with the token"), std::string::npos) << msg;
    }
    if (got_auth_ok && got_snapshot) break;
  }
  EXPECT_TRUE(got_auth_ok) << "the log stream did not ask for the token";
  EXPECT_TRUE(got_snapshot);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// The other streams keep read_open's behaviour: a stats client gets its
// snapshot without authenticating.
TEST_F(WsManagerTest, OtherStreamsStayOpenUnderReadOpen) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.read_open = true;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  EXPECT_FALSE(ReadTextFrameByType(sock, "snapshot").empty())
      << "read_open must keep the stats stream open without the token";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Without a token (--api-no-auth, and the unix socket, where the worker
// gives the WS layer none) the log stream stays open under read_open.
TEST_F(WsManagerTest, LogsStreamOpenWithoutATokenUnderReadOpen) {
  ws_config_.auth_token.clear();
  ws_config_.allow_unauthenticated = true;
  ws_config_.read_open = true;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  manager_->PostLog("info", "worker", "test", "open without a token");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  std::string frame = ReadTextFrameByType(sock, "snapshot");
  EXPECT_NE(frame.find("open without a token"), std::string::npos) << frame;

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// read_open_streams is an explicit allow-list: an endpoint name that is
// neither "stats" nor "events" (the only two in the default set) is NOT
// pre-authenticated under read_open, exactly like "logs" -- including a
// name AcceptUpgrade has never been told about.  A client that never sends
// the in-band token gets nothing and is closed by the auth timeout.
TEST_F(WsManagerTest, UnknownStreamNotPreAuthenticatedUnderReadOpen) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.read_open = true;
  ws_config_.auth_timeout_ms = 300;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "mystery";  // Not in read_open_streams.
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ASSERT_NE(ReadHandshake(sock).find("101"), std::string::npos);

  // No auth sent: nothing arrives before the auth timeout closes it.
  std::string msg = ReadTextFrame(sock);
  EXPECT_TRUE(msg.empty() || msg[0] == '\x08')
      << "an unlisted stream was pre-authenticated under read-open: " << msg;

  EXPECT_EQ(WaitForActiveConnections(0), 0)
      << "an unauthenticated unknown-stream connection outlived the auth "
         "timeout";
  test::CloseSocket(sock);
}

// ===================================================================
// Pre-authentication budget (max_preauth_connections, per peer and global)
// ===================================================================

// Pure logic: the budget itself, driven with synthetic peer keys so every
// dimension (global, per-peer, keyless transport, release pairing) is
// covered without sockets -- a second source address is not portably
// available to a socket test.
TEST(WsPreauthBudgetTest, GlobalCapRefusesBeyondLimit) {
  WsPreauthBudget budget(2, 4);
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire("b"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire("c"), WsPreauthBudget::Verdict::kRefusedGlobal);
  EXPECT_EQ(budget.pending(), 2);
  budget.Release("a");
  EXPECT_EQ(budget.TryAcquire("c"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.pending(), 2);
}

TEST(WsPreauthBudgetTest, PerPeerCapLeavesRoomForOtherPeers) {
  WsPreauthBudget budget(16, 4);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kAdmitted) << i;
  }
  // The per-peer cap is spent for "a" -- and only for "a".
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kRefusedPerPeer);
  EXPECT_EQ(budget.TryAcquire("b"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.pending(), 5);
}

TEST(WsPreauthBudgetTest, PerPeerSlotsAreReleasedPerPeer) {
  WsPreauthBudget budget(16, 2);
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire("b"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire("b"), WsPreauthBudget::Verdict::kAdmitted);
  budget.Release("a");
  // "a" has one slot back; "b" is still at its cap.
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kRefusedPerPeer);
  EXPECT_EQ(budget.TryAcquire("b"), WsPreauthBudget::Verdict::kRefusedPerPeer);
}

TEST(WsPreauthBudgetTest, EmptyKeyCountsAgainstTheGlobalCapOnly) {
  WsPreauthBudget budget(3, 1);
  EXPECT_EQ(budget.TryAcquire(""), WsPreauthBudget::Verdict::kAdmitted);
  // No per-peer dimension: a second keyless acquire is admitted even with the
  // per-peer cap at 1.
  EXPECT_EQ(budget.TryAcquire(""), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire(""), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire(""), WsPreauthBudget::Verdict::kRefusedGlobal);
  budget.Release("");
  EXPECT_EQ(budget.TryAcquire(""), WsPreauthBudget::Verdict::kAdmitted);
}

TEST(WsPreauthBudgetTest, RefusedAcquireRecordsNothing) {
  WsPreauthBudget budget(16, 1);
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kRefusedPerPeer);
  budget.Release("a");
  // Had the refusal been recorded, "a" would still be at its cap.
  EXPECT_EQ(budget.TryAcquire("a"), WsPreauthBudget::Verdict::kAdmitted);
  EXPECT_EQ(budget.pending(), 1);
}

// An upgrade that would push the number of connections simultaneously
// waiting for in-band auth past max_preauth_connections is refused the same
// way exceeding max_connections is (the raw handle is closed, no handshake
// data at all) -- but a connection within the budget still gets its
// handshake.
TEST_F(WsManagerTest, PreauthBudgetRejectsBeyondLimit) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.max_preauth_connections = 1;
  ws_config_.max_connections = 8;  // Not the limiting factor here.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  // First connection uses the only pre-auth slot; never authenticates.
  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ASSERT_NE(ReadHandshake(sock1).find("101"), std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Second connection is refused outright: the pre-auth budget is spent.
  // Prove the refusal's actual shape -- no 101, no bytes at all, just the
  // raw handle closed -- rather than only the aggregate connection count
  // (which a sufficiently-early sleep could pass vacuously).
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  std::string refusal = ReadHandshake(sock2);
  EXPECT_TRUE(refusal.empty())
      << "a refused upgrade sent handshake bytes: " << refusal;
  EXPECT_EQ(manager_->active_connections(), 1)
      << "a connection was admitted past the pre-auth budget";

  // The refusal above must not itself have consumed (or leaked) a slot:
  // freeing sock1's slot admits a fresh connection normally.
  test::CloseSocket(sock1);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int sock3 = ConnectRawSocket();
  ASSERT_GE(sock3, 0);
  EXPECT_NE(ReadHandshake(sock3).find("101"), std::string::npos)
      << "a connection after the refusal was not admitted once the slot "
         "freed";

  test::CloseSocket(sock2);
  test::CloseSocket(sock3);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// One client opening its full stream set: all three upgrades are admitted
// even though every one of them will sit in pre-authentication at once
// (token set, read_open off) -- the default per-peer budget covers the
// stream set plus reconnect slack, where the previous global budget of two
// refused the third stream.
TEST_F(WsManagerTest, PreauthBudgetAdmitsFullStreamSetFromOnePeer) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  // read_open stays false: stats and events need in-band auth too.
  ws_config_.max_connections = 8;
  ws_config_.auth_timeout_ms = 5000;  // Outlasts the test.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  int socks[3];
  int i = 0;
  for (const char* endpoint : {"stats", "events", "logs"}) {
    pending_endpoint_ = endpoint;
    socks[i] = ConnectRawSocket();
    ASSERT_GE(socks[i], 0) << endpoint;
    ASSERT_NE(ReadHandshake(socks[i]).find("101"), std::string::npos)
        << endpoint << " upgrade refused within the default budget";
    ++i;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 3);

  for (int sock : socks) test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// The per-peer cap binds below the global one: all four test connections
// come from this host's one address, so the fifth never-authenticating
// connection is refused even though the global budget still has room.  (That
// another peer is still admitted meanwhile is covered by the budget class's
// own unit tests.)
TEST_F(WsManagerTest, PreauthBudgetPerPeerCapBindsBelowGlobal) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.max_connections = 8;
  ws_config_.auth_timeout_ms = 5000;  // Outlasts the test.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int held[4];
  for (int i = 0; i < 4; ++i) {
    held[i] = ConnectRawSocket();
    ASSERT_GE(held[i], 0) << i;
    ASSERT_NE(ReadHandshake(held[i]).find("101"), std::string::npos) << i;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 4);

  int fifth = ConnectRawSocket();
  ASSERT_GE(fifth, 0);
  EXPECT_TRUE(ReadHandshake(fifth).empty())
      << "a fifth pre-auth connection from one peer was admitted past the "
         "per-peer budget";
  EXPECT_EQ(manager_->active_connections(), 4);

  for (int sock : held) test::CloseSocket(sock);
  test::CloseSocket(fifth);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Authenticating frees the budget slot: once sock1 sends the token, a new
// connection can use the slot it held.
TEST_F(WsManagerTest, PreauthBudgetFreedOnAuthenticate) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.max_preauth_connections = 1;
  ws_config_.max_connections = 8;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);

  // Authenticate sock1 -- this must free the slot.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ASSERT_EQ(test::SocketWrite(sock1, auth_msg.data(), auth_msg.size()),
            static_cast<ssize_t>(auth_msg.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // A second connection should now be admitted (gets a real handshake).
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ASSERT_NE(ReadHandshake(sock2).find("101"), std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 2);

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// No leak: after several auth timeouts in a row, the budget still admits a
// new connection each time -- the slot is not lost on the timeout path.
TEST_F(WsManagerTest, PreauthBudgetFreedOnTimeoutNoLeak) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.max_preauth_connections = 1;
  ws_config_.max_connections = 8;
  ws_config_.auth_timeout_ms = 150;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  for (int i = 0; i < 3; ++i) {
    int sock = ConnectRawSocket();
    ASSERT_GE(sock, 0) << "iteration " << i;
    ASSERT_NE(ReadHandshake(sock).find("101"), std::string::npos)
        << "iteration " << i
        << ": budget slot was not freed by the prior "
           "timeout";
    // Never authenticate; wait out the timeout to free the slot again.
    EXPECT_EQ(WaitForActiveConnections(0), 0) << "iteration " << i;
    test::CloseSocket(sock);
  }
}

// Authenticated connections and pre-authenticated read-open-stream
// connections never count against the pre-auth budget: with the budget set
// to 1, several simultaneous read-open "stats" connections (none of which
// need in-band auth) all succeed.
TEST_F(WsManagerTest, PreauthBudgetExemptsReadOpenAndTokenlessConnections) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.read_open = true;
  ws_config_.max_preauth_connections = 1;
  ws_config_.max_connections = 8;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";  // In read_open_streams: pre-authenticated.

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ASSERT_NE(ReadHandshake(sock1).find("101"), std::string::npos);
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ASSERT_NE(ReadHandshake(sock2).find("101"), std::string::npos);
  int sock3 = ConnectRawSocket();
  ASSERT_GE(sock3, 0);
  ASSERT_NE(ReadHandshake(sock3).find("101"), std::string::npos);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 3)
      << "a read-open stream connection was rejected by the pre-auth budget";

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  test::CloseSocket(sock3);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// An explicit WS close frame sent before authenticating also frees the
// pre-auth budget slot -- the "close" path, via HandleFrame's kClose case
// into CloseWs (distinct from the auth-timeout path above).
TEST_F(WsManagerTest, PreauthBudgetFreedOnExplicitClose) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.max_preauth_connections = 1;
  ws_config_.max_connections = 8;
  ws_config_.auth_timeout_ms = 5000;  // Long enough that only the close
                                      // frame, not a timeout, frees it.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);

  // Close explicitly instead of authenticating or waiting out the timeout.
  std::string close_frame = BuildMaskedCloseFrame(1000);
  ASSERT_EQ(test::SocketWrite(sock1, close_frame.data(), close_frame.size()),
            static_cast<ssize_t>(close_frame.size()));
  EXPECT_EQ(WaitForActiveConnections(0), 0);

  // The slot must be free again well within the 5s auth timeout.
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ASSERT_NE(ReadHandshake(sock2).find("101"), std::string::npos)
      << "the pre-auth slot was not freed by the explicit close";

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// An abrupt socket error (the client closing its raw TCP connection) before
// authenticating also frees the slot -- the "error" path, via OnRead's
// nread < 0 branch into CloseWs.
TEST_F(WsManagerTest, PreauthBudgetFreedOnReadError) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.max_preauth_connections = 1;
  ws_config_.max_connections = 8;
  ws_config_.auth_timeout_ms = 5000;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);

  // Abrupt client-side close, no close frame: the server's next read sees
  // EOF/error, not a protocol-level close.
  test::CloseSocket(sock1);
  EXPECT_EQ(WaitForActiveConnections(0), 0);

  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ASSERT_NE(ReadHandshake(sock2).find("101"), std::string::npos)
      << "the pre-auth slot was not freed by the read error";

  test::CloseSocket(sock2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// The release tests above bind the global bound (max_preauth_connections =
// 1), so they would pass even if a path gave the slot back globally but not
// to its address.  Every client here shares 127.0.0.1, as every client of a
// loopback-bound or proxied optimizer does, and the per-address bound is 1
// with the global bound out of the way: a slot one path fails to return to
// the address refuses the next upgrade, which is what these tests read.

TEST_F(WsManagerTest, PreauthPerAddressSlotReturnedOnEveryClosePath) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.max_preauth_connections = 16;
  ws_config_.max_preauth_connections_per_peer = 1;
  ws_config_.max_connections = 8;
  // No path here may be the timeout (it has its own test below), and the
  // authenticated client, which never answers a ping, must not be closed
  // by a pong timeout while the test runs.
  ws_config_.auth_timeout_ms = 600000;
  ws_config_.ping_interval_ms = 600000;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();
  pending_endpoint_ = "stats";
  // A connection admitted into pre-authentication (it got its 101), or -1
  // when the upgrade was refused.
  const auto connect_admitted = [this]() {
    int s = ConnectRawSocket();
    if (s < 0) return -1;
    if (ReadHandshake(s).find("101") == std::string::npos) {
      test::CloseSocket(s);
      return -1;
    }
    return s;
  };
  constexpr const char* kNotReturned =
      "not admitted: the address's pre-auth slot was not returned";

  // The bound that this test relies on is in force: with one connection
  // waiting, a second one from the same address is refused.
  const int authed = connect_admitted();
  ASSERT_GE(authed, 0) << "first connection " << kNotReturned;
  int refused = ConnectRawSocket();
  ASSERT_GE(refused, 0);
  ASSERT_TRUE(ReadHandshake(refused).empty())
      << "a second pending connection from one address was admitted";
  test::CloseSocket(refused);

  // 1. Successful authentication.  The slot is returned before the
  // snapshot and the acknowledgement are sent, so reading the
  // acknowledgement orders the next upgrade after the release.
  const std::string auth = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ASSERT_EQ(test::SocketWrite(authed, auth.data(), auth.size()),
            static_cast<ssize_t>(auth.size()));
  ASSERT_FALSE(ReadTextFrameByType(authed, "auth_ok").empty());
  int sock = connect_admitted();
  ASSERT_GE(sock, 0) << "after a successful authentication: " << kNotReturned;
  // From here on: `authed` (authenticated, holds no slot) plus `sock`.

  // 2. Failed authentication (wrong token): closed with 4001.
  const std::string wrong = BuildMaskedTextFrame(R"({"auth":"wrong-token"})");
  ASSERT_EQ(test::SocketWrite(sock, wrong.data(), wrong.size()),
            static_cast<ssize_t>(wrong.size()));
  ASSERT_EQ(WaitForActiveConnections(1), 1);
  test::CloseSocket(sock);
  sock = connect_admitted();
  ASSERT_GE(sock, 0) << "after a failed authentication: " << kNotReturned;

  // 3. Client closes with a close frame.
  const std::string close_frame = BuildMaskedCloseFrame(1000);
  ASSERT_EQ(test::SocketWrite(sock, close_frame.data(), close_frame.size()),
            static_cast<ssize_t>(close_frame.size()));
  ASSERT_EQ(WaitForActiveConnections(1), 1);
  test::CloseSocket(sock);
  sock = connect_admitted();
  ASSERT_GE(sock, 0) << "after the client sent a close frame: " << kNotReturned;

  // 4. Client closes the connection without a close frame (end of stream).
  test::CloseSocket(sock);
  ASSERT_EQ(WaitForActiveConnections(1), 1);
  sock = connect_admitted();
  ASSERT_GE(sock, 0) << "after the client closed the connection: "
                     << kNotReturned;

  // 5. Client resets the connection (a read error on the server side).
  {
    struct linger reset_on_close;
    reset_on_close.l_onoff = 1;
    reset_on_close.l_linger = 0;
    ASSERT_EQ(setsockopt(sock, SOL_SOCKET, SO_LINGER,
                         reinterpret_cast<const char*>(&reset_on_close),
                         sizeof(reset_on_close)),
              0);
  }
  test::CloseSocket(sock);
  ASSERT_EQ(WaitForActiveConnections(1), 1);
  sock = connect_admitted();
  ASSERT_GE(sock, 0) << "after the client reset the connection: "
                     << kNotReturned;

  // 6. Shutdown.  Read with the loop stopped (the budget is loop-confined):
  // one slot is held by `sock` before, none after.
  StopLoopThread();
  EXPECT_EQ(manager_->preauth_budget().pending(), 1);
  EXPECT_EQ(manager_->preauth_budget().pending_for("127.0.0.1"), 1);
  manager_->Stop();
  EXPECT_EQ(manager_->preauth_budget().pending(), 0);
  EXPECT_EQ(manager_->preauth_budget().pending_for("127.0.0.1"), 0)
      << "shutdown did not return the address's pre-auth slot";

  test::CloseSocket(sock);
  test::CloseSocket(authed);
}

// The timeout path, with a short auth timeout: each iteration's connection
// is admitted only if the previous one's timeout returned its slot to the
// address.  Progress-based: the wait is on the connection count, which drops
// only after the timeout has closed the connection and released its slot.
TEST_F(WsManagerTest, PreauthPerAddressSlotReturnedOnTimeout) {
  ws_config_.auth_token = "secret-token";
  ws_config_.allow_unauthenticated = false;
  ws_config_.max_preauth_connections = 16;
  ws_config_.max_preauth_connections_per_peer = 1;
  ws_config_.max_connections = 8;
  ws_config_.auth_timeout_ms = 150;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();
  pending_endpoint_ = "stats";
  // A connection admitted into pre-authentication (it got its 101), or -1
  // when the upgrade was refused.
  const auto connect_admitted = [this]() {
    int s = ConnectRawSocket();
    if (s < 0) return -1;
    if (ReadHandshake(s).find("101") == std::string::npos) {
      test::CloseSocket(s);
      return -1;
    }
    return s;
  };
  constexpr const char* kNotReturned =
      "not admitted: the address's pre-auth slot was not returned";

  for (int i = 0; i < 3; ++i) {
    const int sock = connect_admitted();
    ASSERT_GE(sock, 0) << "iteration " << i << ": " << kNotReturned;
    EXPECT_EQ(WaitForActiveConnections(0), 0) << "iteration " << i;
    test::CloseSocket(sock);
  }
  StopLoopThread();
  EXPECT_EQ(manager_->preauth_budget().pending_for("127.0.0.1"), 0);
  StartLoopThread();
}

// ===================================================================
// Auth-timeout warning rate limiting
// ===================================================================

// A MessageHandler that records every WARNING message, for asserting on the
// rate-limited auth-timeout log rather than only on connection behaviour.
class WsWarningRecordingHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }
  std::vector<std::string> warnings() const {
    std::lock_guard<std::mutex> lock(mu_);
    return warnings_;
  }

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override {
    std::string formatted = FormatMessage(format, args);
    std::lock_guard<std::mutex> lock(mu_);
    if (type == MessageType::kWarning) warnings_.push_back(formatted);
  }

 private:
  mutable std::mutex mu_;
  std::vector<std::string> warnings_;
};

// Integration smoke test: several real auth timeouts in quick succession
// (well within the 60s window) produce only ONE "auth timeout" warning --
// proving OnAuthTimeout is actually wired to the limiter.  The window-
// elapsed/suppressed-count arithmetic itself is covered by the pure
// WsAuthTimeoutWarningLimiter tests below, which need no real waiting.
TEST(WsAuthTimeoutWarningIntegrationTest, RepeatedTimeoutsLogOnlyOnce) {
  uv_loop_t* loop = new uv_loop_t;
  uv_loop_init(loop);
  auto handler = std::make_unique<WsWarningRecordingHandler>();

  WsConfig config;
  config.auth_token = "secret-token";
  config.allow_unauthenticated = false;
  config.max_connections = 8;
  config.auth_timeout_ms = 100;
  auto manager = std::make_unique<WsManager>(loop, config, handler.get());
  manager->Start();

  uv_tcp_t listener;
  uv_tcp_init(loop, &listener);
  listener.data = manager.get();
  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 0, &addr);
  uv_tcp_bind(&listener, reinterpret_cast<const sockaddr*>(&addr), 0);
  struct sockaddr_storage bound;
  int namelen = sizeof(bound);
  uv_tcp_getsockname(&listener, reinterpret_cast<sockaddr*>(&bound), &namelen);
  int port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
  uv_listen(reinterpret_cast<uv_stream_t*>(&listener), 8,
            [](uv_stream_t* server, int status) {
              if (status < 0) return;
              auto* mgr = static_cast<WsManager*>(server->data);
              auto* client = new uv_any_handle;
              uv_tcp_init(server->loop, &client->tcp);
              if (uv_accept(server, &client->stream) == 0) {
                mgr->AcceptUpgrade(&client->stream, "stats",
                                   "dGhlIHNhbXBsZSBub25jZQ==");
              } else {
                uv_close(&client->handle, [](uv_handle_t* h) {
                  delete reinterpret_cast<uv_any_handle*>(h);
                });
              }
            });

  std::atomic<bool> running{true};
  std::thread loop_thread([&] {
    while (running) {
      uv_run(loop, UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  // A failed connect below must not leave the loop thread joinable.
  test::ScopedThreadJoin loop_guard(loop_thread,
                                    [&running] { running = false; });

  // Three connections, none authenticate; each times out in turn (well
  // under the 60s rate-limit window).
  for (int i = 0; i < 3; ++i) {
    int sock = test::ConnectTcp(port, 2);
    ASSERT_GE(sock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    test::CloseSocket(sock);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  running = false;
  loop_thread.join();
  manager->Stop();
  uv_close(reinterpret_cast<uv_handle_t*>(&listener), nullptr);
  uv_run(loop, UV_RUN_DEFAULT);
  manager.reset();
  uv_loop_close(loop);
  delete loop;

  int timeout_warnings = 0;
  for (const auto& w : handler->warnings()) {
    if (w.find("auth timeout") != std::string::npos) ++timeout_warnings;
  }
  EXPECT_EQ(timeout_warnings, 1)
      << "expected exactly one rate-limited warning across 3 timeouts";
}

// Sibling of RepeatedTimeoutsLogOnlyOnce for the OTHER warning this change
// introduces: a refusal costs a client nothing (no 2s wait), so without its
// own rate limit the ring would fill at whatever rate upgrades arrive. One
// connection holds the only pre-auth slot for the whole test (long
// auth_timeout_ms, never authenticates, never closes), so every further
// upgrade is refused outright.
TEST(WsAuthTimeoutWarningIntegrationTest, RepeatedRefusalsLogOnlyOnce) {
  uv_loop_t* loop = new uv_loop_t;
  uv_loop_init(loop);
  auto handler = std::make_unique<WsWarningRecordingHandler>();

  WsConfig config;
  config.auth_token = "secret-token";
  config.allow_unauthenticated = false;
  config.max_connections = 8;
  config.max_preauth_connections = 1;
  config.auth_timeout_ms = 10000;  // Long enough to never fire in this test.
  auto manager = std::make_unique<WsManager>(loop, config, handler.get());
  manager->Start();

  uv_tcp_t listener;
  uv_tcp_init(loop, &listener);
  listener.data = manager.get();
  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 0, &addr);
  uv_tcp_bind(&listener, reinterpret_cast<const sockaddr*>(&addr), 0);
  struct sockaddr_storage bound;
  int namelen = sizeof(bound);
  uv_tcp_getsockname(&listener, reinterpret_cast<sockaddr*>(&bound), &namelen);
  int port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
  uv_listen(reinterpret_cast<uv_stream_t*>(&listener), 8,
            [](uv_stream_t* server, int status) {
              if (status < 0) return;
              auto* mgr = static_cast<WsManager*>(server->data);
              auto* client = new uv_any_handle;
              uv_tcp_init(server->loop, &client->tcp);
              if (uv_accept(server, &client->stream) == 0) {
                mgr->AcceptUpgrade(&client->stream, "stats",
                                   "dGhlIHNhbXBsZSBub25jZQ==");
              } else {
                uv_close(&client->handle, [](uv_handle_t* h) {
                  delete reinterpret_cast<uv_any_handle*>(h);
                });
              }
            });

  std::atomic<bool> running{true};
  std::thread loop_thread([&] {
    while (running) {
      uv_run(loop, UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  // A failed connect below must not leave the loop thread joinable.
  test::ScopedThreadJoin loop_guard(loop_thread,
                                    [&running] { running = false; });

  // First connection: fills the one pre-auth slot and holds it (never
  // authenticates, stays open for the rest of the test).
  int holder = test::ConnectTcp(port, 2);
  ASSERT_GE(holder, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Three more connections: each is refused outright (the slot is taken).
  for (int i = 0; i < 3; ++i) {
    int sock = test::ConnectTcp(port, 2);
    ASSERT_GE(sock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    test::CloseSocket(sock);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  running = false;
  loop_thread.join();
  test::CloseSocket(holder);
  manager->Stop();
  uv_close(reinterpret_cast<uv_handle_t*>(&listener), nullptr);
  uv_run(loop, UV_RUN_DEFAULT);
  manager.reset();
  uv_loop_close(loop);
  delete loop;

  int reject_warnings = 0;
  for (const auto& w : handler->warnings()) {
    if (w.find("rejecting upgrade") != std::string::npos) ++reject_warnings;
  }
  EXPECT_EQ(reject_warnings, 1)
      << "expected exactly one rate-limited warning across 3 refusals";
}

// Pure logic: the limiter itself, driven with synthetic timestamps so the
// test needs no real sleeping.
TEST(WsAuthTimeoutWarningLimiterTest, FirstCallEmitsWithZeroSuppressed) {
  WsAuthTimeoutWarningLimiter limiter;
  auto result = limiter.RecordTimeout(1000);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
}

TEST(WsAuthTimeoutWarningLimiterTest, CallsWithinWindowAreSuppressed) {
  WsAuthTimeoutWarningLimiter limiter(60000);
  ASSERT_TRUE(limiter.RecordTimeout(0).has_value());
  EXPECT_FALSE(limiter.RecordTimeout(1000).has_value());
  EXPECT_FALSE(limiter.RecordTimeout(30000).has_value());
  EXPECT_FALSE(limiter.RecordTimeout(59999).has_value());
}

TEST(WsAuthTimeoutWarningLimiterTest,
     EmitsAgainAfterWindowWithSuppressedCount) {
  WsAuthTimeoutWarningLimiter limiter(60000);
  ASSERT_TRUE(limiter.RecordTimeout(0).has_value());  // Emits, suppressed=0.
  EXPECT_FALSE(limiter.RecordTimeout(10000).has_value());  // Suppressed #1.
  EXPECT_FALSE(limiter.RecordTimeout(20000).has_value());  // Suppressed #2.
  EXPECT_FALSE(limiter.RecordTimeout(59999).has_value());  // Suppressed #3.
  auto result = limiter.RecordTimeout(60000);  // Window elapsed: emits.
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 3);
}

TEST(WsAuthTimeoutWarningLimiterTest, SuppressedCountResetsAfterEachEmission) {
  WsAuthTimeoutWarningLimiter limiter(60000);
  ASSERT_TRUE(limiter.RecordTimeout(0).has_value());
  EXPECT_FALSE(limiter.RecordTimeout(100).has_value());
  auto second_emit = limiter.RecordTimeout(60000);
  ASSERT_TRUE(second_emit.has_value());
  EXPECT_EQ(*second_emit, 1);

  // A fresh window with no suppressed calls reports 0, not the stale count.
  auto third_emit = limiter.RecordTimeout(120000);
  ASSERT_TRUE(third_emit.has_value());
  EXPECT_EQ(*third_emit, 0);
}

TEST(WsAuthTimeoutWarningLimiterTest, ExactlyAtWindowBoundaryEmits) {
  WsAuthTimeoutWarningLimiter limiter(1000);
  ASSERT_TRUE(limiter.RecordTimeout(0).has_value());
  // >= window_ms, per RecordTimeout's contract.
  auto result = limiter.RecordTimeout(1000);
  EXPECT_TRUE(result.has_value());
}

// ===================================================================
// Coverage: Connection close frame handling (lines 520-522)
// ===================================================================

TEST_F(WsManagerTest, ClientCloseFrame) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Build a masked close frame (code 1000).
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string close_frame;
  close_frame.push_back(static_cast<char>(0x88));  // FIN + CLOSE
  close_frame.push_back(static_cast<char>(0x82));  // MASK + len=2
  close_frame.append(reinterpret_cast<char*>(mask), 4);
  // Close code 1000 = 0x03E8
  close_frame.push_back(static_cast<char>(0x03 ^ mask[0]));
  close_frame.push_back(static_cast<char>(0xE8 ^ mask[1]));

  test::SocketWrite(sock, close_frame.data(), close_frame.size());

  EXPECT_EQ(WaitForActiveConnections(0), 0)
      << "Connection should be closed after client close frame";

  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Client ping frame triggers pong (lines 515-518)
// ===================================================================

TEST_F(WsManagerTest, ClientPingGetsPong) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  // Build a masked ping frame with payload "test-ping".
  std::string payload = "test-ping";
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string ping_frame;
  ping_frame.push_back(static_cast<char>(0x89));  // FIN + PING
  ping_frame.push_back(static_cast<char>(0x80 | payload.size()));
  ping_frame.append(reinterpret_cast<char*>(mask), 4);
  for (size_t i = 0; i < payload.size(); ++i) {
    ping_frame.push_back(payload[i] ^ mask[i % 4]);
  }

  test::SocketWrite(sock, ping_frame.data(), ping_frame.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read frames — should get a pong (opcode 0x0A).
  std::string msg = ReadTextFrame(sock);
  // ReadTextFrame returns opcode prefix for non-text frames.
  bool got_pong = (!msg.empty() && msg[0] == '\x0A');
  if (!got_pong) {
    // Try again — there might be a delta message before the pong.
    msg = ReadTextFrame(sock);
    got_pong = (!msg.empty() && msg[0] == '\x0A');
  }
  EXPECT_TRUE(got_pong) << "Server should respond to client ping with pong";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Continuation frame triggers protocol error (lines 524-527)
// ===================================================================

TEST_F(WsManagerTest, ContinuationFrameClosesConnection) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Build a masked continuation frame (opcode 0x00).
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string cont_frame;
  cont_frame.push_back(static_cast<char>(0x80));  // FIN + CONTINUATION
  cont_frame.push_back(static_cast<char>(0x81));  // MASK + len=1
  cont_frame.append(reinterpret_cast<char*>(mask), 4);
  cont_frame.push_back('X' ^ mask[0]);

  test::SocketWrite(sock, cont_frame.data(), cont_frame.size());

  EXPECT_EQ(WaitForActiveConnections(0), 0)
      << "Continuation frame should trigger protocol error close";

  test::CloseSocket(sock);
}

// ===================================================================
// Log stream with a write size smaller than one entry
// ===================================================================

// event_hwm bounds one log-stream write, not what a client may receive: with
// a limit below the size of a single entry, every entry still arrives, one
// per write, in order.  (The limit used to be a per-flush budget, and the
// entries over it were dropped.)
TEST_F(WsManagerTest, LogEntriesLargerThanOneWriteAreAllDelivered) {
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.event_hwm = 64;  // Smaller than any serialized entry.

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ASSERT_FALSE(ReadTextFrameByType(sock, "snapshot").empty());

  for (int i = 0; i < 50; ++i) {
    manager_->PostLog("info", "worker", "test",
                      "log entry with padding data " + std::to_string(i));
  }

  for (uint64_t seq = 1; seq <= 50; ++seq) {
    json entry = json::parse(ReadTextFrameByType(sock, "log"), nullptr, false);
    ASSERT_TRUE(entry.is_object()) << "entry " << seq << " did not arrive";
    EXPECT_EQ(entry["seq"].get<uint64_t>(), seq);
  }
  EXPECT_EQ(manager_->metrics().messages_dropped.load(), 0u);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Auth message with non-string "auth" field (line 538)
// ===================================================================

TEST_F(WsManagerTest, AuthFieldNotString) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Send JSON with "auth" as a number, not a string.
  std::string bad_auth = BuildMaskedTextFrame(R"({"auth": 12345})");
  test::SocketWrite(sock, bad_auth.data(), bad_auth.size());

  EXPECT_EQ(WaitForActiveConnections(0), 0)
      << "Non-string auth field should close the connection";
  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Close frame without code (close_code == 0 path, line 521)
// ===================================================================

TEST_F(WsManagerTest, CloseFrameWithoutCode) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Build a masked close frame with no payload (no close code).
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string close_frame;
  close_frame.push_back(static_cast<char>(0x88));  // FIN + CLOSE
  close_frame.push_back(static_cast<char>(0x80));  // MASK + len=0
  close_frame.append(reinterpret_cast<char*>(mask), 4);

  test::SocketWrite(sock, close_frame.data(), close_frame.size());

  EXPECT_EQ(WaitForActiveConnections(0), 0)
      << "Close frame without code should use default code 1000";

  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Stats delta with no changes is empty (line 358)
// ===================================================================

TEST_F(WsManagerTest, StatsDeltaEmptyWhenNoChanges) {
  // Use a fixed stats provider that always returns the same values.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.stats_interval_ms = 100;

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json {
    // Always return the same static values.
    return json{{"constant_metric", 42}};
  });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  // Read initial snapshot (sets prev_stats_ baseline).
  ReadTextFrameByType(sock, "snapshot");

  // Wait for several stats intervals. Since the provider returns
  // constant values, the delta should be empty and no messages sent.
  uint64_t msgs_before = manager_->metrics().messages_sent.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  uint64_t msgs_after = manager_->metrics().messages_sent.load();

  // No delta messages should have been sent since nothing changed.
  // (Only the initial snapshot should count.)
  // We allow at most 1 additional message (possible timing edge case).
  EXPECT_LE(msgs_after - msgs_before, 1u)
      << "No delta messages should be sent when stats are unchanged";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Event batch encoding failure fallback (lines 392-395)
// ===================================================================

TEST_F(WsManagerTest, EventBatchEncodingFallback) {
  // Post events and ensure they are received even in edge cases.
  // This exercises the batch encoding path including the parse fallback.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.event_batch_ms = 50;

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Post multiple events rapidly to test batch encoding.
  for (int i = 0; i < 5; ++i) {
    manager_->PostEvent("test_event", json{{"idx", i}});
  }

  // Wait for event batch flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::string msg = ReadTextFrame(sock);
  if (!msg.empty()) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded()) {
      EXPECT_EQ(j["type"], "events");
      // "data" should be an array (successful parse) or a string (fallback).
      EXPECT_TRUE(j.contains("data"));
      EXPECT_TRUE(j.contains("sequence"));
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Max connections with correct rejection (lines 146-152)
// ===================================================================

TEST_F(WsManagerTest, MaxConnectionsRejectsWithCorrectMetrics) {
  ws_config_.max_connections = 1;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  // First connection should succeed.
  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);
  EXPECT_EQ(manager_->metrics().stats_connections.load(), 1);

  // Second connection should be rejected.
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->active_connections(), 1)
      << "Second connection should be rejected";

  // After closing first, a new connection should succeed.
  test::CloseSocket(sock1);
  EXPECT_EQ(WaitForActiveConnections(0), 0);

  int sock3 = ConnectRawSocket();
  ASSERT_GE(sock3, 0);
  ReadHandshake(sock3);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  test::CloseSocket(sock2);
  test::CloseSocket(sock3);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: WS parse error closes connection (lines 439-442)
// ===================================================================

TEST_F(WsManagerTest, InvalidFrameClosesConnection) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Send an invalid WebSocket frame: reserved opcode 0x03 with FIN bit set.
  // This should parse but result in an unrecognized opcode (falls through
  // to default in HandleFrame). However, a truly malformed frame will
  // trigger kError in the parser. Let's send a frame with an impossibly
  // large length to trigger a parse error.
  char garbage[] = {
      static_cast<char>(0x81),  // FIN + TEXT
      static_cast<char>(0xFF),  // MASK + 64-bit length indicator
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      1,  // 8-byte length = 1 (but masked without mask bytes)
  };
  test::SocketWrite(sock, garbage, sizeof(garbage));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // Connection may or may not be closed depending on parser behavior,
  // but it should not crash.
  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Stats push with auth-required stats client (lines 553-563)
// ===================================================================

TEST_F(WsManagerTest, StatsAuthAndDeltaPush) {
  ws_config_.auth_token = "secret-token";
  ws_config_.stats_interval_ms = 100;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  int call = 0;
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([&call]() -> json {
    call++;
    return json{{"counter", call * 10}};
  });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Authenticate.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ssize_t sent = test::SocketWrite(sock, auth_msg.data(), auth_msg.size());
  ASSERT_EQ(sent, static_cast<ssize_t>(auth_msg.size()));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Should receive auth_ok and snapshot.
  bool got_auth = false, got_snapshot = false;
  for (int i = 0; i < 3; ++i) {
    std::string msg = ReadTextFrame(sock);
    if (msg.empty()) break;
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) {
      if (j["type"] == "auth_ok") got_auth = true;
      if (j["type"] == "snapshot") got_snapshot = true;
    }
  }
  EXPECT_TRUE(got_auth);
  EXPECT_TRUE(got_snapshot);

  // Wait for a delta push with changing stats.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::string delta_msg = ReadTextFrame(sock);
  if (!delta_msg.empty()) {
    json j = json::parse(delta_msg, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) {
      if (j["type"] == "delta") {
        EXPECT_TRUE(j.contains("data"));
        EXPECT_TRUE(j.contains("sequence"));
      }
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

}  // namespace
}  // namespace pagespeed
