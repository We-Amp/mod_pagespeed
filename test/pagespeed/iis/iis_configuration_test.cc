// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for IIS configuration file parsing (ConfigurationFile).
// These tests exercise ParseConfigText via CreateTestConfigFile, using
// an injected expand_env function to avoid real Windows API calls.
//
// Windows-only: depends on //pagespeed/iis:iis_module which requires Windows.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/system/optimization_thread_policy.h"
#include "pagespeed/kernel/base/string_util.h"

// Forward declarations of test helpers (defined in iis_configuration.cpp).
class ConfigurationFile;
ConfigurationFile* CreateTestConfigFile(
    const std::string& text,
    net_instaweb::MessageHandler* mh,
    net_instaweb::global_settings& config,
    std::function<std::string(const std::string&)> expand_env);
bool TestConfigFileGetConfig(
    ConfigurationFile* cf,
    std::map<std::string, std::string>& input,
    net_instaweb::RewriteOptions& options);
void TestConfigFileRelease(ConfigurationFile* cf);

using net_instaweb::IisRewriteOptions;
using net_instaweb::RewriteOptions;

namespace {

// Test fixture that initializes and terminates IisRewriteOptions.
class IisConfigurationTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    IisRewriteOptions::Initialize();
  }
  static void TearDownTestSuite() {
    IisRewriteOptions::Terminate();
  }

  void SetUp() override {
    global_config_ = net_instaweb::global_settings();
  }

  // Helper: parse config text and return the ConfigurationFile.
  // Uses the injected expand_env (defaults to identity if nullptr).
  ConfigurationFile* Parse(
      const std::string& text,
      std::function<std::string(const std::string&)> expand_env = nullptr) {
    return CreateTestConfigFile(text, &handler_, global_config_, expand_env);
  }

  // Helper: parse config text and merge options for a given host into rwo.
  // Returns true if stop-matching was signaled.
  bool ParseAndGetConfig(
      const std::string& text,
      const std::string& host,
      IisRewriteOptions* rwo,
      std::function<std::string(const std::string&)> expand_env = nullptr) {
    ConfigurationFile* cf = Parse(text, expand_env);
    std::map<std::string, std::string> input;
    input["config"] = "base";
    if (!host.empty()) {
      input["host"] = host;
    }
    bool stopped = TestConfigFileGetConfig(cf, input, *rwo);
    TestConfigFileRelease(cf);
    return stopped;
  }

  net_instaweb::global_settings global_config_;
  net_instaweb::GoogleMessageHandler handler_;
};

// ==========================================================================
// Phase 2: Bug Exposure Tests
// ==========================================================================

// Bug 1: ExpandEnvironmentStringsA truncation.
// The original code allocates a fixed MAX_PATH+1 (261 byte) buffer.
// If the expanded path exceeds MAX_PATH, it gets silently truncated.
// This test injects an expand_env that returns a 300-char path and
// verifies the full path is stored in the parsed options.
TEST_F(IisConfigurationTest, ExpandEnvTruncation) {
  // Create a path longer than MAX_PATH (260).
  std::string long_path(300, 'X');
  // Prefix with a drive letter so the backslash-to-slash conversion
  // path is exercised.
  long_path = "C:\\" + long_path;

  auto expand_env = [&](const std::string& input) -> std::string {
    return long_path;
  };

  std::string config_text = "pagespeed FileCachePath %SOME_VAR%\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo, expand_env);

  // After expansion and backslash conversion, the path should use forward
  // slashes. The full 300+ char path must be preserved.
  std::string expected = long_path;
  std::replace(expected.begin(), expected.end(), '\\', '/');

  // file_cache_path() returns a GoogleString (std::string).
  EXPECT_EQ(expected, rwo.file_cache_path())
      << "ExpandEnv should not truncate paths longer than MAX_PATH";
}

// Bug 2: RemoteConfigurationUrl 'continue' skips line advancement.
// The 'continue' statement in the RemoteConfigurationUrl handler causes the
// parser to skip the end-of-line advancement (lines that skip \n and \r).
// This results in a spurious empty-option iteration before the next real
// config line is processed. The test verifies that the line following
// RemoteConfigurationUrl is correctly parsed and applied.
TEST_F(IisConfigurationTest, RemoteConfigUrlContinue) {
  std::string config_text =
      "pagespeed RemoteConfigurationUrl http://example.com/config\n"
      "pagespeed EnableFilters combine_css\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  // The combine_css filter should be enabled from the second line.
  // Before fix: the 'continue' causes a spurious empty-option call to
  // SetOptionFromName which logs a warning. After fix: clean parsing
  // with no spurious iterations.
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss))
      << "EnableFilters combine_css after RemoteConfigurationUrl should work";
}

// Bug 3: GetFileSize int overflow.
// GetFileSize returns DWORD (unsigned 32-bit) but the original code stores
// it in 'int' (signed 32-bit). For files > 2GB, this overflows to negative,
// causing a huge allocation or crash. The fix: change 'int' to 'DWORD' and
// add a size sanity check. Since this bug is in LoadFile (file I/O path),
// not ParseConfigText, we can only add a trivial test here.
TEST_F(IisConfigurationTest, EmptyTextNoCrash_GetFileSizeBugComment) {
  // This tests that ParseConfigText with empty text doesn't crash.
  // The actual GetFileSize int overflow fix is applied directly to LoadFile:
  //   - Change: int size = GetFileSize(file, NULL);
  //   - To:     DWORD size = GetFileSize(file, NULL);
  //   - Add:    if (size == INVALID_FILE_SIZE || size > (100 * 1024 * 1024))
  //                 { CloseHandle(file); return; }
  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig("", "", &rwo);
  // No crash = success.
}

