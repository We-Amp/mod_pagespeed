// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the nginx module's cache-directory generation check
// (src/nginx/cache_generation_check.h): the compare, the cache on/off
// decision, the log-once bookkeeping and the wording of the log lines.
//
// NOT covered here (module wiring, integration-only — t/106-cache-generation.t
// drives it against a running nginx): that a mismatch really passes requests
// through uncached, that the verdict is re-read on the shared-config poll,
// and the $pagespeed_cache_generation* variables.

#include "src/nginx/cache_generation_check.h"

#include <string>

#include "gtest/gtest.h"
#include "src/worker/shared_config.h"

namespace pagespeed {
namespace {

constexpr char kPath[] =
    "/var/cache/pagespeed-optimizer/v1/pagespeed-shared.conf";

TEST(CacheGenerationCheckTest, ModuleGenerationIsTheOptimizersConstant) {
  // Shared, not duplicated: bumping kCacheDirGeneration moves the module.
  EXPECT_EQ(ModuleCacheDirGeneration(), kCacheDirGeneration);
}

TEST(CacheGenerationCheckTest, SameGenerationMatches) {
  const auto v = EvaluateCacheGeneration(2, 2);
  EXPECT_EQ(v.state, CacheGenerationState::kMatch);
  EXPECT_EQ(v.module_generation, 2);
  EXPECT_EQ(v.optimizer_generation, 2);
  EXPECT_TRUE(v.cache_enabled());
}

TEST(CacheGenerationCheckTest, OlderOptimizerIsAMismatchAndDisablesTheCache) {
  const auto v = EvaluateCacheGeneration(1, 2);
  EXPECT_EQ(v.state, CacheGenerationState::kMismatch);
  EXPECT_EQ(v.optimizer_generation, 1);
  EXPECT_FALSE(v.cache_enabled());
}

TEST(CacheGenerationCheckTest, NewerOptimizerIsAMismatchToo) {
  const auto v = EvaluateCacheGeneration(3, 2);
  EXPECT_EQ(v.state, CacheGenerationState::kMismatch);
  EXPECT_FALSE(v.cache_enabled());
}

TEST(CacheGenerationCheckTest, NotStatedIsUnknownAndKeepsTheCache) {
  // 0 is what the parser leaves for an optimizer predating the field, a
  // missing or unreadable shared config, or an unreadable schema version.
  // It must not be compared: today's behaviour carries on.
  for (int stated : {0, -1}) {
    const auto v = EvaluateCacheGeneration(stated, 2);
    EXPECT_EQ(v.state, CacheGenerationState::kUnknown) << stated;
    EXPECT_EQ(v.optimizer_generation, 0) << stated;
    EXPECT_TRUE(v.cache_enabled()) << stated;
  }
}

TEST(CacheGenerationCheckTest, StateNames) {
  EXPECT_STREQ(CacheGenerationStateName(CacheGenerationState::kMatch), "match");
  EXPECT_STREQ(CacheGenerationStateName(CacheGenerationState::kMismatch),
               "mismatch");
  EXPECT_STREQ(CacheGenerationStateName(CacheGenerationState::kUnknown),
               "unknown");
}

TEST(CacheGenerationCheckTest, MismatchLineNamesBothSidesTheOlderOneAndTheFix) {
  const std::string line = DescribeCacheGenerationVerdict(
      EvaluateCacheGeneration(1, 2), CacheGenerationState::kUnknown, kPath);
  EXPECT_EQ(line.rfind("pagespeed: cache-directory generation mismatch", 0), 0u)
      << line;
  EXPECT_NE(line.find("this module is generation 2"), std::string::npos);
  EXPECT_NE(line.find("is generation 1"), std::string::npos);
  EXPECT_NE(line.find(kPath), std::string::npos);
  EXPECT_NE(line.find("the optimizer is older than this module"),
            std::string::npos);
  EXPECT_NE(line.find("DISABLED"), std::string::npos);
  EXPECT_NE(line.find("same release"), std::string::npos);
  EXPECT_NE(line.find("same image tag"), std::string::npos);

  const std::string reverse = DescribeCacheGenerationVerdict(
      EvaluateCacheGeneration(3, 2), CacheGenerationState::kUnknown, kPath);
  EXPECT_NE(reverse.find("this module is older than the optimizer"),
            std::string::npos);
}

TEST(CacheGenerationCheckTest, MatchAndRecoveryLines) {
  const auto match = EvaluateCacheGeneration(2, 2);
  EXPECT_EQ(DescribeCacheGenerationVerdict(
                match, CacheGenerationState::kUnknown, kPath)
                .rfind("pagespeed: cache-directory generation 2 matches the "
                       "optimizer",
                       0),
            0u);
  const std::string recovered = DescribeCacheGenerationVerdict(
      match, CacheGenerationState::kMismatch, kPath);
  EXPECT_NE(recovered.find("now matches"), std::string::npos);
  EXPECT_NE(recovered.find("enabled again"), std::string::npos);
}

TEST(CacheGenerationCheckTest, UnknownLineSaysItCouldNotVerify) {
  const std::string line = DescribeCacheGenerationVerdict(
      EvaluateCacheGeneration(0, 2), CacheGenerationState::kUnknown, kPath);
  EXPECT_NE(line.find("cannot verify the cache-directory generation"),
            std::string::npos);
  EXPECT_NE(line.find("as before"), std::string::npos);
  EXPECT_NE(line.find("generation 2"), std::string::npos);
}

TEST(CacheGenerationMonitorTest, NothingPendingBeforeTheFirstRead) {
  CacheGenerationMonitor m(2);
  EXPECT_FALSE(m.has_verdict());
  EXPECT_FALSE(m.log_pending());
  // Before any read the cache is used as today.
  EXPECT_TRUE(m.cache_enabled());
}

TEST(CacheGenerationMonitorTest, LogsOncePerChangeNotPerReread) {
  CacheGenerationMonitor m(2);
  EXPECT_TRUE(m.Update(1));
  EXPECT_TRUE(m.log_pending());
  EXPECT_FALSE(m.cache_enabled());
  m.MarkLogged();

  // The shared config is re-read every time its mtime moves, and once in
  // every nginx worker after fork: the same verdict must stay quiet.
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(m.Update(1));
    EXPECT_FALSE(m.log_pending());
  }
  EXPECT_FALSE(m.cache_enabled());
}

TEST(CacheGenerationMonitorTest, CompletedRollingUpgradeReEnablesTheCache) {
  CacheGenerationMonitor m(2);
  m.Update(1);
  m.MarkLogged();
  ASSERT_FALSE(m.cache_enabled());

  // The optimizer is upgraded and rewrites its shared config.
  EXPECT_TRUE(m.Update(2));
  EXPECT_TRUE(m.cache_enabled());
  EXPECT_TRUE(m.log_pending());
  EXPECT_EQ(m.logged_state(), CacheGenerationState::kMismatch);
  m.MarkLogged();
  EXPECT_EQ(m.logged_state(), CacheGenerationState::kMatch);
}

TEST(CacheGenerationMonitorTest, RollbackToAnOlderOptimizerDisablesAtRuntime) {
  CacheGenerationMonitor m(2);
  m.Update(2);
  m.MarkLogged();
  EXPECT_TRUE(m.Update(1));
  EXPECT_FALSE(m.cache_enabled());
  EXPECT_TRUE(m.log_pending());
}

TEST(CacheGenerationMonitorTest, AnotherMismatchingGenerationLogsAgain) {
  CacheGenerationMonitor m(2);
  m.Update(1);
  m.MarkLogged();
  EXPECT_TRUE(m.Update(3));
  EXPECT_TRUE(m.log_pending());
  EXPECT_EQ(m.verdict().optimizer_generation, 3);
}

TEST(CacheGenerationMonitorTest, UnknownKeepsTheCacheAndLogsOnce) {
  CacheGenerationMonitor m(2);
  EXPECT_TRUE(m.Update(0));
  EXPECT_EQ(m.verdict().state, CacheGenerationState::kUnknown);
  EXPECT_TRUE(m.cache_enabled());
  EXPECT_TRUE(m.log_pending());
  m.MarkLogged();
  EXPECT_FALSE(m.Update(0));
  EXPECT_FALSE(m.log_pending());
}

TEST(CacheGenerationMonitorTest, ReloadRepeatsAnUnresolvedMismatch) {
  CacheGenerationMonitor m(2);
  m.Update(1);
  m.MarkLogged();

  // nginx -s reload: a new configuration cycle re-reads the same file.
  m.Rearm();
  EXPECT_FALSE(m.Update(1));     // Unchanged verdict ...
  EXPECT_TRUE(m.log_pending());  // ... but logged again for this cycle.
  m.MarkLogged();
  EXPECT_FALSE(m.log_pending());
}

}  // namespace
}  // namespace pagespeed
