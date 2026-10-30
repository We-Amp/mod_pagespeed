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

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/envoy/header_utils.h"
#include "pagespeed/system/system_server_context.h"
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

// The request method of the Envoy request is carried into the PageSpeed
// request headers.
TEST(HeaderUtilsTest, PropagatesRequestMethod) {
  const struct {
    const char* name;
    RequestHeaders::Method method;
  } kCases[] = {
      {"GET", RequestHeaders::kGet},       {"HEAD", RequestHeaders::kHead},
      {"POST", RequestHeaders::kPost},     {"PUT", RequestHeaders::kPut},
      {"DELETE", RequestHeaders::kDelete},
  };
  for (const auto& test_case : kCases) {
    Envoy::Http::TestRequestHeaderMapImpl headers{{":method", test_case.name},
                                                  {":path", "/a.css"}};
    std::unique_ptr<RequestHeaders> request_headers =
        HeaderUtils::toPageSpeedRequestHeaders(headers);
    EXPECT_EQ(test_case.method, request_headers->method()) << test_case.name;
  }
}

// Only GET and HEAD are served from the cache; only GET is stored.
TEST(HeaderUtilsTest, ServesOnlyGetAndHeadFromCache) {
  EXPECT_TRUE(HeaderUtils::MayServeFromCache(RequestHeaders::kGet));
  EXPECT_TRUE(HeaderUtils::MayServeFromCache(RequestHeaders::kHead));
  EXPECT_TRUE(HeaderUtils::MayStoreInCache(RequestHeaders::kGet));
  EXPECT_FALSE(HeaderUtils::MayStoreInCache(RequestHeaders::kHead));
  for (RequestHeaders::Method method :
       {RequestHeaders::kPost, RequestHeaders::kPut, RequestHeaders::kDelete,
        RequestHeaders::kOptions, RequestHeaders::kPatch,
        RequestHeaders::kError}) {
    EXPECT_FALSE(HeaderUtils::MayServeFromCache(method));
    EXPECT_FALSE(HeaderUtils::MayStoreInCache(method));
  }
}

// With in-place optimization on, an in-place eligible URL is looked up in the
// cache for GET requests only; a POST is passed on to the rest of the chain
// untouched.
TEST_F(HttpFilterTest, InPlaceLookupServesOnlyGetAndHead) {
  // Resource URL decoding needs the decoding driver of an initialized server
  // context.
  std::vector<SystemServerContext*> server_contexts{server_context_};
  GoogleString error_message;
  int error_index = -1;
  Statistics* global_statistics = nullptr;
  factory_->PostConfig(server_contexts, &error_message, &error_index,
                       &global_statistics);
  ASSERT_EQ(-1, error_index) << error_message;
  EnvoyRewriteDriverFactory::InitStats(global_statistics);
  factory_->RootInit();
  factory_->ChildInit();

  RewriteOptions* options = server_context_->global_options();
  options->ClearSignatureForTesting();
  options->set_enabled(RewriteOptions::kEnabledOn);
  options->set_in_place_rewriting_enabled(true);
  server_context_->ComputeSignature(options);

  // The lookup's miss is reported through the dispatcher; collect the posted
  // callbacks instead of running them.
  testing::FakeDispatcher fake_dispatcher;
  ON_CALL(decoder_callbacks_, dispatcher())
      .WillByDefault(::testing::ReturnRef(fake_dispatcher));
  EXPECT_CALL(decoder_callbacks_, dispatcher()).Times(::testing::AnyNumber());
  EXPECT_CALL(fake_dispatcher, post(::testing::_))
      .Times(::testing::AnyNumber());
  Envoy::Http::TestRequestHeaderMapImpl post_headers{
      {":method", "POST"}, {":path", "/a.css"}, {":authority", "example.com"}};
  EXPECT_EQ(FilterHeadersStatus::Continue,
            filter_->decodeHeaders(post_headers, true));
  EXPECT_EQ(0u, fake_dispatcher.pendingCallbackCount());

  EXPECT_CALL(decoder_callbacks_, continueDecoding())
      .Times(::testing::AnyNumber());
  auto wait_for_posted_callback = [&fake_dispatcher]() {
    for (int i = 0; i < 500 && fake_dispatcher.pendingCallbackCount() == 0;
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fake_dispatcher.pendingCallbackCount();
  };
  auto new_filter = [&]() {
    auto filter = std::make_unique<Envoy::Http::HttpPageSpeedDecoderFilter>(
        config_, server_context_, nullptr /* proxy_fetch_factory */);
    filter->setDecoderFilterCallbacks(decoder_callbacks_);
    return filter;
  };

  // A HEAD request is looked up too, but a miss starts no recording.
  auto head_filter = new_filter();
  Envoy::Http::TestRequestHeaderMapImpl head_headers{
      {":method", "HEAD"}, {":path", "/a.css"}, {":authority", "example.com"}};
  EXPECT_EQ(FilterHeadersStatus::StopIteration,
            head_filter->decodeHeaders(head_headers, true));
  EXPECT_EQ(1u, wait_for_posted_callback());
  fake_dispatcher.runAllPendingCallbacks();
  EXPECT_EQ(0, server_context_->rewrite_stats()->ipro_not_in_cache()->Get());

  // A GET request is looked up, and a miss starts a recording.
  auto get_filter = new_filter();
  Envoy::Http::TestRequestHeaderMapImpl get_headers{
      {":method", "GET"}, {":path", "/a.css"}, {":authority", "example.com"}};
  EXPECT_EQ(FilterHeadersStatus::StopIteration,
            get_filter->decodeHeaders(get_headers, true));
  EXPECT_EQ(1u, wait_for_posted_callback());
  fake_dispatcher.runAllPendingCallbacks();
  EXPECT_EQ(1, server_context_->rewrite_stats()->ipro_not_in_cache()->Get());
}

}  // namespace
}  // namespace net_instaweb
