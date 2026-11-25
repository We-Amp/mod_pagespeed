// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Shared Config File Tests

#include "src/worker/shared_config.h"

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "volume.hpp"

namespace pagespeed {
namespace {

class SharedConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() /
               ("shared_config_test_" + std::to_string(
#ifdef _WIN32
                                            _getpid()
#else
                                            getpid()
#endif
                                                ));
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

  // Helper: write raw content to a file and return the path.
  std::string WriteRawFile(const std::string& name,
                           const std::string& content) {
    auto path = (tmp_dir_ / name).string();
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return path;
  }

  std::filesystem::path tmp_dir_;
};

// ---------------------------------------------------------------------------
// Parsing tests
// ---------------------------------------------------------------------------

TEST_F(SharedConfigTest, ParseEmptyContent) {
  auto config = ParseSharedConfig("");
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
  EXPECT_EQ(config.html_mode, HtmlMode::kFull);
}

TEST_F(SharedConfigTest, ParseValidContent) {
  auto config = ParseSharedConfig(
      "version=1\n"
      "pid=12345\n"
      "socket_path=/run/pagespeed/worker.sock\n"
      "disable_html=true\n");
  EXPECT_EQ(config.socket_path, "/run/pagespeed/worker.sock");
  EXPECT_EQ(config.html_mode, HtmlMode::kOff);
}

TEST_F(SharedConfigTest, ParseAgentOptimizeLlmsTxtEnabled) {
  EXPECT_FALSE(ParseSharedConfig("").agent_optimize_llms_txt_enabled);
  EXPECT_TRUE(ParseSharedConfig("agent_optimize_llms_txt_enabled=true\n")
                  .agent_optimize_llms_txt_enabled);
  EXPECT_TRUE(ParseSharedConfig("agent_optimize_llms_txt_enabled=1\n")
                  .agent_optimize_llms_txt_enabled);
  EXPECT_FALSE(ParseSharedConfig("agent_optimize_llms_txt_enabled=false\n")
                   .agent_optimize_llms_txt_enabled);
}

TEST_F(SharedConfigTest, WriteReadRoundTripCarriesLlmsTxtFlag) {
  // Locks the write/parse lockstep for the llms.txt serve flag.
  SharedConfig in;
  in.agent_optimize_entitled = true;
  in.agent_optimize_llms_txt_enabled = true;
  auto path = (tmp_dir_ / "shared.conf").string();
  ASSERT_TRUE(WriteSharedConfigFile(path, in));
  SharedConfig out = ReadSharedConfigFile(path);
  EXPECT_TRUE(out.agent_optimize_entitled);
  EXPECT_TRUE(out.agent_optimize_llms_txt_enabled);
}

// --- the published volume sizing -------------------------------------------
//
// The worker publishes the size it opened its cache volume with so a peer can
// INHERIT it.  It matters that "not stated" stays distinguishable from a real
// size: the documented contract is that a consumer seeing 0 declines to open
// rather than substituting a default, and a default is precisely what creates
// a second, permanently cold volume beside the worker's.

TEST_F(SharedConfigTest, VolumeSizeDefaultsToNotStated) {
  EXPECT_EQ(ParseSharedConfig("").volume_size, 0u);
}

TEST_F(SharedConfigTest, VolumeSizeParses) {
  EXPECT_EQ(ParseSharedConfig("volume_size=2147483648\n").volume_size,
            2147483648ull);
}

TEST_F(SharedConfigTest, MalformedVolumeSizeReadsAsNotStated) {
  // Degrades into the existing safe state rather than into a wrong number.
  EXPECT_EQ(ParseSharedConfig("volume_size=\n").volume_size, 0u);
  EXPECT_EQ(ParseSharedConfig("volume_size=abc\n").volume_size, 0u);
  EXPECT_EQ(ParseSharedConfig("volume_size=64M\n").volume_size, 0u);
  EXPECT_EQ(ParseSharedConfig("volume_size=-1\n").volume_size, 0u);
}

TEST_F(SharedConfigTest, WriteReadRoundTripCarriesVolumeSize) {
  SharedConfig in;
  in.volume_size = 64ull * 1024 * 1024;
  auto path = (tmp_dir_ / "shared.conf").string();
  ASSERT_TRUE(WriteSharedConfigFile(path, in));
  EXPECT_EQ(ReadSharedConfigFile(path).volume_size, in.volume_size);
}

TEST_F(SharedConfigTest, UnknownVolumeSizeIsNotWritten) {
  // Writing volume_size=0 would tell a reader "the volume is zero bytes",
  // which it cannot tell apart from "the writer did not know".
  SharedConfig in;
  in.volume_size = 0;
  auto path = (tmp_dir_ / "shared.conf").string();
  ASSERT_TRUE(WriteSharedConfigFile(path, in));
  std::ifstream f(path);
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content.find("volume_size="), std::string::npos) << content;
}

TEST_F(SharedConfigTest, AddingVolumeSizeDidNotBumpTheSchemaVersion) {
  // The gate is a reader-side "I cannot read this AT ALL": a reader seeing a
  // higher version discards the whole file and runs on compiled-in defaults.
  // An optional key does not need that — unknown keys are ignored by contract
  // — so a bump here would degrade every already-deployed reader in exchange
  // for announcing a key none of them consult.  This pins the decision so a
  // later edit has to argue with it rather than drift past it.
  EXPECT_EQ(kSharedConfigVersion, 1);

  // ... and the contract that makes it safe: a reader that does not know the
  // key still gets every other field.
  SharedConfig out = ParseSharedConfig(
      "version=1\n"
      "some_key_from_the_future=1\n"
      "volume_size=123\n"
      "socket_path=/tmp/x.sock\n");
  EXPECT_EQ(out.socket_path, "/tmp/x.sock");
  EXPECT_EQ(out.volume_size, 123u);
}

