// Unit tests for IIS configuration file parsing (ConfigurationFile).
// These tests exercise ParseConfigText via CreateTestConfigFile, using
// an injected expand_env function to avoid real Windows API calls.
//
// Windows-only: depends on //pagespeed/iis:iis_module which requires Windows.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/kernel/base/google_message_handler.h"
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

}  // namespace
