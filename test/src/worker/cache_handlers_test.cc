// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Cache API Handlers Unit Tests
//
// Tests cache inspection and mutation endpoints using a real PageSpeedCache
// backed by a temporary file, and an HttpServer for integration testing.

#include "src/worker/cache_handlers.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"
#include "nlohmann/json.hpp"
#include "src/worker/known_sites.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

class CacheHandlersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a temporary cache volume.
    tmp_dir_ = test::MakeTempDir();
    std::filesystem::create_directories(tmp_dir_);
    cache_path_ = (tmp_dir_ / "cache.vol").string();

    PageSpeedCacheConfig cache_config;
    cache_config.volume_path = cache_path_;
    cache_config.volume_size = static_cast<uint64_t>(16 * 1024 * 1024);  // 16MB
    cache_config.ram_cache_size = 0;  // Disable RAM cache
    auto cache_result = PageSpeedCache::Create(cache_config);
    ASSERT_TRUE(cache_result.has_value()) << "Cache creation failed";
    cache_ = std::move(*cache_result);

    url_registry_ = std::make_unique<UrlRegistry>(10000);

    // Set up the context.
    ctx_ = std::make_unique<CacheApiContext>(CacheApiContext{
        .cache = cache_.get(),
        .url_registry = *url_registry_,
        .invalidate_url =
            [this](const std::string& url, const std::string& hostname,
                   const std::string& scheme, const std::string& site) {
              auto result = cache_->Remove(url, hostname, scheme, site);
              return result.has_value() ? 1 : 0;
            },
        .clear_dedup = nullptr,
        .enqueue_reprocess = nullptr,
        .reset_cache = [this]() -> std::string {
          auto result = cache_->ResetVolume();
          if (!result) {
            return make_error_code(result.error()).message();
          }
          return {};
        },
    });

    // Set up the HTTP server.
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();
    HttpServerConfig server_config;
    server_config.port = 0;
    // An empty token now fails closed; these fixtures
    // exercise the routes, not the credential gate.
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

    cache_.reset();
    std::filesystem::remove_all(tmp_dir_);
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

  // Write a test alternate into the cache.
  void WriteTestAlternate(const std::string& url, const std::string& hostname,
                          AlternateId id, const AlternateMetadata& meta,
                          const std::string& content,
                          const std::string& scheme = "https") {
    auto result =
        cache_->WriteAlternate(url, hostname, scheme, id, content.size(), meta);
    ASSERT_TRUE(result.has_value());
    auto written = result->write_sync(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(content.data()), content.size()));
    ASSERT_TRUE(written.has_value());
    (void)result->close_sync();
  }

  // Mirror what the worker does on a notification: compute the unique alternate
  // count straight from the cache (the same ListAlternates + CountUniqueAlternates
  // path the worker's HandleNotification guard uses) and store it in the
  // registry.  The listing handler no longer reads the cache per row — it reads
  // this cached value — so tests must seed the count the way production does.
  void RefreshAlternateCount(const std::string& url,
                             const std::string& hostname,
                             const std::string& scheme = "https") {
    auto alts = cache_->ListAlternates(url, hostname, scheme);
    url_registry_->SetAlternateCount(
        url, hostname, scheme,
        alts ? static_cast<uint32_t>(CountUniqueAlternates(*alts)) : 0);
  }

  std::string SendRequest(const std::string& request) {
    std::string wire = test::InjectConnectionClose(
        test::InjectCsrfHeaderForV1Mutations(request));
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, /*timeout_sec=*/10);

    if (sock < 0) return "";

    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    if (sent != static_cast<ssize_t>(wire.size())) {
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

  json ParseJsonBody(const std::string& response) {
    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) return {};
    return json::parse(response.substr(body_start + 4));
  }

  std::filesystem::path tmp_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
  std::unique_ptr<UrlRegistry> url_registry_;
  std::unique_ptr<CacheApiContext> ctx_;
  uv_loop_t* loop_ = nullptr;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
};

// =============================================================================
// URL Registry Tests (unit tests, no HTTP)
// =============================================================================

TEST(UrlRegistryTest, RecordAndList) {
  UrlRegistry reg(100);
  reg.Record("http://example.com/a", "example.com", "https");
  reg.Record("http://example.com/b", "example.com", "https");
  EXPECT_EQ(reg.size(), 2u);

  auto page = reg.List(0, 10);
  EXPECT_EQ(page.total, 2u);
  EXPECT_EQ(page.entries.size(), 2u);
  // Newest first.
  EXPECT_EQ(page.entries[0].url, "http://example.com/b");
  EXPECT_EQ(page.entries[1].url, "http://example.com/a");
}

TEST(UrlRegistryTest, DuplicateMovesToFront) {
  UrlRegistry reg(100);
  reg.Record("http://example.com/a", "example.com", "https");
  reg.Record("http://example.com/b", "example.com", "https");
  reg.Record("http://example.com/a", "example.com", "https");  // Move to front

  EXPECT_EQ(reg.size(), 2u);
  auto page = reg.List(0, 10);
  EXPECT_EQ(page.entries[0].url, "http://example.com/a");
}

TEST(UrlRegistryTest, EvictsOldest) {
  UrlRegistry reg(2);
  reg.Record("http://example.com/a", "example.com", "https");
  reg.Record("http://example.com/b", "example.com", "https");
  reg.Record("http://example.com/c", "example.com", "https");  // Evicts /a

  EXPECT_EQ(reg.size(), 2u);
  auto page = reg.List(0, 10);
  EXPECT_EQ(page.entries[0].url, "http://example.com/c");
  EXPECT_EQ(page.entries[1].url, "http://example.com/b");
}

TEST(UrlRegistryTest, RemoveEntry) {
  UrlRegistry reg(100);
  reg.Record("http://example.com/a", "example.com", "https");
  reg.Record("http://example.com/b", "example.com", "https");
  reg.Remove("http://example.com/a", "example.com", "https");

  EXPECT_EQ(reg.size(), 1u);
  auto page = reg.List(0, 10);
  EXPECT_EQ(page.entries[0].url, "http://example.com/b");
}

TEST(UrlRegistryTest, Pagination) {
  UrlRegistry reg(100);
  for (int i = 0; i < 5; ++i) {
    reg.Record("http://example.com/" + std::to_string(i), "example.com",
               "https");
  }

  auto page1 = reg.List(0, 2);
  EXPECT_EQ(page1.entries.size(), 2u);
  EXPECT_TRUE(page1.has_more);
  EXPECT_EQ(page1.total, 5u);

  auto page2 = reg.List(page1.next_offset, 2);
  EXPECT_EQ(page2.entries.size(), 2u);
  EXPECT_TRUE(page2.has_more);

  auto page3 = reg.List(page2.next_offset, 2);
  EXPECT_EQ(page3.entries.size(), 1u);
  EXPECT_FALSE(page3.has_more);
}

TEST(UrlRegistryTest, HostnameFilter) {
  UrlRegistry reg(100);
  reg.Record("http://a.com/1", "a.com", "https");
  reg.Record("http://b.com/1", "b.com", "https");
  reg.Record("http://a.com/2", "a.com", "https");
  reg.Record("http://b.com/2", "b.com", "https");

  // Filter to a.com — should see 2 entries, newest first.
  auto page = reg.List(0, 10, "a.com");
  EXPECT_EQ(page.total, 2u);
  EXPECT_EQ(page.entries.size(), 2u);
  EXPECT_EQ(page.entries[0].url, "http://a.com/2");
  EXPECT_EQ(page.entries[1].url, "http://a.com/1");
  EXPECT_FALSE(page.has_more);

  // Filter to b.com.
  auto page_b = reg.List(0, 10, "b.com");
  EXPECT_EQ(page_b.total, 2u);
  EXPECT_EQ(page_b.entries[0].url, "http://b.com/2");

  // Filter to unknown host — empty.
  auto page_none = reg.List(0, 10, "unknown.com");
  EXPECT_EQ(page_none.total, 0u);
  EXPECT_EQ(page_none.entries.size(), 0u);
  EXPECT_FALSE(page_none.has_more);
}

TEST(UrlRegistryTest, HasHostname) {
  UrlRegistry reg(100);
  EXPECT_FALSE(reg.HasHostname("unknown.com"));

  reg.Record("http://a.com/1", "a.com", "https");
  EXPECT_TRUE(reg.HasHostname("a.com"));
  EXPECT_FALSE(reg.HasHostname("b.com"));

  // Known hostnames persist even after all entries for that host are removed.
  reg.Remove("http://a.com/1", "a.com", "https");
  EXPECT_TRUE(reg.HasHostname("a.com"));
}

TEST(UrlRegistryTest, HasHostnameSurvivesEviction) {
  UrlRegistry reg(2);  // Max 2 entries.
  reg.Record("http://a.com/1", "a.com", "https");
  reg.Record("http://b.com/1", "b.com", "https");
  // This evicts a.com's entry from the LRU registry.
  reg.Record("http://c.com/1", "c.com", "https");

  // a.com is still a known hostname even though evicted from the registry.
  EXPECT_TRUE(reg.HasHostname("a.com"));
  EXPECT_TRUE(reg.HasHostname("b.com"));
  EXPECT_TRUE(reg.HasHostname("c.com"));
}

TEST(UrlRegistryTest, HostnameFilterPagination) {
  UrlRegistry reg(100);
  // Interleave two hostnames.
  for (int i = 0; i < 6; ++i) {
    std::string host = (i % 2 == 0) ? "even.com" : "odd.com";
    reg.Record("http://" + host + "/" + std::to_string(i), host, "https");
  }

  // even.com has entries 0, 2, 4 (3 total). Paginate with limit 2.
  auto p1 = reg.List(0, 2, "even.com");
  EXPECT_EQ(p1.total, 3u);
  EXPECT_EQ(p1.entries.size(), 2u);
  EXPECT_TRUE(p1.has_more);

  auto p2 = reg.List(p1.next_offset, 2, "even.com");
  EXPECT_EQ(p2.entries.size(), 1u);
  EXPECT_FALSE(p2.has_more);
}

// =============================================================================
// Cache Handler Integration Tests
// =============================================================================

TEST_F(CacheHandlersTest, ListAlternatesMissingUrl) {
  std::string resp = SendRequest(
      "GET /v1/cache/alternates HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(CacheHandlersTest, ListAlternatesEmpty) {
  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/notfound&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos);
}

