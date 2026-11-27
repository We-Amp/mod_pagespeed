// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The known-sites file: location, format, atomic replacement, and the writer
// that coalesces changes.

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "src/worker/known_sites.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

const std::string kSiteA(64, 'a');
const std::string kSiteB(64, 'b');
const std::string kSiteC = std::string(63, '0') + "1";

class KnownSitesFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = pagespeed::test::MakeTempDir();
    path_ = dir_ + "/pagespeed-sites.conf";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Slurp() {
    std::ifstream f(path_, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }
  void Put(const std::string& content) {
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    f << content;
  }

  std::string dir_;
  std::string path_;
};

TEST_F(KnownSitesFileTest, LivesBesideTheHostAliasesFile) {
  // Compared as paths, like HostAliasesTest.FilePathNormal: the separator
  // before the file name is the platform's own.
  EXPECT_EQ(
      std::filesystem::path(
          KnownSitesFilePath("/var/cache/pagespeed/v7/cache.dat")),
      std::filesystem::path("/var/cache/pagespeed/v7/pagespeed-sites.conf"));
  EXPECT_EQ(KnownSitesFilePath(""), "");
}

TEST_F(KnownSitesFileTest, WritesAVersionLineThenSortedSites) {
  ASSERT_TRUE(WriteKnownSitesFile(path_, {kSiteB, kSiteA, kSiteC, kSiteA}));
  EXPECT_EQ(Slurp(), std::string("# pagespeed-sites v1\n") + kSiteC + "\n" +
                         kSiteA + "\n" + kSiteB + "\n");
#ifndef _WIN32
  struct stat st;
  ASSERT_EQ(0, stat(path_.c_str(), &st));
  EXPECT_EQ(0644u, st.st_mode & 0777u);
#endif
  bool readable = false;
  EXPECT_EQ(ReadKnownSitesFile(path_, &readable),
            (std::vector<std::string>{kSiteC, kSiteA, kSiteB}));
  EXPECT_TRUE(readable);
}

TEST_F(KnownSitesFileTest, ReplacesTheFileAtomically) {
  ASSERT_TRUE(WriteKnownSitesFile(path_, {kSiteA}));
  ASSERT_TRUE(WriteKnownSitesFile(path_, {kSiteB}));
  bool readable = false;
  EXPECT_EQ(ReadKnownSitesFile(path_, &readable),
            (std::vector<std::string>{kSiteB}));
  // No temporary file is left behind.
  EXPECT_FALSE(std::filesystem::exists(path_ + ".tmp"));
  // An empty set writes the version line alone.
  ASSERT_TRUE(WriteKnownSitesFile(path_, {}));
  EXPECT_EQ(Slurp(), "# pagespeed-sites v1\n");
}

TEST_F(KnownSitesFileTest, RefusesToWriteAValueThatIsNotASite) {
  ASSERT_TRUE(WriteKnownSitesFile(path_, {kSiteA}));
  EXPECT_FALSE(WriteKnownSitesFile(path_, {kSiteB, "not-a-site"}));
  bool readable = false;
  EXPECT_EQ(ReadKnownSitesFile(path_, &readable),
            (std::vector<std::string>{kSiteA}))
      << "the previous file is intact";
}

TEST_F(KnownSitesFileTest, SkipsAndCountsBadLines) {
  Put(std::string("# pagespeed-sites v1\n") + kSiteA + "\n" +
      std::string(64, 'A') + "\n" + "short\n" + kSiteB + "\r\n\n" +
      std::string(65, 'c') + "\n");
  bool readable = false;
  size_t bad = 0;
  EXPECT_EQ(ReadKnownSitesFile(path_, &readable, &bad),
            (std::vector<std::string>{kSiteA, kSiteB}));
  EXPECT_TRUE(readable);
  EXPECT_EQ(bad, 3u);
}

TEST_F(KnownSitesFileTest, AMissingFileIsReadableAndEmpty) {
  bool readable = false;
  EXPECT_TRUE(ReadKnownSitesFile(path_, &readable).empty());
  EXPECT_TRUE(readable);
}

TEST_F(KnownSitesFileTest, ACorruptOrOtherVersionFileIsNotReadable) {
  for (const std::string& content :
       {std::string("# pagespeed-sites v2\n") + kSiteA + "\n", kSiteA + "\n",
        std::string(""), std::string("\x00\x01garbage", 9)}) {
    Put(content);
    bool readable = true;
    EXPECT_TRUE(ReadKnownSitesFile(path_, &readable).empty());
    EXPECT_FALSE(readable) << "content of " << content.size() << " bytes";
  }
}

TEST_F(KnownSitesFileTest, TheWriterCoalescesChanges) {
  std::mutex mu;
  std::vector<std::string> sites;
  bool changed = false;
  auto take = [&]() -> std::optional<std::vector<std::string>> {
    std::lock_guard<std::mutex> lock(mu);
    if (!changed) return std::nullopt;
    changed = false;
    return sites;
  };
  KnownSitesWriter writer(path_, take, std::chrono::milliseconds(300));
  writer.Start();
  // Many changes inside one interval.
  for (int i = 0; i < 50; ++i) {
    std::lock_guard<std::mutex> lock(mu);
    sites = {i % 2 == 0 ? kSiteA : kSiteB, kSiteC};
    changed = true;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  EXPECT_EQ(writer.writes(), 1u) << "fifty changes, one write";
  // No change, no write.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  EXPECT_EQ(writer.writes(), 1u);
  // A change just before the stop is still written.
  {
    std::lock_guard<std::mutex> lock(mu);
    sites = {kSiteA};
    changed = true;
  }
  writer.Stop();
  EXPECT_EQ(writer.writes(), 2u);
  bool readable = false;
  EXPECT_EQ(ReadKnownSitesFile(path_, &readable),
            (std::vector<std::string>{kSiteA}));
}

}  // namespace
}  // namespace pagespeed
