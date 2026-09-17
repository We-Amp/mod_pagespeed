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

// Unit tests for EnvoyServerContext.
// These tests verify the server context initialization, options caching,
// VHost configuration, and request context creation.
//
// Note: These tests focus on the EnvoyServerContext-specific functionality
// without requiring full factory lifecycle (which requires additional setup).

#include "pagespeed/envoy/envoy_server_context.h"

#include <memory>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/envoy/envoy_vhost_config.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/thread/pthread_thread_system.h"
#include "pagespeed/system/system_request_context.h"
#include "pagespeed/system/system_thread_system.h"

namespace net_instaweb {

namespace {

// Test fixture that sets up the necessary infrastructure for EnvoyServerContext
// testing. We use the real EnvoyRewriteDriverFactory with full initialization.
class EnvoyServerContextTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // Initialize options system once for all tests.
    EnvoyRewriteOptions::Initialize();
  }

  static void TearDownTestSuite() {
    EnvoyRewriteOptions::Terminate();
  }

  void SetUp() override {
    thread_system_ = new SystemThreadSystem();
    factory_ = std::make_unique<EnvoyRewriteDriverFactory>(
        ProcessContext(), thread_system_, "test.example.com", 8080);
    // Call Init() to complete factory initialization (creates caches_, etc.).
    factory_->Init();
    // Create a server context for testing.
    server_context_ = factory_->MakeEnvoyServerContext("test.example.com", 8080);
  }

  void TearDown() override {
    // Server context is owned by the factory via uninitialized_server_contexts_.
    server_context_ = nullptr;
    factory_->ShutDown();
    factory_.reset();
    // thread_system_ is owned by the factory.
  }

  SystemThreadSystem* thread_system_;
  std::unique_ptr<EnvoyRewriteDriverFactory> factory_;
  EnvoyServerContext* server_context_;
};

// Test basic server context creation.
TEST_F(EnvoyServerContextTest, BasicConstruction) {
  ASSERT_NE(nullptr, server_context_);
  // Verify the factory pointer is correctly stored.
  EXPECT_EQ(factory_.get(), server_context_->envoy_rewrite_driver_factory());
}

// Test that config() returns EnvoyRewriteOptions.
TEST_F(EnvoyServerContextTest, ConfigReturnsEnvoyRewriteOptions) {
  EnvoyRewriteOptions* config = server_context_->config();
  ASSERT_NE(nullptr, config);

  // Verify it's the correct type by checking Envoy-specific options exist.
  EXPECT_TRUE(config->enable_html_rewriting());
  EXPECT_EQ(2000, config->html_rewrite_deadline_ms());
  EXPECT_EQ(2 * 1024 * 1024, config->max_html_buffer_bytes());
}

// Test that ProxiesHtml returns false for EnvoyServerContext.
TEST_F(EnvoyServerContextTest, DoesNotProxyHtml) {
  EXPECT_FALSE(server_context_->ProxiesHtml());
}

// Test FormatOption formatting.
TEST_F(EnvoyServerContextTest, FormatOption) {
  GoogleString formatted = server_context_->FormatOption("EnableFilters", "rewrite_images");
  EXPECT_EQ("pagespeed EnableFilters rewrite_images;", formatted);

  // Test with empty args.
  formatted = server_context_->FormatOption("TestOption", "");
  EXPECT_EQ("pagespeed TestOption ;", formatted);

  // Test with multiple args.
  formatted = server_context_->FormatOption("RewriteLevel", "CoreFilters");
  EXPECT_EQ("pagespeed RewriteLevel CoreFilters;", formatted);
}

// Test VHost config manager access.
TEST_F(EnvoyServerContextTest, VHostConfigManagerAccess) {
  EnvoyVHostConfigManager* manager = server_context_->vhost_config_manager();
  ASSERT_NE(nullptr, manager);
  // Initially should have no VHosts.
  EXPECT_FALSE(manager->HasVHosts());
  EXPECT_EQ(0u, manager->size());
}

// Test FindOptionsForHost with no VHosts configured.
TEST_F(EnvoyServerContextTest, FindOptionsForHostNoVHosts) {
  const EnvoyRewriteOptions* opts =
      server_context_->FindOptionsForHost("any.example.com");
  EXPECT_EQ(nullptr, opts);
}

// Test GetOptionsForHost with no VHosts (should return global options).
TEST_F(EnvoyServerContextTest, GetOptionsForHostNoVHostsReturnsGlobal) {
  const RewriteOptions* opts =
      server_context_->GetOptionsForHost("any.example.com");
  ASSERT_NE(nullptr, opts);
  // Should be the same as global_options.
  EXPECT_EQ(server_context_->global_options(), opts);
}