TEST_F(CacheHandlersTest, ListAlternatesWithData) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();  // Desktop/Identity
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html; charset=utf-8";
  WriteTestAlternate("http://example.com/page", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>hello</html>");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/page&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  // Metadata is read from the Cyclone document header (set_header path).
  EXPECT_EQ(j["alternates"][0]["content_type"], "html");
  EXPECT_EQ(j["alternates"][0]["origin_content_type"],
            "text/html; charset=utf-8");
  EXPECT_FALSE(j["alternates"][0]["is_sentinel"].get<bool>());
  // Single entry: chain_length equals count (no stale duplicates).
  EXPECT_EQ(j["chain_length"], 1);
  EXPECT_EQ(j["cache_key"], "ks4:https://example.comhttp://example.com/page");
}

TEST_F(CacheHandlersTest, ListUrlsEndpoint) {
  url_registry_->Record("http://example.com/a", "example.com", "https");
  url_registry_->Record("http://example.com/b", "example.com", "https");

  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=10 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["total"], 2);
  EXPECT_EQ(j["urls"].size(), 2u);
  // alternate_count should be 0 when nothing is written to cache.
  EXPECT_EQ(j["urls"][0]["alternate_count"], 0);
  // The reported key is the string the cache composes, namespace included.
  EXPECT_EQ(j["urls"][0]["cache_key"],
            "ks4:https://example.comhttp://example.com/b");
}

TEST_F(CacheHandlersTest, ListUrlsAlternateCount) {
  // Write two alternates for one URL, none for the other.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  WriteTestAlternate("http://example.com/page", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>hi</html>");
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  AlternateMetadata meta2;
  meta2.full_mask = webp_mask.Encode();
  meta2.content_type = ContentType::kImage;
  meta2.origin_content_type = "image/webp";
  WriteTestAlternate("http://example.com/page", "example.com",
                     MaskToAlternateId(webp_mask.Encode() & 0xFF), meta2,
                     "fake-webp");

  url_registry_->Record("http://example.com/page", "example.com", "https");
  url_registry_->Record("http://example.com/empty", "example.com", "https");

  // Seed the cached counts exactly as the worker would on a notification.
  RefreshAlternateCount("http://example.com/page", "example.com");
  RefreshAlternateCount("http://example.com/empty", "example.com");

  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=10 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["urls"].size(), 2u);

  // Find the URL entries (order is newest-first: /empty then /page).
  auto& empty_entry = j["urls"][0];
  auto& page_entry = j["urls"][1];
  EXPECT_EQ(empty_entry["url"], "http://example.com/empty");
  EXPECT_EQ(empty_entry["alternate_count"], 0);
  EXPECT_EQ(page_entry["url"], "http://example.com/page");
  EXPECT_EQ(page_entry["alternate_count"], 2);
}

TEST_F(CacheHandlersTest, ListUrlsHostnameFilter) {
  url_registry_->Record("http://a.com/x", "a.com", "https");
  url_registry_->Record("http://b.com/y", "b.com", "https");
  url_registry_->Record("http://a.com/z", "a.com", "https");

  std::string resp = SendRequest(
      "GET /v1/cache/urls?hostname=a.com&limit=10 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["total"], 2);
  EXPECT_EQ(j["urls"].size(), 2u);
  EXPECT_EQ(j["urls"][0]["hostname"], "a.com");
  EXPECT_EQ(j["urls"][1]["hostname"], "a.com");
}

TEST_F(CacheHandlersTest, SelectEndpoint) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  WriteTestAlternate("http://example.com/img.jpg", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-jpeg-content");

  uint32_t mask = CapabilityMask().Encode();
  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/img.jpg&hostname="
      "example.com&mask=" +
      std::to_string(mask) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_GT(j["best_score"].get<int>(), 0);
  EXPECT_GE(j["best_index"].get<int>(), 0);
}

TEST_F(CacheHandlersTest, ContentEndpoint) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  std::string content = "<html>test</html>";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/test", "example.com", id, meta,
                     content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/test&hostname="
      "example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("text/html"), std::string::npos);
  EXPECT_NE(resp.find(content), std::string::npos);
}

TEST_F(CacheHandlersTest, PurgeEndpoint) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  WriteTestAlternate("http://example.com/purge-me", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>bye</html>");
  url_registry_->Record("http://example.com/purge-me", "example.com", "https");

  std::string body =
      R"({"url":"http://example.com/purge-me","hostname":"example.com"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_GE(j["deleted"].get<int>(), 0);

  // Verify URL removed from registry.
  EXPECT_EQ(url_registry_->size(), 0u);
}

TEST_F(CacheHandlersTest, PurgeAll) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  WriteTestAlternate("http://example.com/page1", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>1</html>");
  WriteTestAlternate("http://example.com/page2", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>2</html>");
  url_registry_->Record("http://example.com/page1", "example.com", "https");
  url_registry_->Record("http://example.com/page2", "example.com", "https");

  std::string body = R"({"scope":"all","confirm":"purge-all"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["scope"], "all");
  EXPECT_EQ(j["urls_cleared"].get<int>(), 2);
  EXPECT_TRUE(j["volume_reset"].get<bool>());

  // Registry should be empty.
  EXPECT_EQ(url_registry_->size(), 0u);

  // Cache should be empty — listing alternates should fail.
  auto alts = cache_->ListAlternates("http://example.com/page1", "example.com",
                                     "https");
  EXPECT_FALSE(alts.has_value());

  // Cache should still be functional — writes should succeed.
  AlternateMetadata meta2;
  meta2.full_mask = CapabilityMask().Encode();
  meta2.content_type = ContentType::kHtml;
  WriteTestAlternate("http://example.com/page3", "example.com",
                     MaskToAlternateId(meta2.full_mask & 0xFF), meta2,
                     "<html>3</html>");
  auto alts2 = cache_->ListAlternates("http://example.com/page3", "example.com",
                                      "https");
  EXPECT_TRUE(alts2.has_value());
}

TEST_F(CacheHandlersTest, PurgeAllRequiresConfirm) {
  std::string body = R"({"scope":"all"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("purge-all"), std::string::npos);
}

TEST_F(CacheHandlersTest, PurgeInvalidJson) {
  std::string body = "not json";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(CacheHandlersTest, PurgeRejectsUnknownHostname) {
  // Register a known hostname so the registry has something.
  url_registry_->Record("http://known.com/page", "known.com", "https");

  // Attempt to purge with a hostname that was never recorded.
  std::string body =
      R"({"url":"http://evil.com/victim","hostname":"evil.com"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("403"), std::string::npos);
  EXPECT_NE(resp.find("never served"), std::string::npos);
}

TEST_F(CacheHandlersTest, PurgeAllowsKnownHostname) {
  // Register a hostname and write content to purge.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  WriteTestAlternate("http://known.com/page", "known.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>ok</html>");
  url_registry_->Record("http://known.com/page", "known.com", "https");

  std::string body =
      R"({"url":"http://known.com/page","hostname":"known.com"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
}

TEST_F(CacheHandlersTest, PurgeAllowsAliasedHostname) {
  // Simulate the nginx flow: hostname is recorded under its canonical
  // (alias-resolved) name.  A purge request using the alias must
  // succeed when url_norm_config is set.
  UrlNormalizationConfig alias_config;
  alias_config.host_aliases["www.example.com"] = "example.com";
  ctx_->url_norm_config = &alias_config;

  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  // nginx records using the canonical hostname (after NormalizeCacheHostname).
  WriteTestAlternate("/page", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>aliased</html>");
  url_registry_->Record("/page", "example.com", "https");

  // Purge request uses the alias hostname — should resolve and succeed.
  std::string body = R"({"url":"/page","hostname":"www.example.com"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos)
      << "Purge with aliased hostname should succeed, got: " << resp;

  // The response should show the canonical hostname, not the alias.
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["hostname"], "example.com");

  // Reset config to avoid dangling pointer in TearDown.
  ctx_->url_norm_config = nullptr;
}

TEST_F(CacheHandlersTest, ReprocessAllowsAliasedHostname) {
  // Same scenario as PurgeAllowsAliasedHostname but for reprocess.
  UrlNormalizationConfig alias_config;
  alias_config.host_aliases["www.example.com"] = "example.com";
  ctx_->url_norm_config = &alias_config;

  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  WriteTestAlternate("/page", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>aliased</html>");
  url_registry_->Record("/page", "example.com", "https");

  std::string body = R"({"url":"/page","hostname":"www.example.com"})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos)
      << "Reprocess with aliased hostname should succeed, got: " << resp;

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["hostname"], "example.com");

  ctx_->url_norm_config = nullptr;
}

TEST_F(CacheHandlersTest, ReprocessRejectsUnknownHostname) {
  url_registry_->Record("http://known.com/page", "known.com", "https");

  std::string body =
      R"({"url":"http://evil.com/victim","hostname":"evil.com"})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("403"), std::string::npos);
  EXPECT_NE(resp.find("never served"), std::string::npos);
}

TEST_F(CacheHandlersTest, ReprocessEndpoint) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kImage;
  WriteTestAlternate("http://example.com/reprocess", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "image-data");
  url_registry_->Record("http://example.com/reprocess", "example.com", "https");

  std::string body =
      R"({"url":"http://example.com/reprocess","hostname":"example.com"})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_TRUE(j.contains("reprocess_enqueued"));
}

TEST_F(CacheHandlersTest, ContentMissingAlternateId) {
  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/test HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(CacheHandlersTest, SelectMissingMask) {
  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/test&hostname=example.com "
      "HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
}

// =============================================================================
// SVG Content-Type and Security Header Tests
// =============================================================================

TEST_F(CacheHandlersTest, SvgContentTypeAndSecurityHeaders) {
  // Create an SVG alternate: format bits = 3 (kSvg), Desktop viewport.
  CapabilityMask svg_mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = svg_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";  // Original was PNG
  std::string svg_content =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect/></svg>)";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/logo.svg", "example.com", id, meta,
                     svg_content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/logo.svg&hostname="
      "example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");

  // Verify 200 OK.
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  // Verify Content-Type is image/svg+xml (not the origin image/png).
  EXPECT_NE(resp.find("image/svg+xml"), std::string::npos);

  // Verify CSP header is present.
  EXPECT_NE(resp.find("Content-Security-Policy: "
                      "default-src 'none'; style-src 'unsafe-inline'"),
            std::string::npos);

  // Verify X-Content-Type-Options: nosniff is present.
  EXPECT_NE(resp.find("X-Content-Type-Options: nosniff"), std::string::npos);

  // Verify SVG content is served.
  EXPECT_NE(resp.find(svg_content), std::string::npos);
}

