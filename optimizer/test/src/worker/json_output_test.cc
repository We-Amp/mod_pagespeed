// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// JSON Output Hardening Tests
//
// Covers the one safe JSON serialization choke point (DumpJson(),
// src/worker/json_dump.h) and the last-resort nets around it:
//
//   - DumpJson() unit tests: well-formed output for a string nlohmann
//     cannot losslessly encode as strict JSON, byte-identical output for
//     valid input.
//   - Integration tests: the management API and the WebSocket streams
//     answer well-formed JSON for any stored URL or posted message, and
//     keep serving afterward.
//   - Net test: a route handler that fails answers 500 with a fixed body,
//     and the server keeps serving.
//   - Gate: no bare nlohmann::json::dump( call remains under src/worker/
//     outside the helper's own implementation.

#ifndef _WIN32
#include <signal.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "nlohmann/json.hpp"
#include "src/worker/cache_handlers.h"
#include "src/worker/http_server.h"
#include "src/worker/json_dump.h"
#include "src/worker/url_registry.h"
#include "src/worker/ws_handlers.h"
#include "test/test_util/tcp_client.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

// =============================================================================
// DumpJson() unit tests
// =============================================================================

TEST(DumpJsonTest, ReplacesLoneInvalidByte) {
  json j;
  j["s"] = std::string(
      "lone\xFF"
      "byte");

  std::string out;
  ASSERT_NO_THROW(out = DumpJson(j));

  json parsed = json::parse(out, nullptr, false);
  ASSERT_FALSE(parsed.is_discarded());
  EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(DumpJsonTest, ReplacesTruncatedMultiByteSequence) {
  // 0xE2 0x82 is the first two bytes of a 3-byte sequence (e.g. the Euro
  // sign, 0xE2 0x82 0xAC), cut short.
  json j;
  j["s"] = std::string(
      "trunc\xE2\x82"
      "end");

  std::string out;
  ASSERT_NO_THROW(out = DumpJson(j));

  json parsed = json::parse(out, nullptr, false);
  ASSERT_FALSE(parsed.is_discarded());
  EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(DumpJsonTest, ReplacesOverlongEncoding) {
  // 0xC0 0xAF is an overlong 2-byte encoding of '/' (U+002F) -- not
  // permitted in UTF-8, even though the bit pattern resolves to a valid
  // code point.
  json j;
  j["s"] = std::string(
      "overlong\xC0\xAF"
      "end");

  std::string out;
  ASSERT_NO_THROW(out = DumpJson(j));

  json parsed = json::parse(out, nullptr, false);
  ASSERT_FALSE(parsed.is_discarded());
  EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(DumpJsonTest, ReplacesSurrogateEncoding) {
  // 0xED 0xA0 0x80 is the UTF-8 encoding of U+D800, a UTF-16 surrogate
  // half -- never a valid standalone UTF-8 code point.
  json j;
  j["s"] = std::string(
      "surrogate\xED\xA0\x80"
      "end");

  std::string out;
  ASSERT_NO_THROW(out = DumpJson(j));

  json parsed = json::parse(out, nullptr, false);
  ASSERT_FALSE(parsed.is_discarded());
  EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(DumpJsonTest, ByteIdenticalToDumpForValidInput) {
  json j;
  j["a"] = "hello world";
  j["b"] = 42;
  j["c"] = true;
  j["nested"] = json{{"x", 1}, {"y", json::array({1, 2, 3})}};

  EXPECT_EQ(DumpJson(j), j.dump());
}

TEST(DumpJsonTest, ByteIdenticalToDumpWithIndent) {
  json j;
  j["a"] = "hello";
  j["b"] = json::array({1, 2, 3});

  EXPECT_EQ(DumpJson(j, 2), j.dump(2));
}

// =============================================================================
// No-bare-dump gate
//
// Every .dump( call under src/worker/ must go through DumpJson() instead
// -- the one sanctioned exception is the helper's own implementation.
// test/src/worker/BUILD gives this target :all_sources as `data` so the
// whole package is present, by relative path, under the test's working
// directory (the runfiles root -- the same convention
// test/lib/image:png_optimizer_test relies on for its testdata/).
// =============================================================================

TEST(NoBareJsonDumpGateTest, NoBareDumpCallsOutsideTheHelper) {
  namespace fs = std::filesystem;
  const fs::path dir = "src/worker";
  ASSERT_TRUE(fs::exists(dir))
      << "src/worker not found relative to the test's working directory -- "
         "is //src/worker:all_sources wired into this target's `data`?";

  // `.dump(` with any whitespace around the name.
  const std::regex kDumpCall(R"(\.\s*dump\s*\()");
  std::vector<std::string> violations;
  for (const auto& dir_entry : fs::recursive_directory_iterator(dir)) {
    if (!dir_entry.is_regular_file()) continue;
    const fs::path& path = dir_entry.path();
    if (path.filename() == "json_dump.h") continue;  // the sanctioned call.
    const std::string ext = path.extension().string();
    if (ext != ".cc" && ext != ".h") continue;

    std::ifstream f(path);
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
      ++lineno;
      if (std::regex_search(line, kDumpCall)) {
        std::ostringstream oss;
        oss << path.string() << ":" << lineno << ": " << line;
        violations.push_back(oss.str());
      }
    }
  }

  std::ostringstream report;
  for (const auto& v : violations) report << v << "\n";
  EXPECT_TRUE(violations.empty())
      << "bare .dump( call(s) found under src/worker/ outside json_dump.h "
         "-- route them through DumpJson() instead:\n"
      << report.str();
}

// =============================================================================
// HTTP net: a route handler that fails still answers, and the server
// keeps serving.
// =============================================================================

class JsonOutputHttpNetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    config_.port = 0;
    config_.allow_unauthenticated = true;
    handler_ = std::make_unique<NullMessageHandler>();
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  }

  void TearDown() override {
    if (loop_thread_.joinable()) StopLoopThread();
    server_->Stop();
    uv_run(loop_, UV_RUN_DEFAULT);
    server_.reset();
    uv_loop_close(loop_);
    delete loop_;
  }

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

  std::string SendRequest(const std::string& request) {
    std::string wire = test::InjectConnectionClose(request);
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, 5);
    if (sock < 0) {
      return "<connect to port " + std::to_string(port) +
             " failed: " + std::strerror(errno) + ">";
    }

    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    (void)sent;

    std::string response;
    char buf[4096];
    while (true) {
      ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
      if (n <= 0) break;
      response.append(buf, n);
    }
    test::CloseSocket(sock);
    return response;
  }

  uv_loop_t* loop_ = nullptr;
  HttpServerConfig config_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
};

TEST_F(JsonOutputHttpNetTest, FailingHandlerAnswers500AndServerKeepsServing) {
  server_->AddRoute("GET", "/v1/test-failing-handler",
                    [](const HttpRequest&) -> HttpResponse {
                      throw std::runtime_error("unexpected failure detail");
                    });
  server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
    return HttpResponse().Json(R"({"status":"ok"})");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp1 = SendRequest(
      "GET /v1/test-failing-handler HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_EQ(resp1.rfind("HTTP/1.1 500", 0), 0u) << resp1;
  // The failure detail is never echoed back to the client.
  EXPECT_EQ(resp1.find("unexpected failure detail"), std::string::npos);

  // The server keeps serving afterward.
  std::string resp2 =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp2.find("200 OK"), std::string::npos)
      << "response: [" << resp2 << "]";
}

// =============================================================================
// Cache API: well-formed JSON for any stored URL, process keeps serving.
// =============================================================================

class JsonOutputCacheApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    url_registry_ = std::make_unique<UrlRegistry>(1000);
    ctx_ = std::make_unique<CacheApiContext>(CacheApiContext{
        .cache = nullptr,
        .url_registry = *url_registry_,
        .invalidate_url = nullptr,
        .clear_dedup = nullptr,
        .enqueue_reprocess = nullptr,
        .reset_cache = nullptr,
        .url_norm_config = nullptr,
        .get_cooldown = nullptr,
        .list_cooldowns =
            [this]() -> std::vector<CacheApiContext::CooldownEntry> {
          return cooldowns_;
        },
    });

    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();
    HttpServerConfig server_config;
    server_config.port = 0;
    server_config.allow_unauthenticated = true;
    server_ =
        std::make_unique<HttpServer>(loop_, server_config, handler_.get());
    RegisterCacheRoutes(*server_, *ctx_);
    ASSERT_TRUE(server_->Start());
    StartLoopThread();
  }

  void TearDown() override {
    if (loop_thread_.joinable()) StopLoopThread();
    server_->Stop();
    uv_run(loop_, UV_RUN_DEFAULT);
    server_.reset();
    uv_loop_close(loop_);
    delete loop_;
  }

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

  std::string SendRequest(const std::string& request) {
    std::string wire = test::InjectConnectionClose(request);
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, 5);
    if (sock < 0) {
      return "<connect to port " + std::to_string(port) +
             " failed: " + std::strerror(errno) + ">";
    }

    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    (void)sent;

    std::string response;
    char buf[4096];
    while (true) {
      ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
      if (n <= 0) {
        if (response.empty()) {
          response = n == 0 ? "<closed by peer before any byte>"
                            : std::string("<read failed: ") +
                                  std::strerror(errno) + ">";
          response += " active_connections=" +
                      std::to_string(server_->active_connections());
        }
        break;
      }
      response.append(buf, n);
    }
    test::CloseSocket(sock);
    return response;
  }

  json ParseJsonBody(const std::string& response) {
    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) return {};
    return json::parse(response.substr(body_start + 4));
  }

  std::unique_ptr<UrlRegistry> url_registry_;
  std::unique_ptr<CacheApiContext> ctx_;
  std::vector<CacheApiContext::CooldownEntry> cooldowns_;
  uv_loop_t* loop_ = nullptr;
  std::unique_ptr<MessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
};

TEST_F(JsonOutputCacheApiTest, UrlsServesWellFormedJsonForAnyStoredUrl) {
  // Recorded verbatim, exactly as a live notification would land it in the
  // URL registry.
  const std::string stored_url = std::string("http://example.com/p-\xFF-q");
  url_registry_->Record(stored_url, "example.com", "https");

  std::string resp1 = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=10 HTTP/1.1\r\nHost: "
      "localhost\r\n\r\n");
  ASSERT_NE(resp1.find("200 OK"), std::string::npos)
      << "response: [" << resp1 << "]";
  json j1 = ParseJsonBody(resp1);
  ASSERT_EQ(j1["urls"].size(), 1u);
  EXPECT_NE(j1["urls"][0]["url"].get<std::string>().find("\xEF\xBF\xBD"),
            std::string::npos);

  // The process keeps serving: a second request succeeds too.
  std::string resp2 = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=10 HTTP/1.1\r\nHost: "
      "localhost\r\n\r\n");
  EXPECT_NE(resp2.find("200 OK"), std::string::npos)
      << "response: [" << resp2 << "]";
  json j2 = ParseJsonBody(resp2);
  EXPECT_EQ(j2["urls"].size(), 1u);
}

TEST_F(JsonOutputCacheApiTest, CooldownsServesWellFormedJsonForAnyUrl) {
  const std::string stored_url =
      std::string("http://example.com/cooldown-\xFF-q");
  cooldowns_.push_back(CacheApiContext::CooldownEntry{
      .url = stored_url,
      .hostname = "example.com",
      .scheme = "https",
      .reason = "processing",
      .remaining_seconds = 10,
      .duration_seconds = 60,
  });

  std::string resp1 = SendRequest(
      "GET /v1/cache/cooldowns HTTP/1.1\r\nHost: localhost\r\n\r\n");
  ASSERT_NE(resp1.find("200 OK"), std::string::npos)
      << "response: [" << resp1 << "]";
  json j1 = ParseJsonBody(resp1);
  ASSERT_EQ(j1["cooldowns"].size(), 1u);
  EXPECT_NE(j1["cooldowns"][0]["url"].get<std::string>().find("\xEF\xBF\xBD"),
            std::string::npos);

  // The process keeps serving: a second request succeeds too.
  std::string resp2 = SendRequest(
      "GET /v1/cache/cooldowns HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp2.find("200 OK"), std::string::npos)
      << "response: [" << resp2 << "]";
}

// =============================================================================
// WebSocket streams: well-formed JSON for any posted event or log message,
// process keeps serving.
// =============================================================================

class JsonOutputWsTest : public ::testing::Test {
 protected:
  void SetUp() override {
#ifndef _WIN32
    // The WebSocket server under test and its clients live in this one
    // process, and several tests close a client while the server still has
    // frames to write to it.  The daemon ignores SIGPIPE at start-up, so
    // that write is an EPIPE error that closes the connection; give the
    // server side here the same process state, or the write ends the test
    // process instead.  That the daemon binary itself survives a vanished
    // client is pinned separately, in worker_client_disconnect_test.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &saved_sigpipe_);
#endif
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();

    ws_config_.allow_unauthenticated = true;
    ws_config_.max_connections = 8;
    ws_config_.ping_interval_ms = 30000;
    ws_config_.pong_timeout_ms = 10000;
    ws_config_.auth_timeout_ms = 5000;
    ws_config_.stats_interval_ms = 60000;
    ws_config_.event_batch_ms = 30;

    manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());

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
                auto* self = static_cast<JsonOutputWsTest*>(server->data);
                auto* client = new uv_any_handle;
                uv_tcp_init(self->loop_, &client->tcp);
                if (uv_accept(server, &client->stream) == 0) {
                  self->manager_->AcceptUpgrade(&client->stream,
                                                self->pending_endpoint_,
                                                "dGhlIHNhbXBsZSBub25jZQ==", 0);
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

  int ConnectRawSocket() const { return test::ConnectTcp(port_, 2); }

  // Read until the full handshake response, byte-by-byte near the end so
  // WS frame bytes arriving in the same TCP segment are not consumed.
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
      for (unsigned char byte : ext) {
        len = (len << 8) | byte;
      }
    }

    std::string payload(len, '\0');
    size_t total = 0;
    while (total < len) {
      n = test::SocketRead(sock, payload.data() + total, len - total);
      if (n <= 0) break;
      total += n;
    }

    if (opcode != 0x1) return std::string(1, char(opcode)) + payload;
    return payload;
  }

  // Read text frames until one matches the expected JSON "type", skipping
  // control frames. Gives up after 3 attempts.
  std::string ReadTextFrameByType(int sock, const std::string& expected_type) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      std::string msg = ReadTextFrame(sock);
      if (msg.empty()) break;
      if (msg[0] == '\x09' || msg[0] == '\x0A' || msg[0] == '\x08') continue;
      json j = json::parse(msg, nullptr, false);
      if (!j.is_discarded() && j.contains("type") &&
          j["type"] == expected_type) {
        return msg;
      }
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
  std::string pending_endpoint_ = "events";
};

TEST_F(JsonOutputWsTest, EventsServesWellFormedJsonForAnyPostedUrl) {
  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Mirrors worker.cc's ev["url"] = notification.url feeding
  // WsManager::PostEvent for the SVG pipeline's event notifications.
  const std::string posted_url = std::string("/svg-\xFF-icon.png");
  manager_->PostEvent("svg_vectorized", json{{"url", posted_url}});

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::string msg = ReadTextFrameByType(sock, "events");
  ASSERT_FALSE(msg.empty());
  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  ASSERT_TRUE(j["data"].is_array());
  ASSERT_EQ(j["data"].size(), 1u);
  EXPECT_EQ(j["data"][0]["type"], "svg_vectorized");
  std::string got_url = j["data"][0]["data"]["url"].get<std::string>();
  EXPECT_NE(got_url.find("\xEF\xBF\xBD"), std::string::npos);

  test::CloseSocket(sock);

  // The process keeps serving: a fresh connection, a fresh event.
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ReadHandshake(sock2);
  manager_->PostEvent("svg_vectorized", json{{"url", "/fine.png"}});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::string msg2 = ReadTextFrameByType(sock2, "events");
  EXPECT_FALSE(msg2.empty());
  test::CloseSocket(sock2);
}

TEST_F(JsonOutputWsTest, LogsSnapshotServesWellFormedJsonForAnyRingEntry) {
  const std::string posted_message =
      std::string("optimize failed: \xFF bad byte");
  manager_->PostLog("warning", "worker", "image", posted_message);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  std::string handshake = ReadHandshake(sock);
  ASSERT_NE(handshake.find("101"), std::string::npos);

  std::string msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(msg.empty());
  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  ASSERT_GE(j["entries"].size(), 1u);
  std::string got = j["entries"][0]["message"].get<std::string>();
  EXPECT_NE(got.find("\xEF\xBF\xBD"), std::string::npos);

  test::CloseSocket(sock);

  // The process keeps serving: a fresh connection still gets a snapshot.
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  std::string handshake2 = ReadHandshake(sock2);
  EXPECT_NE(handshake2.find("101"), std::string::npos);
  test::CloseSocket(sock2);
}

TEST_F(JsonOutputWsTest, LogsDeliversWellFormedJsonForAnyPostedMessage) {
  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrameByType(sock, "snapshot");  // Drain the (possibly empty) one.

  const std::string posted_message = std::string("image too large: \xFF");
  manager_->PostLog("warning", "worker", "image", posted_message);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::string msg = ReadTextFrameByType(sock, "log");
  ASSERT_FALSE(msg.empty());
  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  std::string got = j["message"].get<std::string>();
  EXPECT_NE(got.find("\xEF\xBF\xBD"), std::string::npos);

  test::CloseSocket(sock);
}

}  // namespace
}  // namespace pagespeed
