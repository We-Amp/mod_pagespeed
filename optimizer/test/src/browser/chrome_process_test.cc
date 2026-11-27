// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for ChromeProcess (lifecycle, page counting).
// These tests do NOT spawn Chrome — they test internal logic only.

#include "src/browser/chrome_process.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "uv.h"

namespace pagespeed {
namespace {

class ChromeProcessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    ASSERT_EQ(0, uv_loop_init(loop_));
  }

  void TearDown() override {
    // Close all libuv handles.
    uv_walk(
        loop_,
        [](uv_handle_t* h, void*) {
          if (!uv_is_closing(h)) uv_close(h, nullptr);
        },
        nullptr);
    for (int i = 0; i < 10; i++) {
      if (uv_run(loop_, UV_RUN_NOWAIT) == 0) break;
    }
    uv_loop_close(loop_);
    delete loop_;
  }

  uv_loop_t* loop_ = nullptr;
};

// --- T3: IncrementPageCount tests ---

TEST_F(ChromeProcessTest, IncrementPageCountReturnsFalse) {
  ChromeProcessConfig config;
  config.recycle_after_pages = 5;
  ChromeProcess chrome(loop_, config);

  // First 4 increments should return false.
  for (int i = 0; i < 4; i++) {
    EXPECT_FALSE(chrome.IncrementPageCount())
        << "Should not recycle at page " << (i + 1);
  }
}

TEST_F(ChromeProcessTest, IncrementPageCountReturnsTrueAtThreshold) {
  ChromeProcessConfig config;
  config.recycle_after_pages = 3;
  ChromeProcess chrome(loop_, config);

  EXPECT_FALSE(chrome.IncrementPageCount());  // page 1
  EXPECT_FALSE(chrome.IncrementPageCount());  // page 2
  EXPECT_TRUE(chrome.IncrementPageCount());   // page 3 = threshold
}

TEST_F(ChromeProcessTest, IncrementPageCountReturnsTruePastThreshold) {
  ChromeProcessConfig config;
  config.recycle_after_pages = 2;
  ChromeProcess chrome(loop_, config);

  EXPECT_FALSE(chrome.IncrementPageCount());  // page 1
  EXPECT_TRUE(chrome.IncrementPageCount());   // page 2
  EXPECT_TRUE(chrome.IncrementPageCount());   // page 3 (past)
}

TEST_F(ChromeProcessTest, IncrementPageCountDisabledWhenZero) {
  ChromeProcessConfig config;
  config.recycle_after_pages = 0;  // Disabled.
  ChromeProcess chrome(loop_, config);

  // Should never return true.
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(chrome.IncrementPageCount());
  }
}

TEST_F(ChromeProcessTest, IncrementPageCountWithThresholdOne) {
  ChromeProcessConfig config;
  config.recycle_after_pages = 1;
  ChromeProcess chrome(loop_, config);

  // First increment should return true.
  EXPECT_TRUE(chrome.IncrementPageCount());
}

TEST_F(ChromeProcessTest, DefaultStateNotRunning) {
  ChromeProcessConfig config;
  ChromeProcess chrome(loop_, config);

  EXPECT_FALSE(chrome.running());
  EXPECT_EQ(0, chrome.rss_mb());
}

TEST_F(ChromeProcessTest, StopWhenNotRunningIsNoop) {
  ChromeProcessConfig config;
  ChromeProcess chrome(loop_, config);

  // Should not crash or hang.
  chrome.Stop();
  EXPECT_FALSE(chrome.running());
}

TEST_F(ChromeProcessTest, StartFailsWithInvalidChromePath) {
  ChromeProcessConfig config;
  config.chrome_path = "/nonexistent/path/to/chrome";
  ChromeProcess chrome(loop_, config);

  auto status = chrome.Start();
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsNotFound(status));
  EXPECT_FALSE(chrome.running());
}

