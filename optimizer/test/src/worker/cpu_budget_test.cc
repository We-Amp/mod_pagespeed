// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/cpu_budget.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

class CpuBudgetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() /
        ("cpu_budget_test_" +
         std::string(
             ::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    sources_.cgroup_v2_cpu_max = (dir_ / "cpu.max").string();
    sources_.cgroup_v1_quota = (dir_ / "quota").string();
    sources_.cgroup_v1_period = (dir_ / "period").string();
    sources_.cpu_count_override = 64;
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  void Write(const std::string& path, const std::string& text) {
    std::ofstream(path) << text << "\n";
  }
  void V2(const std::string& text) { Write(sources_.cgroup_v2_cpu_max, text); }
  void V1(const std::string& quota, const std::string& period) {
    Write(sources_.cgroup_v1_quota, quota);
    Write(sources_.cgroup_v1_period, period);
  }

  std::filesystem::path dir_;
  CpuBudgetSources sources_;
};

TEST_F(CpuBudgetTest, MissingFilesAreUnlimited) {
  CpuBudget b = AvailableCpus(sources_);
  EXPECT_EQ(b.cpus, 64);
  EXPECT_STREQ(b.source, "affinity");
}

TEST_F(CpuBudgetTest, V2MaxIsUnlimited) {
  V2("max 100000");
  EXPECT_EQ(AvailableCpus(sources_).cpus, 64);
}

TEST_F(CpuBudgetTest, V2QuotaWholeCpus) {
  V2("1200000 100000");
  CpuBudget b = AvailableCpus(sources_);
  EXPECT_EQ(b.cpus, 12);
  EXPECT_STREQ(b.source, "cgroup v2 quota");
}

TEST_F(CpuBudgetTest, V2QuotaRoundsUp) {
  V2("150000 100000");
  EXPECT_EQ(AvailableCpus(sources_).cpus, 2);
}

TEST_F(CpuBudgetTest, V1Quota) {
  V1("600000", "100000");
  CpuBudget b = AvailableCpus(sources_);
  EXPECT_EQ(b.cpus, 6);
  EXPECT_STREQ(b.source, "cgroup v1 quota");
}

TEST_F(CpuBudgetTest, V1NegativeQuotaIsUnlimited) {
  V1("-1", "100000");
  EXPECT_EQ(AvailableCpus(sources_).cpus, 64);
}

TEST_F(CpuBudgetTest, AffinitySmallerThanQuotaWins) {
  sources_.cpu_count_override = 4;
  V2("1200000 100000");
  CpuBudget b = AvailableCpus(sources_);
  EXPECT_EQ(b.cpus, 4);
  EXPECT_STREQ(b.source, "affinity");
}

TEST_F(CpuBudgetTest, MalformedFilesAreUnlimited) {
  V2("garbage");
  EXPECT_EQ(AvailableCpus(sources_).cpus, 64);
  V2("12abc 100000");
  EXPECT_EQ(AvailableCpus(sources_).cpus, 64);
  V2("garbage");
  V1("abc", "100000");
  EXPECT_EQ(AvailableCpus(sources_).cpus, 64);
}

TEST_F(CpuBudgetTest, NeverBelowOne) {
  V2("1000 100000");  // 0.01 CPU rounds up to 1
  EXPECT_EQ(AvailableCpus(sources_).cpus, 1);
  sources_.cpu_count_override = 0;
  EXPECT_EQ(AvailableCpus(sources_).cpus, 1);
}

TEST_F(CpuBudgetTest, DefaultThreadCountClamps) {
  EXPECT_EQ(DefaultThreadCount({1, "affinity"}), 2);
  EXPECT_EQ(DefaultThreadCount({12, "affinity"}), 12);
  EXPECT_EQ(DefaultThreadCount({512, "affinity"}), 128);
}

TEST(CpuBudgetSystemTest, RealSystemIsAtLeastOne) {
  EXPECT_GE(AvailableCpus().cpus, 1);
}

}  // namespace
}  // namespace pagespeed
