// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// HTTP Server Unit Tests
//
// Tests the core HTTP server infrastructure:
// - Request/response types (serialization, error format)
// - URL decoding and query string parsing
// - Route matching
// - Authentication middleware
// - CORS handling
// - Integration test with live TCP server

#include "src/worker/http_server.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "test/test_util/tcp_client.h"

namespace pagespeed {
namespace {

// =============================================================================
// URL Decoding
// =============================================================================

TEST(UrlDecodeTest, NoEncoding) {
  EXPECT_EQ(UrlDecode("hello"), "hello");
  EXPECT_EQ(UrlDecode("/v1/health"), "/v1/health");
}

TEST(UrlDecodeTest, PercentEncoding) {
  EXPECT_EQ(UrlDecode("hello%20world"), "hello world");
  EXPECT_EQ(UrlDecode("%2Fpath%2Fto%2Ffile"), "/path/to/file");
  EXPECT_EQ(UrlDecode("100%25"), "100%");
}

TEST(UrlDecodeTest, PlusAsSpace) {
  // UrlDecode (query string): + becomes space.
  EXPECT_EQ(UrlDecode("hello+world"), "hello world");
  EXPECT_EQ(UrlDecode("a+b+c"), "a b c");
}

TEST(UrlDecodeTest, PathPlusLiteral) {
  // UrlDecodePath (URL path): + stays literal.
  EXPECT_EQ(UrlDecodePath("hello+world"), "hello+world");
  EXPECT_EQ(UrlDecodePath("a+b+c"), "a+b+c");
  // Percent-encoding still works.
  EXPECT_EQ(UrlDecodePath("hello%20world"), "hello world");
}

TEST(UrlDecodeTest, MixedEncoding) {
  EXPECT_EQ(UrlDecode("q=hello+world%21"), "q=hello world!");
}

TEST(UrlDecodeTest, InvalidPercentSequence) {
  EXPECT_EQ(UrlDecode("100%"), "100%");
  EXPECT_EQ(UrlDecode("100%2"), "100%2");
  EXPECT_EQ(UrlDecode("100%ZZ"), "100%ZZ");
}

TEST(UrlDecodeTest, EmptyString) { EXPECT_EQ(UrlDecode(""), ""); }

// =============================================================================
// HttpRequest
// =============================================================================

TEST(HttpRequestTest, QueryStringParsing) {
  HttpRequest req;
  req.query_string = "url=https%3A%2F%2Fexample.com&hostname=example.com";
  req.ParseQueryString();

  EXPECT_EQ(req.QueryParam("url"), "https://example.com");
  EXPECT_EQ(req.QueryParam("hostname"), "example.com");
  EXPECT_EQ(req.QueryParam("missing"), "");
}

TEST(HttpRequestTest, QueryStringEmpty) {
  HttpRequest req;
  req.query_string = "";
  req.ParseQueryString();
  EXPECT_TRUE(req.query_params.empty());
}

TEST(HttpRequestTest, QueryStringNoValue) {
  HttpRequest req;
  req.query_string = "flag&key=value";
  req.ParseQueryString();

  EXPECT_EQ(req.QueryParam("flag"), "");
  EXPECT_EQ(req.QueryParam("key"), "value");
}

TEST(HttpRequestTest, HeaderLookupCaseInsensitive) {
  HttpRequest req;
  req.headers["content-type"] = "application/json";
  req.headers["authorization"] = "Bearer token123";

  EXPECT_EQ(req.Header("Content-Type"), "application/json");
  EXPECT_EQ(req.Header("AUTHORIZATION"), "Bearer token123");
  EXPECT_EQ(req.Header("X-Missing"), "");
}

// =============================================================================
// HttpResponse
// =============================================================================

TEST(HttpResponseTest, BasicJsonResponse) {
  auto resp = HttpResponse().Status(200).Json(R"({"status":"ok"})");
  std::string wire = resp.Serialize();

  EXPECT_TRUE(wire.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_NE(wire.find("Content-Type: application/json"), std::string::npos);
  EXPECT_NE(wire.find("Content-Length: 15"), std::string::npos);
  EXPECT_TRUE(wire.ends_with(R"({"status":"ok"})"));
}

TEST(HttpResponseTest, ConnectionCloseByDefault) {
  std::string wire = HttpResponse().Serialize();
  EXPECT_NE(wire.find("Connection: close"), std::string::npos);
}

TEST(HttpResponseTest, ExplicitContentLength) {
  auto resp = HttpResponse().SetHeader("Content-Length", "42").Body("test");
  std::string wire = resp.Serialize();

  // Should use explicit Content-Length, not auto-computed.
  EXPECT_NE(wire.find("Content-Length: 42"), std::string::npos);
  // Should NOT have a second Content-Length.
  auto first = wire.find("Content-Length:");
  auto second = wire.find("Content-Length:", first + 1);
  EXPECT_EQ(second, std::string::npos);
}

TEST(HttpResponseTest, StatusCodes) {
  EXPECT_EQ(HttpResponse().Status(404).status_code(), 404);
  EXPECT_EQ(HttpResponse().Status(500).status_code(), 500);
}

// =============================================================================
// Error Response Format (C2 resolution)
// =============================================================================

TEST(ErrorResponseTest, StandardFormat) {
  auto resp =
      HttpResponse::Error(ApiErrorCode::kBadRequest, "Invalid parameter");
  EXPECT_EQ(resp.status_code(), 400);

  const std::string& body = resp.body();
  EXPECT_NE(body.find("\"error\""), std::string::npos);
  EXPECT_NE(body.find(R"("code":"BAD_REQUEST")"), std::string::npos);
  EXPECT_NE(body.find(R"("message":"Invalid parameter")"), std::string::npos);
}

TEST(ErrorResponseTest, WithDetails) {
  auto resp = HttpResponse::Error(ApiErrorCode::kPayloadTooLarge,
                                  "URL too long", "{\"max_length\":8192}");
  EXPECT_EQ(resp.status_code(), 413);

  const std::string& body = resp.body();
  EXPECT_NE(body.find(R"("details":{"max_length":8192})"), std::string::npos);
}

TEST(ErrorResponseTest, MessageEscaping) {
  auto resp =
      HttpResponse::Error(ApiErrorCode::kBadRequest, "field \"name\" invalid");
  const std::string& body = resp.body();
  EXPECT_NE(body.find(R"(field \"name\" invalid)"), std::string::npos);
}

TEST(ErrorResponseTest, ControlCharEscaping) {
  auto resp =
      HttpResponse::Error(ApiErrorCode::kBadRequest, "line1\r\nline2\ttab");
  const std::string& body = resp.body();
  EXPECT_NE(body.find("\\r"), std::string::npos);
  EXPECT_NE(body.find("\\n"), std::string::npos);
  EXPECT_NE(body.find("\\t"), std::string::npos);
  // Raw control characters should NOT appear in the JSON.
  EXPECT_EQ(body.find('\r'), std::string::npos);
  EXPECT_EQ(body.find('\t'), std::string::npos);
}

TEST(ErrorResponseTest, AllErrorCodes) {
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kBadRequest), 400);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kUnauthorized), 401);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kForbidden), 403);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kNotFound), 404);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kMethodNotAllowed), 405);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kPayloadTooLarge), 413);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kUnsupportedMediaType), 415);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kTooManyRequests), 429);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kInternalError), 500);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kServiceUnavailable), 503);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kGatewayTimeout), 504);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kBadGateway), 502);
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kBadGateway), "BAD_GATEWAY");

  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kBadRequest), "BAD_REQUEST");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kNotFound), "NOT_FOUND");
}

// =============================================================================
// Integration: Live HTTP Server
// =============================================================================

class HttpServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    config_.port = 0;  // Ephemeral port
    config_.request_timeout_ms = 5000;
    // The base fixture is the deliberately-unauthenticated
    // server.  Sub-fixtures that set an auth_token exercise the credential
    // gate; the fail-closed behaviour of an unset token has its own tests
    // (HttpServerFailClosedTest).
    config_.allow_unauthenticated = true;
    handler_ = std::make_unique<NullMessageHandler>();
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  }

  void TearDown() override {
    if (loop_thread_.joinable()) {
      StopLoopThread();
    }
    server_->Stop();
    // Drain the loop to process the close callback BEFORE
    // destroying the server (which owns the uv_tcp_t handle).
    uv_run(loop_, UV_RUN_DEFAULT);
    server_.reset();
    uv_loop_close(loop_);
    delete loop_;
    loop_ = nullptr;
  }

  // Start the event loop in a background thread.
  void StartLoopThread() {
    loop_running_ = true;
    loop_thread_ = std::thread([this] {
      while (loop_running_) {
        uv_run(loop_, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  // Stop the background loop thread.
  void StopLoopThread() {
    loop_running_ = false;
    if (loop_thread_.joinable()) {
      loop_thread_.join();
    }
  }

  // Send an HTTP request and return the raw response.
  // Requires the loop thread to be running.
  // Automatically injects "Connection: close" if no Connection header is
  // present, so the server closes the connection after responding and the
  // read loop terminates via EOF.
  std::string SendRequest(const std::string& request) {
    std::string wire = test::InjectConnectionClose(request);
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, 2);
    if (sock < 0) return "";

    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    (void)sent;

    // Brief delay to let the libuv loop pick up the connection and process
    // the request before we start reading.  On Linux/GCC (epoll) the
    // UV_RUN_NOWAIT loop may not have dispatched the read callback yet,
    // leading to an empty response.
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

  uv_loop_t* loop_ = nullptr;
  HttpServerConfig config_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
};

TEST_F(HttpServerTest, StartAndStop) {
  ASSERT_TRUE(server_->Start());
  EXPECT_GT(server_->bound_port(), 0);
  EXPECT_EQ(server_->active_connections(), 0);
}

TEST_F(HttpServerTest, RouteMatching) {
  server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
    return HttpResponse().Json("{\"status\":\"ok\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find(R"({"status":"ok"})"), std::string::npos);
}

TEST_F(HttpServerTest, NotFound) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp =
      SendRequest("GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos);
  EXPECT_NE(resp.find("NOT_FOUND"), std::string::npos);
}

TEST_F(HttpServerTest, MethodNotAllowed) {
  server_->AddRoute("GET", "/v1/config", [](const HttpRequest&) {
    return HttpResponse().Json("{}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp =
      SendRequest("DELETE /v1/config HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("405"), std::string::npos);
  EXPECT_NE(resp.find("METHOD_NOT_ALLOWED"), std::string::npos);
}

TEST_F(HttpServerTest, PrefixRouting) {
  server_->AddRoute("GET", "/console/*", [](const HttpRequest& req) {
    return HttpResponse().Body("path=" + req.path);
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /console/index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("path=/console/index.html"), std::string::npos);
}

TEST_F(HttpServerTest, QueryStringParsed) {
  server_->AddRoute("GET", "/v1/test", [](const HttpRequest& req) {
    return HttpResponse().Json("{\"url\":\"" +
                               std::string(req.QueryParam("url")) + "\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /v1/test?url=hello%20world HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("hello world"), std::string::npos);
}

TEST_F(HttpServerTest, PostWithBody) {
  server_->AddRoute("POST", "/v1/test", [](const HttpRequest& req) {
    return HttpResponse().Json("{\"body\":\"" + req.body + "\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string body = R"({"key":"value"})";
  std::string request =
      "POST /v1/test HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "X-Requested-With: XMLHttpRequest\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

TEST_F(HttpServerTest, PathTraversalRejected) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /console/../etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("Invalid path"), std::string::npos);
}

TEST_F(HttpServerTest, NullByteInPathRejected) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /console/evil%00.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("Invalid path"), std::string::npos);
}

TEST_F(HttpServerTest, MalformedRequestReturns400) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest("NOT A VALID HTTP REQUEST\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(HttpServerTest, BodySizeLimitEnforced) {
  server_->AddRoute("POST", "/v1/test", [](const HttpRequest& req) {
    return HttpResponse().Json("{\"size\":" + std::to_string(req.body.size()) +
                               "}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Send a body larger than max_request_body (64KB default).
  std::string big_body(70000, 'X');
  std::string request =
      "POST /v1/test HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: " +
      std::to_string(big_body.size()) + "\r\n\r\n" + big_body;

  std::string resp = SendRequest(request);
  // Should get either 413 (body too large) or 400 (parse error from HPE_USER).
  bool got_413 = resp.find("413") != std::string::npos;
  bool got_400 = resp.find("400") != std::string::npos;
  EXPECT_TRUE(got_413 || got_400)
      << "Expected 413 or 400, got: " << resp.substr(0, 80);
}

TEST_F(HttpServerTest, PlusInPathKeptLiteral) {
  server_->AddRoute("GET", "/v1/a+b", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp =
      SendRequest("GET /v1/a+b HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

// =============================================================================
// Authentication
// =============================================================================

class HttpServerAuthTest : public HttpServerTest {
 protected:
  void SetUp() override {
    HttpServerTest::SetUp();
    config_.auth_token = "secret-token-123";
    // Rebuild server with auth config.
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());

    server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
      return HttpResponse().Json("{\"status\":\"ok\"}");
    });
    server_->AddRoute("GET", "/v1/stats", [](const HttpRequest&) {
      return HttpResponse().Json("{\"stats\":true}");
    });
    server_->AddRoute("PATCH", "/v1/config", [](const HttpRequest&) {
      return HttpResponse().Json("{\"applied\":{}}");
    });
  }
};

TEST_F(HttpServerAuthTest, HealthBypassesAuth) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

TEST_F(HttpServerAuthTest, MissingToken) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("401"), std::string::npos);
  EXPECT_NE(resp.find("UNAUTHORIZED"), std::string::npos);
}

TEST_F(HttpServerAuthTest, InvalidScheme) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /v1/stats HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Authorization: Basic abc123\r\n\r\n");
  EXPECT_NE(resp.find("401"), std::string::npos);
}

TEST_F(HttpServerAuthTest, WrongToken) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /v1/stats HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Authorization: Bearer wrong-token\r\n\r\n");
  EXPECT_NE(resp.find("403"), std::string::npos);
}

TEST_F(HttpServerAuthTest, ValidToken) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /v1/stats HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Authorization: Bearer secret-token-123\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("{\"stats\":true}"), std::string::npos);
}

TEST_F(HttpServerAuthTest, ReadOpenMode) {
  // Reconfigure with read_open.
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.read_open = true;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute(
      "GET", "/v1/stats",
      [](const HttpRequest&) {
        return HttpResponse().Json("{\"stats\":true}");
      },
      RouteAuth::kReadOpenOk);
  server_->AddRoute("PATCH", "/v1/config", [](const HttpRequest&) {
    return HttpResponse().Json("{\"applied\":{}}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // GET should work without auth.
  std::string resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  // PATCH should still require auth.
  resp = SendRequest(
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: 2\r\n\r\n{}");
  EXPECT_NE(resp.find("401"), std::string::npos);
}

// Regression for issue #1: in read-open mode HEAD must follow the same auth
// rule as GET (allowed without a Bearer token). Monitoring tools (UptimeRobot,
// curl health checks) use HEAD and were getting false 401s while GET to the
// same endpoint succeeded. /v1/stats is registered as a real GET route, so this
// exercises the production routing/auth path — HEAD is served by the GET route
// with the body omitted.
TEST_F(HttpServerAuthTest, ReadOpenHeadFollowsGet) {
  // Reconfigure with read_open, registering only the real GET /v1/stats route.
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.read_open = true;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute(
      "GET", "/v1/stats",
      [](const HttpRequest&) {
        return HttpResponse().Json("{\"stats\":true}");
      },
      RouteAuth::kReadOpenOk);
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // GET still works without a token (baseline — confirms read-open is active).
  std::string get_resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(get_resp.find("200 OK"), std::string::npos);
  EXPECT_NE(get_resp.find("{\"stats\":true}"), std::string::npos);

  // HEAD to the same endpoint must NOT 401 — it follows the GET bypass.
  // Before the fix this returned 401 (read-open only matched "GET").
  std::string head_resp =
      SendRequest("HEAD /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(head_resp.find("200 OK"), std::string::npos);
  EXPECT_EQ(head_resp.find("401"), std::string::npos);
  EXPECT_EQ(head_resp.find("405"), std::string::npos);
  // HEAD carries the GET Content-Length but no body.
  EXPECT_NE(head_resp.find("Content-Length: 14"), std::string::npos);
  EXPECT_EQ(head_resp.find("{\"stats\":true}"), std::string::npos);
}

// Parity check: when read-open is DISABLED, HEAD requires a token just like GET.
TEST_F(HttpServerAuthTest, HeadRequiresTokenWhenReadOpenDisabled) {
  // config_.auth_token is set and read_open defaults to false in this fixture.
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // GET without a token is rejected (existing MissingToken behavior).
  std::string get_resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(get_resp.find("401"), std::string::npos);

  // HEAD without a token is rejected the same way — no bypass when read-open
  // is off.
  std::string head_resp =
      SendRequest("HEAD /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(head_resp.find("401"), std::string::npos);
}

// A route registered with RouteAuth::kReadOpenOk opts out of the token
// under --api-read-open on TCP, for GET and HEAD alike, while a route that
// does NOT opt in (the default, RouteAuth::kToken) stays closed.
TEST_F(HttpServerAuthTest, ReadOpenKeepsTokenOnlyRoutesClosed) {
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.read_open = true;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute(
      "GET", "/v1/stats",
      [](const HttpRequest&) {
        return HttpResponse().Json("{\"stats\":true}");
      },
      RouteAuth::kReadOpenOk);
  server_->AddRoute(
      "GET", "/v1/logs",
      [](const HttpRequest&) {
        return HttpResponse().Json("{\"entries\":[]}");
      },
      RouteAuth::kToken);
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // The allow-listed route: open under read-open (the baseline).
  EXPECT_NE(SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n")
                .find("200 OK"),
            std::string::npos);

  // The token-only route: 401 without a token, and no body leaks.
  std::string get =
      SendRequest("GET /v1/logs HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(get.find("401"), std::string::npos) << get;
  EXPECT_EQ(get.find("entries"), std::string::npos) << get;
  std::string head =
      SendRequest("HEAD /v1/logs HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(head.find("401"), std::string::npos) << head;

  // 403 with a wrong token, the answer with the right one.
  EXPECT_NE(SendRequest("GET /v1/logs HTTP/1.1\r\nHost: localhost\r\n"
                        "Authorization: Bearer wrong-token\r\n\r\n")
                .find("403"),
            std::string::npos);
  std::string ok = SendRequest(
      "GET /v1/logs HTTP/1.1\r\nHost: localhost\r\n"
      "Authorization: Bearer secret-token-123\r\n\r\n");
  EXPECT_NE(ok.find("200 OK"), std::string::npos) << ok;
  EXPECT_NE(ok.find("{\"entries\":[]}"), std::string::npos) << ok;
}

// read-open is an explicit allow-list: a route registered with NO auth
// argument at all (the ordinary way every route but this test's is
// registered) gets the default, RouteAuth::kToken, and therefore still
// needs the bearer token under --api-read-open -- for GET and HEAD alike.
// A route that forgets to opt in does NOT become readable for free.
TEST_F(HttpServerAuthTest, UnmarkedRouteStaysClosedUnderReadOpen) {
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.read_open = true;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  // No RouteAuth argument -- relies entirely on the default.
  server_->AddRoute("GET", "/v1/new-endpoint", [](const HttpRequest&) {
    return HttpResponse().Json("{\"secret\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string get =
      SendRequest("GET /v1/new-endpoint HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(get.find("401"), std::string::npos) << get;
  EXPECT_EQ(get.find("secret"), std::string::npos) << get;

  std::string head =
      SendRequest("HEAD /v1/new-endpoint HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(head.find("401"), std::string::npos) << head;

  // The right token still reaches the handler.
  std::string ok = SendRequest(
      "GET /v1/new-endpoint HTTP/1.1\r\nHost: localhost\r\n"
      "Authorization: Bearer secret-token-123\r\n\r\n");
  EXPECT_NE(ok.find("200 OK"), std::string::npos) << ok;

  // A path matching NO registered route at all is the same "fails closed
  // by default" case: ReadOpenApplies returns false (not true, as it used
  // to for an unmatched path), so CheckAuth asks for the token before
  // routing ever gets a chance to answer 404. The CheckAuth 401 therefore
  // now runs first for an unknown path too -- a visible behaviour change
  // from before this allow-list (previously 404), noted in the report.
  std::string unknown =
      SendRequest("GET /v1/does-not-exist HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(unknown.find("401"), std::string::npos) << unknown;
}

// ReadOpenApplies must answer about the route DispatchRequest would
// actually pick, not "does any kReadOpenOk route happen to match this
// path". Registered first (and so the one dispatch picks for an exact
// match, per DispatchRequest's first-match loop): a specific, token-only
// route. Registered second: a broader allow-listed prefix that ALSO
// matches the same literal path. A scan that checks only "any kReadOpenOk
// match" (ignoring which one dispatch would use) would fail open here.
TEST_F(HttpServerAuthTest, ReadOpenConsultsTheRouteDispatchWouldPick) {
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.read_open = true;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/cache/secret", [](const HttpRequest&) {
    return HttpResponse().Json("{\"secret\":true}");
  });
  server_->AddRoute(
      "GET", "/v1/cache/*",
      [](const HttpRequest&) {
        return HttpResponse().Json("{\"prefix\":true}");
      },
      RouteAuth::kReadOpenOk);
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Dispatch picks the specific route (registered first); auth must follow
  // that same route, not the broader prefix that also happens to match.
  std::string secret =
      SendRequest("GET /v1/cache/secret HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(secret.find("401"), std::string::npos) << secret;
  EXPECT_EQ(secret.find("\"secret\":true"), std::string::npos) << secret;

  // A path only the prefix matches is unaffected and stays open.
  std::string other =
      SendRequest("GET /v1/cache/other HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(other.find("200 OK"), std::string::npos) << other;
  EXPECT_NE(other.find("\"prefix\":true"), std::string::npos) << other;
}

// Transport invariance #1: the unix socket's filesystem-group credential
// does not consult RouteAuth at all (HttpServer::CheckAuth returns early for
// is_pipe()), so a route with no RouteAuth argument is unaffected by the
// read-open default flip -- it stays open there exactly as before, even
// with --api-read-open also set.
#ifndef _WIN32
TEST_F(HttpServerAuthTest, UnmarkedRouteOverSocketUnaffectedByReadOpenDefault) {
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  std::string socket_path =
      absl::StrCat("/tmp/pagespeed-api-test-readopen-", ::getpid(), ".sock");
  ::unlink(socket_path.c_str());
  config_.socket_path = socket_path;
  config_.port = 0;
  config_.read_open = true;  // Irrelevant on the socket transport.
  config_.allow_unauthenticated = false;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/new-endpoint", [](const HttpRequest&) {
    return HttpResponse().Json("{\"secret\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(sock, 0);
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(
      ::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
      0);
  std::string wire = test::InjectConnectionClose(
      "GET /v1/new-endpoint HTTP/1.1\r\nHost: x\r\n\r\n");
  (void)test::SocketWrite(sock, wire.data(), wire.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, static_cast<size_t>(n));
  }
  ::close(sock);
  ::unlink(socket_path.c_str());

  EXPECT_NE(response.find("200"), std::string::npos) << response;
  EXPECT_NE(response.find("\"secret\":true"), std::string::npos) << response;
}
#endif  // !_WIN32

// Transport invariance #2: --api-no-auth (allow_unauthenticated on a
// tokenless server) also returns from CheckAuth before RouteAuth is ever
// consulted, so a route with no RouteAuth argument stays open there too,
// read-open default flip notwithstanding.
TEST_F(HttpServerTest, UnmarkedRouteUnderApiNoAuthUnaffectedByReadOpenDefault) {
  config_.read_open = true;  // Also set, to show it changes nothing here.
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/new-endpoint", [](const HttpRequest&) {
    return HttpResponse().Json("{\"secret\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp =
      SendRequest("GET /v1/new-endpoint HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos) << resp;
}

// Issue #1449: the console's static bundle carries no operational data, so
// GET/HEAD to /console and /console/* load without the bearer token even when
// one is configured.  Every /v1/* data endpoint stays behind the token, and
// the exemption does not extend past the /console path segment.
TEST_F(HttpServerAuthTest, ConsoleStaticsBypassAuth) {
  server_->AddRoute("GET", "/console/*", [](const HttpRequest&) {
    return HttpResponse().ContentType("text/html").Body("<html></html>");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // The bundle itself loads with no Authorization header.
  std::string resp =
      SendRequest("GET /console/app.js HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("<html></html>"), std::string::npos);

  resp = SendRequest("GET /console/ HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  // Bare /console is exempt from auth but matches no route (the bundle is
  // registered as the /console/ prefix), so it 404s rather than 401s.
  resp = SendRequest("GET /console HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos);
  EXPECT_EQ(resp.find("401"), std::string::npos);

  // HEAD follows GET (the read-only sibling), with the body omitted.
  resp = SendRequest("HEAD /console/ HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_EQ(resp.find("401"), std::string::npos);
  EXPECT_EQ(resp.find("<html></html>"), std::string::npos);

  // The exemption stops at the path-segment boundary: "/consoles" is neither
  // "/console" nor "/console/...", so it still requires the token.
  resp = SendRequest("GET /consoles HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("401"), std::string::npos);

  // A data endpoint right next to the exempt set still 401s without a token.
  resp = SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("401"), std::string::npos);
}

// The console exemption is method-scoped: a POST to a console path is not
// accidentally exempted — it still requires the token.
TEST_F(HttpServerAuthTest, ConsolePostRequiresAuth) {
  server_->AddRoute("GET", "/console/*", [](const HttpRequest&) {
    return HttpResponse().ContentType("text/html").Body("<html></html>");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "POST /console/app.js HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 2\r\n\r\n{}");
  EXPECT_NE(resp.find("401"), std::string::npos);
}

// =============================================================================
// CORS
// =============================================================================

class HttpServerCorsTest : public HttpServerTest {
 protected:
  void SetUp() override {
    HttpServerTest::SetUp();
    config_.cors_origin = "http://localhost:3000";
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());

    server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
      return HttpResponse().Json("{\"status\":\"ok\"}");
    });
  }
};

TEST_F(HttpServerCorsTest, PreflightRequest) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "OPTIONS /v1/health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Origin: http://localhost:3000\r\n\r\n");
  EXPECT_NE(resp.find("204"), std::string::npos);
  EXPECT_NE(resp.find("Access-Control-Allow-Methods"), std::string::npos);
}

TEST_F(HttpServerCorsTest, CorsHeadersOnResponse) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /v1/health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Origin: http://localhost:3000\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("Access-Control-Allow-Origin: "
                      "http://localhost:3000"),
            std::string::npos);
}

TEST_F(HttpServerCorsTest, WrongOriginNoCors) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /v1/health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Origin: http://evil.com\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_EQ(resp.find("Access-Control-Allow-Origin"), std::string::npos);
}

// =============================================================================
// HttpStatusText - completeness
// =============================================================================

TEST(HttpStatusTextTest, AllKnownCodes) {
  EXPECT_STREQ(HttpStatusText(200), "OK");
  EXPECT_STREQ(HttpStatusText(204), "No Content");
  EXPECT_STREQ(HttpStatusText(301), "Moved Permanently");
  EXPECT_STREQ(HttpStatusText(304), "Not Modified");
  EXPECT_STREQ(HttpStatusText(400), "Bad Request");
  EXPECT_STREQ(HttpStatusText(401), "Unauthorized");
  EXPECT_STREQ(HttpStatusText(403), "Forbidden");
  EXPECT_STREQ(HttpStatusText(404), "Not Found");
  EXPECT_STREQ(HttpStatusText(405), "Method Not Allowed");
  EXPECT_STREQ(HttpStatusText(413), "Payload Too Large");
  EXPECT_STREQ(HttpStatusText(415), "Unsupported Media Type");
  EXPECT_STREQ(HttpStatusText(429), "Too Many Requests");
  EXPECT_STREQ(HttpStatusText(500), "Internal Server Error");
  EXPECT_STREQ(HttpStatusText(503), "Service Unavailable");
  EXPECT_STREQ(HttpStatusText(504), "Gateway Timeout");
}

TEST(HttpStatusTextTest, UnknownCodesReturnUnknown) {
  EXPECT_STREQ(HttpStatusText(0), "Unknown");
  EXPECT_STREQ(HttpStatusText(418), "Unknown");
  EXPECT_STREQ(HttpStatusText(999), "Unknown");
}

// =============================================================================
// ParseQueryString - edge cases
// =============================================================================

TEST(HttpRequestTest, QueryStringTrailingAmpersand) {
  HttpRequest req;
  req.query_string = "key=value&";
  req.ParseQueryString();
  EXPECT_EQ(req.QueryParam("key"), "value");
}

TEST(HttpRequestTest, QueryStringDuplicateKeys) {
  HttpRequest req;
  req.query_string = "key=first&key=second";
  req.ParseQueryString();
  // Last value wins (unordered_map assignment overwrites).
  EXPECT_EQ(req.QueryParam("key"), "second");
}

TEST(HttpRequestTest, QueryStringOnlyAmpersands) {
  HttpRequest req;
  req.query_string = "&&&";
  req.ParseQueryString();
  // Should not crash.
  EXPECT_EQ(req.QueryParam("anything"), "");
}

TEST(HttpRequestTest, QueryStringEncodedKey) {
  HttpRequest req;
  req.query_string = "na%6De=value";
  req.ParseQueryString();
  EXPECT_EQ(req.QueryParam("name"), "value");
}

// =============================================================================
// UrlDecode - adversarial
// =============================================================================

TEST(UrlDecodeTest, NullBytePercent) {
  std::string result = UrlDecode("hello%00world");
  // Should decode %00 to a null byte.
  EXPECT_EQ(result.size(), 11u);
  EXPECT_EQ(result[5], '\0');
}

TEST(UrlDecodeTest, PathTraversalPercent) {
  // %2F decodes to '/' — verify it's decoded.
  EXPECT_EQ(UrlDecode("%2F"), "/");
  EXPECT_EQ(UrlDecode("..%2F..%2Fetc%2Fpasswd"), "../../etc/passwd");
}

TEST(UrlDecodeTest, LongInput) {
  std::string input(10000, 'a');
  EXPECT_EQ(UrlDecode(input), input);
}

TEST(UrlDecodeTest, AllPercentEncoded) {
  // Every character percent-encoded.
  EXPECT_EQ(UrlDecode("%41%42%43"), "ABC");
}

// =============================================================================
// Coverage gap tests: error code strings, JSON escaping of control chars,
// all ApiErrorCode string mappings
// =============================================================================

TEST(ErrorCodeStringMappingTest, AllErrorCodeStrings) {
  // Verify every ApiErrorCode maps to the correct string.
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kBadRequest), "BAD_REQUEST");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kUnauthorized), "UNAUTHORIZED");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kForbidden), "FORBIDDEN");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kNotFound), "NOT_FOUND");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kMethodNotAllowed),
               "METHOD_NOT_ALLOWED");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kPayloadTooLarge),
               "PAYLOAD_TOO_LARGE");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kUnsupportedMediaType),
               "UNSUPPORTED_MEDIA_TYPE");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kTooManyRequests),
               "TOO_MANY_REQUESTS");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kInternalError),
               "INTERNAL_ERROR");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kServiceUnavailable),
               "SERVICE_UNAVAILABLE");
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kGatewayTimeout),
               "GATEWAY_TIMEOUT");
}

TEST(ErrorCodeStringMappingTest, AllErrorCodeStatuses) {
  // Verify every ApiErrorCode maps to the correct HTTP status.
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kBadRequest), 400);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kUnauthorized), 401);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kForbidden), 403);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kNotFound), 404);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kMethodNotAllowed), 405);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kPayloadTooLarge), 413);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kUnsupportedMediaType), 415);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kTooManyRequests), 429);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kInternalError), 500);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kServiceUnavailable), 503);
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kGatewayTimeout), 504);
}