TEST_F(ChromeProcessTest, DoubleStartFailsWhenAlreadyRunning) {
  // Verify that Start() on a non-running process with a bad
  // path fails and allows retry (not stuck in bad state).
  ChromeProcessConfig config;
  config.chrome_path = "/nonexistent/chrome";
  ChromeProcess chrome(loop_, config);

  auto status1 = chrome.Start();
  EXPECT_FALSE(status1.ok());
  EXPECT_FALSE(chrome.running());

  // Should be able to try again.
  auto status2 = chrome.Start();
  EXPECT_FALSE(status2.ok());
  EXPECT_FALSE(chrome.running());
}

// ---- Additional coverage: config defaults, state queries ----

TEST_F(ChromeProcessTest, ConfigDefaults) {
  ChromeProcessConfig config;
  // Verify sensible defaults.
  EXPECT_FALSE(config.chrome_path.empty());  // Has a default Chrome path.
  EXPECT_GE(config.recycle_after_pages, 0);
}

TEST_F(ChromeProcessTest, PidBeforeStart) {
  ChromeProcessConfig config;
  ChromeProcess chrome(loop_, config);
  // Before Start, pid should be 0 or -1.
  EXPECT_LE(chrome.pid(), 0);
}

TEST_F(ChromeProcessTest, RssMbBeforeStart) {
  ChromeProcessConfig config;
  ChromeProcess chrome(loop_, config);
  EXPECT_EQ(chrome.rss_mb(), 0);
}

TEST_F(ChromeProcessTest, CdpClientNullBeforeStart) {
  ChromeProcessConfig config;
  ChromeProcess chrome(loop_, config);
  EXPECT_EQ(chrome.cdp_client(), nullptr);
}

TEST_F(ChromeProcessTest, StopIdempotent) {
  ChromeProcessConfig config;
  ChromeProcess chrome(loop_, config);
  // Multiple stops should be harmless.
  chrome.Stop();
  chrome.Stop();
  chrome.Stop();
  EXPECT_FALSE(chrome.running());
}

TEST_F(ChromeProcessTest, SetExitCallbackBeforeStart) {
  ChromeProcessConfig config;
  ChromeProcess chrome(loop_, config);
  bool called = false;
  chrome.SetExitCallback([&](int64_t, int) { called = true; });
  // Callback shouldn't fire when not started.
  chrome.Stop();
  EXPECT_FALSE(called);
}

TEST_F(ChromeProcessTest, RecycleAfterPagesLargeValue) {
  ChromeProcessConfig config;
  config.recycle_after_pages = 1000000;
  ChromeProcess chrome(loop_, config);

  // Should never trigger recycling.
  for (int i = 0; i < 100; ++i) {
    EXPECT_FALSE(chrome.IncrementPageCount());
  }
}

// ---- Dead-resolver SSRF backstop: MAP * ~NOTFOUND --------------------------

// The single most important launch flag for the network-trust model: Chrome must
// never resolve a hostname itself (all egress goes through the out-of-Chrome
// IP-pinned fetcher). The only way to break it is to delete the line, so assert it
// is present unconditionally.
TEST(ChromeProcessArgs, DeadResolverFlagAlwaysPresent) {
  ChromeProcessConfig config;
  auto args = ChromeProcess::BuildChromeArgs(config);
  EXPECT_NE(std::find(args.begin(), args.end(),
                      "--host-resolver-rules=MAP * ~NOTFOUND"),
            args.end());
}

TEST(ChromeProcessArgs, NoWebRtcUdpFlagPresentRegardlessOfConfig) {
  for (bool headless : {false, true}) {
    for (bool no_sandbox : {false, true}) {
      for (const char* udd : {"", "/tmp/x"}) {
        ChromeProcessConfig config;
        config.headless = headless;
        config.no_sandbox = no_sandbox;
        config.user_data_dir = udd;
        auto args = ChromeProcess::BuildChromeArgs(config);
        EXPECT_NE(
            std::find(args.begin(), args.end(),
                      "--webrtc-ip-handling-policy=disable_non_proxied_udp"),
            args.end());
        // Before the start URL, so Chrome reads it as a switch.
        EXPECT_EQ(args.back(), "about:blank");
      }
    }
  }
}