TEST_F(SharedConfigTest, ParseWebBotAuthFields) {
  // Default OFF/empty; parse accepts true/1.
  SharedConfig defaults = ParseSharedConfig("");
  EXPECT_FALSE(defaults.web_bot_auth);
  EXPECT_TRUE(defaults.web_bot_auth_verified_bots.empty());
  EXPECT_TRUE(defaults.web_bot_auth_directory_hosts.empty());

  SharedConfig on = ParseSharedConfig(
      "web_bot_auth=true\n"
      "web_bot_auth_verified_bots=kid1=crawler,kid2=indexer\n"
      "web_bot_auth_directory_hosts=keys.example.com,dir.example.org\n");
  EXPECT_TRUE(on.web_bot_auth);
  EXPECT_EQ(on.web_bot_auth_verified_bots, "kid1=crawler,kid2=indexer");
  EXPECT_EQ(on.web_bot_auth_directory_hosts,
            "keys.example.com,dir.example.org");
  EXPECT_FALSE(ParseSharedConfig("web_bot_auth=false\n").web_bot_auth);
}

TEST_F(SharedConfigTest, WriteReadRoundTripCarriesWebBotAuthFields) {
  SharedConfig in;
  in.web_bot_auth = true;
  in.web_bot_auth_verified_bots = "kid1=crawler";
  in.web_bot_auth_directory_hosts = "keys.example.com";
  auto path = (tmp_dir_ / "shared.conf").string();
  ASSERT_TRUE(WriteSharedConfigFile(path, in));
  SharedConfig out = ReadSharedConfigFile(path);
  EXPECT_TRUE(out.web_bot_auth);
  EXPECT_EQ(out.web_bot_auth_verified_bots, "kid1=crawler");
  EXPECT_EQ(out.web_bot_auth_directory_hosts, "keys.example.com");
}

TEST_F(SharedConfigTest, WriteRejectsNewlineInWebBotAuthValues) {
  SharedConfig in;
  in.web_bot_auth_verified_bots = "kid=name\ninjected=true";
  auto path = (tmp_dir_ / "shared.conf").string();
  EXPECT_FALSE(WriteSharedConfigFile(path, in));
}

// Opt-in counter mode: defaults off, round-trips valid modes,
// rejects unknown values (keeping default), rejects a newline-injected value.
TEST_F(SharedConfigTest, ParseWebBotAuthPublicCounter) {
  EXPECT_EQ(ParseSharedConfig("").web_bot_auth_public_counter, "off");
  EXPECT_EQ(ParseSharedConfig("web_bot_auth_public_counter=public\n")
                .web_bot_auth_public_counter,
            "public");
  EXPECT_EQ(ParseSharedConfig("web_bot_auth_public_counter=private\n")
                .web_bot_auth_public_counter,
            "private");
  // Unknown value keeps the default.
  EXPECT_EQ(ParseSharedConfig("web_bot_auth_public_counter=bogus\n")
                .web_bot_auth_public_counter,
            "off");
}

TEST_F(SharedConfigTest, WriteReadRoundTripCarriesPublicCounter) {
  SharedConfig in;
  in.web_bot_auth_public_counter = "public";
  auto path = (tmp_dir_ / "shared.conf").string();
  ASSERT_TRUE(WriteSharedConfigFile(path, in));
  SharedConfig out = ReadSharedConfigFile(path);
  EXPECT_EQ(out.web_bot_auth_public_counter, "public");
}

TEST_F(SharedConfigTest, WriteRejectsNewlineInPublicCounter) {
  SharedConfig in;
  in.web_bot_auth_public_counter = "public\ninjected=true";
  auto path = (tmp_dir_ / "shared.conf").string();
  EXPECT_FALSE(WriteSharedConfigFile(path, in));
}

TEST_F(SharedConfigTest, ParseRslCapFields) {
  // Experimental: default OFF/empty; parse accepts true/1.
  SharedConfig defaults = ParseSharedConfig("");
  EXPECT_FALSE(defaults.rsl_cap_enforcement);
  EXPECT_TRUE(defaults.rsl_cap_directory_hosts.empty());
  EXPECT_TRUE(defaults.rsl_cap_requested_license.empty());
  EXPECT_TRUE(defaults.rsl_cap_requested_scope.empty());
  EXPECT_TRUE(defaults.rsl_cap_issuer.empty());

  SharedConfig on = ParseSharedConfig(
      "rsl_cap_enforcement=true\n"
      "rsl_cap_directory_hosts=keys.example.com,dir.example.org\n"
      "rsl_cap_requested_license=premium\n"
      "rsl_cap_requested_scope=render\n"
      "rsl_cap_issuer=issuer.example\n");
  EXPECT_TRUE(on.rsl_cap_enforcement);
  EXPECT_EQ(on.rsl_cap_directory_hosts, "keys.example.com,dir.example.org");
  EXPECT_EQ(on.rsl_cap_requested_license, "premium");
  EXPECT_EQ(on.rsl_cap_requested_scope, "render");
  EXPECT_EQ(on.rsl_cap_issuer, "issuer.example");
  EXPECT_TRUE(ParseSharedConfig("rsl_cap_enforcement=1\n").rsl_cap_enforcement);
  EXPECT_FALSE(
      ParseSharedConfig("rsl_cap_enforcement=false\n").rsl_cap_enforcement);
}

TEST_F(SharedConfigTest, WriteReadRoundTripCarriesRslCapFields) {
  // Locks the write/parse lockstep the HandleConfigPatch read-modify-write
  // depends on: an unrelated runtime PATCH re-reads this file, so a field
  // that does not round-trip would be silently clobbered to defaults.
  SharedConfig in;
  in.rsl_cap_enforcement = true;
  in.rsl_cap_directory_hosts = "keys.example.com";
  in.rsl_cap_requested_license = "premium";
  in.rsl_cap_requested_scope = "render";
  in.rsl_cap_issuer = "issuer.example";
  auto path = (tmp_dir_ / "shared.conf").string();
  ASSERT_TRUE(WriteSharedConfigFile(path, in));
  SharedConfig out = ReadSharedConfigFile(path);
  EXPECT_TRUE(out.rsl_cap_enforcement);
  EXPECT_EQ(out.rsl_cap_directory_hosts, "keys.example.com");
  EXPECT_EQ(out.rsl_cap_requested_license, "premium");
  EXPECT_EQ(out.rsl_cap_requested_scope, "render");
  EXPECT_EQ(out.rsl_cap_issuer, "issuer.example");
}

