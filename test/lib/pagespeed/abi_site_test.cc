// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The PS_API 1.12 site surface: the _site cache entry points, and the site
// and flags on ps_notify_worker_ex.

#include <stddef.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/pagespeed/pagespeed.h"
#include "src/proto/worker_ipc.h"
#include "test/test_util/temp_dir.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

const std::string kSiteA(PS_SITE_ID_CHARS, 'a');
const std::string kSiteB = std::string(PS_SITE_ID_CHARS - 1, 'a') + "b";
constexpr char kUrl[] = "/style.css";
constexpr char kHost[] = "example.com";
constexpr char kScheme[] = "https";

class AbiSiteCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    const std::string path = temp_dir_ + "/cache.vol";
    ps_cache_config_t config;
    ps_cache_config_init(&config);
    config.volume_path = path.c_str();
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    ASSERT_EQ(ps_cache_open(&config, &cache_), PS_OK);
  }

  void TearDown() override {
    if (cache_ != nullptr) ps_cache_close(cache_);
    std::filesystem::remove_all(temp_dir_);
  }

  // Writes a durable original through the unsited entry point (site ==
  // nullptr) or the _site one.
  void WriteOriginal(const char* site, bool use_site_entry,
                     const std::string& body) {
    ps_write_params_t params;
    ps_write_params_init_sized(&params, sizeof(params));
    params.content_length = body.size();
    params.content_type = PS_CONTENT_CSS;
    ps_write_handle_t* wh = nullptr;
    if (use_site_entry) {
      ASSERT_EQ(PS_OK, ps_cache_write_original_site(
                           cache_, kUrl, kHost, kScheme, site, &params, &wh));
    } else {
      ASSERT_EQ(PS_OK, ps_cache_write_original(cache_, kUrl, kHost, kScheme,
                                               &params, &wh));
    }
    ASSERT_EQ(PS_OK, ps_write_data(wh, body.data(), body.size()));
    ASSERT_EQ(PS_OK, ps_write_close(wh));
  }

  // Reads the durable original under `site`; "<miss>" when there is none.
  std::string ReadOriginal(const char* site) {
    ps_read_result_t* r = nullptr;
    if (ps_cache_read_alternate_site(cache_, kUrl, kHost, kScheme, site,
                                     PS_SENTINEL_ORIGINAL, &r) != PS_OK) {
      return "<miss>";
    }
    const uint8_t* data = nullptr;
    size_t len = 0;
    std::string out = "<unreadable>";
    if (ps_read_content(r, &data, &len) == PS_OK) {
      out.assign(reinterpret_cast<const char*>(data), len);
    }
    ps_read_free(r);
    return out;
  }

  std::string temp_dir_;
  ps_cache_t* cache_ = nullptr;
};

TEST_F(AbiSiteCacheTest, AnEmptySiteIsTheUnsitedKeyspace) {
  WriteOriginal(nullptr, /*use_site_entry=*/false, "unsited-body");
  EXPECT_EQ("unsited-body", ReadOriginal(nullptr));
  EXPECT_EQ("unsited-body", ReadOriginal(""));
  // And the unsited reader sees what the _site writer wrote with "".
  WriteOriginal("", /*use_site_entry=*/true, "written-with-empty-site");
  ps_read_result_t* r = nullptr;
  ASSERT_EQ(PS_OK, ps_cache_read_alternate(cache_, kUrl, kHost, kScheme,
                                           PS_SENTINEL_ORIGINAL, &r));
  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(PS_OK, ps_read_content(r, &data, &len));
  EXPECT_EQ("written-with-empty-site",
            std::string(reinterpret_cast<const char*>(data), len));
  ps_read_free(r);
}

TEST_F(AbiSiteCacheTest, TwoSitesAndTheUnsitedKeyspaceAreMutualMisses) {
  WriteOriginal(kSiteA.c_str(), true, "body-a");
  EXPECT_EQ("body-a", ReadOriginal(kSiteA.c_str()));
  EXPECT_EQ("<miss>", ReadOriginal(kSiteB.c_str()));
  EXPECT_EQ("<miss>", ReadOriginal(nullptr));

  WriteOriginal(kSiteB.c_str(), true, "body-b");
  WriteOriginal(nullptr, false, "body-unsited");
  EXPECT_EQ("body-a", ReadOriginal(kSiteA.c_str()));
  EXPECT_EQ("body-b", ReadOriginal(kSiteB.c_str()));
  EXPECT_EQ("body-unsited", ReadOriginal(nullptr));
}

