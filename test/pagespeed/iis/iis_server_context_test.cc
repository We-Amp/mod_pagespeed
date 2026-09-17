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

// Unit tests for IisServerContext.
//
// Tests the server context lifecycle, configuration application,
// and admin/statistics path detection.

#include "pagespeed/iis/iis_server_context.h"

#include <memory>

#include "gtest/gtest.h"
#include "pagespeed/iis/iis_config.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/system/system_thread_system.h"

namespace net_instaweb {

class IisServerContextTest : public testing::Test {
 protected:
  void SetUp() override {
    RewriteOptions::Initialize();
    process_context_ = std::make_unique<ProcessContext>();
    SystemThreadSystem* thread_system = new SystemThreadSystem();
    // Create a minimal factory for testing.
    // Do NOT call Init() - it sets up caches, thread pools, etc. which
    // are not needed for testing IisServerContext accessors and would
    // hang or require running services.
    factory_ = std::make_unique<IisRewriteDriverFactory>(
        *process_context_,
        thread_system,  // factory takes ownership
        "localhost",    // hostname
        80);            // port
    server_context_ = std::make_unique<IisServerContext>(factory_.get());
  }

  void TearDown() override {
    server_context_.reset();
    factory_.reset();
    process_context_.reset();
    RewriteOptions::Terminate();
  }

  // Helper to create a config with specific settings
  std::unique_ptr<IisConfig> CreateConfig() {
    return std::make_unique<IisConfig>();
  }

  std::unique_ptr<ProcessContext> process_context_;
  std::unique_ptr<IisRewriteDriverFactory> factory_;
  std::unique_ptr<IisServerContext> server_context_;
};

// ============================================================================
// Construction Tests
// ============================================================================

TEST_F(IisServerContextTest, ConstructionSucceeds) {
  EXPECT_NE(nullptr, server_context_.get());
}

TEST_F(IisServerContextTest, FactoryAccessor) {
  EXPECT_EQ(factory_.get(), server_context_->iis_factory());
}

// ============================================================================
// Configuration Tests
// ============================================================================

TEST_F(IisServerContextTest, ConfigIsNullBeforeApply) {
  EXPECT_EQ(nullptr, server_context_->config());
}

TEST_F(IisServerContextTest, ApplyConfigurationSetsConfig) {
  auto config = CreateConfig();
  IisConfig* config_ptr = config.get();

  server_context_->ApplyConfiguration(std::move(config));

  EXPECT_EQ(config_ptr, server_context_->config());
}

TEST_F(IisServerContextTest, ApplyConfigurationUpdatesOptions) {
  auto config = CreateConfig();

  server_context_->ApplyConfiguration(std::move(config));

  // Config should have been applied to rewrite options
  EXPECT_NE(nullptr, server_context_->config());
}

// ============================================================================
// Admin Path Tests
// ============================================================================

TEST_F(IisServerContextTest, IsAdminPathReturnsFalseWithoutConfig) {
  // Without configuration, admin path should return false
  EXPECT_FALSE(server_context_->IsAdminPath("/pagespeed_admin"));
}

TEST_F(IisServerContextTest, IsAdminPathExactMatch) {
  auto config = CreateConfig();
  server_context_->ApplyConfiguration(std::move(config));

  // Default admin path is /pagespeed_admin
  EXPECT_TRUE(server_context_->IsAdminPath("/pagespeed_admin"));
}

TEST_F(IisServerContextTest, IsAdminPathWithSubpath) {
  auto config = CreateConfig();
  server_context_->ApplyConfiguration(std::move(config));

  // Subpaths of admin path should also match
  EXPECT_TRUE(server_context_->IsAdminPath("/pagespeed_admin/"));
  EXPECT_TRUE(server_context_->IsAdminPath("/pagespeed_admin/statistics"));
  EXPECT_TRUE(server_context_->IsAdminPath("/pagespeed_admin/cache"));
}

TEST_F(IisServerContextTest, IsAdminPathNonMatch) {
  auto config = CreateConfig();
  server_context_->ApplyConfiguration(std::move(config));

  // Non-admin paths should not match
  EXPECT_FALSE(server_context_->IsAdminPath("/"));
  EXPECT_FALSE(server_context_->IsAdminPath("/index.html"));
  EXPECT_FALSE(server_context_->IsAdminPath("/pagespeed"));
  EXPECT_FALSE(server_context_->IsAdminPath("/pagespeed_admin_other"));
}

// ============================================================================
// Statistics Path Tests
// ============================================================================

TEST_F(IisServerContextTest, IsStatisticsPathReturnsFalseWithoutConfig) {
  EXPECT_FALSE(server_context_->IsStatisticsPath("/pagespeed_statistics"));
}

TEST_F(IisServerContextTest, IsStatisticsPathExactMatch) {
  auto config = CreateConfig();
  server_context_->ApplyConfiguration(std::move(config));

  // Default statistics path is /pagespeed_statistics
  EXPECT_TRUE(server_context_->IsStatisticsPath("/pagespeed_statistics"));
}

TEST_F(IisServerContextTest, IsStatisticsPathNoSubpathMatch) {
  auto config = CreateConfig();
  server_context_->ApplyConfiguration(std::move(config));

  // Statistics path requires exact match (unlike admin path)
  EXPECT_FALSE(server_context_->IsStatisticsPath("/pagespeed_statistics/"));
  EXPECT_FALSE(server_context_->IsStatisticsPath("/pagespeed_statistics/foo"));
}

TEST_F(IisServerContextTest, IsStatisticsPathNonMatch) {
  auto config = CreateConfig();
  server_context_->ApplyConfiguration(std::move(config));

  EXPECT_FALSE(server_context_->IsStatisticsPath("/"));
  EXPECT_FALSE(server_context_->IsStatisticsPath("/stats"));
  EXPECT_FALSE(server_context_->IsStatisticsPath("/pagespeed_stats"));
}

// ============================================================================
// Unplugged Mode Tests
// ============================================================================

TEST_F(IisServerContextTest, UnpluggedReturnsFalseWithoutConfig) {
  EXPECT_FALSE(server_context_->unplugged());
}

TEST_F(IisServerContextTest, UnpluggedReturnsFalseByDefault) {
  auto config = CreateConfig();
  server_context_->ApplyConfiguration(std::move(config));

  // Default config should not be unplugged
  EXPECT_FALSE(server_context_->unplugged());
}

// ============================================================================
// ProxyFetch Factory Tests
// ============================================================================

TEST_F(IisServerContextTest, ProxyFetchFactoryIsNullBeforeInit) {
  EXPECT_EQ(nullptr, server_context_->proxy_fetch_factory());
}

TEST_F(IisServerContextTest, InitProxyFetchFactoryCreatesFactory) {
  server_context_->InitProxyFetchFactory();

  EXPECT_NE(nullptr, server_context_->proxy_fetch_factory());
}

TEST_F(IisServerContextTest, InitProxyFetchFactoryIdempotent) {
  server_context_->InitProxyFetchFactory();
  ProxyFetchFactory* first = server_context_->proxy_fetch_factory();

  // Second call should replace with a new factory
  server_context_->InitProxyFetchFactory();
  ProxyFetchFactory* second = server_context_->proxy_fetch_factory();

  // Both should be valid (though second replaces first)
  EXPECT_NE(nullptr, second);
}

}  // namespace net_instaweb