TEST_F(SharedConfigTest, WriteRejectsNewlineInRslCapValues) {
  // Every rsl_cap string field must reject config-line injection.
  auto path = (tmp_dir_ / "shared.conf").string();
  {
    SharedConfig in;
    in.rsl_cap_directory_hosts = "keys.example.com\ninjected=true";
    EXPECT_FALSE(WriteSharedConfigFile(path, in));
  }
  {
    SharedConfig in;
    in.rsl_cap_requested_license = "premium\ninjected=true";
    EXPECT_FALSE(WriteSharedConfigFile(path, in));
  }
  {
    SharedConfig in;
    in.rsl_cap_requested_scope = "render\rinjected=true";
    EXPECT_FALSE(WriteSharedConfigFile(path, in));
  }
  {
    SharedConfig in;
    in.rsl_cap_issuer = "issuer.example\ninjected=true";
    EXPECT_FALSE(WriteSharedConfigFile(path, in));
  }
}

TEST_F(SharedConfigTest, ParseCommentsSkipped) {
  auto config = ParseSharedConfig(
      "# This is a comment\n"
      "socket_path=/custom/path.sock\n"
      "# Another comment\n");
  EXPECT_EQ(config.socket_path, "/custom/path.sock");
}

TEST_F(SharedConfigTest, ParseEmptyLinesSkipped) {
  auto config = ParseSharedConfig(
      "\n"
      "socket_path=/custom/path.sock\n"
      "\n"
      "disable_html=true\n"
      "\n");
  EXPECT_EQ(config.socket_path, "/custom/path.sock");
  EXPECT_EQ(config.html_mode, HtmlMode::kOff);
}

TEST_F(SharedConfigTest, ParseNoEqualsSign) {
  // Lines without '=' are silently skipped; other fields still parse.
  auto config = ParseSharedConfig(
      "this line has no equals sign\n"
      "socket_path=/good.sock\n"
      "another bad line\n");
  EXPECT_EQ(config.socket_path, "/good.sock");
}

TEST_F(SharedConfigTest, ParseUnknownKeys) {
  auto config = ParseSharedConfig(
      "foo=bar\n"
      "socket_path=/known.sock\n"
      "baz=quux\n");
  EXPECT_EQ(config.socket_path, "/known.sock");
  // Unknown keys don't corrupt defaults.
  EXPECT_EQ(config.html_mode, HtmlMode::kFull);
}

TEST_F(SharedConfigTest, ParseVersionOne) {
  auto config = ParseSharedConfig(
      "version=1\n"
      "socket_path=/v1.sock\n");
  EXPECT_EQ(config.socket_path, "/v1.sock");
}

TEST_F(SharedConfigTest, ParseVersionTwo) {
  // version >= 2 is a future format; parser returns defaults.
  auto config = ParseSharedConfig(
      "version=2\n"
      "socket_path=/v2.sock\n"
      "cache_mode=aggressive\n");
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
  EXPECT_TRUE(config.cache_mode.empty());
  EXPECT_EQ(config.html_mode, HtmlMode::kFull);
}

TEST_F(SharedConfigTest, ParseVersionZero) {
  auto config = ParseSharedConfig(
      "version=0\n"
      "socket_path=/v0.sock\n");
  EXPECT_EQ(config.socket_path, "/v0.sock");
}

TEST_F(SharedConfigTest, ParseMissingVersion) {
  // No version line; other fields still parse normally.
  auto config = ParseSharedConfig(
      "socket_path=/no-version.sock\n"
      "disable_html=true\n");
  EXPECT_EQ(config.socket_path, "/no-version.sock");
  EXPECT_EQ(config.html_mode, HtmlMode::kOff);
}

TEST_F(SharedConfigTest, ParseEmptyValue) {
  auto config = ParseSharedConfig("strip_query_params=\n");
  EXPECT_EQ(config.strip_query_params, "");
}

TEST_F(SharedConfigTest, ParseEmptySocketPathKeepsDefault) {
  // Empty socket_path= should keep the default rather than setting empty.
  auto config = ParseSharedConfig("socket_path=\n");
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
}

TEST_F(SharedConfigTest, ParseValueWithEquals) {
  // Values may contain '=' characters (only the first one splits).
  auto config = ParseSharedConfig("strip_query_params=abc=def==\n");
  EXPECT_EQ(config.strip_query_params, "abc=def==");
}

TEST_F(SharedConfigTest, ParseDisableHtmlTrue) {
  {
    auto config = ParseSharedConfig("disable_html=true\n");
    EXPECT_EQ(config.html_mode, HtmlMode::kOff);
  }
  {
    auto config = ParseSharedConfig("disable_html=1\n");
    EXPECT_EQ(config.html_mode, HtmlMode::kOff);
  }
}

TEST_F(SharedConfigTest, ParseDisableHtmlFalse) {
  {
    auto config = ParseSharedConfig("disable_html=false\n");
    EXPECT_EQ(config.html_mode, HtmlMode::kFull);
  }
  {
    auto config = ParseSharedConfig("disable_html=0\n");
    EXPECT_EQ(config.html_mode, HtmlMode::kFull);
  }
  {
    auto config = ParseSharedConfig("disable_html=\n");
    EXPECT_EQ(config.html_mode, HtmlMode::kFull);
  }
}

// ---------------------------------------------------------------------------
// html_mode and the legacy disable_html key
// ---------------------------------------------------------------------------

TEST_F(SharedConfigTest, ParseHtmlModeValues) {
  EXPECT_EQ(ParseSharedConfig("html_mode=off\n").html_mode, HtmlMode::kOff);
  EXPECT_EQ(ParseSharedConfig("html_mode=markdown-static\n").html_mode,
            HtmlMode::kMarkdownStatic);
  EXPECT_EQ(ParseSharedConfig("html_mode=markdown-rendered\n").html_mode,
            HtmlMode::kMarkdownRendered);
  EXPECT_EQ(ParseSharedConfig("html_mode=full\n").html_mode, HtmlMode::kFull);
  EXPECT_EQ(ParseSharedConfig("html_mode=off\r\n").html_mode, HtmlMode::kOff);
}

TEST_F(SharedConfigTest, NoHtmlKeyIsFull) {
  // A file without either key (an older writer, or none at all) is full,
  // which is the behaviour before html_mode existed.
  EXPECT_EQ(ParseSharedConfig("version=1\nsocket_path=/s.sock\n").html_mode,
            HtmlMode::kFull);
  EXPECT_EQ(SharedConfig{}.html_mode, HtmlMode::kFull);
}