TEST_F(AbiSiteCacheTest, ReadBestSiteHitsOnlyItsOwnSite) {
  // A content variant written through the unsited writer is found by
  // read_best_site with an empty site, and by no site.
  const std::string body = "variant-body";
  ps_write_params_t params;
  ps_write_params_init_sized(&params, sizeof(params));
  params.content_length = body.size();
  params.content_type = PS_CONTENT_CSS;
  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(PS_OK,
            ps_cache_write_begin(cache_, kUrl, kHost, kScheme, &params, &wh));
  ASSERT_EQ(PS_OK, ps_write_data(wh, body.data(), body.size()));
  ASSERT_EQ(PS_OK, ps_write_close(wh));

  ps_read_result_t* r = nullptr;
  ASSERT_EQ(PS_OK,
            ps_cache_read_best_site(cache_, kUrl, kHost, kScheme, "", 0, &r));
  ps_read_free(r);
  r = nullptr;
  ASSERT_EQ(PS_OK, ps_cache_read_best_site(cache_, kUrl, kHost, kScheme,
                                           nullptr, 0, &r));
  ps_read_free(r);
  r = nullptr;
  EXPECT_EQ(PS_ERR_NOT_FOUND,
            ps_cache_read_best_site(cache_, kUrl, kHost, kScheme,
                                    kSiteA.c_str(), 0, &r));
}

TEST_F(AbiSiteCacheTest, AMalformedSiteIsRefused) {
  for (const std::string& bad :
       {std::string(PS_SITE_ID_CHARS - 1, 'a'),
        std::string(PS_SITE_ID_CHARS + 1, 'a'),
        std::string(PS_SITE_ID_CHARS, 'A'),
        std::string(PS_SITE_ID_CHARS - 1, 'a') + "g"}) {
    ps_read_result_t* r = nullptr;
    EXPECT_EQ(PS_ERR_INVALID_ARG,
              ps_cache_read_best_site(cache_, kUrl, kHost, kScheme, bad.c_str(),
                                      0, &r));
    EXPECT_EQ(PS_ERR_INVALID_ARG, ps_cache_read_alternate_site(
                                      cache_, kUrl, kHost, kScheme, bad.c_str(),
                                      PS_SENTINEL_ORIGINAL, &r));
    ps_write_params_t params;
    ps_write_params_init_sized(&params, sizeof(params));
    params.content_length = 1;
    ps_write_handle_t* wh = nullptr;
    EXPECT_EQ(PS_ERR_INVALID_ARG,
              ps_cache_write_original_site(cache_, kUrl, kHost, kScheme,
                                           bad.c_str(), &params, &wh));
    EXPECT_EQ(nullptr, wh);
  }
  // Nothing was written by the refused writes.
  EXPECT_EQ("<miss>", ReadOriginal(nullptr));
}

// ---------------------------------------------------------------------------
// ps_notify_worker_ex: site, flags, and the refusals. A local listener
// captures what is actually sent.
// ---------------------------------------------------------------------------

#ifndef _WIN32

class FrameListener {
 public:
  explicit FrameListener(std::string path) : path_(std::move(path)) {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    ok_ = fd_ >= 0 &&
          bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
          listen(fd_, 16) == 0;
    if (ok_) thread_ = std::thread([this] { Run(); });
  }

  ~FrameListener() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) close(fd_);
  }

  bool ok() const { return ok_; }

  // Waits briefly for in-flight connections, then returns every frame
  // received so far, decoded.
  std::vector<pagespeed::CacheNotification> Frames() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<pagespeed::CacheNotification> out;
    size_t pos = 0;
    while (bytes_.size() - pos >= 4) {
      const auto* p =
          reinterpret_cast<const unsigned char*>(bytes_.data()) + pos;
      const size_t total =
          4 + ((static_cast<size_t>(p[0]) << 24) |
               (static_cast<size_t>(p[1]) << 16) |
               (static_cast<size_t>(p[2]) << 8) | static_cast<size_t>(p[3]));
      if (bytes_.size() - pos < total) break;
      pagespeed::CacheNotification n;
      EXPECT_TRUE(pagespeed::CacheNotification::Deserialize(
          std::string_view(bytes_.data() + pos, total), &n));
      out.push_back(n);
      pos += total;
    }
    return out;
  }

  int connections() const { return connections_.load(); }

 private:
  void Run() {
    while (!stop_) {
      pollfd pfd = {fd_, POLLIN, 0};
      if (poll(&pfd, 1, 50) <= 0) continue;
      const int c = accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      connections_.fetch_add(1);
      char buf[4096];
      for (;;) {
        pollfd cp = {c, POLLIN, 0};
        if (poll(&cp, 1, 500) <= 0) break;
        const ssize_t n = read(c, buf, sizeof(buf));
        if (n <= 0) break;
        std::lock_guard<std::mutex> lock(mu_);
        bytes_.append(buf, static_cast<size_t>(n));
      }
      close(c);
    }
  }

  std::string path_;
  int fd_ = -1;
  bool ok_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<int> connections_{0};
  std::thread thread_;
  std::mutex mu_;
  std::string bytes_;
};

class AbiSiteNotifyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    socket_path_ = temp_dir_ + "/notify.sock";
    listener_ = std::make_unique<FrameListener>(socket_path_);
    ASSERT_TRUE(listener_->ok());
  }

  void TearDown() override {
    listener_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  // A valid notification from the module: a site, FROM_MODULE, a CSS type.
  static ps_notify_params_t ModuleParams() {
    ps_notify_params_t p;
    ps_notify_params_init_auto(&p);
    p.url = kUrl;
    p.hostname = kHost;
    p.scheme = kScheme;
    p.content_type = PS_CONTENT_CSS;
    p.mask = 0x08;
    p.site = kSiteA.c_str();
    p.notify_flags = PS_NOTIFY_FLAG_FROM_MODULE;
    return p;
  }

  std::string temp_dir_;
  std::string socket_path_;
  std::unique_ptr<FrameListener> listener_;
};

TEST_F(AbiSiteNotifyTest, AModuleNotificationCarriesItsSiteAndFlag) {
  const ps_notify_params_t p = ModuleParams();
  ASSERT_EQ(PS_OK, ps_notify_worker_ex(socket_path_.c_str(), &p));
  const auto frames = listener_->Frames();
  ASSERT_EQ(1u, frames.size());
  EXPECT_EQ(kSiteA, frames[0].site);
  EXPECT_TRUE(frames[0].from_module);
  EXPECT_FALSE(frames[0].host_vouched);
  EXPECT_FALSE(frames[0].legacy_ex_caller);
}

TEST_F(AbiSiteNotifyTest, EveryRefusalRuleSendsNothing) {
  std::vector<ps_notify_params_t> refused;
  {
    ps_notify_params_t p = ModuleParams();
    p.site = "";  // module without a site
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.site = nullptr;  // module without a site
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.content_type = PS_CONTENT_HTML;  // module HTML (also a zeroed type)
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.notify_flags |= PS_NOTIFY_FLAG_HOST_VOUCHED;
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.agent_request = 1;
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.mask = 0xFFFFFFFDu;  // the llms.txt build mask
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.notify_flags = 0x4u;  // the library's own bit
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.notify_flags |= PS_NOTIFY_FLAG_HTML_STORED;  // stored, but not HTML
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.content_type = PS_CONTENT_HTML;  // HTML, stored, but not vouched
    p.notify_flags |= PS_NOTIFY_FLAG_HTML_STORED;
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.content_type = PS_CONTENT_HTML;  // HTML, vouched, but not stored
    p.notify_flags |= PS_NOTIFY_FLAG_HOST_VOUCHED;
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.notify_flags = PS_NOTIFY_FLAG_HOST_VOUCHED | PS_NOTIFY_FLAG_HTML_STORED;
    p.content_type = PS_CONTENT_HTML;  // stored, but not from the module
    refused.push_back(p);
  }
  {
    ps_notify_params_t p = ModuleParams();
    p.content_type = PS_CONTENT_HTML;  // a stored page, with the llms.txt mask
    p.notify_flags |= PS_NOTIFY_FLAG_HOST_VOUCHED | PS_NOTIFY_FLAG_HTML_STORED;
    p.mask = 0xFFFFFFFDu;
    refused.push_back(p);
  }
  for (uint32_t bit = 4; bit < 32; ++bit) {
    ps_notify_params_t p = ModuleParams();
    p.notify_flags |= (1u << bit);  // reserved
    refused.push_back(p);
  }
  static const std::string kBadSites[] = {
      std::string(PS_SITE_ID_CHARS - 1, 'a'),
      std::string(PS_SITE_ID_CHARS + 1, 'a'),
      std::string(PS_SITE_ID_CHARS, 'A')};
  for (const std::string& bad : kBadSites) {
    ps_notify_params_t p = ModuleParams();
    p.notify_flags = 0;
    p.site = bad.c_str();  // malformed site, from any sender
    refused.push_back(p);
  }

  for (size_t i = 0; i < refused.size(); ++i) {
    EXPECT_EQ(PS_ERR_INVALID_ARG,
              ps_notify_worker_ex(socket_path_.c_str(), &refused[i]))
        << "case " << i;
  }
  EXPECT_TRUE(listener_->Frames().empty());
  EXPECT_EQ(0, listener_->connections());

  // Control: a sender that is not the module may send HTML, unsited and
  // vouched, with an agent request.
  ps_notify_params_t p = ModuleParams();
  p.site = nullptr;
  p.notify_flags = PS_NOTIFY_FLAG_HOST_VOUCHED;
  p.content_type = PS_CONTENT_HTML;
  p.agent_request = 1;
  ASSERT_EQ(PS_OK, ps_notify_worker_ex(socket_path_.c_str(), &p));
  const auto frames = listener_->Frames();
  ASSERT_EQ(1u, frames.size());
  EXPECT_TRUE(frames[0].site.empty());
  EXPECT_TRUE(frames[0].host_vouched);
  EXPECT_FALSE(frames[0].from_module);
}

