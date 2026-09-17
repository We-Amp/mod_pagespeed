/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Unit tests for AdminSite.

#include "pagespeed/system/admin_site.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/query_params.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/system/daemon_reader.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"
#include "test/net/instaweb/rewriter/custom_rewrite_test_base.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace net_instaweb {

namespace {

class SystemServerContextNoProxyHtml : public SystemServerContext {
 public:
  explicit SystemServerContextNoProxyHtml(RewriteDriverFactory* factory)
      : SystemServerContext(factory, "fake_hostname", 80 /* fake port */) {}

  virtual bool ProxiesHtml() const { return false; }

 private:
  SystemServerContextNoProxyHtml(const SystemServerContextNoProxyHtml&) =
      delete;
  SystemServerContextNoProxyHtml& operator=(
      const SystemServerContextNoProxyHtml&) = delete;
};

class AdminSiteTest : public CustomRewriteTestBase<SystemRewriteOptions> {
 protected:
  AdminSiteTest()
      : thread_system_(Platform::CreateThreadSystem()),
        options_(new SystemRewriteOptions(thread_system_.get())) {}

  virtual void SetUp() {
    CustomRewriteTestBase<SystemRewriteOptions>::SetUp();
    server_context_.reset(SetupServerContext(options_.release()));
    admin_site_ = std::make_unique<AdminSite>(timer(), message_handler(),
                                              nullptr /* daemon_reader */);
  }

  virtual void TearDown() { RewriteTestBase::TearDown(); }

  ServerContext* SetupServerContext(SystemRewriteOptions* config) {
    std::unique_ptr<SystemServerContext> server_context(
        new SystemServerContextNoProxyHtml(factory()));
    server_context->reset_global_options(config);
    server_context->set_statistics(factory()->statistics());
    return server_context.release();
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<ServerContext> server_context_;
  std::unique_ptr<SystemRewriteOptions> options_;
  std::unique_ptr<AdminSite> admin_site_;
};

TEST_F(AdminSiteTest, MessageHistoryReturnsJson) {
  EXPECT_EQ(message_handler(), admin_site_->MessageHandlerForTesting());
  message_handler()->Message(kInfo, "Ignore the first line.");
  message_handler()->Message(kError, "Test for %s", "Errors");
  message_handler()->Message(kWarning, "Test for %s", "Warnings");
  message_handler()->Message(kInfo, "Test for %s", "Infos");
  GoogleString buffer;
  StringAsyncFetch fetch(rewrite_driver()->request_context(), &buffer);
  admin_site_->MessageHistoryHandler(*(rewrite_driver()->options()),
                                     AdminSite::kPageSpeedAdmin, &fetch);
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"severity\":\"error\""));
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"severity\":\"warning\""));
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"severity\":\"info\""));
  EXPECT_THAT(buffer, ::testing::HasSubstr("\"messages\":["));
  EXPECT_THAT(buffer, ::testing::HasSubstr("Test for Errors"));
  EXPECT_THAT(buffer, ::testing::HasSubstr("Test for Warnings"));
  EXPECT_THAT(buffer, ::testing::HasSubstr("Test for Infos"));
}

// =============================================================================
// Defense-in-depth admin-exposure warning tests.
// =============================================================================

// IsLoopbackClientIp predicate — no MessageHandler, no atomic state.
class IsLoopbackClientIpTest : public ::testing::Test {};

TEST_F(IsLoopbackClientIpTest, AcceptsIpv4Loopback) {
  EXPECT_TRUE(IsLoopbackClientIp("127.0.0.1"));
  EXPECT_TRUE(IsLoopbackClientIp("127.0.0.255"));
  EXPECT_TRUE(IsLoopbackClientIp("127.1.2.3"));
  EXPECT_TRUE(IsLoopbackClientIp("127.255.255.255"));
}

TEST_F(IsLoopbackClientIpTest, AcceptsIpv6Loopback) {
  EXPECT_TRUE(IsLoopbackClientIp("::1"));
}

