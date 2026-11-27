// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for AtomicWriteFile: content integrity, file mode, and —
// issue #1401 — a truthful errno on every failure leg (open/write/fsync/
// close/rename), preserved across temp-file cleanup so callers can log the
// real reason a persist failed.

#include "lib/base/atomic_file_writer.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "lib/base/posix_compat.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>

#include <atomic>
#endif

#include "gtest/gtest.h"

#if defined(__linux__)
// ---------------------------------------------------------------------------
// Fault-injection shims (Linux only).
//
// AtomicWriteFile calls unlink/close/fsync directly; defining them here
// interposes on the dynamic symbols for calls made from this test binary.
// An armed shim fails once with an injected errno; disarmed, the call passes
// through to the real syscall. This makes temp-file *cleanup* clobber errno
// deterministically — successful cleanup syscalls do not touch errno on
// Linux, which is exactly why the errno preservation in AtomicWriteFile
// would otherwise have no observable effect to pin a test on.
// ---------------------------------------------------------------------------
namespace {

// errno value to inject on the next qualifying call; 0 = disarmed.
std::atomic<int> g_unlink_inject{0};
std::atomic<int> g_close_inject{0};
std::atomic<int> g_fsync_inject{0};

bool PathExists(const char* path) {
  struct stat st;
  return ::stat(path, &st) == 0;
}

}  // namespace

extern "C" int unlink(const char* path) {
  int inject = g_unlink_inject.load();
  // Only fail the unlink of a path that actually exists: AtomicWriteFile's
  // initial stale-temp unlink targets a nonexistent path and must pass
  // through (failing it with the injected errno would fire one leg early).
  if (inject != 0 && PathExists(path)) {
    g_unlink_inject.store(0);
    errno = inject;
    return -1;
  }
  return static_cast<int>(::syscall(SYS_unlinkat, AT_FDCWD, path, 0));
}

extern "C" int close(int fd) {
  int inject = g_close_inject.exchange(0);
  if (inject != 0) {
    // Do NOT actually close: fd state is unspecified after a failed close,
    // and AtomicWriteFile deliberately does not retry it. The fd leaks until
    // process exit — harmless in a test.
    errno = inject;
    return -1;
  }
  return static_cast<int>(::syscall(SYS_close, fd));
}

extern "C" int fsync(int fd) {
  int inject = g_fsync_inject.exchange(0);
  if (inject != 0) {
    errno = inject;
    return -1;
  }
  return static_cast<int>(::syscall(SYS_fsync, fd));
}
#endif  // defined(__linux__)

namespace pagespeed {
namespace {

#ifdef _WIN32
int TestPid() { return _getpid(); }
#else
int TestPid() { return getpid(); }
#endif

class AtomicFileWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() /
               ("atomic_writer_test_" + std::to_string(TestPid()));
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

  static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
  }

  std::filesystem::path tmp_dir_;
};

