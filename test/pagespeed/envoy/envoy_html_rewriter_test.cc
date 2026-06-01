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

// Unit tests for EnvoyHtmlRewriter.
// Note: These tests focus on the state machine and detection logic that can
// be tested without the full Envoy infrastructure. Integration testing of
// the full HTML rewriting path requires the system tests.

#include "pagespeed/envoy/envoy_html_rewriter.h"

#include "gtest/gtest.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "test/pagespeed/kernel/base/null_thread_system.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

// Test that EnvoyRewriteOptions has the correct default values for
// HTML rewriting options.
class EnvoyHtmlRewriterOptionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    EnvoyRewriteOptions::Initialize();
    options_ = std::make_unique<EnvoyRewriteOptions>(&thread_system_);
  }

  void TearDown() override { EnvoyRewriteOptions::Terminate(); }

  NullThreadSystem thread_system_;
  std::unique_ptr<EnvoyRewriteOptions> options_;
};

TEST_F(EnvoyHtmlRewriterOptionsTest, DefaultHtmlRewritingEnabled) {
  EXPECT_TRUE(options_->enable_html_rewriting());
}

TEST_F(EnvoyHtmlRewriterOptionsTest, DefaultHtmlRewriteDeadlineMs) {
  // Default should be 2000ms
  EXPECT_EQ(2000, options_->html_rewrite_deadline_ms());
}

TEST_F(EnvoyHtmlRewriterOptionsTest, DefaultMaxHtmlBufferBytes) {
  // Default should be 2MB
  EXPECT_EQ(2 * 1024 * 1024, options_->max_html_buffer_bytes());
}

TEST_F(EnvoyHtmlRewriterOptionsTest, CanDisableHtmlRewriting) {
  // Set EnableHtmlRewriting to false using the option mechanism.
  GoogleString msg;
  NullMessageHandler handler;
  EXPECT_EQ(RewriteOptions::kOptionOk,
            options_->ParseAndSetOptionFromName1("EnableHtmlRewriting", "off",
                                                  &msg, &handler));
  EXPECT_FALSE(options_->enable_html_rewriting());
}

TEST_F(EnvoyHtmlRewriterOptionsTest, CanSetHtmlRewriteDeadlineMs) {
  GoogleString msg;
  NullMessageHandler handler;
  EXPECT_EQ(RewriteOptions::kOptionOk,
            options_->ParseAndSetOptionFromName1("HtmlRewriteDeadlineMs", "5000",
                                                  &msg, &handler));
  EXPECT_EQ(5000, options_->html_rewrite_deadline_ms());
}

TEST_F(EnvoyHtmlRewriterOptionsTest, CanSetMaxHtmlBufferBytes) {
  GoogleString msg;
  NullMessageHandler handler;
  EXPECT_EQ(RewriteOptions::kOptionOk,
            options_->ParseAndSetOptionFromName1("MaxHtmlBufferBytes", "1048576",
                                                  &msg, &handler));
  EXPECT_EQ(1048576, options_->max_html_buffer_bytes());
}

// Test that options are properly cloned.
TEST_F(EnvoyHtmlRewriterOptionsTest, OptionsClonePreservesHtmlSettings) {
  GoogleString msg;
  NullMessageHandler handler;

  // Modify the options.
  options_->ParseAndSetOptionFromName1("EnableHtmlRewriting", "off", &msg,
                                        &handler);
  options_->ParseAndSetOptionFromName1("HtmlRewriteDeadlineMs", "3000", &msg,
                                        &handler);
  options_->ParseAndSetOptionFromName1("MaxHtmlBufferBytes", "500000", &msg,
                                        &handler);

  // Clone and verify.
  std::unique_ptr<EnvoyRewriteOptions> cloned(options_->Clone());
  EXPECT_FALSE(cloned->enable_html_rewriting());
  EXPECT_EQ(3000, cloned->html_rewrite_deadline_ms());
  EXPECT_EQ(500000, cloned->max_html_buffer_bytes());
}

}  // namespace net_instaweb