TEST_F(CacheHandlersTest, NonSvgImageNoCSP) {
  // Create a WebP alternate: format bits = 1 (kWebP).
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = webp_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  std::string content = "fake-webp-data";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/photo.webp", "example.com", id, meta,
                     content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/photo.webp&hostname="
      "example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");

  // Verify 200 OK.
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  // Verify Content-Type is image/webp (not the origin image/jpeg).
  EXPECT_NE(resp.find("image/webp"), std::string::npos);

  // Verify CSP header is NOT present (only for SVG).
  EXPECT_EQ(resp.find("Content-Security-Policy"), std::string::npos);

  // Verify nosniff is still present (always set on content endpoint).
  EXPECT_NE(resp.find("X-Content-Type-Options: nosniff"), std::string::npos);
}

TEST_F(CacheHandlersTest, HtmlContentNoCSP) {
  // Verify that non-image (HTML) responses don't get CSP header.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html; charset=utf-8";
  std::string content = "<html>hello</html>";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/page.html", "example.com", id, meta,
                     content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/page.html&hostname="
      "example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");

  // Verify 200 OK.
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  // Verify Content-Type is text/html.
  EXPECT_NE(resp.find("text/html"), std::string::npos);

  // Verify CSP header is NOT present.
  EXPECT_EQ(resp.find("Content-Security-Policy"), std::string::npos);
}

TEST_F(CacheHandlersTest, SvgFormatInMaskToJson) {
  // Verify that MaskToJson correctly reports "svg" for kSvg format.
  CapabilityMask svg_mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = svg_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  std::string content = "<svg/>";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/icon.svg", "example.com", id, meta,
                     content);

  // Use the select endpoint to verify MaskToJson reports "svg".
  uint32_t mask = svg_mask.Encode();
  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/icon.svg&hostname="
      "example.com&mask=" +
      std::to_string(mask) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["client_mask"]["format"], "svg");
}

TEST_F(CacheHandlersTest, ListUrlsAlternateCountDeduplicates) {
  // Write same alternate_id twice (simulating append-only chain dup)
  // plus a different alternate_id once.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  AlternateId id1 = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/dup", "example.com", id1, meta,
                     "version1");
  WriteTestAlternate("http://example.com/dup", "example.com", id1, meta,
                     "version2");

  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  AlternateMetadata meta2;
  meta2.full_mask = webp_mask.Encode();
  meta2.content_type = ContentType::kImage;
  meta2.origin_content_type = "image/webp";
  AlternateId id2 = MaskToAlternateId(webp_mask.Encode() & 0xFF);
  WriteTestAlternate("http://example.com/dup", "example.com", id2, meta2,
                     "webp-data");

  url_registry_->Record("http://example.com/dup", "example.com", "https");
  RefreshAlternateCount("http://example.com/dup", "example.com");

  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=10 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  ASSERT_EQ(j["urls"].size(), 1u);
  // Unique-by-id dedup: two writes of id1 + one of id2 => 2.
  EXPECT_EQ(j["urls"][0]["alternate_count"], 2);
}

TEST_F(CacheHandlersTest, ListUrlsAlternateCountAllSameId) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/same", "example.com", id, meta, "v1");
  WriteTestAlternate("http://example.com/same", "example.com", id, meta, "v2");
  WriteTestAlternate("http://example.com/same", "example.com", id, meta, "v3");

  url_registry_->Record("http://example.com/same", "example.com", "https");
  RefreshAlternateCount("http://example.com/same", "example.com");

  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=10 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  ASSERT_EQ(j["urls"].size(), 1u);
  // All three writes share one id => 1.
  EXPECT_EQ(j["urls"][0]["alternate_count"], 1);
}

// =============================================================================
// Additional Coverage Tests
// =============================================================================

TEST_F(CacheHandlersTest, ListAlternatesWithAvifVariant) {
  // Create an AVIF alternate: format bits = 2 (kAvif).
  CapabilityMask avif_mask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = avif_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  WriteTestAlternate("http://example.com/img.avif", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-avif-data");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/img.avif&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["format"], "avif");
  EXPECT_EQ(j["alternates"][0]["content_type"], "image");
}

TEST_F(CacheHandlersTest, ListAlternatesWithCssContentType) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kCss;
  meta.origin_content_type = "text/css";
  WriteTestAlternate("http://example.com/style.css", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "body { margin: 0; }");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/style.css&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["content_type"], "css");
  EXPECT_EQ(j["alternates"][0]["origin_content_type"], "text/css");
}

TEST_F(CacheHandlersTest, ListAlternatesWithJsContentType) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kJs;
  meta.origin_content_type = "application/javascript";
  WriteTestAlternate("http://example.com/app.js", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "console.log('hi');");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/app.js&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["content_type"], "js");
  EXPECT_EQ(j["alternates"][0]["origin_content_type"],
            "application/javascript");
}

TEST_F(CacheHandlersTest, ListUrlsInvalidOffset) {
  url_registry_->Record("http://example.com/a", "example.com", "https");

  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=abc&limit=10 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("offset"), std::string::npos);
}

TEST_F(CacheHandlersTest, ListUrlsInvalidLimit) {
  url_registry_->Record("http://example.com/a", "example.com", "https");

  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=xyz HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("limit"), std::string::npos);
}

TEST_F(CacheHandlersTest, SelectMissingUrl) {
  std::string resp = SendRequest(
      "GET /v1/cache/select?mask=8 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

TEST_F(CacheHandlersTest, SelectNonexistentUrl) {
  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/nope&hostname="
      "example.com&mask=8 HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos);
}

TEST_F(CacheHandlersTest, SelectInvalidMask) {
  // Write something so URL exists.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  WriteTestAlternate("http://example.com/mask-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>hi</html>");

  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/mask-test&hostname="
      "example.com&mask=notanumber HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("mask"), std::string::npos);
}

TEST_F(CacheHandlersTest, ContentMissingUrl) {
  std::string resp = SendRequest(
      "GET /v1/cache/content?alternate_id=8 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

TEST_F(CacheHandlersTest, ContentAlternateIdOutOfRange) {
  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/test&hostname="
      "example.com&alternate_id=999 HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("0-255"), std::string::npos);
}

TEST_F(CacheHandlersTest, ContentInvalidAlternateId) {
  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/test&hostname="
      "example.com&alternate_id=abc HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(CacheHandlersTest, ContentNonexistentAlternate) {
  // URL has no alternates written.
  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/notcached&hostname="
      "example.com&alternate_id=8 HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos);
}

TEST_F(CacheHandlersTest, PurgeDeeplyNestedJson) {
  // Build a deeply nested JSON body (exceeds limit of 32).
  std::string body;
  for (int i = 0; i < 40; ++i) body += "{\"a\":";
  body += "1";
  for (int i = 0; i < 40; ++i) body += "}";

  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("nesting"), std::string::npos);
}

TEST_F(CacheHandlersTest, PurgeMissingUrl) {
  std::string body = R"({"hostname":"example.com"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

TEST_F(CacheHandlersTest, ReprocessInvalidJson) {
  std::string body = "not json {{";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(CacheHandlersTest, ReprocessMissingUrl) {
  std::string body = R"({"hostname":"example.com"})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

TEST_F(CacheHandlersTest, ReprocessDeeplyNestedJson) {
  std::string body;
  for (int i = 0; i < 40; ++i) body += "{\"a\":";
  body += "1";
  for (int i = 0; i < 40; ++i) body += "}";

  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("nesting"), std::string::npos);
}

TEST_F(CacheHandlersTest, ContentAvifFormat) {
  CapabilityMask avif_mask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = avif_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  std::string content = "fake-avif-content";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/photo.avif", "example.com", id, meta,
                     content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/photo.avif&hostname="
      "example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");

  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("image/avif"), std::string::npos);
  // CSP should NOT be present for AVIF.
  EXPECT_EQ(resp.find("Content-Security-Policy"), std::string::npos);
}

TEST_F(CacheHandlersTest, ContentOriginalFormatUsesOriginContentType) {
  // Original format (format bits = 0) should use origin_content_type.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();  // Original, Desktop, 1x, Off
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  std::string content = "fake-png-content";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/img.png", "example.com", id, meta,
                     content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/img.png&hostname="
      "example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");

  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("image/png"), std::string::npos);
}

TEST_F(CacheHandlersTest, ContentNonImageUsesOriginContentType) {
  // Non-image content should use origin_content_type.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kCss;
  meta.origin_content_type = "text/css; charset=utf-8";
  std::string content = "body{margin:0}";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/style.css", "example.com", id, meta,
                     content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/style.css&hostname="
      "example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");

  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("text/css"), std::string::npos);
}

TEST_F(CacheHandlersTest, ListUrlsLimitClampedTo1000) {
  url_registry_->Record("http://example.com/a", "example.com", "https");

  // Limit > 1000 should be clamped to 1000.
  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=5000 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["limit"], 1000);
}

TEST_F(CacheHandlersTest, ListUrlsZeroLimitDefaultsTo100) {
  url_registry_->Record("http://example.com/a", "example.com", "https");

  // Limit = 0 should default to 100.
  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=0 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["limit"], 100);
}

// =============================================================================
// Additional Coverage: Format/Viewport/Encoding/Sentinel/ContentClass
// =============================================================================

TEST_F(CacheHandlersTest, ListAlternatesWebpFormat) {
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = webp_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  WriteTestAlternate("http://example.com/webp-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-webp");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/webp-test&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["format"], "webp");
}

