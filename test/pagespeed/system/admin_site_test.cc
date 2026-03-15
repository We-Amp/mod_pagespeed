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

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"
#include "test/net/instaweb/rewriter/custom_rewrite_test_base.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

class SystemServerContextNoProxyHtml : public SystemServerContext {
 public:
  explicit SystemServerContextNoProxyHtml(RewriteDriverFactory* factory)
      : SystemServerContext(factory, "fake_hostname", 80 /* fake port */) {}

  virtual bool ProxiesHtml() const { return false; }

 private:
  DISALLOW_COPY_AND_ASSIGN(SystemServerContextNoProxyHtml);
};

class AdminSiteTest : public CustomRewriteTestBase<SystemRewriteOptions> {
 protected:
  AdminSiteTest()
      : thread_system_(Platform::CreateThreadSystem()),
        options_(new SystemRewriteOptions(thread_system_.get())) {}

  virtual void SetUp() {
    CustomRewriteTestBase<SystemRewriteOptions>::SetUp();
    server_context_.reset(SetupServerContext(options_.release()));
    admin_site_.reset(new AdminSite(timer(), thread_system_.get(),
                                    message_handler(), nullptr /* fetcher */,
                                    "/tmp/admin_site_test"));
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

}  // namespace

}  // namespace net_instaweb