TEST(ChromeProcessArgs, DeadResolverFlagPresentRegardlessOfConfig) {
  // Present whether or not the conditional flags (headless, user_data_dir) fire.
  for (bool headless : {false, true}) {
    for (const char* udd : {"", "/tmp/x"}) {
      ChromeProcessConfig config;
      config.headless = headless;
      config.user_data_dir = udd;
      auto args = ChromeProcess::BuildChromeArgs(config);
      EXPECT_NE(std::find(args.begin(), args.end(),
                          "--host-resolver-rules=MAP * ~NOTFOUND"),
                args.end())
          << "headless=" << headless << " udd=" << udd;
      EXPECT_EQ(args.front(), config.chrome_path);  // argv[0]
      EXPECT_EQ(args.back(), "about:blank");
    }
  }
}

// ---- Sandbox flag contract -------------------------------------------------

// The gate itself: --no-sandbox used to be pushed on every launch, which made
// every headless render unsandboxed.  It must now appear if and ONLY if the
// operator opted out.
TEST(ChromeProcessArgs, NoSandboxFlagOnlyWhenOptedOut) {
  ChromeProcessConfig sandboxed;
  auto args = ChromeProcess::BuildChromeArgs(sandboxed);
  EXPECT_EQ(std::find(args.begin(), args.end(), "--no-sandbox"), args.end())
      << "the default must not disable the Chrome sandbox";

  ChromeProcessConfig opted_out;
  opted_out.no_sandbox = true;
  args = ChromeProcess::BuildChromeArgs(opted_out);
  EXPECT_NE(std::find(args.begin(), args.end(), "--no-sandbox"), args.end());
}

// --no-zygote would disable the very layer-1 architecture the sandbox is, so
// it must never appear regardless of mode.
TEST(ChromeProcessArgs, NeverDisablesTheZygote) {
  for (bool no_sandbox : {false, true}) {
    ChromeProcessConfig config;
    config.no_sandbox = no_sandbox;
    auto args = ChromeProcess::BuildChromeArgs(config);
    EXPECT_EQ(std::find(args.begin(), args.end(), "--no-zygote"), args.end())
        << "no_sandbox=" << no_sandbox;
  }
}

// The profile directory is pinned by the daemon; when it is set the flag is
// emitted verbatim, in both sandbox modes.
TEST(ChromeProcessArgs, UserDataDirIsEmittedInEveryMode) {
  for (bool no_sandbox : {false, true}) {
    ChromeProcessConfig config;
    config.no_sandbox = no_sandbox;
    config.user_data_dir = "/run/pagespeed-optimizer/chrome";
    auto args = ChromeProcess::BuildChromeArgs(config);
    EXPECT_NE(std::find(args.begin(), args.end(),
                        "--user-data-dir=/run/pagespeed-optimizer/chrome"),
              args.end())
        << "no_sandbox=" << no_sandbox;
  }
}

// H4 must not weaken the SSRF backstop while it is changing the flag set.
TEST(ChromeProcessArgs, DeadResolverSurvivesEverySandboxMode) {
  for (bool no_sandbox : {false, true}) {
    ChromeProcessConfig config;
    config.no_sandbox = no_sandbox;
    auto args = ChromeProcess::BuildChromeArgs(config);
    EXPECT_NE(std::find(args.begin(), args.end(),
                        "--host-resolver-rules=MAP * ~NOTFOUND"),
              args.end())
        << "no_sandbox=" << no_sandbox;
    EXPECT_NE(std::find(args.begin(), args.end(), "--disable-dev-shm-usage"),
              args.end())
        << "no_sandbox=" << no_sandbox;
  }
}