TEST_F(CacheHandlersTest, ListAlternatesSvgFormat) {
  CapabilityMask svg_mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = svg_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  WriteTestAlternate("http://example.com/svg-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta, "<svg/>");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/svg-test&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["format"], "svg");
}

TEST_F(CacheHandlersTest, ListAlternatesWorkerProcessedOriginalShowsJpeg) {
  // Worker-processed variant with kOriginal format bits and origin_content_type
  // "image/jpeg" should report format="jpeg", not "original".
  CapabilityMask mask(CapabilityMask::ImageFormat::kOriginal,
                      CapabilityMask::Viewport::kDesktop,
                      CapabilityMask::PixelDensity::k1x,
                      CapabilityMask::SaveData::kOff,
                      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  WriteTestAlternate("http://example.com/wp-jpeg", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-jpeg-data");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/wp-jpeg&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["format"], "jpeg");
}

TEST_F(CacheHandlersTest, ListAlternatesWorkerProcessedOriginalShowsPng) {
  CapabilityMask mask(
      CapabilityMask::ImageFormat::kOriginal, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  WriteTestAlternate("http://example.com/wp-png", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-png-data");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/wp-png&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["format"], "png");
}

TEST_F(CacheHandlersTest, ListAlternatesNginxOriginalKeepsOriginalFormat) {
  // Nginx-written original (no kFlagWorkerProcessed) should keep
  // format="original" even with a JPEG origin_content_type.
  CapabilityMask mask(CapabilityMask::ImageFormat::kOriginal,
                      CapabilityMask::Viewport::kDesktop,
                      CapabilityMask::PixelDensity::k1x,
                      CapabilityMask::SaveData::kOff,
                      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.flags = 0;  // Not worker-processed (nginx-written).
  WriteTestAlternate("http://example.com/nginx-orig", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "raw-jpeg-data");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/nginx-orig&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["format"], "original");
}

TEST_F(CacheHandlersTest, ListAlternatesTabletViewport) {
  CapabilityMask tablet_mask(
      CapabilityMask::ImageFormat::kOriginal, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = tablet_mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  WriteTestAlternate("http://example.com/tablet-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>tablet</html>");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/tablet-test&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["viewport"], "tablet");
}

TEST_F(CacheHandlersTest, ListAlternatesMobileViewport) {
  CapabilityMask mobile_mask(
      CapabilityMask::ImageFormat::kOriginal, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = mobile_mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  WriteTestAlternate("http://example.com/mobile-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>mobile</html>");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/mobile-test&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["viewport"], "mobile");
}

TEST_F(CacheHandlersTest, ListAlternatesHighDensity) {
  CapabilityMask hd_mask(CapabilityMask::ImageFormat::kOriginal,
                         CapabilityMask::Viewport::kDesktop,
                         CapabilityMask::PixelDensity::k2xPlus,
                         CapabilityMask::SaveData::kOff,
                         CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = hd_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  WriteTestAlternate("http://example.com/hd-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-hd-image");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/hd-test&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["density"], "2x+");
}

TEST_F(CacheHandlersTest, ListAlternatesGzipEncoding) {
  CapabilityMask gz_mask(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff, CapabilityMask::TransferEncoding::kGzip);

  AlternateMetadata meta;
  meta.full_mask = gz_mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  WriteTestAlternate("http://example.com/gzip-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "gzipped-content");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/gzip-test&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["encoding"], "gzip");
}

TEST_F(CacheHandlersTest, ListAlternatesBrotliEncoding) {
  CapabilityMask br_mask(CapabilityMask::ImageFormat::kOriginal,
                         CapabilityMask::Viewport::kDesktop,
                         CapabilityMask::PixelDensity::k1x,
                         CapabilityMask::SaveData::kOff,
                         CapabilityMask::TransferEncoding::kBrotli);

  AlternateMetadata meta;
  meta.full_mask = br_mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  WriteTestAlternate("http://example.com/brotli-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "brotli-content");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/brotli-test&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["encoding"], "brotli");
}

TEST_F(CacheHandlersTest, ListAlternatesSentinel) {
  // Write an early hints sentinel using WriteSentinel.
  std::string hint_data = "/images/hero.jpg\nimage:/images/lcp.webp";
  auto result =
      cache_->WriteSentinel("http://example.com/sentinel-test", "example.com",
                            "https", SentinelId::kEarlyHints, hint_data.size());
  ASSERT_TRUE(result.has_value()) << "WriteSentinel should succeed";
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(hint_data.data()), hint_data.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/sentinel-test&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_TRUE(j["alternates"][0]["is_sentinel"].get<bool>());
  EXPECT_EQ(j["alternates"][0]["sentinel_name"], "early_hints");
}

TEST_F(CacheHandlersTest, ContentSvgType) {
  // Verify the content endpoint returns image/svg+xml for SVG alternates.
  CapabilityMask svg_mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = svg_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  std::string svg_content = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/svg-content-test", "example.com", id,
                     meta, svg_content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/svg-content-test&hostname="
      "example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");

  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("image/svg+xml"), std::string::npos)
      << "SVG alternate should have image/svg+xml content type";
}

// =============================================================================
// Additional Coverage: Sentinel names, content class, SSIMULACRA2, dedup,
// select with sentinels, reprocess callbacks, edge cases
// =============================================================================

TEST_F(CacheHandlersTest, ListAlternatesWithSsimulacra2Score) {
  // Write an image alternate with a SSIMULACRA2 score set.
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = webp_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.ssimulacra2_score_x100 = 7500;  // 75.00
  WriteTestAlternate("http://example.com/scored-img", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-webp-scored");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/scored-img&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_TRUE(j["alternates"][0].contains("ssimulacra2_score"));
  EXPECT_NEAR(j["alternates"][0]["ssimulacra2_score"].get<double>(), 75.0,
              0.01);
}

TEST_F(CacheHandlersTest, ListAlternatesWithContentClass) {
  // Write an image alternate with content_class = Photo (0).
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = webp_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.content_class = 0;  // Photo
  WriteTestAlternate("http://example.com/classed-img", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-webp-classed");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/classed-img&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_TRUE(j["alternates"][0].contains("content_class"));
  EXPECT_EQ(j["alternates"][0]["content_class"], "photo");
}

TEST_F(CacheHandlersTest, ListAlternatesContentClassScreenshot) {
  CapabilityMask mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  meta.content_class = 1;  // Screenshot
  WriteTestAlternate("http://example.com/screenshot-img", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-screenshot");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "screenshot-img&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["alternates"][0]["content_class"], "screenshot");
}

TEST_F(CacheHandlersTest, ListAlternatesContentClassIllustration) {
  CapabilityMask mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  meta.content_class = 2;  // Illustration
  WriteTestAlternate("http://example.com/illustration-img", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-illustration");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "illustration-img&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["alternates"][0]["content_class"], "illustration");
}

TEST_F(CacheHandlersTest, ListAlternatesContentClassNoisy) {
  CapabilityMask mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.content_class = 3;  // Noisy
  WriteTestAlternate("http://example.com/noisy-img", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-noisy");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "noisy-img&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["alternates"][0]["content_class"], "noisy");
}

TEST_F(CacheHandlersTest, ListAlternatesNeedsRevalidationFlag) {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  meta.flags = AlternateMetadata::kFlagNeedsRevalidation;
  WriteTestAlternate("http://example.com/reval-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>reval</html>");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "reval-test&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_TRUE(j["alternates"][0]["needs_revalidation"].get<bool>());
  EXPECT_EQ(j["alternates"][0]["flags"].get<int>(),
            AlternateMetadata::kFlagNeedsRevalidation);
}

TEST_F(CacheHandlersTest, ListAlternatesOtherContentType) {
  // ContentType::kOther should return "other" from ContentTypeString.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "application/octet-stream";
  WriteTestAlternate("http://example.com/other-type", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "binary-data");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "other-type&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["alternates"][0]["content_type"], "other");
}

TEST_F(CacheHandlersTest, ListAlternatesSaveDataOn) {
  CapabilityMask sd_mask(CapabilityMask::ImageFormat::kOriginal,
                         CapabilityMask::Viewport::kDesktop,
                         CapabilityMask::PixelDensity::k1x,
                         CapabilityMask::SaveData::kOn,
                         CapabilityMask::TransferEncoding::kIdentity);

  AlternateMetadata meta;
  meta.full_mask = sd_mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  WriteTestAlternate("http://example.com/savedata-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>savedata</html>");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "savedata-test&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_TRUE(j["alternates"][0]["save_data"].get<bool>());
}

TEST_F(CacheHandlersTest, ListAlternatesReportsOneEntryPerAlternateId) {
  // Write the same alternate_id twice. The storage layer unlinks the
  // superseded node as part of the second write, so the raw chain holds one
  // node rather than a stale one plus a current one. The handler's own
  // deduplication is kept as a defensive second line — a splice the storage
  // layer has to defer can still leave a superseded node behind — so what is
  // pinned here is the reported result, which is one entry either way.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  std::string old_content(5000, 'a');
  std::string new_content(3000, 'b');
  WriteTestAlternate("http://example.com/dedup-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     old_content);
  // Write again (same ID) - simulates overwrite in append-only chain.
  WriteTestAlternate("http://example.com/dedup-test", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     new_content);

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "dedup-test&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  // Exactly 1 entry (the newest).
  EXPECT_EQ(j["count"], 1);
  // The raw chain now holds only the current node.
  EXPECT_EQ(j["chain_length"], 1);
  // Reported size must be the newest (smaller) content, not the stale one.
  auto reported_size = j["alternates"][0]["size"].get<uint64_t>();
  EXPECT_EQ(reported_size, new_content.size());
}

TEST_F(CacheHandlersTest, ListAlternatesMultipleSentinelTypes) {
  // Write multiple sentinel types and verify their names.
  std::string data = "test-data";

  auto write_sentinel = [&](SentinelId sid) {
    auto result =
        cache_->WriteSentinel("http://example.com/multi-sentinel",
                              "example.com", "https", sid, data.size());
    ASSERT_TRUE(result.has_value());
    auto written = result->write_sync(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data.data()), data.size()));
    ASSERT_TRUE(written.has_value());
    (void)result->close_sync();
  };

  write_sentinel(SentinelId::kEarlyHints);
  write_sentinel(SentinelId::kWarmupRequest);
  write_sentinel(SentinelId::kContentHash);

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "multi-sentinel&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_GE(j["count"].get<int>(), 3);

  // Collect sentinel names.
  std::vector<std::string> sentinel_names;
  for (const auto& alt : j["alternates"]) {
    if (alt["is_sentinel"].get<bool>()) {
      sentinel_names.push_back(alt["sentinel_name"].get<std::string>());
    }
  }
  // All three should be present.
  EXPECT_NE(
      std::find(sentinel_names.begin(), sentinel_names.end(), "early_hints"),
      sentinel_names.end());
  EXPECT_NE(std::find(sentinel_names.begin(), sentinel_names.end(), "warmup"),
            sentinel_names.end());
  EXPECT_NE(
      std::find(sentinel_names.begin(), sentinel_names.end(), "content_hash"),
      sentinel_names.end());
}

TEST_F(CacheHandlersTest, SelectEndpointWithSentinelSkipped) {
  // Write a regular alternate and a sentinel.  The select endpoint should
  // give score 0 to the sentinel and still pick the regular one.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  WriteTestAlternate("http://example.com/select-sentinel", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "fake-jpeg");

  // Write a sentinel.
  std::string hint = "preload-data";
  auto result =
      cache_->WriteSentinel("http://example.com/select-sentinel", "example.com",
                            "https", SentinelId::kEarlyHints, hint.size());
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(hint.data()), hint.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  uint32_t mask = CapabilityMask().Encode();
  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/"
      "select-sentinel&hostname=example.com&mask=" +
      std::to_string(mask) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);

  EXPECT_GT(j["best_score"].get<int>(), 0);
  // The sentinel should have score 0.
  bool found_sentinel_with_zero_score = false;
  for (const auto& alt : j["alternates"]) {
    if (alt["is_sentinel"].get<bool>()) {
      EXPECT_EQ(alt["score"].get<int>(), 0);
      found_sentinel_with_zero_score = true;
    }
  }
  EXPECT_TRUE(found_sentinel_with_zero_score);
}

TEST_F(CacheHandlersTest, ReprocessWithCallbacks) {
  // Set up ctx with actual clear_dedup and enqueue_reprocess callbacks.
  bool clear_called = false;
  std::string cleared_url;
  bool enqueue_called = false;
  std::string enqueued_url;
  ContentType enqueued_ct = ContentType::kOther;

  ctx_->clear_dedup = [&](const std::string& u, const std::string& /*hostname*/,
                          const std::string& /*scheme*/,
                          const std::string& /*site*/) {
    clear_called = true;
    cleared_url = u;
  };
  ctx_->enqueue_reprocess = [&](const std::string& u,
                                const std::string& /*hostname*/,
                                const std::string& /*scheme*/,
                                const std::string& /*site*/, ContentType ct) {
    enqueue_called = true;
    enqueued_url = u;
    enqueued_ct = ct;
  };

  // Write an image original so the reprocess handler can read content type.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  WriteTestAlternate("http://example.com/reprocess-cb", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "image-data");
  url_registry_->Record("http://example.com/reprocess-cb", "example.com",
                        "https");

  std::string body =
      R"({"url":"http://example.com/reprocess-cb","hostname":"example.com"})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_TRUE(j["reprocess_enqueued"].get<bool>());

  // Verify callbacks were invoked.
  EXPECT_TRUE(clear_called);
  EXPECT_EQ(cleared_url, "http://example.com/reprocess-cb");
  EXPECT_TRUE(enqueue_called);
  EXPECT_EQ(enqueued_url, "http://example.com/reprocess-cb");
  EXPECT_EQ(enqueued_ct, ContentType::kImage);
}

TEST_F(CacheHandlersTest, ReprocessWithNoOriginal) {
  // Reprocess for a URL with no original alternate in cache.
  // Should still succeed, with kOther content type.
  bool enqueue_called = false;
  ContentType enqueued_ct = ContentType::kHtml;  // Will be overwritten

  ctx_->enqueue_reprocess = [&](const std::string& /*url*/,
                                const std::string& /*hostname*/,
                                const std::string& /*scheme*/,
                                const std::string& /*site*/, ContentType ct) {
    enqueue_called = true;
    enqueued_ct = ct;
  };

  // Register hostname so the validation check passes.
  url_registry_->Record("http://example.com/other", "example.com", "https");

  std::string body =
      R"({"url":"http://example.com/no-original","hostname":"example.com"})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_TRUE(enqueue_called);
  // Content type should default to kOther since no alternate exists.
  EXPECT_EQ(enqueued_ct, ContentType::kOther);
}

TEST_F(CacheHandlersTest, ContentImageNoOriginContentType) {
  // Image with format bits = 0 (original) but empty origin_content_type.
  // Should fall through to "application/octet-stream".
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "";  // Empty
  std::string content = "raw-image-bytes";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/no-ct-img", "example.com", id, meta,
                     content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/"
      "no-ct-img&hostname=example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("application/octet-stream"), std::string::npos);
}

TEST_F(CacheHandlersTest, ContentNonImageEmptyOriginContentType) {
  // Non-image with empty origin_content_type should get octet-stream.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "";
  std::string content = "misc-data";
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/no-ct-other", "example.com", id, meta,
                     content);

  std::string resp = SendRequest(
      "GET /v1/cache/content?url=http://example.com/"
      "no-ct-other&hostname=example.com&alternate_id=" +
      std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("application/octet-stream"), std::string::npos);
}

TEST_F(CacheHandlersTest, PurgeSingleUrlWithoutHostname) {
  // Purge with URL but no hostname in body.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  WriteTestAlternate("http://example.com/purge-nohost", "",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>hi</html>");
  // Register the hostname extracted from the URL so validation passes.
  url_registry_->Record("http://example.com/purge-nohost", "example.com",
                        "https");

  std::string body = R"({"url":"http://example.com/purge-nohost"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["url"], "http://example.com/purge-nohost");
  // hostname is extracted from the URL when not provided explicitly.
  EXPECT_EQ(j["hostname"], "example.com");
}

TEST_F(CacheHandlersTest, ListAlternatesWithEmptyHostname) {
  // The hostname parameter is optional. Test with empty hostname.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  WriteTestAlternate("http://example.com/no-host", "",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>no host</html>");

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/no-host HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["hostname"], "");
}

TEST_F(CacheHandlersTest, SelectEndpointContentTypeReported) {
  // Verify the select endpoint reports content_type from metadata.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kCss;
  meta.origin_content_type = "text/css";
  WriteTestAlternate("http://example.com/select-css", "example.com",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "body{margin:0}");

  uint32_t mask = CapabilityMask().Encode();
  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/"
      "select-css&hostname=example.com&mask=" +
      std::to_string(mask) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);

  EXPECT_GT(j["best_score"].get<int>(), 0);
  // Find the non-sentinel alternate and verify content_type.
  for (const auto& alt : j["alternates"]) {
    if (!alt["is_sentinel"].get<bool>()) {
      EXPECT_EQ(alt["content_type"], "css");
    }
  }
}

TEST_F(CacheHandlersTest, ReprocessWithHostnameOmitted) {
  // Reprocess without hostname in the body: hostname is extracted from the URL.
  // The extracted hostname must be registered or the request is rejected (403).
  url_registry_->Record("http://example.com/page", "example.com", "https");

  std::string body = R"({"url":"http://example.com/reprocess-nohost"})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["url"], "http://example.com/reprocess-nohost");
  // Hostname is extracted from the URL when not provided explicitly.
  EXPECT_EQ(j["hostname"], "example.com");
}

TEST_F(CacheHandlersTest, ReprocessOmittedHostnameBypassPrevented) {
  // Omitting the hostname field must not bypass validation.
  // The hostname is extracted from the URL and checked against known hostnames.
  // If the extracted hostname was never served, the request must be rejected.
  std::string body = R"({"url":"http://unknown.example.org/page"})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("403"), std::string::npos);
  EXPECT_NE(resp.find("never served"), std::string::npos);
}

// =============================================================================
// Coverage: SentinelName for unknown sentinel IDs (line 110-111)
// =============================================================================

TEST_F(CacheHandlersTest, ListAlternatesUnknownSentinelId) {
  // Write a sentinel with an ID that doesn't map to any known SentinelId.
  // We need a byte value where IsSentinel() returns true but the enum
  // switch falls through to default. Sentinel check: ((id >> 2) & 0x03) == 3.
  // Named sentinels go up to 0x9C (kLlmsTxtMeta); 0xFC is a
  // sentinel (low nibble 0xC -> bits 2-3 = 11) not present in the enum ->
  // unknown. (0x8C/0x9C are now kLlmsTxt/kLlmsTxtMeta, so they are named.)
  uint8_t unknown_sentinel_byte = 0xFC;

  // Write directly using cache's lower-level API: WriteSentinel with a
  // cast to SentinelId. This is a bit hacky but needed to test the default case.
  std::string data = "test-unknown-sentinel";
  auto result = cache_->WriteSentinel(
      "http://example.com/unknown-sentinel", "example.com", "https",
      static_cast<SentinelId>(unknown_sentinel_byte), data.size());
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data.data()), data.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "unknown-sentinel&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_GE(j["count"].get<int>(), 1);

  // Find the unknown sentinel and verify its name.
  bool found_unknown = false;
  for (const auto& alt : j["alternates"]) {
    if (alt["is_sentinel"].get<bool>() &&
        alt["alternate_id"].get<int>() == unknown_sentinel_byte) {
      EXPECT_EQ(alt["sentinel_name"], "unknown_sentinel");
      found_unknown = true;
    }
  }
  EXPECT_TRUE(found_unknown)
      << "Should find a sentinel with unknown_sentinel name";
}

TEST_F(CacheHandlersTest, ListAlternatesAgentMarkdownSentinelId) {
  // The agent_optimize markdown variant (0x7C) is a NAMED sentinel.
  uint8_t agent_byte = static_cast<uint8_t>(SentinelId::kAgentMarkdown);
  std::string data = "# rendered markdown";
  auto result =
      cache_->WriteSentinel("http://example.com/agent-md", "example.com",
                            "https", SentinelId::kAgentMarkdown, data.size());
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data.data()), data.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "agent-md&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  bool found = false;
  for (const auto& alt : j["alternates"]) {
    if (alt["is_sentinel"].get<bool>() &&
        alt["alternate_id"].get<int>() == agent_byte) {
      EXPECT_EQ(alt["sentinel_name"], "agent_markdown");
      found = true;
    }
  }
  EXPECT_TRUE(found) << "Should find the agent_markdown sentinel";
}

