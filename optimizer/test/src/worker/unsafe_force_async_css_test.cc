// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/unsafe_force_async_css.h"

#include <gtest/gtest.h>

#include <string>

namespace pagespeed {
namespace {

TEST(UnsafeForceAsyncCssFlagTest, FlagSetsTheField) {
  bool force = false;
  EXPECT_TRUE(ParseUnsafeForceAsyncCssFlag("--unsafe-force-async-css", &force));
  EXPECT_TRUE(force);
}

TEST(UnsafeForceAsyncCssFlagTest, AbsentFlagLeavesTheFieldAtItsDefault) {
  // The whole point of the switch is that a worker started without it behaves
  // exactly as it did before the switch existed.
  bool force = false;
  for (const char* arg : {"--no-async-css", "--async-css-min-coverage",
                          "--socket", "--enable-browser-analysis"}) {
    EXPECT_FALSE(ParseUnsafeForceAsyncCssFlag(arg, &force)) << arg;
  }
  EXPECT_FALSE(force);
}

TEST(UnsafeForceAsyncCssFlagTest, NearMissesAreNotTheFlag) {
  // Exact match only: no prefix form, no `=value` form, no negation. A worker
  // must never enable this by accident.
  bool force = false;
  for (const char* arg :
       {"--unsafe-force-async-css=1", "--unsafe-force-async-css-really",
        "unsafe-force", "--no-unsafe-force-async-css", "--unsafe-force", ""}) {
    EXPECT_FALSE(ParseUnsafeForceAsyncCssFlag(arg, &force)) << arg;
  }
  EXPECT_FALSE(force);
}

TEST(UnsafeForceAsyncCssFlagTest, WarningNamesTheFlagAndTheRisk) {
  const std::string warning(kUnsafeForceAsyncCssWarning);
  EXPECT_NE(warning.find(kUnsafeForceAsyncCssFlag), std::string::npos);
  EXPECT_NE(warning.find("unstyled"), std::string::npos);
  EXPECT_NE(warning.find("Diagnostic"), std::string::npos);
}

TEST(AsyncCssDeferralAllowedTest, DefaultLeavesTheSufficiencyVerdictIntact) {
  // force=false is the shipped configuration: the verdict passes through
  // unchanged in BOTH directions.
  EXPECT_TRUE(AsyncCssDeferralAllowed(/*sufficiency_verdict=*/true,
                                      /*unsafe_force=*/false));
  EXPECT_FALSE(AsyncCssDeferralAllowed(/*sufficiency_verdict=*/false,
                                       /*unsafe_force=*/false));
}

TEST(AsyncCssDeferralAllowedTest, ForceOverridesARefusal) {
  EXPECT_TRUE(AsyncCssDeferralAllowed(/*sufficiency_verdict=*/false,
                                      /*unsafe_force=*/true));
}

TEST(AsyncCssDeferralAllowedTest, ForceDoesNotDisturbAnApproval) {
  EXPECT_TRUE(AsyncCssDeferralAllowed(/*sufficiency_verdict=*/true,
                                      /*unsafe_force=*/true));
}

}  // namespace
}  // namespace pagespeed