TEST_F(SharedConfigTest, HtmlModeWinsOverDisableHtmlInEitherOrder) {
  EXPECT_EQ(ParseSharedConfig("html_mode=markdown-static\ndisable_html=true\n")
                .html_mode,
            HtmlMode::kMarkdownStatic);
  EXPECT_EQ(ParseSharedConfig("disable_html=true\nhtml_mode=markdown-static\n")
                .html_mode,
            HtmlMode::kMarkdownStatic);
  EXPECT_EQ(ParseSharedConfig("disable_html=true\nhtml_mode=full\n").html_mode,
            HtmlMode::kFull);
  EXPECT_EQ(ParseSharedConfig("html_mode=off\ndisable_html=false\n").html_mode,
            HtmlMode::kOff);
}

TEST_F(SharedConfigTest, UnknownHtmlModeFallsBackToDisableHtml) {
  // A value this build does not know (a newer writer's) is not "given": the
  // disable_html that writer also emits decides.
  EXPECT_EQ(ParseSharedConfig("html_mode=markdown-future\ndisable_html=true\n")
                .html_mode,
            HtmlMode::kOff);
  EXPECT_EQ(ParseSharedConfig("html_mode=markdown-future\ndisable_html=false\n")
                .html_mode,
            HtmlMode::kFull);
  EXPECT_EQ(ParseSharedConfig("html_mode=Full\n").html_mode, HtmlMode::kFull);
  EXPECT_EQ(ParseSharedConfig("html_mode=\ndisable_html=1\n").html_mode,
            HtmlMode::kOff);
}

TEST_F(SharedConfigTest, WriterEmitsHtmlModeAndDerivedDisableHtml) {
  struct Case {
    HtmlMode mode;
    const char* html_mode_line;
    const char* disable_html_line;
  };
  const Case cases[] = {
      {HtmlMode::kOff, "\nhtml_mode=off\n", "\ndisable_html=true\n"},
      {HtmlMode::kMarkdownStatic, "\nhtml_mode=markdown-static\n",
       "\ndisable_html=false\n"},
      {HtmlMode::kMarkdownRendered, "\nhtml_mode=markdown-rendered\n",
       "\ndisable_html=false\n"},
      {HtmlMode::kFull, "\nhtml_mode=full\n", "\ndisable_html=false\n"},
  };
  for (const Case& c : cases) {
    std::string path = (tmp_dir_ / "mode.conf").string();
    SharedConfig config;
    config.html_mode = c.mode;
    ASSERT_TRUE(WriteSharedConfigFile(path, config, 1));
    std::ifstream f(path);
    std::string raw((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    EXPECT_NE(raw.find(c.html_mode_line), std::string::npos) << raw;
    EXPECT_NE(raw.find(c.disable_html_line), std::string::npos) << raw;
    // No schema bump: html_mode is an optional key.
    EXPECT_NE(raw.find("\nversion=1\n"), std::string::npos) << raw;
    EXPECT_EQ(ReadSharedConfigFile(path).html_mode, c.mode);
  }
}

TEST_F(SharedConfigTest, ReaderWithoutHtmlModeKeepsWorkingOnNewFile) {
  // A reader that predates html_mode ignores the key (unknown keys are
  // skipped by contract) and acts on disable_html alone.  Model it by
  // dropping the html_mode line from a new file: off stays off, and every
  // other mode reads as full, which is what that reader did before.
  for (HtmlMode mode : {HtmlMode::kOff, HtmlMode::kMarkdownStatic,
                        HtmlMode::kMarkdownRendered, HtmlMode::kFull}) {
    std::string path = (tmp_dir_ / "old-reader.conf").string();
    SharedConfig config;
    config.socket_path = "/run/old-reader.sock";
    config.html_mode = mode;
    config.agent_optimize_entitled = true;
    ASSERT_TRUE(WriteSharedConfigFile(path, config, 1));
    std::ifstream f(path);
    std::string raw((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    const std::string line =
        "html_mode=" + std::string(HtmlModeName(mode)) + "\n";
    const size_t at = raw.find("\n" + line);
    ASSERT_NE(at, std::string::npos) << raw;
    raw.erase(at + 1, line.size());
    ASSERT_EQ(raw.find("html_mode="), std::string::npos) << raw;
    ResetSharedConfigVersionSkewForTesting();
    const SharedConfig old_view = ParseSharedConfig(raw);
    EXPECT_FALSE(SharedConfigVersionSkewState().mismatch);
    EXPECT_EQ(old_view.html_mode,
              mode == HtmlMode::kOff ? HtmlMode::kOff : HtmlMode::kFull)
        << HtmlModeName(mode);
    // The rest of the file is read unchanged.
    EXPECT_EQ(old_view.socket_path, "/run/old-reader.sock");
    EXPECT_TRUE(old_view.agent_optimize_entitled);
  }
}

TEST_F(SharedConfigTest, ParseTrailingCarriageReturn) {
  // Windows-style line endings: \r should be stripped from values.
  auto config = ParseSharedConfig("socket_path=/foo\r\n");
  EXPECT_EQ(config.socket_path, "/foo");
}

TEST_F(SharedConfigTest, ParseNoTrailingNewline) {
  // File without final \n: last line should still parse.
  auto config = ParseSharedConfig("socket_path=/no-newline.sock");
  EXPECT_EQ(config.socket_path, "/no-newline.sock");
}

// ---------------------------------------------------------------------------
// File I/O tests
// ---------------------------------------------------------------------------

TEST_F(SharedConfigTest, WriteAndReadRoundTrip) {
  std::string path = (tmp_dir_ / "pagespeed-shared.conf").string();
  SharedConfig original;
  original.socket_path = "/run/test.sock";
  original.html_mode = HtmlMode::kOff;
  original.agent_optimize_entitled = true;
  original.cache_mode = "aggressive";

  ASSERT_TRUE(WriteSharedConfigFile(path, original, 42));

  auto loaded = ReadSharedConfigFile(path);
  EXPECT_EQ(loaded.socket_path, original.socket_path);
  EXPECT_EQ(loaded.html_mode, original.html_mode);
  EXPECT_EQ(loaded.agent_optimize_entitled, original.agent_optimize_entitled);
  EXPECT_EQ(loaded.cache_mode, original.cache_mode);
  EXPECT_EQ(loaded, original);
}

TEST_F(SharedConfigTest, WriteCreatesFile) {
  std::string path = (tmp_dir_ / "new-file.conf").string();
  EXPECT_FALSE(std::filesystem::exists(path));

  SharedConfig config;
  ASSERT_TRUE(WriteSharedConfigFile(path, config, 1));
  EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(SharedConfigTest, WriteOverwritesFile) {
  std::string path = (tmp_dir_ / "overwrite.conf").string();

  SharedConfig first;
  first.socket_path = "/first.sock";
  first.cache_mode = "safe";
  ASSERT_TRUE(WriteSharedConfigFile(path, first, 1));

  SharedConfig second;
  second.socket_path = "/second.sock";
  second.cache_mode = "aggressive";
  ASSERT_TRUE(WriteSharedConfigFile(path, second, 2));

  auto loaded = ReadSharedConfigFile(path);
  EXPECT_EQ(loaded.socket_path, "/second.sock");
  EXPECT_EQ(loaded.cache_mode, "aggressive");
}

TEST_F(SharedConfigTest, WriteFilePermissions) {
  std::string path = (tmp_dir_ / "perms.conf").string();
  SharedConfig config;
  ASSERT_TRUE(WriteSharedConfigFile(path, config, 1));

#ifndef _WIN32
  // Unix file permission checks — not applicable on Windows (no mode_t 0640).
  struct stat st;
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  // 0640 = owner (daemon) rw, group (web-server peers) r, others nothing.
  // Was 0644 before the H1-H3 privilege drop.
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0640));
#endif
}

#ifndef _WIN32
TEST_F(SharedConfigTest, WriteFilePermissionsUmaskIndependent) {
  // The mode must be exact even under a restrictive umask — correctness
  // never rides on the process umask (explicit fchmod in AtomicWriteFile).
  mode_t old_umask = ::umask(0077);
  std::string path = (tmp_dir_ / "umask.conf").string();
  SharedConfig config;
  ASSERT_TRUE(WriteSharedConfigFile(path, config, 1));
  ::umask(old_umask);

  struct stat st;
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0640));
}
#endif

TEST_F(SharedConfigTest, CacheDirGenerationRoundTrip) {
  // Default: not stated (0) — a writer predating the field.
  auto parsed = ParseSharedConfig("socket_path=/x.sock\n");
  EXPECT_EQ(parsed.cache_dir_generation, 0);

  // Parsed when present, including a generation other than this build's:
  // the reader reports what the writer stated and leaves the verdict to the
  // peer comparing it with its own.
  parsed = ParseSharedConfig("cache_dir_generation=1\n");
  EXPECT_EQ(parsed.cache_dir_generation, 1);
  parsed = ParseSharedConfig(
      "cache_dir_generation=" + std::to_string(kCacheDirGeneration) + "\n");
  EXPECT_EQ(parsed.cache_dir_generation, kCacheDirGeneration);

  // Emitted by the writer when set.

  std::string path = (tmp_dir_ / "gen.conf").string();
  SharedConfig config;
  config.cache_dir_generation = kCacheDirGeneration;
  ASSERT_TRUE(WriteSharedConfigFile(path, config, 1));
  auto loaded = ReadSharedConfigFile(path);
  EXPECT_EQ(loaded.cache_dir_generation, kCacheDirGeneration);

  // A generation of 0 is never emitted (0 must stay distinguishable as
  // "writer did not state a generation").
  std::string path0 = (tmp_dir_ / "gen0.conf").string();
  SharedConfig unset;
  ASSERT_TRUE(WriteSharedConfigFile(path0, unset, 1));
  std::ifstream f(path0);
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content.find("cache_dir_generation"), std::string::npos);

  // Garbage values degrade to 0, "not stated".
  EXPECT_EQ(
      ParseSharedConfig("cache_dir_generation=abc\n").cache_dir_generation, 0);
  EXPECT_EQ(ParseSharedConfig("cache_dir_generation=-2\n").cache_dir_generation,
            0);
}