// =============================================================================
// Coverage: HandlePurge with 'url' key present but not a string (line 511)
// =============================================================================

TEST_F(CacheHandlersTest, PurgeUrlNotString) {
  // url is a number, not a string.
  std::string body = R"({"url":12345})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

// =============================================================================
// Coverage: HandlePurge with 'url' key as array (line 511)
// =============================================================================

TEST_F(CacheHandlersTest, PurgeUrlIsArray) {
  std::string body = R"({"url":["http://example.com/a"]})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

// =============================================================================
// Coverage: HandleReprocess with 'url' key not a string (line 550)
// =============================================================================

TEST_F(CacheHandlersTest, ReprocessUrlNotString) {
  std::string body = R"({"url":42})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

// =============================================================================
// Coverage: HandlePurge with hostname present but not a string (line 518)
// =============================================================================

TEST_F(CacheHandlersTest, PurgeHostnameNotString) {
  // hostname is a number, should be ignored (no crash, hostname stays empty).
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  WriteTestAlternate("http://example.com/purge-hn-test", "",
                     MaskToAlternateId(meta.full_mask & 0xFF), meta,
                     "<html>hi</html>");
  // Register the URL-extracted hostname so validation passes.
  url_registry_->Record("http://example.com/purge-hn-test", "example.com",
                        "https");

  std::string body =
      R"({"url":"http://example.com/purge-hn-test","hostname":123})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  // hostname is extracted from the URL when the JSON value is not a string.
  EXPECT_EQ(j["hostname"], "example.com");
}

// =============================================================================
// Coverage: HandleReprocess with hostname not a string (line 557)
// =============================================================================

TEST_F(CacheHandlersTest, ReprocessHostnameNotString) {
  // Register the hostname that will be extracted from the URL.
  url_registry_->Record("http://example.com/reprocess-hn", "example.com",
                        "https");

  std::string body =
      R"({"url":"http://example.com/reprocess-hn","hostname":false})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["url"], "http://example.com/reprocess-hn");
  // hostname is extracted from the URL when the JSON value is not a string.
  EXPECT_EQ(j["hostname"], "example.com");
}

// =============================================================================
// Coverage: HandleListUrls with null cache pointer (line 287-288)
// =============================================================================

TEST_F(CacheHandlersTest, ListUrlsWithNullCache) {
  // Decisive proof the per-row cache traversal is gone: alternate_count now
  // comes from the registry, so the listing reports the cached value even when
  // the cache pointer is null (previously this loop did a ListAlternates() per
  // row on the event-loop thread — the /console/urls timeout).
  url_registry_->Record("http://example.com/null-cache", "example.com",
                        "https");
  url_registry_->SetAlternateCount("http://example.com/null-cache",
                                   "example.com", "https", 3);

  PageSpeedCache* original_cache = ctx_->cache;
  ctx_->cache = nullptr;

  std::string resp = SendRequest(
      "GET /v1/cache/urls?offset=0&limit=10 HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  ASSERT_GE(j["total"].get<int>(), 1);
  // The null cache is never consulted; the count is served from the registry.
  bool found = false;
  for (const auto& url_entry : j["urls"]) {
    if (url_entry["url"] == "http://example.com/null-cache") {
      EXPECT_EQ(url_entry["alternate_count"], 3);
      found = true;
    }
  }
  EXPECT_TRUE(found);

  // Restore cache pointer.
  ctx_->cache = original_cache;
}

// =============================================================================
// Coverage: HandlePurge with empty JSON body (no "url" key, no "scope")
// =============================================================================

TEST_F(CacheHandlersTest, PurgeEmptyJsonObject) {
  std::string body = R"({})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

// =============================================================================
// Coverage: HandleReprocess with empty JSON body
// =============================================================================

TEST_F(CacheHandlersTest, ReprocessEmptyJsonObject) {
  std::string body = R"({})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("url"), std::string::npos);
}

// =============================================================================
// Coverage: HandlePurge with scope=all but wrong confirm value
// =============================================================================

TEST_F(CacheHandlersTest, PurgeAllWrongConfirmValue) {
  std::string body = R"({"scope":"all","confirm":"yes"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("purge-all"), std::string::npos);
}

// =============================================================================
// Coverage: Select endpoint with multiple alternates including sentinels,
// exercises the sentinel scoring and read failure paths (lines 352-372)
// =============================================================================

TEST_F(CacheHandlersTest, SelectEndpointMultipleAlternatesAndSentinels) {
  // Write two regular alternates and one sentinel.
  AlternateMetadata meta1;
  meta1.full_mask = CapabilityMask().Encode();  // Desktop/Original
  meta1.content_type = ContentType::kImage;
  meta1.origin_content_type = "image/jpeg";
  WriteTestAlternate("http://example.com/multi-select", "example.com",
                     MaskToAlternateId(meta1.full_mask & 0xFF), meta1,
                     "jpeg-data");

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  AlternateMetadata meta2;
  meta2.full_mask = webp_mask.Encode();
  meta2.content_type = ContentType::kImage;
  meta2.origin_content_type = "image/jpeg";
  WriteTestAlternate("http://example.com/multi-select", "example.com",
                     MaskToAlternateId(meta2.full_mask & 0xFF), meta2,
                     "webp-data");

  // Write sentinel.
  std::string hints = "preload-data";
  auto result =
      cache_->WriteSentinel("http://example.com/multi-select", "example.com",
                            "https", SentinelId::kEarlyHints, hints.size());
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(hints.data()), hints.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  // Select with WebP mask — should prefer the WebP alternate.
  uint32_t mask = webp_mask.Encode();
  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/"
      "multi-select&hostname=example.com&mask=" +
      std::to_string(mask) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_GT(j["best_score"].get<int>(), 0);
  EXPECT_GE(j["alternates"].size(), 3u);

  // Verify all alternates are scored.
  int sentinel_count = 0;
  int scored_count = 0;
  for (const auto& alt : j["alternates"]) {
    if (alt["is_sentinel"].get<bool>()) {
      EXPECT_EQ(alt["score"].get<int>(), 0);
      sentinel_count++;
    } else {
      scored_count++;
    }
  }
  EXPECT_GE(sentinel_count, 1);
  EXPECT_GE(scored_count, 2);
}

// =============================================================================
// Coverage: ListAlternates sentinel types not yet tested individually
// (kSubresourceManifest, kBrowserProfile)
// =============================================================================

TEST_F(CacheHandlersTest, ListAlternatesBrowserProfileSentinel) {
  std::string data = "browser-profile-data";
  auto result =
      cache_->WriteSentinel("http://example.com/bp-sentinel", "example.com",
                            "https", SentinelId::kBrowserProfile, data.size());
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data.data()), data.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "bp-sentinel&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_GE(j["count"].get<int>(), 1);

  bool found = false;
  for (const auto& alt : j["alternates"]) {
    if (alt["is_sentinel"].get<bool>() &&
        alt["sentinel_name"] == "browser_profile") {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "Should find browser_profile sentinel";
}

TEST_F(CacheHandlersTest, ListAlternatesSubresourceManifestSentinel) {
  std::string data = "subresource-manifest-data";
  auto result = cache_->WriteSentinel(
      "http://example.com/sr-sentinel", "example.com", "https",
      SentinelId::kSubresourceManifest, data.size());
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data.data()), data.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "sr-sentinel&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_GE(j["count"].get<int>(), 1);

  bool found = false;
  for (const auto& alt : j["alternates"]) {
    if (alt["is_sentinel"].get<bool>() &&
        alt["sentinel_name"] == "subresource_manifest") {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "Should find subresource_manifest sentinel";
}

// =============================================================================
// Coverage: ListAlternates with original_content sentinel
// =============================================================================

TEST_F(CacheHandlersTest, ListAlternatesOriginalContentSentinel) {
  std::string data = "original-content-marker";
  auto result =
      cache_->WriteSentinel("http://example.com/oc-sentinel", "example.com",
                            "https", SentinelId::kOriginalContent, data.size());
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data.data()), data.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/"
      "oc-sentinel&hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  json j = ParseJsonBody(resp);
  EXPECT_GE(j["count"].get<int>(), 1);

  bool found = false;
  for (const auto& alt : j["alternates"]) {
    if (alt["is_sentinel"].get<bool>() &&
        alt["sentinel_name"] == "original_content") {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "Should find original_content sentinel";
}

// =============================================================================
// Coverage: HandleListUrls default offset and limit (no query params)
// =============================================================================

TEST_F(CacheHandlersTest, ListUrlsDefaultParams) {
  url_registry_->Record("http://example.com/default-params", "example.com",
                        "https");

  std::string resp = SendRequest(
      "GET /v1/cache/urls HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["offset"], 0);
  EXPECT_EQ(j["limit"], 100);
  EXPECT_GE(j["total"].get<int>(), 1);
}

// =============================================================================
// Coverage: HandlePurge with scope=all but missing confirm key entirely
// =============================================================================

TEST_F(CacheHandlersTest, PurgeAllMissingConfirmKey) {
  std::string body = R"({"scope":"all"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("purge-all"), std::string::npos);
}

// =============================================================================
// Cooldown Endpoint Tests
// =============================================================================

// Fixture with cooldown callbacks wired into the CacheApiContext.
// ---------------------------------------------------------------------------
// Sites: per-URL purge, reprocess, content and the listing take an optional
// site, and a purge without one reaches every known site.
// ---------------------------------------------------------------------------

namespace {

const std::string kSiteA(64, 'a');
const std::string kSiteB(64, 'b');

std::string PostJson(const std::string& path, const std::string& body) {
  return "POST " + path +
         " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string PurgeBody(const std::string& extra = "") {
  return R"({"url":"/s.css","hostname":"example.com","scheme":"https")" +
         extra + "}";
}

}  // namespace

class SiteCacheHandlersTest : public CacheHandlersTest {
 protected:
  // An entry for /s.css in `site`'s keyspace, recorded in the registry.
  void WriteSited(const std::string& site, const std::string& content) {
    AlternateMetadata meta;
    meta.full_mask = CapabilityMask().Encode();
    meta.content_type = ContentType::kCss;
    auto wh = cache_->WriteAlternate("/s.css", "example.com", "https",
                                     MaskToAlternateId(meta.full_mask & 0xFF),
                                     content.size(), meta, site);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span(content.data(), content.size())))
            .has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
    url_registry_->Record("/s.css", "example.com", "https", site);
  }
  bool Exists(const std::string& site) {
    return cache_->AlternateExists(
        "/s.css", "example.com", "https",
        MaskToAlternateId(CapabilityMask().Encode() & 0xFF), site);
  }
};

TEST_F(SiteCacheHandlersTest, PurgeWithAKnownSitePurgesOnlyThatSite) {
  WriteSited(kSiteA, "a{}");
  WriteSited(kSiteB, "b{}");
  WriteSited("", "u{}");
  std::string resp = SendRequest(
      PostJson("/v1/cache/purge", PurgeBody(R"(,"site":")" + kSiteA + "\"")));
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["deleted"].get<int>(), 1);
  ASSERT_EQ(j["sites"].size(), 1u);
  EXPECT_EQ(j["sites"][0]["site"], kSiteA);
  EXPECT_EQ(j["sites_checked"].get<int>(), 1);
  EXPECT_FALSE(j.contains("note"));
  EXPECT_FALSE(Exists(kSiteA));
  EXPECT_TRUE(Exists(kSiteB));
  EXPECT_TRUE(Exists(""));
}

TEST_F(SiteCacheHandlersTest, PurgeWithAnUnknownSiteIsForbidden) {
  WriteSited(kSiteA, "a{}");
  std::string resp = SendRequest(
      PostJson("/v1/cache/purge", PurgeBody(R"(,"site":")" + kSiteB + "\"")));
  EXPECT_NE(resp.find("403"), std::string::npos) << resp;
  EXPECT_NE(resp.find("Site was never served through this instance."),
            std::string::npos);
  EXPECT_TRUE(Exists(kSiteA));
}

TEST_F(SiteCacheHandlersTest, PurgeWithAMalformedSiteIsABadRequest) {
  WriteSited(kSiteA, "a{}");
  for (const std::string& bad : {R"(,"site":")" + std::string(63, 'a') + "\"",
                                 R"(,"site":")" + std::string(64, 'A') + "\"",
                                 R"(,"site":")" + std::string(63, 'a') + "g\"",
                                 std::string(R"(,"site":7)")}) {
    std::string resp = SendRequest(PostJson("/v1/cache/purge", PurgeBody(bad)));
    EXPECT_NE(resp.find("400"), std::string::npos) << bad << "\n" << resp;
  }
  EXPECT_TRUE(Exists(kSiteA));
}

TEST_F(SiteCacheHandlersTest, PurgeWithoutASiteCountsEachSite) {
  WriteSited(kSiteA, "a{}");
  WriteSited(kSiteB, "b{}");
  WriteSited("", "u{}");
  std::string resp = SendRequest(PostJson("/v1/cache/purge", PurgeBody()));
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["deleted"].get<int>(), 3);
  EXPECT_EQ(j["sites_checked"].get<int>(), 3);
  ASSERT_EQ(j["sites"].size(), 3u);
  std::set<std::string> seen;
  for (const auto& s : j["sites"]) {
    EXPECT_EQ(s["deleted"].get<int>(), 1);
    seen.insert(s["site"].get<std::string>());
  }
  EXPECT_EQ(seen, (std::set<std::string>{kSiteA, kSiteB, ""}));
  EXPECT_FALSE(j["known_sites_incomplete"].get<bool>());
  EXPECT_FALSE(Exists(kSiteA) || Exists(kSiteB) || Exists(""));
}