TEST_F(AtomicFileWriterTest, WritesContentAtomically) {
  std::string path = (tmp_dir_ / "out.txt").string();
  ASSERT_TRUE(AtomicWriteFile(path, "hello world", 0644));
  EXPECT_EQ(ReadFile(path), "hello world");
  // No temp file left behind.
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

TEST_F(AtomicFileWriterTest, WritesEmptyContent) {
  std::string path = (tmp_dir_ / "empty.txt").string();
  ASSERT_TRUE(AtomicWriteFile(path, "", 0644));
  EXPECT_EQ(ReadFile(path), "");
}

TEST_F(AtomicFileWriterTest, ReplacesExistingFile) {
  std::string path = (tmp_dir_ / "out.txt").string();
  ASSERT_TRUE(AtomicWriteFile(path, "old", 0644));
  ASSERT_TRUE(AtomicWriteFile(path, "new-content", 0644));
  EXPECT_EQ(ReadFile(path), "new-content");
}

// A reader that holds the destination open (the way the web server module
// holds the generation file for a moment on each check) must not block the
// replace, and must keep seeing the old content through its open handle.
TEST_F(AtomicFileWriterTest, ReplacesFileWhileReaderHoldsItOpen) {
  std::string path = (tmp_dir_ / "held.gen").string();
  ASSERT_TRUE(AtomicWriteFile(path, "1\n", 0644));

#ifdef _WIN32
  auto open_reader = [&path]() {
    return CreateFileA(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  };
  auto read_old = [](HANDLE h) {
    char buf[8] = {};
    DWORD got = 0;
    EXPECT_TRUE(::ReadFile(h, buf, sizeof(buf), &got, nullptr));
    return std::string(buf, got);
  };

  // End to end: the replace through AtomicWriteFile succeeds while the
  // reader holds the file, and the reader still sees the old content.
  HANDLE reader = open_reader();
  ASSERT_NE(reader, INVALID_HANDLE_VALUE);
  EXPECT_TRUE(AtomicWriteFile(path, "2\n", 0644));
  EXPECT_EQ(read_old(reader), "1\n");
  CloseHandle(reader);
  EXPECT_EQ(ReadFile(path), "2\n");

  // The POSIX-semantics rename itself must succeed (no silent fallback).
  reader = open_reader();
  ASSERT_NE(reader, INVALID_HANDLE_VALUE);
  const std::string staged = path + ".staged";
  {
    std::ofstream(staged, std::ios::binary) << "3\n";
  }
  EXPECT_TRUE(
      ps_compat_detail::PosixSemanticsRename(staged.c_str(), path.c_str()));
  EXPECT_EQ(read_old(reader), "2\n");
  CloseHandle(reader);
  EXPECT_EQ(ReadFile(path), "3\n");
#else
  int reader = ::open(path.c_str(), O_RDONLY);
  ASSERT_GE(reader, 0);
  EXPECT_TRUE(AtomicWriteFile(path, "2\n", 0644));
  EXPECT_EQ(ReadFile(path), "2\n");
  // The open descriptor still reads the file it was opened on.
  char old_buf[8] = {};
  ssize_t got = ::read(reader, old_buf, sizeof(old_buf));
  EXPECT_EQ(std::string(old_buf, got > 0 ? static_cast<size_t>(got) : 0),
            "1\n");
  ::close(reader);
#endif
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

// Open leg: creating the temp file in a nonexistent directory fails, and
// errno names the reason.
TEST_F(AtomicFileWriterTest, OpenFailureReportsErrno) {
  errno = 0;
  EXPECT_FALSE(AtomicWriteFile((tmp_dir_ / "no_such_dir" / "out.txt").string(),
                               "x", 0644));
#ifdef _WIN32
  // The Windows CRT reports EACCES for a missing intermediate directory.
  EXPECT_EQ(errno, EACCES);
#else
  EXPECT_EQ(errno, ENOENT);
#endif
}

#ifndef _WIN32

TEST_F(AtomicFileWriterTest, AppliesMode) {
  std::string path = (tmp_dir_ / "secret.txt").string();
  ASSERT_TRUE(AtomicWriteFile(path, "s3cr3t", 0600));
  struct stat st;
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(static_cast<unsigned int>(st.st_mode) & 0777u, 0600u);
}

// Write leg, short write (issue #1401 review): RLIMIT_FSIZE caps the file
// at 100 bytes, so writing 4 KiB returns a positive short count and then
// fails with EFBIG. The writer must retry the short write and report the
// REAL reason — a bare write() would leave the pre-existing (stale) errno
// in place, misreporting e.g. "No such file or directory" for a full
// filesystem, or "Success".
TEST_F(AtomicFileWriterTest, ShortWriteIsRetriedAndReportsRealReason) {
  std::string path = (tmp_dir_ / "capped.txt").string();

  struct rlimit saved;
  ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &saved), 0);
  struct rlimit cap = saved;
  cap.rlim_cur = 100;
  ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &cap), 0);
  // SIGXFSZ's default action kills the process on the capped write.
  struct sigaction sa = {};
  sa.sa_handler = SIG_IGN;
  struct sigaction saved_sa = {};
  ASSERT_EQ(::sigaction(SIGXFSZ, &sa, &saved_sa), 0);

  // Reproduce the reported misreport: a stale ENOENT from an unrelated
  // earlier unlink is sitting in errno when the write fails.
  errno = ENOENT;
  bool ok = AtomicWriteFile(path, std::string(4096, 'x'), 0644);
  int captured = errno;

  // Restore limits before any assertion can abort the test.
  EXPECT_EQ(::setrlimit(RLIMIT_FSIZE, &saved), 0);
  EXPECT_EQ(::sigaction(SIGXFSZ, &saved_sa, nullptr), 0);

  EXPECT_FALSE(ok);
  EXPECT_EQ(captured, EFBIG)
      << "stale errno leaked into the diagnostic: " << std::strerror(captured);
  // The temp file was cleaned up; the destination never appears.
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

#endif  // _WIN32

#if defined(__linux__)
// The remaining tests interpose on libc symbols — a Linux-linker trick that
// macOS's two-level namespaces and the Windows CRT do not support.

// Write/fsync leg, errno preservation: fsync fails (injected EIO) and the
// cleanup unlink then clobbers errno (injected EACCES). The preserved
// reason must be the fsync failure, not the cleanup failure.
TEST_F(AtomicFileWriterTest, FsyncFailureReasonSurvivesCleanup) {
  std::string path = (tmp_dir_ / "out.txt").string();
  g_fsync_inject.store(EIO);
  g_unlink_inject.store(EACCES);

  errno = 0;
  EXPECT_FALSE(AtomicWriteFile(path, "content", 0644));
  EXPECT_EQ(errno, EIO) << "cleanup clobbered the reason: "
                        << std::strerror(errno);

  // The injected unlink failed, so the temp file is still there.
  std::filesystem::remove(path + ".tmp");
}

// Close leg, errno preservation: close fails (injected EIO) and the cleanup
// unlink clobbers errno. The preserved reason must be the close failure.
TEST_F(AtomicFileWriterTest, CloseFailureReasonSurvivesCleanup) {
  std::string path = (tmp_dir_ / "out.txt").string();
  g_close_inject.store(EIO);
  g_unlink_inject.store(EACCES);

  errno = 0;
  EXPECT_FALSE(AtomicWriteFile(path, "content", 0644));
  EXPECT_EQ(errno, EIO) << "cleanup clobbered the reason: "
                        << std::strerror(errno);

  std::filesystem::remove(path + ".tmp");
}

// Rename leg, errno preservation: the destination is an existing directory,
// so rename fails EISDIR, and the cleanup unlink clobbers errno. The
// preserved reason must be the rename failure.
TEST_F(AtomicFileWriterTest, RenameFailureReasonSurvivesCleanup) {
  std::filesystem::path target = tmp_dir_ / "out.txt";
  std::filesystem::create_directories(target);
  g_unlink_inject.store(EACCES);

  errno = 0;
  EXPECT_FALSE(AtomicWriteFile(target.string(), "content", 0644));
  EXPECT_EQ(errno, EISDIR) << "cleanup clobbered the reason: "
                           << std::strerror(errno);

  std::filesystem::remove(target.string() + ".tmp");
}

#endif  // defined(__linux__)

}  // namespace
}  // namespace pagespeed
