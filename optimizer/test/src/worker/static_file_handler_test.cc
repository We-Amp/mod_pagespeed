// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Static File Handler Unit Tests
//
// Tests: content-type detection, ETag computation, file loading,
// route serving, SPA fallback, 304 responses, CSP headers.

#include "src/worker/static_file_handler.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"
#include "uv.h"

namespace pagespeed {
namespace {

// ===================================================================
// Content-Type detection tests
// ===================================================================

TEST(ContentTypeTest, HtmlFiles) {
  EXPECT_EQ(ContentTypeForExtension("index.html"), "text/html; charset=utf-8");
  EXPECT_EQ(ContentTypeForExtension("page.htm"), "text/html; charset=utf-8");
}

TEST(ContentTypeTest, JsFiles) {
  EXPECT_EQ(ContentTypeForExtension("app.js"), "application/javascript");
  EXPECT_EQ(ContentTypeForExtension("module.mjs"), "application/javascript");
}

TEST(ContentTypeTest, CssFiles) {
  EXPECT_EQ(ContentTypeForExtension("style.css"), "text/css");
}

TEST(ContentTypeTest, JsonFiles) {
  EXPECT_EQ(ContentTypeForExtension("data.json"), "application/json");
}

TEST(ContentTypeTest, ImageFiles) {
  EXPECT_EQ(ContentTypeForExtension("logo.svg"), "image/svg+xml");
  EXPECT_EQ(ContentTypeForExtension("photo.png"), "image/png");
  EXPECT_EQ(ContentTypeForExtension("photo.jpg"), "image/jpeg");
  EXPECT_EQ(ContentTypeForExtension("icon.ico"), "image/x-icon");
  EXPECT_EQ(ContentTypeForExtension("image.webp"), "image/webp");
}

TEST(ContentTypeTest, FontFiles) {
  EXPECT_EQ(ContentTypeForExtension("font.woff2"), "font/woff2");
  EXPECT_EQ(ContentTypeForExtension("font.woff"), "font/woff");
}

TEST(ContentTypeTest, UnknownExtension) {
  EXPECT_EQ(ContentTypeForExtension("data.xyz"), "application/octet-stream");
  EXPECT_EQ(ContentTypeForExtension("noext"), "application/octet-stream");
}

// ===================================================================
// StaticFileCache tests
// ===================================================================

class StaticFileCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    handler_ = std::make_unique<NullMessageHandler>();
    // Create a temp directory with test files.
    tmp_dir_ = test::MakeTempDir();
    std::filesystem::create_directories(tmp_dir_);

    WriteFile("index.html", "<html><body>Hello</body></html>");
    WriteFile("app.js", "console.log('hello');");
    WriteFile("style.css", "body { color: red; }");

    std::filesystem::create_directories(tmp_dir_ / "assets");
    WriteFile("assets/logo.svg", "<svg></svg>");
  }

  void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

  void WriteFile(const std::string& rel_path, const std::string& content) {
    auto full = tmp_dir_ / rel_path;
    std::ofstream ofs(full, std::ios::binary);
    ofs.write(content.data(), content.size());
  }

  std::filesystem::path tmp_dir_;
  std::unique_ptr<NullMessageHandler> handler_;
};

TEST_F(StaticFileCacheTest, LoadAndLookup) {
  StaticFileCache cache;
  ASSERT_TRUE(cache.Load(tmp_dir_.string(),
                         static_cast<size_t>(10) * 1024 * 1024,
                         handler_.get()));

  EXPECT_EQ(cache.file_count(), 4u);
  EXPECT_GT(cache.total_bytes(), 0u);

  auto* html = cache.Lookup("index.html");
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->content, "<html><body>Hello</body></html>");
  EXPECT_EQ(html->content_type, "text/html; charset=utf-8");
  EXPECT_FALSE(html->etag.empty());

  auto* js = cache.Lookup("app.js");
  ASSERT_NE(js, nullptr);
  EXPECT_EQ(js->content_type, "application/javascript");

  auto* svg = cache.Lookup("assets/logo.svg");
  ASSERT_NE(svg, nullptr);
  EXPECT_EQ(svg->content_type, "image/svg+xml");
}

