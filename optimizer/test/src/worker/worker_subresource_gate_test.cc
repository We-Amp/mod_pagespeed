// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A page variant is built only from sub-resources the server serves to an
// anonymous client.  These tests run the worker against a small local origin
// that answers the worker's own requests, so the check is the real one: the
// worker's fetcher, its User-Agent, and the origin's answer.

// Keep the fixture header first: it preserves the old worker_test.cc include
// order for the code it carries.
#include "test/src/worker/worker_test_fixture.h"

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/subresource_gate.h"
#include "src/worker/worker.h"

namespace pagespeed {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// A loopback origin that refuses everything under /held/ (403) and answers
// 200 for anything else, recording each request line and User-Agent.
class LocalOrigin {
 public:
  LocalOrigin() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return;
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd_, 16) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }

  ~LocalOrigin() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
  }

  bool ok() const { return fd_ >= 0 && port_ != 0; }
  int port() const { return port_; }

  std::vector<std::string> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

 private:
  void Serve() {
    while (!stop_.load()) {
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      std::string req;
      char buf[2048];
      while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        pollfd cp{c, POLLIN, 0};
        if (::poll(&cp, 1, 1000) <= 0) break;
        ssize_t n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
      }
      const std::string line = req.substr(0, req.find("\r\n"));
      std::string ua;
      if (size_t at = req.find("\r\nUser-Agent: "); at != std::string::npos) {
        size_t start = at + 14;
        ua = req.substr(start, req.find("\r\n", start) - start);
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(line + " | ");
        requests_.back() += ua;
      }
      const bool held = line.find(" /held/") != std::string::npos;
      const std::string resp =
          held ? "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                 "Connection: close\r\n\r\n"
               : "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n";
      ::send(c, resp.data(), resp.size(), kSendFlags);
      ::close(c);
    }
  }

  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
};

class WorkerSubresourceGateTest : public WorkerTest {
 protected:
  void SetUp() override {
    WorkerTest::SetUp();
    // These tests ask the real server.
    SubresourceGate::SetVerdictOverrideForTest(nullptr);
  }

