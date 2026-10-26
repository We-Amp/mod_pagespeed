// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for the pagespeed.config layering of the IIS module:
//   - the resolver order (canonical %ProgramData% location, then the legacy
//     location of the previous product, then nothing), and
//   - the base / per-site merge (the per-site file is applied after the base
//     and wins for the directives it sets).
//
// The resolver is pointed at a temporary directory tree through
// iis_config_util::SetProgramDataRootForTesting, so no real %ProgramData% is
// read or written. The release lane covers the same contract end to end on a
// live IIS host (test/system/iis/test_iis_config_fallback.ps1); these tests
// run on every pull request.
//
// Windows-only: the resolver uses the Win32 shell and file APIs.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/iis/iis_config_util.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/kernel/base/google_message_handler.h"

namespace {

namespace fs = std::filesystem;

using net_instaweb::IisRewriteOptions;
using net_instaweb::RewriteOptions;

class IisConfigLayeringTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { IisRewriteOptions::Initialize(); }
  static void TearDownTestSuite() { IisRewriteOptions::Terminate(); }

  void SetUp() override {
    const char* tmp = std::getenv("TEST_TMPDIR");
    ASSERT_TRUE(tmp != nullptr) << "bazel always sets TEST_TMPDIR";
    const testing::TestInfo* info =
        testing::UnitTest::GetInstance()->current_test_info();
    root_ = (fs::path(tmp) / (std::string("cfg_layering_") + info->name()))
                .string();
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_);
    iis_config_util::SetProgramDataRootForTesting(root_ + "\\ProgramData");
  }

  void TearDown() override {
    iis_config_util::SetProgramDataRootForTesting("");
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  std::string ProgramData() const { return root_ + "\\ProgramData"; }
  std::string CanonicalDir() const {
    return ProgramData() + "\\We-Amp\\PageSpeed\\";
  }
  std::string LegacyDir() const {
    return ProgramData() + "\\We-Amp\\IISWebSpeed\\";
  }
  // Site physical path, with the trailing backslash IIS returns.
  std::string SiteDir() const { return root_ + "\\site\\"; }

  // Writes |dir|\|name| with |content|, creating |dir| as needed.
  void Write(const std::string& dir, const std::string& name,
             const std::string& content) {
    fs::create_directories(dir);
    std::ofstream out(dir + name, std::ios::binary);
    ASSERT_TRUE(out.good()) << dir << name;
    out << content;
    out.flush();
    ASSERT_TRUE(out.good()) << "write failed: " << dir << name;
  }

  // Applies the resolved chain for SiteDir() the way the module does (base
  // first, per-site last) and returns the merged options.
  void ApplyChain(IisRewriteOptions* rwo) {
    const std::vector<std::string> chain =
        iis_config_util::ResolveConfigPaths(SiteDir());
    std::list<std::string> paths(chain.begin(), chain.end());
    std::map<std::string, std::string> input;
    input["config"] = "base";
    ConfigFactory factory;
    net_instaweb::global_settings settings;
    factory.GetConfig(paths, input, *rwo, &handler_, &settings);
  }

  std::string root_;
  net_instaweb::GoogleMessageHandler handler_;
};

// ---------------------------------------------------------------------------
// Resolver order
// ---------------------------------------------------------------------------

TEST_F(IisConfigLayeringTest, CanonicalLocationWinsOverLegacy) {
  Write(CanonicalDir(), "pagespeed.config", "pagespeed on\n");
  Write(LegacyDir(), "pagespeed.config", "pagespeed off\n");

  bool exists = false;
  EXPECT_EQ(CanonicalDir() + "pagespeed.config",
            iis_config_util::ResolveProgramDataConfig(&exists));
  EXPECT_TRUE(exists);
}

TEST_F(IisConfigLayeringTest, LegacyLocationUsedOnlyWhenCanonicalAbsent) {
  Write(LegacyDir(), "pagespeed.config", "pagespeed off\n");

  bool exists = false;
  EXPECT_EQ(LegacyDir() + "pagespeed.config",
            iis_config_util::ResolveProgramDataConfig(&exists));
  EXPECT_TRUE(exists);
}

TEST_F(IisConfigLayeringTest, LegacyFileNameInLegacyLocationIsFound) {
  Write(LegacyDir(), "iiswebspeed.config", "pagespeed off\n");

  bool exists = false;
  EXPECT_EQ(LegacyDir() + "iiswebspeed.config",
            iis_config_util::ResolveProgramDataConfig(&exists));
  EXPECT_TRUE(exists);
}

TEST_F(IisConfigLayeringTest, PrimaryFileNameWinsWithinOneLocation) {
  Write(CanonicalDir(), "pagespeed.config", "pagespeed on\n");
  Write(CanonicalDir(), "iiswebspeed.config", "pagespeed off\n");

  bool exists = false;
  EXPECT_EQ(CanonicalDir() + "pagespeed.config",
            iis_config_util::ResolveProgramDataConfig(&exists));
  EXPECT_TRUE(exists);
}

TEST_F(IisConfigLayeringTest, CanonicalFallbackNameBeatsLegacyLocation) {
  // Both names of the canonical location are tried before the legacy
  // location is consulted at all.
  Write(CanonicalDir(), "iiswebspeed.config", "pagespeed on\n");
  Write(LegacyDir(), "pagespeed.config", "pagespeed off\n");

  bool exists = false;
  EXPECT_EQ(CanonicalDir() + "iiswebspeed.config",
            iis_config_util::ResolveProgramDataConfig(&exists));
  EXPECT_TRUE(exists);
}

