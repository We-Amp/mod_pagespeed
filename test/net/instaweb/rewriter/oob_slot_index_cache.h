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

// Test-only CacheInterface wrapper that rewrites every hit carrying a
// parseable OutputPartitions so that its first partition input names a slot
// index far outside any rewrite's slot table.  Used to check that a
// corrupted metadata entry degrades to a miss instead of being indexed into
// the slot table.

#ifndef TEST_NET_INSTAWEB_REWRITER_OOB_SLOT_INDEX_CACHE_H_
#define TEST_NET_INSTAWEB_REWRITER_OOB_SLOT_INDEX_CACHE_H_

#include "net/instaweb/rewriter/cached_result.pb.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/cache_interface.h"

namespace net_instaweb {

class OobSlotIndexCache : public CacheInterface {
 public:
  explicit OobSlotIndexCache(CacheInterface* delegate) : delegate_(delegate) {}

  void Get(const GoogleString& key, Callback* callback) override {
    delegate_->Get(key, new CorruptingCallback(callback, &num_corrupted_));
  }
  void Put(const GoogleString& key, const SharedString& value) override {
    delegate_->Put(key, value);
  }
  void Delete(const GoogleString& key) override { delegate_->Delete(key); }
  GoogleString Name() const override {
    return StrCat("OobSlotIndex(", delegate_->Name(), ")");
  }
  bool IsBlocking() const override { return delegate_->IsBlocking(); }
  bool IsHealthy() const override { return delegate_->IsHealthy(); }
  void ShutDown() override { delegate_->ShutDown(); }

  int num_corrupted() const { return num_corrupted_; }

 private:
  class CorruptingCallback : public CacheInterface::Callback {
   public:
    CorruptingCallback(CacheInterface::Callback* callback, int* num_corrupted)
        : callback_(callback), num_corrupted_(num_corrupted) {}

    bool ValidateCandidate(const GoogleString& key,
                           CacheInterface::KeyState state) override {
      if (state == CacheInterface::kAvailable) {
        OutputPartitions partitions;
        if (partitions.ParseFromString(value().Value().as_string()) &&
            partitions.partition_size() > 0 &&
            partitions.partition(0).input_size() > 0) {
          partitions.mutable_partition(0)
              ->mutable_input(0)
              ->set_index(kOobSlotIndex);
          GoogleString corrupted;
          // Serializing the mutated in-memory message cannot fail here;
          // the bytes are corruption test input either way.
          (void)partitions.SerializeToString(&corrupted);
          callback_->set_value(MappedSharedString(SharedString(corrupted)));
          ++*num_corrupted_;
        }
      }
      return callback_->DelegatedValidateCandidate(key, state);
    }

    void Done(CacheInterface::KeyState state) override {
      callback_->DelegatedDone(state);
      delete this;
    }

   private:
    // Well past the end of any real slot table.
    static constexpr int kOobSlotIndex = 1000;

    CacheInterface::Callback* callback_;
    int* num_corrupted_;

    CorruptingCallback(const CorruptingCallback&) = delete;
    CorruptingCallback& operator=(const CorruptingCallback&) = delete;
  };

  CacheInterface* delegate_;  // Not owned.
  int num_corrupted_ = 0;

  OobSlotIndexCache(const OobSlotIndexCache&) = delete;
  OobSlotIndexCache& operator=(const OobSlotIndexCache&) = delete;
};

}  // namespace net_instaweb

#endif  // TEST_NET_INSTAWEB_REWRITER_OOB_SLOT_INDEX_CACHE_H_
