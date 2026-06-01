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

#include "pagespeed/iis/iis_config.h"

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"

namespace net_instaweb {

class IisConfigTest : public testing::Test {
 protected:
  void SetUp() override {
    RewriteOptions::Initialize();
  }

  void TearDown() override {
    RewriteOptions::Terminate();
  }

  IisConfig config_;
};

// ============================================================================
// Default Values Tests
// ============================================================================

TEST_F(IisConfigTest, DefaultValuesAreReasonable) {
  EXPECT_TRUE(config_.enabled());
  EXPECT_TRUE(config_.admin_enabled());
  EXPECT_TRUE(config_.statistics_enabled());
  EXPECT_TRUE(config_.html_rewriting_enabled());
  EXPECT_TRUE(config_.webp_enabled());

  EXPECT_EQ("/pagespeed_admin", config_.admin_path());
  EXPECT_EQ("/pagespeed_statistics", config_.statistics_path());

  // Default cache sizes
  EXPECT_GT(config_.file_cache_size_kb(), 0);
  EXPECT_GT(config_.lru_cache_size_bytes(), 0);

  // Default timeouts
  EXPECT_GT(config_.fetcher_timeout_ms(), 0);
  EXPECT_GT(config_.html_rewrite_deadline_ms(), 0);

  // Default image quality
  EXPECT_GT(config_.image_recompress_quality(), 0);
  EXPECT_LE(config_.image_recompress_quality(), 100);
  EXPECT_GT(config_.webp_quality(), 0);
  EXPECT_LE(config_.webp_quality(), 100);
}

TEST_F(IisConfigTest, DefaultCachePathIsSet) {
  EXPECT_FALSE(config_.file_cache_path().empty());
}

// ============================================================================
// Redis Configuration Tests
// ============================================================================

TEST_F(IisConfigTest, DefaultRedisConfig) {
  EXPECT_FALSE(config_.redis_enabled());
  EXPECT_EQ("localhost", config_.redis_server());
  EXPECT_EQ(6379, config_.redis_port());
  EXPECT_GT(config_.redis_timeout_ms(), 0);
  EXPECT_EQ(0, config_.redis_database());
}

// ============================================================================
// CSS Configuration Tests
// ============================================================================

TEST_F(IisConfigTest, DefaultCssConfig) {
  EXPECT_TRUE(config_.css_combine_enabled());
  EXPECT_TRUE(config_.css_inline_enabled());
  EXPECT_GT(config_.css_inline_max_bytes(), 0);
  EXPECT_TRUE(config_.css_minify_enabled());
  EXPECT_TRUE(config_.css_flatten_imports_enabled());
  EXPECT_GT(config_.css_flatten_imports_max_depth(), 0);
}

// ============================================================================
// JavaScript Configuration Tests
// ============================================================================

TEST_F(IisConfigTest, DefaultJavaScriptConfig) {
  EXPECT_TRUE(config_.js_combine_enabled());
  EXPECT_TRUE(config_.js_inline_enabled());
  EXPECT_GT(config_.js_inline_max_bytes(), 0);
  EXPECT_TRUE(config_.js_minify_enabled());
  EXPECT_FALSE(config_.js_defer_enabled());  // Defer is off by default
}

// ============================================================================
// HTML Configuration Tests
// ============================================================================

TEST_F(IisConfigTest, DefaultHtmlConfig) {
  EXPECT_TRUE(config_.html_collapse_whitespace());
  EXPECT_TRUE(config_.html_remove_comments());
  EXPECT_TRUE(config_.html_elide_attributes());
  EXPECT_GT(config_.max_html_buffer_bytes(), 0);
}

// ============================================================================
// ApplyTo RewriteOptions Tests
// ============================================================================

TEST_F(IisConfigTest, ApplyToRewriteOptions) {
  RewriteOptions options(nullptr);

  config_.ApplyTo(&options);

  // Verify IIS-specific settings are configured
  // Note: file_cache_path and max_html_buffer_bytes are IIS-specific configs
  // that are not part of the base RewriteOptions class.
  EXPECT_FALSE(config_.file_cache_path().empty());
  EXPECT_GT(config_.max_html_buffer_bytes(), 0);

  // Verify image settings were applied
  EXPECT_EQ(config_.image_recompress_quality(),
            options.image_recompress_quality());
}

TEST_F(IisConfigTest, ApplyToCssFilters) {
  RewriteOptions options(nullptr);

  config_.ApplyTo(&options);

  // Verify CSS filters were enabled
  EXPECT_TRUE(options.Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kInlineCss));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kRewriteCss));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kFlattenCssImports));
}

TEST_F(IisConfigTest, ApplyToJavaScriptFilters) {
  RewriteOptions options(nullptr);

  config_.ApplyTo(&options);

  // Verify JavaScript filters were enabled
  EXPECT_TRUE(options.Enabled(RewriteOptions::kCombineJavascript));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kInlineJavascript));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kRewriteJavascriptExternal));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kRewriteJavascriptInline));

  // Defer should be off by default
  EXPECT_FALSE(options.Enabled(RewriteOptions::kDeferJavascript));
}

TEST_F(IisConfigTest, ApplyToHtmlFilters) {
  RewriteOptions options(nullptr);

  config_.ApplyTo(&options);

  // Verify HTML filters were enabled
  EXPECT_TRUE(options.Enabled(RewriteOptions::kCollapseWhitespace));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kRemoveComments));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kElideAttributes));
}

TEST_F(IisConfigTest, ApplyToImageFilters) {
  RewriteOptions options(nullptr);

  config_.ApplyTo(&options);

  // Verify image filters were enabled
  EXPECT_TRUE(options.Enabled(RewriteOptions::kLazyloadImages));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kConvertJpegToWebp));
  EXPECT_TRUE(options.Enabled(RewriteOptions::kResizeImages));
}

// ============================================================================
// Inline Size Configuration Tests
// ============================================================================

TEST_F(IisConfigTest, InlineMaxBytesDefaults) {
  EXPECT_EQ(2048, config_.css_inline_max_bytes());
  EXPECT_EQ(2048, config_.js_inline_max_bytes());
}

TEST_F(IisConfigTest, ApplyToInlineSizes) {
  RewriteOptions options(nullptr);

  config_.ApplyTo(&options);

  EXPECT_EQ(config_.css_inline_max_bytes(), options.css_inline_max_bytes());
  EXPECT_EQ(config_.js_inline_max_bytes(), options.js_inline_max_bytes());
}

// ============================================================================
// Enabled/Disabled Filter Lists Tests
// ============================================================================

TEST_F(IisConfigTest, EnabledFiltersListIsEmpty) {
  EXPECT_TRUE(config_.enabled_filters().empty());
}

TEST_F(IisConfigTest, DisabledFiltersListIsEmpty) {
  EXPECT_TRUE(config_.disabled_filters().empty());
}

// ============================================================================
// Fetcher Configuration Tests
// ============================================================================

TEST_F(IisConfigTest, FetcherProxyIsEmptyByDefault) {
  EXPECT_TRUE(config_.fetcher_proxy().empty());
}

TEST_F(IisConfigTest, FetcherTimeoutHasReasonableDefault) {
  EXPECT_GE(config_.fetcher_timeout_ms(), 1000);  // At least 1 second
  EXPECT_LE(config_.fetcher_timeout_ms(), 60000);  // At most 60 seconds
}

// ============================================================================
// HTML Rewriting Configuration Tests
// ============================================================================

TEST_F(IisConfigTest, HtmlRewriteDeadlineMatchesModPagespeed) {
  // Must match RewriteOptions::kDefaultRewriteDeadlineMs exactly.
  // Debug builds: 20ms, release builds: 10ms.
#ifdef NDEBUG
  EXPECT_EQ(10, config_.html_rewrite_deadline_ms());
#else
  EXPECT_EQ(20, config_.html_rewrite_deadline_ms());
#endif
}

TEST_F(IisConfigTest, MaxHtmlBufferHasReasonableDefault) {
  EXPECT_GE(config_.max_html_buffer_bytes(), 1024 * 1024);  // At least 1MB
  EXPECT_LE(config_.max_html_buffer_bytes(), 10 * 1024 * 1024);  // At most 10MB
}

}  // namespace net_instaweb