// Tripwire for shared_config.h's bump rule 2: a storage format major that
// moves after a generation has shipped takes the generation with it.  When
// this fails, the Cyclone pin moved the format major: decide the generation
// (bump it, or record why not in shared_config.h), then update both numbers
// here together.
TEST(CacheDirGeneration, TracksTheStorageFormatMajor) {
  EXPECT_EQ(cyclone::VolumeHeader::kFormatVersionMajor, 9);
  EXPECT_EQ(kCacheDirGeneration, 3);
}

TEST_F(SharedConfigTest, WritePidField) {
  std::string path = (tmp_dir_ / "pid.conf").string();
  SharedConfig config;
  ASSERT_TRUE(WriteSharedConfigFile(path, config, 9999));

  std::ifstream f(path);
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("pid=9999"), std::string::npos);
}

TEST_F(SharedConfigTest, WriteDefaultPid) {
  std::string path = (tmp_dir_ / "default-pid.conf").string();
  SharedConfig config;
  // pid=0 triggers getpid() inside WriteSharedConfigFile.
  ASSERT_TRUE(WriteSharedConfigFile(path, config, 0));

  std::ifstream f(path);
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

  std::string expected_pid = "pid=" + std::to_string(
#ifdef _WIN32
                                          _getpid()
#else
                                          getpid()
#endif
                                      );
  EXPECT_NE(content.find(expected_pid), std::string::npos);
}

TEST_F(SharedConfigTest, ReadMissingFile) {
  auto config = ReadSharedConfigFile((tmp_dir_ / "nonexistent.conf").string());
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
  EXPECT_EQ(config.html_mode, HtmlMode::kFull);
}

TEST_F(SharedConfigTest, ReadEmptyFile) {
  std::string path = WriteRawFile("empty.conf", "");
  auto config = ReadSharedConfigFile(path);
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
  EXPECT_EQ(config.html_mode, HtmlMode::kFull);
}

// ---------------------------------------------------------------------------
// Path derivation tests
// ---------------------------------------------------------------------------

TEST_F(SharedConfigTest, SharedConfigFilePath_Normal) {
  EXPECT_EQ(std::filesystem::path(SharedConfigFilePath("/data/cache.vol")),
            std::filesystem::path("/data/pagespeed-shared.conf"));
}

TEST_F(SharedConfigTest, SharedConfigFilePath_Empty) {
  EXPECT_EQ(SharedConfigFilePath(""), "");
}

TEST_F(SharedConfigTest, SharedConfigFilePath_RootDir) {
  EXPECT_EQ(std::filesystem::path(SharedConfigFilePath("/cache.vol")),
            std::filesystem::path("/pagespeed-shared.conf"));
}

