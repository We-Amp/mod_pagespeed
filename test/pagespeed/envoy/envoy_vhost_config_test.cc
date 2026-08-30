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

#include "pagespeed/envoy/envoy_vhost_config.h"

#include <chrono>
#include <string>
#include <memory>

#include "gtest/gtest.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "test/pagespeed/kernel/base/null_thread_system.h"

namespace net_instaweb {

class EnvoyVHostConfigTest : public ::testing::Test {
 protected:
  void SetUp() override { EnvoyRewriteOptions::Initialize(); }

  void TearDown() override { EnvoyRewriteOptions::Terminate(); }

  // Helper to create options with a specific admin path for identification.
  std::unique_ptr<EnvoyRewriteOptions> MakeOptions(const char* admin_path) {
    auto options = std::make_unique<EnvoyRewriteOptions>(&thread_system_);
    GoogleString msg;
    options->ParseAndSetOptionFromName1("AdminPath", admin_path, &msg,
                                        &handler_);
    return options;
  }

  NullThreadSystem thread_system_;
  NullMessageHandler handler_;
};

// Test exact host matching.
TEST_F(EnvoyVHostConfigTest, ExactMatch) {
  EXPECT_TRUE(
      EnvoyVHostConfigManager::HostPatternMatches("example.com", "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("www.example.com",
                                                          "www.example.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("example.com",
                                                           "www.example.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("www.example.com",
                                                           "example.com"));
}

// Test case-insensitivity.
TEST_F(EnvoyVHostConfigTest, CaseInsensitive) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("Example.COM",
                                                          "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("example.com",
                                                          "EXAMPLE.COM"));
  EXPECT_TRUE(
      EnvoyVHostConfigManager::HostPatternMatches("WWW.Example.Com",
                                                   "www.EXAMPLE.com"));
}

// Test single-character wildcard (?).
TEST_F(EnvoyVHostConfigTest, SingleCharWildcard) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("api-?.example.com",
                                                          "api-1.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("api-?.example.com",
                                                          "api-a.example.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("api-?.example.com",
                                                           "api-12.example.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("api-?.example.com",
                                                           "api-.example.com"));
}

// Test multi-character wildcard (*).
TEST_F(EnvoyVHostConfigTest, MultiCharWildcard) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.example.com",
                                                          "www.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.example.com",
                                                          "api.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.example.com",
                                                          "sub.domain.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.example.com",
                                                          ".example.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("*.example.com",
                                                           "example.com"));

  // Star matches zero or more chars.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*example.com",
                                                          "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*example.com",
                                                          "www.example.com"));
}

// Test multiple wildcards.
TEST_F(EnvoyVHostConfigTest, MultipleWildcards) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.*.example.com",
                                                          "a.b.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.*.*",
                                                          "a.b.c"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("*.*.example.com",
                                                           "a.example.com"));
}

// Test trailing wildcard.
TEST_F(EnvoyVHostConfigTest, TrailingWildcard) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("example.*",
                                                          "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("example.*",
                                                          "example.org"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("example.*",
                                                          "example.co.uk"));
}