// Test request context creation with default parameters.
TEST_F(EnvoyServerContextTest, NewRequestContextDefault) {
  SystemRequestContext* ctx = server_context_->NewRequestContext();
  ASSERT_NE(nullptr, ctx);

  // Default should be localhost:80.
  EXPECT_EQ(80, ctx->local_port());
  EXPECT_EQ("localhost", ctx->local_ip());
  EXPECT_FALSE(ctx->using_http2());

  // Request context is ref-counted, drop our reference.
  ctx->Release();
}

// Test request context creation with explicit hostname and port.
TEST_F(EnvoyServerContextTest, NewRequestContextWithHostPort) {
  SystemRequestContext* ctx =
      server_context_->NewRequestContext("myhost.example.com", 9090);
  ASSERT_NE(nullptr, ctx);

  EXPECT_EQ(9090, ctx->local_port());
  EXPECT_EQ("myhost.example.com", ctx->local_ip());
  EXPECT_FALSE(ctx->using_http2());

  ctx->Release();
}

// Test request context creation with port in hostname.
TEST_F(EnvoyServerContextTest, NewRequestContextHostnameWithPort) {
  // When hostname contains a port (e.g., from Host header), it should be parsed.
  SystemRequestContext* ctx =
      server_context_->NewRequestContext("myhost.example.com:8888", 9090);
  ASSERT_NE(nullptr, ctx);

  // Port from hostname should override the explicit port.
  EXPECT_EQ(8888, ctx->local_port());
  EXPECT_EQ("myhost.example.com", ctx->local_ip());

  ctx->Release();
}

// Test message handler access.
// Note: In unit test context, the server context's message_handler may not
// be set up the same way as in production. The factory's message_handler()
// should be accessible though.
TEST_F(EnvoyServerContextTest, MessageHandlerAccess) {
  // The factory's message handler should be accessible.
  EXPECT_NE(nullptr, factory_->message_handler());
}

// Test VHost configuration and options lookup.
class EnvoyServerContextVHostTest : public EnvoyServerContextTest {
 protected:
  void SetUp() override {
    EnvoyServerContextTest::SetUp();
    // Add some VHost configurations.
    AddVHost("api.example.com", 10);
    AddVHost("*.example.com", 20);
    AddVHost("cdn.example.org", 10);
    server_context_->vhost_config_manager()->SortByPriority();
  }

  void AddVHost(const char* pattern, int priority) {
    auto options = std::make_unique<EnvoyRewriteOptions>(thread_system_);
    // Set a distinguishing option value.
    GoogleString msg;
    NullMessageHandler handler;
    GoogleString deadline = StrCat(IntegerToString(priority * 100));
    options->ParseAndSetOptionFromName1("HtmlRewriteDeadlineMs", deadline,
                                        &msg, &handler);
    server_context_->vhost_config_manager()->AddVHost(pattern, priority,
                                                      std::move(options));
  }
};

// Test FindOptionsForHost with VHosts configured.
TEST_F(EnvoyServerContextVHostTest, FindOptionsForHostExactMatch) {
  const EnvoyRewriteOptions* opts =
      server_context_->FindOptionsForHost("api.example.com");
  ASSERT_NE(nullptr, opts);
  // Priority 10 * 100 = 1000.
  EXPECT_EQ(1000, opts->html_rewrite_deadline_ms());
}

// Test FindOptionsForHost with wildcard match.
TEST_F(EnvoyServerContextVHostTest, FindOptionsForHostWildcardMatch) {
  const EnvoyRewriteOptions* opts =
      server_context_->FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, opts);
  // Priority 20 * 100 = 2000.
  EXPECT_EQ(2000, opts->html_rewrite_deadline_ms());
}

// Test FindOptionsForHost with no match.
TEST_F(EnvoyServerContextVHostTest, FindOptionsForHostNoMatch) {
  const EnvoyRewriteOptions* opts =
      server_context_->FindOptionsForHost("other.example.net");
  EXPECT_EQ(nullptr, opts);
}

// Test GetOptionsForHost returns merged options for VHost match.
TEST_F(EnvoyServerContextVHostTest, GetOptionsForHostMergesOptions) {
  const RewriteOptions* opts =
      server_context_->GetOptionsForHost("api.example.com");
  ASSERT_NE(nullptr, opts);

  // Should have the VHost-specific deadline.
  const EnvoyRewriteOptions* envoy_opts =
      EnvoyRewriteOptions::DynamicCast(opts);
  ASSERT_NE(nullptr, envoy_opts);
  EXPECT_EQ(1000, envoy_opts->html_rewrite_deadline_ms());

  // Should not be the same pointer as global_options (it's merged).
  EXPECT_NE(server_context_->global_options(), opts);
}

// Test GetOptionsForHost caching behavior.
TEST_F(EnvoyServerContextVHostTest, GetOptionsForHostCachesMergedOptions) {
  const RewriteOptions* opts1 =
      server_context_->GetOptionsForHost("api.example.com");
  const RewriteOptions* opts2 =
      server_context_->GetOptionsForHost("api.example.com");

  // Same hostname should return cached options (same pointer).
  EXPECT_EQ(opts1, opts2);
}

