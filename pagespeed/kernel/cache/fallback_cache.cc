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

#include "pagespeed/kernel/cache/fallback_cache.h"

#include "base/logging.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"

namespace {

// We decide in Put whether the value goes to large_object_cache_
// or small_object_cache_.  When a value goes to small_object_cache_,
// we suffix it with an 'S'.  When it goes to the large_object_cache_, we
// put into small_object_cache_ a single 'L', which will tell us
// to look in large_object_cache_.  Values stored in large_object_cache_
// do not have a suffix.
//
// We use suffixes so that we can strip the value from the
// SharedString via RemoveSuffix, which does not require mutating the
// base string data, which might, at least in the case of LRUCache, be
// accessed concurrently by multiple threads.
char kInSmallObjectCache = 'S';
char kInLargeObjectCache = 'L';

}  // namespace

namespace net_instaweb {

namespace {

// Forwards Get requests to the large_cache when the response matches
// kInLargeObjectCache.  For kInSmallObjectCache, unwraps the payload
// and delivers it to the callback.
class FallbackCallback : public CacheInterface::Callback {
 public:
  FallbackCallback(CacheInterface::Callback* callback,
                   CacheInterface* large_object_cache)
      : callback_(callback),
        large_object_cache_(large_object_cache),
        validate_candidate_called_(false) {}

  ~FallbackCallback() override {}

  void Done(CacheInterface::KeyState state) override {
    DCHECK(validate_candidate_called_);
    if (callback_ != nullptr) {
      callback_->DelegatedDone(state);
    }
    delete this;
  }

  // This validation is called by the small-object cache.  We need to decode
  // the value and decide whether to unwrap the small value, or forward the
  // request to the large_object_cache_.
  bool ValidateCandidate(const GoogleString& key,
                         CacheInterface::KeyState state) override {
    validate_candidate_called_ = true;
    size_t size = value().size();
    const char* val = value().data();
    if ((size == 1) && (val[0] == kInLargeObjectCache)) {
      // Delegate the fetch to large_object_cache_, passing the
      // original callback directly to the large_object_cache_.
      // We erase callback_ so we don't forward the Done report
      // from the small cache.
      Callback* callback = callback_;
      callback_ = nullptr;
      large_object_cache_->Get(key, callback);
      return true;  // The forwarding-marker in the small object cache is OK.
    } else if ((size >= 1) && (val[size - 1] == kInSmallObjectCache)) {
      // Link values together, but strip the marker from the new view.
      // An owned copy is required because MappedSharedString doesn't support
      // RemoveSuffix - we need to work with the owned SharedString.
      const MappedSharedString& mapped_value = value();
      SharedString new_value;
      if (mapped_value.is_mapped()) {
        // De-alias the borrowed (e.g. Cyclone zero-copy) bytes with the
        // verified copy (copy-then-verify); a torn borrow is treated
        // as a miss via the same not-found path used for a bad encoding below.
        GoogleString devalias;
        if (!CopyMappedVerified(mapped_value.Value(), mapped_value,
                                &devalias)) {
          callback_->DelegatedValidateCandidate(key, CacheInterface::kNotFound);
          return false;
        }
        new_value.SwapWithString(&devalias);
      } else {
        new_value = mapped_value.ToOwned();
      }
      new_value.RemoveSuffix(1);
      callback_->set_value(new_value);
      return callback_->DelegatedValidateCandidate(key, state);
    }
    // The value in the cache was missing or encoded incorrectly.
    callback_->DelegatedValidateCandidate(key, CacheInterface::kNotFound);
    return false;
  }

 private:
  CacheInterface::Callback* callback_;
  CacheInterface* large_object_cache_;
  bool validate_candidate_called_;

  FallbackCallback(const FallbackCallback&) = delete;
  FallbackCallback& operator=(const FallbackCallback&) = delete;
};

}  // namespace

FallbackCache::FallbackCache(CacheInterface* small_object_cache,
                             CacheInterface* large_object_cache,
                             int threshold_bytes, MessageHandler* handler)
    : small_object_cache_(small_object_cache),
      large_object_cache_(large_object_cache),
      threshold_bytes_(threshold_bytes),
      account_for_key_size_(true),
      message_handler_(handler) {}

FallbackCache::~FallbackCache() {}

GoogleString FallbackCache::FormatName(StringPiece small, StringPiece large) {
  return StrCat("Fallback(small=", small, ",large=", large, ")");
}

void FallbackCache::Get(const GoogleString& key, Callback* callback) {
  callback = new FallbackCallback(callback, large_object_cache_);
  small_object_cache_->Get(key, callback);
}

void FallbackCache::MultiGet(MultiGetRequest* request) {
  for (int i = 0, n = request->size(); i < n; ++i) {
    KeyCallback& key_callback = (*request)[i];
    key_callback.callback =
        new FallbackCallback(key_callback.callback, large_object_cache_);
  }
  small_object_cache_->MultiGet(request);
}

void FallbackCache::Put(const GoogleString& key, const SharedString& value) {
  int store_size = value.size();
  if (account_for_key_size_) {
    store_size += static_cast<int>(key.size());
  }
  store_size += 1;  // For kInSmallObjectCache marker.

  if (store_size > threshold_bytes_) {
    // Write the actual value to large_object_cache_ first, then the
    // forwarding marker to small_object_cache_. This ordering ensures
    // that if the large cache write fails silently, readers won't find
    // a forwarding marker pointing to non-existent data.
    large_object_cache_->Put(key, value);
    SharedString forwarding_value;
    forwarding_value.Assign(&kInLargeObjectCache, 1);
    small_object_cache_->Put(key, forwarding_value);
  } else {
    SharedString wrapped_value(value);
    // 'value' may share storage with a response that is being served (e.g. a
    // write-through fill of a cache-hit value); appending in place could
    // reallocate the shared bytes under a concurrent reader, so detach first.
    wrapped_value.DetachRetainingContent();
    wrapped_value.Append(&kInSmallObjectCache, 1);
    small_object_cache_->Put(key, wrapped_value);
  }
}

void FallbackCache::Delete(const GoogleString& key) {
  small_object_cache_->Delete(key);
  large_object_cache_->Delete(key);
}

}  // namespace net_instaweb