// Test wildcard matching entire string.
TEST_F(EnvoyVHostConfigTest, WildcardMatchAll) {
  EXPECT_TRUE(
      EnvoyVHostConfigManager::HostPatternMatches("*", "example.com"));
  EXPECT_TRUE(
      EnvoyVHostConfigManager::HostPatternMatches("*", "anything.at.all"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*", ""));
}

// Test empty strings.
TEST_F(EnvoyVHostConfigTest, EmptyStrings) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("", ""));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("", "example.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("example.com", ""));
}

// Test combined wildcards.
TEST_F(EnvoyVHostConfigTest, CombinedWildcards) {
  EXPECT_TRUE(
      EnvoyVHostConfigManager::HostPatternMatches("api-?-*.example.com",
                                                   "api-1-dev.example.com"));
  EXPECT_TRUE(
      EnvoyVHostConfigManager::HostPatternMatches("api-?-*.example.com",
                                                   "api-a-production.example.com"));
  EXPECT_FALSE(
      EnvoyVHostConfigManager::HostPatternMatches("api-?-*.example.com",
                                                   "api-12-dev.example.com"));
}

// =============================================================================
// Priority Ordering Tests
// =============================================================================

// The daemon's management API socket (the /v1/daemon/* console proxy, issue
// #886) defaults to the daemon's packaged path and is settable per options.
TEST_F(EnvoyVHostConfigTest, DaemonApiSocketPathDefaultAndParse) {
  auto options = std::make_unique<EnvoyRewriteOptions>(&thread_system_);
  EXPECT_STREQ("/run/pagespeed-optimizer/api.sock",
               options->daemon_api_socket_path());
  GoogleString msg;
  EXPECT_EQ(RewriteOptions::kOptionOk,
            options->ParseAndSetOptionFromName1(
                "DaemonApiSocketPath", "/run/custom/api.sock", &msg,
                &handler_));
  EXPECT_STREQ("/run/custom/api.sock", options->daemon_api_socket_path());
}

// Test that lower priority values are matched first.
TEST_F(EnvoyVHostConfigTest, PriorityOrderingBasic) {
  EnvoyVHostConfigManager manager;

  // Add in reverse priority order.
  manager.AddVHost("*.example.com", 100, MakeOptions("/admin_low"));
  manager.AddVHost("www.example.com", 10, MakeOptions("/admin_high"));
  manager.SortByPriority();

  // The exact match with higher priority (lower number) should win.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_high", options->admin_path());
}

// Test that priority ordering is stable (preserves insertion order for equal
// priorities).
TEST_F(EnvoyVHostConfigTest, PriorityOrderingStable) {
  EnvoyVHostConfigManager manager;

  // Add two entries with the same priority.
  manager.AddVHost("*.example.com", 50, MakeOptions("/admin_first"));
  manager.AddVHost("*.example.com", 50, MakeOptions("/admin_second"));
  manager.SortByPriority();

  // The first one added should match.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_first", options->admin_path());
}

// Test that without sorting, insertion order is used.
TEST_F(EnvoyVHostConfigTest, PriorityOrderingWithoutSort) {
  EnvoyVHostConfigManager manager;

  // Add in order: high priority number first, low priority number second.
  manager.AddVHost("*.example.com", 100, MakeOptions("/admin_inserted_first"));
  manager.AddVHost("*.example.com", 10, MakeOptions("/admin_inserted_second"));
  // Note: Not calling SortByPriority()

  // Without sorting, insertion order is used.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_inserted_first", options->admin_path());
}

// Test priority ordering with many entries.
TEST_F(EnvoyVHostConfigTest, PriorityOrderingManyEntries) {
  EnvoyVHostConfigManager manager;

  // Add entries with various priorities in random order.
  manager.AddVHost("*.example.com", 50, MakeOptions("/admin_50"));
  manager.AddVHost("*.example.com", 10, MakeOptions("/admin_10"));
  manager.AddVHost("*.example.com", 100, MakeOptions("/admin_100"));
  manager.AddVHost("*.example.com", 1, MakeOptions("/admin_1"));
  manager.AddVHost("*.example.com", 25, MakeOptions("/admin_25"));
  manager.SortByPriority();

  // Priority 1 should win.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_1", options->admin_path());
}

// Test negative priorities.
TEST_F(EnvoyVHostConfigTest, PriorityOrderingNegative) {
  EnvoyVHostConfigManager manager;

  manager.AddVHost("*.example.com", 0, MakeOptions("/admin_zero"));
  manager.AddVHost("*.example.com", -100, MakeOptions("/admin_negative"));
  manager.AddVHost("*.example.com", 100, MakeOptions("/admin_positive"));
  manager.SortByPriority();

  // Negative priority should win.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_negative", options->admin_path());
}

// Test priority with different pattern types.
TEST_F(EnvoyVHostConfigTest, PriorityWithDifferentPatterns) {
  EnvoyVHostConfigManager manager;

  // A common pattern: exact match has highest priority, then specific
  // wildcards, then catch-all.
  manager.AddVHost("*", 1000, MakeOptions("/admin_catchall"));
  manager.AddVHost("*.example.com", 100, MakeOptions("/admin_domain"));
  manager.AddVHost("www.example.com", 10, MakeOptions("/admin_exact"));
  manager.SortByPriority();

  // Test each pattern priority.
  EXPECT_EQ("/admin_exact",
            manager.FindOptionsForHost("www.example.com")->admin_path());
  EXPECT_EQ("/admin_domain",
            manager.FindOptionsForHost("api.example.com")->admin_path());
  EXPECT_EQ("/admin_catchall",
            manager.FindOptionsForHost("other.org")->admin_path());
}

// =============================================================================
// Config Lookup Tests
// =============================================================================

// Test FindOptionsForHost returns nullptr when no match.
TEST_F(EnvoyVHostConfigTest, FindOptionsNoMatch) {
  EnvoyVHostConfigManager manager;
  manager.AddVHost("example.com", 0, MakeOptions("/admin"));
  manager.SortByPriority();

  EXPECT_EQ(nullptr, manager.FindOptionsForHost("other.com"));
  EXPECT_EQ(nullptr, manager.FindOptionsForHost("www.example.com"));
}

// Test FindOptionsForHost with empty manager.
TEST_F(EnvoyVHostConfigTest, FindOptionsEmptyManager) {
  EnvoyVHostConfigManager manager;
  EXPECT_EQ(nullptr, manager.FindOptionsForHost("example.com"));
  EXPECT_FALSE(manager.HasVHosts());
  EXPECT_EQ(0u, manager.size());
}

// Test HasVHosts and size().
TEST_F(EnvoyVHostConfigTest, HasVHostsAndSize) {
  EnvoyVHostConfigManager manager;
  EXPECT_FALSE(manager.HasVHosts());
  EXPECT_EQ(0u, manager.size());

  manager.AddVHost("example.com", 0, MakeOptions("/admin1"));
  EXPECT_TRUE(manager.HasVHosts());
  EXPECT_EQ(1u, manager.size());

  manager.AddVHost("other.com", 0, MakeOptions("/admin2"));
  EXPECT_TRUE(manager.HasVHosts());
  EXPECT_EQ(2u, manager.size());
}

// Test that the first matching pattern wins after sorting.
TEST_F(EnvoyVHostConfigTest, FirstMatchWins) {
  EnvoyVHostConfigManager manager;

  // Both patterns match www.example.com but with different priorities.
  manager.AddVHost("*.example.com", 20, MakeOptions("/admin_wildcard"));
  manager.AddVHost("www.*", 10, MakeOptions("/admin_prefix"));
  manager.SortByPriority();

  // www.* has lower priority number, so it wins.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_prefix", options->admin_path());
}

// =============================================================================
// Edge Cases: Empty and Special Hostnames
// =============================================================================

// Test empty hostname lookup.
TEST_F(EnvoyVHostConfigTest, EmptyHostnameLookup) {
  EnvoyVHostConfigManager manager;
  manager.AddVHost("*", 0, MakeOptions("/admin_catchall"));
  manager.AddVHost("", 0, MakeOptions("/admin_empty"));
  manager.SortByPriority();

  // Empty hostname should match empty pattern first (same priority, insertion
  // order).
  // Note: * also matches empty string.
  const EnvoyRewriteOptions* options = manager.FindOptionsForHost("");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_catchall", options->admin_path());
}

// Test empty pattern with empty hostname.
TEST_F(EnvoyVHostConfigTest, EmptyPatternEmptyHostname) {
  EnvoyVHostConfigManager manager;
  manager.AddVHost("", 0, MakeOptions("/admin_empty"));
  manager.SortByPriority();

  const EnvoyRewriteOptions* options = manager.FindOptionsForHost("");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_empty", options->admin_path());

  // Empty pattern should not match non-empty hostname.
  EXPECT_EQ(nullptr, manager.FindOptionsForHost("example.com"));
}

// Test very long hostnames.
TEST_F(EnvoyVHostConfigTest, VeryLongHostname) {
  EnvoyVHostConfigManager manager;

  // Create a very long hostname (253 chars is max for DNS).
  std::string long_hostname;
  for (int i = 0; i < 25; ++i) {
    if (i > 0) long_hostname += ".";
    long_hostname += "abcdefghij";  // 10 chars per segment
  }

  manager.AddVHost(long_hostname, 0, MakeOptions("/admin_long"));
  manager.AddVHost("*.com", 100, MakeOptions("/admin_wildcard"));
  manager.SortByPriority();

  // Exact match for long hostname.
  const EnvoyRewriteOptions* options = manager.FindOptionsForHost(long_hostname);
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_long", options->admin_path());
}

// Test very long hostname with wildcards.
TEST_F(EnvoyVHostConfigTest, VeryLongHostnameWithWildcard) {
  EnvoyVHostConfigManager manager;

  // Pattern with wildcards that will match a long hostname.
  manager.AddVHost("*.very-long-subdomain-name-that-goes-on.example.com", 0,
                   MakeOptions("/admin"));
  manager.SortByPriority();

  EXPECT_NE(nullptr,
            manager.FindOptionsForHost(
                "sub.very-long-subdomain-name-that-goes-on.example.com"));
}

// Test hostnames with special characters that are valid in hostnames.
TEST_F(EnvoyVHostConfigTest, SpecialCharactersHyphen) {
  // Hyphens are valid in hostnames.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("my-site.example.com",
                                                           "my-site.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("my-*.example.com",
                                                           "my-site.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*-site.example.com",
                                                           "my-site.example.com"));
}

