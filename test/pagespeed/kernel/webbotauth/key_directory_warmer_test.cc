// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Unit tests for KeyDirectoryWarmer::RefreshOnce -- the synchronous fetch+
// populate cycle of the design record A2 background warmer. Two fakes drive the real
// SyncFetcherAdapterCallback: an inline fake (completes the fetch synchronously
// inside Fetch(), exercising the already-done fast path plus status/success/
// size-cap handling and the D4 no-clobber-on-failure invariant), and a deferred
// fake (completes from a SEPARATE thread after a brief delay, so the warmer
// actually blocks in TimedWait and is woken cross-thread -- the production path).
// The warmer thread itself is not started; RefreshOnce is exercised directly.

#include "pagespeed/kernel/webbotauth/key_directory_warmer.h"

#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/posix_timer.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/webbotauth/base64.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"

namespace net_instaweb {
namespace webbotauth {
namespace {

const char kX[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const char kX2[] = "BAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

GoogleString TwoKeyDoc() {
  return StrCat("{\"keys\":[",
                "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\",\"x\":\"",
                kX, "\"},",
                "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k2\",\"x\":\"",
                kX2, "\"}]}");
}

// Completes the AsyncFetch inline (synchronously, inside Fetch). Parameterizable
// status/success so the post-fetch failure branches of FetchJwksBody are
// reachable. Records call count.
class FakeFetcher : public UrlAsyncFetcher {
 public:
  explicit FakeFetcher(GoogleString body) : body_(std::move(body)) {}
  void set_status(int status) { status_ = status; }
  void set_success(bool success) { success_ = success; }
  void set_body(GoogleString body) { body_ = std::move(body); }
  void Fetch(const GoogleString& url, MessageHandler* handler,
             AsyncFetch* fetch) override {
    ++calls_;
    fetch->response_headers()->SetStatusAndReason(
        static_cast<HttpStatus::Code>(status_));
    fetch->Write(body_, handler);
    fetch->Done(success_);
  }
  int calls() const { return calls_; }

 private:
  GoogleString body_;
  int status_ = HttpStatus::kOK;
  bool success_ = true;
  int calls_ = 0;
};

// Completes the AsyncFetch from a SEPARATE thread after a brief delay, so the
// warmer genuinely blocks in SyncFetcherAdapterCallback::TimedWait and is woken
// by the cross-thread Done(). The generous fetch_timeout_ms in the test keeps
// the result deterministic regardless of scheduling.
class DeferredFetcher : public UrlAsyncFetcher {
 public:
  DeferredFetcher(ThreadSystem* thread_system, GoogleString body)
      : thread_system_(thread_system), body_(std::move(body)) {}
  ~DeferredFetcher() override {
    for (size_t i = 0; i < completers_.size(); ++i) {
      completers_[i]->Join();
      delete completers_[i];
    }
  }
  void Fetch(const GoogleString& url, MessageHandler* handler,
             AsyncFetch* fetch) override {
    ++calls_;
    Completer* c = new Completer(thread_system_, fetch, handler, body_);
    completers_.push_back(c);
    c->Start();
  }
  int calls() const { return calls_; }

 private:
  class Completer : public ThreadSystem::Thread {
   public:
    Completer(ThreadSystem* thread_system, AsyncFetch* fetch,
              MessageHandler* handler, GoogleString body)
        : Thread(thread_system, "deferred_completer", ThreadSystem::kJoinable),
          thread_system_(thread_system),
          fetch_(fetch),
          handler_(handler),
          body_(std::move(body)) {}
    void Run() override {
      // Brief portable delay (an unsignaled timed wait) so the warmer reaches
      // TimedWait before we complete.
      std::unique_ptr<ThreadSystem::CondvarCapableMutex> m(
          thread_system_->NewMutex());
      std::unique_ptr<ThreadSystem::Condvar> c(m->NewCondvar());
      {
        ScopedMutex lock(m.get());
        c->TimedWait(25);
      }
      fetch_->response_headers()->SetStatusAndReason(HttpStatus::kOK);
      fetch_->Write(body_, handler_);
      fetch_->Done(true);
    }

   private:
    ThreadSystem* thread_system_;
    AsyncFetch* fetch_;
    MessageHandler* handler_;
    GoogleString body_;
  };

  ThreadSystem* thread_system_;
  GoogleString body_;
  int calls_ = 0;
  std::vector<Completer*> completers_;
};

class KeyDirectoryWarmerTest : public ::testing::Test {
 protected:
  KeyDirectoryWarmerTest()
      : thread_system_(Platform::CreateThreadSystem()), cache_(1 << 16) {}

  KeyDirectoryWarmer::Config BaseConfig() {
    KeyDirectoryWarmer::Config cfg;
    cfg.realm = "wba";
    cfg.host = "keys.example.com";
    cfg.url = "https://keys.example.com/.well-known/jwks.json";
    cfg.allowlist.insert("https://keys.example.com");
    cfg.fetch_timeout_ms = 5000;  // generous: deferred Done (25ms) always wins
    cfg.positive_ttl_sec = 3600;
    return cfg;
  }

  KeyLookupResult ReadCacheOnly(const char* kid) {
    CachedKeyDirectoryProvider cache_only(nullptr, &cache_, "wba",
                                          /*read_through=*/false);
    const int64_t now = timer_.NowMs() / Timer::kSecondMs;
    return cache_only.GetKey("keys.example.com", kid, now);
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  PosixTimer timer_;
  NullMessageHandler handler_;
  LRUCache cache_;
};

TEST_F(KeyDirectoryWarmerTest, FetchPopulatesCacheAndRequestPathResolves) {
  FakeFetcher fetcher(TwoKeyDoc());
  KeyDirectoryWarmer warmer(BaseConfig(), &fetcher, &cache_,
                            thread_system_.get(), &timer_, &handler_);
  EXPECT_EQ(2, warmer.RefreshOnce());
  EXPECT_EQ(1, fetcher.calls());
  EXPECT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k1").status);
  EXPECT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k2").status);
  EXPECT_EQ(KeyLookupResult::kNotFound, ReadCacheOnly("nope").status);
}

// The cross-thread blocking wait: the fetch completes from another thread while
// the warmer is parked in TimedWait. Result must still be the warmed keys.
TEST_F(KeyDirectoryWarmerTest, DeferredCrossThreadCompletionResolves) {
  DeferredFetcher fetcher(thread_system_.get(), TwoKeyDoc());
  KeyDirectoryWarmer warmer(BaseConfig(), &fetcher, &cache_,
                            thread_system_.get(), &timer_, &handler_);
  EXPECT_EQ(2, warmer.RefreshOnce());
  EXPECT_EQ(1, fetcher.calls());
  EXPECT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k1").status);
}

// D4 no-clobber: a FAILED refresh must write nothing and leave the previously
// warmed keys intact (they lapse only on their own TTL).
TEST_F(KeyDirectoryWarmerTest, FailedRefreshDoesNotClobberWarmedKeys) {
  FakeFetcher fetcher(TwoKeyDoc());
  KeyDirectoryWarmer warmer(BaseConfig(), &fetcher, &cache_,
                            thread_system_.get(), &timer_, &handler_);
  ASSERT_EQ(2, warmer.RefreshOnce());  // warm
  ASSERT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k1").status);

  fetcher.set_success(false);  // next refresh fails after the fetch
  EXPECT_EQ(0, warmer.RefreshOnce());
  // Keys from the prior successful warm survive (no clobber, no empty write).
  EXPECT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k1").status);
  EXPECT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k2").status);
}

TEST_F(KeyDirectoryWarmerTest, Non200ResponseWritesNothing) {
  FakeFetcher fetcher(TwoKeyDoc());
  fetcher.set_status(HttpStatus::kNotFound);  // 404
  KeyDirectoryWarmer warmer(BaseConfig(), &fetcher, &cache_,
                            thread_system_.get(), &timer_, &handler_);
  EXPECT_EQ(0, warmer.RefreshOnce());
  EXPECT_EQ(1, fetcher.calls());  // it did fetch, but the result was rejected
  EXPECT_EQ(KeyLookupResult::kNotFound, ReadCacheOnly("k1").status);
}

TEST_F(KeyDirectoryWarmerTest, UnsuccessfulFetchWritesNothing) {
  FakeFetcher fetcher(TwoKeyDoc());
  fetcher.set_success(false);
  KeyDirectoryWarmer warmer(BaseConfig(), &fetcher, &cache_,
                            thread_system_.get(), &timer_, &handler_);
  EXPECT_EQ(0, warmer.RefreshOnce());
  EXPECT_EQ(KeyLookupResult::kNotFound, ReadCacheOnly("k1").status);
}

TEST_F(KeyDirectoryWarmerTest, OversizedBodyIsRejected) {
  FakeFetcher fetcher(TwoKeyDoc());
  KeyDirectoryWarmer::Config cfg = BaseConfig();
  cfg.max_response_bytes = 16;  // smaller than the JWKS body
  KeyDirectoryWarmer warmer(cfg, &fetcher, &cache_, thread_system_.get(),
                            &timer_, &handler_);
  EXPECT_EQ(0, warmer.RefreshOnce());
  EXPECT_EQ(KeyLookupResult::kNotFound, ReadCacheOnly("k1").status);
}

// A successful refresh whose directory has dropped a kid evicts that kid
// promptly (revocation), while keys still present remain.
TEST_F(KeyDirectoryWarmerTest, RefreshPrunesVanishedKid) {
  FakeFetcher fetcher(TwoKeyDoc());
  KeyDirectoryWarmer warmer(BaseConfig(), &fetcher, &cache_,
                            thread_system_.get(), &timer_, &handler_);
  ASSERT_EQ(2, warmer.RefreshOnce());
  ASSERT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k1").status);
  ASSERT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k2").status);

  // Directory now lists only k1; the next successful refresh evicts k2.
  fetcher.set_body(StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\",\"x\":\"",
      kX, "\"}]}"));
  EXPECT_EQ(1, warmer.RefreshOnce());
  EXPECT_EQ(KeyLookupResult::kFound, ReadCacheOnly("k1").status);
  EXPECT_EQ(KeyLookupResult::kNotFound, ReadCacheOnly("k2").status);
}

TEST_F(KeyDirectoryWarmerTest, EmptyAllowlistDoesNotFetch) {
  FakeFetcher fetcher(TwoKeyDoc());
  KeyDirectoryWarmer::Config cfg = BaseConfig();
  cfg.allowlist.clear();  // disabled => fail closed, no network
  KeyDirectoryWarmer warmer(cfg, &fetcher, &cache_, thread_system_.get(),
                            &timer_, &handler_);
  EXPECT_EQ(0, warmer.RefreshOnce());
  EXPECT_EQ(0, fetcher.calls());
}

TEST_F(KeyDirectoryWarmerTest, OffAllowlistUrlDoesNotFetch) {
  FakeFetcher fetcher(TwoKeyDoc());
  KeyDirectoryWarmer::Config cfg = BaseConfig();
  cfg.url = "https://evil.example.com/jwks";  // not in the allowlist
  KeyDirectoryWarmer warmer(cfg, &fetcher, &cache_, thread_system_.get(),
                            &timer_, &handler_);
  EXPECT_EQ(0, warmer.RefreshOnce());
  EXPECT_EQ(0, fetcher.calls());
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb
