/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Test-only CacheInterface wrapper that re-delivers every cache hit as a
// memory-mapped MappedSharedString view, emulating CycloneCache's zero-copy
// borrow path (mmap-backed read handles held under a renewable lease) on top
// of any delegate cache.  Used to exercise HTTPCache/HTTPValue zero-copy
// serving (CycloneZeroCopy / HTTPValue::LinkMapped) without a real Cyclone
// cache.
//
// CRITICAL FOR THE LIFETIME TESTS: the bytes handed to the callback live in
// their own page-aligned mmap region, and the release callback (invoked when
// the last MappedSharedString reference drops) calls mprotect(PROT_NONE) on
// that region.  Any read of the borrowed bytes AFTER release therefore
// faults (SIGSEGV).  This is what makes the keep-alive/collapse tests bite:
// if HTTPValue dropped a borrow too early, the subsequent ExtractContents
// read would crash rather than silently pass.  (A plain owned buffer that
// lived for the whole test would make every lifetime scenario pass
// vacuously.)

#ifndef TEST_NET_INSTAWEB_HTTP_MAPPED_BACKEND_CACHE_H_
#define TEST_NET_INSTAWEB_HTTP_MAPPED_BACKEND_CACHE_H_

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <list>
#include <memory>

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/cache_interface.h"

namespace net_instaweb {

class MappedBackendCache : public CacheInterface {
 public:
  explicit MappedBackendCache(CacheInterface* delegate)
      : delegate_(delegate), shared_(std::make_shared<Shared>()) {}
  ~MappedBackendCache() override {}

  void Get(const GoogleString& key, Callback* callback) override {
    delegate_->Get(key, new MappingCallback(callback, shared_));
  }
  void Put(const GoogleString& key, const SharedString& value) override {
    delegate_->Put(key, value);
  }
  void Delete(const GoogleString& key) override { delegate_->Delete(key); }
  GoogleString Name() const override {
    return StrCat("Mapped(", delegate_->Name(), ")");
  }
  bool IsBlocking() const override { return delegate_->IsBlocking(); }
  bool IsHealthy() const override { return delegate_->IsHealthy(); }
  void ShutDown() override { delegate_->ShutDown(); }

  // Number of mapped views handed out on hits.
  int mapped_hits() const { return shared_->mapped_hits; }
  // Scripts the intent-checked lease-renewal verdict every outstanding
  // (and future) view's RenewLeaseStrict() reports.  Defaults to kOk (a
  // live, unchallenged lease); set kTorn to emulate a wrap committing over
  // the borrow between the cache read and a consumer's verify.
  void set_strict_verdict(LeaseRenewal verdict) {
    shared_->strict_verdict = static_cast<int>(verdict);
  }
  // Number of mapped views whose last reference has been released (and whose
  // backing page has been mprotect(PROT_NONE)'d).
  int release_count() const { return shared_->release_count; }
  // True if p points into a region this wrapper handed out as a mapped view.
  // (Pointer comparison only -- never dereferences, so this is safe even
  // after the region has been PROT_NONE'd.)
  bool ContainsPointer(const char* p) const {
    for (const Region* r : shared_->regions) {
      if (p >= r->data && p < r->data + r->data_len) {
        return true;
      }
    }
    return false;
  }

 private:
  struct Region {
    char* base;        // Page-aligned mmap base.
    size_t map_len;    // Total mmapped length (page multiple).
    const char* data;  // == base; the bytes handed to the callback.
    size_t data_len;   // Logical length of the borrowed value.
    bool released;
  };

  // Shared with each outstanding view token so regions and counters stay
  // valid even if a borrowed view outlives the wrapper itself.  Owns the
  // mmap regions and unmaps them at end of test.
  struct Shared {
    ~Shared() {
      for (Region* r : regions) {
        // Restore RW so munmap on a PROT_NONE region is unambiguous, then
        // release the mapping.
        mprotect(r->base, r->map_len, PROT_READ | PROT_WRITE);
        munmap(r->base, r->map_len);
        delete r;
      }
    }
    std::list<Region*> regions;
    int mapped_hits = 0;
    int release_count = 0;
    int strict_verdict = 0;  // LeaseRenewal::kOk.
  };