// Test hostnames with numbers.
TEST_F(EnvoyVHostConfigTest, SpecialCharactersNumbers) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("server1.example.com",
                                                           "server1.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("server?.example.com",
                                                           "server1.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("192.168.1.1",
                                                           "192.168.1.1"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("192.168.*.1",
                                                           "192.168.1.1"));
}

// Test hostnames with underscores (technically not valid in DNS but sometimes
// used).
TEST_F(EnvoyVHostConfigTest, SpecialCharactersUnderscore) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("my_host.example.com",
                                                           "my_host.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*_host.example.com",
                                                           "my_host.example.com"));
}

// Test hostname with port (should match literally).
TEST_F(EnvoyVHostConfigTest, HostnameWithPort) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches(
      "example.com:8080", "example.com:8080"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("example.com:*",
                                                           "example.com:8080"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches(
      "example.com", "example.com:8080"));
}

// Test punycode/internationalized domain names.
TEST_F(EnvoyVHostConfigTest, PunycodeHostname) {
  // xn--nxasmq5b is the punycode for a Greek domain.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("xn--nxasmq5b.com",
                                                           "xn--nxasmq5b.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("xn--*.com",
                                                           "xn--nxasmq5b.com"));
}

// =============================================================================
// Case Sensitivity Edge Cases
// =============================================================================