TEST_F(IisConfigLayeringTest, NeitherLocationPresentReportsCanonicalPath) {
  bool exists = true;
  EXPECT_EQ(CanonicalDir() + "pagespeed.config",
            iis_config_util::ResolveProgramDataConfig(&exists));
  EXPECT_FALSE(exists);
  EXPECT_TRUE(iis_config_util::ResolveConfigPaths(SiteDir()).empty());
  EXPECT_EQ("", iis_config_util::ResolveEffectiveConfigFile(SiteDir()));
}

TEST_F(IisConfigLayeringTest, ChainIsBaseThenSite) {
  Write(CanonicalDir(), "pagespeed.config", "pagespeed on\n");
  Write(SiteDir(), "pagespeed.config", "pagespeed off\n");

  const std::vector<std::string> chain =
      iis_config_util::ResolveConfigPaths(SiteDir());
  ASSERT_EQ(2u, chain.size());
  EXPECT_EQ(CanonicalDir() + "pagespeed.config", chain[0]);
  EXPECT_EQ(SiteDir() + "pagespeed.config", chain[1]);
  EXPECT_EQ(chain[1], iis_config_util::ResolveEffectiveConfigFile(SiteDir()));
}

TEST_F(IisConfigLayeringTest, ChainFallsBackToLegacyBaseThenSite) {
  Write(LegacyDir(), "pagespeed.config", "pagespeed on\n");
  Write(SiteDir(), "pagespeed.config", "pagespeed off\n");

  const std::vector<std::string> chain =
      iis_config_util::ResolveConfigPaths(SiteDir());
  ASSERT_EQ(2u, chain.size());
  EXPECT_EQ(LegacyDir() + "pagespeed.config", chain[0]);
  EXPECT_EQ(SiteDir() + "pagespeed.config", chain[1]);
}

TEST_F(IisConfigLayeringTest, BaseOnlyChainHasOneEntry) {
  Write(CanonicalDir(), "pagespeed.config", "pagespeed on\n");

  const std::vector<std::string> chain =
      iis_config_util::ResolveConfigPaths(SiteDir());
  ASSERT_EQ(1u, chain.size());
  EXPECT_EQ(CanonicalDir() + "pagespeed.config", chain[0]);
  EXPECT_FALSE(iis_config_util::SiteConfigExists(SiteDir()));
}

// ---------------------------------------------------------------------------
// Base / per-site merge
// ---------------------------------------------------------------------------

TEST_F(IisConfigLayeringTest, MergeBaseOnly) {
  Write(CanonicalDir(), "pagespeed.config",
        "pagespeed DaemonVolumePath D:\\base\\volume\n"
        "pagespeed EnableFilters combine_css\n");

  IisRewriteOptions rwo(nullptr);
  ApplyChain(&rwo);

  EXPECT_EQ("D:\\base\\volume", rwo.daemon_volume_path());
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

TEST_F(IisConfigLayeringTest, MergeSiteOnly) {
  Write(SiteDir(), "pagespeed.config",
        "pagespeed DaemonVolumePath D:\\site\\volume\n"
        "pagespeed EnableFilters combine_css\n");

  IisRewriteOptions rwo(nullptr);
  ApplyChain(&rwo);

  EXPECT_EQ("D:\\site\\volume", rwo.daemon_volume_path());
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

TEST_F(IisConfigLayeringTest, MergeSiteOverridesBaseDirective) {
  Write(CanonicalDir(), "pagespeed.config",
        "pagespeed DaemonVolumePath D:\\base\\volume\n"
        "pagespeed DaemonSocketPath D:\\base\\notify.sock\n");
  Write(SiteDir(), "pagespeed.config",
        "pagespeed DaemonVolumePath D:\\site\\volume\n");

  IisRewriteOptions rwo(nullptr);
  ApplyChain(&rwo);

  // The site wins for the directive it sets; the base value it does not
  // touch is kept.
  EXPECT_EQ("D:\\site\\volume", rwo.daemon_volume_path());
  EXPECT_EQ("D:\\base\\notify.sock", rwo.daemon_socket_path());
}

TEST_F(IisConfigLayeringTest, MergeSiteAddsDirectiveToBase) {
  Write(CanonicalDir(), "pagespeed.config",
        "pagespeed DaemonVolumePath D:\\base\\volume\n");
  Write(SiteDir(), "pagespeed.config", "pagespeed EnableFilters combine_css\n");

  IisRewriteOptions rwo(nullptr);
  ApplyChain(&rwo);

  EXPECT_EQ("D:\\base\\volume", rwo.daemon_volume_path());
  EXPECT_TRUE(rwo.Enabled(RewriteOptions::kCombineCss));
}

TEST_F(IisConfigLayeringTest, MergeLegacyBaseWithSiteOverride) {
  // Upgrade box: only the legacy location holds a base; the site still
  // layers on top of it.
  Write(LegacyDir(), "pagespeed.config",
        "pagespeed DaemonVolumePath D:\\legacy\\volume\n"
        "pagespeed DaemonSocketPath D:\\legacy\\notify.sock\n");
  Write(SiteDir(), "pagespeed.config",
        "pagespeed DaemonSocketPath D:\\site\\notify.sock\n");

  IisRewriteOptions rwo(nullptr);
  ApplyChain(&rwo);

  EXPECT_EQ("D:\\legacy\\volume", rwo.daemon_volume_path());
  EXPECT_EQ("D:\\site\\notify.sock", rwo.daemon_socket_path());
}

}  // namespace
