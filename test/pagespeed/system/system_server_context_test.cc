// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// How server contexts pick up a touch of cache.flush (the legacy whole-cache
// flush, used when cache purging is off). Every context polls its own
// cache.flush; a context that sees a newer flush publishes its timestamp in a
// statistic that every context sharing the statistics can read, so the others
// follow before their own poll is due. Two contexts on the same cache
// directory stand for two server processes of one site: whatever each has
// seen, they must end up with the same options signature, which is part of
// the property-cache key, or a page reads different stored data depending on
// the process that serves it.

#include "pagespeed/system/system_server_context.h"

#include <memory>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "test/net/instaweb/rewriter/custom_rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mem_file_system.h"

namespace net_instaweb {

namespace {

#ifdef _WIN32
const char kSiteCachePath[] = "C:/cache-site";
const char kOtherCachePath[] = "C:/cache-other";
#else
const char kSiteCachePath[] = "/cache-site";
const char kOtherCachePath[] = "/cache-other";
#endif

const int64 kPollIntervalSec = 5;

// MemFileSystem reports a time of 0 for a file that does not exist; a real
// file system reports a failure, which is what the server relies on before
// the first touch of cache.flush.
class FlushFileSystem : public MemFileSystem {
 public:
  FlushFileSystem(ThreadSystem* threads, Timer* timer)
      : MemFileSystem(threads, timer) {}