// Test mixed case in wildcards.
TEST_F(EnvoyVHostConfigTest, CaseSensitivityMixedWildcards) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.EXAMPLE.com",
                                                           "www.example.COM"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("WWW.*.com",
                                                           "www.EXAMPLE.com"));
}

// Test case sensitivity with single character wildcard.
TEST_F(EnvoyVHostConfigTest, CaseSensitivitySingleCharWildcard) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("API-?.example.com",
                                                           "api-1.EXAMPLE.COM"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("api-?.EXAMPLE.com",
                                                           "API-A.example.com"));
}

// Test case sensitivity with all uppercase.
TEST_F(EnvoyVHostConfigTest, CaseSensitivityAllUppercase) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("EXAMPLE.COM",
                                                           "EXAMPLE.COM"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("EXAMPLE.COM",
                                                           "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("example.com",
                                                           "EXAMPLE.COM"));
}

// Test case with alternating case.
TEST_F(EnvoyVHostConfigTest, CaseSensitivityAlternating) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("ExAmPlE.cOm",
                                                           "eXaMpLe.CoM"));
}

// Test case sensitivity in manager lookup.
TEST_F(EnvoyVHostConfigTest, CaseSensitivityManagerLookup) {
  EnvoyVHostConfigManager manager;
  manager.AddVHost("WWW.EXAMPLE.COM", 0, MakeOptions("/admin"));
  manager.SortByPriority();

  EXPECT_NE(nullptr, manager.FindOptionsForHost("www.example.com"));
  EXPECT_NE(nullptr, manager.FindOptionsForHost("WWW.EXAMPLE.COM"));
  EXPECT_NE(nullptr, manager.FindOptionsForHost("Www.Example.Com"));
}