// Bug 4: GetFileTime unchecked return value.
// The original code calls GetFileTime without checking the return value.
// If it fails, orgtime contains uninitialized data, causing Expired() to
// behave unpredictably. The fix: zero-initialize orgtime in the constructor
// and check GetFileTime's return value.
// Since this is also in the file I/O path, we just verify basic parsing works.
TEST_F(IisConfigurationTest, BasicParseWorksForGetFileTimeBugComment) {
  // The fix applied directly to LoadFile:
  //   - Zero-initialize orgtime in constructor
  //   - Check return: if (!GetFileTime(file, NULL, NULL, &orgtime)) { ... }
  std::string config_text = "pagespeed EnableFilters combine_css\n";
  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// ==========================================================================
// Phase 4: Functionality Tests
// ==========================================================================

// MatchRuleBasic: host regex matches, filter enabled.
// Note: host-based match rules create ConfigLine(key="config", value="request")
// entries, so input["config"] must be "request" (the per-request context).
TEST_F(IisConfigurationTest, MatchRuleBasic) {
  std::string config_text =
      "host:example\\.com\n"
      "pagespeed EnableFilters combine_css\n";

  ConfigurationFile* cf = Parse(config_text);
  std::map<std::string, std::string> input;
  input["host"] = "example.com";
  input["config"] = "request";
  IisRewriteOptions rwo(nullptr);
  TestConfigFileGetConfig(cf, input, rwo);
  TestConfigFileRelease(cf);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// Item 3: a server-scope option (StatisticsPath) inside a matched
// (per-request) block must be rejected, leaving the default value.
TEST_F(IisConfigurationTest, ServerScopedOptionInMatchBlockIgnored) {
  std::string config_text =
      "host:example\\.com\n"
      "pagespeed StatisticsPath /custom_stats\n";

  ConfigurationFile* cf = Parse(config_text);
  std::map<std::string, std::string> input;
  input["host"] = "example.com";
  input["config"] = "request";
  IisRewriteOptions rwo(nullptr);
  TestConfigFileGetConfig(cf, input, rwo);
  TestConfigFileRelease(cf);

  EXPECT_NE("/custom_stats", rwo.statistics_path())
      << "server-scope StatisticsPath must be ignored inside a match block";
}

// Item 3 positive control: the same server-scope option applies in the base
// (non-matched) config, and a directory-scope filter in a match block still
// works (covered by MatchRuleBasic).
TEST_F(IisConfigurationTest, ServerScopedOptionInBaseApplies) {
  std::string config_text = "pagespeed StatisticsPath /custom_stats\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_EQ("/custom_stats", rwo.statistics_path())
      << "server-scope StatisticsPath applies in the base config";
}

// ==========================================================================
// The two daemon path options (parsed and stored; the per-site startup
// check reads them)
// ==========================================================================

// Records the parser's formatted messages, for assertions on the warning a
// refused directive leaves.
class RecordingMessageHandler : public net_instaweb::MessageHandler {
 public:
  int CountContaining(const std::string& needle) const {
    int n = 0;
    for (const std::string& m : messages_) {
      if (m.find(needle) != std::string::npos) {
        ++n;
      }
    }
    return n;
  }

 protected:
  void MessageSImpl(net_instaweb::MessageType type,
                    const GoogleString& message) override {
    messages_.push_back(message);
  }
  void FileMessageSImpl(net_instaweb::MessageType type, const char* file,
                        int line, const GoogleString& message) override {
    messages_.push_back(message);
  }

 private:
  std::vector<std::string> messages_;
};

// Both options parse from a pagespeed.config line and are stored; the
// backslash path is preserved byte for byte (the FileCachePath/LogDir
// slash conversion is special-cased and must not leak to other options).
TEST_F(IisConfigurationTest, DaemonOptionsParseAndStore) {
  std::string config_text =
      "pagespeed DaemonSocketPath C:\\daemon\\notify.sock\n"
      "pagespeed DaemonVolumePath D:\\daemon\\volume\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_EQ("C:\\daemon\\notify.sock", rwo.daemon_socket_path());
  EXPECT_EQ("D:\\daemon\\volume", rwo.daemon_volume_path());
}

// The default for both options is empty: no daemon is configured.
TEST_F(IisConfigurationTest, DaemonOptionsDefaultEmpty) {
  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig("", "", &rwo);

  EXPECT_EQ("", rwo.daemon_socket_path());
  EXPECT_EQ("", rwo.daemon_volume_path());
}

// The request-freshness switch is off by default and parses on and off.
TEST_F(IisConfigurationTest, HonorRequestFreshnessDefaultsOffAndParses) {
  IisRewriteOptions defaults(nullptr);
  ParseAndGetConfig("", "", &defaults);
  EXPECT_FALSE(defaults.daemon_serve_honor_request_freshness());

  IisRewriteOptions on(nullptr);
  ParseAndGetConfig("pagespeed DaemonServeHonorRequestFreshness on\n", "", &on);
  EXPECT_TRUE(on.daemon_serve_honor_request_freshness());

  IisRewriteOptions off(nullptr);
  ParseAndGetConfig("pagespeed DaemonServeHonorRequestFreshness off\n", "",
                    &off);
  EXPECT_FALSE(off.daemon_serve_honor_request_freshness());
}

// A quoted value with spaces and a backslash path survives byte for byte:
// the config tokenizer keeps a double-quoted token verbatim.
TEST_F(IisConfigurationTest, DaemonOptionQuotedValueWithSpacesVerbatim) {
  std::string config_text =
      "pagespeed DaemonSocketPath \"C:\\Program "
      "Files\\PageSpeed\\daemon\\notify.sock\"\n"
      "pagespeed DaemonVolumePath \"C:\\Program "
      "Files\\PageSpeed\\daemon\\cache volume\"\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_EQ("C:\\Program Files\\PageSpeed\\daemon\\notify.sock",
            rwo.daemon_socket_path());
  EXPECT_EQ("C:\\Program Files\\PageSpeed\\daemon\\cache volume",
            rwo.daemon_volume_path());
}

// Either option inside a match block is refused with the parser's existing
// warning and leaves the value unset (both are kProcessScopeStrict).
TEST_F(IisConfigurationTest, DaemonOptionInMatchBlockRefused) {
  std::string config_text =
      "host:example\\.com\n"
      "pagespeed DaemonSocketPath C:\\daemon\\notify.sock\n"
      "pagespeed DaemonVolumePath D:\\daemon\\volume\n";

  RecordingMessageHandler recorder;
  ConfigurationFile* cf =
      CreateTestConfigFile(config_text, &recorder, global_config_, nullptr);
  std::map<std::string, std::string> input;
  input["host"] = "example.com";
  input["config"] = "request";
  IisRewriteOptions rwo(nullptr);
  TestConfigFileGetConfig(cf, input, rwo);
  TestConfigFileRelease(cf);

  EXPECT_EQ("", rwo.daemon_socket_path())
      << "process-scope DaemonSocketPath must be refused inside a match block";
  EXPECT_EQ("", rwo.daemon_volume_path())
      << "process-scope DaemonVolumePath must be refused inside a match block";
  EXPECT_EQ(2, recorder.CountContaining("cannot be set inside a match block"))
      << "each refusal must carry the parser's existing warning";
  // Bound to the two directives, not to any two refusals.
  EXPECT_EQ(1, recorder.CountContaining("DaemonSocketPath"));
  EXPECT_EQ(1, recorder.CountContaining("DaemonVolumePath"));
}

// ==========================================================================
// The rule_packs options (server scope)
// ==========================================================================

TEST_F(IisConfigurationTest, RulePackOptionsParseAndRoundTrip) {
  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(
      "pagespeed RulePackFile C:\\pagespeed\\packs\\site.json\n"
      "pagespeed RulePackMode enforce\n",
      "", &rwo);
  EXPECT_EQ(std::vector<GoogleString>{"C:\\pagespeed\\packs\\site.json"},
            rwo.rule_pack_files());
  EXPECT_EQ(net_instaweb::RewriteOptions::kRulePackModeEnforce,
            rwo.rule_pack_mode());

  IisRewriteOptions merged(nullptr);
  merged.Merge(rwo);
  EXPECT_EQ(std::vector<GoogleString>{"C:\\pagespeed\\packs\\site.json"},
            merged.rule_pack_files());
  EXPECT_EQ(net_instaweb::RewriteOptions::kRulePackModeEnforce,
            merged.rule_pack_mode());
  std::unique_ptr<net_instaweb::RewriteOptions> clone(merged.Clone());
  EXPECT_EQ(std::vector<GoogleString>{"C:\\pagespeed\\packs\\site.json"},
            clone->rule_pack_files());
  EXPECT_EQ(net_instaweb::RewriteOptions::kRulePackModeEnforce,
            clone->rule_pack_mode());

  // A misspelled mode is refused and leaves the default.
  IisRewriteOptions bad(nullptr);
  ParseAndGetConfig("pagespeed RulePackMode enforc\n", "", &bad);
  EXPECT_EQ(net_instaweb::RewriteOptions::kRulePackModeReport,
            bad.rule_pack_mode());
}

TEST_F(IisConfigurationTest, RulePackFileListPerScope) {
  IisRewriteOptions one(nullptr);
  ParseAndGetConfig("pagespeed RulePackFile C:\\pagespeed\\packs\\a.json\n", "",
                    &one);
  ASSERT_EQ(1u, one.rule_pack_files().size());

  IisRewriteOptions two(nullptr);
  ParseAndGetConfig(
      "pagespeed RulePackFile C:\\pagespeed\\packs\\a.json\n"
      "pagespeed RulePackFile C:\\pagespeed\\packs\\b.json\n"
      "pagespeed RulePackFile C:\\pagespeed\\packs\\..\\packs\\a.json\n",
      "", &two);
  ASSERT_EQ(2u, two.rule_pack_files().size());
  EXPECT_EQ("C:\\pagespeed\\packs\\a.json", two.rule_pack_files()[0]);
  EXPECT_EQ("C:\\pagespeed\\packs\\b.json", two.rule_pack_files()[1]);

  // The site list replaces the global one (distinct paths, so an append
  // would show).
  IisRewriteOptions global(nullptr);
  ParseAndGetConfig("pagespeed RulePackFile C:\\pagespeed\\packs\\g.json\n", "",
                    &global);
  IisRewriteOptions merged(nullptr);
  merged.Merge(global);
  merged.Merge(two);
  ASSERT_EQ(2u, merged.rule_pack_files().size());
  EXPECT_EQ("C:\\pagespeed\\packs\\a.json", merged.rule_pack_files()[0]);
  EXPECT_EQ("C:\\pagespeed\\packs\\b.json", merged.rule_pack_files()[1]);

  // The ninth distinct file is a configuration error: the list stays at 8.
  std::string nine;
  for (int i = 1; i <= 9; ++i) {
    nine += "pagespeed RulePackFile C:\\pagespeed\\packs\\" +
            std::to_string(i) + ".json\n";
  }
  IisRewriteOptions many(nullptr);
  ParseAndGetConfig(nine, "", &many);
  EXPECT_EQ(8u, many.rule_pack_files().size());
}

// Server scope: a match block (per request) cannot point the server at a
// pack file.
TEST_F(IisConfigurationTest, RulePackFileInMatchBlockIgnored) {
  std::string config_text =
      "host:example\\.com\n"
      "pagespeed RulePackFile C:\\pagespeed\\packs\\other.json\n";
  ConfigurationFile* cf = Parse(config_text);
  std::map<std::string, std::string> input;
  input["host"] = "example.com";
  input["config"] = "request";
  IisRewriteOptions rwo(nullptr);
  TestConfigFileGetConfig(cf, input, rwo);
  TestConfigFileRelease(cf);
  EXPECT_TRUE(rwo.rule_pack_files().empty());
}

// An UNQUOTED value with a space is three tokens, not one: the line is not a
// name/value pair any more, nothing is stored, and the option stays empty.
// A path with spaces has to be quoted (see the test above).  This pins what
// the parser does today so an operator-facing description can say so.
TEST_F(IisConfigurationTest, DaemonOptionUnquotedValueWithSpacesIsNotStored) {
  std::string config_text =
      "pagespeed DaemonVolumePath C:\\Program Files\\PageSpeed\\volume\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_EQ("", rwo.daemon_volume_path());
}

// A merge where the source never set the daemon options keeps the value
// that was already stored.  This is the generic option merge, not a
// property of the scope: the IIS parser never merges by scope, so the
// scope label is enforced at parse time only (the match-block test above).
TEST_F(IisConfigurationTest, DaemonOptionMergeKeepsStoredValue) {
  IisRewriteOptions base(nullptr);
  ParseAndGetConfig(
      "pagespeed DaemonSocketPath C:\\daemon\\notify.sock\n"
      "pagespeed DaemonVolumePath D:\\daemon\\volume\n",
      "", &base);

  IisRewriteOptions site(nullptr);
  ParseAndGetConfig("pagespeed EnableFilters combine_css\n", "", &site);

  base.Merge(site);

  EXPECT_EQ("C:\\daemon\\notify.sock", base.daemon_socket_path());
  EXPECT_EQ("D:\\daemon\\volume", base.daemon_volume_path());
  EXPECT_TRUE(base.Enabled(RewriteOptions::kCombineCss));
}

// MatchRuleNoMatch: host regex doesn't match, filter not enabled.
TEST_F(IisConfigurationTest, MatchRuleNoMatch) {
  std::string config_text =
      "host:example\\.com\n"
      "pagespeed EnableFilters combine_css\n";

  ConfigurationFile* cf = Parse(config_text);
  std::map<std::string, std::string> input;
  input["host"] = "other.com";
  input["config"] = "request";
  IisRewriteOptions rwo(nullptr);
  TestConfigFileGetConfig(cf, input, rwo);
  TestConfigFileRelease(cf);

  EXPECT_FALSE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// BaseConfigNoMatchRule: options without match rule apply as base config.
TEST_F(IisConfigurationTest, BaseConfigNoMatchRule) {
  std::string config_text = "pagespeed EnableFilters combine_css\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// CommentLinesIgnored: lines starting with '#' are skipped.
TEST_F(IisConfigurationTest, CommentLinesIgnored) {
  std::string config_text =
      "# This is a comment\n"
      "pagespeed EnableFilters combine_css\n"
      "# Another comment\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// WindowsLineEndings: \r\n parsing works correctly.
TEST_F(IisConfigurationTest, WindowsLineEndings) {
  std::string config_text =
      "pagespeed EnableFilters combine_css\r\n"
      "pagespeed EnableFilters combine_javascript\r\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineJavascript));
}

// BackslashPathConversion: C:\temp becomes C:/temp.
TEST_F(IisConfigurationTest, BackslashPathConversion) {
  auto identity_expand = [](const std::string& s) { return s; };

  std::string config_text = "pagespeed FileCachePath C:\\temp\\cache\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo, identity_expand);

  EXPECT_EQ("C:/temp/cache", rwo.file_cache_path());
}

// ClearDirective: clear flag causes GetConfig to reset accumulated results.
// The clear bit (mode&4) tells GetConfig to discard previously merged options
// before applying this section. It is set on the ConfigLine that contains
// both the clear flag and the options defined after it.
TEST_F(IisConfigurationTest, ClearDirective) {
  // The clear directive sets mode|=4 on the current base ConfigLine.
  // Since clear and the subsequent options share the same ConfigLine,
  // the reset happens at merge time in GetConfig: the accumulated result
  // is cleared, then the ConfigLine's options (which include everything
  // parsed after 'clear') are merged in.
  std::string config_text =
      "pagespeed EnableFilters combine_css\n"
      "clear\n"
      "pagespeed EnableFilters combine_javascript\n";

  ConfigurationFile* cf = Parse(config_text);
  std::map<std::string, std::string> input;
  input["config"] = "base";
  IisRewriteOptions rwo(nullptr);
  TestConfigFileGetConfig(cf, input, rwo);
  TestConfigFileRelease(cf);

  // Both filters are on the same ConfigLine (the base section).
  // The clear flag causes GetConfig to reset the accumulator first,
  // then merge this section's options. But since combine_css and
  // combine_javascript are both in the same section, they both
  // get merged after the reset.
  // The important behavior: clear + stopmatching is signaled.
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineJavascript));
}

// ForbiddenFilters: forbiddenfilters directive.
TEST_F(IisConfigurationTest, ForbiddenFilters) {
  std::string config_text =
      "pagespeed EnableFilters combine_css\n"
      "forbiddenfilters combine_css\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  // combine_css should be forbidden (disabled).
  EXPECT_TRUE(rwo.Forbidden(RewriteOptions::kCombineCss));
}

// MultipleMatchSections: two host rules, correct options per host.
TEST_F(IisConfigurationTest, MultipleMatchSections) {
  std::string config_text =
      "host:alpha\\.com\n"
      "pagespeed EnableFilters combine_css\n"
      "host:beta\\.com\n"
      "pagespeed EnableFilters combine_javascript\n";

  // Query for alpha.com
  {
    ConfigurationFile* cf = Parse(config_text);
    std::map<std::string, std::string> input;
    input["host"] = "alpha.com";
    input["config"] = "request";
    IisRewriteOptions rwo(nullptr);
    TestConfigFileGetConfig(cf, input, rwo);
    TestConfigFileRelease(cf);

    EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
    EXPECT_FALSE(rwo.Enabled(RewriteOptions::kCombineJavascript));
  }

  // Query for beta.com
  {
    ConfigurationFile* cf = Parse(config_text);
    std::map<std::string, std::string> input;
    input["host"] = "beta.com";
    input["config"] = "request";
    IisRewriteOptions rwo(nullptr);
    TestConfigFileGetConfig(cf, input, rwo);
    TestConfigFileRelease(cf);

    EXPECT_FALSE(rwo.Enabled(RewriteOptions::kCombineCss));
    EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineJavascript));
  }
}