// ---- Spawn environment (issue #1467) ---------------------------------------

// Chrome resolves its crash-report database from $XDG_CONFIG_HOME, else
// $HOME/.config, and CHECK-fails at launch when that cannot be created.  A
// privilege-dropped daemon still carries the launcher's HOME, so with a pinned
// profile the daemon must hand Chrome a HOME it owns and strip the XDG_* home
// overrides that would win over it.
TEST(ChromeProcessEnv, HomeIsPinnedToTheProfileDirectory) {
  ChromeProcessConfig config;
  config.user_data_dir = "/run/pagespeed-optimizer/chrome";
  const char* base[] = {"PATH=/usr/bin",
                        "HOME=/root",
                        "XDG_CONFIG_HOME=/root/.config",
                        "XDG_CACHE_HOME=/root/.cache",
                        "XDG_DATA_HOME=/root/.local/share",
                        "LANG=C",
                        nullptr};
  auto env = ChromeProcess::BuildChromeEnv(config, const_cast<char**>(base));
  std::vector<std::string> expected = {"PATH=/usr/bin", "LANG=C",
                                       "HOME=/run/pagespeed-optimizer/chrome"};
  EXPECT_EQ(env, expected);
}

TEST(ChromeProcessEnv, HomeIsAddedWhenAbsent) {
  ChromeProcessConfig config;
  config.user_data_dir = "/data/chrome";
  const char* base[] = {"PATH=/usr/bin", nullptr};
  auto env = ChromeProcess::BuildChromeEnv(config, const_cast<char**>(base));
  std::vector<std::string> expected = {"PATH=/usr/bin", "HOME=/data/chrome"};
  EXPECT_EQ(env, expected);
}

// Without a pinned profile Chrome picks its own paths from the environment,
// so the environment must pass through untouched (including a null base).
TEST(ChromeProcessEnv, PassesThroughWithoutAProfileDirectory) {
  ChromeProcessConfig config;
  const char* base[] = {"HOME=/root", "XDG_CONFIG_HOME=/root/.config", nullptr};
  auto env = ChromeProcess::BuildChromeEnv(config, const_cast<char**>(base));
  std::vector<std::string> expected = {"HOME=/root",
                                       "XDG_CONFIG_HOME=/root/.config"};
  EXPECT_EQ(env, expected);
  EXPECT_TRUE(ChromeProcess::BuildChromeEnv(config, nullptr).empty());
}

// A variable whose name merely starts with HOME is not HOME.
TEST(ChromeProcessEnv, OnlyExactHomeVariablesAreReplaced) {
  ChromeProcessConfig config;
  config.user_data_dir = "/data/chrome";
  const char* base[] = {"HOMEBREW_PREFIX=/opt/homebrew", "HOME=/root", nullptr};
  auto env = ChromeProcess::BuildChromeEnv(config, const_cast<char**>(base));
  std::vector<std::string> expected = {"HOMEBREW_PREFIX=/opt/homebrew",
                                       "HOME=/data/chrome"};
  EXPECT_EQ(env, expected);
}

// --- Stale SingletonLock reconciliation (issue #1475) ---

namespace {
bool NeverAlive(int) { return false; }
bool AlwaysAlive(int) { return true; }
}  // namespace

TEST(SingletonLock, NoTargetMeansNoLock) {
  auto v = ChromeProcess::ClassifySingletonLock("", "host-a", AlwaysAlive);
  EXPECT_EQ(v.action, SingletonLockAction::kNoLock);
}

// A recreated container has a new hostname: the previous owner's lock is
// stale whatever its pid says (that pid space belonged to another container).
TEST(SingletonLock, OtherHostIsStaleEvenIfPidIsAlive) {
  auto v = ChromeProcess::ClassifySingletonLock("old-container-4242",
                                                "new-container", AlwaysAlive);
  EXPECT_EQ(v.action, SingletonLockAction::kRemoveStale);
  EXPECT_EQ(v.owner_host, "old-container");
  EXPECT_EQ(v.owner_pid, 4242);
}