  bool Mtime(const StringPiece& path, int64* timestamp_sec,
             MessageHandler* handler) override {
    GoogleString name(path.data(), path.size());
    if (!Exists(name.c_str(), handler).is_true()) {
      return false;
    }
    return MemFileSystem::Mtime(path, timestamp_sec, handler);
  }
};

class SystemServerContextFlushTest
    : public CustomRewriteTestBase<SystemRewriteOptions> {
 protected:
  void SetUp() override {
    CustomRewriteTestBase<SystemRewriteOptions>::SetUp();
    // One set of statistics for every context, the way processes share
    // their shared-memory statistics (and, without per-host statistics,
    // virtual hosts share them too).
    stats_ = std::make_unique<SimpleStats>(factory()->thread_system());
    SystemServerContext::InitStats(stats_.get());
    file_system_ = std::make_unique<FlushFileSystem>(factory()->thread_system(),
                                                     factory()->timer());
  }

  std::unique_ptr<SystemServerContext> NewContext(const char* cache_path) {
    auto context = std::make_unique<SystemServerContext>(
        factory(), "flush-test.example.com", 80);
    SystemRewriteOptions* options = context->global_system_rewrite_options();
    options->set_file_cache_path(cache_path);
    options->set_cache_flush_poll_interval_sec(kPollIntervalSec);
    options->ComputeSignature();
    context->set_statistics(stats_.get());
    context->set_timer(factory()->timer());
    context->set_file_system(file_system_.get());
    context->set_message_handler(message_handler());
    return context;
  }

  // Touches <cache_path>/cache.flush now and returns its timestamp in ms, at
  // the whole-second resolution the server reads it with.
  int64 TouchCacheFlush(const char* cache_path) {
    GoogleString name = StrCat(cache_path, "/cache.flush");
    EXPECT_TRUE(file_system_->WriteFile(name.c_str(), "", message_handler()));
    int64 mtime_sec = 0;
    NullMessageHandler null_handler;
    EXPECT_TRUE(file_system_->Mtime(name, &mtime_sec, &null_handler));
    return mtime_sec * Timer::kSecondMs;
  }

  static int64 FlushTimestampMs(SystemServerContext* context) {
    return context->global_options()->has_cache_invalidation_timestamp_ms()
               ? context->global_options()->cache_invalidation_timestamp()
               : -1;
  }

  std::unique_ptr<SimpleStats> stats_;
  std::unique_ptr<FlushFileSystem> file_system_;
};

// A context that has never applied a flush follows a sibling that found one,
// instead of keeping its old signature until its own poll is due.
TEST_F(SystemServerContextFlushTest, NeverFlushedSiblingFollowsBeforeItsPoll) {
  std::unique_ptr<SystemServerContext> busy = NewContext(kSiteCachePath);
  std::unique_ptr<SystemServerContext> quiet = NewContext(kSiteCachePath);

  busy->FlushCacheIfNecessary();  // First poll: no cache.flush yet.
  AdvanceTimeMs(2 * Timer::kSecondMs);
  quiet->FlushCacheIfNecessary();  // Its first poll, also before the touch.
  AdvanceTimeMs(1 * Timer::kSecondMs);
  int64 flush_ms = TouchCacheFlush(kSiteCachePath);
  AdvanceTimeMs(2 * Timer::kSecondMs);

  busy->FlushCacheIfNecessary();   // Its poll is due: applies the flush.
  quiet->FlushCacheIfNecessary();  // Its own poll is 2 s away.

  EXPECT_EQ(flush_ms, FlushTimestampMs(busy.get()));
  EXPECT_EQ(flush_ms, FlushTimestampMs(quiet.get()));
  EXPECT_EQ(busy->global_options()->signature(),
            quiet->global_options()->signature());
}

// A context that applied an earlier flush follows a sibling to a later one.
TEST_F(SystemServerContextFlushTest, FlushedSiblingFollowsBeforeItsPoll) {
  std::unique_ptr<SystemServerContext> busy = NewContext(kSiteCachePath);
  std::unique_ptr<SystemServerContext> quiet = NewContext(kSiteCachePath);
  TouchCacheFlush(kSiteCachePath);
  AdvanceTimeMs(1 * Timer::kSecondMs);
  busy->FlushCacheIfNecessary();
  AdvanceTimeMs(2 * Timer::kSecondMs);
  quiet->FlushCacheIfNecessary();
  AdvanceTimeMs(1 * Timer::kSecondMs);
  int64 flush_ms = TouchCacheFlush(kSiteCachePath);
  AdvanceTimeMs(2 * Timer::kSecondMs);

  busy->FlushCacheIfNecessary();
  quiet->FlushCacheIfNecessary();

  EXPECT_EQ(flush_ms, FlushTimestampMs(busy.get()));
  EXPECT_EQ(flush_ms, FlushTimestampMs(quiet.get()));
  EXPECT_EQ(busy->global_options()->signature(),
            quiet->global_options()->signature());
}

// The statistic is shared with contexts that watch another cache.flush (other
// virtual hosts when statistics are not kept per host). A process of the site
// that happens to serve a request just after another host's flush must not
// take that flush for its own: a process that did not see it would keep the
// site's own timestamp, and the two would never agree again.
TEST_F(SystemServerContextFlushTest, AnotherHostsFlushDoesNotSplitTheSite) {
  std::unique_ptr<SystemServerContext> other = NewContext(kOtherCachePath);
  std::unique_ptr<SystemServerContext> busy = NewContext(kSiteCachePath);
  std::unique_ptr<SystemServerContext> quiet = NewContext(kSiteCachePath);

  other->FlushCacheIfNecessary();  // First poll: no cache.flush yet.
  AdvanceTimeMs(2 * Timer::kSecondMs);
  int64 site_flush_ms = TouchCacheFlush(kSiteCachePath);
  busy->FlushCacheIfNecessary();
  quiet->FlushCacheIfNecessary();
  ASSERT_EQ(site_flush_ms, FlushTimestampMs(busy.get()));
  ASSERT_EQ(site_flush_ms, FlushTimestampMs(quiet.get()));

  // The other host is flushed and polls first; the busy process of the site
  // serves a request right after, before its own poll is due.
  AdvanceTimeMs(4 * Timer::kSecondMs);
  int64 other_flush_ms = TouchCacheFlush(kOtherCachePath);
  other->FlushCacheIfNecessary();
  ASSERT_EQ(other_flush_ms, FlushTimestampMs(other.get()));
  busy->FlushCacheIfNecessary();

  // Later both processes of the site poll their own cache.flush.
  AdvanceTimeMs(kPollIntervalSec * Timer::kSecondMs);
  busy->FlushCacheIfNecessary();
  quiet->FlushCacheIfNecessary();

  EXPECT_EQ(site_flush_ms, FlushTimestampMs(busy.get()));
  EXPECT_EQ(site_flush_ms, FlushTimestampMs(quiet.get()));
  EXPECT_EQ(busy->global_options()->signature(),
            quiet->global_options()->signature());
}

}  // namespace

}  // namespace net_instaweb
