// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/tools/cli/value_mask.h"

#include <fstream>
#include <regex>
#include <sstream>

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace lint {
namespace {

GoogleString ReadSafeOptionNamesTs() {
  std::ifstream in(
      RunfilePath("pagespeed/system/console/src/lib/data/safe-option-names.ts"),
      std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The generated allowlist holds every entry of safe-option-names.ts, counted
// here independently of the generator, so that a format change in the
// TypeScript file fails the build or this test instead of unmasking or
// dropping names silently.
TEST(ValueMaskTest, AllowlistMatchesTheConsoleList) {
  const GoogleString ts = ReadSafeOptionNamesTs();
  ASSERT_FALSE(ts.empty());
  const std::regex entry("^\\s*\"[A-Za-z][A-Za-z0-9_]*\",?\\s*$");
  int entries = 0;
  std::istringstream lines(ts);
  GoogleString line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (std::regex_match(line, entry)) {
      ++entries;
    }
  }
  ValueMask mask;
  EXPECT_GT(mask.safe_name_count(), 100);
  EXPECT_EQ(entries, ValueMask::SourceEntryCount());
  EXPECT_EQ(entries,
            mask.safe_name_count() + ValueMask::SourceSecretExcludedCount());
}

TEST(ValueMaskTest, OnlyAllowlistedValuesAreShown) {
  ValueMask mask;
  EXPECT_TRUE(mask.ValuesAreSafe("ModPagespeedRewriteLevel"));
  EXPECT_TRUE(mask.ValuesAreSafe("modpagespeedrewritelevel"));
  // Options whose values may be credentials or private addresses.
  EXPECT_FALSE(mask.ValuesAreSafe("ModPagespeedUrlSigningKey"));
  EXPECT_FALSE(mask.ValuesAreSafe("ModPagespeedRequestOptionOverride"));
  EXPECT_FALSE(mask.ValuesAreSafe("ModPagespeedCustomFetchHeader"));
  EXPECT_FALSE(mask.ValuesAreSafe("ModPagespeedRedisServer"));
  // Names that are not an option, or not ours.
  EXPECT_FALSE(mask.ValuesAreSafe("ModPagespeed"));
  EXPECT_FALSE(mask.ValuesAreSafe("ModPagespeedNoSuchOption"));
  EXPECT_FALSE(mask.ValuesAreSafe("RewriteLevel"));
  EXPECT_FALSE(mask.ValuesAreSafe(""));

  EXPECT_EQ(std::vector<GoogleString>({kMasked, kMasked}),
            mask.Args("ModPagespeedCustomFetchHeader", {"Authorization", "x"}));
  EXPECT_EQ(std::vector<GoogleString>({"CoreFilters"}),
            mask.Args("ModPagespeedRewriteLevel", {"CoreFilters"}));
  EXPECT_EQ(kMasked, mask.Text("anything"));
}

TEST(ValueMaskTest, ShowAllLiftsTheMask) {
  ValueMask mask;
  mask.ShowAll();
  EXPECT_TRUE(mask.ValuesAreSafe("ModPagespeedUrlSigningKey"));
  EXPECT_EQ("anything", mask.Text("anything"));
}

}  // namespace
}  // namespace lint
}  // namespace net_instaweb
