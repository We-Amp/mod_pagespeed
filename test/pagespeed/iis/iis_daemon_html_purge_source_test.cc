// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_daemon_html_purge_source.h"

#include <memory>

#include "pagespeed/kernel/base/mem_file_system.h"
#include "pagespeed/kernel/base/mock_timer.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/thread/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kFile[] = "/cache/cache.purge";
const char kUrl[] = "https://www.example.test/page";

// The purge state the server's own options hold, as a test sets it.
class FakeSource : public DaemonHtmlPurgeSource {
 public:
  bool IsUrlValidIgnoringGlobal(const GoogleString& url,
                                int64 time_ms) const override {
    return !(url == purged_url && time_ms <= purged_ms);
  }
  int64 GlobalInvalidationTimestampMs() const override { return global_ms; }
  bool FlushFileMtimeMs(int64* mtime_ms) const override { return false; }

  GoogleString purged_url;
  int64 purged_ms = -1;
  int64 global_ms = -1;
};

class IisDaemonHtmlPurgeSourceTest : public ::testing::Test {
 protected:
  IisDaemonHtmlPurgeSourceTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(new NullMutex, MockTimer::kApr_5_2010_ms),
        file_system_(thread_system_.get(), &timer_) {
    source_ = std::make_unique<IisDaemonHtmlPurgeSource>(
        &system_, &file_system_, &timer_, kFile);
  }

  void WriteFile(const GoogleString& contents) {
    ASSERT_TRUE(file_system_.WriteFile(kFile, contents, &handler_));
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  MemFileSystem file_system_;
  NullMessageHandler handler_;
  FakeSource system_;
  std::unique_ptr<IisDaemonHtmlPurgeSource> source_;
};

TEST_F(IisDaemonHtmlPurgeSourceTest, APurgeAnotherProcessWroteIsSeenAtOnce) {
  const int64 now = timer_.NowMs();
  EXPECT_TRUE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 10));
  // Another worker process purges the URL: the system options have not
  // polled it in yet.
  WriteFile(StrCat("-1\n", Integer64ToString(now - 5), " ", kUrl, "\n"));
  EXPECT_FALSE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 10));
  EXPECT_TRUE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 1));
  EXPECT_TRUE(source_->IsUrlValidIgnoringGlobal(
      "https://www.example.test/other", now - 10));
}

TEST_F(IisDaemonHtmlPurgeSourceTest, ThePurgesTheOptionsHoldAreNeverLost) {
  // This process's own purge, not yet in the file it reads.
  const int64 now = timer_.NowMs();
  system_.purged_url = kUrl;
  system_.purged_ms = now - 5;
  WriteFile("-1\n");
  EXPECT_FALSE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 10));
}

TEST_F(IisDaemonHtmlPurgeSourceTest, TheNewerGlobalPurgeOfTheTwoCounts) {
  const int64 now = timer_.NowMs();
  system_.global_ms = now - 100;
  EXPECT_EQ(now - 100, source_->GlobalInvalidationTimestampMs());
  WriteFile(StrCat(Integer64ToString(now - 50), "\n"));
  EXPECT_EQ(now - 50, source_->GlobalInvalidationTimestampMs());
  system_.global_ms = now - 10;
  EXPECT_EQ(now - 10, source_->GlobalInvalidationTimestampMs());
}

TEST_F(IisDaemonHtmlPurgeSourceTest, AMissingOrMalformedFileAddsNothing) {
  const int64 now = timer_.NowMs();
  EXPECT_TRUE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 10));
  WriteFile(StrCat("x\n", Integer64ToString(now - 5), " ", kUrl, "\n"));
  EXPECT_TRUE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 10));
  // A record in the far future is refused, as the purge context refuses it.
  WriteFile(StrCat("-1\n", Integer64ToString(now + 3600 * 1000), " ", kUrl, "\n"));
  EXPECT_TRUE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 10));
}

TEST_F(IisDaemonHtmlPurgeSourceTest, AFileRewrittenWithinOneSecondIsReadAgain) {
  // Same size, same second: re-read once the read is a second old.
  const int64 now = timer_.NowMs();
  WriteFile(StrCat("-1\n", Integer64ToString(now - 5), " https://www.example.test/aaaa\n"));
  EXPECT_TRUE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 10));
  WriteFile(StrCat("-1\n", Integer64ToString(now - 5), " ", kUrl, "\n"));
  timer_.AdvanceMs(1000);
  EXPECT_FALSE(source_->IsUrlValidIgnoringGlobal(kUrl, now - 10));
}

}  // namespace
}  // namespace net_instaweb