// ---------------------------------------------------------------------------
// Additional edge case tests
// ---------------------------------------------------------------------------

TEST_F(SharedConfigTest, ReadOversizedFileReturnsDefaults) {
  std::string large(size_t{65} * 1024, 'x');  // > 64 KB limit
  std::string path = WriteRawFile("oversized.conf", large);
  auto config = ReadSharedConfigFile(path);
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
}

TEST_F(SharedConfigTest, WriteEmptyPathReturnsFalse) {
  SharedConfig config;
  EXPECT_FALSE(WriteSharedConfigFile("", config, 1));
}

TEST_F(SharedConfigTest, ReadEmptyPathReturnsDefaults) {
  auto config = ReadSharedConfigFile("");
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
}

TEST_F(SharedConfigTest, ParseNonNumericVersionContinuesParsing) {
  auto config = ParseSharedConfig(
      "version=abc\n"
      "socket_path=/works.sock\n");
  EXPECT_EQ(config.socket_path, "/works.sock");
}

TEST_F(SharedConfigTest, ParseNegativeVersionContinuesParsing) {
  auto config = ParseSharedConfig(
      "version=-1\n"
      "socket_path=/negative.sock\n");
  EXPECT_EQ(config.socket_path, "/negative.sock");
}

TEST_F(SharedConfigTest, ParseLargeVersionReturnsDefaults) {
  auto config = ParseSharedConfig(
      "version=999999\n"
      "socket_path=/v.sock\n");
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
}

TEST_F(SharedConfigTest, ParseVersionTwoAfterFieldsReturnsDefaults) {
  // version=2 appearing after other fields should discard parsed values.
  auto config = ParseSharedConfig(
      "socket_path=/should-be-discarded.sock\n"
      "version=2\n");
  EXPECT_EQ(config.socket_path, "/run/pagespeed-optimizer/notify.sock");
}

TEST_F(SharedConfigTest, WriteToNonExistentDirectoryReturnsFalse) {
  SharedConfig config;
  EXPECT_FALSE(
      WriteSharedConfigFile("/nonexistent/dir/pagespeed-shared.conf", config));
}

TEST_F(SharedConfigTest, WriteCleansUpStaleTmpFile) {
  std::string path = (tmp_dir_ / "stale.conf").string();
  std::string tmp_path = path + ".tmp";
  // Create stale .tmp from a "previous crash".
  std::ofstream(tmp_path) << "stale data";
  ASSERT_TRUE(std::filesystem::exists(tmp_path));

  SharedConfig config;
  config.socket_path = "/fresh.sock";
  ASSERT_TRUE(WriteSharedConfigFile(path, config, 1));

  auto loaded = ReadSharedConfigFile(path);
  EXPECT_EQ(loaded.socket_path, "/fresh.sock");
  EXPECT_FALSE(std::filesystem::exists(tmp_path));
}

TEST_F(SharedConfigTest, ParseCacheModeAggressive) {
  auto config = ParseSharedConfig("cache_mode=aggressive\n");
  EXPECT_EQ(config.cache_mode, "aggressive");
}

TEST_F(SharedConfigTest, ParseCacheModeSafe) {
  auto config = ParseSharedConfig("cache_mode=safe\n");
  EXPECT_EQ(config.cache_mode, "safe");
}

TEST_F(SharedConfigTest, ParseCacheModeInvalid) {
  // Invalid values are silently ignored; cache_mode stays empty.
  auto config = ParseSharedConfig("cache_mode=invalid\n");
  EXPECT_EQ(config.cache_mode, "");
}

TEST_F(SharedConfigTest, ParseCacheModeEmpty) {
  auto config = ParseSharedConfig("cache_mode=\n");
  EXPECT_EQ(config.cache_mode, "");
}

TEST_F(SharedConfigTest, WriteCacheModeRoundTrip) {
  std::string path = (tmp_dir_ / "cache-mode.conf").string();
  SharedConfig original;
  original.cache_mode = "aggressive";
  ASSERT_TRUE(WriteSharedConfigFile(path, original, 42));

  auto loaded = ReadSharedConfigFile(path);
  EXPECT_EQ(loaded.cache_mode, "aggressive");
}

TEST_F(SharedConfigTest, WriteCacheModeEmpty) {
  // Empty cache_mode should not produce a cache_mode= line.
  std::string path = (tmp_dir_ / "cache-mode-empty.conf").string();
  SharedConfig original;
  // cache_mode is empty by default.
  ASSERT_TRUE(WriteSharedConfigFile(path, original, 42));

  std::ifstream f(path);
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content.find("cache_mode="), std::string::npos);
}

TEST_F(SharedConfigTest, ParseDisableHtmlCaseSensitive) {
  // Only "true" and "1" are truthy; other values are falsy.
  EXPECT_EQ(ParseSharedConfig("disable_html=True\n").html_mode,
            HtmlMode::kFull);
  EXPECT_EQ(ParseSharedConfig("disable_html=TRUE\n").html_mode,
            HtmlMode::kFull);
  EXPECT_EQ(ParseSharedConfig("disable_html=yes\n").html_mode, HtmlMode::kFull);
}

TEST_F(SharedConfigTest, OperatorEquals) {
  SharedConfig a;
  a.socket_path = "/a.sock";
  a.cache_mode = "safe";
  a.html_mode = HtmlMode::kOff;

  SharedConfig b = a;
  EXPECT_EQ(a, b);

  SharedConfig c = a;
  c.socket_path = "/c.sock";
  EXPECT_NE(a, c);
}