TEST_F(IsLoopbackClientIpTest, AcceptsIpv4MappedIpv6Loopback) {
  // ::ffff:127.0.0.1 form (IPv4-mapped).
  EXPECT_TRUE(IsLoopbackClientIp("::ffff:127.0.0.1"));
  EXPECT_TRUE(IsLoopbackClientIp("::FFFF:127.0.0.1"));
  // ::127.0.0.1 form (IPv4-compatible, deprecated but seen in some stacks).
  EXPECT_TRUE(IsLoopbackClientIp("::127.0.0.1"));
}

TEST_F(IsLoopbackClientIpTest, AcceptsLocalhostLiteral) {
  EXPECT_TRUE(IsLoopbackClientIp("localhost"));
}

TEST_F(IsLoopbackClientIpTest, RejectsNonLoopbackIpv4) {
  EXPECT_FALSE(IsLoopbackClientIp("10.0.0.1"));
  EXPECT_FALSE(IsLoopbackClientIp("192.168.1.1"));
  EXPECT_FALSE(IsLoopbackClientIp("8.8.8.8"));
  // Spoofy near-loopback addresses must NOT be accepted.
  EXPECT_FALSE(IsLoopbackClientIp("128.0.0.1"));
  EXPECT_FALSE(IsLoopbackClientIp("1.2.7.0.0.1"));
}

TEST_F(IsLoopbackClientIpTest, RejectsNonLoopbackIpv6) {
  EXPECT_FALSE(IsLoopbackClientIp("::2"));
  EXPECT_FALSE(IsLoopbackClientIp("2001:db8::1"));
  EXPECT_FALSE(IsLoopbackClientIp("fe80::1"));
  // ::ffff:128.x.y.z is NOT loopback.
  EXPECT_FALSE(IsLoopbackClientIp("::ffff:8.8.8.8"));
}

TEST_F(IsLoopbackClientIpTest, RejectsEmpty) {
  // Caller couldn't determine the IP — treat as non-loopback so the warning
  // fires (loud failure mode is the safer default for a security signal).
  EXPECT_FALSE(IsLoopbackClientIp(""));
}

TEST_F(IsLoopbackClientIpTest, RejectsGarbage) {
  EXPECT_FALSE(IsLoopbackClientIp("not-an-ip"));
  EXPECT_FALSE(IsLoopbackClientIp("127.0.0.1 ; rm -rf /"));
}

// WarnIfNonLoopbackAdminAccess — uses MessageHandler + per-family atomic.
class AdminExposureWarningTest : public ::testing::Test {
 protected:
  void SetUp() override {
    thread_system_.reset(Platform::CreateThreadSystem());
    handler_ = std::make_unique<MockMessageHandler>(new NullMutex);
    ResetAdminExposureWarningsForTesting();
  }

  void TearDown() override { ResetAdminExposureWarningsForTesting(); }

  int WarningCount() const { return handler_->MessagesOfType(kWarning); }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<MockMessageHandler> handler_;
};

TEST_F(AdminExposureWarningTest, FiresForNonLoopbackClient) {
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(1, WarningCount());
}

TEST_F(AdminExposureWarningTest, SilentForLoopback) {
  WarnIfNonLoopbackAdminAccess("127.0.0.1", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("::1", AdminHandlerFamily::kStatistics,
                               "mod_pagespeed_statistics", handler_.get());
  WarnIfNonLoopbackAdminAccess("localhost", AdminHandlerFamily::kConsole,
                               "pagespeed_console", handler_.get());
  WarnIfNonLoopbackAdminAccess("::ffff:127.0.0.1",
                               AdminHandlerFamily::kMessages,
                               "mod_pagespeed_message", handler_.get());
  EXPECT_EQ(0, WarningCount());
}

TEST_F(AdminExposureWarningTest, IdempotentPerFamily) {
  // Three calls to the same family from non-loopback IPs → exactly one
  // warning.
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("10.0.0.7", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("8.8.8.8", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(1, WarningCount());
}

TEST_F(AdminExposureWarningTest, IndependentAcrossFamilies) {
  // Distinct families get their own one-shot.
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kGlobalAdmin,
                               "pagespeed_global_admin", handler_.get());
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kStatistics,
                               "mod_pagespeed_statistics", handler_.get());
  WarnIfNonLoopbackAdminAccess(
      "192.168.1.5", AdminHandlerFamily::kGlobalStatistics,
      "mod_pagespeed_global_statistics", handler_.get());
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kConsole,
                               "pagespeed_console", handler_.get());
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kMessages,
                               "mod_pagespeed_message", handler_.get());
  EXPECT_EQ(6, WarningCount());
}