  struct ViewToken {
    ViewToken(std::shared_ptr<Shared> s, Region* r)
        : shared(std::move(s)), region(r) {}
    std::shared_ptr<Shared> shared;
    Region* region;
  };

  // Lease hooks for the views: renew always succeeds, the strict
  // verdict is scripted (see set_strict_verdict), and no forced wrap is
  // ever imminent.
  static int RenewView(void* /*user_data*/) { return 1; }
  static int RenewStrictView(void* user_data) {
    return static_cast<ViewToken*>(user_data)->shared->strict_verdict;
  }
  static uint64_t NsUntilForcedWrapView(void* /*user_data*/) {
    return ~static_cast<uint64_t>(0);
  }

  static void ReleaseView(void* user_data) {
    ViewToken* token = static_cast<ViewToken*>(user_data);
    Region* r = token->region;
    // Poison the region so any read of the borrowed bytes past this point
    // faults.  A too-early keep-alive drop is now a hard fault, not a silent
    // pass.
    mprotect(r->base, r->map_len, PROT_NONE);
    r->released = true;
    ++token->shared->release_count;
    delete token;
  }

  // Copies 'bytes' into a fresh page-aligned mmap region and records it.
  static Region* MapBytes(Shared* shared, StringPiece bytes) {
    static const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    size_t map_len = (bytes.size() / page + 1) * page;
    void* base = mmap(nullptr, map_len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(base != MAP_FAILED) << "MappedBackendCache mmap failed";
    memcpy(base, bytes.data(), bytes.size());
    Region* r = new Region{static_cast<char*>(base), map_len,
                           static_cast<char*>(base), bytes.size(), false};
    shared->regions.push_back(r);
    return r;
  }

  class MappingCallback : public CacheInterface::Callback {
   public:
    MappingCallback(CacheInterface::Callback* callback,
                    std::shared_ptr<Shared> shared)
        : callback_(callback), shared_(std::move(shared)) {}

    bool ValidateCandidate(const GoogleString& key,
                           CacheInterface::KeyState state) override {
      if (state == CacheInterface::kAvailable) {
        // Copy the delegate's bytes into a poisonable mmap region and hand
        // the wrapped callback a genuinely mapped view over it, exactly like
        // CycloneCache::GetWithTier's zero-copy branch.
        Region* r = MapBytes(shared_.get(), value().Value());
        ++shared_->mapped_hits;
        ViewToken* token = new ViewToken(shared_, r);
        callback_->set_value(MappedSharedString::FromMappedView(
            r->data, r->data_len, &MappedBackendCache::ReleaseView,
            &MappedBackendCache::RenewView,
            &MappedBackendCache::RenewStrictView,
            &MappedBackendCache::NsUntilForcedWrapView, token));
      }
      return callback_->DelegatedValidateCandidate(key, state);
    }

    void Done(CacheInterface::KeyState state) override {
      callback_->DelegatedDone(state);
      delete this;
    }

   private:
    CacheInterface::Callback* callback_;
    std::shared_ptr<Shared> shared_;

    MappingCallback(const MappingCallback&) = delete;
    MappingCallback& operator=(const MappingCallback&) = delete;
  };

  CacheInterface* delegate_;  // Not owned.
  std::shared_ptr<Shared> shared_;

  MappedBackendCache(const MappedBackendCache&) = delete;
  MappedBackendCache& operator=(const MappedBackendCache&) = delete;
};

}  // namespace net_instaweb

#endif  // TEST_NET_INSTAWEB_HTTP_MAPPED_BACKEND_CACHE_H_
