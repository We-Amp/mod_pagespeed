// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/key_directory_warmer.h"

#include <algorithm>
#include <cstdint>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/sync_fetcher_adapter_callback.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "pagespeed/kernel/webbotauth/net_fetch_key_directory.h"
#include "pagespeed/opt/http/request_context.h"

namespace net_instaweb {
namespace webbotauth {

KeyDirectoryWarmer::KeyDirectoryWarmer(const Config& config,
                                       UrlAsyncFetcher* fetcher,
                                       CacheInterface* cache,
                                       ThreadSystem* thread_system,
                                       Timer* timer, MessageHandler* handler)
    : Thread(thread_system, "webbotauth_key_warmer", ThreadSystem::kJoinable),
      config_(config),
      fetcher_(fetcher),
      cache_(cache),
      thread_system_(thread_system),
      timer_(timer),
      handler_(handler),
      mutex_(thread_system->NewMutex()),
      condvar_(mutex_->NewCondvar()),
      quit_(false) {}

KeyDirectoryWarmer::~KeyDirectoryWarmer() { Stop(); }

void KeyDirectoryWarmer::Stop() {
  bool was_running;
  {
    ScopedMutex lock(mutex_.get());
    was_running = !quit_;
    quit_ = true;
    condvar_->Signal();
  }
  // Join only on the transition running->quit, and only if the thread started.
  if (was_running && Started()) {
    Join();
  }
}

int64_t KeyDirectoryWarmer::InitialDelayMs() const {
  // A short, jittered initial delay: let the worker finish init and spread the
  // first fetch across workers so they don't stampede the directory at once.
  return (1 + config_.jitter_sec) * Timer::kSecondMs;
}

int64_t KeyDirectoryWarmer::RefreshIntervalMs() const {
  int64_t sec = config_.refresh_sec + config_.jitter_sec;
  if (sec < 1) sec = 1;
  return sec * Timer::kSecondMs;
}

void KeyDirectoryWarmer::Run() {
  // Warm shortly after startup, then once per (jittered) refresh interval. Every
  // wait is interruptible by Stop().
  if (WaitMsOrQuit(InitialDelayMs())) {
    return;
  }
  while (true) {
    RefreshOnce();
    if (WaitMsOrQuit(RefreshIntervalMs())) {
      return;
    }
  }
}

bool KeyDirectoryWarmer::WaitMsOrQuit(int64_t wait_ms) {
  ScopedMutex lock(mutex_.get());
  if (quit_) {
    return true;
  }
  const int64_t deadline_ms = timer_->NowMs() + wait_ms;
  while (!quit_) {
    const int64_t remaining_ms = deadline_ms - timer_->NowMs();
    if (remaining_ms <= 0) {
      break;
    }
    condvar_->TimedWait(remaining_ms);
  }
  return quit_;
}

int KeyDirectoryWarmer::RefreshOnce() {
  GoogleString body;
  if (!FetchJwksBody(&body)) {
    handler_->Message(
        kWarning,
        "webbotauth A2: key-directory warm-fetch failed for host=%s url=%s "
        "(cached keys retained until their TTL lapses)",
        config_.host.c_str(), config_.url.c_str());
    return 0;  // D4 strict-expiry: write nothing; existing entries lapse on TTL.
  }
  const int64_t now_sec = timer_->NowMs() / Timer::kSecondMs;
  std::set<GoogleString> current_kids;
  const int written =
      WarmKeyDirectoryCache(cache_, config_.realm, config_.host, body, now_sec,
                            config_.positive_ttl_sec, &current_kids);

  // Prune kids that vanished since the last successful refresh so a key removed
  // upstream stops verifying promptly instead of lingering until its TTL lapses.
  // Only when this refresh produced a non-empty directory: a transient empty or
  // garbage 200 then cannot wipe all keys (those still lapse on their own TTL,
  // preserving D4). Runs only on a successful fetch, so a failed refresh touches
  // nothing.
  if (written > 0) {
    for (std::set<GoogleString>::const_iterator it = last_kids_.begin();
         it != last_kids_.end(); ++it) {
      if (current_kids.find(*it) == current_kids.end()) {
        cache_->Delete(CachedKeyDirectoryProvider::CacheKey(config_.realm,
                                                            config_.host, *it));
      }
    }
    last_kids_.swap(current_kids);
  }

  handler_->Message(kInfo,
                    "webbotauth A2: warmed %d key(s) for realm=%s host=%s",
                    written, config_.realm.c_str(), config_.host.c_str());
  return written;
}

bool KeyDirectoryWarmer::FetchJwksBody(GoogleString* body) {
  if (fetcher_ == nullptr) {
    return false;
  }
  // SSRF guard: an empty allowlist disables the feature (fail-closed); otherwise
  // https-only + the operator origin allowlist (reused from NetFetchKeyDirectory).
  if (config_.allowlist.empty()) {
    return false;
  }
  if (!IsAllowlistedHttpsUrl(config_.url, config_.allowlist)) {
    return false;
  }

  HttpOptions http_options;
  http_options.respect_vary = false;
  RequestContextPtr request_ctx(
      new RequestContext(http_options, thread_system_->NewMutex(), timer_));

  GoogleString out;
  StringWriter writer(&out);
  // Released after the blocking wait; the callback owns the writer until then so
  // a late Done() (after timeout) cannot write to freed memory.
  SyncFetcherAdapterCallback* cb =
      new SyncFetcherAdapterCallback(thread_system_, &writer, request_ctx);
  fetcher_->Fetch(config_.url, handler_, cb);

  bool done = false;
  int status_code = 0;
  if (cb->LockIfNotReleased()) {
    int64_t now_ms = timer_->NowMs();
    for (int64_t end_ms = now_ms + config_.fetch_timeout_ms;
         !cb->IsDoneLockHeld() && now_ms < end_ms; now_ms = timer_->NowMs()) {
      cb->TimedWait(std::max(static_cast<int64_t>(0), end_ms - now_ms));
    }
    done = cb->IsDoneLockHeld();
    if (done) {
      status_code = cb->response_headers()->status_code();
    }
    cb->Unlock();
  }

  bool ok = false;
  if (done && cb->success() && status_code == HttpStatus::kOK &&
      out.size() <= config_.max_response_bytes) {
    *body = out;
    ok = true;
  }
  cb->Release();
  return ok;
}

}  // namespace webbotauth
}  // namespace net_instaweb