TEST(JsonEscapingTest, BackspaceAndFormfeed) {
  // Test JSON escaping of backspace (\b) and formfeed (\f).
  // These are at lines 241-244 in http_server.cc.
  auto resp =
      HttpResponse::Error(ApiErrorCode::kBadRequest, "value\b has \f control");
  const std::string& body = resp.body();
  EXPECT_NE(body.find("\\b"), std::string::npos)
      << "Backspace should be escaped as \\b";
  EXPECT_NE(body.find("\\f"), std::string::npos)
      << "Formfeed should be escaped as \\f";
  // Raw control characters should not appear.
  EXPECT_EQ(body.find('\b'), std::string::npos);
  EXPECT_EQ(body.find('\f'), std::string::npos);
}

TEST(JsonEscapingTest, NullByte) {
  // Test JSON escaping of null byte (\\u0000).
  // Exercises the static_cast<unsigned char>(c) < 0x20 path (lines 245-249).
  std::string msg = "null\x00byte";
  // std::string truncates at null, so build it with explicit size.
  msg =
      std::string("null", 4) + std::string("\x00", 1) + std::string("byte", 4);
  auto resp = HttpResponse::Error(ApiErrorCode::kBadRequest, msg);
  const std::string& body = resp.body();
  // Null byte should be escaped as \\u0000.
  EXPECT_NE(body.find("\\u0000"), std::string::npos)
      << "Null byte should be escaped as \\u0000";
}

TEST(JsonEscapingTest, OtherControlChars) {
  // Test JSON escaping of control characters (0x01-0x1F) other than
  // the special ones (\n, \r, \t, \b, \f).
  // These exercise the \\u00XX fallback path (lines 245-249).
  std::string msg;
  msg.push_back('\x01');  // SOH
  msg.push_back('\x02');  // STX
  msg.push_back('\x1f');  // US (unit separator)

  auto resp = HttpResponse::Error(ApiErrorCode::kBadRequest, msg);
  const std::string& body = resp.body();

  // Each should be escaped as \\u00XX.
  EXPECT_NE(body.find("\\u0001"), std::string::npos)
      << "SOH should be escaped as \\u0001";
  EXPECT_NE(body.find("\\u0002"), std::string::npos)
      << "STX should be escaped as \\u0002";
  EXPECT_NE(body.find("\\u001f"), std::string::npos)
      << "US should be escaped as \\u001f";

  // Raw control characters should not appear in the JSON body.
  EXPECT_EQ(body.find('\x01'), std::string::npos);
  EXPECT_EQ(body.find('\x02'), std::string::npos);
  EXPECT_EQ(body.find('\x1f'), std::string::npos);
}

TEST(JsonEscapingTest, BackslashEscaping) {
  // Test backslash escaping in JSON error messages.
  auto resp = HttpResponse::Error(ApiErrorCode::kBadRequest, "path\\to\\file");
  const std::string& body = resp.body();
  // Each backslash should be doubled.
  EXPECT_NE(body.find(R"(path\\to\\file)"), std::string::npos)
      << "Backslashes should be escaped";
}

TEST(JsonEscapingTest, MixedEscaping) {
  // Test a message with multiple types of special characters.
  std::string msg = "line1\nline2\ttab\r\nend\b\f\"quoted\"\\slash";
  auto resp = HttpResponse::Error(ApiErrorCode::kBadRequest, msg);
  const std::string& body = resp.body();

  EXPECT_NE(body.find("\\n"), std::string::npos);
  EXPECT_NE(body.find("\\t"), std::string::npos);
  EXPECT_NE(body.find("\\r"), std::string::npos);
  EXPECT_NE(body.find("\\b"), std::string::npos);
  EXPECT_NE(body.find("\\f"), std::string::npos);
  EXPECT_NE(body.find(R"(\"quoted\")"), std::string::npos);
  EXPECT_NE(body.find("\\\\slash"), std::string::npos);

  // No raw control characters.
  EXPECT_EQ(body.find('\n'), std::string::npos);
  EXPECT_EQ(body.find('\r'), std::string::npos);
  EXPECT_EQ(body.find('\t'), std::string::npos);
  EXPECT_EQ(body.find('\b'), std::string::npos);
  EXPECT_EQ(body.find('\f'), std::string::npos);
}

// Test: All error codes produce valid error responses.
TEST(ErrorResponseFormatTest, AllCodesProduceValidJson) {
  ApiErrorCode codes[] = {
      ApiErrorCode::kBadRequest,
      ApiErrorCode::kUnauthorized,
      ApiErrorCode::kForbidden,
      ApiErrorCode::kNotFound,
      ApiErrorCode::kMethodNotAllowed,
      ApiErrorCode::kPayloadTooLarge,
      ApiErrorCode::kUnsupportedMediaType,
      ApiErrorCode::kTooManyRequests,
      ApiErrorCode::kInternalError,
      ApiErrorCode::kServiceUnavailable,
      ApiErrorCode::kGatewayTimeout,
      ApiErrorCode::kBadGateway,
  };

  for (auto code : codes) {
    auto resp = HttpResponse::Error(code, "test message");
    EXPECT_EQ(resp.status_code(), ApiErrorCodeStatus(code))
        << "Status mismatch for code " << static_cast<int>(code);
    const std::string& body = resp.body();
    EXPECT_NE(body.find("\"error\""), std::string::npos)
        << "Missing error field for code " << static_cast<int>(code);
    EXPECT_NE(body.find(R"("code":")"), std::string::npos)
        << "Missing code field for code " << static_cast<int>(code);
    EXPECT_NE(body.find(R"("message":"test message")"), std::string::npos)
        << "Missing message field for code " << static_cast<int>(code);
  }
}

// Test: Error response without details omits details field.
TEST(ErrorResponseFormatTest, NoDetailsOmitsField) {
  auto resp = HttpResponse::Error(ApiErrorCode::kNotFound, "not found");
  const std::string& body = resp.body();
  EXPECT_EQ(body.find("\"details\""), std::string::npos)
      << "Details field should be absent when not provided";
}

// Test: Error response with empty details omits details field.
TEST(ErrorResponseFormatTest, EmptyDetailsOmitsField) {
  auto resp = HttpResponse::Error(ApiErrorCode::kNotFound, "not found", "");
  const std::string& body = resp.body();
  EXPECT_EQ(body.find("\"details\""), std::string::npos)
      << "Details field should be absent for empty string";
}

// =============================================================================
// Coverage: Request timeout (exercises OnTimeout callback, line 549-554)
// =============================================================================

TEST_F(HttpServerTest, RequestTimeoutSendsGatewayTimeout) {
  // Reconfigure with a very short timeout.
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.request_timeout_ms = 200;  // 200ms timeout
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/slow", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Connect but don't send a complete request — just send partial data.
  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 2);
  ASSERT_GE(sock, 0);

  // Send only a partial HTTP request (no \r\n\r\n terminator).
  const char* partial = "GET /v1/slow HTTP/1.1\r\nHost: loc";
  test::SocketWrite(sock, partial, strlen(partial));

  // Wait for timeout to fire.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, n);
  }
  test::CloseSocket(sock);

  EXPECT_NE(response.find("504"), std::string::npos)
      << "Timeout should produce 504 Gateway Timeout";
  EXPECT_NE(response.find("GATEWAY_TIMEOUT"), std::string::npos);
}