TEST_F(StaticFileCacheTest, IndexHtml) {
  StaticFileCache cache;
  ASSERT_TRUE(cache.Load(tmp_dir_.string(),
                         static_cast<size_t>(10) * 1024 * 1024,
                         handler_.get()));

  auto* index = cache.IndexHtml();
  ASSERT_NE(index, nullptr);
  EXPECT_NE(index->content.find("<html>"), std::string::npos);
}

TEST_F(StaticFileCacheTest, LookupNotFound) {
  StaticFileCache cache;
  ASSERT_TRUE(cache.Load(tmp_dir_.string(),
                         static_cast<size_t>(10) * 1024 * 1024,
                         handler_.get()));
  EXPECT_EQ(cache.Lookup("nonexistent.js"), nullptr);
}

TEST_F(StaticFileCacheTest, SizeLimitExceeded) {
  StaticFileCache cache;
  // Set a tiny limit.
  EXPECT_FALSE(cache.Load(tmp_dir_.string(), 10, handler_.get()));
}

TEST_F(StaticFileCacheTest, InvalidDirectory) {
  StaticFileCache cache;
  EXPECT_FALSE(cache.Load("/nonexistent/path",
                          static_cast<size_t>(10) * 1024 * 1024,
                          handler_.get()));
}

TEST_F(StaticFileCacheTest, EtagDeterministic) {
  StaticFileCache cache1;
  ASSERT_TRUE(cache1.Load(tmp_dir_.string(),
                          static_cast<size_t>(10) * 1024 * 1024,
                          handler_.get()));
  StaticFileCache cache2;
  ASSERT_TRUE(cache2.Load(tmp_dir_.string(),
                          static_cast<size_t>(10) * 1024 * 1024,
                          handler_.get()));

  auto* e1 = cache1.Lookup("index.html");
  auto* e2 = cache2.Lookup("index.html");
  ASSERT_NE(e1, nullptr);
  ASSERT_NE(e2, nullptr);
  EXPECT_EQ(e1->etag, e2->etag);
}

// ===================================================================
// Integration test: HTTP route serving
// ===================================================================

class StaticFileRouteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();

    // Create test files.
    tmp_dir_ = test::MakeTempDir();
    std::filesystem::create_directories(tmp_dir_);
    WriteFile("index.html", "<html>SPA</html>");
    WriteFile("app.abc123.js", "var x=1;");
    WriteFile("style.css", "body{}");

    cache_ = std::make_unique<StaticFileCache>();
    ASSERT_TRUE(cache_->Load(tmp_dir_.string(),
                             static_cast<size_t>(10) * 1024 * 1024,
                             handler_.get()));

    HttpServerConfig config;
    config.port = 0;
    // An empty token now fails closed; these fixtures
    // exercise the routes, not the credential gate.
    config.allow_unauthenticated = true;
    server_ = std::make_unique<HttpServer>(loop_, config, handler_.get());
    RegisterConsoleRoute(*server_, *cache_);
    ASSERT_TRUE(server_->Start());
    port_ = server_->bound_port();
    StartLoopThread();
  }

  void TearDown() override {
    StopLoopThread();
    server_->Stop();
    uv_run(loop_, UV_RUN_DEFAULT);
    server_.reset();
    uv_loop_close(loop_);
    delete loop_;
    std::filesystem::remove_all(tmp_dir_);
  }

  void WriteFile(const std::string& name, const std::string& content) {
    std::ofstream ofs(tmp_dir_ / name, std::ios::binary);
    ofs.write(content.data(), content.size());
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

  std::string SendRequest(const std::string& request) const {
#ifdef PS_SANITIZER_BUILD
    constexpr int kTimeout = 10;
#else
    constexpr int kTimeout = 2;
#endif
    int sock = test::ConnectTcp(port_, kTimeout);
    if (sock < 0) return "";

    ssize_t sent = test::SocketWrite(sock, request.data(), request.size());
    if (sent != static_cast<ssize_t>(request.size())) {
      test::CloseSocket(sock);
      return "";
    }

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

  std::string ExtractBody(const std::string& response) {
    auto pos = response.find("\r\n\r\n");
    if (pos == std::string::npos) return "";
    return response.substr(pos + 4);
  }

  uv_loop_t* loop_ = nullptr;
  int port_ = 0;
  std::filesystem::path tmp_dir_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<StaticFileCache> cache_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
};

TEST_F(StaticFileRouteTest, ServeIndexHtml) {
  auto resp = SendRequest("GET /console/ HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("text/html"), std::string::npos);
  EXPECT_NE(resp.find("<html>SPA</html>"), std::string::npos);
  EXPECT_NE(resp.find("no-cache"), std::string::npos);
  EXPECT_NE(resp.find("Content-Security-Policy"), std::string::npos);
  EXPECT_EQ(resp.find("onfastspring"), std::string::npos);
  EXPECT_NE(resp.find("frame-src https://modpagespeed.com"), std::string::npos);
}

TEST_F(StaticFileRouteTest, ServeJsFile) {
  auto resp = SendRequest(
      "GET /console/app.abc123.js HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("application/javascript"), std::string::npos);
  EXPECT_NE(resp.find("var x=1;"), std::string::npos);
  EXPECT_NE(resp.find("immutable"), std::string::npos);
}

TEST_F(StaticFileRouteTest, SpaFallback) {
  // Request a path that doesn't match any file → SPA fallback to index.html.
  auto resp = SendRequest(
      "GET /console/dashboard/settings HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("<html>SPA</html>"), std::string::npos);
  EXPECT_NE(resp.find("no-cache"), std::string::npos);
}

TEST_F(StaticFileRouteTest, EtagAndConditionalRequest) {
  // First request to get the ETag.
  auto resp1 =
      SendRequest("GET /console/style.css HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp1.find("200 OK"), std::string::npos);

  // Extract ETag.
  auto etag_pos = resp1.find("ETag: ");
  ASSERT_NE(etag_pos, std::string::npos);
  auto etag_end = resp1.find("\r\n", etag_pos);
  std::string etag = resp1.substr(etag_pos + 6, etag_end - etag_pos - 6);

  // Conditional request.
  std::string req =
      "GET /console/style.css HTTP/1.1\r\nHost: localhost\r\n"
      "If-None-Match: " +
      etag + "\r\n\r\n";
  auto resp2 = SendRequest(req);
  EXPECT_NE(resp2.find("304"), std::string::npos);
}

TEST_F(StaticFileRouteTest, NosniffHeader) {
  auto resp = SendRequest(
      "GET /console/app.abc123.js HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("X-Content-Type-Options: nosniff"), std::string::npos);
}

TEST_F(StaticFileRouteTest, SecurityHeadersDisabled) {
  // Rebuild server with security headers disabled.
  StopLoopThread();
  server_->Stop();
  uv_run(loop_, UV_RUN_DEFAULT);
  server_.reset();

  HttpServerConfig config;
  config.port = 0;
  // An empty token now fails closed; these fixtures
  // exercise the routes, not the credential gate.
  config.allow_unauthenticated = true;
  server_ = std::make_unique<HttpServer>(loop_, config, handler_.get());
  RegisterConsoleRoute(*server_, *cache_, /*security_headers=*/false);
  ASSERT_TRUE(server_->Start());
  port_ = server_->bound_port();
  StartLoopThread();

  auto resp = SendRequest("GET /console/ HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("<html>SPA</html>"), std::string::npos);
  // Console-specific security headers should be absent.
  EXPECT_EQ(resp.find("Content-Security-Policy"), std::string::npos);
  EXPECT_EQ(resp.find("X-Frame-Options"), std::string::npos);
  // Baseline security defaults from Serialize() should still be present.
  EXPECT_NE(resp.find("X-Content-Type-Options: nosniff"), std::string::npos);
  // ETag and Cache-Control should still be present.
  EXPECT_NE(resp.find("ETag"), std::string::npos);
  EXPECT_NE(resp.find("no-cache"), std::string::npos);
}

// ========== Coverage: nonexistent directory and empty cache ==========

TEST_F(StaticFileCacheTest, LoadNonexistentDirectory) {
  StaticFileCache cache;
  // Loading from a path that doesn't exist should fail.
  EXPECT_FALSE(cache.Load("/nonexistent/path/that/does/not/exist",
                          static_cast<size_t>(10) * 1024 * 1024,
                          handler_.get()));
  EXPECT_EQ(cache.file_count(), 0u);
}

class ConsoleNotConfiguredTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();

    // Create an empty StaticFileCache (no files loaded).
    cache_ = std::make_unique<StaticFileCache>();

    HttpServerConfig config;
    config.port = 0;
    // An empty token now fails closed; these fixtures
    // exercise the routes, not the credential gate.
    config.allow_unauthenticated = true;
    server_ = std::make_unique<HttpServer>(loop_, config, handler_.get());
    RegisterConsoleRoute(*server_, *cache_);
    ASSERT_TRUE(server_->Start());
    port_ = server_->bound_port();
    loop_running_ = true;
    loop_thread_ = std::thread([this] {
      while (loop_running_) {
        uv_run(loop_, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  void TearDown() override {
    loop_running_ = false;
    if (loop_thread_.joinable()) loop_thread_.join();
    server_->Stop();
    uv_run(loop_, UV_RUN_DEFAULT);
    server_.reset();
    uv_loop_close(loop_);
    delete loop_;
  }

  std::string SendRequest(const std::string& request) const {
#ifdef PS_SANITIZER_BUILD
    constexpr int kTimeout = 10;
#else
    constexpr int kTimeout = 2;
#endif
    int sock = test::ConnectTcp(port_, kTimeout);
    if (sock < 0) return "";

    ssize_t sent = test::SocketWrite(sock, request.data(), request.size());
    if (sent != static_cast<ssize_t>(request.size())) {
      test::CloseSocket(sock);
      return "";
    }

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
  int port_ = 0;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<StaticFileCache> cache_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
};

TEST_F(ConsoleNotConfiguredTest, SpaFallbackReturns404WhenNoIndex) {
  // When no console is configured (no files loaded), SPA fallback
  // should return 404 "Console not configured".
  auto resp = SendRequest(
      "GET /console/some/route HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos);
  EXPECT_NE(resp.find("Console not configured"), std::string::npos);
}

// ===================================================================
// Additional content-type coverage
// ===================================================================

TEST(ContentTypeTest, JpegAlternate) {
  EXPECT_EQ(ContentTypeForExtension("photo.jpeg"), "image/jpeg");
}

TEST(ContentTypeTest, GifFiles) {
  EXPECT_EQ(ContentTypeForExtension("anim.gif"), "image/gif");
}

TEST(ContentTypeTest, AvifFiles) {
  EXPECT_EQ(ContentTypeForExtension("image.avif"), "image/avif");
}

TEST(ContentTypeTest, TtfFiles) {
  EXPECT_EQ(ContentTypeForExtension("font.ttf"), "font/ttf");
}

TEST(ContentTypeTest, MapFiles) {
  EXPECT_EQ(ContentTypeForExtension("app.js.map"), "application/json");
}

TEST(ContentTypeTest, TxtFiles) {
  EXPECT_EQ(ContentTypeForExtension("robots.txt"), "text/plain");
}

TEST(ContentTypeTest, XmlFiles) {
  EXPECT_EQ(ContentTypeForExtension("sitemap.xml"), "application/xml");
}

TEST(ContentTypeTest, WebmanifestFiles) {
  EXPECT_EQ(ContentTypeForExtension("site.webmanifest"),
            "application/manifest+json");
}

// ===================================================================
// Load() error paths: unreadable file
// ===================================================================

TEST_F(StaticFileCacheTest, LoadSkipsUnreadableFile) {
#ifdef _WIN32
  GTEST_SKIP() << "Skipping: Windows NTFS ACLs don't support Unix-style "
                  "permission removal via std::filesystem::perms::none";
#else
  if (getuid() == 0) {
    GTEST_SKIP() << "Skipping: running as root (chmod has no effect)";
  }
#endif
  // Create a file then make it unreadable to trigger fopen failure.
  auto unreadable_path = tmp_dir_ / "secret.js";
  {
    std::ofstream ofs(unreadable_path, std::ios::binary);
    ofs << "var secret=1;";
  }
  // Remove read permission.
  std::filesystem::permissions(unreadable_path, std::filesystem::perms::none);

  StaticFileCache cache;
  // Load should succeed but skip the unreadable file.
  ASSERT_TRUE(cache.Load(tmp_dir_.string(),
                         static_cast<size_t>(10) * 1024 * 1024,
                         handler_.get()));

  // The unreadable file should not be in the cache.
  EXPECT_EQ(cache.Lookup("secret.js"), nullptr);

  // Other files should still be loaded (index.html, app.js, style.css,
  // assets/logo.svg = 4 files from SetUp).
  EXPECT_EQ(cache.file_count(), 4u);

  // Restore permissions for cleanup.
  std::filesystem::permissions(
      unreadable_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

}  // namespace
}  // namespace pagespeed
