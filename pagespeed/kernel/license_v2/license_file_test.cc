// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for license file I/O, with emphasis on the read-failure diagnostic
//: a license file that exists but cannot be read must be
// distinguishable from an absent file so the operator gets an actionable
// signal instead of the generic UNLICENSED banner.

#include "pagespeed/kernel/license_v2/license_file.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32) || defined(WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {
namespace {

// Returns a process+counter-unique temp path that does not yet exist, so each
// test case operates on its own file without colliding with parallel runs.
std::filesystem::path UniqueTempPath() {
  static std::atomic<int> counter{0};
  const long pid = static_cast<long>(getpid());
  const int n = counter.fetch_add(1);
  std::filesystem::path dir = std::filesystem::temp_directory_path();
  return dir / ("license_file_test." + std::to_string(pid) + "." +
                std::to_string(n) + ".license");
}

class LicenseFileTest : public ::testing::Test {
 protected:
  void TearDown() override {
    std::error_code ec;
    // Restore perms first so removal succeeds even after a chmod(000) case.
    if (!path_.empty()) {
      std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace, ec);
      std::filesystem::remove(path_, ec);
    }
  }

  void WriteRaw(const std::string& contents) {
    std::ofstream f(path_, std::ios::out | std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(f.is_open());
    f << contents;
    f.close();
  }

  std::filesystem::path path_ = UniqueTempPath();
};

TEST_F(LicenseFileTest, AbsentFileIsAbsentNotUnreadable) {
  // No file written. Must report kAbsent (the normal unlicensed state), so the
  // diagnostic path does NOT fire for an install that simply has no license.
  GoogleString token = "untouched";
  LicenseFileReadStatus status = LicenseFileReadStatus::kOk;
  EXPECT_FALSE(ReadLicenseFile(path_, &token, &status));
  EXPECT_EQ(status, LicenseFileReadStatus::kAbsent);
}

TEST_F(LicenseFileTest, ValidFileReadsToken) {
  WriteRaw("  my.license.token  \n");
  GoogleString token;
  LicenseFileReadStatus status = LicenseFileReadStatus::kAbsent;
  EXPECT_TRUE(ReadLicenseFile(path_, &token, &status));
  EXPECT_EQ(status, LicenseFileReadStatus::kOk);
  EXPECT_EQ(token, "my.license.token");  // trimmed
}

TEST_F(LicenseFileTest, EmptyFileIsUnreadable) {
  WriteRaw("   \n\t ");  // whitespace only
  GoogleString token = "untouched";
  LicenseFileReadStatus status = LicenseFileReadStatus::kAbsent;
  EXPECT_FALSE(ReadLicenseFile(path_, &token, &status));
  EXPECT_EQ(status, LicenseFileReadStatus::kUnreadable);
  EXPECT_TRUE(token.empty());
}

TEST_F(LicenseFileTest, OversizedFileIsUnreadable) {
  WriteRaw(std::string(8192, 'A'));  // > kMaxLicenseFileSize (2048)
  GoogleString token;
  LicenseFileReadStatus status = LicenseFileReadStatus::kAbsent;
  EXPECT_FALSE(ReadLicenseFile(path_, &token, &status));
  EXPECT_EQ(status, LicenseFileReadStatus::kUnreadable);
}

// The core case: a file that exists but is not readable by the
// worker. Skipped when the test runs as root, since root bypasses the
// permission bits and would read the file regardless.
TEST_F(LicenseFileTest, UnreadablePermissionsAreReportedNotAbsent) {
#if defined(_WIN32) || defined(WIN32)
  GTEST_SKIP() << "POSIX permission semantics not applicable on Windows";
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "running as root bypasses file permission bits";
  }
  WriteRaw("some.token.value\n");
  // Remove all permissions: open() should fail with EACCES.
  std::error_code ec;
  std::filesystem::permissions(path_, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace, ec);
  ASSERT_FALSE(ec) << ec.message();

  GoogleString token = "untouched";
  LicenseFileReadStatus status = LicenseFileReadStatus::kAbsent;
  EXPECT_FALSE(ReadLicenseFile(path_, &token, &status));
  // The key assertion: the file is present-but-unreadable, NOT reported as
  // absent (which is what previously masked the misconfiguration).
  EXPECT_EQ(status, LicenseFileReadStatus::kUnreadable);
#endif
}

// The legacy 2-arg overload must keep its boolean contract for existing callers.
TEST_F(LicenseFileTest, TwoArgOverloadStillWorks) {
  WriteRaw("token-via-legacy-api\n");
  GoogleString token;
  EXPECT_TRUE(ReadLicenseFile(path_, &token));
  EXPECT_EQ(token, "token-via-legacy-api");
}

}  // namespace
}  // namespace net_instaweb