TEST_F(SiteCacheHandlersTest, PurgeAfterARestartStillFindsASitedEntry) {
  const std::string sites_file = KnownSitesFilePath(cache_path_);
  {
    KnownSitesWriter writer(
        sites_file, [this] { return url_registry_->TakeKnownSitesIfChanged(); },
        std::chrono::milliseconds(50));
    writer.Start();
    WriteSited(kSiteA, "a{}");
    writer.Stop();
  }
  // A restart: the registry starts empty and loads the known-sites file.
  (void)url_registry_->ClearAll();
  url_registry_->ClearKnownSites();
  ASSERT_FALSE(url_registry_->HasSite(kSiteA));
  url_registry_->Record("/other.css", "example.com", "https");  // the host
  bool readable = false;
  url_registry_->LoadKnownSites(ReadKnownSitesFile(sites_file, &readable));
  ASSERT_TRUE(readable);

  std::string resp = SendRequest(PostJson("/v1/cache/purge", PurgeBody()));
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["deleted"].get<int>(), 1) << resp;
  ASSERT_EQ(j["sites"].size(), 1u);
  EXPECT_EQ(j["sites"][0]["site"], kSiteA);
  EXPECT_FALSE(j.contains("note"));
  EXPECT_FALSE(Exists(kSiteA));
}