// =============================================================================
// Coverage: CORS wildcard origin (exercises lines 830-835)
// =============================================================================

class HttpServerCorsWildcardTest : public HttpServerTest {
 protected:
  void SetUp() override {
    HttpServerTest::SetUp();
    config_.cors_origin = "*";
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());

    server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
      return HttpResponse().Json("{\"status\":\"ok\"}");
    });
  }
};

TEST_F(HttpServerCorsWildcardTest, WildcardReflectsOrigin) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "GET /v1/health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Origin: http://any-origin.example.com\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  // Wildcard CORS should use literal "*", NOT reflect the client's origin.
  // Reflecting arbitrary origins with credentials is a CORS misconfiguration.
  EXPECT_NE(resp.find("Access-Control-Allow-Origin: *"), std::string::npos)
      << "Wildcard CORS should use literal '*'";
  // No credentials header with wildcard origin (browsers enforce this).
  EXPECT_EQ(resp.find("Access-Control-Allow-Credentials"), std::string::npos);
  EXPECT_NE(resp.find("Vary: Origin"), std::string::npos);
}

TEST_F(HttpServerCorsWildcardTest, NoCorsWithoutOriginHeader) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Request without Origin header — no CORS headers should appear.
  std::string resp =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_EQ(resp.find("Access-Control-Allow-Origin"), std::string::npos)
      << "No CORS headers without Origin header";
}

// =============================================================================
// Coverage: HttpResponse::ContentType builder (line 208-210)
// =============================================================================

TEST(HttpResponseTest, ContentTypeBuilder) {
  auto resp = HttpResponse()
                  .Status(200)
                  .ContentType("text/html")
                  .Body("<h1>Hello</h1>");
  std::string wire = resp.Serialize();

  EXPECT_NE(wire.find("Content-Type: text/html"), std::string::npos);
  EXPECT_TRUE(wire.ends_with("<h1>Hello</h1>"));
}

// =============================================================================
// Coverage: HttpResponse::Serialize with explicit Connection header (line 277)
// =============================================================================

TEST(HttpResponseTest, ResponseHeaderCrlfSanitization) {
  // Header values containing CRLF sequences should be sanitized by SetHeader
  // to prevent HTTP response splitting (header injection).
  auto resp = HttpResponse()
                  .Status(200)
                  .SetHeader("X-Custom", "value\r\nInjected-Header: evil")
                  .Body("ok");
  std::string wire = resp.Serialize();

  // The CRLF should be stripped, collapsing the injection attempt into a
  // single header value. The injected text becomes part of X-Custom's value
  // rather than a separate header line.
  auto pos = wire.find("X-Custom: ");
  ASSERT_NE(pos, std::string::npos);
  auto value_start = pos + strlen("X-Custom: ");
  auto value_end = wire.find("\r\n", value_start);
  ASSERT_NE(value_end, std::string::npos);
  std::string header_value = wire.substr(value_start, value_end - value_start);

  // The sanitized value should be the original with CR/LF stripped.
  EXPECT_EQ(header_value, "valueInjected-Header: evil");
  // No raw CR or LF should remain in the header value.
  EXPECT_EQ(header_value.find('\r'), std::string::npos);
  EXPECT_EQ(header_value.find('\n'), std::string::npos);

  // The injected text must NOT appear as a separate header line.
  // Count occurrences of "\r\n" followed by "Injected-Header" -- should be zero.
  EXPECT_EQ(wire.find("\r\nInjected-Header:"), std::string::npos)
      << "CRLF injection should not create a separate header line";
}

TEST(HttpResponseTest, ExplicitConnectionHeader) {
  auto resp = HttpResponse().SetHeader("Connection", "keep-alive").Body("test");
  std::string wire = resp.Serialize();

  // Should use explicit Connection: keep-alive, not auto-add close.
  EXPECT_NE(wire.find("Connection: keep-alive"), std::string::npos);
  // Should NOT have a second Connection header.
  auto first = wire.find("Connection:");
  auto second = wire.find("Connection:", first + 1);
  EXPECT_EQ(second, std::string::npos)
      << "Should not add a second Connection header";
}

// =============================================================================
// Coverage: HttpResponse::Serialize empty body (Content-Length: 0)
// =============================================================================

TEST(HttpResponseTest, EmptyBodyContentLengthZero) {
  auto resp = HttpResponse().Status(204);
  std::string wire = resp.Serialize();

  EXPECT_NE(wire.find("HTTP/1.1 204 No Content"), std::string::npos);
  EXPECT_NE(wire.find("Content-Length: 0"), std::string::npos);
}

// =============================================================================
// Coverage: Start returns false when already listening (line 361)
// =============================================================================

TEST_F(HttpServerTest, StartTwiceReturnsFalse) {
  ASSERT_TRUE(server_->Start());
  // Second start should return false.
  EXPECT_FALSE(server_->Start())
      << "Starting an already-listening server should return false";
}

// =============================================================================
// Coverage: DispatchRequest CORS preflight (lines 747-755)
// =============================================================================

TEST_F(HttpServerCorsTest, PreflightMaxAge) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp = SendRequest(
      "OPTIONS /v1/health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Origin: http://localhost:3000\r\n"
      "Access-Control-Request-Method: POST\r\n\r\n");
  EXPECT_NE(resp.find("204"), std::string::npos);
  EXPECT_NE(resp.find("Access-Control-Max-Age: 86400"), std::string::npos);
  EXPECT_NE(resp.find("Access-Control-Allow-Headers"), std::string::npos);
}

// =============================================================================
// Coverage: URL too long (line 634-637)
// =============================================================================

TEST_F(HttpServerTest, UrlTooLongReturns400) {
  config_.max_url_length = 50;
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Build a request with a very long URL.
  std::string long_path(100, 'x');
  std::string resp =
      SendRequest("GET /" + long_path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos)
      << "URL exceeding max length should return 400";
}

// =============================================================================
// Coverage: HttpResponse::Body builder and chaining (lines 212-215)
// =============================================================================

TEST(HttpResponseTest, BodyBuilderChaining) {
  auto resp = HttpResponse()
                  .Status(200)
                  .ContentType("text/plain")
                  .Body("Hello, World!");
  EXPECT_EQ(resp.status_code(), 200);
  EXPECT_EQ(resp.body(), "Hello, World!");

  std::string wire = resp.Serialize();
  EXPECT_NE(wire.find("Content-Type: text/plain"), std::string::npos);
  EXPECT_NE(wire.find("Content-Length: 13"), std::string::npos);
  EXPECT_TRUE(wire.ends_with("Hello, World!"));
}

// =============================================================================
// Coverage: Serialize all header types together
// =============================================================================

TEST(HttpResponseTest, SerializeMultipleHeaders) {
  auto resp = HttpResponse()
                  .Status(200)
                  .SetHeader("X-Custom", "value1")
                  .SetHeader("X-Other", "value2")
                  .ContentType("application/json")
                  .Body("{\"ok\":true}");
  std::string wire = resp.Serialize();

  EXPECT_NE(wire.find("X-Custom: value1"), std::string::npos);
  EXPECT_NE(wire.find("X-Other: value2"), std::string::npos);
  EXPECT_NE(wire.find("Content-Type: application/json"), std::string::npos);
  EXPECT_NE(wire.find("Content-Length: 11"), std::string::npos);
}

// =============================================================================
// Coverage: JsonNestingTooDeep (header-only, lines 257-267)
// =============================================================================

TEST(JsonNestingTest, ShallowJsonPasses) {
  EXPECT_FALSE(JsonNestingTooDeep(R"({"a":{"b":1}})"));
  EXPECT_FALSE(JsonNestingTooDeep("[]"));
  EXPECT_FALSE(JsonNestingTooDeep(""));
}

TEST(JsonNestingTest, DeepJsonFails) {
  std::string deep;
  for (int i = 0; i < 40; ++i) deep += '{';
  for (int i = 0; i < 40; ++i) deep += '}';
  EXPECT_TRUE(JsonNestingTooDeep(deep));
}

TEST(JsonNestingTest, ExactlyAtLimit) {
  // Default limit is 32. Exactly 32 deep should not trigger.
  std::string json;
  for (int i = 0; i < 32; ++i) json += '[';
  for (int i = 0; i < 32; ++i) json += ']';
  EXPECT_FALSE(JsonNestingTooDeep(json));

  // 33 deep should trigger.
  std::string deep;
  for (int i = 0; i < 33; ++i) deep += '[';
  for (int i = 0; i < 33; ++i) deep += ']';
  EXPECT_TRUE(JsonNestingTooDeep(deep));
}

TEST(JsonNestingTest, CustomMaxDepth) {
  std::string json = "[[[]]]";  // depth 3
  EXPECT_FALSE(JsonNestingTooDeep(json, 3));
  EXPECT_TRUE(JsonNestingTooDeep(json, 2));
}