TEST(SingletonLock, SameHostDeadPidIsStale) {
  auto v =
      ChromeProcess::ClassifySingletonLock("host-a-4242", "host-a", NeverAlive);
  EXPECT_EQ(v.action, SingletonLockAction::kRemoveStale);
  EXPECT_EQ(v.owner_host, "host-a");
  EXPECT_EQ(v.owner_pid, 4242);
}

TEST(SingletonLock, SameHostLivePidIsARealConflict) {
  int asked = 0;
  auto v = ChromeProcess::ClassifySingletonLock("host-a-4242", "host-a",
                                                [&](int pid) {
                                                  asked = pid;
                                                  return true;
                                                });
  EXPECT_EQ(v.action, SingletonLockAction::kKeepLive);
  EXPECT_EQ(asked, 4242);
}

// Hostnames may contain dashes: only the LAST dash separates the pid.
TEST(SingletonLock, SplitsAtTheLastDash) {
  auto v = ChromeProcess::ClassifySingletonLock("web-01-eu-77", "web-01-eu",
                                                AlwaysAlive);
  EXPECT_EQ(v.action, SingletonLockAction::kKeepLive);
  EXPECT_EQ(v.owner_host, "web-01-eu");
  EXPECT_EQ(v.owner_pid, 77);
}

// Nothing running can own a lock Chrome would not have written.
TEST(SingletonLock, UnparseableTargetIsStale) {
  for (const char* target : {"host-a-", "host-a-abc", "nodash", "host-a-0"}) {
    auto v =
        ChromeProcess::ClassifySingletonLock(target, "host-a", AlwaysAlive);
    EXPECT_EQ(v.action, SingletonLockAction::kRemoveStale) << target;
    EXPECT_EQ(v.owner_pid, 0) << target;
  }
}

#ifndef _WIN32
class SingletonLockDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/singleton-lock-XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(tmpl));
    dir_ = tmpl;
  }
  void TearDown() override {
    for (const char* n :
         {"SingletonLock", "SingletonSocket", "SingletonCookie"}) {
      ::unlink((dir_ + "/" + n).c_str());
    }
    ::rmdir(dir_.c_str());
  }
  bool Exists(const char* name) const {
    struct stat st{};
    return ::lstat((dir_ + "/" + name).c_str(), &st) == 0;
  }
  std::string dir_;
};

TEST_F(SingletonLockDirTest, EmptyProfileIsANoOp) {
  auto r = ChromeProcess::ReconcileSingletonLock(dir_, "host-a", NeverAlive);
  EXPECT_EQ(r.verdict.action, SingletonLockAction::kNoLock);
  EXPECT_FALSE(r.removed);
  EXPECT_TRUE(r.error.empty());
}

TEST_F(SingletonLockDirTest, StaleLockRemovesAllThreeEntries) {
  ASSERT_EQ(0, ::symlink("otherhost-12345", (dir_ + "/SingletonLock").c_str()));
  ASSERT_EQ(0, ::symlink("/nonexistent", (dir_ + "/SingletonSocket").c_str()));
  ASSERT_EQ(0, ::symlink("cookie", (dir_ + "/SingletonCookie").c_str()));
  auto r = ChromeProcess::ReconcileSingletonLock(dir_, "host-a", AlwaysAlive);
  EXPECT_EQ(r.verdict.action, SingletonLockAction::kRemoveStale);
  EXPECT_EQ(r.verdict.owner_host, "otherhost");
  EXPECT_EQ(r.verdict.owner_pid, 12345);
  EXPECT_TRUE(r.removed);
  EXPECT_TRUE(r.error.empty()) << r.error;
  EXPECT_FALSE(Exists("SingletonLock"));
  EXPECT_FALSE(Exists("SingletonSocket"));
  EXPECT_FALSE(Exists("SingletonCookie"));
}

// Only the lock present (socket/cookie already gone): ENOENT is not an error.
TEST_F(SingletonLockDirTest, MissingSiblingsAreNotAnError) {
  ASSERT_EQ(0, ::symlink("host-a-99999", (dir_ + "/SingletonLock").c_str()));
  auto r = ChromeProcess::ReconcileSingletonLock(dir_, "host-a", NeverAlive);
  EXPECT_EQ(r.verdict.action, SingletonLockAction::kRemoveStale);
  EXPECT_TRUE(r.removed);
  EXPECT_TRUE(r.error.empty()) << r.error;
  EXPECT_FALSE(Exists("SingletonLock"));
}

TEST_F(SingletonLockDirTest, LiveLockIsLeftInPlace) {
  ASSERT_EQ(0, ::symlink("host-a-4242", (dir_ + "/SingletonLock").c_str()));
  ASSERT_EQ(0, ::symlink("cookie", (dir_ + "/SingletonCookie").c_str()));
  auto r = ChromeProcess::ReconcileSingletonLock(dir_, "host-a", AlwaysAlive);
  EXPECT_EQ(r.verdict.action, SingletonLockAction::kKeepLive);
  EXPECT_FALSE(r.removed);
  EXPECT_TRUE(Exists("SingletonLock"));
  EXPECT_TRUE(Exists("SingletonCookie"));
}

// A regular file named SingletonLock is not something Chrome wrote and it
// never clears itself: remove it as stale.
TEST_F(SingletonLockDirTest, RegularFileLockIsStale) {
  FILE* f = ::fopen((dir_ + "/SingletonLock").c_str(), "w");
  ASSERT_NE(nullptr, f);
  ::fclose(f);
  auto r = ChromeProcess::ReconcileSingletonLock(dir_, "host-a", AlwaysAlive);
  EXPECT_EQ(r.verdict.action, SingletonLockAction::kRemoveStale);
  EXPECT_EQ(r.verdict.owner_pid, 0);
  EXPECT_TRUE(r.removed);
  EXPECT_TRUE(r.error.empty()) << r.error;
  EXPECT_FALSE(Exists("SingletonLock"));
}

// A sibling that cannot be unlinked is reported, not fatal; the lock itself
// still goes.
TEST_F(SingletonLockDirTest, UnlinkFailureIsReported) {
  ASSERT_EQ(0, ::symlink("otherhost-12345", (dir_ + "/SingletonLock").c_str()));
  ASSERT_EQ(0, ::mkdir((dir_ + "/SingletonSocket").c_str(), 0700));
  auto r = ChromeProcess::ReconcileSingletonLock(dir_, "host-a", AlwaysAlive);
  EXPECT_EQ(r.verdict.action, SingletonLockAction::kRemoveStale);
  EXPECT_TRUE(r.removed);
  EXPECT_NE(r.error.find("SingletonSocket"), std::string::npos) << r.error;
  EXPECT_FALSE(Exists("SingletonLock"));
  ::rmdir((dir_ + "/SingletonSocket").c_str());
}

// Default predicate: our own pid is alive, so a lock naming it is kept.
TEST_F(SingletonLockDirTest, DefaultPredicateSeesOurOwnPidAsAlive) {
  char host[256] = {0};
  ASSERT_EQ(0, ::gethostname(host, sizeof(host) - 1));
  const std::string target =
      std::string(host) + "-" + std::to_string(::getpid());
  ASSERT_EQ(0, ::symlink(target.c_str(), (dir_ + "/SingletonLock").c_str()));
  auto r = ChromeProcess::ReconcileSingletonLock(dir_);
  EXPECT_EQ(r.verdict.action, SingletonLockAction::kKeepLive);
  EXPECT_TRUE(Exists("SingletonLock"));
}
#endif

}  // namespace
}  // namespace pagespeed
