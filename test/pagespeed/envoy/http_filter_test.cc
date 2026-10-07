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

// Unit tests for HttpPageSpeedDecoderFilter request-header handling.
// Integration testing of the full request path requires the system tests.

#include "pagespeed/envoy/http_filter.h"

#include <memory>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/system/system_thread_system.h"
#include "source/common/stats/isolated_store_impl.h"
#include "test/pagespeed/envoy/mocks.h"

namespace net_instaweb {
namespace {

using Envoy::Http::FilterHeadersStatus;

// Test fixture with a real EnvoyServerContext, mirroring
// envoy_server_context_test.cc.
class HttpFilterTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { EnvoyRewriteOptions::Initialize(); }
  static void TearDownTestSuite() { EnvoyRewriteOptions::Terminate(); }

  void SetUp() override {
    thread_system_ = new SystemThreadSystem();
    factory_ = std::make_unique<EnvoyRewriteDriverFactory>(
        ProcessContext(), thread_system_, "test.example.com", 8080);
    factory_->Init();
    server_context_ =
        factory_->MakeEnvoyServerContext("test.example.com", 8080);

    pagespeed::Decoder proto_config;
    config_ = std::make_shared<Envoy::Http::HttpPageSpeedDecoderFilterConfig>(
        proto_config, *stats_scope_);
    filter_ = std::make_unique<Envoy::Http::HttpPageSpeedDecoderFilter>(
        config_, server_context_, nullptr /* proxy_fetch_factory */);
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
  }

  void TearDown() override {
    filter_.reset();
    server_context_ = nullptr;
    factory_->ShutDown();
    factory_.reset();
    // thread_system_ is owned by the factory.
  }

  SystemThreadSystem* thread_system_;
  std::unique_ptr<EnvoyRewriteDriverFactory> factory_;
  EnvoyServerContext* server_context_;
  Envoy::Stats::IsolatedStoreImpl stats_store_;
  Envoy::Stats::ScopeSharedPtr stats_scope_ = stats_store_.rootScope();
  Envoy::Http::HttpPageSpeedDecoderFilterConfigSharedPtr config_;
  std::unique_ptr<Envoy::Http::HttpPageSpeedDecoderFilter> filter_;
  testing::MockStreamDecoderFilterCallbacks decoder_callbacks_;
};

// A request that carries no path header matches none of the filter's routes
// and is passed on to the rest of the filter chain.
TEST_F(HttpFilterTest, RequestWithoutPathHeaderPassesThrough) {
  Envoy::Http::TestRequestHeaderMapImpl headers{{":method", "GET"},
                                                {":authority", "example.com"}};
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));
}

// The same holds when the request carries neither a host nor a path header.
TEST_F(HttpFilterTest, RequestWithoutHostOrPathHeaderPassesThrough) {
  Envoy::Http::TestRequestHeaderMapImpl headers{{":method", "GET"}};
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));
}

}  // namespace
}  // namespace net_instaweb