TEST(JsonNestingTest, StringBypass) {
  // Closing braces inside JSON string literals should not count as nesting.
  EXPECT_FALSE(JsonNestingTooDeep(R"({"key": "}}}}"})"));
  // Opening braces inside JSON string literals should not count as nesting.
  EXPECT_FALSE(JsonNestingTooDeep(R"({"key": "{{{{"})"));
  // Real nesting at 33+ levels should still be detected.
  std::string deep;
  for (int i = 0; i < 33; ++i) deep += "{\"a\":";
  deep += "1";
  for (int i = 0; i < 33; ++i) deep += '}';
  EXPECT_TRUE(JsonNestingTooDeep(deep));
  // Malformed: more closes than opens (depth goes negative).
  EXPECT_TRUE(JsonNestingTooDeep("}}}{"));
}

TEST(JsonNestingTest, EscapedQuotes) {
  // Escaped quote inside a string should not end the string prematurely.
  // The \" inside the string value should be treated as literal, not as
  // a string terminator.
  EXPECT_FALSE(JsonNestingTooDeep("{\"key\": \"test\\\"\"}"));
}

// =============================================================================
// Coverage: Header size limit exceeded (lines 571-572, 594-595)
// =============================================================================

TEST_F(HttpServerTest, HeaderFieldSizeLimitExceeded) {
  // Configure a very small header byte limit.
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.max_header_bytes = 64;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/test", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Send a request with a header field name that exceeds the limit.
  std::string big_header_name(128, 'X');
  std::string request = "GET /v1/test HTTP/1.1\r\n" + big_header_name +
                        ": value\r\nHost: localhost\r\n\r\n";

  std::string resp = SendRequest(request);
  // HPE_USER from OnHeaderField causes llhttp_execute to return error,
  // resulting in a 400 "Malformed HTTP request" response.
  EXPECT_NE(resp.find("400"), std::string::npos)
      << "Header field exceeding max_header_bytes should return 400";
}

TEST_F(HttpServerTest, HeaderValueSizeLimitExceeded) {
  // Configure a very small header byte limit.
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.max_header_bytes = 64;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/test", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Send a request with a header value that exceeds the limit.
  std::string big_header_value(128, 'Y');
  std::string request =
      "GET /v1/test HTTP/1.1\r\nHost: localhost\r\n"
      "X-Big: " +
      big_header_value + "\r\n\r\n";

  std::string resp = SendRequest(request);
  // HPE_USER from OnHeaderValue causes llhttp_execute to return error,
  // resulting in a 400 "Malformed HTTP request" response.
  EXPECT_NE(resp.find("400"), std::string::npos)
      << "Header value exceeding max_header_bytes should return 400";
}

// =============================================================================
// Coverage: Body size limit via OnBody callback (line 607-608)
// =============================================================================

TEST_F(HttpServerTest, BodySizeLimitViaOnBody) {
  // Use a small max_request_body to ensure the OnBody callback (line 607)
  // rejects the body before the raw data check in OnRead (line 497).
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.max_request_body = 128;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("POST", "/v1/test", [](const HttpRequest& req) {
    return HttpResponse().Json("{\"size\":" + std::to_string(req.body.size()) +
                               "}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Send a body larger than 128 bytes but small enough that it arrives in
  // a single read, ensuring OnBody sees the full body in one callback.
  std::string body(256, 'Z');
  std::string request =
      "POST /v1/test HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  // OnBody returns HPE_USER, causing llhttp_execute error -> 400.
  bool got_413 = resp.find("413") != std::string::npos;
  bool got_400 = resp.find("400") != std::string::npos;
  EXPECT_TRUE(got_413 || got_400)
      << "Body exceeding max_request_body should return 413 or 400, got: "
      << resp.substr(0, 100);
}

// =============================================================================
// Coverage: Connection limit exceeded (lines 435-441)
// =============================================================================

TEST_F(HttpServerTest, ConnectionLimitExceeded) {
  // Configure max_connections to 1.
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.max_connections = 1;
  config_.request_timeout_ms = 5000;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/test", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();

  // Open first connection and hold it (send partial request so it stays open).
  int sock1 = test::ConnectTcp(port, 2);

  ASSERT_GE(sock1, 0);
  // Send a partial request to keep the connection alive.
  const char* partial = "GET /v1/test HTTP/1.1\r\n";
  test::SocketWrite(sock1, partial, strlen(partial));

  // Give the loop time to accept the first connection.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Now try a second connection -- it should be immediately closed by the
  // server since max_connections (1) is exceeded.
  int sock2 = test::ConnectTcp(port, 2);
  ASSERT_GE(sock2, 0);

  // Send a full request on the second socket.
  std::string request = "GET /v1/test HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock2, request.data(), request.size());

  // Wait for the server to process.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Try to read from sock2 -- it should be closed (empty or connection reset).
  char buf[4096];
  ssize_t n = test::SocketRead(sock2, buf, sizeof(buf));
  // The server accepted and immediately closed the connection, so we expect
  // either 0 (EOF) or -1 (connection reset) -- not a valid HTTP response.
  EXPECT_LE(n, 0)
      << "Connection exceeding limit should be closed without response";

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
}

// =============================================================================
// Coverage: WebSocket upgrade path (lines 667-732)
// =============================================================================

TEST_F(HttpServerTest, WebSocketUpgradeTriggersHandler) {
  std::atomic<bool> upgrade_called{false};
  std::string received_path;

  server_->SetUpgradeHandler([&](HttpRequest request, uv_stream_t* handle) {
    received_path = request.path;
    upgrade_called.store(true, std::memory_order_release);
    // Clean up the detached handle.
    uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
  });

  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Send a valid WebSocket upgrade request.
  std::string request =
      "GET /v1/ws/events HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";

  // Use raw socket since SendRequest expects a normal HTTP response.
  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 2);

  ASSERT_GE(sock, 0);
  test::SocketWrite(sock, request.data(), request.size());

  // Wait for the server to process.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  test::CloseSocket(sock);

  EXPECT_TRUE(upgrade_called.load(std::memory_order_acquire))
      << "Upgrade handler should have been called";
  EXPECT_EQ(received_path, "/v1/ws/events");
}

TEST_F(HttpServerTest, WebSocketUpgradeIgnoredWithoutHandler) {
  // No upgrade handler set -- request should route normally (404 or matched).
  server_->AddRoute("GET", "/v1/ws/events", [](const HttpRequest&) {
    return HttpResponse().Json("{\"regular\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string request =
      "GET /v1/ws/events HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";

  std::string resp = SendRequest(request);
  // Without an upgrade handler, the request should be routed normally.
  EXPECT_NE(resp.find("200 OK"), std::string::npos)
      << "Without upgrade handler, WS request should route normally";
  EXPECT_NE(resp.find("{\"regular\":true}"), std::string::npos);
}

TEST_F(HttpServerTest, WebSocketUpgradeRequiresAllHeaders) {
  std::atomic<bool> upgrade_called{false};
  server_->SetUpgradeHandler([&](HttpRequest, uv_stream_t* handle) {
    upgrade_called.store(true, std::memory_order_release);
    uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
  });
  server_->AddRoute("GET", "/v1/ws/events", [](const HttpRequest&) {
    return HttpResponse().Json("{\"regular\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Missing Sec-WebSocket-Key header -- should not trigger upgrade.
  std::string request =
      "GET /v1/ws/events HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n\r\n";

  std::string resp = SendRequest(request);
  EXPECT_FALSE(upgrade_called.load(std::memory_order_acquire))
      << "Missing Sec-WebSocket-Key should not trigger upgrade";
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

// =============================================================================
// Coverage: WebSocket upgrade authentication (lines 708-721)
// =============================================================================

class HttpServerWsAuthTest : public HttpServerTest {
 protected:
  void SetUp() override {
    HttpServerTest::SetUp();
    config_.auth_token = "ws-secret-token";
    config_.read_open = true;  // GETs and WS open without auth
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());

    upgrade_called_.store(false, std::memory_order_relaxed);
    server_->SetUpgradeHandler(
        [this](HttpRequest request, uv_stream_t* handle) {
          upgrade_path_ = request.path;
          upgrade_called_.store(true, std::memory_order_release);
          uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
            delete reinterpret_cast<uv_any_handle*>(h);
          });
        });
  }

  std::atomic<bool> upgrade_called_{false};
  std::string upgrade_path_;
};

TEST_F(HttpServerWsAuthTest, WsUpgradeAllowedInReadOpenMode) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // WebSocket upgrade without auth should succeed in read_open mode.
  std::string request =
      "GET /v1/ws/events HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";

  (void)SendRequest(request);
  EXPECT_TRUE(upgrade_called_.load(std::memory_order_acquire))
      << "WS upgrade should succeed in read_open mode without auth";
}

TEST_F(HttpServerWsAuthTest, WsUpgradeRequiresAuthWhenNotReadOpen) {
  // Recreate server with read_open=false.
  config_.read_open = false;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  upgrade_called_.store(false, std::memory_order_relaxed);
  server_->SetUpgradeHandler([this](HttpRequest request, uv_stream_t* handle) {
    upgrade_path_ = request.path;
    upgrade_called_.store(true, std::memory_order_release);
    uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
  });

  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string request =
      "GET /v1/ws/events HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";

  std::string resp = SendRequest(request);
  EXPECT_FALSE(upgrade_called_.load(std::memory_order_acquire))
      << "WS upgrade without auth should not trigger handler";
  EXPECT_NE(resp.find("401"), std::string::npos)
      << "Missing auth on WS upgrade should return 401";
}

TEST_F(HttpServerWsAuthTest, WsUpgradeWithWrongTokenReturnsForbidden) {
  // Recreate server with read_open=false.
  config_.read_open = false;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  upgrade_called_.store(false, std::memory_order_relaxed);
  server_->SetUpgradeHandler([this](HttpRequest request, uv_stream_t* handle) {
    upgrade_path_ = request.path;
    upgrade_called_.store(true, std::memory_order_release);
    uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
  });

  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string request =
      "GET /v1/ws/events HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Authorization: Bearer wrong-token\r\n\r\n";

  std::string resp = SendRequest(request);
  EXPECT_FALSE(upgrade_called_.load(std::memory_order_acquire))
      << "WS upgrade with wrong token should not trigger handler";
  EXPECT_NE(resp.find("403"), std::string::npos)
      << "Wrong token on WS upgrade should return 403";
}

TEST_F(HttpServerWsAuthTest, WsUpgradeWithValidTokenSucceeds) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string request =
      "GET /v1/ws/events HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Authorization: Bearer ws-secret-token\r\n\r\n";

  // Use raw socket -- upgrade doesn't produce a normal HTTP response.
  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 2);

  ASSERT_GE(sock, 0);
  test::SocketWrite(sock, request.data(), request.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  test::CloseSocket(sock);

  EXPECT_TRUE(upgrade_called_.load(std::memory_order_acquire))
      << "WS upgrade with valid token should trigger handler";
  EXPECT_EQ(upgrade_path_, "/v1/ws/events");
}

// =============================================================================
// Coverage: WebSocket upgrade Connection header tokenization (lines 678-697)
// =============================================================================

TEST_F(HttpServerTest, WebSocketUpgradeWithMultiValueConnectionHeader) {
  std::atomic<bool> upgrade_called{false};
  server_->SetUpgradeHandler([&](HttpRequest, uv_stream_t* handle) {
    upgrade_called.store(true, std::memory_order_release);
    uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // "Connection: keep-alive, Upgrade" — the Upgrade token is in a comma list.
  std::string request =
      "GET /v1/ws/test HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: keep-alive, Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";

  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 2);

  ASSERT_GE(sock, 0);
  test::SocketWrite(sock, request.data(), request.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  test::CloseSocket(sock);

  EXPECT_TRUE(upgrade_called.load(std::memory_order_acquire))
      << "Upgrade should be detected in multi-value Connection header";
}

// =============================================================================
// Coverage: Destructor cleanup when listening (lines 335-338)
// =============================================================================

TEST(HttpServerDestructorTest, DestructorStopsListeningServer) {
  auto* loop = new uv_loop_t;
  uv_loop_init(loop);
  NullMessageHandler handler;
  HttpServerConfig config;
  config.port = 0;
  config.allow_unauthenticated = true;

  {
    // Create and start a server, then explicitly Stop() and drain the event
    // loop while the server is still alive.  The destructor verifies that
    // double-Stop() is safe.
    HttpServer server(loop, config, &handler);
    server.AddRoute("GET", "/v1/health", [](const HttpRequest&) {
      return HttpResponse().Json("{\"status\":\"ok\"}");
    });
    ASSERT_TRUE(server.Start());
    EXPECT_GT(server.bound_port(), 0);
    // Stop initiates async uv_close; drain callbacks while server is alive.
    server.Stop();
    uv_run(loop, UV_RUN_DEFAULT);
    // Destructor runs here -- Stop() is idempotent.
  }
  uv_loop_close(loop);
  delete loop;
  // Test passes if no crash or leak occurs.
}

// =============================================================================
// Coverage: Premature connection close (exercises CloseConnection via OnRead
// EOF path, lines 485-488)
// =============================================================================

TEST_F(HttpServerTest, PrematureConnectionClose) {
  server_->AddRoute("GET", "/v1/test", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Connect and immediately close without sending anything.
  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 2);

  ASSERT_GE(sock, 0);

  // Immediately close — triggers OnRead with nread < 0 (EOF).
  test::CloseSocket(sock);

  // Give the server time to process the close.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Server should still be functional.
  std::string resp =
      SendRequest("GET /v1/test HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos)
      << "Server should remain functional after premature client close";
}

// =============================================================================
// Coverage: Multiple sequential requests on separate connections
// =============================================================================

TEST_F(HttpServerTest, MultipleSequentialRequests) {
  int request_count = 0;
  server_->AddRoute("GET", "/v1/counter", [&](const HttpRequest&) {
    request_count++;
    return HttpResponse().Json("{\"count\":" + std::to_string(request_count) +
                               "}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Send multiple requests sequentially.
  for (int i = 1; i <= 3; ++i) {
    std::string resp =
        SendRequest("GET /v1/counter HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(resp.find("200 OK"), std::string::npos);
  }
  EXPECT_EQ(request_count, 3);
}

// =============================================================================
// Coverage: Various malformed HTTP request lines (exercises OnRead llhttp
// parse error path, lines 510-513)
// =============================================================================

TEST_F(HttpServerTest, MalformedRequestIncompleteVerb) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Incomplete method with no path.
  std::string resp = SendRequest("GE\r\n\r\n");
  // Should get 400 (malformed) or empty (connection closed).
  if (!resp.empty()) {
    EXPECT_NE(resp.find("400"), std::string::npos);
  }
}

TEST_F(HttpServerTest, MalformedRequestBinaryGarbage) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Send binary garbage.
  std::string garbage;
  for (int i = 0; i < 64; ++i) {
    garbage.push_back(static_cast<char>(0x80 + (i % 64)));
  }
  garbage += "\r\n\r\n";

  std::string resp = SendRequest(garbage);
  if (!resp.empty()) {
    EXPECT_NE(resp.find("400"), std::string::npos);
  }
}

// =============================================================================
// Coverage: Empty path request
// =============================================================================

TEST_F(HttpServerTest, EmptyPathRequest) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // Request with just "/" path.
  std::string resp = SendRequest("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos);
}

// =============================================================================
// Coverage: Stop without Start is a no-op (line 410)
// =============================================================================

TEST_F(HttpServerTest, StopWithoutStartIsNoop) {
  // Server is created but not started.  Stop should be a safe no-op.
  server_->Stop();
  // Calling Stop again should also be safe.
  server_->Stop();
  // Test passes if no crash occurs.
}

// =============================================================================
// Coverage: Multiple connections closed during Stop (lines 417-421)
// =============================================================================

TEST_F(HttpServerTest, StopClosesAllActiveConnections) {
  config_.request_timeout_ms = 30000;  // Long timeout so connections stay open
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/test", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();

  // Open multiple connections with partial requests to keep them alive.
  std::vector<int> sockets;
  for (int i = 0; i < 3; ++i) {
    int sock = test::ConnectTcp(port, 2);

    ASSERT_GE(sock, 0);
    // Send partial request to keep connection open.
    const char* partial = "GET /v1/test HTTP/1.1\r\n";
    test::SocketWrite(sock, partial, strlen(partial));
    sockets.push_back(sock);
  }

  // Give server time to accept all connections.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(server_->active_connections(), 3);

  // Stop the server — should close all connections.
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_DEFAULT);

  // Clean up client sockets.
  for (int s : sockets) {
    test::CloseSocket(s);
  }

  EXPECT_EQ(server_->active_connections(), 0);
}

// =============================================================================
// Coverage: HttpResponse::Json sets correct Content-Type (lines 217-220)
// =============================================================================

TEST(HttpResponseTest, JsonSetsApplicationJsonContentType) {
  auto resp = HttpResponse().Status(200).Json(R"({"key":"value"})");
  std::string wire = resp.Serialize();

  EXPECT_NE(wire.find("Content-Type: application/json"), std::string::npos);
  EXPECT_NE(wire.find(R"({"key":"value"})"), std::string::npos);
  EXPECT_EQ(resp.body(), R"({"key":"value"})");
}

// =============================================================================
// Coverage: Error response with details JSON (lines 258-260)
// =============================================================================

TEST(ErrorResponseFormatTest, DetailsJsonIncluded) {
  auto resp = HttpResponse::Error(ApiErrorCode::kBadRequest, "test error",
                                  R"({"field":"name","max":100})");
  const std::string& body = resp.body();
  EXPECT_NE(body.find(R"("details":{"field":"name","max":100})"),
            std::string::npos);
  EXPECT_NE(body.find(R"("code":"BAD_REQUEST")"), std::string::npos);
  EXPECT_NE(body.find(R"("message":"test error")"), std::string::npos);
}

// =============================================================================
// Coverage: Send partial headers then close (exercises nread == 0 path
// in OnRead, line 491-493)
// =============================================================================

TEST_F(HttpServerTest, PartialHeadersThenClose) {
  server_->AddRoute("GET", "/v1/test", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 2);

  ASSERT_GE(sock, 0);

  // Send partial headers.
  const char* partial = "GET /v1/test HTTP/1.1\r\nHost: localhost\r\n";
  test::SocketWrite(sock, partial, strlen(partial));

  // Wait briefly, then close without completing the request.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  test::CloseSocket(sock);

  // Give server time to process.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Server should still be functional.
  std::string resp =
      SendRequest("GET /v1/test HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

// =============================================================================
// Coverage: Query string with empty value (key=) and special characters
// =============================================================================

TEST(HttpRequestTest, QueryStringEmptyValue) {
  HttpRequest req;
  req.query_string = "key=&other=val";
  req.ParseQueryString();
  EXPECT_EQ(req.QueryParam("key"), "");
  EXPECT_EQ(req.QueryParam("other"), "val");
}

TEST(HttpRequestTest, QueryStringSpecialCharsInValue) {
  HttpRequest req;
  req.query_string = "url=http%3A%2F%2Fexample.com%2Fpath%3Fq%3D1%26r%3D2";
  req.ParseQueryString();
  EXPECT_EQ(req.QueryParam("url"), "http://example.com/path?q=1&r=2");
}

// =============================================================================
// Coverage: CORS with matching specific origin (not wildcard)
// Exercises lines 836-839 when cors_origin matches request origin exactly
// =============================================================================

TEST_F(HttpServerCorsTest, MatchingOriginSetsCorsHeaders) {
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  // The config has cors_origin = "http://localhost:3000".
  // Request with exactly that origin.
  std::string resp = SendRequest(
      "GET /v1/health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Origin: http://localhost:3000\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("Access-Control-Allow-Credentials: true"),
            std::string::npos);
  EXPECT_NE(resp.find("Vary: Origin"), std::string::npos);
}

// =============================================================================
// API5: Content-Type validation — 415 on non-JSON POST/PATCH bodies
// =============================================================================

TEST_F(HttpServerTest, PostWithWrongContentTypeReturns415) {
  server_->AddRoute("POST", "/v1/test", [](const HttpRequest& req) {
    return HttpResponse().Json("{\"body\":\"" + req.body + "\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string body = R"({"key":"value"})";
  std::string request =
      "POST /v1/test HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: text/plain\r\n"
      "X-Requested-With: XMLHttpRequest\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("415"), std::string::npos);
  EXPECT_NE(resp.find("UNSUPPORTED_MEDIA_TYPE"), std::string::npos);
}

TEST_F(HttpServerTest, PostWithJsonContentTypeCharsetAccepted) {
  server_->AddRoute("POST", "/v1/test", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string body = R"({"key":"value"})";
  std::string request =
      "POST /v1/test HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json; charset=utf-8\r\n"
      "X-Requested-With: XMLHttpRequest\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

TEST_F(HttpServerTest, PostWithEmptyBodySkipsContentTypeCheck) {
  // Empty body POST should be routed normally even without Content-Type.
  server_->AddRoute("POST", "/v1/test", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string request =
      "POST /v1/test HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "X-Requested-With: XMLHttpRequest\r\n"
      "Content-Length: 0\r\n\r\n";

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

TEST_F(HttpServerTest, NonApiPathSkipsContentTypeCheck) {
  // Content-Type check only applies to /v1/* paths.
  server_->AddRoute("POST", "/upload", [](const HttpRequest&) {
    return HttpResponse().Body("ok");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string body = "not json";
  std::string request =
      "POST /upload HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

// =============================================================================
// UnsupportedMediaType error code
// =============================================================================

TEST(ErrorCodeTest, UnsupportedMediaType415) {
  EXPECT_EQ(ApiErrorCodeStatus(ApiErrorCode::kUnsupportedMediaType), 415);
  EXPECT_STREQ(ApiErrorCodeString(ApiErrorCode::kUnsupportedMediaType),
               "UNSUPPORTED_MEDIA_TYPE");
  EXPECT_STREQ(HttpStatusText(415), "Unsupported Media Type");
}

// =============================================================================
// API9: Body inactivity timeout — slow body upload closed
// =============================================================================

TEST_F(HttpServerTest, BodyInactivityTimeoutCloses) {
  // Configure a very short body inactivity timeout.
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.body_read_timeout_ms = 200;  // 200ms body inactivity
  config_.request_timeout_ms = 10000;  // Long overall timeout
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("POST", "/v1/test", [](const HttpRequest& req) {
    return HttpResponse().Json("{\"size\":" + std::to_string(req.body.size()) +
                               "}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 3);
  ASSERT_GE(sock, 0);

  // Send headers declaring a large body, then stop sending.
  std::string headers =
      "POST /v1/test HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 1024\r\n\r\n"
      "{";  // 1 byte of body, then silence
  test::SocketWrite(sock, headers.data(), headers.size());

  // Wait for the body inactivity timeout to fire.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Server should have closed the connection with a timeout response.
  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, n);
  }
  test::CloseSocket(sock);

  EXPECT_NE(response.find("504"), std::string::npos)
      << "Body inactivity should produce 504 Gateway Timeout, got: "
      << response.substr(0, 120);
}

// =============================================================================
// HTTP/1.1 Keep-Alive
// =============================================================================

TEST_F(HttpServerTest, KeepAliveMultipleRequestsOnSameConnection) {
  std::atomic<int> request_count{0};
  server_->AddRoute("GET", "/v1/counter", [&](const HttpRequest&) {
    int n = ++request_count;
    return HttpResponse().Json("{\"n\":" + std::to_string(n) + "}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();
  // Use a short recv timeout (1s) so reads don't block forever.
  int sock = test::ConnectTcp(port, 1);
  ASSERT_GE(sock, 0);

  for (int i = 1; i <= 3; ++i) {
    std::string req = "GET /v1/counter HTTP/1.1\r\nHost: localhost\r\n\r\n";
    test::SocketWrite(sock, req.data(), req.size());

    // Brief delay to let the libuv loop process the request.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    char buf[4096];
    ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
    ASSERT_GT(n, 0) << "Expected response for request " << i;
    std::string resp(buf, n);

    EXPECT_NE(resp.find("200 OK"), std::string::npos)
        << "Request " << i << " failed: " << resp.substr(0, 120);
    EXPECT_NE(resp.find("Connection: keep-alive"), std::string::npos)
        << "Request " << i << " missing keep-alive header";
    EXPECT_NE(resp.find("\"n\":" + std::to_string(i)), std::string::npos)
        << "Request " << i << " wrong counter value";
  }

  test::CloseSocket(sock);
  EXPECT_EQ(request_count.load(), 3);
}

TEST_F(HttpServerTest, ConnectionCloseHeaderDisablesKeepAlive) {
  server_->AddRoute("GET", "/v1/ping", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 1);
  ASSERT_GE(sock, 0);

  // Send request with Connection: close — server should close after response.
  std::string req =
      "GET /v1/ping HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n\r\n";
  test::SocketWrite(sock, req.data(), req.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, n);
  }

  EXPECT_NE(response.find("200 OK"), std::string::npos);
  EXPECT_NE(response.find("Connection: close"), std::string::npos);

  // Verify the server closed the connection: a second write should fail
  // or a subsequent read should return 0/error.
  std::string req2 = "GET /v1/ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock, req2.data(), req2.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
  EXPECT_LE(n, 0) << "Connection should be closed after Connection: close";

  test::CloseSocket(sock);
}

TEST_F(HttpServerTest, Http10DefaultsToClose) {
  server_->AddRoute("GET", "/v1/ping", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 1);
  ASSERT_GE(sock, 0);

  // HTTP/1.0 defaults to Connection: close.
  std::string req = "GET /v1/ping HTTP/1.0\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock, req.data(), req.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, n);
  }

  EXPECT_NE(response.find("200 OK"), std::string::npos);
  // Server should close the connection after the response.
  test::CloseSocket(sock);
}

TEST_F(HttpServerTest, PipeliningTwoRequests) {
  std::atomic<int> request_count{0};
  server_->AddRoute("GET", "/v1/counter", [&](const HttpRequest&) {
    int n = ++request_count;
    return HttpResponse().Json("{\"n\":" + std::to_string(n) + "}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 2);
  ASSERT_GE(sock, 0);

  // Send two pipelined requests in a single write.  The second request
  // uses Connection: close so the server closes after responding to it.
  std::string pipelined =
      "GET /v1/counter HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /v1/counter HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: close\r\n\r\n";
  test::SocketWrite(sock, pipelined.data(), pipelined.size());

  // Read until the server closes the connection.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, n);
  }
  test::CloseSocket(sock);

  // Both responses should be present.
  EXPECT_NE(response.find("\"n\":1"), std::string::npos)
      << "First pipelined response missing";
  EXPECT_NE(response.find("\"n\":2"), std::string::npos)
      << "Second pipelined response missing";
  EXPECT_EQ(request_count.load(), 2);
}

TEST_F(HttpServerTest, MaxRequestsPerConnectionEnforced) {
  // Reconfigure the server with a very low request limit.
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.max_requests_per_connection = 2;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/ping", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int port = server_->bound_port();
  int sock = test::ConnectTcp(port, 2);
  ASSERT_GE(sock, 0);

  // First request — should get keep-alive.
  std::string req1 = "GET /v1/ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock, req1.data(), req1.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  char buf[4096];
  ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  std::string resp1(buf, n);
  EXPECT_NE(resp1.find("Connection: keep-alive"), std::string::npos)
      << "First request should get keep-alive";

  // Second request — hits the limit, should get Connection: close and
  // the server should close the connection.
  std::string req2 = "GET /v1/ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock, req2.data(), req2.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string resp2;
  while (true) {
    n = test::SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    resp2.append(buf, n);
  }
  EXPECT_NE(resp2.find("200 OK"), std::string::npos);
  EXPECT_NE(resp2.find("Connection: close"), std::string::npos)
      << "Second request should get Connection: close (limit reached)";

  test::CloseSocket(sock);
}

// =============================================================================
// CSRF protection for state-changing data/admin endpoints
//
// When no API token is configured (the default in the in-process ASP.NET host),
// CheckAuth() lets requests through without a credential.  The embedded console
// proxy now forwards /v1/cache/*, /v1/capture/*, /v1/config to the worker on the
// customer's public app port, so a cross-origin browser POST could otherwise
// reach these mutating endpoints with neither auth nor CSRF.  The dispatcher
// requires X-Requested-With: XMLHttpRequest on POST/PATCH to /v1 paths when
// auth_token is empty.  Uses the default HttpServerTest fixture,
// whose config has an empty auth_token.
// =============================================================================

TEST_F(HttpServerTest, CsrfCachePurgePostWithoutHeaderRejected) {
  // POST /v1/cache/purge WITHOUT X-Requested-With must be rejected with 400
  // (CSRF) before reaching routing.  A route is registered so that, absent the
  // CSRF gate, the request would reach the handler (proving the gate fires).
  server_->AddRoute("POST", "/v1/cache/purge", [](const HttpRequest&) {
    return HttpResponse().Json("{\"purged\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string body = R"({"url":"https://example.com"})";
  std::string request =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("400"), std::string::npos)
      << "POST to /v1/cache/purge without X-Requested-With should be a CSRF "
         "400";
  EXPECT_NE(resp.find("CSRF"), std::string::npos);
  // Must NOT have reached the handler.
  EXPECT_EQ(resp.find("\"purged\":true"), std::string::npos)
      << "CSRF gate should block before the handler runs";
}

TEST_F(HttpServerTest, CsrfCachePurgePostWithHeaderReachesHandler) {
  // Same request WITH X-Requested-With: XMLHttpRequest must NOT be rejected by
  // the CSRF gate — it should reach routing/handler.
  server_->AddRoute("POST", "/v1/cache/purge", [](const HttpRequest&) {
    return HttpResponse().Json("{\"purged\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string body = R"({"url":"https://example.com"})";
  std::string request =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "X-Requested-With: XMLHttpRequest\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  // The CSRF gate must not fire — the request reaches the handler (200 here).
  EXPECT_EQ(resp.find("CSRF"), std::string::npos)
      << "CSRF gate should not fire when X-Requested-With is present";
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("\"purged\":true"), std::string::npos)
      << "Request with CSRF header should reach the handler";
}

TEST_F(HttpServerTest, CsrfConfigPatchWithoutHeaderRejected) {
  // PATCH /v1/config WITHOUT X-Requested-With must be rejected with 400 (CSRF).
  server_->AddRoute("PATCH", "/v1/config", [](const HttpRequest&) {
    return HttpResponse().Json("{\"applied\":{}}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string body = R"({"RewriteLevel":"PassThrough"})";
  std::string request =
      "PATCH /v1/config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(request);
  EXPECT_NE(resp.find("400"), std::string::npos)
      << "PATCH to /v1/config without X-Requested-With should be a CSRF 400";
  EXPECT_NE(resp.find("CSRF"), std::string::npos);
  EXPECT_EQ(resp.find("\"applied\""), std::string::npos)
      << "CSRF gate should block before the handler runs";
}

TEST_F(HttpServerTest, CsrfGetDataEndpointNotAffected) {
  // GET requests must pass through unaffected (the POST/PATCH filter never
  // touches them) — e.g. /v1/health, /v1/stats.
  server_->AddRoute("GET", "/v1/stats", [](const HttpRequest&) {
    return HttpResponse().Json("{\"stats\":true}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string resp =
      SendRequest("GET /v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_EQ(resp.find("CSRF"), std::string::npos)
      << "GET requests should never hit the CSRF gate";
  EXPECT_NE(resp.find("\"stats\":true"), std::string::npos);
}

// Opt-in cross-site WebSocket hijacking defense (audit 2026-05-29,
// worker http). The WS upgrade validates Origin against the configured
// cors_origin allowlist when one is set.
TEST(WsOriginAllowlistTest, EmptyAllowlistImposesNoCheck) {
  EXPECT_TRUE(HttpServer::IsWsOriginAllowed("https://evil.example", ""));
  EXPECT_TRUE(HttpServer::IsWsOriginAllowed("", ""));
}

TEST(WsOriginAllowlistTest, WildcardImposesNoCheck) {
  EXPECT_TRUE(HttpServer::IsWsOriginAllowed("https://evil.example", "*"));
}

TEST(WsOriginAllowlistTest, MatchingOriginAllowed) {
  EXPECT_TRUE(HttpServer::IsWsOriginAllowed("https://app.example",
                                            "https://app.example"));
}

TEST(WsOriginAllowlistTest, MismatchedOriginRejected) {
  EXPECT_FALSE(HttpServer::IsWsOriginAllowed("https://evil.example",
                                             "https://app.example"));
}

TEST(WsOriginAllowlistTest, AbsentOriginAllowedForNonBrowserClient) {
  // Browsers always send Origin on a WS handshake; an absent Origin is a
  // non-browser client and not a CSWSH vector.
  EXPECT_TRUE(HttpServer::IsWsOriginAllowed("", "https://app.example"));
}

// =============================================================================
// Bind classification, fail-closed auth, unix-socket transport
// =============================================================================

TEST(ApiBindClassificationTest, LoopbackIsTheWholeSlashEightBlock) {
  EXPECT_TRUE(IsLoopbackApiBind("127.0.0.1"));
  EXPECT_TRUE(IsLoopbackApiBind("127.0.0.53"));
  EXPECT_TRUE(IsLoopbackApiBind("127.255.255.254"));
  // Empty means "use the compiled default", which is 127.0.0.1.
  EXPECT_TRUE(IsLoopbackApiBind(""));
}

// The server binds through uv_ip4_addr, so a hostname or an IPv6 literal
// cannot be bound at all.  They are refused at config parse with a named
// message rather than classified as loopback and then failing the bind.
TEST(ApiBindClassificationTest, HostnamesAndIpv6AreNotIpv4Literals) {
  for (const char* addr : {"localhost", "::1", "[::1]",
                           "::", "::ffff:127.0.0.1", "example.internal"}) {
    EXPECT_FALSE(IsIpv4LiteralApiBind(addr)) << addr;
    EXPECT_FALSE(IsLoopbackApiBind(addr)) << addr;
  }
  EXPECT_TRUE(IsIpv4LiteralApiBind(""));
  EXPECT_TRUE(IsIpv4LiteralApiBind("127.0.0.1"));
  EXPECT_TRUE(IsIpv4LiteralApiBind("0.0.0.0"));
}

TEST(ApiBindClassificationTest, WildcardsAndRoutableAddressesAreRemote) {
  EXPECT_FALSE(IsLoopbackApiBind("0.0.0.0"));
  EXPECT_FALSE(IsLoopbackApiBind("10.0.0.5"));
  EXPECT_FALSE(IsLoopbackApiBind("192.168.1.10"));
  EXPECT_FALSE(IsLoopbackApiBind("203.0.113.7"));
  EXPECT_FALSE(IsLoopbackApiBind("128.0.0.1"));
  EXPECT_FALSE(IsLoopbackApiBind("126.255.255.255"));
}

// An address the classifier cannot parse must never be assumed safe: the
// refusal is the correct outcome for a typo, not a silent public bind.
TEST(ApiBindClassificationTest, UnparseableAddressesAreTreatedAsRemote) {
  EXPECT_FALSE(IsLoopbackApiBind("127.0.0"));
  EXPECT_FALSE(IsLoopbackApiBind("127.0.0.1.1"));
  EXPECT_FALSE(IsLoopbackApiBind("127.0.0.1 "));
  EXPECT_FALSE(IsLoopbackApiBind("127.0.0.256"));
  EXPECT_FALSE(IsLoopbackApiBind("127.0.0.0001"));
  EXPECT_FALSE(IsLoopbackApiBind("127.0.0.x"));
}

// The regression test for the defect: an empty auth token used to make
// CheckAuth return "no error" for EVERY request, which left cache purge and
// the whole read surface open whenever the API was on without a token.
class HttpServerFailClosedTest : public HttpServerTest {
 protected:
  void SetUp() override {
    HttpServerTest::SetUp();
    config_.allow_unauthenticated = false;  // no token, not opened
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
    server_->AddRoute("GET", "/v1/cache/urls", [](const HttpRequest&) {
      return HttpResponse().Json("{\"urls\":[\"secret\"]}");
    });
    server_->AddRoute("POST", "/v1/cache/purge", [this](const HttpRequest&) {
      purged_ = true;
      return HttpResponse().Json("{\"purged\":true}");
    });
    server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
      return HttpResponse().Json("{\"status\":\"ok\"}");
    });
    ASSERT_TRUE(server_->Start());
    StartLoopThread();
  }

  bool purged_ = false;
};

TEST_F(HttpServerFailClosedTest, TokenlessReadIsRefused) {
  std::string response =
      SendRequest("GET /v1/cache/urls HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_NE(response.find("401"), std::string::npos) << response;
  EXPECT_EQ(response.find("secret"), std::string::npos) << response;
}

// The load-bearing leg: an unauthenticated purge must not reach the handler.
TEST_F(HttpServerFailClosedTest, TokenlessPurgeIsRefusedAndDoesNotRun) {
  std::string response = SendRequest(
      "POST /v1/cache/purge HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: application/json\r\n"
      "X-Requested-With: XMLHttpRequest\r\n"
      "Content-Length: 24\r\n\r\n{\"confirm\":\"purge-all\"}\n");
  EXPECT_NE(response.find("401"), std::string::npos) << response;
  EXPECT_FALSE(purged_) << "the purge handler ran without a credential";
}

// Monitoring depends on it, so /v1/health stays unauthenticated — and must
// carry nothing an unauthenticated caller should not see.
TEST_F(HttpServerFailClosedTest, HealthStaysUnauthenticated) {
  std::string response =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_NE(response.find("200"), std::string::npos) << response;
}

// read_open is now an explicit choice, so it opens GET even with a token —
// and never opens anything on its own when no token is configured.
class HttpServerReadOpenTest : public HttpServerTest {
 protected:
  void SetUp() override {
    HttpServerTest::SetUp();
    config_.allow_unauthenticated = false;
    config_.auth_token = "tok";
    config_.read_open = false;
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
    server_->AddRoute("GET", "/v1/cache/urls", [](const HttpRequest&) {
      return HttpResponse().Json("{\"urls\":[]}");
    });
    ASSERT_TRUE(server_->Start());
    StartLoopThread();
  }
};

TEST_F(HttpServerReadOpenTest, TokenWithoutReadOpenStillRequiresBearer) {
  EXPECT_NE(
      SendRequest("GET /v1/cache/urls HTTP/1.1\r\nHost: x\r\n\r\n").find("401"),
      std::string::npos);
  EXPECT_NE(SendRequest("GET /v1/cache/urls HTTP/1.1\r\nHost: x\r\n"
                        "Authorization: Bearer wrong\r\n\r\n")
                .find("403"),
            std::string::npos);
  EXPECT_NE(SendRequest("GET /v1/cache/urls HTTP/1.1\r\nHost: x\r\n"
                        "Authorization: Bearer tok\r\n\r\n")
                .find("200"),
            std::string::npos);
}

#ifndef _WIN32
// The unix-socket transport: HTTP/1.1 over AF_UNIX, no TCP listener at all,
// and a mode the daemon sets explicitly rather than inheriting from a umask.
class HttpServerUnixSocketTest : public HttpServerTest {
 protected:
  void SetUp() override {
    HttpServerTest::SetUp();
    socket_path_ = absl::StrCat("/tmp/pagespeed-api-test-", ::getpid(), "-",
                                ++counter_, ".sock");
    ::unlink(socket_path_.c_str());
    config_.socket_path = socket_path_;
    config_.port = 0;
    // Deliberately NOT allow_unauthenticated: the transport is the credential
    // on this path, and these tests assert exactly that.
    config_.allow_unauthenticated = false;
    server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
    server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
      return HttpResponse().Json("{\"status\":\"ok\"}");
    });
    ASSERT_TRUE(server_->Start());
    StartLoopThread();
  }

  void TearDown() override {
    HttpServerTest::TearDown();
    ::unlink(socket_path_.c_str());
  }

  std::string SendOverSocket(const std::string& request) {
    std::string wire = test::InjectConnectionClose(request);
    int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return "";
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(),
                 sizeof(addr.sun_path) - 1);
    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
      ::close(sock);
      return "";
    }
    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    (void)sent;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::string response;
    char buf[4096];
    while (true) {
      ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
      if (n <= 0) break;
      response.append(buf, static_cast<size_t>(n));
    }
    ::close(sock);
    return response;
  }

  std::string socket_path_;
  static int counter_;
};
int HttpServerUnixSocketTest::counter_ = 0;

TEST_F(HttpServerUnixSocketTest, ServesHttpOverTheSocketAndBindsNoPort) {
  EXPECT_EQ(server_->bound_port(), 0)
      << "the unix transport must bind no TCP port at all";
  struct stat st{};
  ASSERT_EQ(::stat(socket_path_.c_str(), &st), 0);
  // Cast through unsigned explicitly: mode_t is 16-bit on macOS, so the
  // masked value promotes to int there and to unsigned int on Linux.
  EXPECT_EQ(static_cast<unsigned>(st.st_mode & 0777u), 0660u)
      << "the API socket mode is set explicitly, never umask-derived";
  EXPECT_EQ(static_cast<unsigned>(st.st_mode & static_cast<mode_t>(S_IWOTH)),
            0u);

  std::string response =
      SendOverSocket("GET /v1/health HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_NE(response.find("200"), std::string::npos) << response;
  EXPECT_NE(response.find("\"status\":\"ok\""), std::string::npos) << response;
}

// Reaching the socket means passing its 0660 group
// scope, so no bearer token is asked for on top -- that removal is the whole
// reason the socket transport was chosen over 127.0.0.1 + token.
TEST_F(HttpServerUnixSocketTest, NoBearerNeededOverTheSocket) {
  server_->AddRoute("GET", "/v1/stats", [](const HttpRequest&) {
    return HttpResponse().Json("{\"received\":1}");
  });
  std::string response =
      SendOverSocket("GET /v1/stats HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_NE(response.find("200"), std::string::npos) << response;
  EXPECT_NE(response.find("\"received\""), std::string::npos) << response;
}

// Over the socket the filesystem is the credential for every route, the
// token-only ones included: the read-open exception is a TCP rule.
TEST_F(HttpServerUnixSocketTest, TokenOnlyRouteNeedsNoBearerOverTheSocket) {
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  ::unlink(socket_path_.c_str());
  config_.auth_token = "a-perfectly-good-token";
  config_.read_open = true;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute(
      "GET", "/v1/logs",
      [](const HttpRequest&) {
        return HttpResponse().Json("{\"entries\":[]}");
      },
      RouteAuth::kToken);
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  std::string response =
      SendOverSocket("GET /v1/logs HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_NE(response.find("200"), std::string::npos) << response;
  EXPECT_NE(response.find("{\"entries\":[]}"), std::string::npos) << response;
}

// The mutating half of the same rule: no bearer token, but the CSRF header is
// still required — see the next test for why.
TEST_F(HttpServerUnixSocketTest, PurgeRunsOverTheSocketWithoutACredential) {
  bool purged = false;
  server_->AddRoute("POST", "/v1/cache/purge", [&purged](const HttpRequest&) {
    purged = true;
    return HttpResponse().Json("{\"purged\":true}");
  });
  std::string response = SendOverSocket(
      "POST /v1/cache/purge HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: application/json\r\n"
      "X-Requested-With: XMLHttpRequest\r\n"
      "Content-Length: "
      "32\r\n\r\n{\"scope\":\"all\",\"confirm\":\"purge-all\"}");
  EXPECT_NE(response.find("200"), std::string::npos) << response;
  EXPECT_TRUE(purged) << "the socket's group scope IS the credential";
}

// The CSRF backstop stays ON over the socket, and there it is the ONLY control
// left: the intended topology reverse-proxies a console onto this socket, so a
// browser CAN reach it cross-origin — with no credential anywhere in the
// chain, because the socket transport deliberately asks for none. Without this
// header requirement that is a one-request cache purge from whatever page the
// operator happens to have open.
TEST_F(HttpServerUnixSocketTest, MutatingRequestOverSocketNeedsTheCsrfHeader) {
  bool purged = false;
  server_->AddRoute("POST", "/v1/cache/purge", [&purged](const HttpRequest&) {
    purged = true;
    return HttpResponse().Json("{\"purged\":true}");
  });
  std::string response = SendOverSocket(
      "POST /v1/cache/purge HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: "
      "32\r\n\r\n{\"scope\":\"all\",\"confirm\":\"purge-all\"}");
  EXPECT_NE(response.find("400"), std::string::npos)
      << "expected the CSRF refusal, got: " << response;
  EXPECT_NE(response.find("X-Requested-With"), std::string::npos) << response;
  EXPECT_FALSE(purged) << "a header-less cross-origin POST reached the handler";
}

// A GET changes nothing, so the header is not demanded on reads — the console
// must still be able to poll over the socket without ceremony.
TEST_F(HttpServerUnixSocketTest, ReadsOverTheSocketNeedNoCsrfHeader) {
  server_->AddRoute("GET", "/v1/config", [](const HttpRequest&) {
    return HttpResponse().Json("{\"ok\":true}");
  });
  EXPECT_NE(
      SendOverSocket("GET /v1/config HTTP/1.1\r\nHost: x\r\n\r\n").find("200"),
      std::string::npos);
}

// A token configured alongside the socket does not resurrect the bearer
// requirement -- the packaged install has one in daemon.env for the TCP case
// and must not be made to look broken on the socket.
TEST_F(HttpServerUnixSocketTest, ATokenDoesNotReimposeAuthOverTheSocket) {
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  config_.auth_token = "a-perfectly-good-token";
  config_.allow_unauthenticated = false;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  server_->AddRoute("GET", "/v1/stats", [](const HttpRequest&) {
    return HttpResponse().Json("{\"received\":1}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();
  EXPECT_NE(
      SendOverSocket("GET /v1/stats HTTP/1.1\r\nHost: x\r\n\r\n").find("200"),
      std::string::npos);
}

// The TCP side is untouched: a configured token is still required there.
TEST_F(HttpServerReadOpenTest, TcpStillRequiresTheBearerWithATokenSet) {
  EXPECT_NE(
      SendRequest("GET /v1/cache/urls HTTP/1.1\r\nHost: x\r\n\r\n").find("401"),
      std::string::npos);
}

// The socket must never accidentally widen the TCP transport: a pipe server
// and a TCP server built from the same config disagree, on purpose.
TEST_F(HttpServerFailClosedTest, TcpTokenlessIsStillRefused) {
  EXPECT_NE(
      SendRequest("GET /v1/cache/urls HTTP/1.1\r\nHost: x\r\n\r\n").find("401"),
      std::string::npos);
}

TEST_F(HttpServerUnixSocketTest, StopRemovesTheSocketFile) {
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  struct stat st{};
  EXPECT_NE(::stat(socket_path_.c_str(), &st), 0)
      << "a stale socket file would make the next start bind-fail";
}
#endif  // !_WIN32

// =============================================================================
// Async routes (AddAsyncRoute): the response is produced later
// =============================================================================

// Runs `fn` on the event-loop thread (the one thread allowed to call a
// Responder) by way of a uv_async_t.
class HttpServerAsyncRouteTest : public HttpServerTest {
 protected:
  void SetUp() override {
    HttpServerTest::SetUp();
    uv_async_init(loop_, &fire_async_, [](uv_async_t* h) {
      auto* self = static_cast<HttpServerAsyncRouteTest*>(h->data);
      std::function<void()> fn;
      {
        std::lock_guard<std::mutex> lock(self->fire_mu_);
        fn = std::move(self->fire_fn_);
        self->fire_fn_ = nullptr;
      }
      if (fn) fn();
      self->fired_.store(true);
    });
    fire_async_.data = this;
  }

  void TearDown() override {
    if (loop_thread_.joinable()) StopLoopThread();
    uv_close(reinterpret_cast<uv_handle_t*>(&fire_async_), nullptr);
    HttpServerTest::TearDown();
  }

  // Runs `fn` on the loop thread and waits until it has run.
  void FireOnLoop(std::function<void()> fn) {
    fired_.store(false);
    {
      std::lock_guard<std::mutex> lock(fire_mu_);
      fire_fn_ = std::move(fn);
    }
    uv_async_send(&fire_async_);
    ASSERT_TRUE(WaitFor([this] { return fired_.load(); }));
  }

  static bool WaitFor(const std::function<bool()>& pred,
                      int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
  }

  static std::string ReadAll(int sock) {
    std::string response;
    char buf[4096];
    while (true) {
      ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
      if (n <= 0) break;
      response.append(buf, static_cast<size_t>(n));
    }
    return response;
  }

  // Reads until `needle` appears or the socket closes / times out.
  static std::string ReadUntil(int sock, const std::string& needle) {
    std::string response;
    char buf[4096];
    while (response.find(needle) == std::string::npos) {
      ssize_t n = test::SocketRead(sock, buf, sizeof(buf));
      if (n <= 0) break;
      response.append(buf, static_cast<size_t>(n));
    }
    return response;
  }

  // Registers an async route that parks its Responder in `responder_`.
  void AddParkedRoute(const std::string& path) {
    server_->AddAsyncRoute("GET", path,
                           [this](const HttpRequest&, Responder respond) {
                             responder_ = std::move(respond);
                             parked_.store(true);
                           });
  }

  uv_async_t fire_async_;
  std::mutex fire_mu_;
  std::function<void()> fire_fn_;
  std::atomic<bool> fired_{false};
  std::atomic<bool> parked_{false};
  Responder responder_;  // Touched on the loop thread only.
};

TEST_F(HttpServerAsyncRouteTest, AsyncRouteRespondsLater) {
  AddParkedRoute("/v1/slow");
  server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
    return HttpResponse().Json("{\"status\":\"ok\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int sock = test::ConnectTcp(server_->bound_port(), 2);
  ASSERT_GE(sock, 0);
  const std::string req =
      "GET /v1/slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  test::SocketWrite(sock, req.data(), req.size());
  ASSERT_TRUE(WaitFor([this] { return parked_.load(); }));

  // The handler has not answered, yet another connection is served.
  std::string health =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(health.find("\"status\":\"ok\""), std::string::npos) << health;

  FireOnLoop([this] {
    responder_(HttpResponse().Json("{\"late\":true}"));
    responder_ = nullptr;
  });
  std::string response = ReadAll(sock);
  test::CloseSocket(sock);
  EXPECT_NE(response.find("200 OK"), std::string::npos) << response;
  EXPECT_NE(response.find("{\"late\":true}"), std::string::npos) << response;
}

TEST_F(HttpServerAsyncRouteTest, AsyncRouteAnsweringInlineStillResponds) {
  server_->AddAsyncRoute("GET", "/v1/inline",
                         [](const HttpRequest&, Responder respond) {
                           respond(HttpResponse().Json("{\"inline\":true}"));
                         });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();
  std::string response =
      SendRequest("GET /v1/inline HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(response.find("{\"inline\":true}"), std::string::npos) << response;
}

TEST_F(HttpServerAsyncRouteTest, AsyncRouteResponderAfterClientCloseIsDropped) {
  AddParkedRoute("/v1/slow");
  server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
    return HttpResponse().Json("{\"status\":\"ok\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int sock = test::ConnectTcp(server_->bound_port(), 2);
  ASSERT_GE(sock, 0);
  const std::string req = "GET /v1/slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock, req.data(), req.size());
  ASSERT_TRUE(WaitFor([this] { return parked_.load(); }));
  test::CloseSocket(sock);

  // The client is gone; answering must neither crash nor disturb the server.
  FireOnLoop([this] {
    responder_(HttpResponse().Json("{\"late\":true}"));
    responder_ = nullptr;
  });
  std::string health =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(health.find("\"status\":\"ok\""), std::string::npos) << health;
}

TEST_F(HttpServerAsyncRouteTest, AsyncRouteKeepsPipelineOrder) {
  AddParkedRoute("/v1/a");
  std::atomic<int> b_calls{0};
  server_->AddRoute("GET", "/v1/b", [&b_calls](const HttpRequest&) {
    ++b_calls;
    return HttpResponse().Json("{\"which\":\"b\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int sock = test::ConnectTcp(server_->bound_port(), 2);
  ASSERT_GE(sock, 0);
  const std::string pipelined =
      "GET /v1/a HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "GET /v1/b HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  test::SocketWrite(sock, pipelined.data(), pipelined.size());
  ASSERT_TRUE(WaitFor([this] { return parked_.load(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(b_calls.load(), 0) << "the second request must wait for the first";

  FireOnLoop([this] {
    responder_(HttpResponse().Json("{\"which\":\"a\"}"));
    responder_ = nullptr;
  });
  std::string response = ReadAll(sock);
  test::CloseSocket(sock);
  auto a = response.find("{\"which\":\"a\"}");
  auto b = response.find("{\"which\":\"b\"}");
  ASSERT_NE(a, std::string::npos) << response;
  ASSERT_NE(b, std::string::npos) << response;
  EXPECT_LT(a, b);
  EXPECT_EQ(b_calls.load(), 1);
}

TEST_F(HttpServerAsyncRouteTest, AsyncRouteTimeoutClosesAndDropsLateResponse) {
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.request_timeout_ms = 200;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  AddParkedRoute("/v1/slow");
  server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
    return HttpResponse().Json("{\"status\":\"ok\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int sock = test::ConnectTcp(server_->bound_port(), 2);
  ASSERT_GE(sock, 0);
  const std::string req = "GET /v1/slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock, req.data(), req.size());
  ASSERT_TRUE(WaitFor([this] { return parked_.load(); }));

  // The handler never answers: the timeout answers 504 and closes.
  std::string response = ReadAll(sock);
  test::CloseSocket(sock);
  EXPECT_NE(response.find("504"), std::string::npos) << response;

  // A response arriving after the close is dropped.
  FireOnLoop([this] {
    responder_(HttpResponse().Json("{\"late\":true}"));
    responder_ = nullptr;
  });
  std::string health =
      SendRequest("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(health.find("\"status\":\"ok\""), std::string::npos) << health;
}

TEST_F(HttpServerAsyncRouteTest, AsyncRouteGetsFullTimeoutOnIdleKeepAlive) {
  server_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  server_.reset();
  config_.request_timeout_ms = 3000;
  server_ = std::make_unique<HttpServer>(loop_, config_, handler_.get());
  AddParkedRoute("/v1/slow");
  server_->AddRoute("GET", "/v1/health", [](const HttpRequest&) {
    return HttpResponse().Json("{\"status\":\"ok\"}");
  });
  ASSERT_TRUE(server_->Start());
  StartLoopThread();

  int sock = test::ConnectTcp(server_->bound_port(), 10);
  ASSERT_GE(sock, 0);
  const std::string health =
      "GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock, health.data(), health.size());
  EXPECT_NE(ReadUntil(sock, "\"status\":\"ok\"").find("200"),
            std::string::npos);

  // Idle for most of the receive budget, then send the async request.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  const std::string slow = "GET /v1/slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test::SocketWrite(sock, slow.data(), slow.size());
  ASSERT_TRUE(WaitFor([this] { return parked_.load(); }));

  // Answer 1 s after the original receive deadline has passed (2000 + 2000 > 3000)
  // but well inside a fresh full timeout: the answer must be delivered.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  FireOnLoop([this] {
    responder_(HttpResponse().Json("{\"late\":true}"));
    responder_ = nullptr;
  });
  std::string response = ReadUntil(sock, "\"late\":true");
  test::CloseSocket(sock);
  EXPECT_NE(response.find("200"), std::string::npos) << response;
  EXPECT_EQ(response.find("504"), std::string::npos) << response;
}

}  // namespace
}  // namespace pagespeed