TEST_F(SiteCacheHandlersTest, PurgeAllDropsKnownSitesFromMemoryAndFile) {
  const std::string sites_file = KnownSitesFilePath(cache_path_);
  KnownSitesWriter writer(
      sites_file, [this] { return url_registry_->TakeKnownSitesIfChanged(); },
      std::chrono::milliseconds(50));
  writer.Start();
  WriteSited(kSiteA, "a{}");

  // A bare ClearAll keeps them.
  (void)url_registry_->ClearAll();
  EXPECT_TRUE(url_registry_->HasSite(kSiteA));
  url_registry_->Record("/s.css", "example.com", "https", kSiteA);

  std::string resp = SendRequest(
      PostJson("/v1/cache/purge", R"({"scope":"all","confirm":"purge-all"})"));
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  EXPECT_FALSE(url_registry_->HasSite(kSiteA));
  writer.Stop();
  bool readable = false;
  EXPECT_TRUE(ReadKnownSitesFile(sites_file, &readable).empty());
  EXPECT_TRUE(readable);
}

TEST_F(SiteCacheHandlersTest, PurgeReportsIncompleteKnownSites) {
  WriteSited(kSiteA, "a{}");
  ctx_->known_sites_incomplete = [] { return true; };
  json j = ParseJsonBody(SendRequest(PostJson("/v1/cache/purge", PurgeBody())));
  EXPECT_TRUE(j["known_sites_incomplete"].get<bool>());
}

TEST_F(SiteCacheHandlersTest, ReprocessCarriesTheSite) {
  WriteSited(kSiteA, "a{}");
  std::string seen_site = "unset";
  ctx_->enqueue_reprocess = [&](const std::string&, const std::string&,
                                const std::string&, const std::string& site,
                                ContentType) { seen_site = site; };
  std::string resp = SendRequest(PostJson(
      "/v1/cache/reprocess", PurgeBody(R"(,"site":")" + kSiteA + "\"")));
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  EXPECT_EQ(seen_site, kSiteA);
  EXPECT_EQ(ParseJsonBody(resp)["site"], kSiteA);

  resp = SendRequest(
      PostJson("/v1/cache/reprocess", PurgeBody(R"(,"site":"nope")")));
  EXPECT_NE(resp.find("400"), std::string::npos);
}