TEST_F(SharedConfigTest, WriteRejectsNewlineInSocketPath) {
  std::string path = (tmp_dir_ / "newline.conf").string();
  SharedConfig config;
  config.socket_path = "/tmp/test.sock\nmalicious=true";
  EXPECT_FALSE(WriteSharedConfigFile(path, config, 1));
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(SharedConfigTest, WriteRejectsCarriageReturnInValue) {
  std::string path = (tmp_dir_ / "cr.conf").string();
  SharedConfig config;
  config.cache_mode = "safe\rmalicious=true";
  EXPECT_FALSE(WriteSharedConfigFile(path, config, 1));
  EXPECT_FALSE(std::filesystem::exists(path));
}

// The agent_optimize_entitled serve-side flag.
TEST_F(SharedConfigTest, ParseAgentOptimizeEntitled) {
  EXPECT_TRUE(ParseSharedConfig("version=1\nagent_optimize_entitled=true\n")
                  .agent_optimize_entitled);
  EXPECT_TRUE(ParseSharedConfig("version=1\nagent_optimize_entitled=1\n")
                  .agent_optimize_entitled);
  EXPECT_FALSE(ParseSharedConfig("version=1\nagent_optimize_entitled=false\n")
                   .agent_optimize_entitled);
  // Forward/backward compat: a file without the key defaults to false.
  EXPECT_FALSE(ParseSharedConfig("version=1\ndisable_html=true\n")
                   .agent_optimize_entitled);
}

TEST_F(SharedConfigTest, AgentOptimizeEntitledRoundTrip) {
  std::string path = (tmp_dir_ / "agent.conf").string();
  SharedConfig original;
  original.agent_optimize_entitled = true;
  ASSERT_TRUE(WriteSharedConfigFile(path, original, 7));
  auto loaded = ReadSharedConfigFile(path);
  EXPECT_TRUE(loaded.agent_optimize_entitled);
  // Default stays false through a round-trip when unset.
  SharedConfig off;
  std::string path2 = (tmp_dir_ / "agent-off.conf").string();
  ASSERT_TRUE(WriteSharedConfigFile(path2, off, 8));
  EXPECT_FALSE(ReadSharedConfigFile(path2).agent_optimize_entitled);
}

// ---------------------------------------------------------------------------
// There is no license state in the shared config any more
// ---------------------------------------------------------------------------

TEST_F(SharedConfigTest, WriterEmitsNoLicenseKeys) {
  // The 2.0 writer published license_key / license_valid /
  // license_checked_once.  The 2.1 writer must publish none of them (and
  // nothing else that spells "license"), on any config.
  std::string path = (tmp_dir_ / "no-license.conf").string();
  SharedConfig config;
  config.agent_optimize_entitled = true;
  config.agent_optimize_llms_txt_enabled = true;
  config.html_mode = HtmlMode::kOff;
  ASSERT_TRUE(WriteSharedConfigFile(path, config, 1));
  std::ifstream f(path);
  std::string raw((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  // Line-anchored: rsl_cap_requested_license= (the unrelated RSL-CAP protocol
  // setting) legitimately contains the substring.
  for (const char* key :
       {"\nlicense_key=", "\nlicense_valid=", "\nlicense_checked_once="}) {
    EXPECT_EQ(raw.find(key), std::string::npos) << key << "\n" << raw;
  }
  EXPECT_EQ(raw.find("unlicensed"), std::string::npos) << raw;
  EXPECT_EQ(raw.find("x-pagespeed-warn"), std::string::npos) << raw;
  // The serve toggles still ride along, under their historical key name.
  EXPECT_NE(raw.find("agent_optimize_entitled=true\n"), std::string::npos)
      << raw;
  EXPECT_NE(raw.find("agent_optimize_llms_txt_enabled=true\n"),
            std::string::npos)
      << raw;
}

TEST_F(SharedConfigTest, LegacyLicenseKeysAreIgnoredOnRead) {
  // A file written by a 2.0 daemon (mixed-version window during a package
  // upgrade) still parses; its license keys are unknown keys now and change
  // nothing, while the keys that still exist are honoured.
  ResetSharedConfigVersionSkewForTesting();  // the skew record is sticky
  auto config = ParseSharedConfig(
      "version=1\n"
      "socket_path=/run/legacy.sock\n"
      "license_key=abc.def.ghi\n"
      "license_valid=true\n"
      "license_checked_once=true\n"
      "disable_html=true\n"
      "agent_optimize_entitled=true\n");
  SharedConfig expected;
  expected.socket_path = "/run/legacy.sock";
  expected.html_mode = HtmlMode::kOff;
  expected.agent_optimize_entitled = true;
  EXPECT_EQ(config, expected);
  // Same schema version on both sides: the daemon deliberately did NOT bump
  // it for the removal (see kSharedConfigVersion).
  EXPECT_EQ(kSharedConfigVersion, 1);
  EXPECT_FALSE(SharedConfigVersionSkewState().mismatch);
}

// ---------------------------------------------------------------------------
// Schema-version skew: the one loud signal
// ---------------------------------------------------------------------------

// Collects the messages the version-mismatch signal emits, so the "loud" half
// can be asserted and not merely hoped for.
class CapturingHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }
  std::vector<std::string> messages() const { return messages_; }

 protected:
  void MessageV(MessageType /*type*/, const char* format,
                va_list args) override {
    messages_.push_back(FormatMessage(format, args));
  }

 private:
  std::vector<std::string> messages_;
};

// Installs a capturing handler for the duration of a test and puts the sticky
// skew record back the way it was found.  The record is process-wide by
// design (it reports a deployment state, not an event), so a test that dirties
// it and walks away would leak into every test after it.
class SharedConfigSkewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ResetSharedConfigVersionSkewForTesting();
    SetSharedConfigMessageHandler(&handler_);
  }
  void TearDown() override {
    SetSharedConfigMessageHandler(nullptr);
    ResetSharedConfigVersionSkewForTesting();
  }
  CapturingHandler handler_;
};

// BOTH halves of the mandatory signal, in one test because either alone is a
// different (and wrong) design: loud-without-fallback would be a hard failure
// on skew, and fallback-without-loud is exactly the silent path this replaces.
TEST_F(SharedConfigSkewTest, UnreadableVersionFallsBackToDefaultsAndIsLoud) {
  const auto config = ParseSharedConfig(
      "version=2\n"
      "socket_path=/from-the-future.sock\n"
      "cache_mode=aggressive\n"
      "disable_html=true\n");

  // Half 1 — pass-through on compiled-in defaults.  Asserted field by field
  // rather than by comparing to SharedConfig{}, so a future field that IS
  // wrongly carried across cannot hide behind an aggregate comparison.
  const SharedConfig defaults;
  EXPECT_EQ(config.socket_path, defaults.socket_path);
  EXPECT_EQ(config.cache_mode, defaults.cache_mode);
  EXPECT_EQ(config.html_mode, defaults.html_mode);
  EXPECT_EQ(config, defaults) << "nothing from an unreadable schema may leak "
                                 "into the returned configuration";

  // Half 2 — it was said out loud, naming BOTH versions.  A message that says
  // only "unsupported version" leaves the operator unable to tell which side
  // to move.
  const auto messages = handler_.messages();
  ASSERT_EQ(messages.size(), 1u)
      << "a version mismatch must produce exactly one signal, not zero";
  EXPECT_NE(messages[0].find("version 2"), std::string::npos) << messages[0];
  EXPECT_NE(messages[0].find("version " + std::to_string(kSharedConfigVersion)),
            std::string::npos)
      << messages[0];
  EXPECT_NE(messages[0].find("default"), std::string::npos)
      << "the message must say what is in effect instead: " << messages[0];

  // Half 3 — and the state is readable, not only printable, because a support
  // bundle is assembled long after the log line scrolled past.
  const auto skew = SharedConfigVersionSkewState();
  EXPECT_TRUE(skew.mismatch);
  EXPECT_EQ(skew.observed_version, 2);
  EXPECT_EQ(skew.supported_version, kSharedConfigVersion);
  EXPECT_EQ(skew.observations, 1u);
}

