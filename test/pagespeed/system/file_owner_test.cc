// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// SetOwnerThroughDescriptor: what the root process gives to the workers'
// user, and what it leaves alone.

#include "pagespeed/system/file_owner.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class FileOwnerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* tmp = getenv("TEST_TMPDIR");
    GoogleString root = (tmp != nullptr && *tmp != '\0') ? tmp : "/tmp";
    GoogleString pattern = StrCat(root, "/file_owner_test_XXXXXX");
    ASSERT_NE(nullptr, mkdtemp(&pattern[0]));
    dir_ = pattern;
  }

  GoogleString Path(StringPiece name) const { return StrCat(dir_, "/", name); }

  static void WriteFile(const GoogleString& path) {
    std::ofstream out(path.c_str());
    out << "contents";
  }

  SetOwnerResult SetToSelf(const GoogleString& path, OwnerMatch match,
                           GoogleString* why) {
    return SetOwnerThroughDescriptor(path, geteuid(), getegid(), match, why);
  }

  GoogleString dir_;
};

TEST_F(FileOwnerTest, HandsOverADirectory) {
  const GoogleString dir = Path("cache");
  ASSERT_EQ(0, mkdir(dir.c_str(), 0755));
  GoogleString why;
  EXPECT_EQ(SetOwnerResult::kDone,
            SetToSelf(dir, OwnerMatch::kUserAndGroup, &why))
      << why;
  EXPECT_EQ(SetOwnerResult::kDone, SetToSelf(dir, OwnerMatch::kUserOnly, &why))
      << why;
}

TEST_F(FileOwnerTest, HandsOverASingleLinkRegularFile) {
  const GoogleString file = Path("data");
  WriteFile(file);
  GoogleString why;
  EXPECT_EQ(SetOwnerResult::kDone,
            SetToSelf(file, OwnerMatch::kUserAndGroup, &why))
      << why;
}

TEST_F(FileOwnerTest, LeavesASymbolicLinkAlone) {
  const GoogleString target = Path("elsewhere");
  const GoogleString link = Path("cache");
  ASSERT_EQ(0, mkdir(target.c_str(), 0755));
  ASSERT_EQ(0, symlink(target.c_str(), link.c_str()));
  GoogleString why;
  EXPECT_EQ(SetOwnerResult::kSymbolicLink,
            SetToSelf(link, OwnerMatch::kUserAndGroup, &why));
  EXPECT_EQ("not a plain file or directory", why);
}

TEST_F(FileOwnerTest, IgnoresTrailingSlashes) {
  const GoogleString dir = Path("cache");
  const GoogleString target = Path("elsewhere");
  const GoogleString link = Path("linked");
  ASSERT_EQ(0, mkdir(dir.c_str(), 0755));
  ASSERT_EQ(0, mkdir(target.c_str(), 0755));
  ASSERT_EQ(0, symlink(target.c_str(), link.c_str()));
  GoogleString why;
  EXPECT_EQ(SetOwnerResult::kDone,
            SetToSelf(StrCat(dir, "//"), OwnerMatch::kUserAndGroup, &why))
      << why;
  EXPECT_EQ(SetOwnerResult::kSymbolicLink,
            SetToSelf(StrCat(link, "/"), OwnerMatch::kUserAndGroup, &why));
}

TEST_F(FileOwnerTest, RefusesAFifo) {
  const GoogleString fifo = Path("cache");
  ASSERT_EQ(0, mkfifo(fifo.c_str(), 0600));
  GoogleString why;
  EXPECT_EQ(SetOwnerResult::kFailed,
            SetToSelf(fifo, OwnerMatch::kUserAndGroup, &why));
  EXPECT_EQ("not a plain file or directory", why);
}

TEST_F(FileOwnerTest, RefusesAFileWithASecondLink) {
  const GoogleString other = Path("elsewhere");
  const GoogleString file = Path("data");
  WriteFile(other);
  ASSERT_EQ(0, link(other.c_str(), file.c_str()));
  GoogleString why;
  EXPECT_EQ(SetOwnerResult::kFailed,
            SetToSelf(file, OwnerMatch::kUserAndGroup, &why));
  EXPECT_EQ("not a plain file or directory", why);
}

TEST_F(FileOwnerTest, ReportsAMissingPath) {
  GoogleString why;
  EXPECT_EQ(SetOwnerResult::kFailed,
            SetToSelf(Path("missing"), OwnerMatch::kUserAndGroup, &why));
  EXPECT_FALSE(why.empty());
}

TEST_F(FileOwnerTest, ChangesTheOwnerAsRoot) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "needs root";
  }
  constexpr uid_t kWorker = 65534;  // conventionally "nobody"
  const GoogleString dir = Path("cache");
  ASSERT_EQ(0, mkdir(dir.c_str(), 0755));
  GoogleString why;
  ASSERT_EQ(SetOwnerResult::kDone,
            SetOwnerThroughDescriptor(dir, kWorker, kWorker,
                                      OwnerMatch::kUserAndGroup, &why))
      << why;
  struct stat st;
  ASSERT_EQ(0, stat(dir.c_str(), &st));
  EXPECT_EQ(kWorker, st.st_uid);
  EXPECT_EQ(static_cast<gid_t>(kWorker), st.st_gid);
}

}  // namespace
}  // namespace net_instaweb
