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

#include <memory>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
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
    admin_site_ = std::make_unique<AdminSite>(
        timer(), thread_system_.get(), message_handler(), nullptr /* fetcher */,
        "/tmp/admin_site_test");
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

}  // namespace

}  // namespace net_instaweb