TEST_F(AdminExposureWarningTest, ResetReArmsAllFamilies) {
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(1, WarningCount());
  ResetAdminExposureWarningsForTesting();
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(2, WarningCount());
}

TEST_F(AdminExposureWarningTest, EmptyIpStillFires) {
  // Empty client IP = caller couldn't determine. Treat as non-loopback so the
  // operator at least sees that the handler ran.
  WarnIfNonLoopbackAdminAccess("", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(1, WarningCount());
}

TEST_F(AdminExposureWarningTest, WarningMentionsHandlerAndIp) {
  WarnIfNonLoopbackAdminAccess("10.20.30.40", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  GoogleString dump;
  StringWriter writer(&dump);
  handler_->Dump(&writer);
  EXPECT_THAT(dump, ::testing::HasSubstr("pagespeed_admin"));
  EXPECT_THAT(dump, ::testing::HasSubstr("10.20.30.40"));
  EXPECT_THAT(dump, ::testing::HasSubstr("loopback"));
}

TEST_F(AdminExposureWarningTest, NullHandlerIsSafe) {
  // Don't crash if the caller can't supply a handler (unusual but harmless).
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", nullptr);
  // But: the atomic still flips, so a subsequent call with a real handler
  // is suppressed. This is fine — the warning is best-effort.
  WarnIfNonLoopbackAdminAccess("192.168.1.5", AdminHandlerFamily::kAdmin,
                               "pagespeed_admin", handler_.get());
  EXPECT_EQ(0, WarningCount());
}

// =============================================================================
// PostInitHook: AdminSite construction and the absence of license state,
// and drop-in compatibility with releases that had it.
// =============================================================================

// Builds a serving (non-stub) SystemServerContext whose FileCachePath is
// |file_cache_path| and runs its PostInitHook, as every port does at startup.
std::unique_ptr<SystemServerContext> ServingContextOn(
    RewriteDriverFactory* factory, ThreadSystem* thread_system,
    MessageHandler* handler, const GoogleString& file_cache_path) {
  std::unique_ptr<SystemServerContext> sc(
      new SystemServerContextNoProxyHtml(factory));
  SystemRewriteOptions* opts = new SystemRewriteOptions(thread_system);
  opts->set_file_cache_path(file_cache_path);
  sc->reset_global_options(opts);
  sc->set_statistics(factory->statistics());
  sc->set_message_handler(handler);
  sc->PostInitHook();
  return sc;
}

// A decoding stub context must skip AdminSite setup in PostInitHook(): it
// runs on default options (no FileCachePath) and never serves requests.
TEST_F(AdminSiteTest, DecodingStubSkipsAdminSiteInit) {
  std::unique_ptr<SystemServerContext> stub(
      new SystemServerContextNoProxyHtml(factory()));
  stub->reset_global_options(new SystemRewriteOptions(thread_system_.get()));
  stub->set_statistics(factory()->statistics());
  stub->set_message_handler(message_handler());
  stub->set_is_decoding_stub(true);
  stub->PostInitHook();
  EXPECT_EQ(nullptr, stub->admin_site());
}

// There is no license state. A serving context with no license
// file anywhere builds its AdminSite and logs nothing about licensing -- the
// old startup UNLICENSED warning is gone.
TEST_F(AdminSiteTest, ServingContextLogsNoLicenseMessage) {
  ResetStaleLicenseFileNoticeForTesting();
  std::unique_ptr<SystemServerContext> sc =
      ServingContextOn(factory(), thread_system_.get(), message_handler(),
                       StrCat(GTestTempDir(), "/no-such-dir/cache"));
  EXPECT_NE(nullptr, sc->admin_site());
  GoogleString messages;
  StringWriter writer(&messages);
  message_handler()->Dump(&writer);
  EXPECT_THAT(messages, ::testing::Not(::testing::HasSubstr("UNLICENSED")));
  EXPECT_THAT(messages, ::testing::Not(::testing::HasSubstr("icense")));
}

// Drop-in compatibility: a pagespeed.license left behind by an
// earlier release -- next to FileCachePath, where those releases kept it -- is
// ignored: never read, never deleted, and mentioned exactly once per process
// at INFO so the operator knows it can go. Two serving contexts (Apache: one
// per vhost) share the one notice; a trailing separator on the cache path is
// stripped, as the old reader did, so the sibling file is still found.
TEST_F(AdminSiteTest, StaleLicenseFileNoticedOnceAtInfoAndLeftAlone) {
  ResetStaleLicenseFileNoticeForTesting();
  namespace fs = std::filesystem;
  const fs::path root = fs::path(GTestTempDir()) / "stale-license-notice";
  fs::remove_all(root);
  const fs::path cache_dir = root / "cache";
  ASSERT_TRUE(fs::create_directories(cache_dir));
  const fs::path license_file = root / "pagespeed.license";
  const char kStaleToken[] = "stale-token-from-a-previous-release";
  {
    std::ofstream out(license_file);
    out << kStaleToken;
  }
  ASSERT_TRUE(fs::exists(license_file));

  const GoogleString cache_path = StrCat(cache_dir.string(), "/");
  std::unique_ptr<SystemServerContext> first = ServingContextOn(
      factory(), thread_system_.get(), message_handler(), cache_path);
  std::unique_ptr<SystemServerContext> second = ServingContextOn(
      factory(), thread_system_.get(), message_handler(), cache_path);
  EXPECT_NE(nullptr, first->admin_site());
  EXPECT_NE(nullptr, second->admin_site());

  GoogleString messages;
  StringWriter writer(&messages);
  message_handler()->Dump(&writer);
  const GoogleString notice =
      StrCat(license_file.string(),
             ": this file is no longer read since 2.1 and can be removed");
  EXPECT_EQ(1, CountSubstring(messages, notice)) << messages;
  // INFO, not a warning: MockMessageHandler prefixes each line with its type.
  EXPECT_THAT(messages,
              ::testing::HasSubstr(StrCat("[Info] [00000] ", notice)));
  EXPECT_THAT(messages, ::testing::Not(::testing::HasSubstr("UNLICENSED")));

  // Left alone: still there, byte-identical.
  ASSERT_TRUE(fs::exists(license_file));
  std::ifstream in(license_file);
  const std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(kStaleToken, contents);
  fs::remove_all(root);
}

// With no stale file next to the cache directory nothing is logged, and the
// process-wide latch stays unset for a later context that does have one.
TEST_F(AdminSiteTest, NoStaleLicenseFileMeansNoNotice) {
  ResetStaleLicenseFileNoticeForTesting();
  namespace fs = std::filesystem;
  const fs::path root = fs::path(GTestTempDir()) / "no-stale-license";
  fs::remove_all(root);
  ASSERT_TRUE(fs::create_directories(root / "cache"));
  std::unique_ptr<SystemServerContext> sc =
      ServingContextOn(factory(), thread_system_.get(), message_handler(),
                       (root / "cache").string());
  EXPECT_NE(nullptr, sc->admin_site());
  GoogleString messages;
  StringWriter writer(&messages);
  message_handler()->Dump(&writer);
  EXPECT_THAT(
      messages,
      ::testing::Not(::testing::HasSubstr("is no longer read since 2.1")));
  fs::remove_all(root);
}

// The factory marks the context it builds through
// InitStubDecodingServerContext() as a decoding stub.
TEST_F(AdminSiteTest, FactoryDecodingContextIsMarkedAsStub) {
  std::unique_ptr<ServerContext> stub(factory()->NewDecodingServerContext());
  EXPECT_TRUE(stub->is_decoding_stub());
}

// =============================================================================
// /v1/daemon/* read-only proxy tests.
// =============================================================================

// Test double for the DaemonReader seam: completes synchronously with a
// canned response, records the requested daemon path, and can hold one
// request in flight (to exercise the per-endpoint slot).
class FakeDaemonReader : public DaemonReader {
 public:
  explicit FakeDaemonReader(MessageHandler* handler) : handler_(handler) {}

  void Get(StringPiece daemon_path, AsyncFetch* fetch) override {
    ++call_count_;
    daemon_path.CopyToString(&last_daemon_path_);
    if (daemon_path == hold_path_) {
      held_fetch_ = fetch;
      return;
    }
    Complete(fetch);
  }

  void Complete(AsyncFetch* fetch) {
    if (fail_) {
      fetch->Done(false);
      return;
    }
    fetch->response_headers()->set_status_code(status_);
    fetch->response_headers()->Add(HttpAttributes::kContentType, content_type_);
    // An upstream-only header that must never leak to the client response.
    fetch->response_headers()->Add("X-Upstream-Only", "secret");
    fetch->Write(body_, handler_);
    fetch->Done(true);
  }

  MessageHandler* handler_;
  int call_count_ = 0;
  GoogleString last_daemon_path_;
  int status_ = HttpStatus::kOK;
  GoogleString content_type_ = "application/json";
  GoogleString body_ = "{\"status\":\"ok\"}";
  bool fail_ = false;
  // Requests for this daemon path are held in flight until Complete() is
  // called on held_fetch_.
  GoogleString hold_path_;
  AsyncFetch* held_fetch_ = nullptr;
};

class AdminSiteDaemonTest : public AdminSiteTest {
 protected:
  void SetUp() override {
    AdminSiteTest::SetUp();
    // daemon_site_ takes ownership of the reader.
    fake_reader_ = new FakeDaemonReader(message_handler());
    daemon_site_ =
        std::make_unique<AdminSite>(timer(), message_handler(), fake_reader_);
  }

  // Issues a request for `url` against `site`'s AdminPage and returns the
  // client fetch.  `url` includes the admin path prefix and may carry a
  // query string.
  std::unique_ptr<StringAsyncFetch> FetchAdminPage(
      AdminSite* site, const GoogleString& url, RequestHeaders::Method method,
      GoogleString* buffer) {
    std::unique_ptr<StringAsyncFetch> fetch(
        new StringAsyncFetch(rewrite_driver()->request_context(), buffer));
    fetch->request_headers()->set_method(method);
    GoogleUrl gurl(url);
    QueryParams query_params;
    SystemServerContext* system_server_context =
        static_cast<SystemServerContext*>(server_context_.get());
    site->AdminPage(true /* is_global */, gurl, query_params,
                    rewrite_driver()->options(), nullptr /* cache_path */,
                    fetch.get(), nullptr /* system_caches */,
                    nullptr /* filesystem_metadata_cache */,
                    nullptr /* http_cache */, nullptr /* metadata_cache */,
                    nullptr /* page_property_cache */, system_server_context,
                    factory()->statistics(), factory()->statistics(),
                    system_server_context->global_system_rewrite_options(),
                    "" /* request_body */);
    return fetch;
  }

  FakeDaemonReader* fake_reader_ = nullptr;  // owned by daemon_site_
  std::unique_ptr<AdminSite> daemon_site_;
};

TEST_F(AdminSiteDaemonTest, HappyPathForwardsStatusContentTypeAndBody) {
  GoogleString buffer;
  auto fetch =
      FetchAdminPage(daemon_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/health",
                     RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(1, fake_reader_->call_count_);
  EXPECT_EQ("/v1/health", fake_reader_->last_daemon_path_);
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("{\"status\":\"ok\"}", buffer);
  const char* content_type =
      fetch->response_headers()->Lookup1(HttpAttributes::kContentType);
  ASSERT_TRUE(content_type != nullptr);
  EXPECT_STREQ("application/json", content_type);
  // Cache-Control: no-store on every daemon response.
  const char* cache_control =
      fetch->response_headers()->Lookup1(HttpAttributes::kCacheControl);
  ASSERT_TRUE(cache_control != nullptr);
  EXPECT_STREQ("no-store", cache_control);
  // No upstream response headers are forwarded.
  EXPECT_FALSE(fetch->response_headers()->Has("X-Upstream-Only"));
}

TEST_F(AdminSiteDaemonTest, EndpointTableMapping) {
  struct {
    const char* url_suffix;
    const char* upstream_path;
  } cases[] = {
      {"health", "/v1/health"},
      {"stats", "/v1/stats"},
      {"cooldowns", "/v1/cache/cooldowns"},
  };
  for (const auto& c : cases) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_admin/v1/daemon/", c.url_suffix),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << c.url_suffix;
    EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
    EXPECT_EQ(c.upstream_path, fake_reader_->last_daemon_path_);
  }
}

TEST_F(AdminSiteDaemonTest, UnknownLeafIs404AndNotProxied) {
  // "config" is a real module admin leaf; routing through the daemon branch
  // must not fall through to it.
  for (const char* suffix : {"config", "stats/extra", "", "HEALTH"}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        StrCat("http://localhost/pagespeed_admin/v1/daemon/", suffix),
        RequestHeaders::kGet, &buffer);
    ASSERT_TRUE(fetch->done()) << suffix;
    EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code())
        << suffix;
    EXPECT_THAT(buffer, ::testing::HasSubstr("Unknown daemon endpoint"));
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, NonGetMethodsAre405) {
  for (RequestHeaders::Method method :
       {RequestHeaders::kPost, RequestHeaders::kPut, RequestHeaders::kDelete,
        RequestHeaders::kPatch}) {
    GoogleString buffer;
    auto fetch = FetchAdminPage(
        daemon_site_.get(),
        "http://localhost/pagespeed_admin/v1/daemon/health", method, &buffer);
    ASSERT_TRUE(fetch->done());
    EXPECT_EQ(HttpStatus::kMethodNotAllowed,
              fetch->response_headers()->status_code());
  }
  EXPECT_EQ(0, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, HeadRequestSendsNoBody) {
  GoogleString buffer;
  auto fetch =
      FetchAdminPage(daemon_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/health",
                     RequestHeaders::kHead, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(1, fake_reader_->call_count_);
}

TEST_F(AdminSiteDaemonTest, QueryStringIsNotForwarded) {
  GoogleString buffer;
  auto fetch = FetchAdminPage(
      daemon_site_.get(),
      "http://localhost/pagespeed_admin/v1/daemon/cooldowns?hostname=evil",
      RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kOK, fetch->response_headers()->status_code());
  EXPECT_EQ("/v1/cache/cooldowns", fake_reader_->last_daemon_path_);
}

TEST_F(AdminSiteDaemonTest, NoReaderMeansDaemonUnreachable) {
  // The fixture's admin_site_ has a null DaemonReader (this port/context has
  // no daemon transport, or the socket path is empty).
  GoogleString buffer;
  auto fetch =
      FetchAdminPage(admin_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/health",
                     RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer, ::testing::HasSubstr("daemon unreachable"));
  const char* cache_control =
      fetch->response_headers()->Lookup1(HttpAttributes::kCacheControl);
  ASSERT_TRUE(cache_control != nullptr);
  EXPECT_STREQ("no-store", cache_control);
}

TEST_F(AdminSiteDaemonTest, UpstreamFailureIs502) {
  fake_reader_->fail_ = true;
  GoogleString buffer;
  auto fetch =
      FetchAdminPage(daemon_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/stats",
                     RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(HttpStatus::kBadGateway, fetch->response_headers()->status_code());
  EXPECT_THAT(buffer, ::testing::HasSubstr("daemon unreachable"));
}

TEST_F(AdminSiteDaemonTest, UpstreamStatusAndBodyPassThroughContentTypePinned) {
  // Status and body pass through, but the Content-Type is pinned to
  // application/json: the socket peer is untrusted, and every daemon endpoint
  // serves JSON.  A spoofed text/html upstream must not render in the admin
  // origin.
  fake_reader_->status_ = 503;
  fake_reader_->content_type_ = "text/html";
  fake_reader_->body_ = "<script>alert(1)</script>";
  GoogleString buffer;
  auto fetch =
      FetchAdminPage(daemon_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/stats",
                     RequestHeaders::kGet, &buffer);
  ASSERT_TRUE(fetch->done());
  EXPECT_EQ(503, fetch->response_headers()->status_code());
  EXPECT_EQ("<script>alert(1)</script>", buffer);
  const char* content_type =
      fetch->response_headers()->Lookup1(HttpAttributes::kContentType);
  ASSERT_TRUE(content_type != nullptr);
  EXPECT_STREQ("application/json", content_type);
}

TEST_F(AdminSiteDaemonTest, ConcurrentRequestOnSameEndpointGets429) {
  fake_reader_->hold_path_ = "/v1/health";
  GoogleString buffer1, buffer2;
  auto first =
      FetchAdminPage(daemon_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/health",
                     RequestHeaders::kGet, &buffer1);
  ASSERT_FALSE(first->done());  // held upstream
  auto second =
      FetchAdminPage(daemon_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/health",
                     RequestHeaders::kGet, &buffer2);
  ASSERT_TRUE(second->done());
  EXPECT_EQ(429, second->response_headers()->status_code());
  // Release the held upstream read; the first request completes.
  ASSERT_TRUE(fake_reader_->held_fetch_ != nullptr);
  fake_reader_->Complete(fake_reader_->held_fetch_);
  ASSERT_TRUE(first->done());
  EXPECT_EQ(HttpStatus::kOK, first->response_headers()->status_code());
  EXPECT_EQ("{\"status\":\"ok\"}", buffer1);
}

TEST_F(AdminSiteDaemonTest, PerEndpointSlotsDoNotBlockEachOther) {
  fake_reader_->hold_path_ = "/v1/health";
  GoogleString buffer1, buffer2;
  auto first =
      FetchAdminPage(daemon_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/health",
                     RequestHeaders::kGet, &buffer1);
  ASSERT_FALSE(first->done());  // held upstream
  // A different endpoint has its own slot and answers immediately.
  auto second =
      FetchAdminPage(daemon_site_.get(),
                     "http://localhost/pagespeed_admin/v1/daemon/stats",
                     RequestHeaders::kGet, &buffer2);
  ASSERT_TRUE(second->done());
  EXPECT_EQ(HttpStatus::kOK, second->response_headers()->status_code());
  ASSERT_TRUE(fake_reader_->held_fetch_ != nullptr);
  fake_reader_->Complete(fake_reader_->held_fetch_);
  ASSERT_TRUE(first->done());
}

// The /v1/license/* admin API is gone. Its former routes fall
// through to leaf dispatch and answer 404 JSON -- never a config dump, never a
// 5xx -- and a POST no longer meets a CSRF gate (403). The daemon proxy next
// to it is untouched.
TEST_F(AdminSiteDaemonTest, RetiredLicenseRoutesAnswer404) {
  for (const char* leaf : {"status", "apply", "activate", "consent"}) {
    for (RequestHeaders::Method method :
         {RequestHeaders::kGet, RequestHeaders::kPost}) {
      GoogleString buffer;
      auto fetch = FetchAdminPage(
          daemon_site_.get(),
          StrCat("http://localhost/pagespeed_global_admin/v1/license/", leaf),
          method, &buffer);
      ASSERT_TRUE(fetch->done()) << leaf;
      EXPECT_EQ(HttpStatus::kNotFound, fetch->response_headers()->status_code())
          << leaf << " method=" << static_cast<int>(method);
      EXPECT_THAT(buffer, ::testing::HasSubstr("Unknown admin page")) << leaf;
      EXPECT_THAT(buffer, ::testing::Not(::testing::HasSubstr("CSRF"))) << leaf;
      EXPECT_EQ(0, fake_reader_->call_count_) << leaf;
    }
  }
}

}  // namespace

}  // namespace net_instaweb