// Test GetOptionsForHost with no VHost match returns global.
TEST_F(EnvoyServerContextVHostTest, GetOptionsForHostNoMatchReturnsGlobal) {
  const RewriteOptions* opts =
      server_context_->GetOptionsForHost("unknown.example.net");
  EXPECT_EQ(server_context_->global_options(), opts);
}

// Test that different hostnames get different merged options.
TEST_F(EnvoyServerContextVHostTest, DifferentHostsDifferentOptions) {
  const RewriteOptions* api_opts =
      server_context_->GetOptionsForHost("api.example.com");
  const RewriteOptions* www_opts =
      server_context_->GetOptionsForHost("www.example.com");

  // Different VHosts should have different options.
  EXPECT_NE(api_opts, www_opts);

  // Verify they have different deadline values.
  const EnvoyRewriteOptions* api_envoy =
      EnvoyRewriteOptions::DynamicCast(api_opts);
  const EnvoyRewriteOptions* www_envoy =
      EnvoyRewriteOptions::DynamicCast(www_opts);
  ASSERT_NE(nullptr, api_envoy);
  ASSERT_NE(nullptr, www_envoy);
  EXPECT_EQ(1000, api_envoy->html_rewrite_deadline_ms());
  EXPECT_EQ(2000, www_envoy->html_rewrite_deadline_ms());
}

// Test VHost priority ordering - exact match should take precedence.
TEST_F(EnvoyServerContextVHostTest, PriorityOrderingExactBeforeWildcard) {
  // "api.example.com" (priority 10) should match before "*.example.com" (20).
  const EnvoyRewriteOptions* opts =
      server_context_->FindOptionsForHost("api.example.com");
  ASSERT_NE(nullptr, opts);
  EXPECT_EQ(1000, opts->html_rewrite_deadline_ms());
}

// Test multiple server contexts from the same factory.
TEST_F(EnvoyServerContextTest, MultipleServerContexts) {
  EnvoyServerContext* ctx1 =
      factory_->MakeEnvoyServerContext("host1.example.com", 80);
  EnvoyServerContext* ctx2 =
      factory_->MakeEnvoyServerContext("host2.example.com", 8080);

  ASSERT_NE(nullptr, ctx1);
  ASSERT_NE(nullptr, ctx2);
  EXPECT_NE(ctx1, ctx2);

  // Both should reference the same factory.
  EXPECT_EQ(factory_.get(), ctx1->envoy_rewrite_driver_factory());
  EXPECT_EQ(factory_.get(), ctx2->envoy_rewrite_driver_factory());

  // Each should have independent VHost managers.
  EXPECT_NE(ctx1->vhost_config_manager(), ctx2->vhost_config_manager());
}

// Test that global options can be accessed and modified.
TEST_F(EnvoyServerContextTest, GlobalOptionsAccess) {
  RewriteOptions* global = server_context_->global_options();
  ASSERT_NE(nullptr, global);

  // Verify it's modifiable and the changes persist.
  global->set_enabled(RewriteOptions::kEnabledOff);
  EXPECT_EQ(RewriteOptions::kEnabledOff, global->enabled());
}

// Test thread system access through the context.
// Note: timer() requires worker threads to be started, so we don't test it
// in this unit test context.
TEST_F(EnvoyServerContextTest, ThreadSystemAccess) {
  EXPECT_NE(nullptr, server_context_->thread_system());
}

// Test that factory methods return proper types.
TEST_F(EnvoyServerContextTest, FactoryNewRewriteOptions) {
  RewriteOptions* opts = factory_->NewRewriteOptions();
  ASSERT_NE(nullptr, opts);

  // Should be EnvoyRewriteOptions.
  EnvoyRewriteOptions* envoy_opts = EnvoyRewriteOptions::DynamicCast(opts);
  EXPECT_NE(nullptr, envoy_opts);

  // Should have CoreFilters by default.
  EXPECT_EQ(RewriteOptions::kCoreFilters, opts->level());

  delete opts;
}

// Test NewRewriteOptionsForQuery returns a clean EnvoyRewriteOptions.
TEST_F(EnvoyServerContextTest, FactoryNewRewriteOptionsForQuery) {
  RewriteOptions* opts = factory_->NewRewriteOptionsForQuery();
  ASSERT_NE(nullptr, opts);

  // Should be EnvoyRewriteOptions.
  EnvoyRewriteOptions* envoy_opts = EnvoyRewriteOptions::DynamicCast(opts);
  EXPECT_NE(nullptr, envoy_opts);

  delete opts;
}

// Note: NewDecodingServerContext requires full statistics initialization
// which is complex in a unit test. We skip this test as it's better suited
// for integration testing.

}  // namespace

}  // namespace net_instaweb