// =============================================================================
// Performance Tests with Many VHost Patterns
// =============================================================================

// Test lookup performance with many patterns.
TEST_F(EnvoyVHostConfigTest, PerformanceManyPatterns) {
  EnvoyVHostConfigManager manager;

  // Add 1000 vhost patterns.
  for (int i = 0; i < 1000; ++i) {
    std::string pattern = "host" + std::to_string(i) + ".example.com";
    std::string admin_path = "/admin" + std::to_string(i);
    manager.AddVHost(pattern, i, MakeOptions(admin_path.c_str()));
  }
  manager.SortByPriority();

  EXPECT_EQ(1000u, manager.size());

  // Lookup should still work correctly.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("host500.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin500", options->admin_path());

  // Test a non-matching lookup.
  EXPECT_EQ(nullptr, manager.FindOptionsForHost("host9999.example.com"));
}

// Test lookup performance with many wildcard patterns.
TEST_F(EnvoyVHostConfigTest, PerformanceManyWildcardPatterns) {
  EnvoyVHostConfigManager manager;

  // Add 500 wildcard patterns with increasing specificity.
  for (int i = 0; i < 500; ++i) {
    std::string pattern = "*.domain" + std::to_string(i) + ".example.com";
    std::string admin_path = "/admin" + std::to_string(i);
    manager.AddVHost(pattern, i, MakeOptions(admin_path.c_str()));
  }
  manager.SortByPriority();

  // Add a catch-all with lowest priority.
  manager.AddVHost("*", 10000, MakeOptions("/admin_catchall"));
  manager.SortByPriority();

  // Test wildcard matching.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("sub.domain250.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin250", options->admin_path());

  // Test catch-all for non-matching.
  options = manager.FindOptionsForHost("random.site.org");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_catchall", options->admin_path());
}

// Test that sorting many entries completes.
TEST_F(EnvoyVHostConfigTest, PerformanceSortManyEntries) {
  EnvoyVHostConfigManager manager;

  // Add entries in reverse priority order to stress sorting.
  for (int i = 1000; i > 0; --i) {
    std::string pattern = "host" + std::to_string(i) + ".example.com";
    std::string admin_path = "/admin" + std::to_string(i);
    manager.AddVHost(pattern, i, MakeOptions(admin_path.c_str()));
  }

  // Sorting should handle this efficiently.
  manager.SortByPriority();

  // Verify order is correct by checking first match.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("host1.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin1", options->admin_path());
}

// =============================================================================
// Additional Pattern Matching Edge Cases
// =============================================================================

// Test multiple consecutive wildcards.
TEST_F(EnvoyVHostConfigTest, ConsecutiveWildcards) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("**", "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("***", "example.com"));
  EXPECT_TRUE(
      EnvoyVHostConfigManager::HostPatternMatches("**.example.com",
                                                   "www.example.com"));
}

// Test wildcard at various positions.
TEST_F(EnvoyVHostConfigTest, WildcardPositions) {
  // Start.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*example.com",
                                                           "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*example.com",
                                                           "www.example.com"));
  // Middle.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("www.*.com",
                                                           "www.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("www.*.com",
                                                           "www.other.com"));
  // End.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("www.example.*",
                                                           "www.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("www.example.*",
                                                           "www.example.org"));
}

// Test question mark at various positions.
TEST_F(EnvoyVHostConfigTest, QuestionMarkPositions) {
  // Start.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("?ww.example.com",
                                                           "www.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("?ww.example.com",
                                                           "xww.example.com"));
  // Middle.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("www.ex?mple.com",
                                                           "www.example.com"));
  // End.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("www.example.co?",
                                                           "www.example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("www.example.co?",
                                                           "www.example.coz"));
}

// Test patterns that look similar but should not match.
TEST_F(EnvoyVHostConfigTest, SimilarButNotMatching) {
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("example.com",
                                                            "example.com.evil.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("example.com",
                                                            "fakeexample.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("*.example.com",
                                                            "example.com.evil.com"));
}

// Test that dots are matched literally (not as regex wildcards).
TEST_F(EnvoyVHostConfigTest, DotsMatchedLiterally) {
  EXPECT_TRUE(
      EnvoyVHostConfigManager::HostPatternMatches("example.com", "example.com"));
  EXPECT_FALSE(
      EnvoyVHostConfigManager::HostPatternMatches("example.com", "exampleXcom"));
  EXPECT_FALSE(
      EnvoyVHostConfigManager::HostPatternMatches("a.b.c", "aXbXc"));
}

// Test backtracking scenarios with wildcards.
TEST_F(EnvoyVHostConfigTest, WildcardBacktracking) {
  // Pattern that requires backtracking.
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*ample.com",
                                                           "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*ple.com",
                                                           "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*a*e.com",
                                                           "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*a*e*m",
                                                           "example.com"));
}

// Test patterns with trailing characters after wildcard.
TEST_F(EnvoyVHostConfigTest, WildcardWithSuffix) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.com",
                                                           "example.com"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.com",
                                                           "www.example.com"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("*.com",
                                                            "example.org"));
  EXPECT_FALSE(EnvoyVHostConfigManager::HostPatternMatches("*.com",
                                                            "example.commercial"));
}

// Test IPv6-style hostnames (square brackets).
TEST_F(EnvoyVHostConfigTest, IPv6StyleHostnames) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("[::1]", "[::1]"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("[*]", "[::1]"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches(
      "[2001:db8::1]", "[2001:db8::1]"));
}

// Test localhost and common special hostnames.
TEST_F(EnvoyVHostConfigTest, SpecialHostnames) {
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("localhost",
                                                           "localhost"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("local*",
                                                           "localhost"));
  EXPECT_TRUE(EnvoyVHostConfigManager::HostPatternMatches("*.local",
                                                           "myhost.local"));
}

// =============================================================================
// Re-sorting Behavior
// =============================================================================

// Test that adding entries after sorting invalidates the sort.
TEST_F(EnvoyVHostConfigTest, AddAfterSortNeedResort) {
  EnvoyVHostConfigManager manager;

  manager.AddVHost("*.example.com", 100, MakeOptions("/admin_low"));
  manager.SortByPriority();

  // Add a higher priority entry after sorting.
  manager.AddVHost("www.example.com", 1, MakeOptions("/admin_high"));
  // Without re-sorting, the new entry is at the end.

  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, options);
  // Without re-sort, the wildcard matches first.
  EXPECT_EQ("/admin_low", options->admin_path());

  // After re-sorting, the exact match should win.
  manager.SortByPriority();
  options = manager.FindOptionsForHost("www.example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_high", options->admin_path());
}

// =============================================================================
// Duplicate Pattern Handling
// =============================================================================

// Test multiple entries with the same pattern.
TEST_F(EnvoyVHostConfigTest, DuplicatePatterns) {
  EnvoyVHostConfigManager manager;

  manager.AddVHost("example.com", 10, MakeOptions("/admin_first"));
  manager.AddVHost("example.com", 20, MakeOptions("/admin_second"));
  manager.AddVHost("example.com", 5, MakeOptions("/admin_third"));
  manager.SortByPriority();

  // The one with lowest priority number should win.
  const EnvoyRewriteOptions* options =
      manager.FindOptionsForHost("example.com");
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/admin_third", options->admin_path());
}

}  // namespace net_instaweb