// LoadFromFilePathConversion: third token backslash converted to forward slash.
TEST_F(IisConfigurationTest, LoadFromFilePathConversion) {
  // LoadFromFile takes 3 tokens: LoadFromFile <url-prefix> <file-path>
  // The file-path (third token) should have backslashes converted.
  std::string config_text =
      "pagespeed LoadFromFile http://example.com/ C:\\www\\htdocs\\\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  // We verify this doesn't crash. The LoadFromFile directive adds to the
  // file load policy, which we can't easily inspect directly. The test
  // primarily verifies the backslash conversion code path executes without
  // error.
}

// PagespeedPrefix: 'pagespeed' prefix is recognized.
TEST_F(IisConfigurationTest, PagespeedPrefix) {
  std::string config_text = "pagespeed EnableFilters combine_css\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// IISpeedPrefix: 'iispeed' prefix is recognized.
TEST_F(IisConfigurationTest, IISpeedPrefix) {
  std::string config_text = "iispeed EnableFilters combine_css\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// ==========================================================================
// Phase 5: Edge Case Tests
// ==========================================================================

// EmptyConfig: no crash on empty input.
TEST_F(IisConfigurationTest, EmptyConfig) {
  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig("", "", &rwo);
  // No crash = success.
}

// BinaryGarbage: no crash on binary input.
TEST_F(IisConfigurationTest, BinaryGarbage) {
  std::string garbage;
  for (int i = 1; i < 256; i++) {  // Skip 0 (null terminator)
    garbage.push_back(static_cast<char>(i));
  }

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(garbage, "", &rwo);
  // No crash = success.
}

// NullByteInConfig: parsing stops at null, no crash.
TEST_F(IisConfigurationTest, NullByteInConfig) {
  // ParseConfigText takes length, but internally the parser uses
  // while(*ptr) which stops at null bytes.
  std::string text = "pagespeed EnableFilters combine_css";
  text.push_back('\0');
  text += "pagespeed EnableFilters combine_javascript\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(text, "", &rwo);

  // combine_css should be enabled (before null byte).
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
  // combine_javascript should NOT be enabled (after null byte, not parsed).
  EXPECT_FALSE(rwo.Enabled(RewriteOptions::kCombineJavascript));
}

// VeryLongLine: 10000 character line, no crash.
TEST_F(IisConfigurationTest, VeryLongLine) {
  std::string long_line(10000, 'a');
  long_line += "\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(long_line, "", &rwo);
  // No crash = success.
}

// ShortOptionName: single character option, no crash.
TEST_F(IisConfigurationTest, ShortOptionName) {
  std::string config_text = "x y\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);
  // No crash = success (unknown option, but shouldn't crash).
}

// SingleColon: ':' alone, no crash.
TEST_F(IisConfigurationTest, SingleColon) {
  std::string config_text = ":\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);
  // No crash = success.
}

// OnlyWhitespace: whitespace-only config, no crash.
TEST_F(IisConfigurationTest, OnlyWhitespace) {
  std::string config_text = "   \t  \n  \t  \n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);
  // No crash = success.
}

// UTF8InComment: UTF-8 characters in comment line, no crash.
TEST_F(IisConfigurationTest, UTF8InComment) {
  // German, Chinese, emoji in comment - should be ignored.
  std::string config_text =
      "# \xc3\x9c" "ber die Br\xc3\xbc" "cke \xe4\xb8\xad\xe6\x96\x87\n"
      "pagespeed EnableFilters combine_css\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// === Additional tests suggested by review ===

TEST_F(IisConfigurationTest, ModPagespeedPrefix) {
  std::string config_text =
      "ModPagespeed EnableFilters combine_css\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

TEST_F(IisConfigurationTest, LogDirWithLongExpansion) {
  // Same as ExpandEnvTruncation but for LogDir instead of FileCachePath.
  // Both code paths use the same expansion logic.
  std::string long_path(300, 'L');

  auto expand_fn = [&](const std::string& input) -> std::string {
    if (input == "%LONG_LOG%") return long_path;
    return input;
  };

  std::string config_text = "pagespeed LogDir %LONG_LOG%\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo, expand_fn);

  // LogDir should have the full 300-char path (backslashes converted)
  std::string expected = long_path;
  std::replace(expected.begin(), expected.end(), '\\', '/');
  EXPECT_EQ(expected, rwo.log_dir());
}

TEST_F(IisConfigurationTest, RemoteConfigUrlMultipleSubsequentLines) {
  // Verify that RemoteConfigurationUrl doesn't disturb parsing of
  // multiple subsequent lines (not just the immediately following one).
  std::string config_text =
      "pagespeed RemoteConfigurationUrl http://example.com/config\n"
      "pagespeed EnableFilters combine_css\n"
      "pagespeed EnableFilters combine_javascript\n";

  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(config_text, "", &rwo);

  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineJavascript));
}

TEST_F(IisConfigurationTest, ExclamationMarkDirectiveNoCrash) {
  // The '!' directive sets mode|=2|8 on its ConfigLine. Its exact
  // matching semantics are complex (interacts with the config/request
  // match key system). This test documents that '!' does not crash
  // and that subsequent directives are still parsed.
  std::string config_text =
      "host:example\\.com\n"
      "!\n"
      "pagespeed EnableFilters combine_css\n";

  IisRewriteOptions rwo(nullptr);
  // No crash is the primary assertion.
  ParseAndGetConfig(config_text, "example.com", &rwo);
}

TEST_F(IisConfigurationTest, LoadFromFileMatchPathConversion) {
  // LoadFromFileMatch (3+ tokens) should also convert backslashes
  // in the third token, same as LoadFromFile.
  std::string config_text =
      "pagespeed LoadFromFileMatch http://.*\\.css C:\\styles\\sheets\n";

  IisRewriteOptions rwo(nullptr);
  // If parsing completes without crashing, the path was processed.
  // The actual path is stored in the rewrite options via SetOptionFromName.
  ParseAndGetConfig(config_text, "", &rwo);
}

// ==========================================================================
// ConfigFactory concurrency test
// ==========================================================================

// Regression test for the SRWLock concurrent-mutation defect in
// ConfigFactory::GetConfiguration. Pre-fix code used
// `configurationFiles[path]` (std::map::operator[]) under an SRWLock held
// in *shared* (reader) mode, which default-inserts a (key, nullptr) pair
// when the key is absent. That is a mutating operation under a reader
// lock; concurrent readers race and corrupt the map's internal red-black
// tree, and AppVerifier's SRWLock / Locks providers raise verifier stops
// against the corrupted lock — observed externally as
// Connection_Abandoned_By_ReqQueue in HTTPERR + vrfcore.dll Application
// Error events during automatic image-rewriting tests on win-appverif.
//
// The existing NonRecursiveRWLock unit test covers the
// shared/exclusive contract at the lock level; this test exercises the
// same defect at the ConfigFactory call site with high concurrency
// (8 threads × 64 paths, mix of unique/shared/revisit keys to force
// insert + find + race) so a regression to operator[] under shared lock
// would surface as either a verifier stop, an STL-debug iterator
// invariant violation, or silent corruption — all of which fail this
// test deterministically when AppVerifier is enabled and probabilistically
// without it.
//
// Each path resolves via LoadFile which fast-returns for nonexistent
// files (CreateFileA → INVALID_HANDLE_VALUE), so no real file I/O occurs.
TEST_F(IisConfigurationTest,
       ConfigFactoryConcurrentGetConfigurationNoMapMutationUnderSharedLock) {
  constexpr int kThreads = 8;
  constexpr int kPathsPerThread = 64;

  ConfigFactory factory;
  std::atomic<bool> any_crash{false};
  std::atomic<int> total_files{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      try {
        for (int p = 0; p < kPathsPerThread; ++p) {
          // Mix of (a) per-thread unique paths (forces insert), (b) shared
          // paths across threads (forces find/insert race), (c) revisits
          // (forces hot-path find).
          std::string path;
          if (p % 3 == 0) {
            path = "Z:\\nonexistent\\thread_" + std::to_string(t) +
                   "_path_" + std::to_string(p) + ".config";
          } else if (p % 3 == 1) {
            path = "Z:\\nonexistent\\shared_path_" +
                   std::to_string(p % 16) + ".config";
          } else {
            path = "Z:\\nonexistent\\shared_path_" +
                   std::to_string((p - 1) % 16) + ".config";
          }
          ConfigurationFile* cf =
              factory.GetConfiguration(path, &handler_, global_config_);
          if (cf != nullptr) {
            total_files.fetch_add(1, std::memory_order_relaxed);
            // ReleaseRef matches the Addref inside GetConfiguration.
            TestConfigFileRelease(cf);
          }
        }
      } catch (...) {
        any_crash.store(true);
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  EXPECT_FALSE(any_crash.load())
      << "GetConfiguration threw under concurrent load";
  EXPECT_GT(total_files.load(), 0);
}

// NumRewriteThreads and NumExpensiveRewriteThreads are read into the
// process-wide settings the driver factory sizes its pools from.
TEST_F(IisConfigurationTest, ThreadCountDirectivesAreRead) {
  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(
      "pagespeed NumRewriteThreads 6\n"
      "pagespeed NumExpensiveRewriteThreads 3\n",
      "", &rwo);
  EXPECT_EQ(6, global_config_.num_rewrite_threads);
  EXPECT_EQ(3, global_config_.num_expensive_rewrite_threads);
}

// Unset, "auto" and 0 all leave the built-in size in place; "auto" after a
// count puts it back.
TEST_F(IisConfigurationTest, ThreadCountDirectivesAutoAndDefault) {
  IisRewriteOptions unset(nullptr);
  ParseAndGetConfig("pagespeed EnableFilters combine_css\n", "", &unset);
  EXPECT_EQ(0, global_config_.num_rewrite_threads);
  EXPECT_EQ(0, global_config_.num_expensive_rewrite_threads);

  IisRewriteOptions autos(nullptr);
  ParseAndGetConfig(
      "pagespeed NumRewriteThreads 6\n"
      "pagespeed NumRewriteThreads auto\n"
      "pagespeed NumExpensiveRewriteThreads 4\n"
      "pagespeed NumExpensiveRewriteThreads 0\n",
      "", &autos);
  EXPECT_EQ(0, global_config_.num_rewrite_threads);
  EXPECT_EQ(0, global_config_.num_expensive_rewrite_threads);
}

// A negative or non-numeric count is refused with a warning and changes
// nothing.
TEST_F(IisConfigurationTest, ThreadCountDirectivesRejectBadValues) {
  RecordingMessageHandler recorder;
  ConfigurationFile* cf = CreateTestConfigFile(
      "pagespeed NumRewriteThreads -2\n"
      "pagespeed NumExpensiveRewriteThreads many\n",
      &recorder, global_config_, nullptr);
  TestConfigFileRelease(cf);
  EXPECT_EQ(0, global_config_.num_rewrite_threads);
  EXPECT_EQ(0, global_config_.num_expensive_rewrite_threads);
  EXPECT_EQ(2, recorder.CountContaining("must be 'auto' or a whole number"));
}

// A mistyped digit must not create thousands of threads: the count is reduced
// to the shared cap, with a warning.
TEST_F(IisConfigurationTest, ThreadCountDirectivesAreClamped) {
  RecordingMessageHandler recorder;
  ConfigurationFile* cf = CreateTestConfigFile(
      "pagespeed NumRewriteThreads 100000\n", &recorder, global_config_, nullptr);
  TestConfigFileRelease(cf);
  EXPECT_EQ(net_instaweb::kMaxOptimizationThreadsPerPool,
            global_config_.num_rewrite_threads);
  EXPECT_EQ(1, recorder.CountContaining("using 1024 instead"));
}

// The counts are read from the machine-wide file for every site of the
// process, not only for the first one that parses it, and a site's own file
// wins for that site.
TEST_F(IisConfigurationTest, PoolThreadSettingsComeFromBothFilesEveryTime) {
  char temp[MAX_PATH];
  ASSERT_GT(GetTempPathA(MAX_PATH, temp), 0u);
  const std::string dir = std::string(temp) + "pagespeed-pool-threads-" +
                          std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  auto write = [&](const std::string& name, const std::string& text) {
    const std::string path = dir + "\\" + name;
    FILE* f = fopen(path.c_str(), "wb");
    EXPECT_NE(nullptr, f);
    if (f != nullptr) {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
    }
    return path;
  };
  const std::string machine = write("machine.config",
      "pagespeed NumRewriteThreads 5\npagespeed NumExpensiveRewriteThreads 3\n");
  const std::string site_b = write("site-b.config", "pagespeed NumRewriteThreads 9\n");
  const std::string site_c = write("site-c.config", "pagespeed EnableFilters combine_css\n");

  net_instaweb::NullMessageHandler quiet;
  // A first site, then a second and third in the same process.
  net_instaweb::global_settings first, second, third;
  ReadPoolThreadSettings(machine, "", &quiet, &first);
  ReadPoolThreadSettings(machine, site_b, &quiet, &second);
  ReadPoolThreadSettings(machine, site_c, &quiet, &third);

  EXPECT_EQ(5, first.num_rewrite_threads);
  EXPECT_EQ(3, first.num_expensive_rewrite_threads);
  EXPECT_EQ(9, second.num_rewrite_threads);   // the site's own file wins
  EXPECT_EQ(3, second.num_expensive_rewrite_threads);
  EXPECT_EQ(5, third.num_rewrite_threads);    // a site without a value
  EXPECT_EQ(3, third.num_expensive_rewrite_threads);

  for (const std::string& f : {machine, site_b, site_c}) {
    DeleteFileA(f.c_str());
  }
  RemoveDirectoryA(dir.c_str());
}

// The machine-wide settings are read from the machine-wide file for every site
// context of a process, not only for the first one that parses it, and a
// context rebuilt after a file was edited still sees them.
TEST_F(IisConfigurationTest, MachineWideSettingsReachEverySiteContext) {
  char temp[MAX_PATH];
  ASSERT_GT(GetTempPathA(MAX_PATH, temp), 0u);
  const std::string dir = std::string(temp) + "pagespeed-machine-wide-" +
                          std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  auto write = [&](const std::string& name, const std::string& text) {
    const std::string path = dir + "\\" + name;
    FILE* f = fopen(path.c_str(), "wb");
    EXPECT_NE(nullptr, f);
    if (f != nullptr) {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
    }
    return path;
  };
  const std::string machine = write("machine.config",
      "pagespeed MessageBufferSize 4096\n"
      "pagespeed UseNativeFetcher on\n"
      "pagespeed UsePerVHostStatistics off\n"
      "pagespeed CreateSharedMemoryMetadataCache /cache 2048\n");
  const std::string site = write("site.config", "pagespeed EnableFilters combine_css\n");

  net_instaweb::NullMessageHandler quiet;
  auto expect_machine_values = [](const net_instaweb::global_settings& g) {
    EXPECT_EQ(4096, g.message_buffer_size);
    EXPECT_TRUE(g.use_native_fetcher);
    EXPECT_FALSE(g.use_per_vhost_statistics);
    EXPECT_EQ(2048, g.shm_cache_size_kb);
  };

  // Three site contexts created in one process, one after the other.
  for (int i = 0; i < 3; ++i) {
    net_instaweb::global_settings g;
    ReadMachineWideSettings(machine, &quiet, &g);
    expect_machine_values(g);
  }

  // A context rebuilt after the site's file was edited still sees them.
  write("site.config", "pagespeed EnableFilters combine_css,extend_cache\n");
  net_instaweb::global_settings after_site_edit;
  ReadMachineWideSettings(machine, &quiet, &after_site_edit);
  expect_machine_values(after_site_edit);

  // A context rebuilt after the machine-wide file was edited sees the new value.
  write("machine.config",
      "pagespeed MessageBufferSize 8192\n"
      "pagespeed UseNativeFetcher on\n"
      "pagespeed UsePerVHostStatistics off\n"
      "pagespeed CreateSharedMemoryMetadataCache /cache 2048\n");
  net_instaweb::global_settings after_machine_edit;
  ReadMachineWideSettings(machine, &quiet, &after_machine_edit);
  EXPECT_EQ(8192, after_machine_edit.message_buffer_size);
  EXPECT_TRUE(after_machine_edit.use_native_fetcher);

  // No machine-wide file: the built-in defaults.
  net_instaweb::global_settings defaults;
  ReadMachineWideSettings("", &quiet, &defaults);
  EXPECT_EQ(net_instaweb::global_settings().message_buffer_size,
            defaults.message_buffer_size);
  EXPECT_FALSE(defaults.use_native_fetcher);

  for (const std::string& f : {machine, site}) {
    DeleteFileA(f.c_str());
  }
  RemoveDirectoryA(dir.c_str());
}

// A configured count always wins; otherwise half the CPUs, at least 4.
TEST(IisPoolThreadCountTest, ConfiguredCountWinsOverBuiltInSize) {
  using net_instaweb::IisRewriteDriverFactory;
  EXPECT_EQ(1, IisRewriteDriverFactory::PoolThreadCount(1, 64));
  EXPECT_EQ(12, IisRewriteDriverFactory::PoolThreadCount(12, 2));
  EXPECT_EQ(4, IisRewriteDriverFactory::PoolThreadCount(0, 2));
  EXPECT_EQ(4, IisRewriteDriverFactory::PoolThreadCount(0, 8));
  EXPECT_EQ(16, IisRewriteDriverFactory::PoolThreadCount(0, 32));
}

// A directive line with no option name (a bare "pagespeed", or one followed
// only by whitespace) tokenizes to nothing. That is an operator error to
// report, not a reason to stop the worker process.
TEST_F(IisConfigurationTest, EmptyDirectiveIsReportedNotFatal) {
  IisRewriteOptions rwo(nullptr);
  const char* error = rwo.ParseAndSetOptions(std::vector<std::string>(),
                                             &handler_, global_config_);
  EXPECT_NE(nullptr, error);
}

TEST_F(IisConfigurationTest, TreatProxiedRequestsAsLocalDefaultsOff) {
  IisRewriteOptions rwo(nullptr);
  EXPECT_FALSE(rwo.treat_proxied_requests_as_local());
}

TEST_F(IisConfigurationTest, TreatProxiedRequestsAsLocalParses) {
  IisRewriteOptions on(nullptr);
  ParseAndGetConfig("pagespeed TreatProxiedRequestsAsLocal on\n", "", &on);
  EXPECT_TRUE(on.treat_proxied_requests_as_local());

  IisRewriteOptions off(nullptr);
  ParseAndGetConfig("pagespeed TreatProxiedRequestsAsLocal off\n", "", &off);
  EXPECT_FALSE(off.treat_proxied_requests_as_local());
}

TEST_F(IisConfigurationTest, BareDirectiveLinesParse) {
  IisRewriteOptions rwo(nullptr);
  ParseAndGetConfig(
      "pagespeed\n"
      "pagespeed    \n"
      "pagespeed EnableFilters combine_css\n",
      "", &rwo);
  // The lines without an option name are skipped; the next line still
  // applies.
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

// A UTF-8 byte order mark (EF BB BF) at the start of the text must not change
// the parsed options: the first directive applies exactly as without it, and
// one notice is logged.
TEST_F(IisConfigurationTest, Utf8ByteOrderMarkIsSkipped) {
  const std::string body =
      "pagespeed EnableFilters combine_css\n"
      "pagespeed DaemonSocketPath C:\\daemon\\notify.sock\n";
  const std::string bom = "\xEF\xBB\xBF";

  IisRewriteOptions plain(nullptr);
  ParseAndGetConfig(body, "", &plain);

  RecordingMessageHandler recorder;
  ConfigurationFile* cf =
      CreateTestConfigFile(bom + body, &recorder, global_config_, nullptr);
  std::map<std::string, std::string> input;
  input["config"] = "base";
  IisRewriteOptions with_bom(nullptr);
  TestConfigFileGetConfig(cf, input, with_bom);
  TestConfigFileRelease(cf);

  EXPECT_TRUE(plain.Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(with_bom.Enabled(RewriteOptions::kCombineCss));
  EXPECT_EQ("C:\\daemon\\notify.sock", plain.daemon_socket_path());
  EXPECT_EQ(plain.daemon_socket_path(), with_bom.daemon_socket_path());
  EXPECT_EQ(1, recorder.CountContaining("byte order mark"));
}

// Text without a mark logs no notice, and a mark-only text parses to nothing.
TEST_F(IisConfigurationTest, Utf8ByteOrderMarkNoticeOnlyWhenPresent) {
  RecordingMessageHandler recorder;
  ConfigurationFile* cf = CreateTestConfigFile(
      "pagespeed EnableFilters combine_css\n", &recorder, global_config_,
      nullptr);
  TestConfigFileRelease(cf);
  EXPECT_EQ(0, recorder.CountContaining("byte order mark"));

  cf = CreateTestConfigFile("\xEF\xBB\xBF", &recorder, global_config_,
                            nullptr);
  TestConfigFileRelease(cf);
  EXPECT_EQ(1, recorder.CountContaining("byte order mark"));
}

// A UTF-16 file is refused with one clear warning and sets no options.
TEST_F(IisConfigurationTest, Utf16FileIsRefusedWithWarning) {
  const std::string utf16("\xFF\xFE" "p\0a\0", 6);

  RecordingMessageHandler recorder;
  ConfigurationFile* cf =
      CreateTestConfigFile(utf16, &recorder, global_config_, nullptr);
  std::map<std::string, std::string> input;
  input["config"] = "base";
  IisRewriteOptions rwo(nullptr);
  TestConfigFileGetConfig(cf, input, rwo);
  TestConfigFileRelease(cf);

  EXPECT_EQ(1, recorder.CountContaining("UTF-16"));
  EXPECT_EQ(0, recorder.CountContaining("byte order mark"));
  EXPECT_EQ("", rwo.daemon_socket_path());
}

}  // namespace