// The quiet path: a file this build CAN read says nothing at all.  Without
// this, "loud on mismatch" is unfalsifiable — a signal that fires on every
// parse is not a signal.
TEST_F(SharedConfigSkewTest, MatchingVersionIsSilent) {
  const auto config = ParseSharedConfig(
      "version=1\n"
      "socket_path=/matched.sock\n"
      "disable_html=true\n");

  EXPECT_EQ(config.socket_path, "/matched.sock");
  EXPECT_EQ(config.html_mode, HtmlMode::kOff);
  EXPECT_TRUE(handler_.messages().empty())
      << "a version this build reads must not be reported as skew";
  EXPECT_FALSE(SharedConfigVersionSkewState().mismatch);
}

// A file with no version line at all is a version-1 file (that is what
// "missing keys keep defaults" means here) and must stay quiet — otherwise
// every embedder reading a config written by an older build gets a false
// alarm.
TEST_F(SharedConfigSkewTest, AbsentVersionIsSilent) {
  const auto config = ParseSharedConfig("socket_path=/no-version.sock\n");
  EXPECT_EQ(config.socket_path, "/no-version.sock");
  EXPECT_TRUE(handler_.messages().empty());
  EXPECT_FALSE(SharedConfigVersionSkewState().mismatch);
}

// A mismatched file is re-read on every reload and, through the embedding API,
// potentially far more often.  The signal stays loud without becoming noise:
// one line per DISTINCT version, while the observation count keeps rising so
// the persistence of the condition is still visible.
TEST_F(SharedConfigSkewTest, RepeatedMismatchReportsOncePerDistinctVersion) {
  for (int i = 0; i < 5; ++i) {
    ParseSharedConfig("version=2\n");
  }
  EXPECT_EQ(handler_.messages().size(), 1u);
  EXPECT_EQ(SharedConfigVersionSkewState().observations, 5u);

  // A second peer at a different version is a new fact and must not be
  // swallowed by the rate limit the first one armed.
  ParseSharedConfig("version=7\n");
  ASSERT_EQ(handler_.messages().size(), 2u);
  EXPECT_NE(handler_.messages()[1].find("version 7"), std::string::npos);
  EXPECT_EQ(SharedConfigVersionSkewState().observed_version, 7);
  EXPECT_EQ(SharedConfigVersionSkewState().observations, 6u);
}

// The skew record is STICKY: once observed it stays observed, even if a later
// parse succeeds.  A diagnostic surface that forgot a mismatch the moment the
// next reload happened to read a good file would report "healthy" for an
// install that spent the last hour running on defaults.
TEST_F(SharedConfigSkewTest, MismatchStateIsSticky) {
  ParseSharedConfig("version=2\n");
  ASSERT_TRUE(SharedConfigVersionSkewState().mismatch);

  ParseSharedConfig("version=1\nsocket_path=/ok.sock\n");
  EXPECT_TRUE(SharedConfigVersionSkewState().mismatch)
      << "an observed mismatch must not be erased by a later good parse";
  EXPECT_EQ(SharedConfigVersionSkewState().observed_version, 2);
}

// The file path, not just the string path — this is the shape every consumer
// actually calls.
TEST_F(SharedConfigSkewTest, ReadFileWithUnreadableVersionIsLoudAndDefaults) {
  auto dir = std::filesystem::temp_directory_path() /
             ("shared_config_skew_" + std::to_string(
#ifdef _WIN32
                                          _getpid()
#else
                                          getpid()
#endif
                                              ));
  std::filesystem::create_directories(dir);
  auto path = (dir / "pagespeed-shared.conf").string();
  {
    std::ofstream f(path, std::ios::binary);
    f << "version=99\nsocket_path=/future.sock\n";
  }

  const auto config = ReadSharedConfigFile(path);
  EXPECT_EQ(config, SharedConfig{});
  EXPECT_EQ(handler_.messages().size(), 1u);
  EXPECT_TRUE(SharedConfigVersionSkewState().mismatch);
  EXPECT_EQ(SharedConfigVersionSkewState().observed_version, 99);

  std::filesystem::remove_all(dir);
}

// The version this build writes is the version it reads.  A writer and reader
// that disagree in the same binary would manufacture the very skew the signal
// exists to report.
TEST_F(SharedConfigSkewTest, WrittenFileDeclaresTheSupportedVersion) {
  auto dir = std::filesystem::temp_directory_path() /
             ("shared_config_skew_w_" + std::to_string(
#ifdef _WIN32
                                            _getpid()
#else
                                            getpid()
#endif
                                                ));
  std::filesystem::create_directories(dir);
  auto path = (dir / "pagespeed-shared.conf").string();

  SharedConfig cfg;
  cfg.socket_path = "/round-trip.sock";
  ASSERT_TRUE(WriteSharedConfigFile(path, cfg, 1234));

  std::string content;
  {
    // Scoped: an open handle blocks the remove_all below on Windows.
    std::ifstream f(path);
    content.assign((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  }
  EXPECT_NE(
      content.find("version=" + std::to_string(kSharedConfigVersion) + "\n"),
      std::string::npos)
      << content;

  EXPECT_EQ(ReadSharedConfigFile(path).socket_path, "/round-trip.sock");
  EXPECT_TRUE(handler_.messages().empty())
      << "this build must be able to read what this build writes";

  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace pagespeed
