// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// KeyDirectoryWarmer: the off-request background refresher for a remote
// Web-Bot-Auth / RSL-CAP key directory. It exists so the
// request path can stay strictly synchronous and non-blocking: all network I/O
// happens HERE, off the nginx event loop, and only its result (keys in a shared
// blocking cache) is read synchronously at request time.
//
// Mechanism: a DEDICATED per-worker thread (NOT a Scheduler alarm -- alarms must
// be lightweight, and run with the scheduler mutex held; a multi-second blocking
// fetch there would stall all scheduler work). Each cycle the thread does one
// bounded, SSRF-guarded blocking HTTPS fetch of the directory JWKS (mirroring
// ServerContext::FetchRemoteConfig's SyncFetcherAdapterCallback + TimedWait
// idiom) and writes every kid-bearing key into the cache as a positive entry via
// WarmKeyDirectoryCache(). Between cycles it waits one (jittered) refresh
// interval on a condvar, interruptibly. D4 strict-expiry: a failed refresh
// writes nothing; stale entries lapse on their own TTL (no serve-stale).
//
// Fetcher constraint: a blocking fetch off the event loop only completes with a
// fetcher that owns its own I/O thread -- the curl fetcher, which is the nginx
// default. The native NgxUrlAsyncFetcher can only be driven by the main event
// loop, so the caller MUST NOT construct a warmer when use_native_fetcher() is
// true (the request path then simply falls back to the local-file provider).
//
// Lifetime: construct, then Start(). On shutdown call Stop() (signals quit +
// Join) BEFORE the fetcher/cache it borrows are torn down. Owns none of its
// pointer arguments.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_KEY_DIRECTORY_WARMER_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_KEY_DIRECTORY_WARMER_H_

#include <cstdint>
#include <memory>
#include <set>

#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/cache_interface.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_annotations.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

class Timer;

namespace webbotauth {

class KeyDirectoryWarmer : public ThreadSystem::Thread {
 public:
  struct Config {
    GoogleString realm;  // cache namespace ("wba" / "rsl")
    GoogleString host;   // cache-key host identity (directory_host)
    GoogleString url;    // JWKS fetch URL
    std::set<GoogleString>
        allowlist;                    // SSRF https-origin allowlist (empty=off)
    int64_t refresh_sec = 3600;       // base refresh interval
    int64_t jitter_sec = 0;           // per-worker spread added to each wait
    int64_t fetch_timeout_ms = 2000;  // bounded blocking-fetch wait
    size_t max_response_bytes = 256 * 1024;
    int64_t positive_ttl_sec = 3600;  // cache entry lifetime (clamped in core)
  };

  // Does NOT take ownership of any pointer argument. `cache` must be a blocking
  // CacheInterface (Cyclone). `fetcher` must own its own I/O thread (curl).
  KeyDirectoryWarmer(const Config& config, UrlAsyncFetcher* fetcher,
                     CacheInterface* cache, ThreadSystem* thread_system,
                     Timer* timer, MessageHandler* handler);
  ~KeyDirectoryWarmer() override;

  // Signal the run loop to quit and Join the thread. Idempotent; safe even if
  // Start() was never called.
  void Stop();

  // One synchronous refresh cycle: blocking-fetch the JWKS (SSRF-guarded, size-
  // capped) and populate the cache. Returns the number of keys written (0 on any
  // fetch/parse failure). Used by the run loop AND directly by unit tests (which
  // inject a fake synchronous fetcher).
  int RefreshOnce();

 protected:
  void Run() override;

 private:
  // Blocking-fetch the configured URL into *body (mirrors FetchRemoteConfig).
  // Applies the allowlist + https guard before, and the size cap after. Returns
  // true only on a 200 whose body is within the cap.
  bool FetchJwksBody(GoogleString* body);

  // Wait up to `wait_ms`, or until Stop() is called, whichever comes first.
  // Returns true if the warmer should quit.
  bool WaitMsOrQuit(int64_t wait_ms);

  int64_t InitialDelayMs() const;
  int64_t RefreshIntervalMs() const;

  Config config_;
  UrlAsyncFetcher* fetcher_;     // not owned
  CacheInterface* cache_;        // not owned (blocking)
  ThreadSystem* thread_system_;  // not owned
  Timer* timer_;                 // not owned
  MessageHandler* handler_;      // not owned

  // Kids written in the previous successful refresh, so a refresh that observes a
  // key's removal can promptly evict it (rather than waiting out its TTL). Only
  // touched on the warmer thread.
  std::set<GoogleString> last_kids_;

  std::unique_ptr<ThreadSystem::CondvarCapableMutex> mutex_;
  std::unique_ptr<ThreadSystem::Condvar> condvar_;
  bool quit_ GUARDED_BY(mutex_);

  KeyDirectoryWarmer(const KeyDirectoryWarmer&) = delete;
  KeyDirectoryWarmer& operator=(const KeyDirectoryWarmer&) = delete;
};

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_KEY_DIRECTORY_WARMER_H_
