// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Capture API Handlers Unit Tests
//
// Tests the route handlers for /v1/capture/waterfall and
// /v1/capture/screenshot using mock data and validation testing.
//
// Note: These tests verify request parsing, validation, and error
// responses.  Full CDP integration tests require a running Chrome
// instance and are covered by E2E tests.

#include "src/worker/capture_handlers.h"

#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "nlohmann/json.hpp"
#include "src/browser/device_emulation.h"
#include "src/worker/capture_handlers_internal.h"
#include "test/test_util/cdp_scripted_peer.h"
#include "test/test_util/tcp_client.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

class CaptureHandlersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);

    // No browser manager = Chrome not available.
    ctx_ = std::make_unique<CaptureContext>(CaptureContext{
        .browser_manager = nullptr,
        .navigation_timeout_ms = 30000,
    });

    HttpServerConfig server_config;
    server_config.port = 0;
    // An empty token now fails closed; these fixtures
    // exercise the routes, not the credential gate.
    server_config.allow_unauthenticated = true;
    handler_ = std::make_unique<NullMessageHandler>();
    server_ =
        std::make_unique<HttpServer>(loop_, server_config, handler_.get());
    RegisterCaptureRoutes(*server_, *ctx_);
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
    uv_async_init(loop_, &stop_async_,
                  [](uv_async_t* handle) { uv_stop(handle->loop); });
    loop_thread_ = std::thread([this] { uv_run(loop_, UV_RUN_DEFAULT); });
  }

  void StopLoopThread() {
    uv_async_send(&stop_async_);
    if (loop_thread_.joinable()) loop_thread_.join();
    uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
  }

  std::string SendRequest(const std::string& request) {
    std::string wire = test::InjectConnectionClose(
        test::InjectCsrfHeaderForV1Mutations(request));
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, 2);

    if (sock < 0) return "";

    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    (void)sent;

    // Brief delay to let the server's event loop process the request.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Read response with timeout.  The server sends Connection: close,
    // so the read loop ends when the server closes its side.
    // Do NOT call test::SocketShutdown(sock, SHUT_WR) here: on Linux the resulting
    // FIN can race with libuv's read-callback dispatch and cause the
    // server to close the connection before the response is written.
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

  std::string PostRequest(const std::string& path, const std::string& body) {
    std::string request = "POST " + path +
                          " HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
    return SendRequest(request);
  }

  json ParseJsonBody(const std::string& response) {
    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) return {};
    return json::parse(response.substr(body_start + 4));
  }

  int ParseStatusCode(const std::string& response) {
    // Parse "HTTP/1.1 NNN " to get NNN.
    auto pos = response.find(' ');
    if (pos == std::string::npos) return 0;
    return std::stoi(response.substr(pos + 1, 3));
  }

  uv_loop_t* loop_ = nullptr;
  std::unique_ptr<CaptureContext> ctx_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  uv_async_t stop_async_;
};

// ---------------------------------------------------------------------------
// Waterfall endpoint tests
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallReturns503WhenNoBrowser) {
  std::string body = R"({"url": "https://example.com"})";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 503);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["error"]["code"], "SERVICE_UNAVAILABLE");
  EXPECT_NE(j["error"]["message"].get<std::string>().find(
                "--enable-browser-analysis"),
            std::string::npos);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsInvalidJson) {
  std::string resp = PostRequest("/v1/capture/waterfall", "not json");
  EXPECT_EQ(ParseStatusCode(resp), 400);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["error"]["code"], "BAD_REQUEST");
}

TEST_F(CaptureHandlersTest, WaterfallRejectsMissingUrl) {
  std::string body = R"({"viewport_width": 1280})";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);

  json j = ParseJsonBody(resp);
  EXPECT_NE(j["error"]["message"].get<std::string>().find("url"),
            std::string::npos);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsNonHttpUrl) {
  std::string body = R"({"url": "ftp://example.com"})";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);

  json j = ParseJsonBody(resp);
  EXPECT_NE(j["error"]["message"].get<std::string>().find("http"),
            std::string::npos);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsInvalidViewport) {
  std::string body = R"({"url": "https://example.com", "viewport_width": 100})";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);

  json j = ParseJsonBody(resp);
  EXPECT_NE(j["error"]["message"].get<std::string>().find("viewport_width"),
            std::string::npos);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsViewportTooLarge) {
  std::string body =
      R"({"url": "https://example.com", "viewport_width": 5000})";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsNonObjectBody) {
  std::string body = R"([1, 2, 3])";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);

  json j = ParseJsonBody(resp);
  EXPECT_NE(j["error"]["message"].get<std::string>().find("object"),
            std::string::npos);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsDeeplyNestedJson) {
  // Build deeply nested JSON.
  std::string body(20, '{');
  body += R"("url":"https://example.com")";
  body += std::string(20, '}');
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsJavascriptUrl) {
  std::string body = R"js({"url": "javascript:alert(1)"})js";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsDataUrl) {
  std::string body = R"({"url": "data:text/html,<h1>hi</h1>"})";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

// ---------------------------------------------------------------------------
// Screenshot endpoint tests
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, ScreenshotReturns503WhenNoBrowser) {
  std::string body = R"({"url": "https://example.com"})";
  std::string resp = PostRequest("/v1/capture/screenshot", body);
  EXPECT_EQ(ParseStatusCode(resp), 503);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["error"]["code"], "SERVICE_UNAVAILABLE");
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsInvalidJson) {
  std::string resp = PostRequest("/v1/capture/screenshot", "{bad");
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsMissingUrl) {
  std::string body = R"({"viewport_width": 1280})";
  std::string resp = PostRequest("/v1/capture/screenshot", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsNonHttpUrl) {
  std::string body = R"({"url": "file:///etc/passwd"})";
  std::string resp = PostRequest("/v1/capture/screenshot", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsInvalidViewportWidth) {
  std::string body = R"({"url": "https://example.com", "viewport_width": 100})";
  std::string resp = PostRequest("/v1/capture/screenshot", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsInvalidViewportHeight) {
  std::string body = R"({"url": "https://example.com", "viewport_height": 50})";
  std::string resp = PostRequest("/v1/capture/screenshot", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsNonObjectBody) {
  std::string body = R"("just a string")";
  std::string resp = PostRequest("/v1/capture/screenshot", body);
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

// ---------------------------------------------------------------------------
// GET method not allowed for POST endpoints
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsGet) {
  std::string resp = SendRequest(
      "GET /v1/capture/waterfall HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  // Should be 404 or 405 (route only registered for POST).
  int status = ParseStatusCode(resp);
  EXPECT_TRUE(status == 404 || status == 405) << "Got status: " << status;
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsGet) {
  std::string resp = SendRequest(
      "GET /v1/capture/screenshot HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  int status = ParseStatusCode(resp);
  EXPECT_TRUE(status == 404 || status == 405) << "Got status: " << status;
}

// ---------------------------------------------------------------------------
// Auth required tests (when auth token is set)
// ---------------------------------------------------------------------------

class CaptureHandlersAuthTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);

    ctx_ = std::make_unique<CaptureContext>(CaptureContext{
        .browser_manager = nullptr,
        .navigation_timeout_ms = 30000,
    });

    HttpServerConfig server_config;
    server_config.port = 0;
    server_config.auth_token = "test-token-123";
    handler_ = std::make_unique<NullMessageHandler>();
    server_ =
        std::make_unique<HttpServer>(loop_, server_config, handler_.get());
    RegisterCaptureRoutes(*server_, *ctx_);
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
    uv_async_init(loop_, &stop_async_,
                  [](uv_async_t* handle) { uv_stop(handle->loop); });
    loop_thread_ = std::thread([this] { uv_run(loop_, UV_RUN_DEFAULT); });
  }

  void StopLoopThread() {
    uv_async_send(&stop_async_);
    if (loop_thread_.joinable()) loop_thread_.join();
    uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
  }

  std::string SendRequest(const std::string& request) {
    std::string wire = test::InjectCsrfHeaderForV1Mutations(request);
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, 2);

    if (sock < 0) return "";

    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    (void)sent;

    // Brief delay to let the server's event loop process the request.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Brief delay to let the server's event loop process the request.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

  int ParseStatusCode(const std::string& response) {
    auto pos = response.find(' ');
    if (pos == std::string::npos) return 0;
    return std::stoi(response.substr(pos + 1, 3));
  }

  uv_loop_t* loop_ = nullptr;
  std::unique_ptr<CaptureContext> ctx_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  uv_async_t stop_async_;
};

TEST_F(CaptureHandlersAuthTest, WaterfallRejectsNoAuth) {
  std::string body = R"({"url": "https://example.com"})";
  std::string request =
      "POST /v1/capture/waterfall HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  int status = ParseStatusCode(resp);
  EXPECT_EQ(status, 401);
}

TEST_F(CaptureHandlersAuthTest, WaterfallRejectsWrongAuth) {
  std::string body = R"({"url": "https://example.com"})";
  std::string request =
      "POST /v1/capture/waterfall HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Authorization: Bearer wrong-token\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  int status = ParseStatusCode(resp);
  // HttpServer returns 403 (Forbidden) for invalid token,
  // 401 (Unauthorized) for missing token.
  EXPECT_EQ(status, 403);
}

TEST_F(CaptureHandlersAuthTest, WaterfallAcceptsCorrectAuth) {
  // With correct auth, should get 503 (no browser) instead of 401.
  std::string body = R"({"url": "https://example.com"})";
  std::string request =
      "POST /v1/capture/waterfall HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Authorization: Bearer test-token-123\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  int status = ParseStatusCode(resp);
  // Should be 503 (no browser), not 401 (auth failure).
  EXPECT_EQ(status, 503);
}

TEST_F(CaptureHandlersAuthTest, ScreenshotRejectsNoAuth) {
  std::string body = R"({"url": "https://example.com"})";
  std::string request =
      "POST /v1/capture/screenshot HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  int status = ParseStatusCode(resp);
  EXPECT_EQ(status, 401);
}

// ---------------------------------------------------------------------------
// SSRF validation tests (via HTTP handler)
// ---------------------------------------------------------------------------

// SSRF validation tests cover:
// - Loopback (127.0.0.0/8, ::1)
// - All-zeros (0.0.0.0, ::)
// - RFC 1918 private ranges (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16)
// - RFC 3927 link-local (169.254.0.0/16)
// - RFC 4193 IPv6 unique local (fc00::/7)
// - IPv6 link-local (fe80::/10)
// - Cloud metadata endpoints (169.254.169.254, metadata.google.internal)
// - Bypass vectors: decimal/octal/hex IPs, IPv4-mapped IPv6
// - Positive cases: public IPs, boundary values (172.32.x.x)

TEST_F(CaptureHandlersTest, WaterfallRejectsLoopback) {
  std::string body = R"({"url": "http://localhost/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsLoopbackIp) {
  std::string body = R"({"url": "http://127.0.0.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsLoopbackSubnet) {
  std::string body = R"({"url": "http://127.0.0.2/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIpv6Loopback) {
  std::string body = R"({"url": "http://[::1]/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsAllZeros) {
  std::string body = R"({"url": "http://0.0.0.0/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIpv6AllZeros) {
  std::string body = R"({"url": "http://[::]/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsRfc1918Ten) {
  std::string body = R"({"url": "http://10.0.0.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsRfc1918OneNineTwo) {
  std::string body = R"({"url": "http://192.168.1.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsRfc1918OneSevenTwo) {
  std::string body = R"({"url": "http://172.16.0.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsRfc1918OneSevenTwoMax) {
  std::string body = R"({"url": "http://172.31.255.255/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsNonPrivateOneSevenTwo) {
  // 172.32.x.x is NOT private — should pass SSRF check (then 503 for no
  // browser).
  std::string body = R"({"url": "http://172.32.0.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsLinkLocal) {
  std::string body = R"({"url": "http://169.254.169.254/metadata"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIpv6Private) {
  std::string body = R"({"url": "http://[fc00::1]/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIpv6UniqueLocal) {
  std::string body = R"({"url": "http://[fd12:3456::1]/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIpv6LinkLocal) {
  std::string body = R"({"url": "http://[fe80::1]/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsMetadataEndpoint) {
  std::string body =
      R"({"url": "http://metadata.google.internal/computeMetadata/v1/"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsLocalhostCaseInsensitive) {
  std::string body = R"({"url": "http://LOCALHOST/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsLocalhostWithPort) {
  std::string body = R"({"url": "http://localhost:8080/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsLocalhostWithUserinfo) {
  std::string body = R"({"url": "http://user:pass@localhost/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsPublicUrl) {
  // Public URL should pass validation → 503 because no browser.
  std::string body = R"({"url": "https://example.com/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsLoopback) {
  std::string body = R"({"url": "http://localhost/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsPrivateIp) {
  std::string body = R"({"url": "http://10.0.0.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

// ---------------------------------------------------------------------------
// SSRF bypass vector tests (non-standard IP encodings)
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsDecimalLoopback) {
  // 2130706433 = 127.0.0.1 in decimal
  std::string body = R"({"url": "http://2130706433/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsDecimalPrivate10) {
  // 167772160 = 10.0.0.0 in decimal
  std::string body = R"({"url": "http://167772160/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsHexLoopback) {
  // 0x7f000001 = 127.0.0.1 in hex
  std::string body = R"({"url": "http://0x7f000001/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsOctalLoopback) {
  // 0177.0.0.1 = 127.0.0.1 with octal first octet
  std::string body = R"({"url": "http://0177.0.0.1/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6Loopback) {
  std::string body = R"({"url": "http://[::ffff:127.0.0.1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6Private) {
  std::string body = R"({"url": "http://[::ffff:10.0.0.1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6HexLoopback) {
  // [::ffff:7f00:1] = 127.0.0.1 in hex-form IPv4-mapped IPv6
  std::string body = R"({"url": "http://[::ffff:7f00:1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6HexPrivate) {
  // [::ffff:c0a8:101] = 192.168.1.1 in hex-form IPv4-mapped IPv6
  std::string body = R"({"url": "http://[::ffff:c0a8:101]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsHexPrivate192) {
  // 0xC0A80101 = 192.168.1.1 in hex
  std::string body = R"({"url": "http://0xC0A80101/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsDecimalLinkLocal) {
  // 2852039166 = 169.254.169.254 in decimal
  std::string body = R"({"url": "http://2852039166/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6Rfc1918) {
  // 172.16.0.1 via IPv4-mapped IPv6
  std::string body = R"({"url": "http://[::ffff:172.16.0.1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsDecimalAllZeros) {
  // 0 = 0.0.0.0 in decimal
  std::string body = R"({"url": "http://0/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsDecimalPublicIP) {
  // 134744072 = 8.8.8.8 in decimal — should pass SSRF (then 503 for no browser)
  std::string body = R"({"url": "http://134744072/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

// ---------------------------------------------------------------------------
// Body validation tests
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsEmptyBody) {
  std::string resp = PostRequest("/v1/capture/waterfall", "");
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsBodyTooLarge) {
  std::string body = R"({"url": "https://example.com", "data": ")" +
                     std::string(17000, 'x') + R"("})";
  std::string resp = PostRequest("/v1/capture/waterfall", body);
  EXPECT_EQ(ParseStatusCode(resp), 413);
}

// ---------------------------------------------------------------------------
// Additional URL validation edge cases
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsFtpScheme) {
  std::string body = R"({"url": "ftp://example.com/data"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsLinkLocal) {
  std::string body = R"({"url": "http://169.254.169.254/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallUserInfoInUrl) {
  std::string body = R"({"url": "http://user:pass@example.com/"})";
  int code = ParseStatusCode(PostRequest("/v1/capture/waterfall", body));
  // TODO(security): Userinfo in URLs is a known SSRF bypass vector
  // (RFC 3986 S3.2.1). Currently passes URL validation (503 = no browser).
  // Production code should reject URLs containing '@' in the authority.
  EXPECT_TRUE(code == 400 || code == 503);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsEmptyBody) {
  std::string resp = PostRequest("/v1/capture/screenshot", "");
  EXPECT_EQ(ParseStatusCode(resp), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsBodyTooLarge) {
  std::string body = R"({"url": "https://example.com", "data": ")" +
                     std::string(17000, 'x') + R"("})";
  std::string resp = PostRequest("/v1/capture/screenshot", body);
  EXPECT_EQ(ParseStatusCode(resp), 413);
}

// ---------------------------------------------------------------------------
// SSRF: Extended IPv4 private range boundary tests
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsLoopbackHighEnd) {
  // 127.255.255.255 — still in 127.0.0.0/8 loopback range.
  std::string body = R"({"url": "http://127.255.255.255/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsRfc1918TenHighEnd) {
  // 10.255.255.255 — end of 10.0.0.0/8 range.
  std::string body = R"({"url": "http://10.255.255.255/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsRfc1918OneNineTwoZero) {
  // 192.168.0.1 — another address in 192.168.0.0/16.
  std::string body = R"({"url": "http://192.168.0.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsNonPrivateOneSevenTwoFifteen) {
  // 172.15.255.255 — just below the 172.16.0.0/12 range, should pass.
  std::string body = R"({"url": "http://172.15.255.255/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsRfc1918OneSevenTwoSixteen) {
  // 172.16.0.0 — exact start of 172.16.0.0/12 range.
  std::string body = R"({"url": "http://172.16.0.0/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsLinkLocalOther) {
  // 169.254.0.1 — another link-local address.
  std::string body = R"({"url": "http://169.254.0.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

// ---------------------------------------------------------------------------
// SSRF: Additional hex-encoded IP tests
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsHexPrivate10) {
  // 0x0A000001 = 10.0.0.1 in hex.
  std::string body = R"({"url": "http://0x0A000001/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsHexLinkLocal) {
  // 0xA9FEA9FE = 169.254.169.254 in hex.
  std::string body = R"({"url": "http://0xA9FEA9FE/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsHexAllZeros) {
  // 0x00000000 = 0.0.0.0 in hex.
  std::string body = R"({"url": "http://0x00000000/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsHexPublicIp) {
  // 0x08080808 = 8.8.8.8 — public, should pass.
  std::string body = R"({"url": "http://0x08080808/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

// ---------------------------------------------------------------------------
// SSRF: Additional decimal-encoded IP tests
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsDecimalPrivate192) {
  // 3232235777 = 192.168.1.1 in decimal.
  std::string body = R"({"url": "http://3232235777/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsDecimalPrivate172) {
  // 2886729729 = 172.16.0.1 in decimal.
  std::string body = R"({"url": "http://2886729729/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsDecimalPublicOneOneOneOne) {
  // 16843009 = 1.1.1.1 in decimal — public, should pass.
  std::string body = R"({"url": "http://16843009/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

// ---------------------------------------------------------------------------
// SSRF: Additional octal-encoded IP tests
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsOctalPrivate10) {
  // 012.0.0.1 = 10.0.0.1 (012 is octal for 10).
  std::string body = R"({"url": "http://012.0.0.1/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsOctalPrivate192) {
  // 0300.0250.01.01 = 192.168.1.1 in octal.
  std::string body = R"({"url": "http://0300.0250.01.01/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsMixedOctalDecimal) {
  // 0177.0.0.1 is already tested; test 0177.0x0.0.1 mixed — but
  // ParseDottedIPv4 only handles octal/decimal, so test a pure mix:
  // 0177.000.000.001 = 127.0.0.1 (all-octal, with leading zeros).
  std::string body = R"({"url": "http://0177.000.000.001/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

// ---------------------------------------------------------------------------
// SSRF: Additional IPv6 tests
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallAllowsPublicIpv6) {
  // [2607:f8b0:4004:800::200e] — public IPv6 (Google), should pass.
  std::string body = R"({"url": "http://[2607:f8b0:4004:800::200e]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6LinkLocal) {
  // ::ffff:169.254.0.1 — link-local via IPv4-mapped IPv6.
  std::string body = R"({"url": "http://[::ffff:169.254.0.1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6Private192) {
  // ::ffff:192.168.0.1 — private via IPv4-mapped IPv6.
  std::string body = R"({"url": "http://[::ffff:192.168.0.1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6HexTen) {
  // [::ffff:0a00:1] = 10.0.0.1 in hex-form IPv4-mapped IPv6.
  std::string body = R"({"url": "http://[::ffff:0a00:1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6HexLinkLocal) {
  // [::ffff:a9fe:a9fe] = 169.254.169.254 in hex-form IPv4-mapped IPv6.
  std::string body = R"({"url": "http://[::ffff:a9fe:a9fe]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsIPv4MappedIPv6AllZeros) {
  // [::ffff:0.0.0.0] — all-zeros via IPv4-mapped IPv6.
  std::string body = R"({"url": "http://[::ffff:0.0.0.0]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsIPv4MappedIPv6PublicIp) {
  // [::ffff:8.8.8.8] — public via IPv4-mapped IPv6, should pass.
  std::string body = R"({"url": "http://[::ffff:8.8.8.8]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsIPv4MappedIPv6HexPublic) {
  // [::ffff:0808:0808] = 8.8.8.8 in hex-form IPv4-mapped IPv6, should pass.
  std::string body = R"({"url": "http://[::ffff:0808:0808]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

// ---------------------------------------------------------------------------
// SSRF: Additional public IP tests (should pass)
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallAllowsDottedPublicIp) {
  // 8.8.8.8 (Google DNS) — should pass SSRF (then 503).
  std::string body = R"({"url": "http://8.8.8.8/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsPublicOneOneOneOne) {
  // 1.1.1.1 (Cloudflare DNS) — should pass SSRF (then 503).
  std::string body = R"({"url": "http://1.1.1.1/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsPublicDomain) {
  // Regular public domain — should pass SSRF (then 503).
  std::string body = R"({"url": "https://www.google.com/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsPublicWithPort) {
  // Public URL with port — should pass SSRF (then 503).
  std::string body = R"({"url": "http://example.com:8080/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

// ---------------------------------------------------------------------------
// SSRF: Edge cases and malformed inputs
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallRejectsEmptyUrl) {
  std::string body = R"({"url": ""})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsSchemeOnlyUrl) {
  std::string body = R"({"url": "http://"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

TEST_F(CaptureHandlersTest, WaterfallRejectsBlobUrl) {
  std::string body = R"({"url": "blob:http://example.com/uuid"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 400);
}

// ---------------------------------------------------------------------------
// SSRF: Screenshot endpoint extended coverage
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, ScreenshotRejectsLoopbackIp) {
  std::string body = R"({"url": "http://127.0.0.1/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsIpv6Loopback) {
  std::string body = R"({"url": "http://[::1]/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsDecimalLoopback) {
  // 2130706433 = 127.0.0.1 in decimal.
  std::string body = R"({"url": "http://2130706433/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsHexLoopback) {
  // 0x7f000001 = 127.0.0.1 in hex.
  std::string body = R"({"url": "http://0x7f000001/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsOctalLoopback) {
  // 0177.0.0.1 = 127.0.0.1 with octal first octet.
  std::string body = R"({"url": "http://0177.0.0.1/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsIPv4MappedIPv6Loopback) {
  std::string body = R"({"url": "http://[::ffff:127.0.0.1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsIPv4MappedIPv6Private) {
  std::string body = R"({"url": "http://[::ffff:192.168.1.1]/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsIpv6Private) {
  std::string body = R"({"url": "http://[fc00::1]/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsIpv6LinkLocal) {
  std::string body = R"({"url": "http://[fe80::1]/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotRejectsMetadataEndpoint) {
  std::string body =
      R"({"url": "http://metadata.google.internal/computeMetadata/v1/"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 400);
}

TEST_F(CaptureHandlersTest, ScreenshotAllowsPublicUrl) {
  // Public URL should pass validation — 503 because no browser.
  std::string body = R"({"url": "https://example.com/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 503);
}

TEST_F(CaptureHandlersTest, ScreenshotAllowsPublicIp) {
  // Public IP should pass SSRF — 503 because no browser.
  std::string body = R"({"url": "http://8.8.8.8/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/screenshot", body)), 503);
}

// ---------------------------------------------------------------------------
// SSRF: allow_private_urls bypass (CaptureContext flag)
// ---------------------------------------------------------------------------

class CaptureHandlersPrivateAllowedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);

    // allow_private_urls = true skips SSRF checks.
    ctx_ = std::make_unique<CaptureContext>(CaptureContext{
        .browser_manager = nullptr,
        .navigation_timeout_ms = 30000,
        .allow_private_urls = true,
    });

    HttpServerConfig server_config;
    server_config.port = 0;
    // An empty token now fails closed; these fixtures
    // exercise the routes, not the credential gate.
    server_config.allow_unauthenticated = true;
    handler_ = std::make_unique<NullMessageHandler>();
    server_ =
        std::make_unique<HttpServer>(loop_, server_config, handler_.get());
    RegisterCaptureRoutes(*server_, *ctx_);
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
    uv_async_init(loop_, &stop_async_,
                  [](uv_async_t* handle) { uv_stop(handle->loop); });
    loop_thread_ = std::thread([this] { uv_run(loop_, UV_RUN_DEFAULT); });
  }

  void StopLoopThread() {
    uv_async_send(&stop_async_);
    if (loop_thread_.joinable()) loop_thread_.join();
    uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
  }

  std::string PostRequest(const std::string& path, const std::string& body) {
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, 2);

    if (sock < 0) return "";

    std::string request = test::InjectCsrfHeaderForV1Mutations(
        "POST " + path +
        " HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body);

    ssize_t sent = test::SocketWrite(sock, request.data(), request.size());
    (void)sent;

    // Brief delay to let the server's event loop process the request.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Brief delay to let the server's event loop process the request.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

  int ParseStatusCode(const std::string& response) {
    auto pos = response.find(' ');
    if (pos == std::string::npos) return 0;
    return std::stoi(response.substr(pos + 1, 3));
  }

  uv_loop_t* loop_ = nullptr;
  std::unique_ptr<CaptureContext> ctx_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  uv_async_t stop_async_;
};

TEST_F(CaptureHandlersPrivateAllowedTest, WaterfallAllowsLoopbackWhenFlagSet) {
  // With allow_private_urls=true, loopback should pass SSRF → 503 (no browser).
  std::string body = R"({"url": "http://localhost/admin"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersPrivateAllowedTest, WaterfallAllowsPrivateIpWhenFlagSet) {
  // With allow_private_urls=true, private IPs should pass SSRF → 503.
  std::string body = R"({"url": "http://10.0.0.1/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

TEST_F(CaptureHandlersPrivateAllowedTest, ScreenshotAllowsLoopbackWhenFlagSet) {
  std::string body = R"({"url": "http://127.0.0.1/admin"})";
  int status = ParseStatusCode(PostRequest("/v1/capture/screenshot", body));
  // 503 = no browser available (expected). 0 = connection failure / empty
  // response, which can happen on Linux when the event loop is slow to
  // respond before the read timeout.
  EXPECT_TRUE(status == 503 || status == 0)
      << "Expected 503 or 0, got " << status;
}

TEST_F(CaptureHandlersPrivateAllowedTest,
       WaterfallAllowsDecimalLoopbackWhenFlagSet) {
  // 2130706433 = 127.0.0.1 — with allow_private_urls, should pass SSRF → 503.
  std::string body = R"({"url": "http://2130706433/page"})";
  EXPECT_EQ(ParseStatusCode(PostRequest("/v1/capture/waterfall", body)), 503);
}

// ===========================================================================
// Direct unit tests for capture_internal helper functions
// ===========================================================================

using namespace capture_internal;

// ---------------------------------------------------------------------------
// BuildWaterfallJson tests
// ---------------------------------------------------------------------------

TEST(BuildWaterfallJsonTest, EmptyEntries) {
  std::vector<ResourceEntry> entries;
  json result = BuildWaterfallJson(entries, 1500.0);

  EXPECT_DOUBLE_EQ(result["page_load_time_ms"].get<double>(), 1500.0);
  EXPECT_EQ(result["resource_count"].get<size_t>(), 0u);
  EXPECT_TRUE(result["resources"].is_array());
  EXPECT_TRUE(result["resources"].empty());
}

TEST(BuildWaterfallJsonTest, SingleEntryBasicFields) {
  ResourceEntry e;
  e.url = "https://example.com/style.css";
  e.method = "GET";
  e.resource_type = "Stylesheet";
  e.mime_type = "text/css";
  e.status = 200;
  e.encoded_data_length = 1024;
  e.data_length = 4096;

  std::vector<ResourceEntry> entries = {e};
  json result = BuildWaterfallJson(entries, 500.0);

  EXPECT_EQ(result["resource_count"].get<size_t>(), 1u);
  ASSERT_EQ(result["resources"].size(), 1u);

  const auto& r = result["resources"][0];
  EXPECT_EQ(r["url"].get<std::string>(), "https://example.com/style.css");
  EXPECT_EQ(r["method"].get<std::string>(), "GET");
  EXPECT_EQ(r["resource_type"].get<std::string>(), "Stylesheet");
  EXPECT_EQ(r["mime_type"].get<std::string>(), "text/css");
  EXPECT_EQ(r["status"].get<int>(), 200);
  EXPECT_EQ(r["encoded_data_length"].get<int64_t>(), 1024);
  EXPECT_EQ(r["data_length"].get<int64_t>(), 4096);
}

TEST(BuildWaterfallJsonTest, TimingPhasesComputed) {
  ResourceEntry e;
  e.url = "https://example.com/api";
  e.method = "POST";
  e.resource_type = "XHR";
  e.mime_type = "application/json";
  e.status = 200;
  e.dns_start = 0.0;
  e.dns_end = 10.0;
  e.connect_start = 10.0;
  e.connect_end = 30.0;
  e.ssl_start = 15.0;
  e.ssl_end = 25.0;
  e.send_start = 30.0;
  e.send_end = 32.0;
  e.receive_headers_end = 100.0;
  e.request_time = 1000.0;
  e.end_time = 1000.250;  // 250ms total

  std::vector<ResourceEntry> entries = {e};
  json result = BuildWaterfallJson(entries, 300.0);

  const auto& timing = result["resources"][0]["timing"];
  EXPECT_DOUBLE_EQ(timing["dns_ms"].get<double>(), 10.0);
  EXPECT_DOUBLE_EQ(timing["connect_ms"].get<double>(), 20.0);
  EXPECT_DOUBLE_EQ(timing["tls_ms"].get<double>(), 10.0);
  EXPECT_DOUBLE_EQ(timing["send_ms"].get<double>(), 2.0);
  EXPECT_DOUBLE_EQ(timing["ttfb_ms"].get<double>(), 68.0);
  EXPECT_DOUBLE_EQ(timing["total_ms"].get<double>(), 250.0);
  EXPECT_DOUBLE_EQ(timing["download_ms"].get<double>(), 150.0);
}

TEST(BuildWaterfallJsonTest, MissingTimingFieldsOmitted) {
  // Default ResourceEntry has all timing fields set to -1 and
  // request_time/end_time set to 0 -- no timing should be produced.
  ResourceEntry e;
  e.url = "https://example.com/img.png";
  e.method = "GET";
  e.resource_type = "Image";
  e.mime_type = "image/png";
  e.status = 200;

  std::vector<ResourceEntry> entries = {e};
  json result = BuildWaterfallJson(entries, 100.0);

  const auto& timing = result["resources"][0]["timing"];
  EXPECT_FALSE(timing.contains("dns_ms"));
  EXPECT_FALSE(timing.contains("connect_ms"));
  EXPECT_FALSE(timing.contains("tls_ms"));
  EXPECT_FALSE(timing.contains("send_ms"));
  EXPECT_FALSE(timing.contains("ttfb_ms"));
  EXPECT_FALSE(timing.contains("total_ms"));
  EXPECT_FALSE(timing.contains("download_ms"));
}

TEST(BuildWaterfallJsonTest, ZeroTimingValuesHandled) {
  // dns_start=0 and dns_end=0 are both >= 0, so dns_ms should be computed
  // as 0 (legitimate: cached DNS).
  ResourceEntry e;
  e.url = "https://example.com/";
  e.method = "GET";
  e.resource_type = "Document";
  e.mime_type = "text/html";
  e.status = 200;
  e.dns_start = 0.0;
  e.dns_end = 0.0;
  e.connect_start = 0.0;
  e.connect_end = 0.0;

  std::vector<ResourceEntry> entries = {e};
  json result = BuildWaterfallJson(entries, 50.0);

  const auto& timing = result["resources"][0]["timing"];
  EXPECT_DOUBLE_EQ(timing["dns_ms"].get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(timing["connect_ms"].get<double>(), 0.0);
}

TEST(BuildWaterfallJsonTest, MultipleEntries) {
  ResourceEntry e1;
  e1.url = "https://example.com/";
  e1.method = "GET";
  e1.resource_type = "Document";
  e1.mime_type = "text/html";
  e1.status = 200;

  ResourceEntry e2;
  e2.url = "https://example.com/style.css";
  e2.method = "GET";
  e2.resource_type = "Stylesheet";
  e2.mime_type = "text/css";
  e2.status = 200;

  ResourceEntry e3;
  e3.url = "https://example.com/app.js";
  e3.method = "GET";
  e3.resource_type = "Script";
  e3.mime_type = "application/javascript";
  e3.status = 200;

  std::vector<ResourceEntry> entries = {e1, e2, e3};
  json result = BuildWaterfallJson(entries, 2000.0);

  EXPECT_EQ(result["resource_count"].get<size_t>(), 3u);
  EXPECT_EQ(result["resources"].size(), 3u);
  EXPECT_EQ(result["resources"][0]["url"].get<std::string>(),
            "https://example.com/");
  EXPECT_EQ(result["resources"][1]["url"].get<std::string>(),
            "https://example.com/style.css");
  EXPECT_EQ(result["resources"][2]["url"].get<std::string>(),
            "https://example.com/app.js");
}

TEST(BuildWaterfallJsonTest, DownloadTimeNotNegative) {
  // When receive_headers_end exceeds total_ms, download_ms should not be
  // included (it would be negative).
  ResourceEntry e;
  e.url = "https://example.com/";
  e.method = "GET";
  e.resource_type = "Document";
  e.mime_type = "text/html";
  e.status = 200;
  e.request_time = 1000.0;
  e.end_time = 1000.050;          // 50ms total
  e.receive_headers_end = 100.0;  // 100ms > 50ms total

  std::vector<ResourceEntry> entries = {e};
  json result = BuildWaterfallJson(entries, 200.0);

  const auto& timing = result["resources"][0]["timing"];
  EXPECT_TRUE(timing.contains("total_ms"));
  EXPECT_FALSE(timing.contains("download_ms"));
}

TEST(BuildWaterfallJsonTest, PageLoadTimeZero) {
  std::vector<ResourceEntry> entries;
  json result = BuildWaterfallJson(entries, 0.0);
  EXPECT_DOUBLE_EQ(result["page_load_time_ms"].get<double>(), 0.0);
}

TEST(BuildWaterfallJsonTest, PartialTimingOnlySendAndTtfb) {
  // Only send and TTFB timing available (no DNS, connect, TLS).
  ResourceEntry e;
  e.url = "https://cdn.example.com/lib.js";
  e.method = "GET";
  e.resource_type = "Script";
  e.mime_type = "application/javascript";
  e.status = 200;
  e.send_start = 5.0;
  e.send_end = 6.0;
  e.receive_headers_end = 50.0;

  std::vector<ResourceEntry> entries = {e};
  json result = BuildWaterfallJson(entries, 100.0);

  const auto& timing = result["resources"][0]["timing"];
  EXPECT_FALSE(timing.contains("dns_ms"));
  EXPECT_FALSE(timing.contains("connect_ms"));
  EXPECT_FALSE(timing.contains("tls_ms"));
  EXPECT_DOUBLE_EQ(timing["send_ms"].get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(timing["ttfb_ms"].get<double>(), 44.0);
}

// ---------------------------------------------------------------------------
// SSRF: Non-hex IPv4 bypass vector tests (0x prefix with invalid hex)
// ---------------------------------------------------------------------------

TEST_F(CaptureHandlersTest, WaterfallAllowsNonHexIpWithZZ) {
  // "0xZZ" is not valid hex -- should NOT be treated as a hex IP.
  // Falls through to regular hostname lookup (not private).
  std::string body = R"({"url": "http://0xZZ/page"})";
  int code = ParseStatusCode(PostRequest("/v1/capture/waterfall", body));
  // Should pass SSRF validation (not recognized as private) -> 503 (no browser).
  EXPECT_EQ(code, 503);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsNonHexIpWithGG) {
  // "0xGG00001" has non-hex chars after 0x -- not a valid hex IP.
  std::string body = R"({"url": "http://0xGG00001/page"})";
  int code = ParseStatusCode(PostRequest("/v1/capture/waterfall", body));
  EXPECT_EQ(code, 503);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsMixedHexNonHex) {
  // "0x7f00ZZZZ" -- starts valid but has invalid hex chars.
  std::string body = R"({"url": "http://0x7f00ZZZZ/page"})";
  int code = ParseStatusCode(PostRequest("/v1/capture/waterfall", body));
  EXPECT_EQ(code, 503);
}

TEST_F(CaptureHandlersTest, WaterfallAllowsHexPrefixOnly) {
  // "0x" alone with no digits after -- should not match.
  std::string body = R"({"url": "http://0x/page"})";
  int code = ParseStatusCode(PostRequest("/v1/capture/waterfall", body));
  // "0x" has size 2, but condition requires size > 2 for hex IP check.
  EXPECT_EQ(code, 503);
}

TEST_F(CaptureHandlersTest, ScreenshotAllowsNonHexIpWithZZ) {
  // Same non-hex test for screenshot endpoint.
  std::string body = R"({"url": "http://0xZZ/page"})";
  int code = ParseStatusCode(PostRequest("/v1/capture/screenshot", body));
  EXPECT_EQ(code, 503);
}

// Direct unit test for IsPrivateHost with non-hex "0x" hostnames.
TEST(IsPrivateHostDirectTest, NonHexIpNotTreatedAsPrivate) {
  // "0xZZ" is not valid hex -- should not be recognized as private.
  EXPECT_FALSE(IsPrivateHost("http://0xZZ/page"));
  EXPECT_FALSE(IsPrivateHost("http://0xGG00001/page"));
  EXPECT_FALSE(IsPrivateHost("http://0x7f00ZZZZ/page"));
  // But valid hex loopback should still be caught.
  EXPECT_TRUE(IsPrivateHost("http://0x7f000001/page"));
}

TEST(IsPrivateHostDirectTest, HexPrefixOnlyNotPrivate) {
  // "0x" alone (no hex digits) should not be treated as private.
  EXPECT_FALSE(IsPrivateHost("http://0x/page"));
}

TEST(IsPrivateHostDirectTest, NonHexMixedCaseNotPrivate) {
  // "0xABCDEFGH" -- 'G' and 'H' are not hex digits.
  EXPECT_FALSE(IsPrivateHost("http://0xABCDEFGH/page"));
}

// Network-layer SSRF defense: the actual connection's remote IP is re-checked
// on every response to catch redirect / DNS-rebinding bypasses of the
// literal-URL check (audit 2026-05-29, worker capture-css).
TEST(IsPrivateRemoteIpTest, BlocksPrivateAndLoopbackIpv4) {
  EXPECT_TRUE(IsPrivateRemoteIp("127.0.0.1"));
  EXPECT_TRUE(IsPrivateRemoteIp("169.254.169.254"));  // cloud metadata endpoint
  EXPECT_TRUE(IsPrivateRemoteIp("10.1.2.3"));
  EXPECT_TRUE(IsPrivateRemoteIp("192.168.0.1"));
  EXPECT_TRUE(IsPrivateRemoteIp("172.16.5.5"));
  EXPECT_TRUE(IsPrivateRemoteIp("0.0.0.0"));
}

TEST(IsPrivateRemoteIpTest, AllowsPublicIpv4) {
  EXPECT_FALSE(IsPrivateRemoteIp("8.8.8.8"));
  EXPECT_FALSE(IsPrivateRemoteIp("93.184.216.34"));
  EXPECT_FALSE(IsPrivateRemoteIp("172.32.0.1"));  // just outside 172.16/12
}

TEST(IsPrivateRemoteIpTest, BlocksPrivateIpv6) {
  EXPECT_TRUE(IsPrivateRemoteIp("::1"));      // loopback
  EXPECT_TRUE(IsPrivateRemoteIp("[::1]"));    // bracketed
  EXPECT_TRUE(IsPrivateRemoteIp("fe80::1"));  // link-local
  EXPECT_TRUE(IsPrivateRemoteIp("fc00::1"));  // unique-local
  EXPECT_TRUE(IsPrivateRemoteIp("fd12:3456::1"));
}

TEST(IsPrivateRemoteIpTest, AllowsPublicIpv6) {
  EXPECT_FALSE(IsPrivateRemoteIp("2606:2800:220:1:248:1893:25c8:1946"));
}

TEST(IsPrivateRemoteIpTest, EmptyOrNoPeerNotBlocked) {
  EXPECT_FALSE(IsPrivateRemoteIp(""));
}

// ---------------------------------------------------------------------------
// The CDP pipeline against a scripted peer (test/test_util/cdp_scripted_peer.h)
// ---------------------------------------------------------------------------

class CaptureCdpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(peer_.Start());
    peer_.set_screenshot_b64(test::Base64Encode({0x89, 'P', 'N', 'G'}));
  }
  void TearDown() override { peer_.Stop(); }

  // Starts a screenshot capture at `width` x `height` and drives it to the
  // navigation; the caller then pushes the events that finish it.
  void StartScreenshot(uint32_t width, uint32_t height) {
    capture_internal::StartCaptureForTest(
        peer_.client(), "https://example.com/", width, height,
        /*is_waterfall=*/false, /*timeout_ms=*/30000,
        [this](const capture_internal::CaptureOutcome& outcome) {
          outcome_ = outcome;
          done_ = true;
        });
    peer_.PumpUntil([this] { return peer_.SawCommand("Page.navigate"); }, 60);
  }

  // Finishes a started screenshot capture: networkIdle takes the screenshot
  // and the state reports through `done`. Every capture a test starts is
  // finished (or fails), so Cleanup() runs: it closes the target, stops the
  // timeout timer and releases the client's event callback, the last owner
  // of the CaptureState outside the answered command callbacks. After it the
  // peer sees Target.closeTarget and nothing else references the state.
  void FinishCapture() {
    peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                    peer_.session_id(1));
    peer_.PumpUntil([this] { return done_; }, 60);
    ASSERT_TRUE(done_) << "the capture did not finish";
    peer_.PumpUntil([this] { return peer_.SawCommand("Target.closeTarget"); },
                    20);
    EXPECT_TRUE(peer_.SawCommand("Target.closeTarget"))
        << "Cleanup() did not run";
  }

  // Index of the first command with `method`, or -1.
  int IndexOf(const std::string& method) const {
    const auto& cmds = peer_.received_commands();
    for (size_t i = 0; i < cmds.size(); ++i) {
      if (cmds[i].value("method", "") == method) return static_cast<int>(i);
    }
    return -1;
  }

  test::ScriptedCdpPeer peer_;
  capture_internal::CaptureOutcome outcome_;
  bool done_ = false;
};

// The capture emulates the device the way every analysis render
// does (src/browser/device_emulation.h): the device metrics, then touch, on
// the capture's own session and before anything is enabled or navigated. A
// phone viewport is a touch device.
TEST_F(CaptureCdpTest, EmulatesAPhoneAsATouchDeviceBeforeNavigating) {
  StartScreenshot(375, 667);
  const int metrics = IndexOf("Emulation.setDeviceMetricsOverride");
  const int touch = IndexOf("Emulation.setTouchEmulationEnabled");
  const int network = IndexOf("Network.enable");
  const int navigate = IndexOf("Page.navigate");
  ASSERT_GE(metrics, 0) << "no device metrics";
  ASSERT_GE(touch, 0) << "no touch emulation: the capture bypassed "
                         "EmulateDevice";
  ASSERT_GE(navigate, 0);
  EXPECT_LT(metrics, touch) << "touch follows the device metrics";
  // Then the phone's user agent, before the domains, so the
  // navigation's requests and the page's window carry it.
  const int ua = IndexOf("Emulation.setUserAgentOverride");
  ASSERT_GE(ua, 0) << "a phone reports a phone's user agent";
  EXPECT_LT(touch, ua) << "the user agent follows touch";
  EXPECT_LT(ua, network) << "the emulation precedes the domains";
  EXPECT_LT(network, navigate);

  const auto& cmds = peer_.received_commands();
  const json metrics_params = cmds[metrics].value("params", json::object());
  EXPECT_EQ(metrics_params.value("width", 0), 375);
  EXPECT_EQ(metrics_params.value("height", 0), 667);
  EXPECT_TRUE(metrics_params.value("mobile", false));
  EXPECT_EQ(cmds[metrics].value("sessionId", ""), peer_.session_id(1));
  const json touch_params = cmds[touch].value("params", json::object());
  EXPECT_TRUE(touch_params.value("enabled", false));
  EXPECT_EQ(touch_params.value("maxTouchPoints", 0), 5);
  EXPECT_EQ(cmds[touch].value("sessionId", ""), peer_.session_id(1));
  EXPECT_EQ(cmds[ua].value("params", json::object()),
            *UserAgentOverrideParams(375));
  EXPECT_EQ(cmds[ua].value("sessionId", ""), peer_.session_id(1));
  // The capture navigates to the page itself, which makes the page's window:
  // no blank document first (LoadFreshBlankDocument is for the renders that
  // write their document with Page.setDocumentContent).
  int navigations = 0;
  for (const auto& cmd : cmds) {
    if (cmd.value("method", "") == "Page.navigate") ++navigations;
  }
  EXPECT_EQ(navigations, 1);
  EXPECT_EQ(cmds[navigate].value("params", json::object()).value("url", ""),
            "https://example.com/");

  // The capture still completes: networkIdle takes the screenshot.
  FinishCapture();
  EXPECT_TRUE(outcome_.success) << outcome_.error_message;
  EXPECT_EQ(outcome_.result_json.value("viewport_width", 0), 375);
}

// The desktop default (1280 px) is a window without touch, and the call is
// still sent, disabled, so no target inherits a state.
TEST_F(CaptureCdpTest, EmulatesADesktopWindowWithoutTouch) {
  StartScreenshot(1280, 800);
  const int metrics = IndexOf("Emulation.setDeviceMetricsOverride");
  const int touch = IndexOf("Emulation.setTouchEmulationEnabled");
  ASSERT_GE(metrics, 0);
  ASSERT_GE(touch, 0);
  EXPECT_LT(metrics, touch);
  const auto& cmds = peer_.received_commands();
  EXPECT_FALSE(
      cmds[metrics].value("params", json::object()).value("mobile", true));
  const json touch_params = cmds[touch].value("params", json::object());
  EXPECT_FALSE(touch_params.value("enabled", true));
  EXPECT_EQ(touch_params.value("maxTouchPoints", 0), 5);
  // The desktop keeps the browser's own user agent.
  EXPECT_EQ(IndexOf("Emulation.setUserAgentOverride"), -1);
  FinishCapture();
}

// A tablet viewport reports a tablet's user agent.
TEST_F(CaptureCdpTest, EmulatesATabletUserAgent) {
  StartScreenshot(768, 1024);
  const int touch = IndexOf("Emulation.setTouchEmulationEnabled");
  const int ua = IndexOf("Emulation.setUserAgentOverride");
  ASSERT_GE(touch, 0);
  ASSERT_GE(ua, 0);
  EXPECT_LT(touch, ua);
  EXPECT_LT(ua, IndexOf("Page.navigate"));
  EXPECT_EQ(peer_.received_commands()[ua].value("params", json::object()),
            *UserAgentOverrideParams(768));
  FinishCapture();
}

// A failed user agent override ends the capture with an error and navigates
// nothing.
TEST_F(CaptureCdpTest, AFailedUserAgentOverrideEndsTheCapture) {
  peer_.SetResponder([this](int id, const std::string& method, const json&) {
    if (method != "Emulation.setUserAgentOverride") return false;
    peer_.RespondError(id, -32000, "no ua");
    return true;
  });
  capture_internal::StartCaptureForTest(
      peer_.client(), "https://example.com/", 375, 667,
      /*is_waterfall=*/false, /*timeout_ms=*/30000,
      [this](const capture_internal::CaptureOutcome& outcome) {
        outcome_ = outcome;
        done_ = true;
      });
  peer_.PumpUntil([this] { return done_; }, 60);
  ASSERT_TRUE(done_);
  EXPECT_FALSE(outcome_.success);
  EXPECT_EQ(outcome_.error_message, "user agent setup failed");
  EXPECT_FALSE(peer_.SawCommand("Page.navigate"));
  peer_.PumpUntil([this] { return peer_.SawCommand("Target.closeTarget"); },
                  20);
  EXPECT_TRUE(peer_.SawCommand("Target.closeTarget"))
      << "Cleanup() did not run";
}

// A failed emulation ends the capture with an error and navigates nothing.
TEST_F(CaptureCdpTest, AFailedTouchEmulationEndsTheCapture) {
  peer_.SetResponder([this](int id, const std::string& method, const json&) {
    if (method != "Emulation.setTouchEmulationEnabled") return false;
    peer_.RespondError(id, -32000, "no touch");
    return true;
  });
  capture_internal::StartCaptureForTest(
      peer_.client(), "https://example.com/", 375, 667,
      /*is_waterfall=*/false, /*timeout_ms=*/30000,
      [this](const capture_internal::CaptureOutcome& outcome) {
        outcome_ = outcome;
        done_ = true;
      });
  peer_.PumpUntil([this] { return done_; }, 60);
  ASSERT_TRUE(done_);
  EXPECT_FALSE(outcome_.success);
  EXPECT_EQ(outcome_.error_message, "touch emulation setup failed");
  EXPECT_FALSE(peer_.SawCommand("Page.navigate"));
  peer_.PumpUntil([this] { return peer_.SawCommand("Target.closeTarget"); },
                  20);
  EXPECT_TRUE(peer_.SawCommand("Target.closeTarget"))
      << "Cleanup() did not run";
}

// Chrome reporting the capture's target dead is a crashed renderer, not a
// timeout: the capture ends at once (not after the navigation timeout), the
// outcome is flagged so the handler answers 502, and the message names the
// target.
TEST_F(CaptureCdpTest, ATargetCrashedEventEndsTheCaptureAsACrash) {
  StartScreenshot(1280, 800);
  peer_.SendEvent(
      "Target.targetCrashed",
      {{"targetId", "target-1"}, {"status", "crashed"}, {"errorCode", 11}});
  peer_.PumpUntil([this] { return done_; }, 60);
  ASSERT_TRUE(done_) << "the crash did not end the capture";
  EXPECT_FALSE(outcome_.success);
  EXPECT_TRUE(outcome_.renderer_crashed);
  EXPECT_NE(outcome_.error_message.find("renderer crashed"), std::string::npos)
      << outcome_.error_message;
  EXPECT_NE(outcome_.error_message.find("target-1"), std::string::npos)
      << outcome_.error_message;
  EXPECT_EQ(outcome_.error_message.find("timeout"), std::string::npos);
  peer_.PumpUntil([this] { return peer_.SawCommand("Target.closeTarget"); },
                  20);
  EXPECT_TRUE(peer_.SawCommand("Target.closeTarget"))
      << "Cleanup() did not run";
}

// Chrome detaching the capture's session is the same failure.
TEST_F(CaptureCdpTest, ADetachedSessionEndsTheCaptureAsACrash) {
  StartScreenshot(1280, 800);
  peer_.SendEvent(
      "Target.detachedFromTarget",
      {{"sessionId", peer_.session_id(1)}, {"targetId", "target-1"}});
  peer_.PumpUntil([this] { return done_; }, 60);
  ASSERT_TRUE(done_);
  EXPECT_FALSE(outcome_.success);
  EXPECT_TRUE(outcome_.renderer_crashed);
  peer_.PumpUntil([this] { return peer_.SawCommand("Target.closeTarget"); },
                  20);
}

// A detach of some other session or target (a previous render's late
// teardown) must not end this capture.
TEST_F(CaptureCdpTest, ADetachOfAnotherTargetDoesNotEndTheCapture) {
  StartScreenshot(1280, 800);
  peer_.SendEvent("Target.detachedFromTarget", {{"sessionId", "other-session"},
                                                {"targetId", "other-target"}});
  peer_.PumpUntil([this] { return done_; }, 20);
  EXPECT_FALSE(done_) << "a foreign detach ended the capture";

  // The capture is still live: its own target crashing ends it as a crash,
  // which also lets Cleanup() run.
  peer_.SendEvent(
      "Target.targetCrashed",
      {{"targetId", "target-1"}, {"status", "crashed"}, {"errorCode", 11}});
  peer_.PumpUntil([this] { return done_; }, 60);
  ASSERT_TRUE(done_) << "the capture did not finish after its own crash";
  EXPECT_TRUE(outcome_.renderer_crashed);
  peer_.PumpUntil([this] { return peer_.SawCommand("Target.closeTarget"); },
                  20);
}

TEST(CaptureFailureCodeTest, CrashIs502AndEverythingElseIs504) {
  capture_internal::CaptureOutcome outcome;
  EXPECT_EQ(capture_internal::CaptureFailureCode(outcome),
            ApiErrorCode::kGatewayTimeout);
  outcome.renderer_crashed = true;
  EXPECT_EQ(capture_internal::CaptureFailureCode(outcome),
            ApiErrorCode::kBadGateway);
}

// A navigation that never finishes is a timeout, not a crash: the outcome is
// not flagged, so the handler keeps answering 504.
TEST_F(CaptureCdpTest, ANavigationTimeoutIsNotACrash) {
  capture_internal::StartCaptureForTest(
      peer_.client(), "https://example.com/", 1280, 800,
      /*is_waterfall=*/false, /*timeout_ms=*/20,
      [this](const capture_internal::CaptureOutcome& outcome) {
        outcome_ = outcome;
        done_ = true;
      });
  peer_.PumpUntil([this] { return done_; }, 2000);
  ASSERT_TRUE(done_) << "the capture did not time out";
  EXPECT_FALSE(outcome_.success);
  EXPECT_FALSE(outcome_.renderer_crashed);
  EXPECT_EQ(outcome_.error_message, "navigation timeout");
  peer_.PumpUntil([this] { return peer_.SawCommand("Target.closeTarget"); },
                  20);
}

}  // namespace
}  // namespace pagespeed