// PS_API 1.14: a page the module stored itself, for a host it vouched for,
// may be named in an HTML notification, with an agent request; the frame
// carries all three flags.
TEST_F(AbiSiteNotifyTest, AStoredPageNotificationCarriesTheStoredFlag) {
  ps_notify_params_t p = ModuleParams();
  p.content_type = PS_CONTENT_HTML;
  p.agent_request = 1;
  p.notify_flags = PS_NOTIFY_FLAG_FROM_MODULE | PS_NOTIFY_FLAG_HOST_VOUCHED |
                   PS_NOTIFY_FLAG_HTML_STORED;
  ASSERT_EQ(PS_OK, ps_notify_worker_ex(socket_path_.c_str(), &p));
  const auto frames = listener_->Frames();
  ASSERT_EQ(1u, frames.size());
  EXPECT_EQ(kSiteA, frames[0].site);
  EXPECT_TRUE(frames[0].from_module);
  EXPECT_TRUE(frames[0].host_vouched);
  EXPECT_TRUE(frames[0].html_stored);
  EXPECT_TRUE(frames[0].agent_request);
  EXPECT_EQ(frames[0].content_type, pagespeed::ContentType::kHtml);
}

TEST_F(AbiSiteNotifyTest, ACallerPredatingTheSiteFieldsIsSentUnsitedOnce) {
  // A caller built against the 1.5 header: struct_size 72. Whatever lies past
  // that is not its memory and must not be read.
  ps_notify_params_t p = ModuleParams();
  p.struct_size = offsetof(ps_notify_params_t, site);
  ASSERT_EQ(72u, p.struct_size);
  p.content_type = PS_CONTENT_HTML;  // allowed: it does not claim the module
  p.site = "not-a-site";             // past struct_size: never read
  p.notify_flags = 0xFFFFFFFFu;      // past struct_size: never read

  testing::internal::CaptureStderr();
  ASSERT_EQ(PS_OK, ps_notify_worker_ex(socket_path_.c_str(), &p));
  ASSERT_EQ(PS_OK, ps_notify_worker_ex(socket_path_.c_str(), &p));
  const std::string err = testing::internal::GetCapturedStderr();

  const std::string warning = "notification caller predates site identity";
  size_t count = 0;
  for (size_t at = err.find(warning); at != std::string::npos;
       at = err.find(warning, at + 1)) {
    ++count;
  }
  EXPECT_EQ(1u, count) << err;

  const auto frames = listener_->Frames();
  ASSERT_EQ(2u, frames.size());
  for (const auto& f : frames) {
    EXPECT_TRUE(f.site.empty());
    EXPECT_FALSE(f.host_vouched);
    EXPECT_FALSE(f.from_module);
    EXPECT_TRUE(f.legacy_ex_caller);
  }
}

#endif  // _WIN32

}  // namespace