TEST_F(SiteCacheHandlersTest, AlternatesTakeTheSite) {
  WriteSited(kSiteA, "a{}");
  WriteSited(kSiteB, "bb{}");
  // Without a site the unsited keyspace is read: nothing is stored there.
  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=/s.css&hostname=example.com&scheme=https"
      " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos) << resp;

  resp = SendRequest(
      "GET /v1/cache/alternates?url=/s.css&hostname=example.com&scheme=https"
      "&site=" +
      kSiteA + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["site"], kSiteA);
  EXPECT_EQ(j["cache_key"], "ks4:st1:" + kSiteA + "@https://example.com/s.css");
  EXPECT_EQ(j["alternates"][0]["content_type"], "css");

  resp = SendRequest(
      "GET /v1/cache/alternates?url=/s.css&hostname=example.com&scheme=https"
      "&site=" +
      kSiteB + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  EXPECT_NE(resp.find("\"count\":1"), std::string::npos);
}

TEST_F(SiteCacheHandlersTest, SelectTakesTheSite) {
  WriteSited(kSiteA, "a{}");
  const std::string query =
      "GET /v1/cache/select?url=/s.css&hostname=example.com&scheme=https"
      "&mask=" +
      std::to_string(CapabilityMask().Encode());
  // Without a site the unsited keyspace is read: nothing is stored there.
  std::string resp =
      SendRequest(query + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("404"), std::string::npos) << resp;

  resp = SendRequest(query + "&site=" + kSiteA +
                     " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  json j = ParseJsonBody(resp);
  EXPECT_GT(j["best_score"].get<int>(), 0);
  EXPECT_GE(j["best_index"].get<int>(), 0);

  resp = SendRequest(query + "&site=" + kSiteB +
                     " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("403"), std::string::npos) << resp;
}

TEST_F(SiteCacheHandlersTest, AlternatesWithAMalformedSiteIsABadRequest) {
  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=/s.css&hostname=example.com&site=nope"
      " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("400"), std::string::npos) << resp;
}

TEST_F(SiteCacheHandlersTest, AlternatesWithAnUnknownSiteIsForbidden) {
  WriteSited(kSiteA, "a{}");
  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=/s.css&hostname=example.com&site=" +
      kSiteB + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("403"), std::string::npos) << resp;
}

TEST_F(SiteCacheHandlersTest, ListingAndContentTakeTheSite) {
  WriteSited(kSiteA, "a{}");
  WriteSited(kSiteB, "b{}");
  std::string resp = SendRequest("GET /v1/cache/urls?site=" + kSiteA +
                                 " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  json j = ParseJsonBody(resp);
  ASSERT_EQ(j["urls"].size(), 1u);
  EXPECT_EQ(j["urls"][0]["site"], kSiteA);
  // The reported key is the one the cache composes: the namespace first,
  // then the site.
  EXPECT_EQ(j["urls"][0]["cache_key"],
            "ks4:st1:" + kSiteA + "@https://example.com/s.css");

  const int id = MaskToAlternateId(CapabilityMask().Encode() & 0xFF);
  resp = SendRequest(
      "GET /v1/cache/content?url=/s.css&hostname=example.com"
      "&alternate_id=" +
      std::to_string(id) + "&site=" + kSiteB +
      " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  EXPECT_NE(resp.find("b{}"), std::string::npos);
  EXPECT_EQ(resp.find("a{}"), std::string::npos);
}

class CooldownHandlersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmp_dir_ = test::MakeTempDir();
    std::filesystem::create_directories(tmp_dir_);
    cache_path_ = (tmp_dir_ / "cache.vol").string();

    PageSpeedCacheConfig cache_config;
    cache_config.volume_path = cache_path_;
    cache_config.volume_size = static_cast<uint64_t>(16 * 1024 * 1024);
    cache_config.ram_cache_size = 0;
    auto cache_result = PageSpeedCache::Create(cache_config);
    ASSERT_TRUE(cache_result.has_value());
    cache_ = std::move(*cache_result);

    url_registry_ = std::make_unique<UrlRegistry>(10000);

    ctx_ = std::make_unique<CacheApiContext>(CacheApiContext{
        .cache = cache_.get(),
        .url_registry = *url_registry_,
        .invalidate_url =
            [this](const std::string& url, const std::string& hostname,
                   const std::string& scheme, const std::string& site) {
              auto result = cache_->Remove(url, hostname, scheme, site);
              return result.has_value() ? 1 : 0;
            },
        .clear_dedup = nullptr,
        .enqueue_reprocess = nullptr,
        .reset_cache = nullptr,
        .get_cooldown =
            [this](const std::string& url, const std::string& hostname,
                   const std::string& /*scheme*/, const std::string& /*site*/)
            -> std::optional<CacheApiContext::CooldownEntry> {
          std::string key = url + "|" + hostname;
          auto it = cooldowns_.find(key);
          if (it == cooldowns_.end()) return std::nullopt;
          return it->second;
        },
        .list_cooldowns =
            [this]() -> std::vector<CacheApiContext::CooldownEntry> {
          std::vector<CacheApiContext::CooldownEntry> result;
          result.reserve(cooldowns_.size());
          for (const auto& [key, entry] : cooldowns_) {
            result.push_back(entry);
          }
          return result;
        },
    });

    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();
    HttpServerConfig server_config;
    server_config.port = 0;
    // An empty token now fails closed; these fixtures
    // exercise the routes, not the credential gate.
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
    cache_.reset();
    std::filesystem::remove_all(tmp_dir_);
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
    std::string wire = test::InjectCsrfHeaderForV1Mutations(request);
    int port = server_->bound_port();
    int sock = test::ConnectTcp(port, 2);

    if (sock < 0) return "";
    ssize_t sent = test::SocketWrite(sock, wire.data(), wire.size());
    if (sent != static_cast<ssize_t>(wire.size())) {
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

  json ParseJsonBody(const std::string& response) {
    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) return {};
    return json::parse(response.substr(body_start + 4));
  }

  void AddCooldown(const std::string& url, const std::string& hostname,
                   const std::string& reason, int remaining, int duration) {
    std::string key = url + "|" + hostname;
    cooldowns_[key] = CacheApiContext::CooldownEntry{
        .url = url,
        .hostname = hostname,
        .scheme = "https",
        .reason = reason,
        .remaining_seconds = remaining,
        .duration_seconds = duration,
    };
  }

  std::filesystem::path tmp_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
  std::unique_ptr<UrlRegistry> url_registry_;
  std::unique_ptr<CacheApiContext> ctx_;
  uv_loop_t* loop_ = nullptr;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<HttpServer> server_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
  std::unordered_map<std::string, CacheApiContext::CooldownEntry> cooldowns_;
};

TEST_F(CooldownHandlersTest, CooldownsEndpointEmpty) {
  std::string req =
      "GET /v1/cache/cooldowns HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n";
  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200"), std::string::npos);
  auto j = ParseJsonBody(resp);
  EXPECT_TRUE(j["enabled"].get<bool>());
  EXPECT_EQ(j["count"].get<int>(), 0);
  EXPECT_TRUE(j["cooldowns"].is_array());
  EXPECT_TRUE(j["cooldowns"].empty());
}

TEST_F(CooldownHandlersTest, CooldownsEndpointWithEntries) {
  AddCooldown("http://example.com/page", "example.com", "processing", 47, 60);
  AddCooldown("http://other.com/css", "other.com", "revalidation", 2, 3);

  std::string req =
      "GET /v1/cache/cooldowns HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n";
  std::string resp = SendRequest(req);
  auto j = ParseJsonBody(resp);
  EXPECT_TRUE(j["enabled"].get<bool>());
  EXPECT_EQ(j["count"].get<int>(), 2);
  EXPECT_EQ(j["cooldowns"].size(), 2u);

  // Verify structure of entries.
  for (const auto& entry : j["cooldowns"]) {
    EXPECT_TRUE(entry.contains("url"));
    EXPECT_TRUE(entry.contains("hostname"));
    EXPECT_TRUE(entry.contains("reason"));
    EXPECT_TRUE(entry.contains("remaining_seconds"));
    EXPECT_TRUE(entry.contains("duration_seconds"));
  }
}

TEST_F(CooldownHandlersTest, CooldownsEndpointHostnameFilter) {
  AddCooldown("http://example.com/page", "example.com", "processing", 47, 60);
  AddCooldown("http://other.com/css", "other.com", "revalidation", 2, 3);

  std::string req =
      "GET /v1/cache/cooldowns?hostname=example.com HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n";
  std::string resp = SendRequest(req);
  auto j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"].get<int>(), 1);
  EXPECT_EQ(j["cooldowns"][0]["hostname"].get<std::string>(), "example.com");
}

TEST_F(CooldownHandlersTest, AlternatesIncludesCooldown) {
  // Write a test alternate so the alternates endpoint has data.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  meta.flags = 0;
  meta.origin_content_type = "text/html";
  auto serialized = meta.Serialize();
  std::string content =
      std::string(reinterpret_cast<const char*>(serialized.data()),
                  serialized.size()) +
      "<html>test</html>";
  auto result = cache_->WriteAlternate(
      "http://example.com/test", "example.com", "https",
      static_cast<AlternateId>(CapabilityMask().Encode() & 0xFF),
      content.size(), meta);
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(content.data()), content.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  // Add cooldown for this URL.
  AddCooldown("http://example.com/test", "example.com", "revalidation", 2, 3);

  std::string req =
      "GET "
      "/v1/cache/alternates?url=http://example.com/test&hostname=example.com"
      " HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n";
  std::string resp = SendRequest(req);
  auto j = ParseJsonBody(resp);
  EXPECT_TRUE(j.contains("cooldown"));
  EXPECT_EQ(j["cooldown"]["reason"].get<std::string>(), "revalidation");
  EXPECT_EQ(j["cooldown"]["remaining_seconds"].get<int>(), 2);
  EXPECT_EQ(j["cooldown"]["duration_seconds"].get<int>(), 3);
}

TEST_F(CooldownHandlersTest, AlternatesNoCooldown) {
  // Write a test alternate.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  meta.flags = 0;
  meta.origin_content_type = "text/html";
  auto serialized = meta.Serialize();
  std::string content =
      std::string(reinterpret_cast<const char*>(serialized.data()),
                  serialized.size()) +
      "<html>test</html>";
  auto result = cache_->WriteAlternate(
      "http://example.com/test", "example.com", "https",
      static_cast<AlternateId>(CapabilityMask().Encode() & 0xFF),
      content.size(), meta);
  ASSERT_TRUE(result.has_value());
  auto written = result->write_sync(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(content.data()), content.size()));
  ASSERT_TRUE(written.has_value());
  (void)result->close_sync();

  // No cooldown added for this URL.
  std::string req =
      "GET "
      "/v1/cache/alternates?url=http://example.com/test&hostname=example.com"
      " HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n";
  std::string resp = SendRequest(req);
  auto j = ParseJsonBody(resp);
  EXPECT_FALSE(j.contains("cooldown"));
}

// Verify existing fixture (no cooldown callbacks) still works — endpoint
// returns enabled=false.
TEST_F(CacheHandlersTest, CooldownsEndpointDisabled) {
  std::string req =
      "GET /v1/cache/cooldowns HTTP/1.1\r\n"
      "Host: localhost\r\n\r\n";
  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("200"), std::string::npos);
  auto j = ParseJsonBody(resp);
  EXPECT_FALSE(j["enabled"].get<bool>());
  EXPECT_EQ(j["count"].get<int>(), 0);
}

// Bug #9 regression: PURGE returns note when deleted == 0
TEST_F(CacheHandlersTest, PurgeNonExistentReturnsNote) {
  // PURGE a URL that was never cached. Response should have deleted: 0
  // and a "note" field explaining the entry was not found.
  // Register the hostname so it passes validation.
  url_registry_->Record("http://example.com/other", "example.com", "https");
  std::string body = R"({"url":"/never-cached.jpg","hostname":"example.com"})";
  std::string request =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  auto resp = SendRequest(request);
  ASSERT_FALSE(resp.empty());
  EXPECT_NE(resp.find("200"), std::string::npos);

  auto j = ParseJsonBody(resp);
  EXPECT_EQ(j["deleted"].get<int>(), 0);
  EXPECT_TRUE(j.contains("note"));
  EXPECT_NE(j["note"].get<std::string>().find("not found"), std::string::npos);
}

// =============================================================================
// Size reporting after overwrite
// =============================================================================

TEST_F(CacheHandlersTest, ListAlternatesSizeAfterOverwrite) {
  // Write a large original at AlternateId 8 (Desktop/Identity).
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kCss;
  meta.origin_content_type = "text/css";
  std::string large_content(6000, 'x');
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/style.css", "example.com", id, meta,
                     large_content);

  // Overwrite the same AlternateId with smaller content (simulates worker
  // writing a minified version).
  std::string small_content(3000, 'y');
  WriteTestAlternate("http://example.com/style.css", "example.com", id, meta,
                     small_content);

  // ListAlternates should report the latest (smaller) size.
  std::string resp = SendRequest(
      "GET /v1/cache/alternates?url=http://example.com/style.css&hostname="
      "example.com HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  EXPECT_EQ(j["count"], 1);
  // Size must be the smaller content (3000), not the original (6000).
  auto reported_size = j["alternates"][0]["size"].get<uint64_t>();
  EXPECT_EQ(reported_size, small_content.size());
}

TEST_F(CacheHandlersTest, SelectSizeAfterOverwrite) {
  // Same overwrite scenario but via the /v1/cache/select endpoint.
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kCss;
  meta.origin_content_type = "text/css";
  std::string large_content(6000, 'x');
  AlternateId id = MaskToAlternateId(meta.full_mask & 0xFF);
  WriteTestAlternate("http://example.com/sel.css", "example.com", id, meta,
                     large_content);

  std::string small_content(3000, 'y');
  WriteTestAlternate("http://example.com/sel.css", "example.com", id, meta,
                     small_content);

  uint32_t mask = CapabilityMask().Encode();
  std::string resp = SendRequest(
      "GET /v1/cache/select?url=http://example.com/sel.css&hostname="
      "example.com&mask=" +
      std::to_string(mask) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  json j = ParseJsonBody(resp);
  // Find the non-sentinel alternate.
  for (auto& alt : j["alternates"]) {
    if (!alt["is_sentinel"].get<bool>()) {
      EXPECT_EQ(alt["size"].get<uint64_t>(), small_content.size());
    }
  }
}

// =============================================================================
// Empty-hostname bypass prevention (issue #213 review finding 1)
// =============================================================================

TEST_F(CacheHandlersTest, PurgeRejectsEmptyHostnameString) {
  // Sending {"url":"...", "hostname":""} must return 400, not bypass
  // the hostname validation gate.
  url_registry_->Record("http://known.com/page", "known.com", "https");

  std::string body = R"({"url":"http://known.com/page","hostname":""})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("must not be empty"), std::string::npos);
}

TEST_F(CacheHandlersTest, ReprocessRejectsEmptyHostnameString) {
  // Same empty-hostname bypass test for the reprocess endpoint.
  url_registry_->Record("http://known.com/page", "known.com", "https");

  std::string body = R"({"url":"http://known.com/page","hostname":""})";
  std::string req =
      "POST /v1/cache/reprocess HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("400"), std::string::npos);
  EXPECT_NE(resp.find("must not be empty"), std::string::npos);
}

// =============================================================================
// URL-extracted hostname validation in purge (issue #213 review finding 3/4)
// =============================================================================

TEST_F(CacheHandlersTest, PurgeOmittedHostnameBypassPrevented) {
  // Omitting the hostname field in purge must validate the URL-extracted
  // hostname against known hostnames.
  url_registry_->Record("http://known.com/page", "known.com", "https");

  std::string body = R"({"url":"http://unknown-evil.org/page"})";
  std::string req =
      "POST /v1/cache/purge HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  std::string resp = SendRequest(req);
  EXPECT_NE(resp.find("403"), std::string::npos);
  EXPECT_NE(resp.find("never served"), std::string::npos);
}

}  // namespace
}  // namespace pagespeed
