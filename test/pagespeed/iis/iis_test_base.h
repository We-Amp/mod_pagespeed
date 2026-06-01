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

#ifndef PAGESPEED_IIS_IIS_TEST_BASE_H_
#define PAGESPEED_IIS_IIS_TEST_BASE_H_

#include <memory>

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/mock_message_handler.h"
#include "pagespeed/kernel/base/mock_timer.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/mock_scheduler.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/iis/mock_iis.h"
#include "test/pagespeed/kernel/base/mock_hasher.h"

namespace net_instaweb {

class RewriteDriver;
class RewriteOptions;
class ServerContext;
class Statistics;

// Base class for IIS module unit tests.
// Provides common test infrastructure including mock IIS context,
// message handler, timer, and thread system.
class IisTestBase : public testing::Test {
 protected:
  IisTestBase();
  ~IisTestBase() override;

  void SetUp() override;
  void TearDown() override;

  // Create a mock HTTP context with the given URL
  std::unique_ptr<MockHttpContext> CreateContext(const GoogleString& url);

  // Create a mock HTTP context with full customization
  std::unique_ptr<MockHttpContext> CreateContext(
      const GoogleString& url,
      const GoogleString& method,
      const std::map<GoogleString, GoogleString>& headers);

  // Create mock configuration
  std::unique_ptr<MockAppHostElement> CreateConfig();
  MockConfigBuilder& config_builder() { return config_builder_; }

  // Access to mock infrastructure
  MockMessageHandler* message_handler() { return message_handler_.get(); }
  MockTimer* timer() { return timer_.get(); }
  MockScheduler* scheduler() { return scheduler_.get(); }
  ThreadSystem* thread_system() { return thread_system_.get(); }
  MockHasher* hasher() { return hasher_.get(); }
  Statistics* statistics() { return statistics_.get(); }

  // Helper to advance mock timer
  void AdvanceTimeMs(int64 ms);

  // Helper to check if a URL is a PageSpeed resource URL
  static bool IsPageSpeedUrl(const GoogleString& url);

  // Helper to create a .pagespeed. URL
  static GoogleString CreatePageSpeedUrl(
      const GoogleString& original_url,
      const GoogleString& filter_id,
      const GoogleString& hash);

  // Helper to decode a .pagespeed. URL
  static bool DecodePageSpeedUrl(
      const GoogleString& pagespeed_url,
      GoogleString* original_url,
      GoogleString* filter_id,
      GoogleString* hash);

 private:
  std::unique_ptr<MockMessageHandler> message_handler_;
  std::unique_ptr<MockTimer> timer_;
  std::unique_ptr<MockScheduler> scheduler_;
  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<MockHasher> hasher_;
  std::unique_ptr<Statistics> statistics_;
  MockConfigBuilder config_builder_;

  DISALLOW_COPY_AND_ASSIGN(IisTestBase);
};

// Convenience macro for IIS-only tests
#ifdef _WIN32
#define IIS_TEST_F(test_fixture, test_name) TEST_F(test_fixture, test_name)
#else
// On non-Windows, skip IIS-specific tests that require actual IIS headers
#define IIS_TEST_F(test_fixture, test_name) \
  TEST_F(test_fixture, DISABLED_##test_name)
#endif

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_TEST_BASE_H_
