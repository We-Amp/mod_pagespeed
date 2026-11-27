// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_purge_source.h"

#include <memory>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/purge_set.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mem_file_system.h"
#include "test/pagespeed/kernel/base/mock_timer.h"

namespace net_instaweb {
namespace {

const int64 kNowMs = MockTimer::kApr_5_2010_ms;

class DaemonHtmlPurgeSourceTest : public testing::Test {
 protected:
  DaemonHtmlPurgeSourceTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(new NullMutex, kNowMs),
        file_system_(thread_system_.get(), &timer_),
        options_(thread_system_.get()) {}

  static void SetUpTestSuite() { RewriteOptions::Initialize(); }
  static void TearDownTestSuite() { RewriteOptions::Terminate(); }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  MemFileSystem file_system_;
  RewriteOptions options_;
  NullMessageHandler handler_;
};

TEST_F(DaemonHtmlPurgeSourceTest, TheFlushFileIsTheLegacyFlushsFile) {
  EXPECT_EQ("/cache/cache.flush", DaemonHtmlCacheFlushPath("", "/cache"));
  EXPECT_EQ("/cache/other", DaemonHtmlCacheFlushPath("other", "/cache"));
  EXPECT_EQ("/abs/flush", DaemonHtmlCacheFlushPath("/abs/flush", "/cache"));
  EXPECT_EQ("", DaemonHtmlCacheFlushPath("", ""));
}

TEST_F(DaemonHtmlPurgeSourceTest, UrlPurgesAndTheGlobalTimestampAreApart) {
  SystemDaemonHtmlPurgeSource source(&options_, &file_system_,
                                     "/cache/cache.flush");
  EXPECT_EQ(-1, source.GlobalInvalidationTimestampMs());
  options_.UpdateCacheInvalidationTimestampMs(1000);
  CopyOnWrite<PurgeSet> purges(options_.purge_set());
  purges.MakeWriteable()->Put("http://a.test/page", 3000);
  ASSERT_TRUE(options_.UpdateCachePurgeSet(purges));

  EXPECT_EQ(1000, source.GlobalInvalidationTimestampMs());
  // The global time is read separately, never through the URL check.
  EXPECT_TRUE(source.IsUrlValidIgnoringGlobal("http://a.test/other", 500));
  EXPECT_FALSE(source.IsUrlValidIgnoringGlobal("http://a.test/page", 3000));
  EXPECT_TRUE(source.IsUrlValidIgnoringGlobal("http://a.test/page", 3001));
}

TEST_F(DaemonHtmlPurgeSourceTest, TheFlushFileCountsOnlyWithoutCachePurge) {
  StdioFileSystem disk;
  const GoogleString path = StrCat(GTestTempDir(), "/purge_source.flush");
  disk.RemoveFile(path.c_str(), &handler_);
  SystemDaemonHtmlPurgeSource source(&options_, &disk, path);
  int64 mtime_ms = 0;
  EXPECT_FALSE(source.FlushFileMtimeMs(&mtime_ms));  // Not there.
  ASSERT_TRUE(disk.WriteFile(path.c_str(), "", &handler_));
  int64 mtime_s = 0;
  ASSERT_TRUE(disk.Mtime(path, &mtime_s, &handler_));
  ASSERT_TRUE(source.FlushFileMtimeMs(&mtime_ms));
  EXPECT_EQ(mtime_s * Timer::kSecondMs, mtime_ms);

  // With EnableCachePurge on the module's purges go through the purge set,
  // and a touch of the file is not one.
  options_.set_enable_cache_purge(true);
  EXPECT_FALSE(source.FlushFileMtimeMs(&mtime_ms));
  options_.set_enable_cache_purge(false);

  SystemDaemonHtmlPurgeSource no_path(&options_, &disk, "");
  EXPECT_FALSE(no_path.FlushFileMtimeMs(&mtime_ms));
  disk.RemoveFile(path.c_str(), &handler_);
}

}  // namespace
}  // namespace net_instaweb
