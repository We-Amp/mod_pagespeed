// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/cache_dir.h"

#ifdef _WIN32
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"

namespace pagespeed {
namespace {

class CacheDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() /
               ("cache_dir_test_" + std::to_string(
#ifdef _WIN32
                                        _getpid()
#else
                                        getpid()
#endif
                                            ));
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

  std::string MakeFile(const std::string& dir, const std::string& name) {
    std::string path = dir + "/" + name;
    std::ofstream f(path);
    f << "x";
    return path;
  }

  std::filesystem::path tmp_dir_;
};

TEST_F(CacheDirTest, ExistingWritableDirIsOk) {
  std::string detail;
  EXPECT_EQ(ValidateCacheDir(tmp_dir_.string(), "cache", &detail),
            CacheDirStatus::kOk);
}

TEST_F(CacheDirTest, DirWithOwnContentIsOk) {
  MakeFile(tmp_dir_.string(), "cache-1-0123456789abcdef.dat");
  MakeFile(tmp_dir_.string(), "pagespeed-shared.conf");
  std::string detail;
  EXPECT_EQ(ValidateCacheDir(tmp_dir_.string(), "cache", &detail),
            CacheDirStatus::kOk);
}

// The diagnosing statuses are POSIX-only: on Windows the access model is
// ACLs rather than uid/mode, the Windows service story is a separate arc,
// and ValidateCacheDir is a documented always-kOk stub there (see the header).
#ifndef _WIN32
TEST_F(CacheDirTest, MissingDirIsReportedAbsent) {
  std::string detail;
  EXPECT_EQ(
      ValidateCacheDir((tmp_dir_ / "no-such-dir").string(), "cache", &detail),
      CacheDirStatus::kMissing);
}

TEST_F(CacheDirTest, RegularFileIsNotADirectory) {
  std::string file = MakeFile(tmp_dir_.string(), "a-file");
  std::string detail;
  EXPECT_EQ(ValidateCacheDir(file, "cache", &detail),
            CacheDirStatus::kNotADirectory);
}
#else
// Pin the stub rather than merely skipping: a Windows build that started
// diagnosing would be a silent change of contract for every call site.
TEST_F(CacheDirTest, WindowsValidationIsAlwaysOk) {
  std::string detail;
  EXPECT_EQ(
      ValidateCacheDir((tmp_dir_ / "no-such-dir").string(), "cache", &detail),
      CacheDirStatus::kOk);
  std::string file = MakeFile(tmp_dir_.string(), "a-file");
  EXPECT_EQ(ValidateCacheDir(file, "cache", &detail), CacheDirStatus::kOk);
}
#endif  // !_WIN32

TEST_F(CacheDirTest, RefuseReturnsTrueOnlyForOk) {
  // The handler-less path must not crash and must route every non-ok
  // status to a refusal.
  EXPECT_TRUE(
      RefuseCacheDir(CacheDirStatus::kOk, tmp_dir_.string(), "", nullptr));
  EXPECT_FALSE(RefuseCacheDir(CacheDirStatus::kMissing, "/x", "", nullptr));
  EXPECT_FALSE(
      RefuseCacheDir(CacheDirStatus::kNotADirectory, "/x", "d", nullptr));
  EXPECT_FALSE(
      RefuseCacheDir(CacheDirStatus::kNotWritable, "/x", "d", nullptr));
  EXPECT_FALSE(
      RefuseCacheDir(CacheDirStatus::kForeignOwned, "/x", "d", nullptr));
  EXPECT_FALSE(RefuseCacheDir(CacheDirStatus::kStatFailed, "/x", "d", nullptr));
}

#ifndef _WIN32
TEST_F(CacheDirTest, UnwritableDirIsPermissionDeniedNotAbsent) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "root ignores mode bits; the EACCES path is untestable";
  }
  std::string dir = (tmp_dir_ / "locked").string();
  std::filesystem::create_directories(dir);
  ASSERT_EQ(::chmod(dir.c_str(), 0500), 0);
  std::string detail;
  EXPECT_EQ(ValidateCacheDir(dir, "cache", &detail),
            CacheDirStatus::kNotWritable);
  ::chmod(dir.c_str(), 0700);  // let TearDown remove it
}