  // The fixture's CacheOriginal keys under https; the local origin is http.
  void CacheHttp(PageSpeedCache* cache, const std::string& url,
                 const std::string& content, const std::string& hostname,
                 std::uint8_t flags = 0) {
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = DefaultOriginContentType(content);
    meta.flags = flags;
    auto wh =
        cache->WriteAlternate(url, hostname, "http", id, content.size(), meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span(content.data(), content.size())))
            .has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  std::string ReadHttpVariant(PageSpeedCache* cache, const std::string& url,
                              const std::string& hostname) {
    CapabilityMask mask = CapabilityMask::Decode(0);
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto result = cache->ReadAlternate(url, hostname, "http", id);
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Runs one HTML notification for `page` against the local origin and
  // returns the HTML variant the worker wrote ("" when none).
  // `open_flags` / `held_flags` are the flag bits the two sheets were
  // recorded with.
  std::string RunPage(const std::string& page, const std::string& html,
                      bool host_vouched, std::vector<std::string>* requests,
                      std::uint8_t open_flags = 0,
                      std::uint8_t held_flags = 0) {
    LocalOrigin origin;
    EXPECT_TRUE(origin.ok());
    if (!origin.ok()) return "";
    const std::string host = "127.0.0.1:" + std::to_string(origin.port());

    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.unsafe_force_async_css = true;  // not under test here
    Worker worker(config, nullptr);
    EXPECT_TRUE(worker.Initialize());

    CacheHttp(worker.cache(), page, html, host);
    CacheHttp(worker.cache(), "/held/held.css", ".heldrule { color: #a1b2c3; }",
              host, held_flags);
    CacheHttp(worker.cache(), "/open/open.css", ".openrule { color: #c3b2a1; }",
              host, open_flags);

    std::thread worker_thread([&worker]() { worker.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CacheNotification notification;
    notification.url = "http://" + host + page;
    notification.hostname = host;
    notification.scheme = "http";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    notification.host_vouched = host_vouched;
    SendNotification(notification);

    std::string variant;
    for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      variant = ReadHttpVariant(worker.cache(), page, host);
      if (!variant.empty()) break;
    }
    WaitForWorkerIdle(worker);
    worker.Shutdown();
    worker_thread.join();
    *requests = origin.requests();
    return variant;
  }

  static constexpr const char* kPage =
      "<html><head>"
      "<style>.inlinerule { color: #0f0e0d; }</style>"
      "<link rel=\"stylesheet\" href=\"/held/held.css\">"
      "<link rel=\"stylesheet\" href=\"/open/open.css\">"
      "</head><body><div class=\"inlinerule\">i</div>"
      "<div class=\"heldrule\">h</div><div class=\"openrule\">o</div>"
      "</body></html>";
};

// A stylesheet the server refuses to anonymous clients is in the cache (an
// admitted client's request put it there), yet none of its bytes reach the
// page variant; a stylesheet the server serves to anyone is still used.
TEST_F(WorkerSubresourceGateTest, RefusedStylesheetNeverReachesTheVariant) {
  std::vector<std::string> requests;
  const std::string variant =
      RunPage("/page.html", kPage, /*host_vouched=*/true, &requests);
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos)
      << variant;
  EXPECT_EQ(variant.find("a1b2c3"), std::string::npos)
      << "refused stylesheet content in the variant:\n"
      << variant;
  EXPECT_NE(variant.find("c3b2a1"), std::string::npos)
      << "public stylesheet content missing from the variant:\n"
      << variant;

  // The worker asked with HEAD, as its own anonymous agent, once per sheet.
  int held_heads = 0;
  int open_heads = 0;
  for (const std::string& r : requests) {
    EXPECT_EQ(r.rfind("HEAD ", 0), 0u) << r;
    EXPECT_NE(r.find("mod_pagespeed"), std::string::npos) << r;
    if (r.find(" /held/held.css ") != std::string::npos) ++held_heads;
    if (r.find(" /open/open.css ") != std::string::npos) ++open_heads;
  }
  EXPECT_EQ(held_heads, 1);
  EXPECT_EQ(open_heads, 1);
}

// For a sender that did not vouch for the host, the worker may not ask it,
// so no cached stylesheet is used at all: absent is the only safe answer.
TEST_F(WorkerSubresourceGateTest, UnvouchedPageUsesNoCachedStylesheet) {
  std::vector<std::string> requests;
  const std::string variant =
      RunPage("/page2.html", kPage, /*host_vouched=*/false, &requests);
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";
  EXPECT_EQ(variant.find("a1b2c3"), std::string::npos) << variant;
  EXPECT_EQ(variant.find("c3b2a1"), std::string::npos) << variant;
  EXPECT_NE(variant.find("0f0e0d"), std::string::npos) << variant;
  EXPECT_TRUE(requests.empty());
}

// A stylesheet the front end recorded from an anonymous request is used for a
// page under a host the sender did not confirm, without asking the host; one
// recorded without that mark is still left out.
TEST_F(WorkerSubresourceGateTest,
       UnvouchedPageUsesStylesheetRecordedFromAnAnonymousRequest) {
  std::vector<std::string> requests;
  const std::string variant =
      RunPage("/page3.html", kPage, /*host_vouched=*/false, &requests,
              /*open_flags=*/AlternateMetadata::kFlagAnonymousRecorded |
                  AlternateMetadata::kFlagHeadersClassified,
              /*held_flags=*/AlternateMetadata::kFlagHeadersClassified);
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";
  const size_t critical = variant.find("data-pagespeed-critical");
  ASSERT_NE(critical, std::string::npos) << variant;
  const size_t open_rule = variant.find("c3b2a1");
  EXPECT_NE(open_rule, std::string::npos)
      << "anonymously recorded stylesheet missing from the variant:\n"
      << variant;
  EXPECT_GT(open_rule, critical) << variant;
  EXPECT_EQ(variant.find("a1b2c3"), std::string::npos)
      << "unmarked stylesheet content in the variant:\n"
      << variant;
  EXPECT_TRUE(requests.empty());
}

// The same page with neither sheet marked uses neither.
TEST_F(WorkerSubresourceGateTest,
       UnvouchedPageIgnoresStylesheetsWithoutTheAnonymousMark) {
  std::vector<std::string> requests;
  const std::string variant =
      RunPage("/page4.html", kPage, /*host_vouched=*/false, &requests,
              /*open_flags=*/AlternateMetadata::kFlagHeadersClassified,
              /*held_flags=*/AlternateMetadata::kFlagHeadersClassified);
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";
  EXPECT_EQ(variant.find("c3b2a1"), std::string::npos) << variant;
  EXPECT_EQ(variant.find("a1b2c3"), std::string::npos) << variant;
  EXPECT_TRUE(requests.empty());
}

// Under a confirmed host marked sheets are asked about exactly as unmarked
// ones: the host's answer decides, so the refused sheet stays out even though
// it carries the mark.
TEST_F(WorkerSubresourceGateTest, VouchedPageAsksAboutMarkedSheetsAsBefore) {
  std::vector<std::string> requests;
  const std::string variant =
      RunPage("/page5.html", kPage, /*host_vouched=*/true, &requests,
              /*open_flags=*/AlternateMetadata::kFlagAnonymousRecorded,
              /*held_flags=*/AlternateMetadata::kFlagAnonymousRecorded);
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";
  EXPECT_NE(variant.find("c3b2a1"), std::string::npos) << variant;
  EXPECT_EQ(variant.find("a1b2c3"), std::string::npos) << variant;
  int held_heads = 0;
  int open_heads = 0;
  for (const std::string& r : requests) {
    if (r.find(" /held/held.css ") != std::string::npos) ++held_heads;
    if (r.find(" /open/open.css ") != std::string::npos) ++open_heads;
  }
  EXPECT_EQ(held_heads, 1);
  EXPECT_EQ(open_heads, 1);
}

}  // namespace
}  // namespace pagespeed

#endif  // _WIN32