TEST_F(CacheDirTest, ForeignOwnedVolumeFileIsRefused) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "creating foreign-owned content needs root";
  }
  // Named after the volume stem, so it is a file the daemon itself would
  // rewrite: that is the case worth refusing on.
  std::string victim = MakeFile(tmp_dir_.string(), "cache-6-0123456789abcdef");
  ASSERT_EQ(::chown(victim.c_str(), 1, 1), 0);  // uid 1 = daemon/bin
  std::string detail;
  EXPECT_EQ(ValidateCacheDir(tmp_dir_.string(), "cache", &detail),
            CacheDirStatus::kForeignOwned);
  // The log detail must name the offending file and both uids.
  EXPECT_NE(detail.find("cache-6-0123456789abcdef"), std::string::npos);
  EXPECT_EQ(0, ::chown(victim.c_str(), geteuid(), -1));
}

TEST_F(CacheDirTest, ForeignOwnedSidecarIsRefused) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "creating foreign-owned content needs root";
  }
  std::string victim = MakeFile(tmp_dir_.string(), "pagespeed-shared.conf");
  ASSERT_EQ(::chown(victim.c_str(), 1, 1), 0);
  std::string detail;
  EXPECT_EQ(ValidateCacheDir(tmp_dir_.string(), "cache", &detail),
            CacheDirStatus::kForeignOwned);
  EXPECT_EQ(0, ::chown(victim.c_str(), geteuid(), -1));
}

TEST_F(CacheDirTest, ForeignOwnedUnrelatedFileIsTolerated) {
  // The cache directory is group-writable BY DESIGN (the web-server peers
  // share the daemon's group), and a filesystem's own lost+found is
  // root-owned.  A directory-wide ownership rule would let any group member
  // permanently refuse the daemon's start by creating one file.  Files the
  // daemon never writes are not its business.
  if (geteuid() != 0) {
    GTEST_SKIP() << "creating foreign-owned content needs root";
  }
  std::string bystander = MakeFile(tmp_dir_.string(), "someone-elses-file");
  ASSERT_EQ(::chown(bystander.c_str(), 1, 1), 0);
  std::string lost = (tmp_dir_ / "lost+found").string();
  ASSERT_EQ(::mkdir(lost.c_str(), 0700), 0);
  ASSERT_EQ(::chown(lost.c_str(), 0, 0), 0);

  std::string detail;
  EXPECT_EQ(ValidateCacheDir(tmp_dir_.string(), "cache", &detail),
            CacheDirStatus::kOk)
      << detail;

  EXPECT_EQ(0, ::chown(bystander.c_str(), geteuid(), -1));
  ::rmdir(lost.c_str());
}
#endif

// The enumeration itself: which names the daemon claims as its own.
TEST_F(CacheDirTest, DaemonAuthoredEntriesAreRecognised) {
  // Stem-named: the volume files, the generation file, staged temporaries.
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("cache", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("cache-6-0123456789abcdef", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("cache.gen", "cache"));
  // An EXTENSIONED --cache-path (cache.vol) still resolves to the stem
  // "cache", and the volume it produces carries the extension at the end.
  // Matching on the full basename instead would miss it and let the
  // daemon's own volume escape the ownership check.
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("cache-6-abc.vol", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("cache.vol", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("cache.vol.gen", "cache"));
  // The fixed sidecars and their atomic-write temporaries.
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("pagespeed-shared.conf", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("pagespeed-shared.conf.tmp", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry(".pagespeed-serve-stats", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("pagespeed.instance-id", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("pagespeed.license", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("pagespeed-hosts.conf", "cache"));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("pagespeed.json", "cache"));
  EXPECT_TRUE(
      IsDaemonAuthoredCacheEntry("pagespeed-webbotauth-keys.conf", "cache"));
  EXPECT_TRUE(
      IsDaemonAuthoredCacheEntry("pagespeed-rslcap-keys.conf", "cache"));
  // Everything else belongs to somebody else.
  EXPECT_FALSE(IsDaemonAuthoredCacheEntry("lost+found", "cache"));
  EXPECT_FALSE(IsDaemonAuthoredCacheEntry("someone-elses-file", "cache"));
  EXPECT_FALSE(IsDaemonAuthoredCacheEntry("pagespeed", "cache"));
  EXPECT_FALSE(IsDaemonAuthoredCacheEntry("notcache-6-abc", "cache"));
  // An empty stem must not turn every name into a daemon file.
  EXPECT_FALSE(IsDaemonAuthoredCacheEntry("anything", ""));
  EXPECT_TRUE(IsDaemonAuthoredCacheEntry("pagespeed-shared.conf", ""));
}

}  // namespace
}  // namespace pagespeed
